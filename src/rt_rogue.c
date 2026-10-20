// Roguelike runs and asynchronous challenges, both started from the title screen.
//
// * Nothing here touches the story save: runs live in RAM (a new game is initialised, the run is
//   played and the console is soft-reset). Only a tiny block of "meta progress" is kept, in the flash
//   sector of the (unused) recorded battle.
// * A challenge code is the packed team of a player (species, level, nature, item, moves and build);
//   a friend types it and fights it with his own team.
#include "global.h"
#include "rt_internal.h"
#include "bg.h"
#include "battle.h"
#include "battle_setup.h"
#include "event_data.h"
#include "field_screen_effect.h"
#include "gpu_regs.h"
#include "item.h"
#include "main.h"
#include "main_menu.h"
#include "menu.h"
#include "new_game.h"
#include "overworld.h"
#include "palette.h"
#include "pokemon.h"
#include "random.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "task.h"
#include "text.h"
#include "window.h"
#include "agb_flash.h"
#include "malloc.h"
#include "gba/flash_internal.h"
#include "save.h"
#include "constants/items.h"
#include "constants/maps.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/species.h"
#include "constants/pokedex.h"
#include "constants/trainers.h"
#include "constants/opponents.h"
#include "data/rt/rogue_maps.h"

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
EWRAM_DATA u8 gRogueMode = 0;            // RG_*
EWRAM_DATA u8 gRogueBattleEntered = 0;   // set by the battle engine when a battle starts
EWRAM_DATA u8 gRogueBoss = 0;
EWRAM_DATA u8 gRogueChalBuild[PARTY_SIZE] = {0};
enum { RG_OFF, RG_RUN, RG_CHALLENGE };

#define RUN_MAX_FLOOR 25
#define META_SECTOR   31
#define META_MAGIC    0x52744D31

struct RogueMeta
{
    u32 magic;
    u16 essence;
    u16 best;
    u16 runs;
    u16 victories;
    u16 chalWins;
    u8 up[8];
    u16 crc;
};

enum { UP_VIT, UP_MIGHT, UP_RESOLVE, UP_KI, UP_REWARD, UP_WIND, UP_BEAST, UP_COUNT };

struct RogueRun
{
    u8 state;
    u8 floor;
    u8 buff[6];       // run-long stacks: atk, def, spe, hp, ki, cooldown
    u8 wind;          // second wind charges
    u8 starter;
    u8 offers[5];
    u8 nOffers;
    u8 bosses;
    u8 timer;
    u8 result;
    u16 starterSpecies[6];
    u8 nStarters;
};

static EWRAM_DATA struct RogueMeta sMeta = {0};
EWRAM_DATA struct RogueRun gRogueRun = {0};
#define sRun gRogueRun
static EWRAM_DATA u8 sSavedParty[sizeof(struct Pokemon) * PARTY_SIZE] = {0};
static EWRAM_DATA u8 sSavedName[PLAYER_NAME_LENGTH + 1] = {0};
static EWRAM_DATA u8 sSavedBuilds[sizeof(struct RtSave)] = {0};   // the player's builds survive the new game of a challenge
static EWRAM_DATA u8 sCodeBuf[96] = {0};      // challenge code text (EOS terminated, 5 bits per character)
static EWRAM_DATA u8 sChalMons = 0;
static EWRAM_DATA u16 sChalSpecies[PARTY_SIZE] = {0};

enum { RS_INIT, RS_WARP, RS_WAIT, RS_FIGHT, RS_DONE };

// ---------------------------------------------------------------------------
// meta progress (flash)
// ---------------------------------------------------------------------------
static u16 MetaCrc(const struct RogueMeta *m)
{
    const u8 *p = (const u8 *)m;
    u32 h = 0x1234, i;
    for (i = 0; i < offsetof(struct RogueMeta, crc); i++)
        h = h * 31 + p[i];
    return h ^ (h >> 16);
}

static void Meta_Load(void)
{
    u8 *buf = Alloc(SECTOR_SIZE);
    memset(&sMeta, 0, sizeof(sMeta));
    if (buf != NULL)
    {
        ReadFlash(META_SECTOR, 0, buf, sizeof(struct RogueMeta));
        memcpy(&sMeta, buf, sizeof(sMeta));
        Free(buf);
    }
    if (sMeta.magic != META_MAGIC || sMeta.crc != MetaCrc(&sMeta))
    {
        memset(&sMeta, 0, sizeof(sMeta));
        sMeta.magic = META_MAGIC;
    }
}

static void Meta_Save(void)
{
    u8 *buf = Alloc(SECTOR_SIZE);
    if (buf == NULL)
        return;
    sMeta.magic = META_MAGIC;
    sMeta.crc = MetaCrc(&sMeta);
    memset(buf, 0xFF, SECTOR_SIZE);
    memcpy(buf, &sMeta, sizeof(sMeta));
    ProgramFlashSectorAndVerify(META_SECTOR, buf);
    Free(buf);
}

static const u8 sUpMax[UP_COUNT] = {5, 5, 5, 5, 1, 2, 4};
static const u16 sUpCost[UP_COUNT] = {50, 50, 50, 40, 400, 250, 150};

// ---------------------------------------------------------------------------
// effects of the run on the battle engine
// ---------------------------------------------------------------------------
u16 RtRogue_StatMul(struct RtActor *a, u8 stat)
{
    u16 m = 100;
    if (gRogueMode != RG_RUN || a->side != RT_SIDE_PLAYER)
        return 100;
    if (stat == STG_ATK || stat == STG_SPA)
        m += sRun.buff[0] * 6 + sMeta.up[UP_MIGHT] * 3;
    else if (stat == STG_DEF || stat == STG_SPD)
        m += sRun.buff[1] * 6 + sMeta.up[UP_RESOLVE] * 3;
    else if (stat == STG_SPE)
        m += sRun.buff[2] * 5;
    return m;
}

u16 RtRogue_HpPct(void)
{
    return gRogueMode == RG_RUN ? 100 + sRun.buff[3] * 8 + sMeta.up[UP_VIT] * 4 : 100;
}

u16 RtRogue_KiStart(void)
{
    return gRogueMode == RG_RUN ? sMeta.up[UP_KI] * 100 : 0;
}

u16 RtRogue_KiPct(void)
{
    return gRogueMode == RG_RUN ? 100 + sRun.buff[4] * 20 : 100;
}

u16 RtRogue_CdPct(void)
{
    u16 p;
    if (gRogueMode != RG_RUN)
        return 100;
    p = 100 - sRun.buff[5] * 6;
    return p < 55 ? 55 : p;
}

// ---------------------------------------------------------------------------
// tiny text screen toolkit
// ---------------------------------------------------------------------------
static const struct BgTemplate sRgBg[] =
{
    { .bg = 0, .charBaseIndex = 0, .mapBaseIndex = 31, .screenSize = 0, .paletteMode = 0, .priority = 0, .baseTile = 0 },
};

static const struct WindowTemplate sRgWin[] =
{
    { .bg = 0, .tilemapLeft = 0, .tilemapTop = 0, .width = 30, .height = 20, .paletteNum = 15, .baseBlock = 1 },
    DUMMY_WIN_TEMPLATE
};

static const u16 sRgPal[16] =
{
    RGB(2, 4, 12), RGB(31, 31, 31), RGB(12, 12, 18), RGB(31, 27, 6), RGB(10, 26, 12), RGB(31, 10, 10),
    RGB(14, 22, 31), RGB(20, 20, 24), RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0),
    RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0)
};

