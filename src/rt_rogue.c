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
#include "constants/battle_move_effects.h"
#include "script_pokemon_util.h"
#include "constants/maps.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/species.h"
#include "constants/pokedex.h"
#include "constants/trainers.h"
#include "constants/opponents.h"
#include "data/rt/rogue_maps.h"

extern const struct Evolution gEvolutionTable[][EVOS_PER_MON];

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
EWRAM_DATA u8 gRogueMode = 0;            // RG_*
EWRAM_DATA u8 gRogueBattleEntered = 0;   // set by the battle engine when a battle starts
EWRAM_DATA u8 gRogueBoss = 0;
EWRAM_DATA u8 gRogueChalBuild[PARTY_SIZE] = {0};
enum { RG_OFF, RG_RUN, RG_CHALLENGE, RG_FRONTIER };

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
    u8 fSize, fDiff, fSpeed, fFac, nPick, sub;
    u16 pick[6];
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

static void CB2_RogueReward(void);
static void CB2_RogueOver(void);

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
// BUILD EDITOR (also reachable from the start menu)
// ---------------------------------------------------------------------------
static EWRAM_DATA u8 sBeMon = 0;
static EWRAM_DATA u8 sBeRow = 0;
static EWRAM_DATA u8 sBeEdit = 0;

static void BeMonName(u8 i, u8 *dst)
{
    GetMonData(&gPlayerParty[i], MON_DATA_NICKNAME, dst);
    StringGet_Nickname(dst);
}

static void DrawBuildList(void)
{
    u8 i, n = PartyCount(), name[POKEMON_NAME_LENGTH + 1];
    static const u8 sTitle[] = _("BUILDS");
    static const u8 sHint2[] = _("A: edit   B: back");
    Rg_Clear();
    Rg_Text(12, 4, sColGold, sTitle);
    for (i = 0; i < n; i++)
    {
        u8 v = RtBuild_Get(&gPlayerParty[i], RtBuild_GetSel(&gPlayerParty[i]));
        BeMonName(i, name);
        Rg_Text(16, 22 + i * 16, sBeMon == i ? sColWhite : sColGray, name);
        StringCopy(gStringVar1, RtBuild_RuneName(v & 15));
        StringAppend(gStringVar1, sText_Slash);
        StringAppend(gStringVar1, RtBuild_SpellName(v >> 4));
        Rg_TextS(110, 24 + i * 16, sColBlue, gStringVar1);
    }
    Rg_Arrow(22 + sBeMon * 16);
    Rg_TextS(12, 146, sColGray, sHint2);
    Rg_Present();
}

static void DrawBuildEdit(void)
{
    static const u8 sLbl[5][9] = {_("1 Rune"), _("1 Spell"), _("2 Rune"), _("2 Spell"), _("Active")};
    static const u8 sHint3[] = _("Left/Right: change   B: back");
    u8 i, name[POKEMON_NAME_LENGTH + 1];
    struct Pokemon *mon = &gPlayerParty[sBeMon];
    Rg_Clear();
    BeMonName(sBeMon, name);
    Rg_Text(12, 4, sColGold, name);
    for (i = 0; i < 5; i++)
    {
        const u8 *col = sBeRow == i ? sColWhite : sColGray;
        Rg_Text(16, 26 + i * 16, col, sLbl[i]);
        if (i < 4)
        {
            u8 v = RtBuild_Get(mon, i / 2);
            Rg_Text(90, 26 + i * 16, col, (i & 1) ? RtBuild_SpellName(v >> 4) : RtBuild_RuneName(v & 15));
        }
        else
        {
            u8 b[4];
            ConvertIntToDecimalStringN(b, RtBuild_GetSel(mon) + 1, STR_CONV_MODE_LEFT_ALIGN, 1);
            Rg_Text(90, 26 + i * 16, col, b);
        }
    }
    Rg_Arrow(26 + sBeRow * 16);
    {
        static const u8 sRuneDesc[RUNE_COUNT][34] = {
            _("No rune."), _("+10% Atk and Sp.Atk."), _("Heal 1/8 of the damage you deal."),
            _("Attackers take 1/8 of the hit."), _("+15% Def and Sp.Def."), _("+15% Speed."),
            _("Moves recharge 12% faster."), _("Ki builds 50% faster.")};
        static const u8 sSpellDesc[SPELL_COUNT][34] = {
            _("No spell."), _("4s: take half damage."), _("Heal 30% of max HP."),
            _("Cure your status condition."), _("Dash a short way forward."),
            _("6s: +30% Atk, +15% Spd, quick moves"), _("Instantly gain lots of Ki.")};
        static const u8 sRuneHdr[] = _("RUNE (always active):");
        static const u8 sSpellHdr[] = _("SPELL (hold START in battle):");
        static const u8 sActHdr[] = _("Which build is used in battle.");
        static const u8 sSpellCd[] = _("Recharge: 45 seconds.");
        u8 v = RtBuild_Get(mon, sBeRow / 2);
        if (sBeRow == 4)
            Rg_TextS(12, 108, sColBlue, sActHdr);
        else if (sBeRow & 1)
        {
            Rg_TextS(12, 105, sColGold, sSpellHdr);
            Rg_TextS(12, 117, sColBlue, sSpellDesc[(v >> 4) % SPELL_COUNT]);
            Rg_TextS(12, 129, sColGray, sSpellCd);
        }
        else
        {
            Rg_TextS(12, 105, sColGold, sRuneHdr);
            Rg_TextS(12, 117, sColBlue, sRuneDesc[(v & 15) % RUNE_COUNT]);
        }
    }
    Rg_TextS(12, 146, sColGray, sHint3);
    Rg_Present();
}

