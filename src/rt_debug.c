// Debug helpers for testing the real-time battle system. Inert unless gRtDebugEnabled is patched to 1.
#include "global.h"
#include "rt_internal.h"
#include "battle.h"
#include "battle_setup.h"
#include "event_data.h"
#include "field_screen_effect.h"
#include "item.h"
#include "main.h"
#include "money.h"
#include "overworld.h"
#include "pokedex.h"
#include "pokemon.h"
#include "random.h"
#include "string_util.h"
#include "script_pokemon_util.h"
#include "constants/items.h"
#include "constants/maps.h"
#include "constants/moves.h"
#include "constants/species.h"
#include "constants/trainers.h"

const u8 gRtDebugEnabled = 0;
EWRAM_DATA u16 gRtDebugSpecies = 0;
EWRAM_DATA u8 gRtDebugLevel = 0;
EWRAM_DATA u16 gRtDebugTrainer = 0;
EWRAM_DATA u8 gRtDebugAutoplay = 0;
EWRAM_DATA u8 gRtDebugWarpIdx = 0;
EWRAM_DATA u8 gRtDebugCommand = 0;
EWRAM_DATA u8 gRtDebugAiSkill = 0;
EWRAM_DATA u16 gRtDebugTestMove = 0; // harness can poke: 1 wild, 2 trainer, 3 next warp

void RtDebug_StartTrainerBattle(u16 trainerId); // battle_setup.c

struct DebugMon
{
    u16 species;
    u8 level;
    u16 moves[4];
};

static const struct DebugMon sDebugParty[] =
{
    {SPECIES_PIKACHU, 40, {MOVE_THUNDERBOLT, MOVE_QUICK_ATTACK, MOVE_THUNDER, MOVE_THUNDER_WAVE}},
    {SPECIES_BLAZIKEN, 45, {MOVE_FLAMETHROWER, MOVE_SKY_UPPERCUT, MOVE_FIRE_BLAST, MOVE_BULK_UP}},
    {SPECIES_SWAMPERT, 45, {MOVE_SURF, MOVE_EARTHQUAKE, MOVE_DIG, MOVE_ICE_BEAM}},
    {SPECIES_GARDEVOIR, 45, {MOVE_PSYCHIC, MOVE_CALM_MIND, MOVE_HYPNOSIS, MOVE_FUTURE_SIGHT}},
    {SPECIES_PIDGEOT, 40, {MOVE_FLY, MOVE_WING_ATTACK, MOVE_GUST, MOVE_SAND_ATTACK}},
    {SPECIES_GOLEM, 40, {MOVE_ROLLOUT, MOVE_ROCK_SLIDE, MOVE_EXPLOSION, MOVE_SPIKES}},
};

static const struct { s8 group, num, x, y; } sDebugWarps[] =
{
#include "data/rt/debug_warps.h"
};