static const u8 sColWhite[] = {0, 1, 2};
static const u8 sColGold[] = {0, 3, 2};
static const u8 sColGreen[] = {0, 4, 2};
static const u8 sColRed[] = {0, 5, 2};
static const u8 sColBlue[] = {0, 6, 2};
static const u8 sColGray[] = {0, 7, 2};

static EWRAM_DATA u8 sScreen = 0;
static EWRAM_DATA u8 sCursor = 0;
static EWRAM_DATA u8 sCount = 0;
static EWRAM_DATA u8 sSub = 0;

static void VBlankCB_Rg(void)
{
    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
}

static void Rg_Open(void)
{
    SetVBlankCallback(NULL);
    SetHBlankCallback(NULL);
    SetGpuReg(REG_OFFSET_DISPCNT, 0);
    SetGpuReg(REG_OFFSET_WININ, 0);
    SetGpuReg(REG_OFFSET_WINOUT, 0);
    SetGpuReg(REG_OFFSET_WIN0H, 0);
    SetGpuReg(REG_OFFSET_WIN0V, 0);
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    SetGpuReg(REG_OFFSET_BLDALPHA, 0);
    SetGpuReg(REG_OFFSET_BLDY, 0);
    DmaFill16(3, 0, (void *)VRAM, VRAM_SIZE);
    DmaFill32(3, 0, (void *)OAM, OAM_SIZE);
    DmaFill16(3, 0, (void *)PLTT, PLTT_SIZE);
    ResetPaletteFade();
    ResetSpriteData();
    ResetTasks();
    FreeAllSpritePalettes();
    ResetBgsAndClearDma3BusyFlags(0);
    InitBgsFromTemplates(0, sRgBg, ARRAY_COUNT(sRgBg));
    InitWindows(sRgWin);
    DeactivateAllTextPrinters();
    LoadPalette(sRgPal, BG_PLTT_ID(15), PLTT_SIZE_4BPP);
    LoadPalette(&sRgPal[0], BG_PLTT_ID(0), PLTT_SIZEOF(1));
    SetVBlankCallback(VBlankCB_Rg);
    ShowBg(0);
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP | DISPCNT_BG0_ON);
}

static void Rg_Clear(void)
{
    FillWindowPixelBuffer(0, PIXEL_FILL(0));
}

static void Rg_Text(u8 x, u8 y, const u8 *col, const u8 *str)
{
    AddTextPrinterParameterized3(0, FONT_NORMAL, x, y, col, TEXT_SKIP_DRAW, str);
}

static void Rg_TextS(u8 x, u8 y, const u8 *col, const u8 *str)
{
    AddTextPrinterParameterized3(0, FONT_SMALL, x, y, col, TEXT_SKIP_DRAW, str);
}

static void Rg_Present(void)
{
    PutWindowTilemap(0);
    CopyWindowToVram(0, COPYWIN_FULL);
}

static void Rg_Number(u8 *dst, s32 v, u8 digits)
{
    ConvertIntToDecimalStringN(dst, v, STR_CONV_MODE_LEFT_ALIGN, digits);
}

static void Rg_Arrow(u8 y)
{
    static const u8 sArrow[] = {CHAR_RIGHT_ARROW, EOS};
    Rg_Text(6, y, sColGold, sArrow);
}

// ---------------------------------------------------------------------------
// content tables
// ---------------------------------------------------------------------------
static const u16 sStarterPool[] =
{
    SPECIES_TREECKO, SPECIES_TORCHIC, SPECIES_MUDKIP, SPECIES_RALTS, SPECIES_MACHOP, SPECIES_GEODUDE,
    SPECIES_ZIGZAGOON, SPECIES_SWABLU, SPECIES_TRAPINCH,
    // bestiary unlocks (4 per level)
    SPECIES_BAGON, SPECIES_LARVITAR, SPECIES_ARON, SPECIES_POOCHYENA,
    SPECIES_ABRA, SPECIES_SLAKOTH, SPECIES_NUMEL, SPECIES_CHARMANDER,
    SPECIES_BULBASAUR, SPECIES_SQUIRTLE, SPECIES_PIKACHU, SPECIES_DRATINI,
    SPECIES_CYNDAQUIL, SPECIES_TOTODILE, SPECIES_CHIKORITA, SPECIES_BELDUM,
};

static const u16 sItemPool[] =
{
    ITEM_LEFTOVERS, ITEM_CHOICE_BAND, ITEM_FOCUS_BAND, ITEM_SITRUS_BERRY, ITEM_SCOPE_LENS, ITEM_SHELL_BELL,
    ITEM_QUICK_CLAW, ITEM_LUM_BERRY, ITEM_BRIGHT_POWDER, ITEM_KINGS_ROCK,
};

enum { RW_HEAL, RW_REVIVE, RW_ATK, RW_DEF, RW_SPE, RW_HP, RW_KI, RW_CDR, RW_RECRUIT, RW_ITEM, RW_RUNE, RW_SPELL, RW_COUNT };

static const u8 sRwName[RW_COUNT][18] =
{
    _("Heal party"), _("Revive"), _("Power"), _("Guard"), _("Haste"), _("Vitality"),
    _("Ki Wellspring"), _("Quick Hands"), _("Recruit"), _("Held item"), _("New rune"), _("New spell"),
};

static const u8 sRwDesc[RW_COUNT][30] =
{
    _("Party +60% HP"), _("Fainted mon back, 50%"), _("Atk / SpA +6%"), _("Def / SpD +6%"),
    _("Speed +5%"), _("Max HP +8%"), _("Ki gain +20%"), _("Move cooldowns -6%"),
    _("A new mon joins"), _("A random item"), _("Random rune, a mon"), _("Random spell, a mon"),
};

static const u8 sUpName[UP_COUNT][16] =
{
    _("Vitality"), _("Might"), _("Resolve"), _("Ki Reserve"), _("Wider Choice"), _("Second Wind"), _("Bestiary"),
};
static const u8 sUpDesc[UP_COUNT][28] =
{
    _("+4% max HP per level"), _("+3% Atk/SpA per level"), _("+3% Def/SpD per level"), _("+100 starting Ki per level"),
    _("4 rewards instead of 3"), _("Revive once per run"), _("4 more starters per level"),
};

static const u8 sText_Victory[] = _("VICTORY!");
static const u8 sText_Defeat[] = _("DEFEAT...");
static const u8 sText_Slash[] = _("/");
static const u8 sText_Roguelike[] = _("ROGUELIKE");
static const u8 sText_Sanctuary[] = _("SANCTUARY");
static const u8 sText_StartRun[] = _("START RUN");
static const u8 sText_Back[] = _("BACK");
static const u8 sText_Essence[] = _("Essence:");
static const u8 sText_Best[] = _("Best floor:");
static const u8 sText_Runs[] = _("Runs:");
static const u8 sText_Wins[] = _("Cleared:");
static const u8 sText_ChooseStarter[] = _("Choose your partner");
static const u8 sText_ChooseReward[] = _("Choose a reward");
static const u8 sText_Floor[] = _("Floor");
static const u8 sText_Cost[] = _("Cost");
static const u8 sText_Max[] = _("MAX");
static const u8 sText_RunOver[] = _("RUN OVER");
static const u8 sText_RunWon[] = _("RUN COMPLETE!");
static const u8 sText_Reached[] = _("Floors cleared:");
static const u8 sText_Gained[] = _("Essence gained:");
static const u8 sText_PressA[] = _("Press A");
static const u8 sText_Hint[] = _("A: select   B: back");