void CB2_RtBuildEditor(void)
{
    static bool8 sInit = FALSE;
    (void)sInit;
    if (gMain.state == 0)
    {
        Rg_Open();
        sBeMon = 0;
        sBeRow = 0;
        sBeEdit = 0;
        DrawBuildList();
        gMain.state = 1;
        return;
    }
    AnimateSprites();
    BuildOamBuffer();
    RunTextPrinters();
    UpdatePaletteFade();
    if (!sBeEdit)
    {
        u8 n = PartyCount();
        if (JOY_NEW(DPAD_UP) && sBeMon > 0) { sBeMon--; PlaySE(SE_SELECT); DrawBuildList(); }
        else if (JOY_NEW(DPAD_DOWN) && sBeMon + 1 < n) { sBeMon++; PlaySE(SE_SELECT); DrawBuildList(); }
        else if (JOY_NEW(A_BUTTON) && n > 0) { sBeEdit = 1; sBeRow = 0; PlaySE(SE_SELECT); DrawBuildEdit(); }
        else if (JOY_NEW(B_BUTTON))
        {
            PlaySE(SE_SELECT);
            gMain.state = 0;
            SetMainCallback2(gMain.savedCallback);
        }
    }
    else
    {
        struct Pokemon *mon = &gPlayerParty[sBeMon];
        if (JOY_NEW(DPAD_UP) && sBeRow > 0) { sBeRow--; PlaySE(SE_SELECT); DrawBuildEdit(); }
        else if (JOY_NEW(DPAD_DOWN) && sBeRow < 4) { sBeRow++; PlaySE(SE_SELECT); DrawBuildEdit(); }
        else if (JOY_NEW(DPAD_LEFT | DPAD_RIGHT | A_BUTTON))
        {
            s8 d = JOY_NEW(DPAD_LEFT) ? -1 : 1;
            if (sBeRow == 4)
                RtBuild_SetSel(mon, RtBuild_GetSel(mon) ^ 1);
            else
            {
                u8 pr = sBeRow / 2, v = RtBuild_Get(mon, pr);
                if (sBeRow & 1)
                {
                    s8 s = (v >> 4) + d;
                    if (s < 0) s = SPELL_COUNT - 1;
                    if (s >= SPELL_COUNT) s = 0;
                    v = (v & 0x0F) | (s << 4);
                }
                else
                {
                    s8 r = (v & 15) + d;
                    if (r < 0) r = RUNE_COUNT - 1;
                    if (r >= RUNE_COUNT) r = 0;
                    v = (v & 0xF0) | r;
                }
                RtBuild_Set(mon, pr, v);
            }
            PlaySE(SE_SELECT);
            DrawBuildEdit();
        }
        else if (JOY_NEW(B_BUTTON)) { sBeEdit = 0; PlaySE(SE_SELECT); DrawBuildList(); }
    }
}

// ---------------------------------------------------------------------------
// BATTLE FRONTIER: a Lv50 draft run through one facility
// ---------------------------------------------------------------------------
#define FR_LEVEL    50
#define FR_SET_LEN  7
#define FR_MAX_ROUND 21
#define FR_BST_LO   430
#define FR_BST_HI   530

EWRAM_DATA u8 gRogueSpeedPct = 80;

static const struct { s8 group, num; u8 x, y; } sFac[] =
{
    {MAP_GROUP(MAP_BATTLE_FRONTIER_BATTLE_TOWER_BATTLE_ROOM), MAP_NUM(MAP_BATTLE_FRONTIER_BATTLE_TOWER_BATTLE_ROOM), 5, 4},
    {MAP_GROUP(MAP_BATTLE_FRONTIER_BATTLE_ARENA_BATTLE_ROOM), MAP_NUM(MAP_BATTLE_FRONTIER_BATTLE_ARENA_BATTLE_ROOM), 8, 5},
    {MAP_GROUP(MAP_BATTLE_FRONTIER_BATTLE_DOME_BATTLE_ROOM), MAP_NUM(MAP_BATTLE_FRONTIER_BATTLE_DOME_BATTLE_ROOM), 10, 5},
    {MAP_GROUP(MAP_BATTLE_FRONTIER_BATTLE_PALACE_BATTLE_ROOM), MAP_NUM(MAP_BATTLE_FRONTIER_BATTLE_PALACE_BATTLE_ROOM), 7, 5},
    {MAP_GROUP(MAP_BATTLE_FRONTIER_BATTLE_FACTORY_BATTLE_ROOM), MAP_NUM(MAP_BATTLE_FRONTIER_BATTLE_FACTORY_BATTLE_ROOM), 6, 6},
};
static const u8 sFacName[5][16] = {_("Battle Tower"), _("Battle Arena"), _("Battle Dome"), _("Battle Palace"), _("Battle Factory")};
static const u8 sDiffName[3][8] = {_("Easy"), _("Normal"), _("Hard")};
static const u8 sDiffDesc[3][36] = {_("Gentler foes, simple tactics"), _("Even match, smart foes"), _("Strong foes, sharp tactics")};
static const u8 sSpeedName[3][8] = {_("Slow"), _("Normal"), _("Fast")};
static const u8 sSpeedPct[3] = {60, 80, 100};
static const u8 sSizeName[2][3] = {_("3"), _("6")};

