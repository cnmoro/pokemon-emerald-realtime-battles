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
EWRAM_DATA u32 gRtLoopCount = 0; // debug ROM: main loop iterations (PlayTimeCounter_Update runs once per loop)
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
#include "constants/moves.h"
#include "sound.h"
#include "constants/songs.h"

struct RtLcCursorOption { const u8 *text; TaskFunc func; };
static bool8 RtLc_PlayerHasMachineFor(u16 move);
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
#define RTLC_OPTION_ABILITY 31
extern struct RtLcPartyMenuInternal *sPartyMenuInternal;
void Task_ClosePartyMenu(u8 taskId);
void CB2_MoveRelearner_Init(void);
void CB2_ReturnToPartyMenuFromSummaryScreen(void);
u8 GetNumberOfRelearnableMoves(struct Pokemon *mon);

static const u8 sText_RtLcMoves[] = _("MOVES");
static const u8 sText_RtLcAbility[] = _("ABILITY");
static void RtLc_CursorCB_Moves(u8 taskId);
static void RtLc_CursorCB_Ability(u8 taskId);
const struct RtLcCursorOption RtLc_CursorOptions[RTLC_OPTION_ABILITY + 1] = {
    [RTLC_OPTION_MOVES] = {sText_RtLcMoves, RtLc_CursorCB_Moves},
    [RTLC_OPTION_ABILITY] = {sText_RtLcAbility, RtLc_CursorCB_Ability},
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
    // FLASH and STRENGTH from the bag on every POKeMON (the field moves are used from this menu)
    {
        static const u16 sMenuMoves[] = {MOVE_FLASH, MOVE_STRENGTH};
        static const u8 sMenuOptions[] = {18 + 0, 18 + 3}; // CURSOR_OPTION_FIELD_MOVES + FIELD_MOVE_FLASH / STRENGTH
        s32 k;
        for (k = 0; k < 2; k++)
        {
            if (m->numActions >= 6 || !RtLc_PlayerHasMachineFor(sMenuMoves[k]) || MonKnowsMove(&mons[slotId], sMenuMoves[k]))
                continue;
            for (at = 1; at < m->numActions && m->actions[at] >= 18; at++)
                ;
            for (i = m->numActions; i > at; i--)
                m->actions[i] = m->actions[i - 1];
            m->actions[at] = sMenuOptions[k];
            m->numActions++;
        }
    }
    // MOVES, then ABILITY, right before SWITCH
    for (at = 1; at < m->numActions; at++)
        if (m->actions[at] == RTLC_OPTION_SWITCH || m->actions[at] == RTLC_OPTION_ITEM || m->actions[at] == RTLC_OPTION_CANCEL
         || m->actions[at] < 18 /* not a field move */)
            break;
    if (m->numActions < 7 && GetNumberOfRelearnableMoves(&mons[slotId]) != 0)
    {
        for (i = m->numActions; i > at; i--)
            m->actions[i] = m->actions[i - 1];
        m->actions[at++] = RTLC_OPTION_MOVES;
        m->numActions++;
    }
    if (m->numActions < 8)
    {
        for (i = m->numActions; i > at; i--)
            m->actions[i] = m->actions[i - 1];
        m->actions[at] = RTLC_OPTION_ABILITY;
        m->numActions++;
    }
}

// --- party menu ABILITY: choose any ability the species supports, with the highlighted one's description ---
#include "window.h"
#include "menu.h"
#include "text_window.h"
#include "string_util.h"
#include "strings.h"
void PartyMenuRemoveWindow(u8 *ptr);
u8 DisplayPartyMenuMessage(const u8 *str, bool8 keepOpen);
void Task_ReturnToChooseMonAfterText(u8 taskId);
void Task_HandleChooseMonInput(u8 taskId);
void DisplayPartyMenuStdMessage(u32 stringId);
void DrawHelpMessageWindowTilesById(u8 windowId);
extern const u8 sFontColorTable[][3];
extern const struct WindowTemplate sFieldMoveDescriptionWindowTemplate;
extern const u8 *const gAbilityDescriptionPointers[];
extern const u8 gAbilityNames[][ABILITY_NAME_LENGTH + 1];

