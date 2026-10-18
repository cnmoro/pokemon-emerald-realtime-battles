// Liquid Crystal build only: glue between the real-time battle engine and the Liquid Crystal ROM
// (a binary hack of FireRed 1.0). The engine is linked against the original FireRed addresses
// (tools/lc/gen_syms.py); tools/lc/patch.py points the ROM's hooks at the functions below.
#include "global.h"
#include "rt_internal.h"
#include "battle.h"
#include "battle_main.h"
#include "battle_setup.h"
#include "help_system.h"
#include "link.h"
#include "load_save.h"
#include "malloc.h"
#include "overworld.h"
#include "pokemon.h"
#include "rt_battle.h"
#include "constants/battle.h"

// FireRed functions that are static in the decompilation (their addresses come from the symbol file)
u8 CreateNPCTrainerParty(struct Pokemon *party, u16 trainerNum);
void Cmd_pickup(void);
void CB2_InitBattleInternal(void);
void CB2_PreInitMultiBattle(void);
void HandleLinkBattleSetup(void);
bool8 IsPlayerDefeated(u32 battleOutcome);
u8 GetTrainerBattleTransition(void);
void CreateBattleStartTask(u8 transition, u16 song);
void LockPlayerFieldControls(void);
void FreezeObjectEvents(void);
void StopPlayerAvatar(void);

