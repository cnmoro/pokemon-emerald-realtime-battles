// Real-time arena battle: main flow, setup, rewards and exit.
#include "global.h"
#include "rt_internal.h"
#include "rt_vfx.h"
#include "battle.h"
#include "battle_main.h"
#include "battle_setup.h"
#include "battle_tower.h"
#include "bg.h"
#include "data.h"
#include "decompress.h"
#include "event_data.h"
#include "evolution_scene.h"
#include "fieldmap.h"
#include "frontier_util.h"
#include "gpu_regs.h"
#include "international_string_util.h"
#include "item.h"
#include "main.h"
#include "malloc.h"
#include "menu.h"
#include "money.h"
#include "m4a.h"
#include "overworld.h"
#include "palette.h"
#include "party_menu.h"
#include "pokedex.h"
#include "pokemon.h"
#include "random.h"
#include "roamer.h"
#include "safari_zone.h"
#include "scanline_effect.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "task.h"
#include "text.h"
#include "tileset_anims.h"
#include "trainer_hill.h"
#include "tv.h"
#include "window.h"
#include "pokeball.h"
#include "battle_anim.h"
#include "naming_screen.h"
#include "constants/frontier_util.h"
#include "constants/battle.h"
#include "constants/abilities.h"
#include "constants/hold_effects.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/trainers.h"
#include "constants/battle_frontier.h"

EWRAM_DATA struct RtBattle *gRt = NULL;

void RtBattle_PrepareParties(void); // battle_main.c
extern u8 gNumSafariBalls;

static void CB2_RtSetup(void);
static void CB2_RtMain(void);
static void VBlankCB_Rt(void);
static void Rt_UpdateIntro(void);
static void Rt_UpdateFight(void);
bool8 RtLegends_IsLegendBattle(void);
bool8 RtLegends_IsRoamingBattle(void);
void RtLegends_AfterBattle(u8 outcome);
void RtProf(u8 n);
u16 RtFrameLinesUsed(void);
static void Rt_UpdateOutro(void);
static void Rt_Exit(void);
static void CB2_RtEvolutionLoop(void);
static void Rt_CheckBattleOver(void);
static void Rt_HandlePlayerInput(struct RtActor *a);
static void Rt_UpdateCamera(void);
static void Rt_SetupPlayerSide(void);
static void Rt_SetupEnemySide(void);
extern const u8 gRtDebugEnabled;
extern u16 gRtDebugTestMove;
extern u8 gRtDebugCommand;

static void Rt_DebugApplyTestMove(struct RtActor *a)
{
    s32 i;
    if (!gRtDebugEnabled || !gRtDebugTestMove)
        return;
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        a->moves[i] = i == 0 ? gRtDebugTestMove : MOVE_NONE;
        a->cdMax[i] = i == 0 ? 90 : 0;
        a->cd[i] = 0;
    }
}
static void Rt_StartOutro(u8 outcome);

const u8 gRtText_Empty[] = _("");
static const u8 sText_WildAppeared[] = _("Wild {STR_VAR_1} appeared!");
static const u8 sText_WildPairAppeared[] = _("Wild {STR_VAR_1} and {STR_VAR_2} appeared!");
static const u8 sText_TrainerWants[] = _("{STR_VAR_1} wants to battle!");
static const u8 sText_TrainersWant[] = _("{STR_VAR_1} and {STR_VAR_2} want to battle!");
static const u8 sText_Go[] = _("Go! {STR_VAR_1}!");
static const u8 sText_SentOut[] = _("{STR_VAR_1} sent out {STR_VAR_2}!");
static const u8 sText_Fainted[] = _("{STR_VAR_1} fainted!");
static const u8 sText_EnemyFainted[] = _("Foe {STR_VAR_1} fainted!");
static const u8 sText_GainedExp[] = _("{STR_VAR_1} gained {STR_VAR_2} EXP!");
static const u8 sText_GrewLevel[] = _("{STR_VAR_1} grew to Lv. {STR_VAR_2}!");
static const u8 sText_LearnedMove[] = _("{STR_VAR_1} learned {STR_VAR_2}!");
static const u8 sText_Defeated[] = _("You defeated {STR_VAR_1}!");
static const u8 sText_GotMoney[] = _("You got ¥{STR_VAR_1} for winning!");
static const u8 sText_PickedUpMoney[] = _("You picked up ¥{STR_VAR_1}!");
static const u8 sText_OutOfMons[] = _("{PLAYER} is out of usable POKéMON!\n{PLAYER} whited out!");
static const u8 sText_GotAway[] = _("Got away safely!");
static const u8 sText_CantEscape[] = _("Can't escape!");
static const u8 sText_NoRunTrainer[] = _("No! There's no running\nfrom a TRAINER battle!");
static const u8 sText_Caught[] = _("Gotcha! {STR_VAR_1} was caught!");
static const u8 sText_SentToPC[] = _("{STR_VAR_1} was transferred\nto someone's PC.");
static const u8 sText_WildFled[] = _("Wild {STR_VAR_1} fled!");
static const u8 sText_NoSafariBalls[] = _("Announcer: You're out of\nSAFARI BALLS! Game over!");
static const u8 sText_Teleported[] = _("{STR_VAR_1} fled the battle!");
static const u8 sText_GiveNickname[] = _("Give a nickname to the\ncaptured {STR_VAR_1}?");
static EWRAM_DATA u8 sNickBuf[POKEMON_NAME_LENGTH + 2] = {0};
static EWRAM_DATA struct Pokemon *sNickMon = NULL;
static void CB2_RtAfterNickname(void);

u16 Rt_Random(u16 n)
{
    if (n == 0)
        return 0;
    return Random() % n;
}

bool8 RtBattle_IsSupported(void)
{
    if (gBattleTypeFlags & (BATTLE_TYPE_LINK | BATTLE_TYPE_RECORDED | BATTLE_TYPE_RECORDED_LINK
                          | BATTLE_TYPE_WALLY_TUTORIAL | BATTLE_TYPE_EREADER_TRAINER | BATTLE_TYPE_RECORDED_IS_MASTER))
        return FALSE;
    if ((gBattleTypeFlags & BATTLE_TYPE_MULTI) && !(gBattleTypeFlags & BATTLE_TYPE_INGAME_PARTNER))
        return FALSE;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Utility accessors
// ---------------------------------------------------------------------------
struct RtActor *Rt_GetActor(u8 id)
{
    if (id >= RT_MAX_ACTORS)
        return NULL;
    return &gRt->actors[id];
}

struct RtActor *Rt_PlayerActor(void)
{
    return &gRt->actors[gRt->playerLead];
}

bool8 Rt_IsActorOnField(struct RtActor *a)
{
    return a->active && a->state != AST_GONE && a->state != AST_NONE && a->state != AST_CAUGHT;
}

struct RtActor *Rt_NearestFoe(struct RtActor *a)
{
    s32 i, best = 0x7FFFFFFF;
    struct RtActor *res = NULL;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *b = &gRt->actors[i];
        s32 d;
        if (!b->active || b->side == a->side || !RtActor_IsAlive(b) || !Rt_IsActorOnField(b))
            continue;
        d = RtDist(FROM_FX(b->x - a->x), FROM_FX(b->y - a->y));
        if (b->invuln)
            d += 200;
        if (d < best)
        {
            best = d;
            res = b;
        }
    }
    return res;
}

u8 Rt_CountUsableParty(u8 side)
{
    struct Pokemon *party = side == RT_SIDE_PLAYER ? gPlayerParty : gEnemyParty;
    u8 i, n = 0;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 sp = GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG && GetMonData(&party[i], MON_DATA_HP) != 0)
            n++;
    }
    return n;
}

static bool8 IsPartyIdxOnField(u8 side, u8 partyIdx)
{
    s32 i;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        if (a->active && a->side == side && a->partyIdx == partyIdx && a->state != AST_GONE)
            return TRUE;
    }
    return FALSE;
}

static bool8 IsMonUsable(struct Pokemon *mon)
{
    u16 sp = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
    return sp != SPECIES_NONE && sp != SPECIES_EGG && GetMonData(mon, MON_DATA_HP) != 0;
}

// Player party indices usable by the player himself (partner battles: 0-2 only)
static u8 PlayerPartyLimit(void)
{
    if (gBattleTypeFlags & BATTLE_TYPE_INGAME_PARTNER)
        return 3;
    return PARTY_SIZE;
}

static u8 FindNextUsable(u8 side, u8 from, u8 to)
{
    struct Pokemon *party = side == RT_SIDE_PLAYER ? gPlayerParty : gEnemyParty;
    u8 i;
    for (i = from; i < to; i++)
    {
        if (IsMonUsable(&party[i]) && !IsPartyIdxOnField(side, i))
            return i;
    }
    return PARTY_SIZE;
}

static void GetOpponentName(u8 *dst, u16 trainerId)
{
    if (trainerId == TRAINER_SECRET_BASE)
    {
        dst[0] = EOS;
        if (gBattleResources && gBattleResources->secretBase)
        {
            u8 i;
            for (i = 0; i < PLAYER_NAME_LENGTH && gBattleResources->secretBase->trainerName[i] != EOS; i++)
                dst[i] = gBattleResources->secretBase->trainerName[i];
            dst[i] = EOS;
        }
    }
    else if (gBattleTypeFlags & BATTLE_TYPE_FRONTIER)
    {
        GetFrontierTrainerName(dst, trainerId);
    }
    else if (gBattleTypeFlags & BATTLE_TYPE_TRAINER_HILL)
    {
        GetTrainerHillTrainerName(dst, trainerId);
    }
    else
    {
        u8 cls = gTrainers[trainerId].trainerClass;
        dst = StringCopy(dst, gTrainerClassNames[cls]);
        *dst++ = CHAR_SPACE;
        StringCopy(dst, gTrainers[trainerId].trainerName);
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
static const struct BgTemplate sRtBgTemplates[] =
{
    { .bg = 0, .charBaseIndex = 2, .mapBaseIndex = 31, .screenSize = 0, .paletteMode = 0, .priority = 0, .baseTile = 0 },
    { .bg = 1, .charBaseIndex = 0, .mapBaseIndex = 24, .screenSize = 1, .paletteMode = 0, .priority = 1, .baseTile = 0 },
    { .bg = 2, .charBaseIndex = 0, .mapBaseIndex = 26, .screenSize = 1, .paletteMode = 0, .priority = 2, .baseTile = 0 },
    { .bg = 3, .charBaseIndex = 0, .mapBaseIndex = 28, .screenSize = 1, .paletteMode = 0, .priority = 3, .baseTile = 0 },
};

static void CB1_RtBattle(void)
{
}

void CB2_InitRtBattle(void)
{
    gPreBattleCallback1 = gMain.callback1;
    gMain.callback1 = CB1_RtBattle;
    gRt = AllocZeroed(sizeof(struct RtBattle));
    {
        s32 i;
        for (i = 0; i < PARTY_SIZE; i++)
        {
            // empty slots get no snapshot: a POKeMON caught into them is not shown on the EXP panel
            if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) == SPECIES_NONE)
                gRt->expStart[i] = 0xFFFFFFFF;
            else
                gRt->expStart[i] = GetMonData(&gPlayerParty[i], MON_DATA_EXP);
        }
    }
    // there is always a POKe BALL in the bag (it is never used up, see RemoveItem in rt_ui.c)
    if (!(gBattleTypeFlags & BATTLE_TYPE_FRONTIER) && !CheckBagHasItem(ITEM_POKE_BALL, 1))
        AddBagItem(ITEM_POKE_BALL, 1);
    gMain.state = 0;
    SetMainCallback2(CB2_RtSetup);
}