static const u8 sText_Frontier[] = _("BATTLE FRONTIER");
static const u8 sText_Records[] = _("RECORDS");
static const u8 sText_PartySize[] = _("Party size");
static const u8 sText_Difficulty[] = _("Difficulty");
static const u8 sText_Speed[] = _("Battle speed");
static const u8 sText_Facility[] = _("Facility");
static const u8 sText_Start[] = _("START");
static const u8 sText_Lv50[] = _("Every battle is at Lv 50");
static const u8 sText_PickSlot[] = _("Pick your team");
static const u8 sText_Of[] = _(" of ");
static const u8 sText_BstLbl[] = _("Total");
static const u8 sText_Round[] = _("Round");
static const u8 sText_Cleared[] = _("cleared!");
static const u8 sText_SetCleared[] = _("SET CLEARED!");
static const u8 sText_Continue[] = _("CONTINUE");
static const u8 sText_Recruit[] = _("RECRUIT");
static const u8 sText_Builds[] = _("BUILDS");
static const u8 sText_Retire[] = _("RETIRE");
static const u8 sText_NextBoss[] = _("Next: the facility leader");
static const u8 sText_NextFoe[] = _("Next: a new challenger");
static const u8 sText_PickNew[] = _("Choose a new teammate");
static const u8 sText_Replace[] = _("Who leaves the team?");
static const u8 sText_Streak[] = _("Best streak");
static const u8 sText_Easy[] = _("Easy");
static const u8 sText_Normal[] = _("Normal");
static const u8 sText_Hard[] = _("Hard");
static const u8 sText_StatLbl[2][20] = {_("HP  ATK  DEF"), _("SPA  SPD  SPE")};

enum { FS_MAIN, FS_SETUP, FS_DRAFT, FS_RECORDS };
enum { BK_MAIN, BK_OFFER, BK_REPL };

static u16 FrBst(u16 sp) { return BaseTotal(sp); }

static bool8 FrSpeciesOk(u16 sp)
{
    u16 dex;
    if (sp == SPECIES_NONE || sp >= NUM_SPECIES || gSpeciesInfo[sp].baseHP == 0)
        return FALSE;
    dex = SpeciesToNationalPokedexNum(sp);
    if (dex == 0 || dex > NATIONAL_DEX_COUNT || IsLegendaryDex(dex))
        return FALSE;
    if (gEvolutionTable[sp][0].targetSpecies != SPECIES_NONE)
        return FALSE;
    if (sp == SPECIES_DITTO || sp == SPECIES_UNOWN || sp == SPECIES_SHEDINJA || sp == SPECIES_SMEARGLE)
        return FALSE;
    return TRUE;
}

static u16 FrPickSpecies(u16 lo, u16 hi, const u16 *ex, u8 nEx, bool8 distinctType, u8 avoidType)
{
    u16 tries;
    u8 k;
    for (tries = 0; tries < 600; tries++)
    {
        u16 sp = 1 + Random() % (NUM_SPECIES - 1), bst;
        if (!FrSpeciesOk(sp))
            continue;
        bst = FrBst(sp);
        if (bst < lo || bst > hi)
            continue;
        for (k = 0; k < nEx; k++)
            if (ex[k] == sp)
                break;
        if (k < nEx)
            continue;
        if (distinctType && tries < 300 && (gSpeciesInfo[sp].types[0] == avoidType || gSpeciesInfo[sp].types[1] == avoidType))
            continue;
        return sp;
    }
    return SPECIES_SNORLAX;
}

// offers of balanced mons: different species, mostly different types
static void FrMakeOffers(u8 n, u16 lo, u16 hi)
{
    u16 ex[10];
    u8 i, nEx = 0;
    for (i = 0; i < sRun.nPick && nEx < 6; i++)
        ex[nEx++] = sRun.pick[i];
    sRun.nStarters = 0;
    for (i = 0; i < n; i++)
    {
        u16 sp = FrPickSpecies(lo, hi, ex, nEx, i > 0, i > 0 ? gSpeciesInfo[sRun.starterSpecies[i - 1]].types[0] : TYPE_NONE);
        sRun.starterSpecies[sRun.nStarters++] = sp;
        ex[nEx++] = sp;
    }
}

static void FrTypeString(u16 sp)
{
    StringCopy(gStringVar1, gTypeNames[gSpeciesInfo[sp].types[0]]);
    if (gSpeciesInfo[sp].types[1] != gSpeciesInfo[sp].types[0])
    {
        StringAppend(gStringVar1, sText_Slash);
        StringAppend(gStringVar1, gTypeNames[gSpeciesInfo[sp].types[1]]);
    }
}

static void FrDrawOfferRows(u8 n, u8 y0)
{
    u8 i;
    for (i = 0; i < n; i++)
    {
        u8 name[POKEMON_NAME_LENGTH + 1];
        u16 sp = sRun.starterSpecies[i];
        GetSpeciesName(name, sp);
        Rg_Text(16, y0 + i * 16, sCursor == i ? sColWhite : sColGray, name);
        FrTypeString(sp);
        Rg_TextS(100, y0 + 2 + i * 16, sColBlue, gStringVar1);
    }
    Rg_Arrow(y0 + sCursor * 16);
    {
        const struct SpeciesInfo *s = &gSpeciesInfo[sRun.starterSpecies[sCursor]];
        u8 b[6];
        u8 y = y0 + n * 16 + 4;
        static const u8 sL[6][4] = {_("HP"), _("ATK"), _("DEF"), _("SPA"), _("SPD"), _("SPE")};
        u16 v[6] = {s->baseHP, s->baseAttack, s->baseDefense, s->baseSpAttack, s->baseSpDefense, s->baseSpeed};
        u8 k;
        for (k = 0; k < 6; k++)
        {
            u8 x = 14 + (k % 3) * 52, yy = y + (k / 3) * 11;
            Rg_TextS(x, yy, sColGreen, sL[k]);
            Rg_Number(b, v[k], 3);
            Rg_TextS(x + 22, yy, sColWhite, b);
        }
        Rg_TextS(176, y, sColGreen, sText_BstLbl);
        Rg_Number(b, FrBst(sRun.starterSpecies[sCursor]), 3);
        Rg_TextS(206, y, sColGold, b);
    }
}

