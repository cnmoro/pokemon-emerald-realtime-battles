// Legendary and mythical Pokémon for the real-time mod (FireRed):
//  - the Johto dogs (Raikou, Entei, Suicune) all roam Kanto from the start of the game, at
//    level 35, and can turn up in any wild encounter on land; they keep their damage between
//    encounters and flee after a while (the original single post-game roamer is not created);
//  - post-game rarities on fitting maps (Celebi in Viridian Forest, Mew in the Pokémon Mansion);
//  - after the Hall of Fame, the event tickets (Mystic Ticket -> Lugia/Ho-Oh at Navel Rock,
//    Aurora Ticket -> Deoxys at Birth Island) are handed out automatically.
// The Kanto birds, Mewtwo and Snorlax keep their original static encounters.
// A legend stops appearing once its species is registered as caught in the Pokédex.
#include "global.h"
#include "event_data.h"
#include "field_player_avatar.h"
#include "item.h"
#include "pokedex.h"
#include "pokemon.h"
#include "random.h"
#include "constants/battle.h"
#include "constants/items.h"
#include "constants/map_types.h"
#include "constants/species.h"
#include "constants/vars.h"
#include "constants/maps.h"

#define FLAG_RT_TICKETS_GIVEN FLAG_UNUSED_0x4A7

enum
{
    LEG_LAND,
    LEG_SEA,
};

struct RtLegend
{
    u16 species;
    u8 level;
    u8 where;
    u16 damageVar; // remembers the damage it carries between encounters
};

// every roaming legend is level 35 (a lucky early ball shouldn't land a Lv70 legend)
static const struct RtLegend sRoamers[] =
{
    {SPECIES_RAIKOU,   35, LEG_LAND, VAR_0x40F7},
    {SPECIES_ENTEI,    35, LEG_LAND, VAR_0x40F8},
    {SPECIES_SUICUNE,  35, LEG_LAND, VAR_0x40F9},
};

struct RtRarity
{
    u16 species;
    u8 level;
    u8 mapGroup;
    u8 mapNumFirst, mapNumLast;
};

static const struct RtRarity sRarities[] =
{
    {SPECIES_CELEBI, 30, MAP_GROUP(MAP_VIRIDIAN_FOREST), MAP_NUM(MAP_VIRIDIAN_FOREST), MAP_NUM(MAP_VIRIDIAN_FOREST)},
    {SPECIES_MEW,    30, MAP_GROUP(MAP_POKEMON_MANSION_1F), MAP_NUM(MAP_POKEMON_MANSION_1F), MAP_NUM(MAP_POKEMON_MANSION_B1F)},
};

#define ROAM_CHANCE   48  // 1 in N wild encounters is a roaming legend (if any is left)
#define RARITY_CHANCE 12  // 1 in N wild encounters on the special maps, after the Hall of Fame

EWRAM_DATA u8 gRtLegendSlot = 0xFF; // index into sRoamers of the current battle, 0xFE = rarity, 0xFF = none

static bool8 IsCaught(u16 species)
{
    return GetSetPokedexFlag(SpeciesToNationalPokedexNum(species), FLAG_GET_CAUGHT);
}

static void GiveEventTickets(void)
{
    if (!FlagGet(FLAG_SYS_GAME_CLEAR) || FlagGet(FLAG_RT_TICKETS_GIVEN))
        return;
    FlagSet(FLAG_RT_TICKETS_GIVEN);
    if (!CheckBagHasItem(ITEM_MYSTIC_TICKET, 1))
        AddBagItem(ITEM_MYSTIC_TICKET, 1);
    if (!CheckBagHasItem(ITEM_AURORA_TICKET, 1))
        AddBagItem(ITEM_AURORA_TICKET, 1);
    FlagSet(FLAG_ENABLE_SHIP_NAVEL_ROCK);
    FlagSet(FLAG_ENABLE_SHIP_BIRTH_ISLAND);
}

// Called from the Hall of Fame (and lazily from encounters for saves that already cleared the game).
void RtLegends_OnGameClear(void)
{
    GiveEventTickets();
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

static bool8 OnMapRange(const struct RtRarity *r)
{
    return gSaveBlock1Ptr->location.mapGroup == r->mapGroup
        && gSaveBlock1Ptr->location.mapNum >= r->mapNumFirst && gSaveBlock1Ptr->location.mapNum <= r->mapNumLast;
}

// Hooked into TryStartRoamerEncounter(): fills gEnemyParty[0] and returns TRUE when a legend appears.
bool8 RtLegends_TryEncounter(void)
{
    u8 i, n = 0, pick[ARRAY_COUNT(sRoamers)];
    bool8 sea = TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING | PLAYER_AVATAR_FLAG_UNDERWATER);

    gRtLegendSlot = 0xFF;
    GiveEventTickets();

    // post-game rarities on their maps
    if (FlagGet(FLAG_SYS_GAME_CLEAR) && !sea)
    {
        for (i = 0; i < ARRAY_COUNT(sRarities); i++)
        {
            const struct RtRarity *r = &sRarities[i];
            if (OnMapRange(r) && !IsCaught(r->species) && (Random() % RARITY_CHANCE) == 0)
            {
                MakeMon(r->species, r->level, 0);
                gRtLegendSlot = 0xFE;
                return TRUE;
            }
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
    {
        const struct RtLegend *l = &sRoamers[i];
        if (IsCaught(l->species))
            continue;
        if (l->where == LEG_SEA)
        {
            // Lugia: only out at sea
            if (!sea || (gMapHeader.mapType != MAP_TYPE_OCEAN_ROUTE && gMapHeader.mapType != MAP_TYPE_UNDERWATER))
                continue;
        }
        else if (sea)
        {
            continue;
        }
        pick[n++] = i;
    }
    if (n == 0)
        return FALSE;
    i = pick[Random() % n];
    MakeMon(sRoamers[i].species, sRoamers[i].level, VarGet(sRoamers[i].damageVar));
    gRtLegendSlot = i;
    return TRUE;
}

bool8 RtLegends_IsRoamingBattle(void)
{
    return gRtLegendSlot < ARRAY_COUNT(sRoamers);
}

bool8 RtLegends_IsLegendBattle(void)
{
    return gRtLegendSlot != 0xFF;
}

// After the battle: remember the damage (or forget it once it was defeated / caught).
void RtLegends_AfterBattle(u8 outcome)
{
    if (gRtLegendSlot < ARRAY_COUNT(sRoamers))
    {
        const struct RtLegend *l = &sRoamers[gRtLegendSlot];
        u16 maxHp = GetMonData(&gEnemyParty[0], MON_DATA_MAX_HP);
        u16 hp = GetMonData(&gEnemyParty[0], MON_DATA_HP);
        if (outcome == B_OUTCOME_WON || outcome == B_OUTCOME_CAUGHT || hp == 0)
            VarSet(l->damageVar, 0); // it recovers and will roam again (unless caught)
        else
            VarSet(l->damageVar, maxHp - hp);
    }
    gRtLegendSlot = 0xFF;
}