static const struct WindowTemplate sRtLcAbilityListWindow = {.bg = 2, .tilemapLeft = 19, .tilemapTop = 15, .width = 10, .height = 4, .paletteNum = 14, .baseBlock = 0x2BF};
static const struct WindowTemplate sRtLcAbilityMsgWindow = {.bg = 2, .tilemapLeft = 1, .tilemapTop = 17, .width = 15, .height = 2, .paletteNum = 15, .baseBlock = 0x299};
static const u8 sText_RtLcChooseAbility[] = _("Choose an ability.");
static const u8 sText_RtLcAbilityNow[] = _("{STR_VAR_1}'s ability is\nnow {STR_VAR_2}!{PAUSE_UNTIL_PRESS}");

static u8 RtLc_AbilityChoices(u16 species, u8 *abilities)
{
    u8 n = 0;
    if (gSpeciesInfo[species].abilities[0] != ABILITY_NONE)
        abilities[n++] = gSpeciesInfo[species].abilities[0];
    if (gSpeciesInfo[species].abilities[1] != ABILITY_NONE && gSpeciesInfo[species].abilities[1] != gSpeciesInfo[species].abilities[0])
        abilities[n++] = gSpeciesInfo[species].abilities[1];
    return n;
}

static void RtLc_PrintAbilityDescription(u8 ability)
{
    u8 *win = &sPartyMenuInternal->windowId[2];
    if (*win == WINDOW_NONE)
        *win = AddWindow(&sFieldMoveDescriptionWindowTemplate);
    DrawHelpMessageWindowTilesById(*win);
    AddTextPrinterParameterized4(*win, FONT_NORMAL, 3, 6, GetFontAttribute(FONT_NORMAL, FONTATTR_LETTER_SPACING), 0,
                                 sFontColorTable[5], 0, gAbilityDescriptionPointers[ability]);
    PutWindowTilemap(*win);
    ScheduleBgCopyTilemapToVram(2);
}

static void RtLc_Task_HandleAbilityInput(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    struct Pokemon *mon = &gPlayerParty[gPartyMenu.slotId];
    u8 abilities[2];
    s8 input;

    if (gPaletteFade.active)
        return;
    RtLc_AbilityChoices(GetMonData(mon, MON_DATA_SPECIES), abilities);
    input = Menu_ProcessInputNoWrapAround();
    if (data[2] != Menu_GetCursorPos())
    {
        data[2] = Menu_GetCursorPos();
        RtLc_PrintAbilityDescription(abilities[data[2]]);
    }
    if (input == MENU_NOTHING_CHOSEN)
        return;
    PartyMenuRemoveWindow(&sPartyMenuInternal->windowId[2]);
    PartyMenuRemoveWindow(&sPartyMenuInternal->windowId[0]);
    PartyMenuRemoveWindow(&sPartyMenuInternal->windowId[1]);
    if (input == MENU_B_PRESSED)
    {
        PlaySE(SE_SELECT);
        DisplayPartyMenuStdMessage(PARTY_MSG_CHOOSE_MON);
        gTasks[taskId].func = Task_HandleChooseMonInput;
        return;
    }
    {
        // the ability comes from the species' slot the POKeMON uses: slot 1 for the second one
        u8 abilityNum = (input == 1 || gSpeciesInfo[GetMonData(mon, MON_DATA_SPECIES)].abilities[0] == ABILITY_NONE) ? 1 : 0;
        SetMonData(mon, MON_DATA_ABILITY_NUM, &abilityNum);
        PlaySE(SE_SUCCESS);
        GetMonNickname(mon, gStringVar1);
        StringCopy(gStringVar2, gAbilityNames[abilities[input]]);
        StringExpandPlaceholders(gStringVar4, sText_RtLcAbilityNow);
        DisplayPartyMenuMessage(gStringVar4, TRUE);
        ScheduleBgCopyTilemapToVram(2);
        gTasks[taskId].func = Task_ReturnToChooseMonAfterText;
    }
}