static void Rt_ResetVideo(void)
{
    SetVBlankCallback(NULL);
    SetHBlankCallback(NULL);
    DisableInterrupts(INTR_FLAG_HBLANK); // left on by the battle transitions: an interrupt every scanline
    ScanlineEffect_Stop();
    SetGpuReg(REG_OFFSET_DISPCNT, 0);
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    SetGpuReg(REG_OFFSET_BLDALPHA, 0);
    SetGpuReg(REG_OFFSET_BLDY, 0);
    SetGpuReg(REG_OFFSET_WININ, 0);
    SetGpuReg(REG_OFFSET_WINOUT, 0);
    SetGpuReg(REG_OFFSET_MOSAIC, 0);
    DmaFill16(3, 0, (void *)VRAM, VRAM_SIZE);
    DmaFill32(3, 0, (void *)OAM, OAM_SIZE);
    DmaFill16(3, 0, (void *)(PLTT), PLTT_SIZE);
    ResetPaletteFade();
    ResetSpriteData();
    ResetTasks();
    FreeAllSpritePalettes();
}

static void CB2_RtSetup(void)
{
    switch (gMain.state)
    {
    case 0:
        Rt_ResetVideo();
        gMain.inBattle = TRUE;
        memset(&gBattleResults, 0, sizeof(gBattleResults));
        gLeveledUpInBattle = 0;
        gBattleOutcome = 0;
        gRt->isTrainer = (gBattleTypeFlags & BATTLE_TYPE_TRAINER) != 0;
        gRt->isDouble = (gBattleTypeFlags & BATTLE_TYPE_DOUBLE) != 0;
        gRt->isSafari = (gBattleTypeFlags & BATTLE_TYPE_SAFARI) != 0;
        gRt->twoOpponents = (gBattleTypeFlags & BATTLE_TYPE_TWO_OPPONENTS) != 0;
        gRt->partner = (gBattleTypeFlags & BATTLE_TYPE_INGAME_PARTNER) != 0;
        gRt->playerParty = gPlayerParty;
        gRt->enemyParty = gEnemyParty;
        gBattleEnvironment = BattleSetup_GetEnvironmentId();
        RtBattle_PrepareParties();
        RtExt_SetupObjective();
        RtArena_Build();
        gMain.state++;
        break;
    case 1:
        ResetBgsAndClearDma3BusyFlags(0);
        InitBgsFromTemplates(0, sRtBgTemplates, ARRAY_COUNT(sRtBgTemplates));
        SetBgTilemapBuffer(0, gRt->bg0Tilemap);
        gMain.state++;
        break;
    case 2:
        {
            extern void RtArena_LoadGfx(void);
            RtArena_LoadGfx();
        }
        gMain.state++;
        break;
    case 3:
        RtFx_Init();
        RtVfx_ResetGhosts();
        RtUi_Init();
        gMain.state++;
        break;
    case 4:
        Rt_SetupPlayerSide();
        Rt_SetupEnemySide();
        gMain.state++;
        break;
    case 5:
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_0 | DISPCNT_OBJ_1D_MAP | DISPCNT_OBJ_ON
                  | DISPCNT_BG0_ON | DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_BG3_ON);
        SetGpuReg(REG_OFFSET_WININ, 0);
        SetGpuReg(REG_OFFSET_WINOUT, 0);
        SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_OBJ | BLDCNT_TGT2_BD);
        SetGpuReg(REG_OFFSET_BLDALPHA, BLDALPHA_BLEND(8, 10));
        ShowBg(0);
        ShowBg(1);
        ShowBg(2);
        ShowBg(3);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB_BLACK);
        SetVBlankCallback(VBlankCB_Rt);
        gRt->state = RTS_INTRO;
        gRt->timer = 0;
        SetMainCallback2(CB2_RtMain);
        break;
    }
}

static void Rt_SetupPlayerSide(void)
{
    u8 idx0, idx1 = PARTY_SIZE;
    struct RtActor *a;
    s32 x, y;

    gRt->playerLead = 0;
    if (gRt->isSafari)
    {
        gRt->safariBalls = gNumSafariBalls;
        return;
    }
    idx0 = FindNextUsable(RT_SIDE_PLAYER, 0, PlayerPartyLimit());
    if (idx0 == PARTY_SIZE)
        idx0 = 0;
    a = &gRt->actors[0];
    RtActor_InitFromMon(a, &gPlayerParty[idx0], RT_SIDE_PLAYER, idx0, 0);
    RtArena_FindSpawn(RT_SIDE_PLAYER, 0, &x, &y);
    a->x = x; a->y = y;
    a->facing = 4;
    RtActor_SendOut(a, idx0, FALSE);
    Rt_DebugApplyTestMove(a);

    if (gRt->isDouble)
    {
        if (gRt->partner)
            idx1 = FindNextUsable(RT_SIDE_PLAYER, 3, PARTY_SIZE);
        else
            idx1 = FindNextUsable(RT_SIDE_PLAYER, 0, PARTY_SIZE);
        if (idx1 != PARTY_SIZE)
        {
            a = &gRt->actors[1];
            RtActor_InitFromMon(a, &gPlayerParty[idx1], RT_SIDE_PLAYER, idx1, 1);
            RtArena_FindSpawn(RT_SIDE_PLAYER, 1, &x, &y);
            a->x = x; a->y = y;
            a->facing = 4;
            RtActor_SendOut(a, idx1, FALSE);
            RtAi_Init(a);
            a->aiSkill = 6;
        }
    }
}

static void Rt_SetupEnemySide(void)
{
    struct RtActor *a;
    s32 x, y;
    u8 idx0, idx1 = PARTY_SIZE;

    idx0 = FindNextUsable(RT_SIDE_ENEMY, 0, PARTY_SIZE);
    if (idx0 == PARTY_SIZE)
        idx0 = 0;
    a = &gRt->actors[2];
    RtActor_InitFromMon(a, &gEnemyParty[idx0], RT_SIDE_ENEMY, idx0, 2);
    RtArena_FindSpawn(RT_SIDE_ENEMY, 0, &x, &y);
    a->x = x; a->y = y;
    a->facing = 0;
    RtActor_SendOut(a, idx0, !gRt->isTrainer);
    RtAi_Init(a);
    Rt_DebugApplyTestMove(a);
    gRt->enemyUsed |= 1 << idx0;
    gRt->targetEnemy = 2;

    if (gRt->isDouble)
    {
        if (gRt->twoOpponents)
            idx1 = FindNextUsable(RT_SIDE_ENEMY, PARTY_SIZE / 2, PARTY_SIZE);
        else
            idx1 = FindNextUsable(RT_SIDE_ENEMY, 0, PARTY_SIZE);
        if (idx1 != PARTY_SIZE)
        {
            a = &gRt->actors[3];
            RtActor_InitFromMon(a, &gEnemyParty[idx1], RT_SIDE_ENEMY, idx1, 3);
            RtArena_FindSpawn(RT_SIDE_ENEMY, 1, &x, &y);
            a->x = x; a->y = y;
            a->facing = 0;
            RtActor_SendOut(a, idx1, !gRt->isTrainer);
            RtAi_Init(a);
            gRt->enemyUsed |= 1 << idx1;
        }
    }

    {
        s32 i;
        for (i = 2; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *e = &gRt->actors[i];
            if (!e->active)
                continue;
            if (!gRt->isTrainer || !(gBattleTypeFlags & BATTLE_TYPE_FRONTIER))
                HandleSetPokedexFlag(SpeciesToNationalPokedexNum(e->species), FLAG_SET_SEEN, GetMonData(e->mon, MON_DATA_PERSONALITY));
            if (!gRt->isTrainer && IsMonShiny(e->mon))
                gBattleResults.shinyWildMon = TRUE;
            gBattleResults.lastOpponentSpecies = e->species;
        }
    }
}

// ---------------------------------------------------------------------------
// VBlank / main loop
// ---------------------------------------------------------------------------
static void VBlankCB_Rt(void)
{
    s32 i;
    s16 sx = gRt->camX, sy = gRt->camY;

    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        if (gRt->gfxPending[i])
        {
            DmaCopy32(3, gRt->gfxBuf[i], (void *)(OBJ_VRAM0 + i * 64 * TILE_SIZE_4BPP), gRt->gfxPendingSize[i]);
            gRt->gfxPending[i] = FALSE;
        }
    }
    if (gRt->shakeTimer > 0)
    {
        sx += (Random() % (gRt->shakeMag * 2 + 1)) - gRt->shakeMag;
        sy += (Random() % (gRt->shakeMag * 2 + 1)) - gRt->shakeMag;
    }
    // the HUD layer never scrolls (a mugshot transition, as the Elite Four's, leaves BG0 shifted)
    SetGpuReg(REG_OFFSET_BG0HOFS, 0);
    SetGpuReg(REG_OFFSET_BG0VOFS, 0);
    SetGpuReg(REG_OFFSET_BG1HOFS, sx);
    SetGpuReg(REG_OFFSET_BG1VOFS, sy);
    SetGpuReg(REG_OFFSET_BG2HOFS, sx);
    SetGpuReg(REG_OFFSET_BG2VOFS, sy);
    SetGpuReg(REG_OFFSET_BG3HOFS, sx);
    SetGpuReg(REG_OFFSET_BG3VOFS, sy);
    // per-scanline brightness (soft dimming around the field, flashes)
    RtFx_LineVBlank();
    LoadOam();
    TransferPlttBuffer();
    TransferTilesetAnimsBuffer();
    {
        extern u32 gRtProf[32];
        u16 v = REG_VCOUNT & 0xFF;
(void)v;
    }
}

extern const u8 gRtDebugEnabled;
extern u8 gRtDebugAutoplay;

EWRAM_DATA u32 gRtProf[32] = {0};
EWRAM_DATA u32 gRtDebugStat = 0;