static void DrawFrMain(void)
{
    u8 buf[8];
    Rg_Clear();
    Rg_Text(12, 6, sColGold, sText_Frontier);
    Rg_Text(16, 34, sCursor == 0 ? sColWhite : sColGray, sText_StartRun);
    Rg_Text(16, 50, sCursor == 1 ? sColWhite : sColGray, sText_Records);
    Rg_Text(16, 66, sCursor == 2 ? sColWhite : sColGray, sText_Back);
    Rg_Arrow(34 + sCursor * 16);
    Rg_TextS(12, 100, sColBlue, sText_Lv50);
    Rg_TextS(12, 122, sColGreen, sText_Runs);
    Rg_Number(buf, sMeta.runs, 4);
    Rg_TextS(84, 122, sColWhite, buf);
    Rg_TextS(12, 135, sColGreen, sText_Wins);
    Rg_Number(buf, sMeta.victories, 4);
    Rg_TextS(84, 135, sColWhite, buf);
    Rg_Present();
}

static void DrawFrSetup(void)
{
    u8 i;
    const u8 *val[4];
    static const u8 sLeft[] = {CHAR_LEFT_ARROW, EOS};
    static const u8 sRight[] = {CHAR_RIGHT_ARROW, EOS};
    val[0] = sSizeName[sRun.fSize];
    val[1] = sDiffName[sRun.fDiff];
    val[2] = sSpeedName[sRun.fSpeed];
    val[3] = sFacName[sRun.fFac];
    Rg_Clear();
    Rg_Text(12, 4, sColGold, sText_Frontier);
    {
        const u8 *lbl[4] = {sText_PartySize, sText_Difficulty, sText_Speed, sText_Facility};
        for (i = 0; i < 4; i++)
        {
            const u8 *col = sCursor == i ? sColWhite : sColGray;
            Rg_Text(16, 26 + i * 18, col, lbl[i]);
            if (sCursor == i)
                Rg_Text(112, 26 + i * 18, sColGold, sLeft);
            Rg_Text(124, 26 + i * 18, col, val[i]);
            if (sCursor == i)
                Rg_Text(222, 26 + i * 18, sColGold, sRight);
        }
    }
    Rg_Text(16, 26 + 4 * 18, sCursor == 4 ? sColWhite : sColGray, sText_Start);
    Rg_Arrow(26 + sCursor * 18);
    Rg_TextS(12, 118, sColBlue, sDiffDesc[sRun.fDiff]);
    Rg_TextS(12, 130, sColBlue, sText_Lv50);
    Rg_TextS(12, 146, sColGray, sText_Hint);
    Rg_Present();
}

static void DrawFrDraft(void)
{
    u8 b[4];
    Rg_Clear();
    Rg_Text(12, 4, sColGold, sText_PickSlot);
    Rg_Number(b, sRun.nPick + 1, 1);
    Rg_TextS(180, 8, sColWhite, b);
    Rg_TextS(188, 8, sColGray, sText_Of);
    Rg_Number(b, sRun.fSize ? 6 : 3, 1);
    Rg_TextS(208, 8, sColWhite, b);
    FrDrawOfferRows(sRun.nStarters, 24);
    Rg_TextS(12, 146, sColGray, sText_Hint);
    Rg_Present();
}

static void DrawFrRecords(void)
{
    u8 buf[6];
    Rg_Clear();
    Rg_Text(12, 6, sColGold, sText_Records);
    Rg_TextS(16, 30, sColGreen, sText_Streak);
    Rg_Text(16, 46, sColWhite, sText_Easy);
    Rg_Number(buf, sMeta.up[0], 2); Rg_Text(110, 46, sColGold, buf);
    Rg_Text(16, 62, sColWhite, sText_Normal);
    Rg_Number(buf, sMeta.up[1], 2); Rg_Text(110, 62, sColGold, buf);
    Rg_Text(16, 78, sColWhite, sText_Hard);
    Rg_Number(buf, sMeta.up[2], 2); Rg_Text(110, 78, sColGold, buf);
    Rg_TextS(12, 108, sColGreen, sText_Runs);
    Rg_Number(buf, sMeta.runs, 4); Rg_TextS(84, 108, sColWhite, buf);
    Rg_TextS(12, 121, sColGreen, sText_Wins);
    Rg_Number(buf, sMeta.victories, 4); Rg_TextS(84, 121, sColWhite, buf);
    Rg_TextS(12, 146, sColGray, sText_Hint);
    Rg_Present();
}

static void CB2_FrMenu(void);