// ---------------------------------------------------------------------------
// helpers on the party
// ---------------------------------------------------------------------------
static u8 PartyCount(void)
{
    u8 n = 0, i;
    for (i = 0; i < PARTY_SIZE; i++)
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE)
            n++;
    return n;
}

static void FloorLevels(u8 floor, u8 *playerLvl, u8 *enemyLvl)
{
    u8 l = 6 + floor * 2;
    if (l > 100) l = 100;
    *playerLvl = l;
    *enemyLvl = (floor % 5 == 0) ? l + 3 : l;
}

// a mon gets the moves it would know at this level (the last four of its level-up list)
static void RelearnMoves(struct Pokemon *mon)
{
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    u8 level = GetMonData(mon, MON_DATA_LEVEL);
    u16 list[40];
    u8 n = 0, i, k;
    for (i = 0; gLevelUpLearnsets[species][i] != LEVEL_UP_END && n < 40; i++)
    {
        u16 e = gLevelUpLearnsets[species][i];
        u8 lv = (e & LEVEL_UP_MOVE_LV) >> 9;
        u16 mv = e & LEVEL_UP_MOVE_ID;
        if (lv > level)
            break;
        for (k = 0; k < n; k++)
            if (list[k] == mv)
                break;
        if (k == n)
            list[n++] = mv;
    }
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        u16 mv = (n > i) ? list[n - 1 - i] : MOVE_NONE;
        u8 pp;
        SetMonMoveSlot(mon, mv, i);
        pp = mv ? gBattleMoves[mv].pp : 0;
        SetMonData(mon, MON_DATA_PP1 + i, &pp);
    }
}

static void SetMonLevel(struct Pokemon *mon, u8 level)
{
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    u32 exp = gExperienceTables[gSpeciesInfo[species].growthRate][level];
    u16 to;
    SetMonData(mon, MON_DATA_EXP, &exp);
    CalculateMonStats(mon);
    // evolve (level based only) as many times as the level allows
    while ((to = GetEvolutionTargetSpecies(mon, EVO_MODE_NORMAL, 0)) != SPECIES_NONE)
    {
        u8 nick[POKEMON_NAME_LENGTH + 1];
        u8 def[POKEMON_NAME_LENGTH + 1];
        GetMonData(mon, MON_DATA_NICKNAME, nick);
        GetSpeciesName(def, species);
        SetMonData(mon, MON_DATA_SPECIES, &to);
        if (StringCompare(nick, def) == 0)
        {
            GetSpeciesName(def, to);
            SetMonData(mon, MON_DATA_NICKNAME, def);
        }
        species = to;
        CalculateMonStats(mon);
    }
    RelearnMoves(mon);
}

static void HealMon(struct Pokemon *mon, u8 pct)
{
    u16 maxHp = GetMonData(mon, MON_DATA_MAX_HP);
    u16 hp = GetMonData(mon, MON_DATA_HP);
    u32 add = maxHp * pct / 100;
    if (hp == 0)
        return;
    hp = (hp + add > maxHp) ? maxHp : hp + add;
    SetMonData(mon, MON_DATA_HP, &hp);
}

static void ReviveMon(struct Pokemon *mon, u8 pct)
{
    u16 maxHp = GetMonData(mon, MON_DATA_MAX_HP);
    u16 hp = maxHp * pct / 100;
    if (hp == 0) hp = 1;
    SetMonData(mon, MON_DATA_HP, &hp);
    u32 st = 0;
    SetMonData(mon, MON_DATA_STATUS, &st);
}

static bool8 AllFainted(void)
{
    u8 i;
    for (i = 0; i < PARTY_SIZE; i++)
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE
         && GetMonData(&gPlayerParty[i], MON_DATA_HP) > 0)
            return FALSE;
    return TRUE;
}

static u16 BaseTotal(u16 sp)
{
    const struct SpeciesInfo *s = &gSpeciesInfo[sp];
    return s->baseHP + s->baseAttack + s->baseDefense + s->baseSpeed + s->baseSpAttack + s->baseSpDefense;
}

static bool8 IsLegendaryDex(u16 dex)
{
    return (dex >= 144 && dex <= 146) || dex == 150 || dex == 151 || (dex >= 243 && dex <= 245) || (dex >= 249 && dex <= 251)
        || (dex >= 377 && dex <= 386);
}

static u16 PickEnemySpecies(u8 floor)
{
    u16 lo = 180 + floor * 12, hi = 290 + floor * 13, tries;
    for (tries = 0; tries < 200; tries++)
    {
        u16 sp = 1 + Random() % (NUM_SPECIES - 1);
        u16 dex = SpeciesToNationalPokedexNum(sp), bst;
        if (dex == 0 || dex > NATIONAL_DEX_COUNT || IsLegendaryDex(dex) || gSpeciesInfo[sp].baseHP == 0)
            continue;
        bst = BaseTotal(sp);
        if (bst >= lo && bst <= hi)
            return sp;
    }
    return SPECIES_ZIGZAGOON;
}

// ---------------------------------------------------------------------------
// the run
// ---------------------------------------------------------------------------
static void Rg_Leave(void)
{
    gRogueMode = RG_OFF;
    SetMainCallback2(CB2_InitMainMenu);
}

static void CB2_RogueMenu(void);
static void CB2_RogueReward(void);
static void CB2_RogueOver(void);

static void DrawRogueMain(void)
{
    u8 buf[16];
    Rg_Clear();
    Rg_Text(12, 6, sColGold, sText_Roguelike);
    Rg_Text(16, 34, sCursor == 0 ? sColWhite : sColGray, sText_StartRun);
    Rg_Text(16, 50, sCursor == 1 ? sColWhite : sColGray, sText_Sanctuary);
    Rg_Text(16, 66, sCursor == 2 ? sColWhite : sColGray, sText_Back);
    Rg_Arrow(34 + sCursor * 16);
    Rg_TextS(12, 96, sColGreen, sText_Essence);
    Rg_Number(buf, sMeta.essence, 5);
    Rg_TextS(84, 96, sColWhite, buf);
    Rg_TextS(12, 109, sColGreen, sText_Best);
    Rg_Number(buf, sMeta.best, 3);
    Rg_TextS(84, 109, sColWhite, buf);
    Rg_TextS(12, 122, sColGreen, sText_Runs);
    Rg_Number(buf, sMeta.runs, 4);
    Rg_TextS(84, 122, sColWhite, buf);
    Rg_TextS(12, 135, sColGreen, sText_Wins);
    Rg_Number(buf, sMeta.victories, 4);
    Rg_TextS(84, 135, sColWhite, buf);
    Rg_Present();
}

static u8 StarterCount(void)
{
    u8 n = 9 + sMeta.up[UP_BEAST] * 4;
    return n > ARRAY_COUNT(sStarterPool) ? ARRAY_COUNT(sStarterPool) : n;
}