static void RtLc_CursorCB_Ability(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    struct Pokemon *mon = &gPlayerParty[gPartyMenu.slotId];
    u8 abilities[2], cur = 0, i, n, windowId;
    u8 current = GetMonAbility(mon);

    PlaySE(SE_SELECT);
    PartyMenuRemoveWindow(&sPartyMenuInternal->windowId[0]);
    PartyMenuRemoveWindow(&sPartyMenuInternal->windowId[1]);
    PartyMenuRemoveWindow(&sPartyMenuInternal->windowId[2]);
    n = RtLc_AbilityChoices(GetMonData(mon, MON_DATA_SPECIES), abilities);
    windowId = AddWindow(&sRtLcAbilityListWindow);
    sPartyMenuInternal->windowId[0] = windowId;
    DrawStdFrameWithCustomTileAndPalette(windowId, FALSE, 0x4F, 13);
    for (i = 0; i < n; i++)
    {
        // the ability the POKeMON has now is shown in blue
        AddTextPrinterParameterized4(windowId, FONT_NORMAL, GetMenuCursorDimensionByFont(FONT_NORMAL, 0), (i * 16) + 2,
                                     GetFontAttribute(FONT_NORMAL, FONTATTR_LETTER_SPACING), 0,
                                     sFontColorTable[abilities[i] == current ? 4 : 3], 0, gAbilityNames[abilities[i]]);
        if (abilities[i] == current)
            cur = i;
    }
    Menu_InitCursor(windowId, FONT_NORMAL, 0, 2, 16, n, cur);
    // "Choose an ability." in the message box
    sPartyMenuInternal->windowId[1] = AddWindow(&sRtLcAbilityMsgWindow);
    DrawStdFrameWithCustomTileAndPalette(sPartyMenuInternal->windowId[1], FALSE, 0x58, 15);
    AddTextPrinterParameterized(sPartyMenuInternal->windowId[1], FONT_NORMAL, sText_RtLcChooseAbility, 0, 2, 0, 0);
    ScheduleBgCopyTilemapToVram(2);
    RtLc_PrintAbilityDescription(abilities[cur]);
    data[2] = cur;
    gTasks[taskId].func = RtLc_Task_HandleAbilityInput;
}

// the field move description box (shown while the cursor is on a field move) has no text for MOVES
enum { TRAMP_FIELD_MOVE_DESC = 2 };
void RtLc_ToggleFieldMoveDescriptionWindow(u8 action)
{
    TRAMP(TRAMP_FIELD_MOVE_DESC, void (*)(u8))(action >= RTLC_OPTION_MOVES ? 0 : action);
}

// --- MOVES / move relearner: after the level-up moves, the egg moves of the POKeMON and its earlier forms ---
#include "constants/moves.h"
#define RTLC_RELEARN_MAX 24 // the relearner's list holds 25 rows, one of them CANCEL
extern const u16 gEggMoves[];
#define RTLC_EGG_MOVES_SPECIES_OFFSET 20000
// FireRed 1.0's evolution table: 5 entries of 8 bytes per species, 412 species
struct RtLcEvolution { u16 method, param, targetSpecies, unused; };
extern const struct RtLcEvolution gEvolutionTable[][5];
#define RTLC_NUM_SPECIES 412
#define RTLC_EGG_MOVES_COUNT (0x8E6 / 2)
enum { TRAMP_RELEARNER_MOVES = 3 };

static void RtLc_AddMove(u16 *moves, u8 *n, const u16 *known, u16 move)
{
    s32 j;
    if (move == MOVE_NONE || move >= MOVES_COUNT || *n >= RTLC_RELEARN_MAX)
        return;
    for (j = 0; j < MAX_MON_MOVES; j++)
        if (known[j] == move)
            return;
    for (j = 0; j < *n; j++)
        if (moves[j] == move)
            return;
    moves[(*n)++] = move;
}