static void FrStartRun(void)
{
    extern void CB2_NewGame(void);
    sSavedName[0] = EOS;
    StringCopy(sSavedName, gSaveBlock2Ptr->playerName);
    sRun.state = RS_INIT;
    sRun.floor = 1;
    gRogueMode = RG_FRONTIER;
    gRogueSpeedPct = sSpeedPct[sRun.fSpeed];
    sMeta.runs++;
    Meta_Save();
    SetMainCallback2(CB2_NewGame);
}

static void FrBeginDraft(void)
{
    sRun.nPick = 0;
    FrMakeOffers(4, FR_BST_LO, FR_BST_HI);
    sScreen = FS_DRAFT;
    sCursor = 0;
    DrawFrDraft();
}

static void CB2_FrMenu(void)
{
    u8 max = 2;
    AnimateSprites();
    BuildOamBuffer();
    RunTextPrinters();
    UpdatePaletteFade();
    if (sScreen == FS_MAIN)
    {
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawFrMain(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor < max) { sCursor++; PlaySE(SE_SELECT); DrawFrMain(); }
        else if (JOY_NEW(B_BUTTON)) { PlaySE(SE_SELECT); SetMainCallback2(CB2_InitMainMenu); }
        else if (JOY_NEW(A_BUTTON))
        {
            PlaySE(SE_SELECT);
            if (sCursor == 0) { sScreen = FS_SETUP; sCursor = 0; DrawFrSetup(); }
            else if (sCursor == 1) { sScreen = FS_RECORDS; DrawFrRecords(); }
            else SetMainCallback2(CB2_InitMainMenu);
        }
    }
    else if (sScreen == FS_RECORDS)
    {
        if (JOY_NEW(A_BUTTON | B_BUTTON)) { PlaySE(SE_SELECT); sScreen = FS_MAIN; sCursor = 1; DrawFrMain(); }
    }
    else if (sScreen == FS_SETUP)
    {
        s8 d = 0;
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawFrSetup(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor < 4) { sCursor++; PlaySE(SE_SELECT); DrawFrSetup(); }
        else if (JOY_NEW(B_BUTTON)) { PlaySE(SE_SELECT); sScreen = FS_MAIN; sCursor = 0; DrawFrMain(); }
        else if (JOY_NEW(DPAD_LEFT)) d = -1;
        else if (JOY_NEW(DPAD_RIGHT)) d = 1;
        else if (JOY_NEW(A_BUTTON))
        {
            PlaySE(SE_SELECT);
            if (sCursor == 4) { FrBeginDraft(); return; }
            d = 1;
        }
        if (d != 0 && sCursor < 4)
        {
            s8 v;
            PlaySE(SE_SELECT);
            if (sCursor == 0) { sRun.fSize ^= 1; }
            else if (sCursor == 1) { v = sRun.fDiff + d; sRun.fDiff = v < 0 ? 2 : (v > 2 ? 0 : v); }
            else if (sCursor == 2) { v = sRun.fSpeed + d; sRun.fSpeed = v < 0 ? 2 : (v > 2 ? 0 : v); }
            else { v = sRun.fFac + d; sRun.fFac = v < 0 ? 4 : (v > 4 ? 0 : v); }
            DrawFrSetup();
        }
    }
    else if (sScreen == FS_DRAFT)
    {
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawFrDraft(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor + 1 < sRun.nStarters) { sCursor++; PlaySE(SE_SELECT); DrawFrDraft(); }
        else if (JOY_NEW(B_BUTTON))
        {
            PlaySE(SE_SELECT);
            if (sRun.nPick > 0) { sRun.nPick--; }
            else { sScreen = FS_SETUP; sCursor = 4; DrawFrSetup(); return; }
            FrMakeOffers(4, FR_BST_LO, FR_BST_HI);
            sCursor = 0;
            DrawFrDraft();
        }
        else if (JOY_NEW(A_BUTTON))
        {
            PlaySE(SE_SELECT);
            sRun.pick[sRun.nPick++] = sRun.starterSpecies[sCursor];
            if (sRun.nPick >= (sRun.fSize ? 6 : 3))
            {
                FrStartRun();
                return;
            }
            FrMakeOffers(4, FR_BST_LO, FR_BST_HI);
            sCursor = 0;
            DrawFrDraft();
        }
    }
}

void CB2_RtRogueStart(void)
{
    Meta_Load();
    Rg_Open();
    memset(&sRun, 0, sizeof(sRun));
    sRun.fSize = 1;
    sRun.fDiff = 1;
    sRun.fSpeed = 1;
    sScreen = FS_MAIN;
    sCursor = 0;
    DrawFrMain();
    SetMainCallback2(CB2_FrMenu);
}