static void DrawDraft(void)
{
    u8 i;
    Rg_Clear();
    Rg_Text(12, 6, sColGold, sText_ChooseStarter);
    for (i = 0; i < sRun.nStarters; i++)
    {
        u8 name[POKEMON_NAME_LENGTH + 1];
        GetSpeciesName(name, sRun.starterSpecies[i]);
        Rg_Text(16, 30 + i * 16, sCursor == i ? sColWhite : sColGray, name);
        StringCopy(gStringVar1, gTypeNames[gSpeciesInfo[sRun.starterSpecies[i]].types[0]]);
        if (gSpeciesInfo[sRun.starterSpecies[i]].types[1] != gSpeciesInfo[sRun.starterSpecies[i]].types[0])
        {
            StringAppend(gStringVar1, sText_Slash);
            StringAppend(gStringVar1, gTypeNames[gSpeciesInfo[sRun.starterSpecies[i]].types[1]]);
        }
        Rg_TextS(100, 32 + i * 16, sColBlue, gStringVar1);
    }
    Rg_Arrow(30 + sCursor * 16);
    Rg_TextS(12, 146, sColGray, sText_Hint);
    Rg_Present();
}

static void DrawSanctuary(void)
{
    u8 i, buf[12];
    Rg_Clear();
    Rg_Text(12, 2, sColGold, sText_Sanctuary);
    Rg_Number(buf, sMeta.essence, 5);
    Rg_TextS(150, 5, sColGreen, sText_Essence);
    Rg_TextS(200, 5, sColWhite, buf);
    for (i = 0; i < UP_COUNT; i++)
    {
        const u8 *col = sCursor == i ? sColWhite : sColGray;
        Rg_TextS(16, 22 + i * 14, col, sUpName[i]);
        ConvertIntToDecimalStringN(buf, sMeta.up[i], STR_CONV_MODE_LEFT_ALIGN, 1);
        StringAppend(buf, sText_Slash);
        ConvertIntToDecimalStringN(&buf[StringLength(buf)], sUpMax[i], STR_CONV_MODE_LEFT_ALIGN, 1);
        Rg_TextS(98, 22 + i * 14, col, buf);
        if (sMeta.up[i] >= sUpMax[i])
            Rg_TextS(130, 22 + i * 14, sColGreen, sText_Max);
        else
        {
            Rg_Number(buf, sUpCost[i] * (sMeta.up[i] + 1), 5);
            Rg_TextS(130, 22 + i * 14, sMeta.essence >= sUpCost[i] * (sMeta.up[i] + 1) ? sColGold : sColRed, buf);
        }
    }
    Rg_TextS(6, 22 + sCursor * 14, sColGold, (const u8[]){CHAR_RIGHT_ARROW, EOS});
    Rg_TextS(12, 124, sColBlue, sUpDesc[sCursor]);
    Rg_TextS(12, 146, sColGray, sText_Hint);
    Rg_Present();
}

enum { SC_MAIN, SC_DRAFT, SC_SANCT };

static void PrepareDraft(void)
{
    u8 n = StarterCount(), i, k;
    u8 want = 3 + sMeta.up[UP_REWARD];
    sRun.nStarters = 0;
    for (i = 0; i < want && sRun.nStarters < ARRAY_COUNT(sRun.starterSpecies); i++)
    {
        u16 sp;
        do
        {
            sp = sStarterPool[Random() % n];
            for (k = 0; k < sRun.nStarters; k++)
                if (sRun.starterSpecies[k] == sp)
                    break;
        } while (k < sRun.nStarters);
        sRun.starterSpecies[sRun.nStarters++] = sp;
    }
}

static void CB2_RogueRunPlay(void)
{
    // the new game is initialised by CB2_NewGame; the field hook drives the run from there
    extern void CB2_NewGame(void);
    sSavedName[0] = EOS;
    StringCopy(sSavedName, gSaveBlock2Ptr->playerName);
    sRun.state = RS_INIT;
    gRogueMode = RG_RUN;
    SetMainCallback2(CB2_NewGame);
}

static void CB2_RogueMenu(void)
{
    AnimateSprites();
    BuildOamBuffer();
    RunTextPrinters();
    UpdatePaletteFade();
    if (sScreen == SC_MAIN)
    {
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawRogueMain(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor < 2) { sCursor++; PlaySE(SE_SELECT); DrawRogueMain(); }
        else if (JOY_NEW(B_BUTTON)) { PlaySE(SE_SELECT); SetMainCallback2(CB2_InitMainMenu); }
        else if (JOY_NEW(A_BUTTON))
        {
            PlaySE(SE_SELECT);
            if (sCursor == 0) { sScreen = SC_DRAFT; sCursor = 0; PrepareDraft(); DrawDraft(); }
            else if (sCursor == 1) { sScreen = SC_SANCT; sCursor = 0; DrawSanctuary(); }
            else SetMainCallback2(CB2_InitMainMenu);
        }
    }
    else if (sScreen == SC_DRAFT)
    {
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawDraft(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor + 1 < sRun.nStarters) { sCursor++; PlaySE(SE_SELECT); DrawDraft(); }
        else if (JOY_NEW(B_BUTTON)) { PlaySE(SE_SELECT); sScreen = SC_MAIN; sCursor = 0; DrawRogueMain(); }
        else if (JOY_NEW(A_BUTTON))
        {
            u16 starter = sRun.starterSpecies[sCursor];
            u8 w = sMeta.up[UP_WIND];
            PlaySE(SE_SELECT);
            memset(&sRun, 0, sizeof(sRun));
            sRun.starter = 1;
            sRun.starterSpecies[0] = starter;
            sRun.wind = w;
            sMeta.runs++;
            Meta_Save();
            CB2_RogueRunPlay();
        }
    }
    else if (sScreen == SC_SANCT)
    {
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawSanctuary(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor + 1 < UP_COUNT) { sCursor++; PlaySE(SE_SELECT); DrawSanctuary(); }
        else if (JOY_NEW(B_BUTTON)) { PlaySE(SE_SELECT); sScreen = SC_MAIN; sCursor = 1; DrawRogueMain(); }
        else if (JOY_NEW(A_BUTTON))
        {
            u16 cost = sUpCost[sCursor] * (sMeta.up[sCursor] + 1);
            if (sMeta.up[sCursor] < sUpMax[sCursor] && sMeta.essence >= cost)
            {
                sMeta.essence -= cost;
                sMeta.up[sCursor]++;
                Meta_Save();
                PlaySE(SE_SUCCESS);
            }
            else
                PlaySE(SE_FAILURE);
            DrawSanctuary();
        }
    }
}

void CB2_RtRogueStart(void)
{
    Meta_Load();
    Rg_Open();
    sScreen = SC_MAIN;
    sCursor = 0;
    DrawRogueMain();
    SetMainCallback2(CB2_RogueMenu);
}

// ---------------------------------------------------------------------------
// rewards
// ---------------------------------------------------------------------------
static bool8 AnyFainted(void)
{
    u8 i;
    for (i = 0; i < PARTY_SIZE; i++)
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE && GetMonData(&gPlayerParty[i], MON_DATA_HP) == 0)
            return TRUE;
    return FALSE;
}

static void PrepareOffers(void)
{
    u8 want = 3 + sMeta.up[UP_REWARD], n = 0, k;
    while (n < want)
    {
        u8 r = Random() % RW_COUNT;
        if ((r == RW_REVIVE && !AnyFainted()) || (r == RW_RECRUIT && PartyCount() >= PARTY_SIZE))
            continue;
        if (r == RW_HEAL && Random() % 2)   // heals are common, but not always on offer
            continue;
        for (k = 0; k < n; k++)
            if (sRun.offers[k] == r)
                break;
        if (k == n)
            sRun.offers[n++] = r;
    }
    sRun.nOffers = n;
}

