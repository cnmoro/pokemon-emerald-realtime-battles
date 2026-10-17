#ifndef GUARD_RT_COMPAT_H
#define GUARD_RT_COMPAT_H

// FireRed compatibility layer: the real-time battle code is shared with the Emerald version of the
// mod; this maps the Emerald names it uses onto pokefirered.
#include "constants/battle.h"
#include "constants/sound.h"
#include "constants/metatile_behaviors.h"
#include "fieldmap.h"
#include "item.h"
#include "new_menu_helpers.h"
#include "text.h"
#include "constants/songs.h"
#include "string_util.h"
#include "trainer_tower.h"
#include "data.h"

#define RT_FIRERED 1

// battle types FireRed doesn't have
// The Trainer Tower (Seven Island) plays the role of Emerald's Battle Frontier: facility trainers built from
// the tower's own data (name, class, picture and speech), no EXP, no prize money, no item stealing.
#define BATTLE_TYPE_FRONTIER        BATTLE_TYPE_TRAINER_TOWER
#define BATTLE_TYPE_TRAINER_HILL    0
#define BATTLE_TYPE_GROUDON         0
#define BATTLE_TYPE_KYOGRE          0
#define BATTLE_TYPE_RAYQUAZA        0
#define BATTLE_TYPE_TWO_OPPONENTS   0
#define BATTLE_TYPE_INGAME_PARTNER  0
#define BATTLE_TYPE_PYRAMID         0
#define BATTLE_TYPE_RECORDED        0
#define BATTLE_TYPE_SECRET_BASE     0
#define BATTLE_TYPE_DOME            0
#define BATTLE_TYPE_PALACE          0
#define BATTLE_TYPE_ARENA           0
#define BATTLE_TYPE_FACTORY         0
#define BATTLE_TYPE_PIKE            0
#define BATTLE_TYPE_WALLY_TUTORIAL  BATTLE_TYPE_OLD_MAN_TUTORIAL

// battle environment = FireRed's battle terrain
#define gBattleEnvironment              gBattleTerrain
#define BATTLE_ENVIRONMENT_GRASS        BATTLE_TERRAIN_GRASS
#define BATTLE_ENVIRONMENT_LONG_GRASS   BATTLE_TERRAIN_LONG_GRASS
#define BATTLE_ENVIRONMENT_SAND         BATTLE_TERRAIN_SAND
#define BATTLE_ENVIRONMENT_UNDERWATER   BATTLE_TERRAIN_UNDERWATER
#define BATTLE_ENVIRONMENT_WATER        BATTLE_TERRAIN_WATER
#define BATTLE_ENVIRONMENT_POND         BATTLE_TERRAIN_POND
#define BATTLE_ENVIRONMENT_MOUNTAIN     BATTLE_TERRAIN_MOUNTAIN
#define BATTLE_ENVIRONMENT_CAVE         BATTLE_TERRAIN_CAVE
#define BATTLE_ENVIRONMENT_BUILDING     BATTLE_TERRAIN_BUILDING
#define BATTLE_ENVIRONMENT_PLAIN        BATTLE_TERRAIN_PLAIN

// metatile attributes (FireRed packs them in 32 bits)
#define METATILE_ATTR_BEHAVIOR_MASK 0x000001FF
#define METATILE_ATTR_LAYER_MASK    0x60000000
#define METATILE_ATTR_LAYER_SHIFT   29
u32 RtMetatileAttributes(u16 metatile);
#define GetMetatileAttributesById RtMetatileAttributes

// metatile behaviours: Emerald names
#define MetatileBehavior_IsSurfableWaterOrUnderwater MetatileBehavior_IsSurfable
#define MetatileBehavior_IsSandOrDeepSand(b) (MetatileBehavior_IsSand(b) || MetatileBehavior_IsDeepSand(b))
#define MetatileBehavior_IsDoor(b) (MetatileBehavior_IsWarpDoor(b) || MetatileBehavior_IsNonAnimDoor(b))
#define RT_MB_NONE 0x1FF // never matches a FireRed behaviour
#define MB_SHORT_GRASS            RT_MB_NONE
#define MB_FOOTPRINTS             RT_MB_NONE
#define MB_LONG_GRASS             RT_MB_NONE
#define MB_ASHGRASS               RT_MB_NONE
#define MB_DEEP_SAND              RT_MB_NONE
#define MB_LONG_GRASS_SOUTH_EDGE  RT_MB_NONE
#define MB_NO_RUNNING             MB_RUNNING_DISALLOWED

// more battle types / ids FireRed doesn't have
#define BATTLE_TYPE_RECORDED_LINK        0
#define BATTLE_TYPE_RECORDED_IS_MASTER   0
#define BATTLE_TYPE_FRONTIER_NO_PYRAMID  0
#define gTrainerBattleOpponent_B         0
#define TRAINER_FRONTIER_BRAIN           0xFFFE
#define FRONTIER_PLAYER_WON_TEXT         0
#define PYRAMID_LOCATION_NONE            0
#define CurrentBattlePyramidLocation()   PYRAMID_LOCATION_NONE

// renamed functions / constants
#define GetItemHoldEffect               ItemId_GetHoldEffect
#define GetItemHoldEffectParam          ItemId_GetHoldEffectParam
#define GetItemPocket                   ItemId_GetPocket
#define GetItemBattleUsage              ItemId_GetBattleUsage
#define CountTotalItemQuantityInBag     BagGetQuantityByItemId
#define BattleSetup_GetEnvironmentId    BattleSetup_GetTerrainId
#define POCKET_BERRIES                  POCKET_BERRY_POUCH
#define FONT_SMALL_NARROW               FONT_SMALL
#define MUS_VICTORY_LEAGUE              MUS_VICTORY_GYM_LEADER
#define MUS_VICTORY_AQUA_MAGMA          MUS_VICTORY_TRAINER
extern const u8 gPPUpClearMask[];

// Emerald features FireRed doesn't have: no-ops
static inline void GetFrontierTrainerName(u8 *dst, u16 id)
{
    u8 name[12];
    GetTrainerTowerOpponentName(name);
    name[11] = EOS;
    dst = StringCopy(dst, gTrainerClassNames[GetTrainerTowerOpponentClass()]);
    *dst++ = CHAR_SPACE;
    StringCopy(dst, name);
}
static inline void GetTrainerHillTrainerName(u8 *dst, u16 id) { dst[0] = 0xFF; }
static inline void CopyFrontierTrainerText(u8 which, u16 id) { GetTrainerTowerOpponentLoseText(gStringVar4, 0); }
static inline u8 GetFrontierTrainerFrontSpriteId(u16 id) { return GetTrainerTowerTrainerFrontSpriteId(); }
static inline u8 GetTrainerHillTrainerFrontSpriteId(u16 id) { return 0; }
static inline void TryPutPokemonTodayOnAir(void) { }
static inline void TryPutBreakingNewsOnAir(void) { }
static inline void RemovePyramidBagItem(u16 item, u16 n) { }

#endif // GUARD_RT_COMPAT_H