// --- the original CB2_InitBattle, with the real-time diversion in front -------------------------------
void RtLc_CB2_InitBattle(void)
{
    if (RtBattle_IsSupported())
    {
        MoveSaveBlocks_ResetHeap();
        AllocateBattleResources();
        CB2_InitRtBattle();
        return;
    }
    MoveSaveBlocks_ResetHeap();
    AllocateBattleResources();
    AllocateBattleSpritesData();
    AllocateMonSpritesGfx();
    if (gBattleTypeFlags & BATTLE_TYPE_MULTI)
    {
        HandleLinkBattleSetup();
        SetMainCallback2(CB2_PreInitMultiBattle);
        gBattleCommunication[MULTIUSE_STATE] = 0;
    }
    else
    {
        CB2_InitBattleInternal();
        if (!(gBattleTypeFlags & BATTLE_TYPE_LINK))
        {
            if (gBattleTypeFlags & BATTLE_TYPE_TRAINER)
            {
                if (gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
                    SetHelpContext(HELPCONTEXT_TRAINER_BATTLE_DOUBLE);
                else
                    SetHelpContext(HELPCONTEXT_TRAINER_BATTLE_SINGLE);
            }
            else if (gBattleTypeFlags & BATTLE_TYPE_SAFARI)
            {
                SetHelpContext(HELPCONTEXT_SAFARI_BATTLE);
            }
            else
            {
                SetHelpContext(HELPCONTEXT_WILD_BATTLE);
            }
        }
    }
}

// --- engine hooks that live in battle_main.c / battle_script_commands.c in the FireRed build ----------
void RtBattle_PrepareParties(void)
{
    s32 i;
    if (!(gBattleTypeFlags & BATTLE_TYPE_LINK))
    {
        CreateNPCTrainerParty(&gEnemyParty[0], gTrainerBattleOpponent_A);
        SetWildMonHeldItem();
    }
    for (i = 0; i < PARTY_SIZE; i++)
        AdjustFriendship(&gPlayerParty[i], FRIENDSHIP_EVENT_LEAGUE_BATTLE);
}

void RtBattle_DoPickup(void)
{
    Cmd_pickup();
}

// Liquid Crystal keeps its own legendaries: the FireRed mod's roaming legends are not part of this build.
bool8 RtLegends_IsLegendBattle(void) { return FALSE; }
bool8 RtLegends_IsRoamingBattle(void) { return FALSE; }
void RtLegends_AfterBattle(u8 outcome) { }

// --- debug-only (gRtDebugEnabled is 0 in the released ROM) ---------------------------------------------
static void CB2_RtDebugEndTrainer(void)
{
    CpuFill16(0, (void *)(BG_PLTT), BG_PLTT_SIZE);
    ResetOamRange(0, 128);
    if (IsPlayerDefeated(gBattleOutcome) == TRUE)
        SetMainCallback2(CB2_WhiteOut);
    else
        SetMainCallback2(CB2_ReturnToField);
}

void RtDebug_StartTrainerBattle(u16 trainerId)
{
    gTrainerBattleOpponent_A = trainerId;
    gBattleTypeFlags = BATTLE_TYPE_TRAINER;
    if (gTrainers[trainerId].doubleBattle)
        gBattleTypeFlags |= BATTLE_TYPE_DOUBLE;
    LockPlayerFieldControls();
    FreezeObjectEvents();
    StopPlayerAvatar();
    gMain.savedCallback = CB2_RtDebugEndTrainer;
    CreateBattleStartTask(GetTrainerBattleTransition(), 0);
}

// first thing in the blob: a small table the patcher checks
struct RtLcHeader { u32 magic; u32 version; };
const struct RtLcHeader RtLc_Entry __attribute__((section(".rtlc_header"))) = { 0x434C5452 /* "RTLC" */, 1 };

// --- InitHeap: the top 4 KB of gHeap hold the engine's own variables (see tools/lc/lc.ld) --------------
extern void *sHeapStart;
extern u32 sHeapSize;
void PutFirstMemBlockHeader(void *block, u32 size);

void RtLc_InitHeap(void *heapStart, u32 heapSize)
{
    if (heapStart == gHeap && heapSize == HEAP_SIZE)
        heapSize -= 0x1000;
    sHeapStart = heapStart;
    sHeapSize = heapSize;
    PutFirstMemBlockHeader(heapStart, heapSize);
}

// --- trampolines: tools/lc/patch.py copies the first instructions of a hooked FireRed function here,
// followed by a jump back into the rest of it, so a hook can still run the original -------------------
#define RTLC_TRAMP_SIZE 32
const u8 RtLc_Trampolines[8][RTLC_TRAMP_SIZE] __attribute__((aligned(4))) = {{0x52, 0x54, 0x4C, 0x54}}; // "RTLT"
#define TRAMP(i, type) ((type)((u32)RtLc_Trampolines[i] + 1))
enum { TRAMP_FIELD_INPUT };

struct FieldInput;
bool8 RtDebug_FieldInput(void);
extern const u8 gRtDebugEnabled;
bool8 RtLc_ProcessPlayerFieldInput(struct FieldInput *input)
{
    if (gRtDebugEnabled && RtDebug_FieldInput())
        return TRUE;
    return TRAMP(TRAMP_FIELD_INPUT, bool8 (*)(struct FieldInput *))(input);
}

// --- party menu MOVES: relearn level-up moves through the game's own move relearner ---------------------
// The cursor option table is copied into the blob with one more entry (index RTLC_OPTION_MOVES, after the
// field moves); tools/lc/patch.py fills the first entries from the ROM and points the party menu at it.
#include "event_data.h"
#include "party_menu.h"
#include "sound.h"
#include "constants/songs.h"

struct RtLcCursorOption { const u8 *text; TaskFunc func; };
struct RtLcPartyMenuInternal
{
    TaskFunc task;
    MainCallback exitCallback;
    u32 bits;
    u8 windowId[3];
    u8 actions[8];
    u8 numActions;
};
#define RTLC_OPTION_SWITCH 1
#define RTLC_OPTION_ITEM   3
#define RTLC_OPTION_CANCEL 2
#define RTLC_OPTION_MOVES  30
extern struct RtLcPartyMenuInternal *sPartyMenuInternal;
void Task_ClosePartyMenu(u8 taskId);
void CB2_MoveRelearner_Init(void);
void CB2_ReturnToPartyMenuFromSummaryScreen(void);
u8 GetNumberOfRelearnableMoves(struct Pokemon *mon);

static const u8 sText_RtLcMoves[] = _("MOVES");
static void RtLc_CursorCB_Moves(u8 taskId);
const struct RtLcCursorOption RtLc_CursorOptions[RTLC_OPTION_MOVES + 1] = {
    [RTLC_OPTION_MOVES] = {sText_RtLcMoves, RtLc_CursorCB_Moves},
};

static EWRAM_DATA bool8 sRtLcRelearnFromParty = FALSE;

static void CB2_RtLcOpenRelearner(void)
{
    gSpecialVar_0x8004 = gPartyMenu.slotId;
    sRtLcRelearnFromParty = TRUE;
    SetMainCallback2(CB2_MoveRelearner_Init);
}

static void RtLc_CursorCB_Moves(u8 taskId)
{
    PlaySE(SE_SELECT);
    sPartyMenuInternal->exitCallback = CB2_RtLcOpenRelearner;
    Task_ClosePartyMenu(taskId);
}

// the relearner's exit (it used to go straight back to the field): back to the party menu when opened from it
void RtLc_RelearnerExit(void)
{
    if (sRtLcRelearnFromParty)
    {
        sRtLcRelearnFromParty = FALSE;
        SetMainCallback2(CB2_ReturnToPartyMenuFromSummaryScreen);
    }
    else
    {
        SetMainCallback2(CB2_ReturnToField);
    }
}

enum { TRAMP_PARTY_ACTIONS = 1 };
// after the game's list (SUMMARY, field moves, SWITCH, ITEM / MAIL, CANCEL): MOVES goes right before SWITCH
void RtLc_SetPartyMonFieldSelectionActions(struct Pokemon *mons, u8 slotId)
{
    struct RtLcPartyMenuInternal *m;
    s32 i, at;
    TRAMP(TRAMP_PARTY_ACTIONS, void (*)(struct Pokemon *, u8))(mons, slotId);
    m = sPartyMenuInternal;
    if (m->numActions >= 7 || GetNumberOfRelearnableMoves(&mons[slotId]) == 0)
        return;
    for (at = 1; at < m->numActions; at++)
        if (m->actions[at] == RTLC_OPTION_SWITCH || m->actions[at] == RTLC_OPTION_ITEM || m->actions[at] == RTLC_OPTION_CANCEL
         || m->actions[at] < 18 /* not a field move */)
            break;
    for (i = m->numActions; i > at; i--)
        m->actions[i] = m->actions[i - 1];
    m->actions[at] = RTLC_OPTION_MOVES;
    m->numActions++;
}

// the field move description box (shown while the cursor is on a field move) has no text for MOVES
enum { TRAMP_FIELD_MOVE_DESC = 2 };
void RtLc_ToggleFieldMoveDescriptionWindow(u8 action)
{
    TRAMP(TRAMP_FIELD_MOVE_DESC, void (*)(u8))(action == RTLC_OPTION_MOVES ? 0 : action);
}