static void DrawReward(void)
{
    u8 i, buf[6];
    Rg_Clear();
    Rg_Text(12, 4, sColGold, sText_ChooseReward);
    Rg_Number(buf, sRun.floor, 3);
    Rg_TextS(190, 8, sColGreen, sText_Floor);
    Rg_TextS(222, 8, sColWhite, buf);
    for (i = 0; i < sRun.nOffers; i++)
    {
        u8 r = sRun.offers[i];
        Rg_Text(16, 28 + i * 20, sCursor == i ? sColWhite : sColGray, sRwName[r]);
        Rg_TextS(110, 31 + i * 20, sColBlue, sRwDesc[r]);
    }
    Rg_Arrow(28 + sCursor * 20);
    Rg_Present();
}

static void GiveReward(u8 r)
{
    u8 i;
    switch (r)
    {
    case RW_HEAL:
        for (i = 0; i < PARTY_SIZE; i++)
            if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE)
                HealMon(&gPlayerParty[i], 60);
        break;
    case RW_REVIVE:
        for (i = 0; i < PARTY_SIZE; i++)
            if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE && GetMonData(&gPlayerParty[i], MON_DATA_HP) == 0)
            {
                ReviveMon(&gPlayerParty[i], 50);
                break;
            }
        break;
    case RW_ATK: sRun.buff[0]++; break;
    case RW_DEF: sRun.buff[1]++; break;
    case RW_SPE: sRun.buff[2]++; break;
    case RW_HP: sRun.buff[3]++; break;
    case RW_KI: sRun.buff[4]++; break;
    case RW_CDR: sRun.buff[5]++; break;
    case RW_RECRUIT:
        {
            u8 pl, el;
            struct Pokemon mon;
            u16 sp = PickEnemySpecies(sRun.floor / 2);
            FloorLevels(sRun.floor + 1, &pl, &el);
            CreateMon(&mon, sp, pl, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
            SetMonLevel(&mon, pl);
            GiveMonToPlayer(&mon);
        }
        break;
    case RW_ITEM:
        {
            u16 item = sItemPool[Random() % ARRAY_COUNT(sItemPool)];
            u8 pick = Random() % PartyCount();
            SetMonData(&gPlayerParty[pick], MON_DATA_HELD_ITEM, &item);
        }
        break;
    case RW_RUNE:
    case RW_SPELL:
        {
            u8 pick = Random() % PartyCount();
            u8 v = RtBuild_Get(&gPlayerParty[pick], 0);
            if (r == RW_RUNE)
                v = (v & 0xF0) | (1 + Random() % (RUNE_COUNT - 1));
            else
                v = (v & 0x0F) | ((1 + Random() % (SPELL_COUNT - 1)) << 4);
            RtBuild_Set(&gPlayerParty[pick], 0, v);
            RtBuild_SetSel(&gPlayerParty[pick], 0);
        }
        break;
    }
}

static void CB2_RogueReward(void)
{
    AnimateSprites();
    BuildOamBuffer();
    RunTextPrinters();
    UpdatePaletteFade();
    if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawReward(); }
    else if (JOY_NEW(DPAD_DOWN) && sCursor + 1 < sRun.nOffers) { sCursor++; PlaySE(SE_SELECT); DrawReward(); }
    else if (JOY_NEW(A_BUTTON))
    {
        PlaySE(SE_SELECT);
        GiveReward(sRun.offers[sCursor]);
        sRun.state = RS_WARP;
        SetMainCallback2(CB2_ReturnToField);
        gFieldCallback = FieldCB_ReturnToFieldNoScriptCheckMusic;
    }
}

static void OpenReward(void)
{
    Rg_Open();
    PrepareOffers();
    sCursor = 0;
    DrawReward();
    SetMainCallback2(CB2_RogueReward);
}

// ---------------------------------------------------------------------------
// game over / victory
// ---------------------------------------------------------------------------
static void CB2_RogueOver(void)
{
    AnimateSprites();
    BuildOamBuffer();
    RunTextPrinters();
    UpdatePaletteFade();
    if (JOY_NEW(A_BUTTON | B_BUTTON | START_BUTTON))
    {
        PlaySE(SE_SELECT);
        gRogueMode = RG_OFF;
        DoSoftReset();
    }
}

static void OpenOver(bool8 won)
{
    u8 buf[8];
    u16 cleared = won ? RUN_MAX_FLOOR : (sRun.floor > 0 ? sRun.floor - 1 : 0);
    u16 gain = 5 + cleared * 10 + sRun.bosses * 20 + (won ? 150 : 0);
    sMeta.essence += gain;
    if (cleared > sMeta.best)
        sMeta.best = cleared;
    if (won)
        sMeta.victories++;
    Meta_Save();
    Rg_Open();
    Rg_Clear();
    Rg_Text(70, 20, won ? sColGold : sColRed, won ? sText_RunWon : sText_RunOver);
    Rg_Text(40, 52, sColWhite, sText_Reached);
    Rg_Number(buf, cleared, 3);
    Rg_Text(160, 52, sColGreen, buf);
    Rg_Text(40, 70, sColWhite, sText_Gained);
    Rg_Number(buf, gain, 4);
    Rg_Text(160, 70, sColGold, buf);
    Rg_Text(40, 88, sColWhite, sText_Essence);
    Rg_Number(buf, sMeta.essence, 5);
    Rg_Text(160, 88, sColGold, buf);
    Rg_Text(84, 126, sColGray, sText_PressA);
    Rg_Present();
    PlayBGM(won ? MUS_LEVEL_UP : MUS_DUMMY);
    SetMainCallback2(CB2_RogueOver);
}

// ---------------------------------------------------------------------------
// challenge codes
// ---------------------------------------------------------------------------
#define CHAL_VER 1
static const u8 sAlpha[33] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

struct BitIO { u8 *buf; u16 pos; };

static void BitPut(struct BitIO *b, u32 v, u8 n)
{
    u8 i;
    for (i = 0; i < n; i++, b->pos++)
    {
        u8 bit = (v >> (n - 1 - i)) & 1;
        if (bit)
            b->buf[b->pos >> 3] |= 0x80 >> (b->pos & 7);
        else
            b->buf[b->pos >> 3] &= ~(0x80 >> (b->pos & 7));
    }
}