// ---------------------------------------------------------------------------
// building the teams
// ---------------------------------------------------------------------------
// the four strongest, varied attacks the species learns by level 50
static void FrSetBestMoves(struct Pokemon *mon)
{
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    u16 list[48];
    u16 score[48];
    u8 n = 0, i, k, picked = 0;
    u16 chosen[MAX_MON_MOVES] = {0};
    u8 types[MAX_MON_MOVES] = {0};
    for (i = 0; gLevelUpLearnsets[species][i] != LEVEL_UP_END && n < 48; i++)
    {
        u16 e = gLevelUpLearnsets[species][i];
        u8 lv = (e & LEVEL_UP_MOVE_LV) >> 9;
        u16 mv = e & LEVEL_UP_MOVE_ID, pw, ef;
        if (lv > FR_LEVEL)
            break;
        for (k = 0; k < n; k++)
            if (list[k] == mv)
                break;
        if (k < n)
            continue;
        ef = gBattleMoves[mv].effect;
        pw = gBattleMoves[mv].power;
        if (pw <= 1 && ef != EFFECT_RETURN)
            continue;
        if (ef == EFFECT_EXPLOSION || ef == EFFECT_DREAM_EATER || ef == EFFECT_BIDE || ef == EFFECT_OHKO || ef == EFFECT_COUNTER
         || ef == EFFECT_MIRROR_COAT || ef == EFFECT_FOCUS_PUNCH || ef == EFFECT_SNORE || ef == EFFECT_SPIT_UP || ef == EFFECT_ENDEAVOR
         || ef == EFFECT_FALSE_SWIPE || ef == EFFECT_LEVEL_DAMAGE || ef == EFFECT_PSYWAVE || ef == EFFECT_FLAIL || ef == EFFECT_FAKE_OUT
         || ef == EFFECT_SUPER_FANG || ef == EFFECT_NIGHTMARE)
            continue;
        if (ef == EFFECT_RETURN)
            pw = 90;
        pw = pw * (gBattleMoves[mv].accuracy ? gBattleMoves[mv].accuracy : 100) / 100;
        if (gBattleMoves[mv].type == gSpeciesInfo[species].types[0] || gBattleMoves[mv].type == gSpeciesInfo[species].types[1])
            pw = pw * 3 / 2;
        if (ef == EFFECT_SOLAR_BEAM || ef == EFFECT_SKY_ATTACK || ef == EFFECT_RAZOR_WIND || ef == EFFECT_SKULL_BASH || ef == EFFECT_SEMI_INVULNERABLE)
            pw = pw / 2;
        if (ef == EFFECT_RECHARGE || ef == EFFECT_OVERHEAT || ef == EFFECT_SUPERPOWER)
            pw = pw * 3 / 4;
        list[n] = mv;
        score[n] = pw;
        n++;
    }
    if (n < 2)
    {
        RelearnMoves(mon);
        return;
    }
    for (picked = 0; picked < MAX_MON_MOVES; picked++)
    {
        s32 best = -1, bi = -1;
        for (i = 0; i < n; i++)
        {
            s32 sc = score[i];
            if (sc == 0)
                continue;
            for (k = 0; k < picked; k++)
                if (types[k] == gBattleMoves[list[i]].type)
                    sc = sc * 6 / 10;
            if (sc > best) { best = sc; bi = i; }
        }
        if (bi < 0)
            break;
        chosen[picked] = list[bi];
        types[picked] = gBattleMoves[list[bi]].type;
        score[bi] = 0;
    }
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        u8 pp = chosen[i] ? gBattleMoves[chosen[i]].pp : 0;
        SetMonMoveSlot(mon, chosen[i], i);
        SetMonData(mon, MON_DATA_PP1 + i, &pp);
    }
}

static void FrFinishMon(struct Pokemon *mon, u16 sp, u8 iv, bool8 item, bool8 build)
{
    CreateMon(mon, sp, FR_LEVEL, iv, FALSE, 0, OT_ID_PLAYER_ID, 0);
    FrSetBestMoves(mon);
    if (item)
    {
        u16 it = sItemPool[Random() % ARRAY_COUNT(sItemPool)];
        SetMonData(mon, MON_DATA_HELD_ITEM, &it);
    }
    if (build)
    {
        RtBuild_Set(mon, 0, (1 + Random() % (RUNE_COUNT - 1)) | ((1 + Random() % (SPELL_COUNT - 1)) << 4));
        RtBuild_Set(mon, 1, (1 + Random() % (RUNE_COUNT - 1)) | ((1 + Random() % (SPELL_COUNT - 1)) << 4));
        RtBuild_SetSel(mon, 0);
    }
}

static void FrBuildPlayerParty(void)
{
    u8 i, n = sRun.fSize ? 6 : 3;
    ZeroPlayerPartyMons();
    for (i = 0; i < n; i++)
    {
        struct Pokemon mon;
        FrFinishMon(&mon, sRun.pick[i], 31, TRUE, FALSE);
        GiveMonToPlayer(&mon);
    }
    for (i = 0; i < n; i++)
    {
        RtBuild_Set(&gPlayerParty[i], 0, (1 + Random() % (RUNE_COUNT - 1)) | ((1 + Random() % (SPELL_COUNT - 1)) << 4));
        RtBuild_Set(&gPlayerParty[i], 1, (1 + Random() % (RUNE_COUNT - 1)) | ((1 + Random() % (SPELL_COUNT - 1)) << 4));
        RtBuild_SetSel(&gPlayerParty[i], 0);
    }
}

static bool8 FrBossRound(u8 round)
{
    return round % FR_SET_LEN == 0;
}

static void FrBuildEnemyParty(u8 round)
{
    u8 n = sRun.fSize ? 6 : 3, i, d = sRun.fDiff;
    u16 lo, hi, ex[PARTY_SIZE];
    static const u8 sIv[3] = {10, 20, 31};
    ZeroEnemyPartyMons();
    if (d == 0) { lo = 380 + round * 5; hi = 455 + round * 6; }
    else if (d == 1) { lo = 430 + round * 4; hi = 505 + round * 5; }
    else { lo = 470 + round * 4; hi = 540 + round * 3; }
    if (FrBossRound(round)) { lo += 30; hi += 40; }
    if (hi > 600) hi = 600;
    if (lo > hi - 20) lo = hi - 20;
    gRogueBoss = FrBossRound(round);
    for (i = 0; i < n; i++)
    {
        ex[i] = FrPickSpecies(lo, hi, ex, i, FALSE, TYPE_NONE);
        FrFinishMon(&gEnemyParty[i], ex[i], sIv[d], d == 2 || (d == 1 && Random() % 2), d > 0);
        gRogueChalBuild[i] = (d > 0) ? ((1 + Random() % (RUNE_COUNT - 1)) | ((1 + Random() % (SPELL_COUNT - 1)) << 4)) : 0;
    }
}