// BATTLE SPEED option: saved code -> percentage of frames that advance the fight
u8 RtBattleSpeedPct(void)
{
    static const u8 sPct[] = {80, 60, 70, 90, 100};
    u8 code;
    code = gSaveBlock2Ptr->optionsRtBattleSpeed;
    return sPct[code < ARRAY_COUNT(sPct) ? code : 0];
} // debug harness: result of a statistics command
static EWRAM_DATA u16 sFrameStartLine = 0;
EWRAM_DATA u8 gRtFxHeavy = 0;
static u16 sProfCur[16];
static u16 sProfLast;
static u16 ProfLine(void)
{
    u16 v = REG_VCOUNT & 0xFF;
    // lines since vblank start (vblank starts at line 160)
    return v >= 160 ? v - 160 : v + 68; // 228 lines
}
#define PROF(n) do { if (gRtDebugEnabled) { u16 _l = ProfLine(); if ((n) == 0) { gRtProf[9]++; gRtProf[0] += _l; memset(sProfCur, 0, sizeof(sProfCur)); sProfCur[0] = _l; } else { u16 _d = (_l >= sProfLast) ? (_l - sProfLast) : (_l + 228 - sProfLast); gRtProf[n] += _d; sProfCur[n] += _d; } sProfLast = _l; if ((n) == 6) { u32 _t = 0, _k; for (_k = 0; _k < 16; _k++) _t += sProfCur[_k]; if (_t > 222) { gRtProf[31]++; for (_k = 0; _k < 15; _k++) gRtProf[16 + _k] += sProfCur[_k]; } } } } while (0)

void RtProf(u8 n) { switch (n) { case 11: PROF(11); break; case 12: PROF(12); break; case 13: PROF(13); break; case 14: PROF(14); break; case 15: PROF(15); break; } }
static void Rt_RenderFrame(void)
{
    s32 i;
    u8 order[RT_MAX_ACTORS + RT_MAX_PROPS];
    s32 key[RT_MAX_ACTORS + RT_MAX_PROPS];
    u8 n = 0;

    RtOam_Reset();
    RtFx_ResetAffine();
    RtUi_DrawObjs();
    PROF(7);
    // actors and props, front (larger y) first: lower OAM index draws on top
    for (i = 0; i < RT_MAX_ACTORS; i++)
        if (gRt->actors[i].active)
        {
            key[n] = gRt->actors[i].y;
            order[n++] = i;
        }
    for (i = 0; i < RT_MAX_PROPS; i++)
        if (gRt->props[i].kind != PROP_NONE)
        {
            key[n] = gRt->props[i].y;
            order[n++] = 0x80 | i;
        }
    for (i = 0; i < n; i++)
    {
        s32 j;
        for (j = i + 1; j < n; j++)
        {
            if (key[j] > key[i])
            {
                u8 t = order[i];
                s32 tk = key[i];
                order[i] = order[j];
                key[i] = key[j];
                order[j] = t;
                key[j] = tk;
            }
        }
    }
    RtFx_DrawProjectiles();
    PROF(8);
    RtFx_DrawAll();
    RtVfx_DrawGhosts();
    RtFx_DrawEdges();
    PROF(10);
    for (i = 0; i < n; i++)
    {
        if (order[i] & 0x80)
            RtProps_DrawOne(order[i] & 0x7F);
        else
            RtActor_Draw(&gRt->actors[order[i]]);
    }
    RtProf(15);
    RtOam_Commit();
}

// --- smooth motion at BATTLE SPEED below 100% ----------------------------------------------------------
static EWRAM_DATA s32 sLerpSaveActX[RT_MAX_ACTORS] = {0};
static EWRAM_DATA s32 sLerpSaveActY[RT_MAX_ACTORS] = {0};
static EWRAM_DATA s32 sLerpSaveProjX[RT_MAX_PROJ] = {0};
static EWRAM_DATA s32 sLerpSaveProjY[RT_MAX_PROJ] = {0};
static EWRAM_DATA bool8 sLerpOn = FALSE;
static EWRAM_DATA u8 sLerpProjList[RT_MAX_PROJ] = {0}; // shots moved for the drawing
static EWRAM_DATA u8 sLerpProjN = 0;

static void Rt_LerpSnapshot(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        gRt->lerpActX[i] = gRt->actors[i].x;
        gRt->lerpActY[i] = gRt->actors[i].y;
    }
    for (i = 0; i < RT_MAX_PROJ; i++)
    {
        gRt->lerpProjX[i] = gRt->proj[i].x;
        gRt->lerpProjY[i] = gRt->proj[i].y;
    }
}

// q: 0..256 of the way from `from` to `to`
static inline s32 LerpCoord(s32 from, s32 to, s32 q)
{
    s32 d = to - from;
    // a jump (sent out, teleported, a new shot in a reused slot) is drawn where it is
    if (d > TO_FX(24) || d < -TO_FX(24))
        return to;
    return from + ((d * q) >> 8);
}

// swaps the drawn positions in (and back out): only the drawing sees them
static void Rt_LerpBegin(void)
{
    s32 i;
    s32 q = gRt->lerpQ8;
    if (gRt->lerpT >= 100 || sLerpOn)
        return;
    sLerpOn = TRUE;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        sLerpSaveActX[i] = a->x;
        sLerpSaveActY[i] = a->y;
        if (a->active)
        {
            a->x = LerpCoord(gRt->lerpActX[i], a->x, q);
            a->y = LerpCoord(gRt->lerpActY[i], a->y, q);
        }
    }
    sLerpProjN = 0;
    for (i = 0; i < RT_MAX_PROJ; i++)
    {
        struct RtProj *p = &gRt->proj[i];
        if (p->kind != PK_NONE && p->age > 1)
        {
            sLerpProjList[sLerpProjN++] = i;
            sLerpSaveProjX[i] = p->x;
            sLerpSaveProjY[i] = p->y;
            p->x = LerpCoord(gRt->lerpProjX[i], p->x, q);
            p->y = LerpCoord(gRt->lerpProjY[i], p->y, q);
        }
    }
}

void RtLerp_ActorDelta(u8 actor, s32 *dx, s32 *dy)
{
    *dx = *dy = 0;
    if (sLerpOn && actor < RT_MAX_ACTORS)
    {
        *dx = gRt->actors[actor].x - sLerpSaveActX[actor];
        *dy = gRt->actors[actor].y - sLerpSaveActY[actor];
    }
}

static void Rt_LerpEnd(void)
{
    s32 i;
    if (!sLerpOn)
        return;
    sLerpOn = FALSE;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        gRt->actors[i].x = sLerpSaveActX[i];
        gRt->actors[i].y = sLerpSaveActY[i];
    }
    for (i = 0; i < sLerpProjN; i++)
    {
        u8 k = sLerpProjList[i];
        gRt->proj[k].x = sLerpSaveProjX[k];
        gRt->proj[k].y = sLerpSaveProjY[k];
    }
}

static void CB2_RtMain(void)
{
    s32 i;

    gRt->frame++;
    sFrameStartLine = RtFrameLinesUsed();
    PROF(0);
    if (gRtDebugEnabled && gRtDebugAutoplay && gRt->state != RTS_FIGHT && (gRt->frame % 20) == 0)
        gMain.newKeys |= A_BUTTON;
    switch (gRt->state)
    {
    case RTS_INTRO:
        Rt_UpdateIntro();
        break;
    case RTS_FIGHT:
        // battle pace: the simulation (movement, moves, timers, animations, effects) advances on
        // BATTLE SPEED % of the frames; presses made on the other frames are kept for the next
        gRt->simAcc += RtBattleSpeedPct();
        gRt->simStep = gRt->simAcc >= 100;
        if (gRt->simStep)
        {
            gRt->simAcc -= 100;
            gMain.newKeys |= gRt->latchNew;
            gMain.newKeysRaw |= gRt->latchNewRaw;
            gRt->latchNew = gRt->latchNewRaw = 0;
            Rt_LerpSnapshot();
            if (gRt->pauseFrames)
                gRt->pauseFrames--;
            else
                Rt_UpdateFight();
        }
        else
        {
            gRt->latchNew |= gMain.newKeys;
            gRt->latchNewRaw |= gMain.newKeysRaw;
        }
        PROF(1);
        break;
    case RTS_MENU:
        // a menu action may itself end the battle (RUN, a Poke Doll): keep the outro it started
        if (RtUi_UpdateMenu() && gRt->state == RTS_MENU)
            gRt->state = gRt->menuReturnState ? gRt->menuReturnState : RTS_FIGHT;
        break;
    case RTS_PROMPT:
        if (RtUi_UpdatePrompt() && gRt->state == RTS_PROMPT)
            gRt->state = gRt->menuReturnState ? gRt->menuReturnState : RTS_FIGHT;
        break;
    case RTS_OUTRO:
        Rt_UpdateOutro();
        break;
    case RTS_EXIT:
        Rt_Exit();
        return;
    }

    {
        // animations and effects keep the fight's pace (outside the fight they run every frame)
        bool8 step = gRt->state != RTS_FIGHT || gRt->simStep;
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            if (gRt->actors[i].active)
            {
                if (step)
                    RtActor_UpdateAnim(&gRt->actors[i]);
                RtActor_LoadGfxIfNeeded(&gRt->actors[i]);
            }
        }
        PROF(2);
        if (step && gRt->state != RTS_MENU && gRt->state != RTS_PROMPT)
            RtFx_UpdateAll();
    }
    // below 100% BATTLE SPEED the fight steps on some frames only: draw (and frame the camera) between
    // the last two steps so the motion stays even
    gRt->lerpT = (gRt->state == RTS_FIGHT && RtBattleSpeedPct() < 100) ? gRt->simAcc : 100;
    gRt->lerpQ8 = gRt->lerpT * 256 / 100;
    RtUi_Update();
    Rt_UpdateLoopSE();
    PROF(4);
    Rt_LerpBegin();
    Rt_UpdateCamera();
    PROF(3);
    Rt_RenderFrame();
    Rt_LerpEnd();
    PROF(5);

    if (gRt->shakeTimer > 0)
        gRt->shakeTimer--;
    RunTasks();
    RunTextPrinters();
    UpdatePaletteFade();
    DoScheduledBgTilemapCopiesToVram();
    UpdateTilesetAnimations();
    PROF(6);
    {
        // frame load: when a frame ran long, cosmetic particle spawns are thinned out for a while
        extern u8 gRtFxHeavy;
        u16 end = RtFrameLinesUsed();
        bool8 over = end < sFrameStartLine || end > 200;
        if (over)
            gRtFxHeavy = 20;
        else if (gRtFxHeavy)
            gRtFxHeavy--;
    }
}

