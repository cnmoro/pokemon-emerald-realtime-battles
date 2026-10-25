// Legendary and mythical POKeMON of the earlier generations for the Platinum demake (as in the Emerald and
// FireRed versions of the mod):
//  - roaming legends that can turn up in any wild encounter at any point of the game (Kanto birds, Johto
//    dogs, Lugia, Ho-Oh, Latias, Latios), at level 35; they keep their damage between encounters and flee
//    after a while;
//  - rarities on fitting maps of the demake (Celebi in Eterna Forest, Jirachi and Mewtwo on Mt. Coronet,
//    the Regis in the caves, Groudon, Kyogre, Rayquaza, Mew and Deoxys on routes that suit them), at the
//    levels of their original encounters: the demake has no post-game to unlock them.
// A legend stops appearing once its species is registered as caught in the POKeDEX.
#include "global.h"
#include "event_data.h"
#include "field_player_avatar.h"
#include "pokedex.h"
#include "pokemon.h"
#include "random.h"
#include "constants/battle.h"
#include "constants/maps.h"
#include "constants/species.h"
#include "constants/vars.h"

struct RtLegend
{
    u16 species;
    u8 level;
    u16 damageVar; // remembers the damage it carries between encounters
};

// every roaming legend is level 35 (a lucky early ball shouldn't land a Lv70 legend)
static const struct RtLegend sRoamers[] =
{
    {SPECIES_ARTICUNO, 35, VAR_UNUSED_0x4083},
    {SPECIES_ZAPDOS,   35, VAR_UNUSED_0x4091},
    {SPECIES_MOLTRES,  35, VAR_UNUSED_0x409B},
    {SPECIES_RAIKOU,   35, VAR_UNUSED_0x409D},
    {SPECIES_ENTEI,    35, VAR_UNUSED_0x40A1},
    {SPECIES_SUICUNE,  35, VAR_UNUSED_0x40A8},
    {SPECIES_LUGIA,    35, VAR_UNUSED_0x40B8},
    {SPECIES_HO_OH,    35, VAR_UNUSED_0x40BB},
    {SPECIES_LATIAS,   35, VAR_UNUSED_0x40E5},
    {SPECIES_LATIOS,   35, VAR_UNUSED_0x404E},
};

struct RtRarity
{
    u16 species;
    u8 level;
    u16 map;
};

static const struct RtRarity sRarities[] =
{
    {SPECIES_CELEBI,    30, MAP_ETERNA_FOREST_1},
    {SPECIES_CELEBI,    30, MAP_ETERNA_FOREST_2},
    {SPECIES_JIRACHI,   30, MAP_MT_CORONET_N1F1},
    {SPECIES_MEWTWO,    70, MAP_MT_CORONET_N1F1},
    {SPECIES_REGIROCK,  40, MAP_OREBURGH_MINE_B2F},
    {SPECIES_REGISTEEL, 40, MAP_OREBURGH_MINE_B1F},
    {SPECIES_REGICE,    40, MAP_RAVAGED_PATH},
    {SPECIES_GROUDON,   70, MAP_ROUTE207},
    {SPECIES_KYOGRE,    70, MAP_LAKE_VERITY},
    {SPECIES_RAYQUAZA,  70, MAP_ROUTE211_WEST},
    {SPECIES_MEW,       30, MAP_FLOAROMA_MEADOW},
    {SPECIES_DEOXYS,    30, MAP_VALLEY_WINDWORKS},
};

#define ROAM_CHANCE   48  // 1 in N wild encounters is a roaming legend (if any is left)
#define RARITY_CHANCE 24  // 1 in N wild encounters on the rarity's map

#define SLOT_NONE   0
#define SLOT_RARITY 0xFE
EWRAM_DATA u8 gRtLegendSlot = SLOT_NONE; // 1 + index into sRoamers of the current battle, SLOT_RARITY, or SLOT_NONE

static bool8 IsCaught(u16 species)
{
    return GetSetPokedexFlag(SpeciesToNationalPokedexNum(species), FLAG_GET_CAUGHT);
}

void RtLegends_OnGameClear(void)
{
}

static void MakeMon(u16 species, u8 level, u16 damage)
{
    u16 maxHp, hp;
    ZeroEnemyPartyMons();
    CreateMon(&gEnemyParty[0], species, level, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
    maxHp = GetMonData(&gEnemyParty[0], MON_DATA_MAX_HP);
    hp = damage >= maxHp ? 1 : maxHp - damage;
    SetMonData(&gEnemyParty[0], MON_DATA_HP, &hp);
}

// Hooked into TryStartRoamerEncounter(): fills gEnemyParty[0] and returns TRUE when a legend appears.
bool8 RtLegends_TryEncounter(void)
{
    u8 i, n = 0, pick[ARRAY_COUNT(sRoamers)];
    u16 here = (gSaveBlock1Ptr->location.mapGroup << 8) | gSaveBlock1Ptr->location.mapNum;

    gRtLegendSlot = SLOT_NONE;

    // rarities on their maps
    for (i = 0; i < ARRAY_COUNT(sRarities); i++)
    {
        const struct RtRarity *r = &sRarities[i];
        if (r->map == here && !IsCaught(r->species) && (Random() % RARITY_CHANCE) == 0)
        {
            MakeMon(r->species, r->level, 0);
            gRtLegendSlot = SLOT_RARITY;
            return TRUE;
        }
    }

    {
        extern const u8 gRtDebugEnabled;
        extern u8 gRtDebugAutoplay;
        bool8 force = gRtDebugEnabled && gRtDebugAutoplay == 2; // test hook (debug ROM only)
        if (!force && (Random() % ROAM_CHANCE) != 0)
            return FALSE;
    }
    for (i = 0; i < ARRAY_COUNT(sRoamers); i++)
        if (!IsCaught(sRoamers[i].species))
            pick[n++] = i;
    if (n == 0)
        return FALSE;
    i = pick[Random() % n];
    MakeMon(sRoamers[i].species, sRoamers[i].level, VarGet(sRoamers[i].damageVar));
    gRtLegendSlot = 1 + i;
    return TRUE;
}

bool8 RtLegends_IsRoamingBattle(void)
{
    return gRtLegendSlot != SLOT_NONE && gRtLegendSlot != SLOT_RARITY;
}

bool8 RtLegends_IsLegendBattle(void)
{
    return gRtLegendSlot != SLOT_NONE;
}

// After the battle: remember the damage (or forget it once it was defeated / caught).
void RtLegends_AfterBattle(u8 outcome)
{
    if (RtLegends_IsRoamingBattle())
    {
        const struct RtLegend *l = &sRoamers[gRtLegendSlot - 1];
        u16 maxHp = GetMonData(&gEnemyParty[0], MON_DATA_MAX_HP);
        u16 hp = GetMonData(&gEnemyParty[0], MON_DATA_HP);
        if (outcome == B_OUTCOME_WON || outcome == B_OUTCOME_CAUGHT || hp == 0)
            VarSet(l->damageVar, 0); // it recovers and will roam again (unless caught)
        else
            VarSet(l->damageVar, maxHp - hp);
    }
    gRtLegendSlot = SLOT_NONE;
}