static u16 RtLc_PreEvolution(u16 species)
{
    s32 i, j;
    for (i = 1; i < RTLC_NUM_SPECIES; i++)
        for (j = 0; j < 5; j++)
            if (gEvolutionTable[i][j].method && gEvolutionTable[i][j].targetSpecies == species)
                return i;
    return SPECIES_NONE;
}

static void RtLc_AddEggMoves(u16 *moves, u8 *n, const u16 *known, u16 species)
{
    s32 i;
    for (i = 0; i < RTLC_EGG_MOVES_COUNT; i++)
    {
        if (gEggMoves[i] == species + RTLC_EGG_MOVES_SPECIES_OFFSET)
        {
            for (i++; i < RTLC_EGG_MOVES_COUNT && gEggMoves[i] < RTLC_EGG_MOVES_SPECIES_OFFSET; i++)
                RtLc_AddMove(moves, n, known, gEggMoves[i]);
            return;
        }
    }
}


u8 RtLc_GetMoveRelearnerMoves(struct Pokemon *mon, u16 *moves)
{
    u16 known[MAX_MON_MOVES];
    u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG, NULL);
    u8 n;
    s32 i;
    if (species == SPECIES_EGG || species == SPECIES_NONE)
        return 0;
    n = TRAMP(TRAMP_RELEARNER_MOVES, u8 (*)(struct Pokemon *, u16 *))(mon, moves);
    if (n > RTLC_RELEARN_MAX)
        n = RTLC_RELEARN_MAX;
    for (i = 0; i < MAX_MON_MOVES; i++)
        known[i] = GetMonData(mon, MON_DATA_MOVE1 + i, NULL);
    {
        // egg moves of the POKeMON and of every earlier form of its line, first form first
        u16 line[4];
        s32 len = 0, k;
        line[len++] = species;
        while (len < 4 && (line[len] = RtLc_PreEvolution(line[len - 1])) != SPECIES_NONE)
            len++;
        for (k = len - 1; k >= 0; k--)
        {
            // Volt Tackle is not in the egg move table: Pichu hatched from a parent holding a Light Ball gets it
            if (line[k] == SPECIES_PICHU)
                RtLc_AddMove(moves, &n, known, MOVE_VOLT_TACKLE);
            RtLc_AddEggMoves(moves, &n, known, line[k]);
        }
    }
    return n;
}

u8 RtLc_GetNumberOfRelearnableMoves(struct Pokemon *mon)
{
    u16 moves[RTLC_RELEARN_MAX + 8];
    return RtLc_GetMoveRelearnerMoves(mon, moves);
}

// debug ROM only (tools/lc/patch.py --debug): the game's PlayTimeCounter_Update, also counting main loops
extern u8 sPlayTimeCounterState;
void PlayTimeCounter_SetToMax(void);
void RtLc_PlayTimeCounter_Update(void)
{
    gRtLoopCount++;
    if (sPlayTimeCounterState == 1)
    {
        gSaveBlock2Ptr->playTimeVBlanks++;
        if (gSaveBlock2Ptr->playTimeVBlanks > 59)
        {
            gSaveBlock2Ptr->playTimeVBlanks = 0;
            gSaveBlock2Ptr->playTimeSeconds++;
            if (gSaveBlock2Ptr->playTimeSeconds > 59)
            {
                gSaveBlock2Ptr->playTimeSeconds = 0;
                gSaveBlock2Ptr->playTimeMinutes++;
                if (gSaveBlock2Ptr->playTimeMinutes > 59)
                {
                    gSaveBlock2Ptr->playTimeMinutes = 0;
                    gSaveBlock2Ptr->playTimeHours++;
                    if (gSaveBlock2Ptr->playTimeHours > 999)
                        PlayTimeCounter_SetToMax();
                }
            }
        }
    }
}

// --- HMs from the bag (as in the FireRed / Emerald builds) --------------------------------------------
// Owning the HM (or the TM) of a field move is enough: the first POKeMON of the party uses it; the
// Liquid Crystal badge checks stay where they are. FLY is still used from a POKeMON that knows it.
#include "event_data.h"
#include "field_player_avatar.h"
#include "item.h"
#include "script.h"
#include "constants/items.h"
#include "constants/flags.h"
extern const u16 sTMHMMoves[];
#define RTLC_NUM_MACHINES 58 // 50 TMs + 8 HMs