u8 RtRogue_FrontierAiSkill(void)
{
    u8 r = sRun.floor ? sRun.floor : 1, d = sRun.fDiff;
    u8 sk = d == 0 ? 2 + r / 4 : (d == 1 ? 5 + r / 7 : 8 + r / 12);
    return sk > 9 ? 9 : sk;
}

// ---------------------------------------------------------------------------
// between the battles
// ---------------------------------------------------------------------------
static void CB2_FrBreak(void);
static void FrOpenBreak(void);

static void FrBackToField(void)
{
    sRun.state = RS_WAIT;
    sRun.timer = 30;
    SetMainCallback2(CB2_ReturnToField);
    gFieldCallback = FieldCB_ReturnToFieldNoScriptCheckMusic;
}

static void FrAfterBuilds(void)
{
    FrOpenBreak();
}

static u8 FrBreakRows(u8 *rows)
{
    u8 n = 0;
    rows[n++] = 0; // continue
    if (FrBossRound(sRun.floor - 1) && !(sRun.sub & 1) && sRun.floor > 1)
        rows[n++] = 1; // recruit (set clear only)
    rows[n++] = 2; // builds
    rows[n++] = 3; // retire
    return n;
}

static void DrawFrBreak(void)
{
    u8 rows[4], n = FrBreakRows(rows), i, b[4];
    static const u8 *const sNames[4] = {sText_Continue, sText_Recruit, sText_Builds, sText_Retire};
    Rg_Clear();
    if (sScreen == BK_MAIN)
    {
        bool8 set = FrBossRound(sRun.floor - 1);
        Rg_Text(12, 6, sColGold, set ? sText_SetCleared : sText_Round);
        if (!set)
        {
            Rg_Number(b, sRun.floor - 1, 2);
            Rg_Text(70, 6, sColWhite, b);
            Rg_Text(92, 6, sColGreen, sText_Cleared);
        }
        for (i = 0; i < n; i++)
            Rg_Text(16, 34 + i * 16, sCursor == i ? sColWhite : sColGray, sNames[rows[i]]);
        Rg_Arrow(34 + sCursor * 16);
        Rg_TextS(12, 112, sColBlue, FrBossRound(sRun.floor) ? sText_NextBoss : sText_NextFoe);
        Rg_TextS(12, 124, sColGray, sText_Round);
        Rg_Number(b, sRun.floor, 2);
        Rg_TextS(46, 124, sColWhite, b);
        Rg_TextS(60, 124, sColGray, sText_Of);
        Rg_Number(b, FR_MAX_ROUND, 2);
        Rg_TextS(80, 124, sColWhite, b);
    }
    else if (sScreen == BK_OFFER)
    {
        Rg_Text(12, 4, sColGold, sText_PickNew);
        FrDrawOfferRows(3, 24);
    }
    else
    {
        u8 name[POKEMON_NAME_LENGTH + 1];
        Rg_Text(12, 4, sColGold, sText_Replace);
        for (i = 0; i < PartyCount(); i++)
        {
            BeMonName(i, name);
            Rg_Text(16, 24 + i * 16, sCursor == i ? sColWhite : sColGray, name);
        }
        Rg_Arrow(24 + sCursor * 16);
    }
    Rg_Present();
}

static void FrOpenBreak(void)
{
    Rg_Open();
    sScreen = BK_MAIN;
    sCursor = 0;
    DrawFrBreak();
    SetMainCallback2(CB2_FrBreak);
}

static void FrEndRun(bool8 won);