static void Rt_UpdateCamera(void)
{
    struct RtActor *a = NULL;
    s32 tx, ty;
    if (gRt->isSafari)
        return; // safari camera follows the reticle (rt_ui.c)
    a = Rt_PlayerActor();
    if (a && a->active && a->state != AST_GONE)
    {
        struct RtActor *f = Rt_NearestFoe(a);
        s32 px = FROM_FX(a->x), py = FROM_FX(a->y) - 12;
        s32 cx = px, cy = py;
        if (f)
        {
            // frame both the player and the nearest foe, favouring the player
            s32 fx = FROM_FX(f->x), fy = FROM_FX(f->y) - 12;
            // frame both: centred between them (the clamps below keep the player on screen)
            cx = (px + fx) / 2;
            cy = (py + fy) / 2;
        }
        // view area between the HUD bars: y in [16, 128)
        tx = cx - DISPLAY_WIDTH / 2;
        ty = cy - (HUD_TOP + (DISPLAY_HEIGHT - HUD_TOP - HUD_BOTTOM) / 2);
        // keep the player comfortably inside the view
        if (px - tx < 40) tx = px - 40;
        if (px - tx > DISPLAY_WIDTH - 40) tx = px - (DISPLAY_WIDTH - 40);
        if (py - ty < HUD_TOP + 24) ty = py - (HUD_TOP + 24);
        if (py - ty > DISPLAY_HEIGHT - HUD_BOTTOM - 24) ty = py - (DISPLAY_HEIGHT - HUD_BOTTOM - 24);
    }
    else
    {
        tx = (ARENA_W - DISPLAY_WIDTH) / 2;
        ty = (ARENA_H - DISPLAY_HEIGHT) / 2;
    }
    RtArena_ClampCamera(&tx, &ty);
    // smooth follow
    if (gRt->frame < 3)
    {
        gRt->camX = tx;
        gRt->camY = ty;
    }
    else
    {
        s32 dx = tx - gRt->camX, dy = ty - gRt->camY;
        gRt->camX += dx / 6 + (dx > 0 ? (dx % 6 != 0) : -(dx % 6 != 0));
        gRt->camY += dy / 6 + (dy > 0 ? (dy % 6 != 0) : -(dy % 6 != 0));
    }
    RtArena_StreamUpdate();
}

// ---------------------------------------------------------------------------
// Intro
// ---------------------------------------------------------------------------
static void Rt_UpdateIntro(void)
{
    s32 i;
    gRt->timer++;
    if (gRt->timer == 1)
    {
        struct RtActor *e0 = &gRt->actors[2], *e1 = &gRt->actors[3];
        if (gRt->isTrainer)
        {
            GetOpponentName(gStringVar1, gTrainerBattleOpponent_A);
            if (gRt->twoOpponents)
            {
                GetOpponentName(gStringVar2, gTrainerBattleOpponent_B);
                RtUi_ToastFmt(sText_TrainersWant, 120);
            }
            else
            {
                RtUi_ToastFmt(sText_TrainerWants, 120);
            }
        }
        else
        {
            GetMonData(e0->mon, MON_DATA_NICKNAME, gStringVar1);
            StringGet_Nickname(gStringVar1);
            if (e1->active)
            {
                GetMonData(e1->mon, MON_DATA_NICKNAME, gStringVar2);
                StringGet_Nickname(gStringVar2);
                RtUi_ToastFmt(sText_WildPairAppeared, 120);
            }
            else
            {
                RtUi_ToastFmt(sText_WildAppeared, 120);
            }
            PlayCry_Normal(e0->species, 25);
        }
    }
    if (gRt->timer == 20 && !gRt->isSafari)
    {
        struct RtActor *p = Rt_PlayerActor();
        GetMonData(p->mon, MON_DATA_NICKNAME, gStringVar1);
        StringGet_Nickname(gStringVar1);
        RtUi_ToastFmt(sText_Go, 70);
    }
    // actors finish their enter animation during the intro
    for (i = 0; i < RT_MAX_ACTORS; i++)
        if (gRt->actors[i].active)
            RtActor_Update(&gRt->actors[i]);
    if (gRt->timer >= 45)
    {
        gRt->state = RTS_FIGHT;
        gRt->timer = 0;
        RtUi_RefreshHud();
        RtUi_RefreshMoves();
        // entry abilities (intimidate, weather)
        {
            extern void RtMove_OnEnterField(struct RtActor *a);
            for (i = 0; i < RT_MAX_ACTORS; i++)
                if (gRt->actors[i].active && gRt->actors[i].state != AST_GONE)
                    RtMove_OnEnterField(&gRt->actors[i]);
        }
        // overworld weather carries into battle
        {
            extern void RtMove_InitWeatherFromField(void);
            RtMove_InitWeatherFromField();
        }
    }
}

// ---------------------------------------------------------------------------
// Fight
// ---------------------------------------------------------------------------
static void Rt_UpdateFight(void)
{
    s32 i;
    struct RtActor *p;

    gRt->timer++;
    if ((gBattleTypeFlags & BATTLE_TYPE_ROAMER) && (RtLegends_IsRoamingBattle() || !RtLegends_IsLegendBattle())
     && gRt->timer == 480 + (gRt->frame & 127)) // roaming legends flee after ~8-10 s
    {
        struct RtActor *e = &gRt->actors[2];
        struct RtActor *p0 = Rt_PlayerActor();
        if (e->active && RtActor_IsAlive(e) && !e->meanLook && !e->trap && !e->ingrain
         && p0->ability != ABILITY_SHADOW_TAG && !(p0->ability == ABILITY_ARENA_TRAP && !RtActor_HasType(e, TYPE_FLYING) && e->ability != ABILITY_LEVITATE))
        {
            Rt_EndBattle(B_OUTCOME_MON_FLED);
            return;
        }
    }
    if (gRtDebugEnabled && gRtDebugCommand == 0x40)
    {
        // catch statistics: 10000 POKe BALL rolls against the first foe at 1 HP -> gRtDebugStat
        extern u8 RtUi_DebugCountShakes(u16 ball, struct RtActor *t);
        struct RtActor *e = &gRt->actors[2];
        u16 hp = e->hp;
        s32 k;
        gRtDebugCommand = 0;
        e->hp = 1;
        gRtDebugStat = 0;
        for (k = 0; k < 10000; k++)
            if (RtUi_DebugCountShakes(ITEM_POKE_BALL, e) >= 4)
                gRtDebugStat++;
        e->hp = hp;
    }
    if (gRtDebugEnabled && gRtDebugCommand == 0x41)
    {
        // debug: the first foe is caught in a POKe BALL (capture outro test)
        struct RtActor *e = &gRt->actors[2];
        gRtDebugCommand = 0;
        if (e->active && RtActor_IsAlive(e) && !gRt->isTrainer)
        {
            gRt->ballItem = ITEM_POKE_BALL;
            e->state = AST_CAUGHT;
            e->hidden = TRUE;
            Rt_HandleCaught();
            return;
        }
    }
    if (gRtDebugEnabled && gRtDebugCommand == 16)
    {
        // lead at 1 HP and confused (self-hit faint test)
        gRtDebugCommand = 0;
        Rt_PlayerActor()->hp = 1;
        Rt_PlayerActor()->confuse = 200;
    }
    if (gRtDebugEnabled && gRtDebugCommand == 9)
    {
        gRtDebugCommand = 0;
        Rt_EndBattle(B_OUTCOME_RAN);
        return;
    }
    if (!gRt->isSafari)
    {
        p = Rt_PlayerActor();
        if (p->active && p->state != AST_GONE)
        {
            if (gRtDebugEnabled && gRtDebugAutoplay)
            {
                if (p->aiSkill == 0)
                    RtAi_Init(p);
                RtAi_Update(p);
            }
            else
            {
                Rt_HandlePlayerInput(p);
            }
        }
    }
    else
    {
        extern void RtUi_SafariInput(void);
        RtUi_SafariInput();
    }

    if (gRt->state != RTS_FIGHT)
        return;

    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        if (!a->active)
            continue;
        if (i != gRt->playerLead || gRt->isSafari || a->side != RT_SIDE_PLAYER)
            RtAi_Update(a);
    }
    RtProf(13);
    for (i = 0; i < RT_MAX_ACTORS; i++)
        if (gRt->actors[i].active)
            RtActor_Update(&gRt->actors[i]);
    RtProf(14);
    // exp participation: every player mon on the field participates against every foe on the field
    for (i = 2; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *e = &gRt->actors[i];
        s32 j;
        if (!e->active || e->state == AST_GONE)
            continue;
        for (j = 0; j < 2; j++)
        {
            struct RtActor *pl = &gRt->actors[j];
            if (pl->active && pl->state != AST_GONE && RtActor_IsAlive(pl))
            {
                e->participated |= 1 << pl->partyIdx;
                gRt->fought |= 1 << pl->partyIdx;
            }
        }
    }
    RtProj_UpdateAll();
    RtProps_Update();
    RtMove_TickFuture();
    RtMove_DoFieldTick();
    RtAi_TrainerItems();
    Rt_CheckBattleOver();

    // player requested pause menu
    if (gRt->state == RTS_FIGHT && RtExt_StartPoll() && !gRt->isSafari)
    {
        RtUi_OpenPauseMenu();
        gRt->menuReturnState = RTS_FIGHT;
        gRt->state = RTS_MENU;
    }
    // pending move learn prompts
    if (gRt->state == RTS_FIGHT && gRt->pendingLearnCount)
    {
        RtUi_OpenLearnPrompt();
        gRt->menuReturnState = RTS_FIGHT;
        gRt->state = RTS_PROMPT;
    }
    // forced switch when lead fainted
    if (gRt->state == RTS_FIGHT && gRt->switchRequest)
    {
        gRt->switchRequest = FALSE;
        RtUi_OpenPartyForced();
        gRt->menuReturnState = RTS_FIGHT;
        gRt->state = RTS_MENU;
    }
}

#define AIM_HOLD_FRAMES 10

static u8 Rt_AssistAim(struct RtActor *a, u8 angle)
{
    // soft lock: if a foe lies within ~25 degrees of the facing direction, aim exactly at it
    s32 i;
    u8 best = angle;
    s32 bestD = 0x7FFF;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *b = &gRt->actors[i];
        s32 dx, dy, d;
        u8 ang;
        s8 diff;
        if (!b->active || b->side == a->side || !RtActor_IsAlive(b) || !Rt_IsActorOnField(b) || b->hidden)
            continue;
        dx = FROM_FX(b->x - a->x);
        dy = FROM_FX(b->y - a->y);
        d = RtDist(dx, dy);
        if (d > 220)
            continue;
        ang = RtAtan2(dx, dy);
        diff = (s8)(ang - angle);
        if (diff < 0) diff = -diff;
        if (diff <= 18 && d < bestD)
        {
            bestD = d;
            best = ang;
        }
    }
    return best;
}