static u32 BitGet(struct BitIO *b, u8 n)
{
    u32 v = 0;
    u8 i;
    for (i = 0; i < n; i++, b->pos++)
        v = (v << 1) | ((b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
    return v;
}

static u16 BitHash(const u8 *buf, u16 bits)
{
    u32 h = 0x2f1, i;
    for (i = 0; i < bits; i++)
        h = (h * 33 + ((buf[i >> 3] >> (7 - (i & 7))) & 1) + 1) & 0xFFFFFF;
    return (h ^ (h >> 10)) & 0x3FF;
}

// text of a code: groups of four characters
static void EncodeParty(u8 count)
{
    u8 raw[64];
    struct BitIO b = {raw, 0};
    u8 i, k, nchar;
    u16 bits;
    memset(raw, 0, sizeof(raw));
    BitPut(&b, CHAL_VER, 3);
    BitPut(&b, count, 3);
    for (i = 0; i < count; i++)
    {
        struct Pokemon *m = &gPlayerParty[i];
        u8 build = RtBuild_Get(m, RtBuild_GetSel(m));
        BitPut(&b, GetMonData(m, MON_DATA_SPECIES), 9);
        BitPut(&b, GetMonData(m, MON_DATA_LEVEL), 7);
        BitPut(&b, GetNature(m), 5);
        BitPut(&b, GetMonData(m, MON_DATA_ABILITY_NUM), 1);
        BitPut(&b, GetMonData(m, MON_DATA_HELD_ITEM), 9);
        for (k = 0; k < MAX_MON_MOVES; k++)
            BitPut(&b, GetMonData(m, MON_DATA_MOVE1 + k), 9);
        BitPut(&b, build & 15, 3);
        BitPut(&b, build >> 4, 3);
    }
    bits = b.pos;
    BitPut(&b, BitHash(raw, bits), 10);
    nchar = (b.pos + 4) / 5;
    for (i = 0; i < nchar; i++)
    {
        u8 v = 0;
        for (k = 0; k < 5; k++)
        {
            u16 p = i * 5 + k;
            v <<= 1;
            if (p < b.pos)
                v |= (raw[p >> 3] >> (7 - (p & 7))) & 1;
        }
        sCodeBuf[i] = sAlpha[v];
    }
    sCodeBuf[nchar] = 0xFF;
    sCodeBuf[nchar + 1] = EOS;
}

static u8 CharVal(u8 c)
{
    u8 i;
    // letters are matched case-insensitively; I -> 1 is not in the alphabet, O -> 0 neither
    for (i = 0; i < 32; i++)
        if (sAlpha[i] == c)
            return i;
    return 0xFF;
}

static bool8 DecodeCode(u8 *code, u8 n)
{
    u8 raw[64];
    struct BitIO b = {raw, 0};
    u8 i, k, count;
    u16 bits;
    memset(raw, 0, sizeof(raw));
    if (n < 8 || n > 92)
        return FALSE;
    for (i = 0; i < n; i++)
    {
        u8 v = CharVal(code[i]);
        if (v == 0xFF)
            return FALSE;
        for (k = 0; k < 5; k++)
        {
            u16 p = i * 5 + k;
            if (p >= sizeof(raw) * 8)
                break;
            if ((v >> (4 - k)) & 1)
                raw[p >> 3] |= 0x80 >> (p & 7);
        }
    }
    if (BitGet(&b, 3) != CHAL_VER)
        return FALSE;
    count = BitGet(&b, 3);
    if (count < 1 || count > PARTY_SIZE)
        return FALSE;
    bits = 6 + count * 73;
    if (n * 5 < bits + 10)
        return FALSE;
    {
        struct BitIO c = {raw, bits};
        if (BitGet(&c, 10) != BitHash(raw, bits))
            return FALSE;
    }
    ZeroEnemyPartyMons();
    for (i = 0; i < count; i++)
    {
        u16 sp = BitGet(&b, 9), item;
        u8 lvl = BitGet(&b, 7), nat = BitGet(&b, 5), abil = BitGet(&b, 1);
        u16 mv[MAX_MON_MOVES];
        u8 rune, spell;
        item = BitGet(&b, 9);
        for (k = 0; k < MAX_MON_MOVES; k++)
            mv[k] = BitGet(&b, 9);
        rune = BitGet(&b, 3);
        spell = BitGet(&b, 3);
        if (sp == 0 || sp >= NUM_SPECIES || gSpeciesInfo[sp].baseHP == 0 || lvl < 1 || lvl > 100 || nat >= NUM_NATURES)
            return FALSE;
        if (rune >= RUNE_COUNT || spell >= SPELL_COUNT)
            return FALSE;
        CreateMonWithNature(&gEnemyParty[i], sp, lvl, 24, nat);
        SetMonData(&gEnemyParty[i], MON_DATA_ABILITY_NUM, &abil);
        SetMonData(&gEnemyParty[i], MON_DATA_HELD_ITEM, &item);
        for (k = 0; k < MAX_MON_MOVES; k++)
        {
            u8 pp;
            if (mv[k] >= MOVES_COUNT)
                mv[k] = MOVE_NONE;
            SetMonMoveSlot(&gEnemyParty[i], mv[k], k);
            pp = mv[k] ? gBattleMoves[mv[k]].pp : 0;
            SetMonData(&gEnemyParty[i], MON_DATA_PP1 + k, &pp);
        }
        gRogueChalBuild[i] = rune | (spell << 4);
        sChalSpecies[i] = sp;
    }
    sChalMons = count;
    return TRUE;
}

// ---- challenge screens: create a code / type a code ----
enum { CS_MAIN, CS_COUNT, CS_SHOW, CS_ENTER, CS_BAD };

static const u8 sText_Challenge[] = _("CHALLENGE");
static const u8 sText_CreateCode[] = _("CREATE CODE");
static const u8 sText_EnterCode[] = _("ENTER CODE");
static const u8 sText_HowMany[] = _("How many Pokemon?");
static const u8 sText_ShareCode[] = _("Share this code:");
static const u8 sText_TypeCode[] = _("Type the code:");
static const u8 sText_BadCode[] = _("That code is not valid.");
static const u8 sText_Del[] = _("DEL");
static const u8 sText_Go[] = _("GO");
static const u8 sText_NoParty[] = _("You have no Pokemon.");
static const u8 sText_ChalWins[] = _("Challenges won:");

static EWRAM_DATA u8 sTyped[96] = {0};
static EWRAM_DATA u8 sTypedLen = 0;
static EWRAM_DATA u8 sKeyCur = 0;

static void DrawChalMain(void)
{
    u8 buf[8];
    Rg_Clear();
    Rg_Text(12, 6, sColGold, sText_Challenge);
    Rg_Text(16, 34, sCursor == 0 ? sColWhite : sColGray, sText_CreateCode);
    Rg_Text(16, 50, sCursor == 1 ? sColWhite : sColGray, sText_EnterCode);
    Rg_Text(16, 66, sCursor == 2 ? sColWhite : sColGray, sText_Back);
    Rg_Arrow(34 + sCursor * 16);
    Rg_TextS(12, 100, sColGreen, sText_ChalWins);
    Rg_Number(buf, sMeta.chalWins, 4);
    Rg_TextS(100, 100, sColWhite, buf);
    Rg_Present();
}

static void DrawChalCount(void)
{
    u8 buf[4];
    Rg_Clear();
    Rg_Text(12, 6, sColGold, sText_CreateCode);
    Rg_Text(12, 40, sColWhite, sText_HowMany);
    Rg_Number(buf, sSub, 1);
    Rg_Text(150, 40, sColGold, buf);
    Rg_TextS(12, 146, sColGray, sText_Hint);
    Rg_Present();
}

// the code on screen: groups of four characters, four groups per line
static void PrintCodeLines(const u8 *code, u8 len, u8 y0, const u8 *col)
{
    u8 line, g, k, p = 0;
    for (line = 0; p < len; line++)
    {
        u8 str[24], n = 0;
        for (g = 0; g < 4 && p < len; g++)
        {
            for (k = 0; k < 4 && p < len; k++)
                str[n++] = code[p++];
            if (g < 3 && p < len)
                str[n++] = CHAR_SPACE;
        }
        str[n] = EOS;
        Rg_Text(10, y0 + line * 16, col, str);
    }
}

static void ConvertCodeToGameChars(u8 *dst, const u8 *src, u8 len)
{
    u8 i;
    for (i = 0; i < len; i++)
    {
        u8 c = src[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? CHAR_A + (c - 'A') : (c >= '2' && c <= '9' ? CHAR_2 + (c - '2') : CHAR_SPACE);
    }
}

static u8 GameCharToAscii(u8 g)
{
    if (g >= CHAR_A && g <= CHAR_Z)
        return 'A' + (g - CHAR_A);
    if (g >= CHAR_0 && g <= CHAR_9)
        return '0' + (g - CHAR_0);
    return '?';
}

static void DrawChalShow(void)
{
    u8 shown[80], n = 0;
    while (sCodeBuf[n] != 0xFF && n < 80)
    {
        shown[n] = sCodeBuf[n];
        n++;
    }
    {
        u8 conv[80];
        ConvertCodeToGameChars(conv, shown, n);
        Rg_Clear();
        Rg_Text(12, 6, sColGold, sText_ShareCode);
        PrintCodeLines(conv, n, 30, sColWhite);
    }
    Rg_TextS(12, 146, sColGray, sText_Hint);
    Rg_Present();
}

static void DrawChalEnter(void)
{
    u8 i, conv[90];
    Rg_Clear();
    Rg_Text(12, 2, sColGold, sText_TypeCode);
    ConvertCodeToGameChars(conv, sTyped, sTypedLen);
    PrintCodeLines(conv, sTypedLen, 20, sColGreen);
    // keyboard: 4 rows of 8 + DEL / GO
    for (i = 0; i < 32; i++)
    {
        u8 s[2] = {CHAR_A + (sAlpha[i] >= 'A' ? sAlpha[i] - 'A' : 0), EOS};
        if (sAlpha[i] >= '2')
            s[0] = CHAR_2 + (sAlpha[i] - '2');
        Rg_TextS(14 + (i % 8) * 24, 86 + (i / 8) * 13, sKeyCur == i ? sColGold : sColWhite, s);
    }
    Rg_TextS(14, 138, sKeyCur == 32 ? sColGold : sColWhite, sText_Del);
    Rg_TextS(62, 138, sKeyCur == 33 ? sColGold : sColWhite, sText_Go);
    if (sKeyCur < 32)
        Rg_TextS(6 + (sKeyCur % 8) * 24, 86 + (sKeyCur / 8) * 13, sColRed, (const u8[]){CHAR_RIGHT_ARROW, EOS});
    Rg_Present();
}

static void CB2_ChallengeMenu(void);

static void StartChallengeBattle(void);

static void CB2_ChallengeMenu(void)
{
    AnimateSprites();
    BuildOamBuffer();
    RunTextPrinters();
    UpdatePaletteFade();
    switch (sScreen)
    {
    case CS_MAIN:
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawChalMain(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor < 2) { sCursor++; PlaySE(SE_SELECT); DrawChalMain(); }
        else if (JOY_NEW(B_BUTTON)) { PlaySE(SE_SELECT); SetMainCallback2(CB2_InitMainMenu); }
        else if (JOY_NEW(A_BUTTON))
        {
            PlaySE(SE_SELECT);
            if (sCursor == 0)
            {
                if (PartyCount() == 0)
                    return;
                sScreen = CS_COUNT;
                sSub = PartyCount() < 3 ? PartyCount() : 3;  // up to 4 Pokemon (a 4 mon code has 62 characters)
                DrawChalCount();
            }
            else if (sCursor == 1)
            {
                sScreen = CS_ENTER;
                sTypedLen = 0;
                sKeyCur = 0;
                DrawChalEnter();
            }
            else
                SetMainCallback2(CB2_InitMainMenu);
        }
        break;
    case CS_COUNT:
        if (JOY_NEW(DPAD_LEFT) && sSub > 1) { sSub--; PlaySE(SE_SELECT); DrawChalCount(); }
        else if (JOY_NEW(DPAD_RIGHT) && sSub < PartyCount() && sSub < 4) { sSub++; PlaySE(SE_SELECT); DrawChalCount(); }
        else if (JOY_NEW(B_BUTTON)) { PlaySE(SE_SELECT); sScreen = CS_MAIN; sCursor = 0; DrawChalMain(); }
        else if (JOY_NEW(A_BUTTON))
        {
            PlaySE(SE_SELECT);
            EncodeParty(sSub);
            sScreen = CS_SHOW;
            DrawChalShow();
        }
        break;
    case CS_SHOW:
        if (JOY_NEW(A_BUTTON | B_BUTTON)) { PlaySE(SE_SELECT); sScreen = CS_MAIN; sCursor = 0; DrawChalMain(); }
        break;
    case CS_ENTER:
        if (JOY_NEW(DPAD_RIGHT)) { sKeyCur = (sKeyCur + 1) % 34; PlaySE(SE_SELECT); DrawChalEnter(); }
        else if (JOY_NEW(DPAD_LEFT)) { sKeyCur = (sKeyCur + 33) % 34; PlaySE(SE_SELECT); DrawChalEnter(); }
        else if (JOY_NEW(DPAD_DOWN)) { sKeyCur = sKeyCur < 24 ? sKeyCur + 8 : (sKeyCur < 32 ? 32 + (sKeyCur >= 28) : 0); PlaySE(SE_SELECT); DrawChalEnter(); }
        else if (JOY_NEW(DPAD_UP)) { sKeyCur = sKeyCur >= 32 ? (sKeyCur == 32 ? 24 : 28) : (sKeyCur >= 8 ? sKeyCur - 8 : 32); PlaySE(SE_SELECT); DrawChalEnter(); }
        else if (JOY_NEW(B_BUTTON))
        {
            PlaySE(SE_SELECT);
            if (sTypedLen)
                sTypedLen--;
            else
            {
                sScreen = CS_MAIN;
                sCursor = 1;
                DrawChalMain();
                return;
            }
            DrawChalEnter();
        }
        else if (JOY_NEW(A_BUTTON))
        {
            PlaySE(SE_SELECT);
            if (sKeyCur < 32)
            {
                if (sTypedLen < 88)
                    sTyped[sTypedLen++] = sAlpha[sKeyCur];
            }
            else if (sKeyCur == 32)
            {
                if (sTypedLen)
                    sTypedLen--;
            }
            else
            {
                if (PartyCount() == 0)
                {
                    sScreen = CS_BAD;
                    Rg_Clear();
                    Rg_Text(24, 60, sColRed, sText_NoParty);
                    Rg_Text(84, 126, sColGray, sText_PressA);
                    Rg_Present();
                    return;
                }
                if (DecodeCode(sTyped, sTypedLen))
                {
                    StartChallengeBattle();
                    return;
                }
                sScreen = CS_BAD;
                Rg_Clear();
                Rg_Text(24, 60, sColRed, sText_BadCode);
                Rg_Text(84, 126, sColGray, sText_PressA);
                Rg_Present();
                return;
            }
            DrawChalEnter();
        }
        break;
    case CS_BAD:
        if (JOY_NEW(A_BUTTON | B_BUTTON)) { sScreen = CS_ENTER; DrawChalEnter(); }
        break;
    }
}

void CB2_RtChallengeStart(void)
{
    Meta_Load();
    Rg_Open();
    sScreen = CS_MAIN;
    sCursor = 0;
    DrawChalMain();
    SetMainCallback2(CB2_ChallengeMenu);
}

static void StartChallengeBattle(void)
{
    extern void CB2_NewGame(void);
    memcpy(sSavedParty, gPlayerParty, sizeof(sSavedParty));
    memcpy(sSavedBuilds, RtSave_Get(), sizeof(sSavedBuilds));
    StringCopy(sSavedName, gSaveBlock2Ptr->playerName);
    gRogueMode = RG_CHALLENGE;
    sRun.state = RS_INIT;
    sRun.floor = 1;
    SetMainCallback2(CB2_NewGame);
}

// ---------------------------------------------------------------------------
// the field hook: runs and challenges are driven from here
// ---------------------------------------------------------------------------
static void ChooseMap(bool8 boss)
{
    u8 n = ARRAY_COUNT(sRogueMaps), i, tries;
    u8 pick = 0;
    for (tries = 0; tries < 40; tries++)
    {
        i = Random() % n;
        if (boss)
        {
            // guardians live in the desert, caves, fire and ghost places, forests and the ice
            if (sRogueMaps[i].biome == RGB_GRASS && sRogueMaps[i].num != MAP_NUM(MAP_PETALBURG_WOODS))
                continue;
        }
        else if (sRogueMaps[i].biome == RGB_GRASS || Random() % 3 == 0)
        {
            // normal floors: anything
        }
        pick = i;
        break;
    }
    SetWarpDestination(sRogueMaps[pick].group, sRogueMaps[pick].num, WARP_ID_NONE, sRogueMaps[pick].x, sRogueMaps[pick].y);
}

static void MakeWildEnemy(u8 floor)
{
    u8 pl, el;
    u16 sp;
    FloorLevels(floor, &pl, &el);
    sp = PickEnemySpecies(floor);
    ZeroEnemyPartyMons();
    CreateMon(&gEnemyParty[0], sp, el, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
}

bool8 RtRogue_FieldInput(void)
{
    if (gRogueMode == RG_OFF)
        return FALSE;
    switch (sRun.state)
    {
    case RS_INIT:
        // the new game has loaded its first map: take over
        if (gRogueMode == RG_CHALLENGE)
        {
            memcpy(gPlayerParty, sSavedParty, sizeof(sSavedParty));
            CalculatePlayerPartyCount();
            StringCopy(gSaveBlock2Ptr->playerName, sSavedName);
            memcpy(RtSave_Get(), sSavedBuilds, sizeof(sSavedBuilds));
        }
        else
        {
            struct Pokemon mon;
            u8 pl, el;
            FloorLevels(1, &pl, &el);
            ZeroPlayerPartyMons();
            CreateMon(&mon, sRun.starterSpecies[0], pl, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
            SetMonLevel(&mon, pl);
            GiveMonToPlayer(&mon);
            StringCopy(gSaveBlock2Ptr->playerName, sSavedName);
            FlagSet(FLAG_SYS_POKEMON_GET);
            FlagSet(FLAG_SYS_B_DASH);
            RtSave_Get();
        }
        sRun.state = RS_WARP;
        return FALSE;
    case RS_WARP:
        if (gRogueMode == RG_RUN)
        {
            u8 pl, el, i;
            sRun.floor++;
            gRogueBoss = (sRun.floor % 5 == 0);
            FloorLevels(sRun.floor, &pl, &el);
            for (i = 0; i < PARTY_SIZE; i++)
            {
                struct Pokemon *m = &gPlayerParty[i];
                if (GetMonData(m, MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE && GetMonData(m, MON_DATA_LEVEL) < pl)
                    SetMonLevel(m, pl);
            }
        }
        ChooseMap(gRogueMode == RG_RUN && gRogueBoss);
        DoWarp();
        sRun.state = RS_WAIT;
        sRun.timer = 0;
        return FALSE;
    case RS_WAIT:
        if (++sRun.timer < 90 || gPaletteFade.active)
            return FALSE;
        gRogueBattleEntered = FALSE;
        sRun.state = RS_FIGHT;
        if (gRogueMode == RG_CHALLENGE)
        {
            extern void RtDebug_StartTrainerBattle(u16 trainerId);
            RtDebug_StartTrainerBattle(TRAINER_ALLEN);
        }
        else
        {
            MakeWildEnemy(sRun.floor);
            BattleSetup_StartWildBattle();
        }
        return FALSE;
    case RS_FIGHT:
        if (!gRogueBattleEntered)
            return FALSE;           // the battle transition is still running on the field
        // back from the battle
        if (gRogueMode == RG_CHALLENGE)
        {
            sRun.state = RS_DONE;
            Rg_Open();
            Rg_Clear();
            {
                bool8 won = (gBattleOutcome == B_OUTCOME_WON);
                if (won)
                {
                    Meta_Load();
                    sMeta.chalWins++;
                    Meta_Save();
                }
                Rg_Text(70, 44, won ? sColGold : sColRed, won ? sText_Victory : sText_Defeat);
                Rg_Text(84, 126, sColGray, sText_PressA);
                Rg_Present();
            }
            SetMainCallback2(CB2_RogueOver);
            return FALSE;
        }
        if (gBattleOutcome == B_OUTCOME_WON || gBattleOutcome == B_OUTCOME_CAUGHT)
        {
            if (gRogueBoss)
                sRun.bosses++;
            if (sRun.floor >= RUN_MAX_FLOOR)
            {
                sRun.state = RS_DONE;
                OpenOver(TRUE);
                return FALSE;
            }
            sRun.state = RS_DONE;
            OpenReward();
            return FALSE;
        }
        // lost: a second wind brings the party back once
        if (sRun.wind > 0)
        {
            u8 i;
            sRun.wind--;
            for (i = 0; i < PARTY_SIZE; i++)
                if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE)
                    ReviveMon(&gPlayerParty[i], 60);
            sRun.floor--;
            sRun.state = RS_WARP;
            return FALSE;
        }
        sRun.state = RS_DONE;
        OpenOver(FALSE);
        return FALSE;
    }
    return FALSE;
}

// debug: encode the party, decode it again and fight it
EWRAM_DATA u8 gRogueSelfTest[16] = {0};
void RtRogue_DebugChallenge(void)
{
    u8 n = PartyCount(), len = 0, i;
    if (n > 4) n = 4;
    EncodeParty(n);
    while (sCodeBuf[len] != 0xFF && len < 92)
        len++;
    memcpy(sTyped, sCodeBuf, len);
    sTypedLen = len;
    gRogueSelfTest[0] = len;
    gRogueSelfTest[1] = DecodeCode(sTyped, len);
    gRogueSelfTest[2] = sChalMons;
    for (i = 0; i < 4 && i < n; i++)
        gRogueSelfTest[3 + i] = sChalSpecies[i] == GetMonData(&gPlayerParty[i], MON_DATA_SPECIES);
    gRogueMode = RG_CHALLENGE;
    sRun.state = RS_FIGHT;
    gRogueBattleEntered = FALSE;
    {
        extern void RtDebug_StartTrainerBattle(u16 trainerId);
        RtDebug_StartTrainerBattle(TRAINER_ALLEN);
    }
}

u8 RtRogue_AiSkill(void)
{
    u8 sk;
    if (gRogueMode == RG_CHALLENGE)
        return 8;
    sk = 3 + sRun.floor / 3;
    return sk > 9 ? 9 : sk;
}
