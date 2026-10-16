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
    {MAP_GROUP(MAP_ROUTE101), MAP_NUM(MAP_ROUTE101), 3, 3},
    {MAP_GROUP(MAP_PETALBURG_WOODS), MAP_NUM(MAP_PETALBURG_WOODS), 19, 26},
    {MAP_GROUP(MAP_RUSTBORO_CITY_GYM), MAP_NUM(MAP_RUSTBORO_CITY_GYM), 6, 16},
    {MAP_GROUP(MAP_METEOR_FALLS_1F_1R), MAP_NUM(MAP_METEOR_FALLS_1F_1R), 18, 19},
    {MAP_GROUP(MAP_SLATEPORT_CITY), MAP_NUM(MAP_SLATEPORT_CITY), 19, 30},
    {MAP_GROUP(MAP_ROUTE113), MAP_NUM(MAP_ROUTE113), 45, 5},
    {MAP_GROUP(MAP_ROUTE119), MAP_NUM(MAP_ROUTE119), 11, 70},
    {MAP_GROUP(MAP_LAVARIDGE_TOWN_GYM_1F), MAP_NUM(MAP_LAVARIDGE_TOWN_GYM_1F), 3, 4},
    {MAP_GROUP(MAP_SOOTOPOLIS_CITY_GYM_1F), MAP_NUM(MAP_SOOTOPOLIS_CITY_GYM_1F), 7, 23},
    {MAP_GROUP(MAP_EVER_GRANDE_CITY_SIDNEYS_ROOM), MAP_NUM(MAP_EVER_GRANDE_CITY_SIDNEYS_ROOM), 6, 7},
    {MAP_GROUP(MAP_SHOAL_CAVE_LOW_TIDE_ENTRANCE_ROOM), MAP_NUM(MAP_SHOAL_CAVE_LOW_TIDE_ENTRANCE_ROOM), 12, 12},
    {MAP_GROUP(MAP_FORTREE_CITY_GYM), MAP_NUM(MAP_FORTREE_CITY_GYM), 15, 9},
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
    if (cmd == 7)
    {
        HealPlayerParty();
        return FALSE;
    }
    if (cmd == 3 || (gMain.newKeys & SELECT_BUTTON))
    {
        gRtDebugWarpIdx = (gRtDebugWarpIdx + 1) % ARRAY_COUNT(sDebugWarps);
        SetWarpDestination(sDebugWarps[gRtDebugWarpIdx].group, sDebugWarps[gRtDebugWarpIdx].num, WARP_ID_NONE, sDebugWarps[gRtDebugWarpIdx].x, sDebugWarps[gRtDebugWarpIdx].y);
        DoWarp();
        return TRUE;
    }
    return FALSE;
}