// AUTO AIM option: the angle to the closest foe on the field (FALSE when there is none)
static bool8 Rt_AutoAimAngle(struct RtActor *a, u8 *angle)
{
    s32 i, bestD = 0x7FFFFFFF;
    bool8 found = FALSE;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *b = &gRt->actors[i];
        s32 dx, dy, d;
        if (!b->active || b->side == a->side || !RtActor_IsAlive(b) || !Rt_IsActorOnField(b) || b->hidden)
            continue;
        dx = FROM_FX(b->x - a->x);
        dy = FROM_FX(b->y - a->y);
        d = dx * dx + dy * dy;
        if (d < bestD)
        {
            bestD = d;
            *angle = RtAtan2(dx, dy);
            found = TRUE;
        }
    }
    return found;
}

static void Rt_HandlePlayerInput(struct RtActor *a)
{
    u16 held = gMain.heldKeysRaw;
    u16 pressed = gMain.newKeysRaw;
    s16 mx = 0, my = 0;
    static const u16 sMoveButtons[MAX_MON_MOVES] = {A_BUTTON, B_BUTTON, L_BUTTON, R_BUTTON};
    s32 i;

    a->inSlot = -1;
    a->inDash = FALSE;

    if (RtStruggle_Involves(a->id))
    {
        // beam struggle: every move button press pushes the clash back
        if (pressed & (A_BUTTON | B_BUTTON | L_BUTTON | R_BUTTON))
            RtStruggle_Press(a->id);
        a->aiming = 0;
        a->aimHold = 0;
        a->inMoveX = a->inMoveY = 0;
        return;
    }

    if (held & DPAD_LEFT) mx -= 256;
    if (held & DPAD_RIGHT) mx += 256;
    if (held & DPAD_UP) my -= 256;
    if (held & DPAD_DOWN) my += 256;
    if (mx && my)
    {
        mx = mx * 181 / 256;
        my = my * 181 / 256;
    }
    if (a->confuse)
    {
        mx = -mx;
        my = -my;
    }

    // aiming with a held move button
    if (a->aiming)
    {
        u8 slot = a->aiming - 1;
        a->aimHold++;
        if (a->aimHold > AIM_HOLD_FRAMES)
        {
            // rotate aim towards the d-pad direction
            if (mx || my)
            {
                u8 want = RtAtan2(mx, my);
                s8 diff = (s8)(want - a->aim);
                if (diff > 6) diff = 6;
                if (diff < -6) diff = -6;
                a->aim += diff;
            }
            mx = my = 0;
        }
        if (!(held & sMoveButtons[slot]))
        {
            // release: fire
            u8 ang = a->aim;
            if (a->aimHold <= AIM_HOLD_FRAMES)
                ang = Rt_AssistAim(a, RtDirToAngle(a->facing));
            a->inSlot = slot;
            a->inAim = ang;
            a->aiming = 0;
            a->aimHold = 0;
        }
    }
    else
    {
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            u8 autoAng;
            if ((pressed & sMoveButtons[i]) && a->moves[i] != MOVE_NONE
             && gSaveBlock2Ptr->optionsRtAutoAim && Rt_AutoAimAngle(a, &autoAng))
            {
                // AUTO AIM: fire right away, straight at the closest foe
                a->inSlot = i;
                a->inAim = autoAng;
                break;
            }
            if ((pressed & sMoveButtons[i]) && a->moves[i] != MOVE_NONE)
            {
                a->aiming = i + 1;
                a->aimHold = 0;
                a->aim = Rt_AssistAim(a, RtDirToAngle(a->facing));
                break;
            }
        }
    }

    RtExt_PlayerInput(a, held, pressed);

    a->inMoveX = mx;
    a->inMoveY = my;

    // ball throwing replaces move 1 while a ball is readied
    if (gRt->ballReady && a->inSlot == 0)
    {
        a->inSlot = -1;
        RtMove_ThrowBall(a, gRt->ballItem, a->inAim);
        gRt->ballReady = FALSE;
        RtUi_RefreshMoves();
    }
    if (gRt->ballReady && (pressed & B_BUTTON) && !a->aiming)
    {
        gRt->ballReady = FALSE; // cancel
        RtUi_RefreshMoves();
    }
}

// ---------------------------------------------------------------------------
// Fainting / switching
// ---------------------------------------------------------------------------
void Rt_EnemySendNext(struct RtActor *slot)
{
    u8 from = 0, to = PARTY_SIZE, idx;
    if (gRt->twoOpponents)
    {
        if (slot->partyIdx >= PARTY_SIZE / 2)
            from = PARTY_SIZE / 2;
        else
            to = PARTY_SIZE / 2;
    }
    idx = FindNextUsable(RT_SIDE_ENEMY, from, to);
    if (idx == PARTY_SIZE && gRt->twoOpponents)
        idx = FindNextUsable(RT_SIDE_ENEMY, 0, PARTY_SIZE);
    if (idx == PARTY_SIZE)
        return;
    RtActor_InitFromMon(slot, &gEnemyParty[idx], RT_SIDE_ENEMY, idx, slot->id);
    {
        s32 x, y;
        RtArena_FindSpawn(RT_SIDE_ENEMY, slot->id - 2, &x, &y);
        slot->x = x;
        slot->y = y;
    }
    RtActor_SendOut(slot, idx, FALSE);
    RtAi_Init(slot);
    gRt->enemyUsed |= 1 << idx;
    HandleSetPokedexFlag(SpeciesToNationalPokedexNum(slot->species), FLAG_SET_SEEN, GetMonData(slot->mon, MON_DATA_PERSONALITY));
    GetOpponentName(gStringVar1, (gRt->twoOpponents && idx >= PARTY_SIZE / 2) ? gTrainerBattleOpponent_B : gTrainerBattleOpponent_A);
    GetMonData(slot->mon, MON_DATA_NICKNAME, gStringVar2);
    StringGet_Nickname(gStringVar2);
    RtUi_ToastFmt(sText_SentOut, 90);
    gRt->targetEnemy = slot->id;
    RtUi_RefreshHud();
    {
        extern void RtMove_OnEnterField(struct RtActor *a);
        RtMove_OnEnterField(slot);
    }
}

void Rt_PlayerSwitchTo(u8 partyIdx)
{
    struct RtActor *a = Rt_PlayerActor();
    s32 x = a->x, y = a->y;
    u8 facing = a->facing;
    bool8 wasAlive = a->active && a->state != AST_GONE && RtActor_IsAlive(a);
    u16 batonStages = 0;
    s8 stages[STG_COUNT];
    bool8 baton = (a->act.move == MOVE_BATON_PASS && a->state == AST_CAST);

    memcpy(stages, a->stages, sizeof(stages));
    if (wasAlive)
        RtActor_Withdraw(a);
    RtActor_InitFromMon(a, &gPlayerParty[partyIdx], RT_SIDE_PLAYER, partyIdx, a->id);
    a->x = x;
    a->y = y;
    a->facing = facing;
    if (baton)
        memcpy(a->stages, stages, sizeof(stages));
    (void)batonStages;
    RtActor_SendOut(a, partyIdx, FALSE);
    GetMonData(a->mon, MON_DATA_NICKNAME, gStringVar1);
    StringGet_Nickname(gStringVar1);
    RtUi_ToastFmt(sText_Go, 70);
    gBattleResults.playerSwitchesCounter++;
    RtUi_RefreshHud();
    RtUi_RefreshMoves();
    {
        extern void RtMove_OnEnterField(struct RtActor *a);
        RtMove_OnEnterField(a);
    }
}

void Rt_OnActorFainted(struct RtActor *a)
{
    GetMonData(a->mon, MON_DATA_NICKNAME, gStringVar1);
    StringGet_Nickname(gStringVar1);
    if (a->side == RT_SIDE_ENEMY)
    {
        RtUi_ToastFmt(gRt->isTrainer ? sText_EnemyFainted : sText_Fainted, 80);
        gBattleResults.opponentFaintCounter++;
        Rt_GiveExpForFaint(a);
    }
    else
    {
        RtUi_ToastFmt(sText_Fainted, 80);
        gBattleResults.playerFaintCounter++;
        AdjustFriendship(a->mon, FRIENDSHIP_EVENT_FAINT_SMALL);
    }
}