static bool8 RtLc_PlayerHasMachineFor(u16 move)
{
    s32 i;
    if (move == MOVE_FLY || move == MOVE_NONE)
        return FALSE;
    for (i = 0; i < RTLC_NUM_MACHINES; i++)
        if (sTMHMMoves[i] == move && CheckBagHasItem(ITEM_TM01 + i, 1))
            return TRUE;
    return FALSE;
}

// the POKeMON shown using a move nobody knows: the first one that isn't an egg
static u8 RtLc_FirstUsablePartyMon(void)
{
    u8 i;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES, NULL) == SPECIES_NONE)
            break;
        if (!GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG, NULL))
            return i;
    }
    return PARTY_SIZE;
}

bool8 RtLc_ScrCmd_checkpartymove(struct ScriptContext *ctx)
{
    u8 i;
    u16 moveId = ScriptReadHalfword(ctx);

    gSpecialVar_Result = PARTY_SIZE;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 species = GetMonData(&gPlayerParty[i], MON_DATA_SPECIES, NULL);
        if (!species)
            break;
        if (!GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG, NULL) && MonKnowsMove(&gPlayerParty[i], moveId) == TRUE)
        {
            gSpecialVar_Result = i;
            gSpecialVar_0x8004 = species;
            break;
        }
    }
    if (gSpecialVar_Result == PARTY_SIZE && RtLc_PlayerHasMachineFor(moveId))
    {
        i = RtLc_FirstUsablePartyMon();
        if (i < PARTY_SIZE)
        {
            gSpecialVar_Result = i;
            gSpecialVar_0x8004 = GetMonData(&gPlayerParty[i], MON_DATA_SPECIES, NULL);
        }
    }
    return FALSE;
}

bool8 RtLc_PartyHasMonWithSurf(void)
{
    u8 i;
    if (!TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING))
    {
        for (i = 0; i < PARTY_SIZE; i++)
        {
            if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES, NULL) == SPECIES_NONE)
                break;
            if (MonKnowsMove(&gPlayerParty[i], MOVE_SURF))
                return TRUE;
        }
        if (RtLc_PlayerHasMachineFor(MOVE_SURF) && RtLc_FirstUsablePartyMon() < PARTY_SIZE)
            return TRUE;
    }
    return FALSE;
}

// dark caves light up by themselves once FLASH is in the bag (with the badge that allows FLASH: the first)
extern const s32 gMaxFlashLevel;
void RtLc_SetDefaultFlashLevel(void)
{
    if (gMapHeader.cave && !FlagGet(FLAG_SYS_FLASH_ACTIVE) && FlagGet(FLAG_BADGE01_GET) && RtLc_PlayerHasMachineFor(MOVE_FLASH))
        FlagSet(FLAG_SYS_FLASH_ACTIVE);
    if (!gMapHeader.cave)
        gSaveBlock1Ptr->flashLevel = 0;
    else if (FlagGet(FLAG_SYS_FLASH_ACTIVE))
        gSaveBlock1Ptr->flashLevel = 0;
    else
        gSaveBlock1Ptr->flashLevel = gMaxFlashLevel;
}

// HM moves can be forgotten like any other move (they are used from the bag)
bool8 RtLc_IsMoveHm(u16 move)
{
    return FALSE;
}

// holding L while walking works as a REPEL: wild POKeMON weaker than the first healthy party member stay away
bool8 RtLc_IsWildLevelAllowedByRepel(u8 wildLevel)
{
    u8 i;
    if (!VarGet(VAR_REPEL_STEP_COUNT) && !(gMain.heldKeysRaw & L_BUTTON))
        return TRUE;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (GetMonData(&gPlayerParty[i], MON_DATA_HP) && !GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG))
            return wildLevel >= GetMonData(&gPlayerParty[i], MON_DATA_LEVEL);
    }
    return FALSE;
}