static void CB2_FrBreak(void)
{
    u8 rows[4], n = FrBreakRows(rows);
    AnimateSprites();
    BuildOamBuffer();
    RunTextPrinters();
    UpdatePaletteFade();
    if (sScreen == BK_MAIN)
    {
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawFrBreak(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor + 1 < n) { sCursor++; PlaySE(SE_SELECT); DrawFrBreak(); }
        else if (JOY_NEW(A_BUTTON))
        {
            PlaySE(SE_SELECT);
            switch (rows[sCursor])
            {
            case 0: FrBackToField(); break;
            case 1:
                sRun.nPick = 0;
                {
                    u16 ex[PARTY_SIZE];
                    u8 i;
                    for (i = 0; i < PartyCount(); i++)
                        ex[i] = GetMonData(&gPlayerParty[i], MON_DATA_SPECIES);
                    for (i = 0; i < PartyCount(); i++)
                        sRun.pick[i] = ex[i];
                    sRun.nPick = PartyCount();
                    if (sRun.nPick > 5) sRun.nPick = 5;
                }
                FrMakeOffers(3, FR_BST_LO + 20, FR_BST_HI + 30);
                sScreen = BK_OFFER; sCursor = 0; DrawFrBreak();
                break;
            case 2:
                gMain.savedCallback = FrAfterBuilds;
                gMain.state = 0;
                SetMainCallback2(CB2_RtBuildEditor);
                break;
            case 3: FrEndRun(FALSE); break;
            }
        }
    }
    else if (sScreen == BK_OFFER)
    {
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawFrBreak(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor + 1 < 3) { sCursor++; PlaySE(SE_SELECT); DrawFrBreak(); }
        else if (JOY_NEW(B_BUTTON)) { PlaySE(SE_SELECT); sScreen = BK_MAIN; sCursor = 0; DrawFrBreak(); }
        else if (JOY_NEW(A_BUTTON))
        {
            sRun.offers[0] = sCursor;
            PlaySE(SE_SELECT);
            sScreen = BK_REPL; sCursor = 0; DrawFrBreak();
        }
    }
    else
    {
        u8 pc = PartyCount();
        if (JOY_NEW(DPAD_UP) && sCursor > 0) { sCursor--; PlaySE(SE_SELECT); DrawFrBreak(); }
        else if (JOY_NEW(DPAD_DOWN) && sCursor + 1 < pc) { sCursor++; PlaySE(SE_SELECT); DrawFrBreak(); }
        else if (JOY_NEW(B_BUTTON)) { PlaySE(SE_SELECT); sScreen = BK_OFFER; sCursor = sRun.offers[0]; DrawFrBreak(); }
        else if (JOY_NEW(A_BUTTON))
        {
            struct Pokemon mon;
            PlaySE(SE_SELECT);
            FrFinishMon(&mon, sRun.starterSpecies[sRun.offers[0]], 31, TRUE, FALSE);
            RtBuild_Set(&mon, 0, (1 + Random() % (RUNE_COUNT - 1)) | ((1 + Random() % (SPELL_COUNT - 1)) << 4));
            RtBuild_Set(&mon, 1, (1 + Random() % (RUNE_COUNT - 1)) | ((1 + Random() % (SPELL_COUNT - 1)) << 4));
            RtBuild_SetSel(&mon, 0);
            gPlayerParty[sCursor] = mon;
            sRun.sub |= 1;
            sScreen = BK_MAIN; sCursor = 0; DrawFrBreak();
        }
    }
}

static void FrEndRun(bool8 won)
{
    u8 buf[6], cleared = won ? FR_MAX_ROUND : (sRun.floor > 0 ? sRun.floor - 1 : 0);
    static const u8 sText_Streak2[] = _("Rounds cleared:");
    Meta_Load();
    if (cleared > sMeta.up[sRun.fDiff])
        sMeta.up[sRun.fDiff] = cleared;
    if (cleared > sMeta.best)
        sMeta.best = cleared;
    if (won)
        sMeta.victories++;
    Meta_Save();
    sRun.state = RS_DONE;
    Rg_Open();
    Rg_Clear();
    Rg_Text(64, 24, won ? sColGold : sColRed, won ? sText_RunWon : sText_RunOver);
    Rg_Text(40, 60, sColWhite, sText_Streak2);
    Rg_Number(buf, cleared, 2);
    Rg_Text(170, 60, sColGreen, buf);
    Rg_Text(40, 78, sColWhite, sText_Difficulty);
    Rg_Text(170, 78, sColGold, sDiffName[sRun.fDiff]);
    Rg_Text(84, 126, sColGray, sText_PressA);
    Rg_Present();
    PlayBGM(won ? MUS_LEVEL_UP : MUS_DUMMY);
    SetMainCallback2(CB2_RogueOver);
}

// called by the field hook
static bool8 FrontierFieldInput(void)
{
    switch (sRun.state)
    {
    case RS_INIT:
        FrBuildPlayerParty();
        StringCopy(gSaveBlock2Ptr->playerName, sSavedName);
        FlagSet(FLAG_SYS_POKEMON_GET);
        FlagSet(FLAG_SYS_B_DASH);
        RtSave_Get();
        sRun.state = RS_WARP;
        return FALSE;
    case RS_WARP:
        SetWarpDestination(sFac[sRun.fFac].group, sFac[sRun.fFac].num, WARP_ID_NONE, sFac[sRun.fFac].x, sFac[sRun.fFac].y);
        DoWarp();
        sRun.state = RS_WAIT;
        sRun.timer = 0;
        return FALSE;
    case RS_WAIT:
        if (++sRun.timer < 90 || gPaletteFade.active)
            return FALSE;
        {
            extern void RtDebug_StartTrainerBattle(u16 trainerId);
            FrBuildEnemyParty(sRun.floor);
            gRogueBattleEntered = FALSE;
            sRun.state = RS_FIGHT;
            RtDebug_StartTrainerBattle(TRAINER_ALLEN);
        }
        return FALSE;
    case RS_FIGHT:
        if (!gRogueBattleEntered)
            return FALSE;
        if (gBattleOutcome == B_OUTCOME_WON)
        {
            HealPlayerParty();
            sRun.floor++;
            if (FrBossRound(sRun.floor - 1))
                sRun.sub &= ~1;
            if (sRun.floor > FR_MAX_ROUND)
            {
                FrEndRun(TRUE);
                return FALSE;
            }
            sRun.state = RS_DONE;
            FrOpenBreak();
            return FALSE;
        }
        FrEndRun(FALSE);
        return FALSE;
    }
    return FALSE;
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
    if (gRogueMode == RG_FRONTIER)
        return FrontierFieldInput();
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
    if (gRogueMode == RG_FRONTIER)
        return RtRogue_FrontierAiSkill();
    if (gRogueMode == RG_CHALLENGE)
        return 8;
    sk = 3 + sRun.floor / 3;
    return sk > 9 ? 9 : sk;
}