// Called by the actor update once the faint animation is done.
void Rt_AfterFaintAnim(struct RtActor *a)
{
    if (a->side == RT_SIDE_ENEMY)
    {
        if (gRt->isTrainer && Rt_CountUsableParty(RT_SIDE_ENEMY) > 0)
        {
            u8 onField = 0, i;
            for (i = 2; i < RT_MAX_ACTORS; i++)
                if (gRt->actors[i].active && gRt->actors[i].state != AST_GONE && RtActor_IsAlive(&gRt->actors[i]))
                    onField++;
            if (Rt_CountUsableParty(RT_SIDE_ENEMY) > onField)
            {
                Rt_EnemySendNext(a);
                return;
            }
        }
        a->state = AST_GONE;
        a->hidden = TRUE;
        // retarget hud
        if (gRt->targetEnemy == a->id)
        {
            u8 o = a->id == 2 ? 3 : 2;
            if (gRt->actors[o].active && gRt->actors[o].state != AST_GONE)
                gRt->targetEnemy = o;
        }
        RtUi_RefreshHud();
    }
    else
    {
        a->state = AST_GONE;
        a->hidden = TRUE;
        if (a->id == gRt->playerLead)
        {
            u8 next = FindNextUsable(RT_SIDE_PLAYER, 0, PlayerPartyLimit());
            if (next != PARTY_SIZE)
            {
                if (Rt_CountUsableParty(RT_SIDE_PLAYER) > 1 || !IsPartyIdxOnField(RT_SIDE_PLAYER, next))
                    gRt->switchRequest = TRUE;
            }
            else
            {
                // controlled mon gone but ally alive: take control of the ally
                if (gRt->actors[1].active && gRt->actors[1].state != AST_GONE && RtActor_IsAlive(&gRt->actors[1])
                    && !(gRt->partner))
                {
                    gRt->playerLead = 1;
                    RtUi_RefreshHud();
                    RtUi_RefreshMoves();
                }
            }
        }
        else
        {
            // ally: send next if any
            u8 next;
            if (gRt->partner)
                next = FindNextUsable(RT_SIDE_PLAYER, 3, PARTY_SIZE);
            else
                next = FindNextUsable(RT_SIDE_PLAYER, 0, PARTY_SIZE);
            if (next != PARTY_SIZE && (gRt->partner || Rt_CountUsableParty(RT_SIDE_PLAYER) > 1))
            {
                s32 x, y;
                RtActor_InitFromMon(a, &gPlayerParty[next], RT_SIDE_PLAYER, next, a->id);
                RtArena_FindSpawn(RT_SIDE_PLAYER, 1, &x, &y);
                a->x = x;
                a->y = y;
                RtActor_SendOut(a, next, FALSE);
                RtAi_Init(a);
                a->aiSkill = 6;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Experience
// ---------------------------------------------------------------------------
static void Rt_GiveExpToMon(u8 partyIdx, u32 exp, u16 defeatedSpecies)
{
    struct Pokemon *mon = &gPlayerParty[partyIdx];
    u32 cur, next;
    u8 level, oldLevel;
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    s32 i;

    if (!IsMonUsable(mon))
        return;
    MonGainEVs(mon, defeatedSpecies);
    level = GetMonData(mon, MON_DATA_LEVEL);
    if (level >= MAX_LEVEL)
        return;
    if (GetMonData(mon, MON_DATA_HELD_ITEM) == ITEM_LUCKY_EGG)
        exp = exp * 150 / 100;
    {
        u32 playerId = gSaveBlock2Ptr->playerTrainerId[0] | (gSaveBlock2Ptr->playerTrainerId[1] << 8)
                     | (gSaveBlock2Ptr->playerTrainerId[2] << 16) | (gSaveBlock2Ptr->playerTrainerId[3] << 24);
        if (GetMonData(mon, MON_DATA_OT_ID) != playerId)
            exp = exp * 150 / 100;
    }
    // EXP BONUS option
    {
        static const u16 sBonusPct[8] = {100, 125, 150, 200, 250, 300, 400, 500};
        exp = exp * sBonusPct[gSaveBlock2Ptr->optionsRtExpBonus & 7] / 100;
    }
    if (exp == 0)
        exp = 1;

    GetMonData(mon, MON_DATA_NICKNAME, gStringVar1);
    StringGet_Nickname(gStringVar1);
    ConvertIntToDecimalStringN(gStringVar2, exp, STR_CONV_MODE_LEFT_ALIGN, 6);
    RtUi_ToastFmt(sText_GainedExp, 70);

    cur = GetMonData(mon, MON_DATA_EXP) + exp;
    if (cur > gExperienceTables[gSpeciesInfo[species].growthRate][MAX_LEVEL])
        cur = gExperienceTables[gSpeciesInfo[species].growthRate][MAX_LEVEL];
    SetMonData(mon, MON_DATA_EXP, &cur);
    oldLevel = level;
    next = gExperienceTables[gSpeciesInfo[species].growthRate][level + 1];
    while (level < MAX_LEVEL && cur >= next)
    {
        u16 learnIdx;
        level++;
        // learn level-up moves for this level
        for (learnIdx = 0; gLevelUpLearnsets[species][learnIdx] != LEVEL_UP_END; learnIdx++)
        {
            u16 entry = gLevelUpLearnsets[species][learnIdx];
            if ((entry & LEVEL_UP_MOVE_LV) == (level << 9))
            {
                u16 move = entry & LEVEL_UP_MOVE_ID;
                u16 res = GiveMoveToMon(mon, move);
                if (res == MON_HAS_MAX_MOVES)
                {
                    if (gRt->pendingLearnCount < ARRAY_COUNT(gRt->pendingLearnMoves))
                    {
                        gRt->pendingLearnMoves[gRt->pendingLearnCount] = move;
                        gRt->pendingLearnMons[gRt->pendingLearnCount] = partyIdx;
                        gRt->pendingLearnCount++;
                    }
                }
                else if (res != MON_ALREADY_KNOWS_MOVE)
                {
                    StringCopy(gStringVar2, gMoveNames[move]);
                    RtUi_ToastFmt(sText_LearnedMove, 80);
                }
            }
        }
        if (level >= MAX_LEVEL)
            break;
        next = gExperienceTables[gSpeciesInfo[species].growthRate][level + 1];
    }
    if (level != oldLevel)
    {
        CalculateMonStats(mon);
        AdjustFriendship(mon, FRIENDSHIP_EVENT_GROW_LEVEL);
        gLeveledUpInBattle |= 1 << partyIdx;
        ConvertIntToDecimalStringN(gStringVar2, level, STR_CONV_MODE_LEFT_ALIGN, 3);
        RtUi_ToastFmt(sText_GrewLevel, 90);
        PlayFanfare(MUS_LEVEL_UP);
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *a = &gRt->actors[i];
            if (a->active && a->side == RT_SIDE_PLAYER && a->partyIdx == partyIdx && a->state != AST_GONE)
            {
                u16 oldMax = a->maxHp;
                u16 mv;
                a->level = level;
                a->maxHp = GetMonData(mon, MON_DATA_MAX_HP);
                if (RtActor_IsAlive(a))
                    a->hp += a->maxHp - oldMax;
                if (a->hp > a->maxHp)
                    a->hp = a->maxHp;
                a->atk = GetMonData(mon, MON_DATA_ATK);
                a->def = GetMonData(mon, MON_DATA_DEF);
                a->spe = GetMonData(mon, MON_DATA_SPEED);
                a->spa = GetMonData(mon, MON_DATA_SPATK);
                a->spd = GetMonData(mon, MON_DATA_SPDEF);
                for (mv = 0; mv < MAX_MON_MOVES; mv++)
                {
                    u16 m = GetMonData(mon, MON_DATA_MOVE1 + mv);
                    if (m != a->moves[mv])
                    {
                        a->moves[mv] = m;
                        a->cd[mv] = 0;
                        a->cdMax[mv] = RtMove_Cooldown(m, GetMonData(mon, MON_DATA_PP_BONUSES), mv);
                    }
                }
                RtFx_Word(a->x, a->y - TO_FX(a->height + 8), FX_W_LV_UP);
                a->hpBarDirty = TRUE;
                RtExt_TryEvolve(a, mon);
            }
        }
        RtUi_RefreshHud();
        RtUi_RefreshMoves();
    }
}

void Rt_GiveExpForFaint(struct RtActor *fainted)
{
    u8 participants[PARTY_SIZE];
    u8 nPart = 0, nShare = 0, i;
    u32 base, expEach, shareEach = 0;
    u16 species = fainted->species;
    u8 mask = fainted->participated;

    if (gRt->isTrainer && (gBattleTypeFlags & BATTLE_TYPE_FRONTIER))
        return;
    for (i = 0; i < PlayerPartyLimit(); i++)
    {
        participants[i] = FALSE;
        if (!IsMonUsable(&gPlayerParty[i]))
            continue;
        if (mask & (1 << i))
        {
            participants[i] = TRUE;
            nPart++;
        }
        if (GetMonData(&gPlayerParty[i], MON_DATA_HELD_ITEM) == ITEM_EXP_SHARE)
            nShare++;
    }
    if (nPart == 0)
        return;
    base = gSpeciesInfo[species].expYield * fainted->level / 7;
    if (gRt->isTrainer)
        base = base * 150 / 100;
    // EXP SHARE option: the EXP is split evenly among the first 2..6 POKeMON of the party
    // (plus any one outside them that took part); the EXP SHARE item is not needed then
    if (gSaveBlock2Ptr->optionsRtExpShare)
    {
        u8 firstN = gSaveBlock2Ptr->optionsRtExpShare + 1, n = 0;
        u32 each;
        for (i = 0; i < PlayerPartyLimit(); i++)
        {
            participants[i] = IsMonUsable(&gPlayerParty[i]) && (i < firstN || (mask & (1 << i)));
            if (participants[i])
                n++;
        }
        if (n == 0)
            return;
        each = base / n;
        if (each == 0)
            each = 1;
        for (i = 0; i < PlayerPartyLimit(); i++)
            if (participants[i])
                Rt_GiveExpToMon(i, each, species);
        return;
    }
    if (nShare)
    {
        expEach = (base / 2) / nPart;
        shareEach = (base / 2) / nShare;
        if (expEach == 0) expEach = 1;
        if (shareEach == 0) shareEach = 1;
    }
    else
    {
        expEach = base / nPart;
        if (expEach == 0) expEach = 1;
    }
    for (i = 0; i < PlayerPartyLimit(); i++)
    {
        u32 e = 0;
        if (!IsMonUsable(&gPlayerParty[i]))
            continue;
        if (participants[i])
            e += expEach;
        if (nShare && GetMonData(&gPlayerParty[i], MON_DATA_HELD_ITEM) == ITEM_EXP_SHARE)
            e += shareEach;
        if (e)
            Rt_GiveExpToMon(i, e, species);
    }
}

// ---------------------------------------------------------------------------
// Running / catching / end conditions
// ---------------------------------------------------------------------------
bool8 Rt_TryRun(void)
{
    struct RtActor *p = Rt_PlayerActor();
    struct RtActor *e = &gRt->actors[2];
    u8 chance;
    s32 i;

    if (gRt->isTrainer)
    {
        RtUi_Toast(sText_NoRunTrainer, 90);
        return FALSE;
    }
    if (gBattleTypeFlags & BATTLE_TYPE_FIRST_BATTLE)
    {
        RtUi_Toast(sText_CantEscape, 60);
        return FALSE;
    }
    if (p->heldItem == ITEM_SMOKE_BALL || p->ability == ABILITY_RUN_AWAY || gRt->isSafari)
    {
        Rt_StartOutro(B_OUTCOME_RAN);
        return TRUE;
    }
    // trapping
    for (i = 2; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *f = &gRt->actors[i];
        if (!f->active || f->state == AST_GONE || !RtActor_IsAlive(f))
            continue;
        if (f->ability == ABILITY_SHADOW_TAG
         || (f->ability == ABILITY_ARENA_TRAP && !RtActor_HasType(p, TYPE_FLYING) && p->ability != ABILITY_LEVITATE)
         || (f->ability == ABILITY_MAGNET_PULL && RtActor_HasType(p, TYPE_STEEL)))
        {
            RtUi_Toast(sText_CantEscape, 60);
            return FALSE;
        }
    }
    if (p->meanLook || p->trap || p->ingrain)
    {
        RtUi_Toast(sText_CantEscape, 60);
        return FALSE;
    }
    gRt->runAttempts++;
    if (p->spe >= e->spe)
    {
        Rt_StartOutro(B_OUTCOME_RAN);
        return TRUE;
    }
    chance = ((p->spe * 128) / (e->spe ? e->spe : 1) + 30 * gRt->runAttempts) & 0xFF;
    if (Random() % 256 < chance)
    {
        Rt_StartOutro(B_OUTCOME_RAN);
        return TRUE;
    }
    RtUi_Toast(sText_CantEscape, 60);
    return FALSE;
}

void Rt_HandleCaught(void)
{
    struct RtActor *e = &gRt->actors[2];
    gRt->caught = TRUE;
    Rt_StartOutro(B_OUTCOME_CAUGHT);
    (void)e;
}

void Rt_EndBattle(u8 outcome)
{
    RtExt_OnBattleEnd(outcome);
    Rt_StartOutro(outcome);
}

static void Rt_CheckBattleOver(void)
{
    s32 i;
    bool8 enemyAlive = FALSE, playerAlive = FALSE, busy = FALSE;

    if (gRt->state != RTS_FIGHT)
        return;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        if (!a->active)
            continue;
        if (a->state == AST_FAINT || a->state == AST_ENTER || a->state == AST_CAUGHT)
            busy = TRUE;
        if (a->state != AST_GONE && RtActor_IsAlive(a))
        {
            if (a->side == RT_SIDE_ENEMY)
                enemyAlive = TRUE;
            else
                playerAlive = TRUE;
        }
    }
    if (busy || gRt->switchRequest)
        return;
    if (gRt->isSafari)
        return;
    if (!enemyAlive && Rt_CountUsableParty(RT_SIDE_ENEMY) == 0)
    {
        Rt_StartOutro(B_OUTCOME_WON);
        return;
    }
    if (!enemyAlive && !gRt->isTrainer)
    {
        Rt_StartOutro(B_OUTCOME_WON);
        return;
    }
    if (!playerAlive)
    {
        u8 n = 0;
        for (i = 0; i < PlayerPartyLimit(); i++)
            if (IsMonUsable(&gPlayerParty[i]))
                n++;
        if (n == 0)
            Rt_StartOutro(B_OUTCOME_LOST);
    }
}

static void Rt_PlayVictoryMusic(void)
{
    if (!gRt->isTrainer)
    {
        PlayBGM(MUS_VICTORY_WILD);
        return;
    }
    if (gBattleTypeFlags & (BATTLE_TYPE_FRONTIER | BATTLE_TYPE_TRAINER_HILL))
    {
        if (gTrainerBattleOpponent_A == TRAINER_FRONTIER_BRAIN)
            PlayBGM(MUS_VICTORY_GYM_LEADER);
        else
            PlayBGM(MUS_VICTORY_TRAINER);
        return;
    }
    if (gTrainerBattleOpponent_A == TRAINER_SECRET_BASE)
    {
        PlayBGM(MUS_VICTORY_TRAINER);
        return;
    }
    switch (gTrainers[gTrainerBattleOpponent_A].trainerClass)
    {
    case TRAINER_CLASS_ELITE_FOUR:
    case TRAINER_CLASS_CHAMPION:
        PlayBGM(MUS_VICTORY_LEAGUE);
        break;
    case TRAINER_CLASS_TEAM_AQUA:
    case TRAINER_CLASS_TEAM_MAGMA:
    case TRAINER_CLASS_AQUA_ADMIN:
    case TRAINER_CLASS_AQUA_LEADER:
    case TRAINER_CLASS_MAGMA_ADMIN:
    case TRAINER_CLASS_MAGMA_LEADER:
        PlayBGM(MUS_VICTORY_AQUA_MAGMA);
        break;
    case TRAINER_CLASS_LEADER:
        PlayBGM(MUS_VICTORY_GYM_LEADER);
        break;
    default:
        PlayBGM(MUS_VICTORY_TRAINER);
        break;
    }
}

static u32 Rt_TrainerMoney(u16 trainerId)
{
    u32 lastLevel = 0, i;
    u8 mult = 1;
    const struct Trainer *t;

    if (trainerId == TRAINER_SECRET_BASE)
    {
        if (gBattleResources && gBattleResources->secretBase)
            return 20 * gBattleResources->secretBase->party.levels[0];
        return 0;
    }
    if (gBattleTypeFlags & (BATTLE_TYPE_FRONTIER | BATTLE_TYPE_TRAINER_HILL))
        return 0;
    t = &gTrainers[trainerId];
    if (t->partySize == 0)
        return 0;
    switch (t->partyFlags)
    {
    case 0:
        lastLevel = t->party.NoItemDefaultMoves[t->partySize - 1].lvl;
        break;
    case F_TRAINER_PARTY_CUSTOM_MOVESET:
        lastLevel = t->party.NoItemCustomMoves[t->partySize - 1].lvl;
        break;
    case F_TRAINER_PARTY_HELD_ITEM:
        lastLevel = t->party.ItemDefaultMoves[t->partySize - 1].lvl;
        break;
    case F_TRAINER_PARTY_CUSTOM_MOVESET | F_TRAINER_PARTY_HELD_ITEM:
        lastLevel = t->party.ItemCustomMoves[t->partySize - 1].lvl;
        break;
    }
    for (i = 0; gTrainerMoneyTable[i].classId != 0xFF; i++)
        if (gTrainerMoneyTable[i].classId == t->trainerClass)
            break;
    for (i = 0; i < PARTY_SIZE; i++)
        if (IsMonUsable(&gPlayerParty[i]) || GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) != SPECIES_NONE)
            if (GetMonData(&gPlayerParty[i], MON_DATA_HELD_ITEM) == ITEM_AMULET_COIN)
                mult = 2;
    {
        u32 j;
        for (j = 0; gTrainerMoneyTable[j].classId != 0xFF; j++)
            if (gTrainerMoneyTable[j].classId == t->trainerClass)
                break;
        if ((gBattleTypeFlags & BATTLE_TYPE_DOUBLE) && !gRt->twoOpponents)
            return 4 * lastLevel * mult * 2 * gTrainerMoneyTable[j].value;
        return 4 * lastLevel * mult * gTrainerMoneyTable[j].value;
    }
}

static void Rt_StartOutro(u8 outcome)
{
    s32 i;
    if (gRt->state == RTS_OUTRO || gRt->state == RTS_EXIT)
        return;
    gBattleOutcome = outcome;
    gRt->outcome = outcome;
    gRt->state = RTS_OUTRO;
    gRt->subState = 0;
    gRt->timer = 0;
    RtUi_HideToast();
    // clear projectiles
    for (i = 0; i < RT_MAX_PROJ; i++)
        gRt->proj[i].kind = PK_NONE;
    // anyone still flying / underground comes back
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        if (a->active && a->state != AST_GONE && a->state != AST_FAINT && a->state != AST_CAUGHT && a->invuln)
        {
            a->invuln = 0;
            a->hidden = FALSE;
            a->z = 0;
            if (a->state == AST_CAST)
                a->state = AST_NORMAL;
        }
    }
    m4aSongNumStop(SE_LOW_HEALTH);
    Rt_StopLoopSE();
}