// --- LEVEL CAP option -------------------------------------------------------------------------------
// The cap is the level of the strongest POKeMON of the easiest Gym Leader not beaten yet (Liquid Crystal's
// own teams, read from its trainer table): Johto first, then the League (Lance), then Kanto in any order.
#include "level_cap.h"
#include "strings.h"
#include "task.h"
#include "menu.h"
struct RtLcCapStage { u16 flag; u8 level; };
static const struct RtLcCapStage sRtLcJohtoCaps[] = {
    {0x820, 17}, // Falkner: Pidgeotto
    {0x821, 22}, // Bugsy: Scyther
    {0x822, 25}, // Whitney: Miltank
    {0x823, 29}, // Morty: Gengar
    {0x824, 38}, // Chuck: Poliwrath
    {0x825, 41}, // Jasmine: Steelix
    {0x826, 42}, // Pryce
    {0x827, 44}, // Clair: Kingdra
};
static const struct RtLcCapStage sRtLcKantoCaps[] = {
    {0x158A, 54}, // Sabrina
    {0x156F, 55}, // Lt. Surge
    {0x188E, 56}, // Erika
    {0x160D, 58}, // Janine
    {0x1642, 63}, // Misty
    {0x167B, 65}, // Brock
    {0x1681, 65}, // Blaine
    {0x1685, 65}, // Blue
};
#define RTLC_CAP_LEAGUE 61 // Lance
#define RTLC_FLAG_GAME_CLEAR 0x82C

static u8 RtLc_LowestOpenStage(const struct RtLcCapStage *st, u8 n)
{
    u8 i, best = 0;
    for (i = 0; i < n; i++)
        if (!FlagGet(st[i].flag) && (best == 0 || st[i].level < best))
            best = st[i].level;
    return best;
}

u8 GetLevelCap(void)
{
    u8 cap;
    if (!gSaveBlock2Ptr->optionsRtLevelCap)
        return MAX_LEVEL;
    cap = RtLc_LowestOpenStage(sRtLcJohtoCaps, ARRAY_COUNT(sRtLcJohtoCaps));
    if (cap)
        return cap;
    if (!FlagGet(RTLC_FLAG_GAME_CLEAR))
        return RTLC_CAP_LEAGUE;
    cap = RtLc_LowestOpenStage(sRtLcKantoCaps, ARRAY_COUNT(sRtLcKantoCaps));
    return cap ? cap : MAX_LEVEL;
}

// RARE CANDY: no effect at the cap (the original handles everything else)
enum { TRAMP_RARE_CANDY = 6 };
void RtLc_ItemUseCB_RareCandy(u8 taskId, TaskFunc func)
{
    struct Pokemon *mon = &gPlayerParty[gPartyMenu.slotId];
    if (GetMonData(mon, MON_DATA_LEVEL) >= GetLevelCap() && GetMonData(mon, MON_DATA_LEVEL) < MAX_LEVEL)
    {
        PlaySE(SE_SELECT);
        gPartyMenuUseExitCallback = FALSE;
        DisplayPartyMenuMessage(gText_WontHaveEffect, TRUE);
        ScheduleBgCopyTilemapToVram(2);
        gTasks[taskId].func = func;
        return;
    }
    TRAMP(TRAMP_RARE_CANDY, void (*)(u8, TaskFunc))(taskId, func);
}

// Goldenrod Dept. Store (the vitamins counter): every evolution stone, so a Growlithe or an Eevee can evolve
// long before Kanto (Liquid Crystal has no trade evolutions: they all happen by level or by stone already)
const u16 RtLc_MartVitaminsAndStones[] = {
    ITEM_PROTEIN, ITEM_IRON, ITEM_CARBOS, ITEM_CALCIUM, ITEM_HP_UP,
    ITEM_FIRE_STONE, ITEM_WATER_STONE, ITEM_THUNDER_STONE, ITEM_LEAF_STONE, ITEM_MOON_STONE, ITEM_SUN_STONE,
    ITEM_NONE
};