void RtDebug_NewGame(void)
{
    u32 i, j;
    static const u8 sName[] = _("TEST");
    StringCopy(gSaveBlock2Ptr->playerName, sName);
    ZeroPlayerPartyMons();
    for (i = 0; i < ARRAY_COUNT(sDebugParty); i++)
    {
        struct Pokemon mon;
        CreateMon(&mon, sDebugParty[i].species, sDebugParty[i].level, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
        for (j = 0; j < 4; j++)
            SetMonMoveSlot(&mon, sDebugParty[i].moves[j], j);
        GiveMonToPlayer(&mon);
        HandleSetPokedexFlag(SpeciesToNationalPokedexNum(sDebugParty[i].species), FLAG_SET_CAUGHT, 0);
    }
    FlagSet(FLAG_SYS_POKEMON_GET);
    FlagSet(FLAG_SYS_POKEDEX_GET);
    FlagSet(FLAG_SYS_B_DASH);
    AddBagItem(ITEM_POTION, 20);
    AddBagItem(ITEM_SUPER_POTION, 10);
    AddBagItem(ITEM_FULL_HEAL, 5);
    AddBagItem(ITEM_REVIVE, 5);
    AddBagItem(ITEM_X_ATTACK, 3);
    AddBagItem(ITEM_POKE_BALL, 20);
    AddBagItem(ITEM_ULTRA_BALL, 10);
    AddBagItem(ITEM_ETHER, 3);
    SetMoney(&gSaveBlock1Ptr->money, 50000);
    gRtDebugWarpIdx = 0;
    SetWarpDestination(sDebugWarps[0].group, sDebugWarps[0].num, WARP_ID_NONE, sDebugWarps[0].x, sDebugWarps[0].y);
    WarpIntoMap();
}

static void StartWild(void)
{
    u16 species = gRtDebugSpecies ? gRtDebugSpecies : SPECIES_ZIGZAGOON;
    u8 level = gRtDebugLevel ? gRtDebugLevel : 8;
    ZeroEnemyPartyMons();
    CreateMon(&gEnemyParty[0], species, level, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
    BattleSetup_StartWildBattle();
}

bool8 RtDebug_FieldInput(void)
{
    u8 cmd = gRtDebugCommand;
    gRtDebugCommand = 0;
    if (cmd == 1 || (gMain.newKeys & L_BUTTON))
    {
        StartWild();
        return TRUE;
    }
    if (cmd == 2 || (gMain.newKeys & R_BUTTON))
    {
        RtDebug_StartTrainerBattle(gRtDebugTrainer ? gRtDebugTrainer : TRAINER_ROXANNE_1);
        return TRUE;
    }
    if (cmd == 28)
    {
        // warp to map gRtDebugSpecies (MAP_* constant: num | group << 8) at (gRtDebugTrainer >> 8, gRtDebugTrainer & 0xFF)
        SetWarpDestination(gRtDebugSpecies >> 8, gRtDebugSpecies & 0xFF, WARP_ID_NONE, gRtDebugTrainer >> 8, gRtDebugTrainer & 0xFF);
        DoWarp();
        return TRUE;
    }
    if (cmd == 29)
    {
        // party of 5: the last party POKeMON is removed (capture-to-party test)
        ZeroMonData(&gPlayerParty[PARTY_SIZE - 1]);
        CalculatePlayerPartyCount();
        return FALSE;
    }
    if (cmd == 23)
    {
        // lead becomes gRtDebugSpecies (Lv gRtDebugLevel) and the bag gets item gRtDebugTestMove
        CreateMon(&gPlayerParty[0], gRtDebugSpecies, gRtDebugLevel ? gRtDebugLevel : 30, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
        AddBagItem(gRtDebugTestMove, 1);
        return FALSE;
    }
    if (cmd == 25)
    {
        // gRtDebugLevel more of item gRtDebugTestMove in the bag (cleared afterwards)
        AddBagItem(gRtDebugTestMove, gRtDebugLevel);
        gRtDebugTestMove = 0;
        return FALSE;
    }
    if (cmd == 24)
    {
        // the lead holds item gRtDebugTestMove (cleared afterwards so no move test starts)
        u16 item = gRtDebugTestMove;
        SetMonData(&gPlayerParty[0], MON_DATA_HELD_ITEM, &item);
        gRtDebugTestMove = 0;
        return FALSE;
    }
    if (cmd == 18)
    {
        // lead becomes a Lv5 Magikarp (nothing to relearn)
        CreateMon(&gPlayerParty[0], SPECIES_MAGIKARP, 5, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
        return FALSE;
    }
    if (cmd == 20 || cmd == 21 || cmd == 22)
    {
        // difficulty: 20 medium, 21 easy, 22 hard
        gSaveBlock2Ptr->optionsRtDifficulty = cmd - 20;
        return FALSE;
    }
    if (cmd == 19)
    {
        // lead becomes a Lv20 Combusken knowing four HMs (MOVES with a full field-move list, egg moves)
        CreateMon(&gPlayerParty[0], SPECIES_COMBUSKEN, 20, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
        SetMonMoveSlot(&gPlayerParty[0], MOVE_CUT, 0);
        SetMonMoveSlot(&gPlayerParty[0], MOVE_FLASH, 1);
        SetMonMoveSlot(&gPlayerParty[0], MOVE_ROCK_SMASH, 2);
        SetMonMoveSlot(&gPlayerParty[0], MOVE_STRENGTH, 3);
        return FALSE;
    }
    if (cmd == 17)
    {
        // Wattson's Magneton as a wild battle
        static const u16 moves[] = {MOVE_SUPERSONIC, MOVE_SHOCK_WAVE, MOVE_THUNDER_WAVE, MOVE_SONIC_BOOM};
        u8 k;
        ZeroEnemyPartyMons();
        CreateMon(&gEnemyParty[0], SPECIES_MAGNETON, 22, 31, FALSE, 0, OT_ID_PLAYER_ID, 0);
        for (k = 0; k < MAX_MON_MOVES; k++)
            SetMonMoveSlot(&gEnemyParty[0], moves[k], k);
        BattleSetup_StartWildBattle();
        return TRUE;
    }
    if (cmd == 10)
    {
        extern u8 gNumSafariBalls;
        StartWild();
        gBattleTypeFlags = BATTLE_TYPE_SAFARI;
        gNumSafariBalls = 10;
        return TRUE;
    }
    if (cmd == 8)
    {
        // Treecko one exp point below level 16 (learns Pursuit and evolves)
        struct Pokemon mon;
        u32 exp;
        CreateMon(&mon, SPECIES_TREECKO, 15, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
        SetMonMoveSlot(&mon, MOVE_POUND, 0);
        SetMonMoveSlot(&mon, MOVE_LEER, 1);
        SetMonMoveSlot(&mon, MOVE_ABSORB, 2);
        SetMonMoveSlot(&mon, MOVE_QUICK_ATTACK, 3);
        exp = gExperienceTables[gSpeciesInfo[SPECIES_TREECKO].growthRate][16] - 1;
        SetMonData(&mon, MON_DATA_EXP, &exp);
        CalculateMonStats(&mon);
        gPlayerParty[0] = mon;
        return FALSE;
    }
    if (cmd == 13)
    {
        // HM test: all HMs and badges, standing below the cut tree of Petalburg Woods
        u16 k;
        for (k = ITEM_HM01; k <= ITEM_HM08; k++)
            AddBagItem(k, 1);
        for (k = FLAG_BADGE01_GET; k <= FLAG_BADGE08_GET; k++)
            FlagSet(k);
        SetWarpDestination(MAP_GROUP(MAP_PETALBURG_WOODS), MAP_NUM(MAP_PETALBURG_WOODS), WARP_ID_NONE, 19, 11);
        DoWarp();
        return TRUE;
    }
    if (cmd == 14 || cmd == 15)
    {
        // lead at 1 HP and poisoned (15: also faint the rest of the party)
        u16 hp = GetMonData(&gPlayerParty[0], MON_DATA_MAX_HP) / (gRtDebugAutoplay ? 3 : 1000); u32 st = STATUS1_POISON; u8 k;
        if (hp == 0) hp = 1;
        SetMonData(&gPlayerParty[0], MON_DATA_HP, &hp);
        SetMonData(&gPlayerParty[0], MON_DATA_STATUS, &st);
        if (cmd == 15)
            for (k = 1, hp = 0; k < PARTY_SIZE; k++)
                if (GetMonData(&gPlayerParty[k], MON_DATA_SPECIES) != SPECIES_NONE)
                    SetMonData(&gPlayerParty[k], MON_DATA_HP, &hp);
        return FALSE;
    }
    if (cmd == 7)
    {
        HealPlayerParty();
        return FALSE;
    }
    if (cmd == 3 || cmd == 12 || (gMain.newKeys & SELECT_BUTTON))
    {
        if (cmd != 12)
            gRtDebugWarpIdx = (gRtDebugWarpIdx + 1) % ARRAY_COUNT(sDebugWarps);
        else if (gRtDebugWarpIdx >= ARRAY_COUNT(sDebugWarps))
            gRtDebugWarpIdx = 0;
        SetWarpDestination(sDebugWarps[gRtDebugWarpIdx].group, sDebugWarps[gRtDebugWarpIdx].num, WARP_ID_NONE, sDebugWarps[gRtDebugWarpIdx].x, sDebugWarps[gRtDebugWarpIdx].y);
        DoWarp();
        return TRUE;
    }
    return FALSE;
}