static void Rt_SyncAllToParty(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        if (a->active && a->side == RT_SIDE_PLAYER && a->mon)
            RtActor_SyncToMon(a);
    }
}

static void Rt_UpdateOutro(void)
{
    s32 i;
    // holding A speeds the whole ending up
    gRt->timer += (gMain.heldKeys & A_BUTTON) ? 3 : 1;
    // keep animating actors lightly
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        if (a->active && (a->state == AST_FAINT || a->state == AST_CAUGHT || a->state == AST_RETURN))
            RtActor_Update(a);
    }
    switch (gRt->subState)
    {
    case 0:
        Rt_SyncAllToParty();
        // after a win, the Pokemon that fought catch their breath: +30% of their max HP (fainted ones get back up)
        if (gRt->outcome == B_OUTCOME_WON || gRt->outcome == B_OUTCOME_CAUGHT)
        {
            for (i = 0; i < PARTY_SIZE; i++)
            {
                struct Pokemon *mon = &gPlayerParty[i];
                u16 hp, maxHp;
                if (!(gRt->fought & (1 << i)) || GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE
                 || GetMonData(mon, MON_DATA_IS_EGG))
                    continue;
                hp = GetMonData(mon, MON_DATA_HP);
                maxHp = GetMonData(mon, MON_DATA_MAX_HP);
                if (hp >= maxHp)
                    continue;
                hp += (maxHp * 30 + 99) / 100;
                if (hp > maxHp)
                    hp = maxHp;
                SetMonData(mon, MON_DATA_HP, &hp);
            }
        }
        if (gRt->outcome != B_OUTCOME_LOST && !(gBattleTypeFlags & BATTLE_TYPE_SAFARI))
        {
            extern void RtBattle_DoPickup(void);
            RtBattle_DoPickup();
        }
        // give back items stolen in trainer battles is not needed (items stay with player in gen3)
        switch (gRt->outcome)
        {
        case B_OUTCOME_WON:
            Rt_PlayVictoryMusic();
            if (gRt->isTrainer)
            {
                GetOpponentName(gStringVar1, gTrainerBattleOpponent_A);
                RtUi_ToastFmt(sText_Defeated, 120);
            }
            break;
        case B_OUTCOME_LOST:
            break;
        case B_OUTCOME_RAN:
            PlaySE(SE_FLEE);
            RtUi_Toast(sText_GotAway, 90);
            break;
        case B_OUTCOME_CAUGHT:
            break;
        case B_OUTCOME_MON_FLED:
            GetMonData(gRt->actors[2].mon, MON_DATA_NICKNAME, gStringVar1);
            StringGet_Nickname(gStringVar1);
            RtUi_ToastFmt(sText_WildFled, 90);
            break;
        case B_OUTCOME_PLAYER_TELEPORTED:
            GetMonData(Rt_PlayerActor()->mon, MON_DATA_NICKNAME, gStringVar1);
            StringGet_Nickname(gStringVar1);
            RtUi_ToastFmt(sText_Teleported, 90);
            break;
        case B_OUTCOME_MON_TELEPORTED:
            GetMonData(gRt->actors[2].mon, MON_DATA_NICKNAME, gStringVar1);
            StringGet_Nickname(gStringVar1);
            RtUi_ToastFmt(sText_WildFled, 90);
            break;
        case B_OUTCOME_NO_SAFARI_BALLS:
            break;
        }
        gRt->subState++;
        gRt->timer = 0;
        break;
    case 1:
        // short pause to see the result
        if (gRt->timer < 50)
            break;
        gRt->subState++;
        gRt->timer = 0;
        if (gRt->outcome == B_OUTCOME_WON && gRt->isTrainer)
        {
            const u8 *txt = NULL;
            if (gBattleTypeFlags & BATTLE_TYPE_FRONTIER)
            {
                CopyFrontierTrainerText(FRONTIER_PLAYER_WON_TEXT, gTrainerBattleOpponent_A);
                txt = gStringVar4;
            }
            else if (gBattleTypeFlags & BATTLE_TYPE_TRAINER_HILL)
            {
                txt = NULL;
            }
            else if (gTrainerBattleOpponent_A != TRAINER_SECRET_BASE)
            {
                txt = GetTrainerALoseText();
            }
            if ((txt == NULL || txt[0] == EOS) && gRtDebugEnabled && gTrainerBattleOpponent_A != TRAINER_SECRET_BASE)
                txt = sText_OutOfMons; // debug battles have no script text
            if (txt != NULL && txt[0] != EOS)
            {
                extern void RtUi_ShowTrainerPic(u16 trainerId);
                RtUi_ShowTrainerPic(gTrainerBattleOpponent_A);
                RtUi_ShowMessageBox(txt);
                gRt->subState = 10;
            }
        }
        else if (gRt->outcome == B_OUTCOME_LOST)
        {
            RtUi_ShowMessageBox(sText_OutOfMons);
            gRt->subState = 12;
        }
        else if (gRt->outcome == B_OUTCOME_CAUGHT)
        {
            struct RtActor *e = &gRt->actors[2];
            u16 ball = gRt->ballItem;
            u8 ballId = ItemIdToBallId(ball);
            GetMonData(e->mon, MON_DATA_NICKNAME, gStringVar1);
            StringGet_Nickname(gStringVar1);
            HandleSetPokedexFlag(SpeciesToNationalPokedexNum(e->species), FLAG_SET_CAUGHT, GetMonData(e->mon, MON_DATA_PERSONALITY));
            SetMonData(e->mon, MON_DATA_POKEBALL, &ballId);
            RtActor_SyncToMon(e);
            gBattleResults.caughtMonSpecies = e->species;
            gBattleResults.caughtMonBall = ballId;
            GetMonData(e->mon, MON_DATA_NICKNAME, gBattleResults.caughtMonNick);
            StringExpandPlaceholders(gStringVar4, sText_Caught);
            RtUi_ShowMessageBox(gStringVar4);
            PlayFanfare(MUS_CAUGHT);
            gRt->subState = 30;
        }
        else if (gRt->outcome == B_OUTCOME_NO_SAFARI_BALLS)
        {
            RtUi_ShowMessageBox(sText_NoSafariBalls);
            gRt->subState = 12;
        }
        break;
    case 2:
        // money
        if (gRt->outcome == B_OUTCOME_WON && gRt->isTrainer)
        {
            u32 money = Rt_TrainerMoney(gTrainerBattleOpponent_A);
            if (gRt->twoOpponents)
                money += Rt_TrainerMoney(gTrainerBattleOpponent_B);
            if (money)
            {
                AddMoney(&gSaveBlock1Ptr->money, money);
                ConvertIntToDecimalStringN(gStringVar1, money, STR_CONV_MODE_LEFT_ALIGN, 6);
                StringExpandPlaceholders(gStringVar4, sText_GotMoney);
                RtUi_ShowMessageBox(gStringVar4);
                gRt->subState = 12;
                break;
            }
        }
        gRt->subState = 3;
        break;
    case 3:
        if (gRt->payDay && gRt->outcome == B_OUTCOME_WON)
        {
            AddMoney(&gSaveBlock1Ptr->money, gRt->payDay);
            ConvertIntToDecimalStringN(gStringVar1, gRt->payDay, STR_CONV_MODE_LEFT_ALIGN, 6);
            gRt->payDay = 0;
            StringExpandPlaceholders(gStringVar4, sText_PickedUpMoney);
            RtUi_ShowMessageBox(gStringVar4);
            gRt->subState = 13;
            break;
        }
        gRt->subState = 4;
        break;
    case 4:
        // EXP gained in this battle, before the move-learning prompts
        if (!gRt->expPanelDone)
        {
            gRt->expPanelDone = TRUE;
            if ((gRt->outcome == B_OUTCOME_WON || gRt->outcome == B_OUTCOME_CAUGHT) && RtUi_ExpPanelStart())
            {
                gRt->subState = 40;
                break;
            }
        }
        if (gRt->pendingLearnCount)
        {
            RtUi_OpenLearnPrompt();
            gRt->menuReturnState = RTS_OUTRO;
            gRt->state = RTS_PROMPT;
            break;
        }
        gRt->subState = 5;
        gRt->timer = 0;
        break;
    case 5:
        if (gRt->timer < 30)
            break;
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
        if (gRt->outcome != B_OUTCOME_WON)
            FadeOutMapMusic(5);
        gRt->subState = 6;
        break;
    case 6:
        if (!gPaletteFade.active)
            gRt->state = RTS_EXIT;
        break;
    case 40: // EXP panel
        if (RtUi_ExpPanelUpdate())
            gRt->subState = 4;
        break;
    case 10: // trainer speech
        if (RtUi_MessageBoxDone())
        {
            extern void RtUi_HideTrainerPic(void);
            RtUi_HideMessageBox();
            RtUi_HideTrainerPic();
            gRt->subState = 2;
        }
        break;
    case 12: // generic message then continue
        if (RtUi_MessageBoxDone())
        {
            RtUi_HideMessageBox();
            gRt->subState = (gRt->outcome == B_OUTCOME_WON) ? 3 : 4;
        }
        break;
    case 13:
        if (RtUi_MessageBoxDone())
        {
            RtUi_HideMessageBox();
            gRt->subState = 4;
        }
        break;
    case 30: // after "Gotcha": nickname?
        if (!IsFanfareTaskInactive())
            break;
        if (RtUi_MessageBoxDone())
        {
            extern void RtUi_OpenYesNo(void);
            GetMonData(gRt->actors[2].mon, MON_DATA_NICKNAME, gStringVar1);
            StringGet_Nickname(gStringVar1);
            RtUi_ShowMessageBox(sText_GiveNickname);
            RtUi_OpenYesNo();
            gRt->subState = 31;
        }
        break;
    case 31:
        {
            extern u8 RtUi_UpdateYesNo(void);
            u8 r = RtUi_UpdateYesNo();
            struct RtActor *e = &gRt->actors[2];
            if (r == 0)
                break;
            RtUi_HideMessageBox();
            if (r == 1)
            {
                // the naming screen needs heap the battle is still holding: it opens from Rt_Exit, after the
                // battle has been freed (opening it from here ran out of memory and froze the game)
                // (with a full party the naming screen itself says the POKeMON went to the PC)
                gRt->nickPending = TRUE;
                sNickMon = e->mon;
                gRt->subState = 4;
                break;
            }
            if (GiveMonToPlayer(e->mon) == MON_GIVEN_TO_PC)
            {
                GetMonData(e->mon, MON_DATA_NICKNAME, gStringVar1);
                StringGet_Nickname(gStringVar1);
                StringExpandPlaceholders(gStringVar4, sText_SentToPC);
                RtUi_ShowMessageBox(gStringVar4);
                gRt->subState = 12;
            }
            else
            {
                gRt->subState = 4;
            }
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Exit
// ---------------------------------------------------------------------------
static void Rt_FinalReturn(void)
{
    if (!(gBattleTypeFlags & BATTLE_TYPE_LINK))
    {
        RandomlyGivePartyPokerus(gPlayerParty);
        PartySpreadPokerus(gPlayerParty);
    }
    gSpecialVar_Result = gBattleOutcome;
    gMain.inBattle = FALSE;
    gMain.callback1 = gPreBattleCallback1;
    if (RtLegends_IsLegendBattle())
    {
        RtLegends_AfterBattle(gBattleOutcome);
    }
    else if (gBattleTypeFlags & BATTLE_TYPE_ROAMER)
    {
        UpdateRoamerHPStatus(&gEnemyParty[0]);
        if (gBattleOutcome == B_OUTCOME_WON || gBattleOutcome == B_OUTCOME_CAUGHT)
            SetRoamerInactive();
    }
    m4aSongNumStop(SE_LOW_HEALTH);
    Rt_StopLoopSE();
    SetMainCallback2(gMain.savedCallback);
}

static void Rt_FreeAll(void)
{
    SetVBlankCallback(NULL);
    ResetSpriteData();
    FreeAllWindowBuffers();
    FreeBattleResources();
    if (gRt)
    {
        Free(gRt);
        gRt = NULL;
    }
}

static u8 sEvoIndex;

static void CB2_RtAfterNickname(void)
{
    SetMonData(sNickMon, MON_DATA_NICKNAME, sNickBuf);
    GetMonData(sNickMon, MON_DATA_NICKNAME, gBattleResults.caughtMonNick);
    GiveMonToPlayer(sNickMon);
    sNickMon = NULL;
    Rt_FinalReturn();
}

static void CB2_RtEvolutionLoop(void)
{
    while (sEvoIndex < PARTY_SIZE)
    {
        u8 i = sEvoIndex++;
        if (gLeveledUpInBattle & (1 << i))
        {
            u16 species;
            gLeveledUpInBattle &= ~(1 << i);
            species = GetEvolutionTargetSpecies(&gPlayerParty[i], EVO_MODE_NORMAL, gLeveledUpInBattle);
            if (species != SPECIES_NONE)
            {
                gCB2_AfterEvolution = CB2_RtEvolutionLoop;
                EvolutionScene(&gPlayerParty[i], species, TRUE, i);
                return;
            }
        }
    }
    Rt_FinalReturn();
}

static void Rt_Exit(void)
{
    bool8 nick;
    // drop the field window / dimming before going back to the overworld
    SetGpuReg(REG_OFFSET_DISPCNT, GetGpuReg(REG_OFFSET_DISPCNT) & ~(DISPCNT_WIN0_ON | DISPCNT_WIN1_ON));
    SetGpuReg(REG_OFFSET_WININ, 0);
    SetGpuReg(REG_OFFSET_WINOUT, 0);
    SetGpuReg(REG_OFFSET_WIN0H, 0);
    SetGpuReg(REG_OFFSET_WIN0V, 0);
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    SetGpuReg(REG_OFFSET_BLDY, 0);
    RtFx_LineStop();
    if (gBattleTypeFlags & BATTLE_TYPE_SAFARI)
        gNumSafariBalls = gRt->safariBalls;
    TryPutPokemonTodayOnAir();
    if (gBattleResults.shinyWildMon && gBattleOutcome != B_OUTCOME_CAUGHT && !gRt->isTrainer)
        TryPutBreakingNewsOnAir();
    nick = gRt->nickPending && sNickMon != NULL;
    Rt_FreeAll();
    if (nick)
    {
        // caught POKeMON being nicknamed: the naming screen, then the mon joins the party / PC
        GetMonData(sNickMon, MON_DATA_NICKNAME, sNickBuf);
        DoNamingScreen(NAMING_SCREEN_CAUGHT_MON, sNickBuf, GetMonData(sNickMon, MON_DATA_SPECIES), GetMonGender(sNickMon),
                       GetMonData(sNickMon, MON_DATA_PERSONALITY), CB2_RtAfterNickname);
        return;
    }
    if (gLeveledUpInBattle && gBattleOutcome == B_OUTCOME_WON)
    {
        sEvoIndex = 0;
        SetMainCallback2(CB2_RtEvolutionLoop);
    }
    else
    {
        Rt_FinalReturn();
    }
}

// Exposed for the move summary screen: cooldown frames for a move.
u16 RtBattle_GetMoveCooldownFrames(u16 move, u8 ppBonuses, u8 slot)
{
    return RtMove_Cooldown(move, ppBonuses, slot);
}
