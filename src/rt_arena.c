// Real-time battle arena: built from the tileset of the map the battle started on.
#include "global.h"
#include "rt_internal.h"
#include "battle.h"
#include "bg.h"
#include "decompress.h"
#include "fieldmap.h"
#include "field_player_avatar.h"
#include "malloc.h"
#include "metatile_behavior.h"
#include "overworld.h"
#include "palette.h"
#include "random.h"
#include "tileset_anims.h"
#include "constants/map_types.h"
#include "constants/metatile_behaviors.h"

// --- fast map access while the arena is built ------------------------------------------------------
// RtArena_Build looks at every cell of an 80x48 area several times (metatile, collision, elevation,
// behaviour). The original accessors (border/connection handling, attribute lookups, behaviour checks
// through several calls) made that take about a second, so the area's blocks and the per-metatile
// behaviour flags are cached for the duration of the build.
#if RT_FIRERED
#define RT_VMAP VMap
#define RT_VMAP_W Xsize
#define RT_VMAP_H Ysize
#else
#define RT_VMAP gBackupMapLayout
#define RT_VMAP_W width
#define RT_VMAP_H height
#endif
static u16 *sBlkCache;          // meta | collision << 10 | elevation << 12 for every cell of the area
static u8 *sCellMF;             // MF_* of every cell of the area (MF_INVALID: no metatile)
static s32 sBlkX0, sBlkY0, sBlkW, sBlkH;
static u16 *sMetaBehCache;      // behaviour per metatile, 0xFFFF = not looked up yet
static u8 *sMetaFlagCache;      // MF_* per metatile, 0 = not looked up yet
static u8 MetaFlags(u16 m);

static u16 RtBlockAtSlow(s32 x, s32 y)
{
    if (x >= 0 && y >= 0 && x < RT_VMAP.RT_VMAP_W && y < RT_VMAP.RT_VMAP_H)
    {
        u16 b = RT_VMAP.map[x + RT_VMAP.RT_VMAP_W * y];
        if (b != MAPGRID_UNDEFINED)
            return b;
    }
    return (MapGridGetMetatileIdAt(x, y) & MAPGRID_METATILE_ID_MASK)
         | ((MapGridGetCollisionAt(x, y) & 3) << MAPGRID_COLLISION_SHIFT)
         | ((MapGridGetElevationAt(x, y) & 15) << MAPGRID_ELEVATION_SHIFT);
}

static inline u16 RtBlockAt(s32 x, s32 y)
{
    u32 ix = x - sBlkX0, iy = y - sBlkY0;
    if (ix < (u32)sBlkW && iy < (u32)sBlkH) // sBlkW is 0 without a cache
        return sBlkCache[iy * sBlkW + ix];
    return RtBlockAtSlow(x, y);
}

static inline u32 RtMapMeta(s32 x, s32 y) { return RtBlockAt(x, y) & MAPGRID_METATILE_ID_MASK; }
static inline u8 RtMapColl(s32 x, s32 y) { return (RtBlockAt(x, y) & MAPGRID_COLLISION_MASK) >> MAPGRID_COLLISION_SHIFT; }
static inline u8 RtMapElev(s32 x, s32 y) { return RtBlockAt(x, y) >> MAPGRID_ELEVATION_SHIFT; }

// The caches live in gDecompressionBuffer (16 KB, only used while graphics are loaded, which happens
// after the build): the heap is nearly full at this point and a failed Alloc stops a FireRed ROM.
static void RtMapCache_Begin(s32 x0, s32 y0, s32 w, s32 h)
{
    s32 x, y;
    u16 *b;
    u8 *f;
    u8 *mem = gDecompressionBuffer;
    sBlkW = sBlkH = 0;
    if (w * h * 3 + NUM_METATILES_TOTAL * 3 > (s32)sizeof(gDecompressionBuffer))
    {
        sBlkCache = NULL;
        sCellMF = NULL;
        sMetaBehCache = NULL;
        sMetaFlagCache = NULL;
        return;
    }
    sBlkCache = (u16 *)mem;
    mem += w * h * sizeof(u16);
    sMetaBehCache = (u16 *)mem;
    mem += NUM_METATILES_TOTAL * sizeof(u16);
    sCellMF = mem;
    mem += w * h;
    sMetaFlagCache = mem;
    memset(sMetaBehCache, 0xFF, NUM_METATILES_TOTAL * sizeof(u16));
    memset(sMetaFlagCache, 0, NUM_METATILES_TOTAL);
    b = sBlkCache;
    f = sCellMF;
    for (y = y0; y < y0 + h; y++)
    {
        // rows inside the loaded map are copied straight from it
        bool8 rowIn = (y >= 0 && y < RT_VMAP.RT_VMAP_H);
        const u16 *row = rowIn ? &RT_VMAP.map[RT_VMAP.RT_VMAP_W * y] : NULL;
        for (x = x0; x < x0 + w; x++)
        {
            u16 v = MAPGRID_UNDEFINED;
            if (rowIn && x >= 0 && x < RT_VMAP.RT_VMAP_W)
                v = row[x];
            if (v == MAPGRID_UNDEFINED)
                v = RtBlockAtSlow(x, y);
            *b++ = v;
            v &= MAPGRID_METATILE_ID_MASK;
            *f++ = (v < NUM_METATILES_TOTAL && sMetaFlagCache[v]) ? sMetaFlagCache[v] : MetaFlags(v);
        }
    }
    sBlkX0 = x0;
    sBlkY0 = y0;
    sBlkW = w;
    sBlkH = h;
}

static void RtMapCache_End(void)
{
    sBlkW = sBlkH = 0;
    sBlkCache = NULL;
    sCellMF = NULL;
    sMetaBehCache = NULL;
    sMetaFlagCache = NULL;
}

// from here on the arena code reads the map through the cache
#define MapGridGetMetatileIdAt(x, y) RtMapMeta(x, y)
#define MapGridGetCollisionAt(x, y) RtMapColl(x, y)
#define MapGridGetElevationAt(x, y) RtMapElev(x, y)

#define SCAN_RX 18
#define SCAN_RY 14
#define MAX_STAMPS 6

struct Stamp
{
    u8 w, h;
    u16 meta[2][2]; // 0xFFFF = empty
};

static u16 MetaAt(s32 x, s32 y)
{
    return MapGridGetMetatileIdAt(x, y);
}

static u8 BehaviorOf(u16 metatile)
{
    if (sMetaBehCache != NULL && metatile < NUM_METATILES_TOTAL)
    {
        if (sMetaBehCache[metatile] == 0xFFFF)
            sMetaBehCache[metatile] = GetMetatileAttributesById(metatile) & METATILE_ATTR_BEHAVIOR_MASK;
        return sMetaBehCache[metatile];
    }
    return GetMetatileAttributesById(metatile) & METATILE_ATTR_BEHAVIOR_MASK;
}

// behaviour classes used per cell by the arena build
#define MF_KNOWN     (1 << 0)
#define MF_SURF      (1 << 1) // surfable water or underwater
#define MF_WATERFALL (1 << 2)
#define MF_LEDGE     (1 << 3)
#define MF_GROUND    (1 << 4) // a ground the battle field can keep as it is
#define MF_INVALID   (1 << 5) // not a metatile of the tilesets
static bool8 IsLedgeMeta(u16 m);
static u8 MetaFlags(u16 m)
{
    u8 f;
    u8 b;
    if (m >= NUM_METATILES_TOTAL)
        return MF_KNOWN | MF_INVALID;
    if (sMetaFlagCache != NULL && sMetaFlagCache[m])
        return sMetaFlagCache[m];
    b = BehaviorOf(m);
    f = MF_KNOWN;
    if (MetatileBehavior_IsSurfableWaterOrUnderwater(b))
        f |= MF_SURF;
    if (MetatileBehavior_IsWaterfall(b))
        f |= MF_WATERFALL;
    if (MetatileBehavior_IsJumpEast(b) || MetatileBehavior_IsJumpWest(b)
     || MetatileBehavior_IsJumpNorth(b) || MetatileBehavior_IsJumpSouth(b))
        f |= MF_LEDGE;
    if (b == MB_NORMAL || b == MB_TALL_GRASS || b == MB_LONG_GRASS || b == MB_ASHGRASS || b == MB_SAND
     || b == MB_DEEP_SAND || b == MB_SHORT_GRASS || b == MB_FOOTPRINTS || b == MB_CAVE
     || b == MB_INDOOR_ENCOUNTER || b == MB_MOUNTAIN_TOP || b == MB_LONG_GRASS_SOUTH_EDGE || b == MB_ICE
     || b == MB_NO_RUNNING || b == MB_TRICK_HOUSE_PUZZLE_8_FLOOR)
        f |= MF_GROUND;
    if (sMetaFlagCache != NULL)
        sMetaFlagCache[m] = f;
    return f;
}

static bool8 IsFloorBehavior(u8 b)
{
    return b == MB_NORMAL || MetatileBehavior_IsSandOrDeepSand(b) || MetatileBehavior_IsIce(b)
        || b == MB_SHORT_GRASS || b == MB_CAVE || b == MB_FOOTPRINTS;
}

static u16 PickFloor(s32 px, s32 py, u16 *counts)
{
    s32 x, y, i;
    u16 best = 0xFFFF, bestN = 0;
    memset(counts, 0, NUM_METATILES_TOTAL * sizeof(u16));
    for (y = py - SCAN_RY; y <= py + SCAN_RY; y++)
    {
        for (x = px - SCAN_RX; x <= px + SCAN_RX; x++)
        {
            u16 m;
            u8 b;
            if (MapGridGetCollisionAt(x, y) != 0)
                continue;
            m = MetaAt(x, y);
            if (m >= NUM_METATILES_TOTAL)
                continue;
            b = BehaviorOf(m);
            if (b != MB_NORMAL)
                continue;
            counts[m]++;
        }
    }
    for (i = 0; i < NUM_METATILES_TOTAL; i++)
    {
        if (counts[i] > bestN)
        {
            bestN = counts[i];
            best = i;
        }
    }
    return best;
}

static bool8 CellPassable(s32 x, s32 y)
{
    return MapGridGetCollisionAt(x, y) == 0;
}

static u8 FindStamps(s32 px, s32 py, struct Stamp *stamps)
{
    s32 x, y;
    u8 n = 0;
    for (y = py - SCAN_RY; y <= py + SCAN_RY && n < MAX_STAMPS; y++)
    {
        for (x = px - SCAN_RX; x <= px + SCAN_RX && n < MAX_STAMPS; x++)
        {
            s32 w, h, i, j;
            bool8 ok = FALSE;
            if (CellPassable(x, y))
                continue;
            // previous cell (left/up) already part of a solid region -> skip, we want top-left corners
            if (!CellPassable(x - 1, y) || !CellPassable(x, y - 1))
                continue;
            // determine extent (max 2x2)
            w = (!CellPassable(x + 1, y)) ? 2 : 1;
            h = (!CellPassable(x, y + 1)) ? 2 : 1;
            // region must be isolated: border ring passable, and no extra solid outside 2x2
            ok = TRUE;
            for (j = -1; j <= h && ok; j++)
            {
                for (i = -1; i <= w && ok; i++)
                {
                    bool8 inside = (i >= 0 && i < w && j >= 0 && j < h);
                    if (inside)
                        continue;
                    if (!CellPassable(x + i, y + j))
                        ok = FALSE;
                }
            }
            if (!ok)
                continue;
            // reject water / special behaviours / out of map
            {
                u16 m = MetaAt(x, y);
                u8 b = BehaviorOf(m);
                if (m >= NUM_METATILES_TOTAL || MetatileBehavior_IsSurfableWaterOrUnderwater(b) || MetatileBehavior_IsDoor(b)
                 || MetatileBehavior_IsWarpDoor(b))
                    continue;
                if (m == 0)
                    continue;
            }
            {
                struct Stamp *s = &stamps[n];
                bool8 dup = FALSE;
                s->w = w;
                s->h = h;
                for (j = 0; j < 2; j++)
                    for (i = 0; i < 2; i++)
                        s->meta[j][i] = (i < w && j < h && !CellPassable(x + i, y + j)) ? MetaAt(x + i, y + j) : 0xFFFF;
                for (i = 0; i < n; i++)
                    if (memcmp(&stamps[i], s, sizeof(*s)) == 0)
                        dup = TRUE;
                if (!dup)
                    n++;
            }
        }
    }
    return n;
}

#define SYN_BORDER 2
static bool8 AreaFree(s32 cx, s32 cy, s32 w, s32 h)
{
    s32 x, y;
    for (y = cy - 1; y <= cy + h; y++)
        for (x = cx - 1; x <= cx + w; x++)
        {
            if (x < 0 || y < 0 || x >= ARENA_MW || y >= ARENA_MH)
                return FALSE;
            if (gRt->solid[y][x])
                return FALSE;
        }
    return TRUE;
}

static bool8 NearSpawn(s32 x, s32 y)
{
    // spawn zones around (12, 9.9) for the player and (12, 5.8) for the enemy
    if (x >= ARENA_MW / 2 - 4 && x <= ARENA_MW / 2 + 3 && y >= 4 && y <= 11)
        return TRUE;
    return FALSE;
}

static void RtArena_BuildSynthetic(void)
{
    const struct MapLayout *layout = gMapHeader.mapLayout;
    s32 px = gSaveBlock1Ptr->pos.x + MAP_OFFSET;
    s32 py = gSaveBlock1Ptr->pos.y + MAP_OFFSET;
    u16 playerMeta = MetaAt(px, py);
    u8 playerBehav = BehaviorOf(playerMeta);
    u16 floor, decor = 0xFFFF;
    u16 *counts = Alloc(NUM_METATILES_TOTAL * sizeof(u16));
    struct Stamp stamps[MAX_STAMPS];
    u8 nStamps = 0;
    s32 x, y, i;
    bool8 water = FALSE, indoor = (gMapHeader.mapType == MAP_TYPE_INDOOR || gMapHeader.mapType == MAP_TYPE_SECRET_BASE);
    u16 border[4];

    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING | PLAYER_AVATAR_FLAG_UNDERWATER)
     || MetatileBehavior_IsSurfableWaterOrUnderwater(playerBehav))
    {
        floor = playerMeta;
        water = TRUE;
    }
    else if (IsFloorBehavior(playerBehav) && CellPassable(px, py))
    {
        floor = playerMeta;
    }
    else
    {
        floor = counts ? PickFloor(px, py, counts) : 0xFFFF;
        if (floor == 0xFFFF)
            floor = playerMeta;
        if (MetatileBehavior_IsTallGrass(playerBehav) || MetatileBehavior_IsLongGrass(playerBehav)
         || MetatileBehavior_IsAshGrass(playerBehav))
            decor = playerMeta;
    }
    gRt->floorKind = water ? 1 : (decor != 0xFFFF ? 2 : 0);
    if (counts)
        Free(counts);

    // border blocks
    for (i = 0; i < 4; i++)
        border[i] = layout->border ? (layout->border[i] & MAPGRID_METATILE_ID_MASK) : floor;

    nStamps = FindStamps(px, py, stamps);

    for (y = 0; y < ARENA_MH; y++)
    {
        for (x = 0; x < ARENA_MW; x++)
        {
            bool8 edge = (x < SYN_BORDER || y < SYN_BORDER || x >= ARENA_MW - SYN_BORDER || y >= ARENA_MH - SYN_BORDER);
            if (edge)
            {
                gRt->arenaMeta[y][x] = border[(y & 1) * 2 + (x & 1)];
                gRt->solid[y][x] = 1;
            }
            else
            {
                gRt->arenaMeta[y][x] = floor;
                gRt->solid[y][x] = 0;
            }
        }
    }

    // indoor maps: side and bottom walls use the room's most common wall block
    if (indoor)
    {
        u16 *cnt = Alloc(NUM_METATILES_TOTAL * sizeof(u16));
        if (cnt)
        {
            u16 wall = 0xFFFF, best = 0;
            memset(cnt, 0, NUM_METATILES_TOTAL * sizeof(u16));
            for (y = 0; y < layout->height; y++)
            {
                for (x = 0; x < layout->width; x++)
                {
                    u16 raw = layout->map[y * layout->width + x];
                    u16 m = raw & MAPGRID_METATILE_ID_MASK;
                    if (!(raw & MAPGRID_COLLISION_MASK))
                        continue;
                    if (m == border[0] || m == border[1] || m == border[2] || m == border[3] || m == 0)
                        continue;
                    cnt[m]++;
                }
            }
            for (i = 0; i < NUM_METATILES_TOTAL; i++)
                if (cnt[i] > best)
                {
                    best = cnt[i];
                    wall = i;
                }
            Free(cnt);
            if (wall != 0xFFFF)
            {
                for (y = 0; y < ARENA_MH; y++)
                    for (x = 0; x < ARENA_MW; x++)
                        if (x < SYN_BORDER || x >= ARENA_MW - SYN_BORDER || y >= ARENA_MH - SYN_BORDER)
                            gRt->arenaMeta[y][x] = wall;
            }
        }
    }
    // indoor maps: use the room's actual top wall rows
    if (indoor && layout->width > 4 && layout->height > 3)
    {
        s32 mw = layout->width;
        for (y = 0; y < SYN_BORDER; y++)
        {
            for (x = 0; x < ARENA_MW; x++)
            {
                s32 mx = 1 + ((x + (px - MAP_OFFSET)) % (mw - 2));
                u16 m = layout->map[y * mw + mx] & MAPGRID_METATILE_ID_MASK;
                gRt->arenaMeta[y][x] = m;
            }
        }
    }

    // obstacles
    if (nStamps)
    {
        u8 want = 3 + Random() % 3;
        u8 tries = 0;
        if (water)
            want = 2;
        while (want && tries < 60)
        {
            struct Stamp *s = &stamps[Random() % nStamps];
            s32 ox = SYN_BORDER + 2 + Random() % (ARENA_MW - SYN_BORDER * 2 - 4 - s->w + 1);
            s32 oy = SYN_BORDER + 2 + Random() % (ARENA_MH - SYN_BORDER * 2 - 4 - s->h + 1);
            tries++;
            if (NearSpawn(ox, oy) || NearSpawn(ox + s->w - 1, oy + s->h - 1))
                continue;
            if (!AreaFree(ox, oy, s->w, s->h))
                continue;
            for (y = 0; y < s->h; y++)
                for (x = 0; x < s->w; x++)
                    if (s->meta[y][x] != 0xFFFF)
                    {
                        gRt->arenaMeta[oy + y][ox + x] = s->meta[y][x];
                        gRt->solid[oy + y][ox + x] = 1;
                    }
            want--;
        }
    }

    // outdoor maps: a couple of clumps of the border pattern (trees, rocks) as cover
    if (!indoor && !water && border[0] != floor)
    {
        u8 want = 1 + Random() % 2;
        u8 tries = 0;
        while (want && tries < 40)
        {
            s32 ox = SYN_BORDER + 2 + (Random() % ((ARENA_MW - SYN_BORDER * 2 - 6) / 2)) * 2;
            s32 oy = SYN_BORDER + 2 + (Random() % ((ARENA_MH - SYN_BORDER * 2 - 6) / 2)) * 2;
            tries++;
            if (NearSpawn(ox, oy) || NearSpawn(ox + 1, oy + 1) || NearSpawn(ox + 1, oy) || NearSpawn(ox, oy + 1))
                continue;
            if (!AreaFree(ox, oy, 2, 2))
                continue;
            for (y = 0; y < 2; y++)
                for (x = 0; x < 2; x++)
                {
                    gRt->arenaMeta[oy + y][ox + x] = border[((oy + y) & 1) * 2 + ((ox + x) & 1)];
                    gRt->solid[oy + y][ox + x] = 1;
                }
            want--;
        }
    }

    // decorative patches (tall grass etc.), walkable
    if (decor != 0xFFFF)
    {
        u8 patches = 4 + Random() % 3;
        while (patches--)
        {
            s32 cx = SYN_BORDER + 1 + Random() % (ARENA_MW - SYN_BORDER * 2 - 4);
            s32 cy = SYN_BORDER + 1 + Random() % (ARENA_MH - SYN_BORDER * 2 - 4);
            s32 w = 2 + Random() % 3, h = 2 + Random() % 2;
            for (y = cy; y < cy + h && y < ARENA_MH - SYN_BORDER; y++)
                for (x = cx; x < cx + w && x < ARENA_MW - SYN_BORDER; x++)
                    if (!gRt->solid[y][x] && !NearSpawn(x, y))
                        gRt->arenaMeta[y][x] = decor;
        }
    }
}

// ---------------------------------------------------------------------------
// Arena from the real map: a 32x16 metatile window of the map around the
// player, chosen to give the largest open, connected area. Everything is drawn
// exactly as in the overworld; unreachable cells become walls.
// ---------------------------------------------------------------------------
#define GRID_W 80
#define GRID_H 48
#define GF_WALK  1
#define GF_SHOT  2   // blocks projectiles (trees, walls, rocks)
#define GF_OPEN  4   // walkable with all 8 neighbours walkable

static EWRAM_DATA u8 sSpawnCell[2][2][2] = {0}; // [side][index][x/y] in arena cells
static EWRAM_DATA bool8 sSpawnValid = FALSE;

static const u16 *GetMetatileTiles(u16 metatile);
static bool8 HasTopLayer(u16 m)
{
    const u16 *t = GetMetatileTiles(m);
    u8 layer = (GetMetatileAttributesById(m) & METATILE_ATTR_LAYER_MASK) >> METATILE_ATTR_LAYER_SHIFT;
    s32 k;
    // NORMAL: upper tiles go to the top layer; COVERED: both are under sprites
    if (layer == METATILE_LAYER_TYPE_COVERED)
        return FALSE;
    for (k = 4; k < 8; k++)
        if ((t[k] & 0x3FF) != 0)
            return TRUE;
    return FALSE;
}

static bool8 IsLedgeMeta(u16 m)
{
    if (m >= NUM_METATILES_TOTAL)
        return FALSE;
    return (MetaFlags(m) & MF_LEDGE) != 0;
}

// the black void around rooms: a metatile drawn with a single tile everywhere
static bool8 IsBlankMetatile(u16 m)
{
    const u16 *t = GetMetatileTiles(m);
    s32 k;
    if (t == NULL)
        return TRUE;
    for (k = 1; k < 8; k++)
        if ((t[k] & 0x3FF) != (t[0] & 0x3FF))
            return FALSE;
    return TRUE;
}

// single-level maps (most rooms and gyms) mix elevation 0 ("any level") floor with the player's level
static bool8 sZeroElevOk;

// A cell that can be part of an untouched battle field: plain walkable ground of the player's level.
static bool8 CellClean(s32 x, s32 y, bool8 water, u8 elev)
{
    u16 m = MapGridGetMetatileIdAt(x, y);
    u8 f, e;
    if (m >= NUM_METATILES_TOTAL || MapGridGetCollisionAt(x, y))
        return FALSE;
    // outside the map there is only the blank border (rooms, gyms) or a neighbour map of another look
    if (x < MAP_OFFSET || y < MAP_OFFSET || x >= MAP_OFFSET + gMapHeader.mapLayout->width
     || y >= MAP_OFFSET + gMapHeader.mapLayout->height)
    {
        if (gMapHeader.connections == NULL || gMapHeader.connections->count == 0)
            return FALSE;
    }
    e = MapGridGetElevationAt(x, y);
    // same level as the player (multi-level maps use 0/15 for stairs and transitions: those are excluded too)
    if (elev != 0 && elev != 15 && e != elev && !(e == 0 && sZeroElevOk))
        return FALSE;
    f = MetaFlags(m);
    if (water)
        return (f & MF_SURF) && !(f & MF_WATERFALL);
    if (f & MF_LEDGE)
        return TRUE; // ledges are painted with the ground around them beforehand
    return (f & MF_GROUND) != 0;
}

// the same as CellClean / CellFlags, from a cached block and its metatile flags
// (cells outside a map without connections never get here)
static inline __attribute__((always_inline)) bool8 CellCleanOf(u16 blk, u8 f, bool8 water, u8 elev)
{
    u8 e;
    if ((f & MF_INVALID) || (blk & MAPGRID_COLLISION_MASK))
        return FALSE;
    e = blk >> MAPGRID_ELEVATION_SHIFT;
    if (elev != 0 && elev != 15 && e != elev && !(e == 0 && sZeroElevOk))
        return FALSE;
    if (water)
        return (f & MF_SURF) && !(f & MF_WATERFALL);
    if (f & MF_LEDGE)
        return TRUE;
    return (f & MF_GROUND) != 0;
}

static inline __attribute__((always_inline)) u8 CellFlagsOf(u16 blk, u8 f, bool8 water)
{
    bool8 coll = (blk & MAPGRID_COLLISION_MASK) != 0;
    bool8 isWater = (f & (MF_SURF | MF_WATERFALL)) != 0;
    if (f & MF_INVALID)
        return GF_SHOT;
    if (water)
    {
        if (isWater && !(f & MF_WATERFALL) && !coll)
            return GF_WALK;
        return coll ? GF_SHOT : 0;
    }
    if (coll)
        return isWater ? 0 : GF_SHOT;
    if (isWater)
        return 0;
    return GF_WALK;
}

static u8 CellFlags(s32 x, s32 y, bool8 water)
{
    u16 m = MapGridGetMetatileIdAt(x, y);
    u8 f, coll = MapGridGetCollisionAt(x, y);
    bool8 isWater, ledge;
    if (m >= NUM_METATILES_TOTAL)
        return GF_SHOT;
    f = MetaFlags(m);
    isWater = (f & (MF_SURF | MF_WATERFALL)) != 0;
    ledge = (f & MF_LEDGE) != 0;
    if (water)
    {
        if (isWater && !(f & MF_WATERFALL) && coll == 0)
            return GF_WALK;
        return coll ? GF_SHOT : 0;
    }
    if (coll)
        return isWater ? 0 : GF_SHOT;
    if (isWater)
        return 0;
    (void)ledge; // ledges are low: in battle they can be crossed both ways
    return GF_WALK;
}

// Breadth-first search of the walkable component containing (sx, sy) inside the window.
// Marks comp[y][x] = 1 and returns the size.
static u16 FloodWindow(const u8 *grid, s32 ox, s32 oy, s32 sx, s32 sy, u8 *comp, u16 *queue)
{
    u16 head = 0, tail = 0, n = 0;
    memset(comp, 0, ARENA_MW * ARENA_MH);
    if (!(grid[(oy + sy) * GRID_W + ox + sx] & GF_WALK))
        return 0;
    comp[sy * ARENA_MW + sx] = 1;
    queue[tail++] = sy * ARENA_MW + sx;
    while (head < tail)
    {
        u16 c = queue[head++];
        s32 cx = c % ARENA_MW, cy = c / ARENA_MW, k;
        n++;
        for (k = 0; k < 4; k++)
        {
            s32 nx = cx + (k == 0) - (k == 1), ny = cy + (k == 2) - (k == 3);
            if (nx < 0 || ny < 0 || nx >= ARENA_MW || ny >= ARENA_MH)
                continue;
            if (comp[ny * ARENA_MW + nx] || !(grid[(oy + ny) * GRID_W + ox + nx] & GF_WALK))
                continue;
            comp[ny * ARENA_MW + nx] = 1;
            queue[tail++] = ny * ARENA_MW + nx;
        }
    }
    return n;
}

// Largest component of the window (seeded from open cells). Leaves it in comp.
static u16 BestComponent(const u8 *grid, s32 ox, s32 oy, u8 *comp, u8 *tmp, u16 *queue)
{
    s32 x, y;
    u16 best = 0;
    u8 *seen = tmp + ARENA_MW * ARENA_MH;
    memset(seen, 0, ARENA_MW * ARENA_MH);
    for (y = 1; y < ARENA_MH - 1; y++)
    {
        for (x = 1; x < ARENA_MW - 1; x++)
        {
            u16 n;
            s32 i;
            if (seen[y * ARENA_MW + x] || !(grid[(oy + y) * GRID_W + ox + x] & GF_OPEN))
                continue;
            n = FloodWindow(grid, ox, oy, x, y, tmp, queue);
            for (i = 0; i < ARENA_MW * ARENA_MH; i++)
                if (tmp[i])
                    seen[i] = 1;
            if (n > best)
            {
                best = n;
                memcpy(comp, tmp, ARENA_MW * ARENA_MH);
            }
        }
    }
    return best;
}

// Groups the blocked cells of a window (everything outside the main walkable area) into
// 8-connected groups. Groups that do not touch the window edge are "islands" (rocks, signs,
// lone trees, inner walls...) and get cleared from the arena. For border groups, counts how
// many of their cells reach deep into the arena ("intrusion"), which makes a window worse.
#define sGroup (gRt->scr.group) // 0 = walkable, 1 = border group, 2 = island
static void ClassifyBlocked(const u8 *comp, u16 *queue, u16 *islands, u16 *intrusion)
{
    s32 i;
    u8 *seen = gRt->scr.seen;
    memset(seen, 0, ARENA_MH * ARENA_MW);
    *islands = 0;
    *intrusion = 0;
    for (i = 0; i < ARENA_MH * ARENA_MW; i++)
    {
        u16 head = 0, tail = 0, n = 0, deep = 0;
        bool8 border = FALSE;
        s32 k;
        sGroup[i] = 0;
        if (comp[i] || seen[i])
            continue;
        seen[i] = 1;
        queue[tail++] = i;
        while (head < tail)
        {
            u16 c = queue[head++];
            s32 cx = c % ARENA_MW, cy = c / ARENA_MW, d;
            n++;
            if (cx == 0 || cy == 0 || cx == ARENA_MW - 1 || cy == ARENA_MH - 1)
                border = TRUE;
            d = cx;
            if (cy < d) d = cy;
            if (ARENA_MW - 1 - cx < d) d = ARENA_MW - 1 - cx;
            if (ARENA_MH - 1 - cy < d) d = ARENA_MH - 1 - cy;
            if (d >= 3)
                deep++;
            for (k = 0; k < 8; k++)
            {
                static const s8 sDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
                static const s8 sDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                s32 nx = cx + sDx[k], ny = cy + sDy[k], ni;
                if (nx < 0 || ny < 0 || nx >= ARENA_MW || ny >= ARENA_MH)
                    continue;
                ni = ny * ARENA_MW + nx;
                if (comp[ni] || seen[ni])
                    continue;
                seen[ni] = 1;
                queue[tail++] = ni;
            }
        }
        // mark the group
        for (k = 0; k < tail; k++)
            sGroup[queue[k]] = border ? 1 : 2;
        if (border)
            *intrusion += deep;
        else
            *islands += n;
    }
}

static bool8 PickSpawn(const u8 *comp, const u8 *open, s32 tx, s32 ty, s32 avoidX, s32 avoidY, s32 minDist, u8 *outX, u8 *outY)
{
    s32 x, y, best = 0x7FFFFFFF;
    bool8 found = FALSE;
    for (y = 1; y < ARENA_MH - 1; y++)
    {
        for (x = 1; x < ARENA_MW - 1; x++)
        {
            s32 d, dx, dy;
            if (!comp[y * ARENA_MW + x])
                continue;
            dx = x - tx;
            dy = y - ty;
            d = dx * dx + dy * dy;
            if (!open[y * ARENA_MW + x])
                d += 40;
            if (avoidX >= 0)
            {
                s32 ax = x - avoidX, ay = y - avoidY;
                if (ax * ax + ay * ay < minDist * minDist)
                    d += 400;
            }
            if (d < best)
            {
                best = d;
                *outX = x;
                *outY = y;
                found = TRUE;
            }
        }
    }
    return found;
}

static void RtArena_ResetPaths(void);
static void MarkBreakables(void);
void RtArena_ClampCamera(s32 *tx, s32 *ty)
{
    // visible play field is screen y in [HUD_TOP, DISPLAY_HEIGHT - HUD_BOTTOM)
    s32 x0 = gRt->viewX0, x1 = gRt->viewX1, y0 = gRt->viewY0 - HUD_TOP, y1 = gRt->viewY1 - (DISPLAY_HEIGHT - HUD_BOTTOM);
    if (x1 <= x0)
    {
        x0 = 0;
        x1 = ARENA_W;
    }
    if (x1 - x0 <= DISPLAY_WIDTH)
        *tx = (x0 + x1 - DISPLAY_WIDTH) / 2;
    else if (*tx < x0)
        *tx = x0;
    else if (*tx > x1 - DISPLAY_WIDTH)
        *tx = x1 - DISPLAY_WIDTH;
    if (y1 <= y0)
        *ty = (y0 + y1) / 2;
    else if (*ty < y0)
        *ty = y0;
    else if (*ty > y1)
        *ty = y1;
}

void RtArena_Build(void)
{
    s32 px = gSaveBlock1Ptr->pos.x + MAP_OFFSET;
    s32 py = gSaveBlock1Ptr->pos.y + MAP_OFFSET;
    s32 gx0 = px - GRID_W / 2, gy0 = py - GRID_H / 2;
    u16 playerMeta = MetaAt(px, py);
    u8 playerBehav = BehaviorOf(playerMeta);
    bool8 water = TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING) || MetatileBehavior_IsSurfableWaterOrUnderwater(playerBehav);
    u8 *grid = Alloc(GRID_W * GRID_H);
    u16 *sum = Alloc((GRID_W + 1) * (GRID_H + 1) * sizeof(u16));
    u8 *comp = Alloc(ARENA_MW * ARENA_MH * 4);
    u16 *queue = Alloc(ARENA_MW * ARENA_MH * sizeof(u16));
    s32 x, y, i;
    s32 bestOx = 0, bestOy = 0, bestScore = -0x7FFFFFFF;
    u16 bestComp = 0;
    struct { s16 ox, oy; s32 score; } cand[8];
    u8 nCand = 0;
    s32 cleanX = 0, cleanY = 0, cleanW = 0, cleanH = 0;

    sSpawnValid = FALSE;
    RtMapCache_Begin(gx0, gy0, GRID_W, GRID_H);
    RtArena_ResetPaths();
    gRt->viewX0 = 0;
    gRt->viewY0 = 0;
    gRt->viewX1 = ARENA_W;
    gRt->viewY1 = ARENA_H;
    gRt->playX0 = 16;
    gRt->playY0 = 16;
    gRt->playX1 = ARENA_W - 16;
    gRt->playY1 = ARENA_H - 16;
    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_UNDERWATER))
        water = FALSE; // underwater maps are walked normally
    if (!grid || !sum || !comp || !queue)
        goto fallback;

    // 1. classify cells around the player
    if (sBlkCache != NULL)
    {
        for (i = 0; i < GRID_W * GRID_H; i++)
            grid[i] = CellFlagsOf(sBlkCache[i], sCellMF[i], water);
    }
    else
    {
        for (y = 0; y < GRID_H; y++)
            for (x = 0; x < GRID_W; x++)
                grid[y * GRID_W + x] = CellFlags(gx0 + x, gy0 + y, water);
    }
    for (y = 1; y < GRID_H - 1; y++)
    {
        const u8 *r0 = &grid[(y - 1) * GRID_W], *r1 = &grid[y * GRID_W], *r2 = &grid[(y + 1) * GRID_W];
        u8 *out = &grid[y * GRID_W];
        // column x is walkable in all three rows; open = three such columns in a row
        u8 c0 = r0[0] & r1[0] & r2[0] & GF_WALK, c1 = r0[1] & r1[1] & r2[1] & GF_WALK;
        for (x = 1; x < GRID_W - 1; x++)
        {
            u8 c2 = r0[x + 1] & r1[x + 1] & r2[x + 1] & GF_WALK;
            if (c0 & c1 & c2)
                out[x] |= GF_OPEN;
            c0 = c1;
            c1 = c2;
        }
    }
    // is this a single-level place? (one elevation besides 0/15 among the walkable cells around)
    {
        u16 cnt[16];
        s32 k, top = 0, second = 0;
        memset(cnt, 0, sizeof(cnt));
        if (sBlkCache != NULL)
        {
            for (i = 0; i < GRID_W * GRID_H; i++)
                if (!(sBlkCache[i] & MAPGRID_COLLISION_MASK))
                    cnt[sBlkCache[i] >> MAPGRID_ELEVATION_SHIFT]++;
        }
        else
        {
            for (y = 0; y < GRID_H; y++)
                for (x = 0; x < GRID_W; x++)
                    if (MapGridGetCollisionAt(gx0 + x, gy0 + y) == 0)
                        cnt[MapGridGetElevationAt(gx0 + x, gy0 + y)]++;
        }
        for (k = 1; k < 15; k++)
        {
            if (cnt[k] > top)
            {
                second = top;
                top = cnt[k];
            }
            else if (cnt[k] > second)
                second = cnt[k];
        }
        // (outdoors, elevation 0 is used by stairs between levels: never part of a field there)
        sZeroElevOk = (second * 12 < top) && gMapHeader.mapType != MAP_TYPE_TOWN && gMapHeader.mapType != MAP_TYPE_CITY
                   && gMapHeader.mapType != MAP_TYPE_ROUTE && gMapHeader.mapType != MAP_TYPE_OCEAN_ROUTE;
    }
    // 3b. the battle field: always FIELD_W x FIELD_H metatiles. It goes where the least has to be cleared
    // (everything that is not plain ground of the player's level counts, the void outside a room the most),
    // close to the player. It is aligned like the map's border blocks so the frame of border blocks
    // (trees, rock) drawn around it later lines up.
    {
        u8 pe = MapGridGetElevationAt(px, py);
        bool8 noConn = (gMapHeader.connections == NULL || gMapHeader.connections->count == 0);
        const struct MapLayout *layout = gMapHeader.mapLayout;
        s32 mapX1 = MAP_OFFSET + layout->width, mapY1 = MAP_OFFSET + layout->height;
        s32 bestC = 0x7FFFFFFF;
        for (x = 0; x <= GRID_W; x++)
            sum[x] = 0;
        for (y = 1; y <= GRID_H; y++)
        {
            s32 cy = gy0 + y - 1, rowSum = 0;
            bool8 rowOut = (cy < MAP_OFFSET || cy >= mapY1);
            u16 *sRow = &sum[y * (GRID_W + 1)];
            const u16 *sUp = sRow - (GRID_W + 1);
            sRow[0] = 0;
            for (x = 1; x <= GRID_W; x++)
            {
                s32 cx = gx0 + x - 1, c = 0;
                bool8 outside = rowOut || cx < MAP_OFFSET || cx >= mapX1;
                if (outside && noConn)
                    c = 6; // (outside the map with connections: CellClean looks at the neighbour map)
                else if (sBlkCache != NULL ? !CellCleanOf(sBlkCache[(y - 1) * GRID_W + x - 1], sCellMF[(y - 1) * GRID_W + x - 1], water, pe)
                                           : !CellClean(cx, cy, water, pe))
                    c = 2;
                rowSum += c;
                sRow[x] = sUp[x] + rowSum;
            }
        }
        for (y = (ARENA_MH - FIELD_H) / 2; y + FIELD_H + (ARENA_MH - FIELD_H) / 2 <= GRID_H; y++)
        {
            if (((gy0 + y) & 1) == 0)
                continue;
            for (x = (ARENA_MW - FIELD_W) / 2; x + FIELD_W + (ARENA_MW - FIELD_W) / 2 <= GRID_W; x++)
            {
                s32 c, dcx, dcy;
                if (((gx0 + x) & 1) == 0)
                    continue;
                c = sum[(y + FIELD_H) * (GRID_W + 1) + x + FIELD_W] - sum[y * (GRID_W + 1) + x + FIELD_W]
                  - sum[(y + FIELD_H) * (GRID_W + 1) + x] + sum[y * (GRID_W + 1) + x];
                dcx = gx0 + x + FIELD_W / 2 - px;
                dcy = gy0 + y + FIELD_H / 2 - py;
                if (dcx < 0) dcx = -dcx;
                if (dcy < 0) dcy = -dcy;
                c = c * 4 + dcx + dcy;
                if (c < bestC)
                {
                    bestC = c;
                    cleanX = x;
                    cleanY = y;
                    cleanW = FIELD_W;
                    cleanH = FIELD_H;
                }
            }
        }
    }
    if (!cleanW)
    {
        // 2. prefix sums of open cells
        for (y = 0; y <= GRID_H; y++)
            for (x = 0; x <= GRID_W; x++)
            {
                if (x == 0 || y == 0)
                    sum[y * (GRID_W + 1) + x] = 0;
                else
                    sum[y * (GRID_W + 1) + x] = ((grid[(y - 1) * GRID_W + x - 1] & GF_OPEN) ? 1 : 0)
                        + sum[(y - 1) * (GRID_W + 1) + x] + sum[y * (GRID_W + 1) + x - 1] - sum[(y - 1) * (GRID_W + 1) + x - 1];
            }
        // 3. coarse ranking of all window positions (open area, close to the player)
        for (y = 0; y <= GRID_H - ARENA_MH; y++)
        {
            for (x = 0; x <= GRID_W - ARENA_MW; x++)
            {
                s32 open = sum[(y + ARENA_MH) * (GRID_W + 1) + x + ARENA_MW] - sum[y * (GRID_W + 1) + x + ARENA_MW]
                         - sum[(y + ARENA_MH) * (GRID_W + 1) + x] + sum[y * (GRID_W + 1) + x];
                s32 dcx = (gx0 + x + ARENA_MW / 2) - px, dcy = (gy0 + y + ARENA_MH / 2) - py;
                s32 score, k;
                if (dcx < 0) dcx = -dcx;
                if (dcy < 0) dcy = -dcy;
                score = open * 4 - (dcx + dcy * 2) * 3;
                // the player's own spot should normally be part of the arena
                if (px < gx0 + x + 1 || px >= gx0 + x + ARENA_MW - 1 || py < gy0 + y + 1 || py >= gy0 + y + ARENA_MH - 1)
                    score -= 120;
                // keep the best few, spread apart
                for (k = 0; k < nCand; k++)
                    if ((cand[k].ox - x) * (cand[k].ox - x) + (cand[k].oy - y) * (cand[k].oy - y) < 9)
                        break;
                if (k < nCand)
                {
                    if (score > cand[k].score)
                    {
                        cand[k].ox = x;
                        cand[k].oy = y;
                        cand[k].score = score;
                    }
                    continue;
                }
                if (nCand < ARRAY_COUNT(cand))
                {
                    cand[nCand].ox = x;
                    cand[nCand].oy = y;
                    cand[nCand].score = score;
                    nCand++;
                }
                else
                {
                    s32 worst = 0;
                    for (k = 1; k < nCand; k++)
                        if (cand[k].score < cand[worst].score)
                            worst = k;
                    if (score > cand[worst].score)
                    {
                        cand[worst].ox = x;
                        cand[worst].oy = y;
                        cand[worst].score = score;
                    }
                }
            }
        }
    }
    if (cleanW)
    {
        // the window: the clean rectangle, centred
        s32 ox = cleanX - (ARENA_MW - cleanW) / 2, oy = cleanY - (ARENA_MH - cleanH) / 2;
        if (ox > cleanX - 1) ox = cleanX - 1;
        if (oy > cleanY - 1) oy = cleanY - 1;
        if (ox + ARENA_MW < cleanX + cleanW + 1) ox = cleanX + cleanW + 1 - ARENA_MW;
        if (oy + ARENA_MH < cleanY + cleanH + 1) oy = cleanY + cleanH + 1 - ARENA_MH;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
        if (ox > GRID_W - ARENA_MW) ox = GRID_W - ARENA_MW;
        if (oy > GRID_H - ARENA_MH) oy = GRID_H - ARENA_MH;
        nCand = 1;
        cand[0].ox = ox;
        cand[0].oy = oy;
        cand[0].score = 0;
    }
    // 4. exact check of the candidates: size of the largest connected open area
    for (i = 0; i < nCand; i++)
    {
        u16 n = BestComponent(grid, cand[i].ox, cand[i].oy, comp, comp + ARENA_MW * ARENA_MH, queue);
        s32 dcx = (gx0 + cand[i].ox + ARENA_MW / 2) - px, dcy = (gy0 + cand[i].oy + ARENA_MH / 2) - py;
        s32 score;
        if (dcx < 0) dcx = -dcx;
        if (dcy < 0) dcy = -dcy;
        {
            // cleared islands become playable area; walls reaching deep into the arena are bad
            u16 isl, intr;
            ClassifyBlocked(comp, queue, &isl, &intr);
            n += isl;
            score = n * 4 - intr * 8 - (dcx + dcy * 2) * 3;
        }
        if (px < gx0 + cand[i].ox + 1 || px >= gx0 + cand[i].ox + ARENA_MW - 1 || py < gy0 + cand[i].oy + 1 || py >= gy0 + cand[i].oy + ARENA_MH - 1)
            score -= 120;
        if (score > bestScore)
        {
            bestScore = score;
            bestOx = cand[i].ox;
            bestOy = cand[i].oy;
            bestComp = n;
        }
    }
    if (bestComp < 20 && !cleanW)
        goto fallback;

    // 5. build the arena from the chosen window
    BestComponent(grid, bestOx, bestOy, comp, comp + ARENA_MW * ARENA_MH, queue);
    for (y = 0; y < ARENA_MH; y++)
    {
        for (x = 0; x < ARENA_MW; x++)
        {
            u8 g = grid[(bestOy + y) * GRID_W + bestOx + x];
            gRt->arenaMeta[y][x] = MetaAt(gx0 + bestOx + x, gy0 + bestOy + y);
            gRt->solid[y][x] = (comp[y * ARENA_MW + x] ? 0 : RT_SOLID_WALK) | ((g & GF_SHOT) ? RT_SOLID_SHOT : 0);
        }
    }
    // ledges are one-way barriers in the overworld; in battle they would only confuse, so they are
    // painted over with the ground around them
    {
        u16 common = 0xFFFF;
        u16 cnt[16], ids[16];
        u8 nIds = 0;
        // most common plain walkable ground of the window, as a last resort
        for (y = 0; y < ARENA_MH; y++)
            for (x = 0; x < ARENA_MW; x++)
            {
                u16 m = gRt->arenaMeta[y][x];
                s32 k;
                if (gRt->solid[y][x] || IsLedgeMeta(m) || BehaviorOf(m) != MB_NORMAL)
                    continue;
                for (k = 0; k < nIds; k++)
                    if (ids[k] == m)
                        break;
                if (k < nIds)
                    cnt[k]++;
                else if (nIds < 16)
                {
                    ids[nIds] = m;
                    cnt[nIds++] = 1;
                }
            }
        for (i = 0; i < nIds; i++)
            if (common == 0xFFFF || cnt[i] > cnt[common])
                common = i;
        common = (common == 0xFFFF) ? 0xFFFF : ids[common];
        u8 (*ledge)[ARENA_MW] = gRt->scr.ledge;
        for (y = 0; y < ARENA_MH; y++)
            for (x = 0; x < ARENA_MW; x++)
                ledge[y][x] = IsLedgeMeta(gRt->arenaMeta[y][x]);
        // the small blocked end caps of a ledge: tiny groups of blocked cells touching a ledge
        for (y = 0; y < ARENA_MH; y++)
            for (x = 0; x < ARENA_MW; x++)
            {
                u8 gx[4], gy[4], gn = 0, head = 0;
                bool8 touches = FALSE, tooBig = FALSE;
                s32 k;
                if (ledge[y][x] || (grid[(bestOy + y) * GRID_W + bestOx + x] & GF_WALK))
                    continue;
                // flood the blocked group (max 3 cells)
                gx[gn] = x; gy[gn] = y; gn++;
                while (head < gn && !tooBig)
                {
                    s32 cx = gx[head], cy = gy[head];
                    head++;
                    for (k = 0; k < 8; k++)
                    {
                        static const s8 sDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
                        static const s8 sDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                        s32 nx = cx + sDx[k], ny = cy + sDy[k], j;
                        if (nx < 0 || ny < 0 || nx >= ARENA_MW || ny >= ARENA_MH)
                        {
                            if (k < 4)
                                tooBig = TRUE;
                            continue;
                        }
                        if (ledge[ny][nx] == 1)
                        {
                            touches = TRUE;
                            continue;
                        }
                        if (k >= 4 || (grid[(bestOy + ny) * GRID_W + bestOx + nx] & GF_WALK))
                            continue;
                        for (j = 0; j < gn; j++)
                            if (gx[j] == nx && gy[j] == ny)
                                break;
                        if (j < gn)
                            continue;
                        if (gn >= 3)
                        {
                            tooBig = TRUE;
                            break;
                        }
                        gx[gn] = nx; gy[gn] = ny; gn++;
                    }
                }
                if (!touches || tooBig)
                    continue;
                for (k = 0; k < gn; k++)
                    ledge[gy[k]][gx[k]] = 2;
            }
        for (y = 0; y < ARENA_MH; y++)
        {
            for (x = 0; x < ARENA_MW; x++)
            {
                u16 repl = 0xFFFF;
                s32 k;
                if (!ledge[y][x])
                    continue;
                if (ledge[y][x] == 2 && x > 0 && y > 0 && x < ARENA_MW - 1 && y < ARENA_MH - 1)
                    gRt->solid[y][x] = 0;
                // the window's most common plain ground, else the ground next to the ledge
                repl = common;
                for (k = 0; k < 8 && repl == 0xFFFF; k++)
                {
                    static const s8 sDx[8] = {0, 0, -1, 1, -1, 1, -1, 1};
                    static const s8 sDy[8] = {-1, 1, 0, 0, -1, -1, 1, 1};
                    s32 nx = x + sDx[k], ny = y + sDy[k];
                    u16 m;
                    if (nx < 0 || ny < 0 || nx >= ARENA_MW || ny >= ARENA_MH)
                        continue;
                    m = gRt->arenaMeta[ny][nx];
                    if (!ledge[ny][nx] && BehaviorOf(m) == MB_NORMAL && MapGridGetCollisionAt(gx0 + bestOx + nx, gy0 + bestOy + ny) == 0
                     && (GetMetatileAttributesById(m) & METATILE_ATTR_LAYER_MASK) == (METATILE_LAYER_TYPE_NORMAL << METATILE_ATTR_LAYER_SHIFT))
                        repl = m;
                }
                if (repl == 0xFFFF)
                    repl = common;
                if (repl != 0xFFFF)
                    gRt->arenaMeta[y][x] = repl;
            }
        }
    }
    // 5b. the playable field: a rectangle of the window that is all plain ground of the player's level.
    // Non-ground cells inside it ("dirt": rocks, signs, ladders, ledges, other levels...) are painted with
    // the field's ground. Dirt is grouped into 8-connected objects: small objects are erased entirely
    // (also their part outside the field, so nothing is left cut in half); big ones (cliffs, forests,
    // buildings) make a rectangle very expensive, so the field avoids cutting through them.
    // Everything outside the field is scenery: dimmed on screen and not walkable.
    {
        u16 floorMeta = 0xFFFF;
        u32 *cost = gRt->scr.cost;
        u8 (*clean)[ARENA_MW] = gRt->scr.clean;
        u8 (*objId)[ARENA_MW] = gRt->scr.objId;   // 0 clean, else object index
        u16 *objSize = gRt->scr.objSize;
        u8 *objEdge = gRt->scr.objEdge;
        u8 *objErase = gRt->scr.objErase;
        u16 *objFill = gRt->scr.objFill;
        u8 *objVote = gRt->scr.objVote;
        u8 pe = MapGridGetElevationAt(px, py);
        s32 w, h, bx0 = 1, by0 = 1, bw = ARENA_MW - 2, bh = ARENA_MH - 2, bestS = -0x7FFFFFFF;
        u16 nObj = 0;
        u16 *q = queue;
        u16 *freq;
        bool8 indoorField;

        for (y = 0; y < ARENA_MH; y++)
            for (x = 0; x < ARENA_MW; x++)
            {
                clean[y][x] = CellClean(gx0 + bestOx + x, gy0 + bestOy + y, water, pe);
                objId[y][x] = 0;
            }
        // the field's ground: the most common clean metatile of the window (no tall grass on land)
        {
            freq = (u16 *)gRt->scr.cost; // temporary use (holds 1024 u16), rebuilt below
            u16 bestN = 0;
            memset(freq, 0, NUM_METATILES_TOTAL * sizeof(u16));
            for (y = 0; y < ARENA_MH; y++)
                for (x = 0; x < ARENA_MW; x++)
                {
                    u16 m = gRt->arenaMeta[y][x];
                    u8 b;
                    if (!clean[y][x] || m >= NUM_METATILES_TOTAL)
                        continue;
                    b = BehaviorOf(m);
                    if (!water && (MetatileBehavior_IsTallGrass(b) || MetatileBehavior_IsLongGrass(b) || HasTopLayer(m)))
                        continue;
                    if (++freq[m] > bestN)
                    {
                        bestN = freq[m];
                        floorMeta = m;
                    }
                }
            if (floorMeta == 0xFFFF)
                floorMeta = playerMeta;
        }
        indoorField = !water && (gMapHeader.mapType == MAP_TYPE_INDOOR || gMapHeader.mapType == MAP_TYPE_SECRET_BASE
                                 || BehaviorOf(floorMeta) == MB_INDOOR_ENCOUNTER);
        // classify: 0 = plain ground (kept as it is: grass, paths, sand...), 1 = ground that needs
        // painting (under a tree crown or a roof, ledges), 2 = obstacle
        for (y = 0; y < ARENA_MH; y++)
            for (x = 0; x < ARENA_MW; x++)
            {
                u16 m = gRt->arenaMeta[y][x];
                if (!clean[y][x])
                    clean[y][x] = 2;
                else if (HasTopLayer(m) || IsLedgeMeta(MetaAt(gx0 + bestOx + x, gy0 + bestOy + y)))
                    clean[y][x] = 1;
                else if (indoorField && m != floorMeta)
                    clean[y][x] = 1; // rooms: one plain floor (rugs, stools and the like go with the furniture)
                else if (water && m != floorMeta && MetatileBehavior_IsSurfableWaterOrUnderwater(BehaviorOf(m)))
                    clean[y][x] = 1; // at sea: one water (no patchwork of currents and depths)
                else if (m != floorMeta && freq[m] <= 16
                      && (water || !(MetatileBehavior_IsTallGrass(BehaviorOf(m)) || MetatileBehavior_IsLongGrass(BehaviorOf(m)))))
                    clean[y][x] = 1; // rare pieces (steps, odd stones, a lone plank): painted when in the field
                else
                    clean[y][x] = 0;
            }
        // decorations: tiny groups (up to 4 cells) of a rare walkable ground that is neither the field's ground
        // nor grass, like a chair, a lone plank or steps cut in a cliff, are painted too
        {
            u8 (*vis)[ARENA_MW] = gRt->scr.erased; // scratch, rebuilt below
            memset(vis, 0, sizeof(gRt->scr.erased));
            for (y = 0; y < ARENA_MH; y++)
                for (x = 0; x < ARENA_MW; x++)
                {
                    u16 head = 0, tail = 0, k;
                    u16 m = gRt->arenaMeta[y][x];
                    if (vis[y][x] || clean[y][x] != 0 || m == floorMeta
                     || (!water && (MetatileBehavior_IsTallGrass(BehaviorOf(m)) || MetatileBehavior_IsLongGrass(BehaviorOf(m)))))
                        continue;
                    vis[y][x] = 1;
                    q[tail++] = y * ARENA_MW + x;
                    while (head < tail)
                    {
                        u16 c = q[head++];
                        s32 cx = c % ARENA_MW, cy = c / ARENA_MW;
                        for (k = 0; k < 4; k++)
                        {
                            s32 nx = cx + (k == 0) - (k == 1), ny = cy + (k == 2) - (k == 3);
                            u16 nm;
                            if (nx < 0 || ny < 0 || nx >= ARENA_MW || ny >= ARENA_MH || vis[ny][nx] || clean[ny][nx] != 0)
                                continue;
                            nm = gRt->arenaMeta[ny][nx];
                            if (nm != m)
                                continue;
                            vis[ny][nx] = 1;
                            q[tail++] = ny * ARENA_MW + nx;
                        }
                    }
                    bool8 squeezed = TRUE;
                    for (k = 0; k < tail; k++)
                    {
                        s32 cx = q[k] % ARENA_MW, cy = q[k] / ARENA_MW;
                        bool8 h = cx > 0 && cx < ARENA_MW - 1 && clean[cy][cx - 1] == 2 && clean[cy][cx + 1] == 2;
                        bool8 v = cy > 0 && cy < ARENA_MH - 1 && clean[cy - 1][cx] == 2 && clean[cy + 1][cx] == 2;
                        if (!h && !v)
                            squeezed = FALSE;
                    }
                    if (tail <= 4 && (freq[m] <= 6 || squeezed))
                        for (k = 0; k < tail; k++)
                            clean[q[k] / ARENA_MW][q[k] % ARENA_MW] = 1;
                }
        }
        // label everything that is not field ground into 8-connected objects
        for (y = 0; y < ARENA_MH; y++)
            for (x = 0; x < ARENA_MW; x++)
            {
                u16 head = 0, tail = 0, id;
                if (clean[y][x] == 0 || objId[y][x] || nObj >= 255)
                    continue;
                id = ++nObj;
                objSize[id] = 0;
                objEdge[id] = 0;       // bit0: touches the window edge, bit1: contains an obstacle
                objId[y][x] = id;
                q[tail++] = y * ARENA_MW + x;
                while (head < tail)
                {
                    u16 c = q[head++];
                    s32 cx = c % ARENA_MW, cy = c / ARENA_MW, k;
                    objSize[id]++;
                    if (cx == 0 || cy == 0 || cx == ARENA_MW - 1 || cy == ARENA_MH - 1)
                        objEdge[id] |= 1;
                    if (clean[cy][cx] == 2)
                        objEdge[id] |= 2;
                    for (k = 0; k < 8; k++)
                    {
                        static const s8 sDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
                        static const s8 sDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                        s32 nx = cx + sDx[k], ny = cy + sDy[k];
                        if (nx < 0 || ny < 0 || nx >= ARENA_MW || ny >= ARENA_MH || clean[ny][nx] == 0 || objId[ny][nx])
                            continue;
                        objId[ny][nx] = id;
                        q[tail++] = ny * ARENA_MW + nx;
                    }
                }
            }
        // small things are erased completely; other ground is painted only inside the field;
        // big obstacles (cliffs, buildings, forests) must stay out of the field
        for (i = 1; i <= nObj; i++)
        {
            objErase[i] = (objSize[i] <= 9 && !(objEdge[i] & 1));
            objFill[i] = floorMeta;
            objVote[i] = 0;
        }
        // what replaces an object: the ground most found around it (majority vote)
        for (y = 0; y < ARENA_MH; y++)
            for (x = 0; x < ARENA_MW; x++)
            {
                s32 k;
                u16 m = gRt->arenaMeta[y][x];
                if (clean[y][x] != 0 || (!water && (MetatileBehavior_IsTallGrass(BehaviorOf(m)) || MetatileBehavior_IsLongGrass(BehaviorOf(m)))))
                    continue;
                for (k = 0; k < 8; k++)
                {
                    static const s8 sDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
                    static const s8 sDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                    s32 nx = x + sDx[k], ny = y + sDy[k];
                    u8 id;
                    if (nx < 0 || ny < 0 || nx >= ARENA_MW || ny >= ARENA_MH || !(id = objId[ny][nx]))
                        continue;
                    if (objVote[id] == 0)
                    {
                        objFill[id] = m;
                        objVote[id] = 1;
                    }
                    else if (objFill[id] == m)
                    {
                        if (objVote[id] < 255)
                            objVote[id]++;
                    }
                    else
                        objVote[id]--;
                }
            }
        for (y = 0; y <= ARENA_MH; y++)
            for (x = 0; x <= ARENA_MW; x++)
            {
                if (x == 0 || y == 0)
                    cost[y * (ARENA_MW + 1) + x] = 0;
                else
                {
                    u8 id = objId[y - 1][x - 1];
                    u16 c0 = 0;
                    if (id)
                        c0 = objErase[id] ? 1 : ((clean[y - 1][x - 1] == 2) ? 400 : (indoorField ? 1 : 3));
                    cost[y * (ARENA_MW + 1) + x] = c0 + cost[(y - 1) * (ARENA_MW + 1) + x] + cost[y * (ARENA_MW + 1) + x - 1]
                                                  - cost[(y - 1) * (ARENA_MW + 1) + x - 1];
                }
            }
        if (cleanW)
        {
            bx0 = cleanX - bestOx;
            by0 = cleanY - bestOy;
            bw = cleanW;
            bh = cleanH;
        }
        for (y = 0; y < ARENA_MH; y++)
        {
            for (x = 0; x < ARENA_MW; x++)
            {
                bool8 inside = x >= bx0 && x < bx0 + bw && y >= by0 && y < by0 + bh;
                u8 id = objId[y][x];
                if (id && (inside || objErase[id]))
                {
                    // erase: small objects completely, big ones only where they enter the field
                    bool8 touchesField = inside;
                    if (!inside)
                    {
                        // erase a small object only if part of it is inside the field
                        s32 yy, xx;
                        for (yy = by0; yy < by0 + bh && !touchesField; yy++)
                            for (xx = bx0; xx < bx0 + bw && !touchesField; xx++)
                                if (objId[yy][xx] == id)
                                    touchesField = TRUE;
                    }
                    if (touchesField)
                        gRt->arenaMeta[y][x] = objFill[id];
                }
                // ledges inside the field (and what the ledge pass put in their place) become field ground
                if (inside && IsLedgeMeta(MetaAt(gx0 + bestOx + x, gy0 + bestOy + y)))
                    gRt->arenaMeta[y][x] = id ? objFill[id] : floorMeta;
                gRt->solid[y][x] = inside ? 0 : (RT_SOLID_WALK | RT_SOLID_SHOT);
            }
        }
        // shadows and crowns left around erased objects: plain cells of another look touching them
        {
            u8 (*erased)[ARENA_MW] = gRt->scr.erased;
            for (y = 0; y < ARENA_MH; y++)
                for (x = 0; x < ARENA_MW; x++)
                    erased[y][x] = objId[y][x] && gRt->arenaMeta[y][x] == objFill[objId[y][x]];
            for (y = 0; y < ARENA_MH; y++)
                for (x = 0; x < ARENA_MW; x++)
                {
                    u16 m = gRt->arenaMeta[y][x];
                    s32 j, k;
                    u8 near = 0;
                    if (erased[y][x] || clean[y][x] == 2 || !HasTopLayer(m))
                        continue;
                    for (j = -1; j <= 1 && !near; j++)
                        for (k = -1; k <= 1 && !near; k++)
                            if (y + j >= 0 && y + j < ARENA_MH && x + k >= 0 && x + k < ARENA_MW && erased[y + j][x + k])
                                near = objId[y + j][x + k];
                    if (near)
                        gRt->arenaMeta[y][x] = objFill[near];
                }
        }
        // a frame of the map's border blocks (the trees or rock that close off the map) around the field,
        // wherever something cut by the field's edge would show: the field looks like a clearing instead of
        // a hole cut through buildings and cliffs
        {
            const struct MapLayout *layout = gMapHeader.mapLayout;
            u8 (*erased)[ARENA_MW] = gRt->scr.erased;
            if (!water && !indoorField && layout->border != NULL
             && !MetatileBehavior_IsSurfableWaterOrUnderwater(BehaviorOf(layout->border[0] & MAPGRID_METATILE_ID_MASK))
             && !IsBlankMetatile(layout->border[0] & MAPGRID_METATILE_ID_MASK))
            {
                s32 bxs, bys;
                for (bys = by0 - 2; bys < by0 + bh + 2; bys += 2)
                    for (bxs = bx0 - 2; bxs < bx0 + bw + 2; bxs += 2)
                    {
                        s32 j, k;
                        bool8 dirty = FALSE;
                        if (bxs >= bx0 && bxs < bx0 + bw && bys >= by0 && bys < by0 + bh)
                            continue; // inside the field
                        if (bxs < 0 || bys < 0 || bxs + 1 >= ARENA_MW || bys + 1 >= ARENA_MH)
                            continue;
                        for (j = 0; j < 2; j++)
                            for (k = 0; k < 2; k++)
                                if (clean[bys + j][bxs + k] != 0 && !erased[bys + j][bxs + k])
                                    dirty = TRUE;
                        if (!dirty)
                            continue;
                        for (j = 0; j < 2; j++)
                            for (k = 0; k < 2; k++)
                            {
                                s32 ax = gx0 + bestOx + bxs + k, ay = gy0 + bestOy + bys + j;
                                gRt->arenaMeta[bys + j][bxs + k] = layout->border[((ax + 1) & 1) + (((ay + 1) & 1) << 1)] & MAPGRID_METATILE_ID_MASK;
                            }
                    }
            }
        }
        // the scenery the camera can show around the field (2 cells) is made clean and whole: outdoors a ring
        // of the map's own border blocks (trees, rock), indoors the room's floor, never black void or a
        // building / cliff / piece of furniture sliced by the edge. (Squaring off the corners with whole border
        // blocks was tried: inside the bright field they read as stray bushes, so the field stays a clean
        // rectangle and the soft dimming does the transition.)
        {
            const struct MapLayout *layout = gMapHeader.mapLayout;
            u16 b0 = layout->border != NULL ? (layout->border[0] & MAPGRID_METATILE_ID_MASK) : 0;
            bool8 ring = !water && layout->border != NULL && !IsBlankMetatile(b0)
                      && !MetatileBehavior_IsSurfableWaterOrUnderwater(BehaviorOf(b0));
            // with a ring of trees / rock around it the field edge is obvious: the sides need no dimming
            gRt->sideDim = !ring || indoorField;
            s32 rx0 = bx0 - 2, rx1 = bx0 + bw + 2, ry0 = by0 - 2, ry1 = by0 + bh + 2;
            if (rx0 < 0) rx0 = 0;
            if (ry0 < 0) ry0 = 0;
            if (rx1 > ARENA_MW) rx1 = ARENA_MW;
            if (ry1 > ARENA_MH) ry1 = ARENA_MH;
            for (y = ry0; y < ry1; y++)
                for (x = rx0; x < rx1; x++)
                {
                    s32 ax = gx0 + bestOx + x, ay = gy0 + bestOy + y;
                    u16 border = ring ? (layout->border[((ax + 1) & 1) + (((ay + 1) & 1) << 1)] & MAPGRID_METATILE_ID_MASK) : floorMeta;
                    if (x >= bx0 && x < bx0 + bw && y >= by0 && y < by0 + bh)
                        continue;
                    if (indoorField)
                        gRt->arenaMeta[y][x] = floorMeta;
                    else if (ring)
                        gRt->arenaMeta[y][x] = border;
                    else
                        gRt->arenaMeta[y][x] = floorMeta; // no usable border block: the ground runs on into the dark
                }
        }
        gRt->playX0 = bx0 * 16;
        gRt->playY0 = by0 * 16;
        gRt->playX1 = (bx0 + bw) * 16;
        gRt->playY1 = (by0 + bh) * 16;
    }
    // maps without connections (rooms, caves...) end in a blank border: keep the camera on the map
    if (gMapHeader.connections == NULL || gMapHeader.connections->count == 0)
    {
        const struct MapLayout *layout = gMapHeader.mapLayout;
        s32 mx0 = MAP_OFFSET - (gx0 + bestOx), my0 = MAP_OFFSET - (gy0 + bestOy);
        s32 mx1 = mx0 + layout->width, my1 = my0 + layout->height;
        if (mx0 < 0) mx0 = 0;
        if (my0 < 0) my0 = 0;
        if (mx1 > ARENA_MW) mx1 = ARENA_MW;
        if (my1 > ARENA_MH) my1 = ARENA_MH;
        gRt->viewX0 = mx0 * 16;
        gRt->viewY0 = my0 * 16;
        gRt->viewX1 = mx1 * 16;
        gRt->viewY1 = my1 * 16;
        // the whole field stays reachable by the camera even where it goes past a small room
        if (gRt->viewX0 > gRt->playX0) gRt->viewX0 = gRt->playX0;
        if (gRt->viewY0 > gRt->playY0) gRt->viewY0 = gRt->playY0;
        if (gRt->viewX1 < gRt->playX1) gRt->viewX1 = gRt->playX1;
        if (gRt->viewY1 < gRt->playY1) gRt->viewY1 = gRt->playY1;
    }
    // the outermost ring is always closed for walking (the view ends there)
    for (x = 0; x < ARENA_MW; x++)
    {
        gRt->solid[0][x] |= RT_SOLID_WALK;
        gRt->solid[ARENA_MH - 1][x] |= RT_SOLID_WALK;
    }
    for (y = 0; y < ARENA_MH; y++)
    {
        gRt->solid[y][0] |= RT_SOLID_WALK;
        gRt->solid[y][ARENA_MW - 1] |= RT_SOLID_WALK;
    }

    // the camera may show a little of the dimmed scenery around the field
    if (gRt->viewX0 < gRt->playX0 - 32) gRt->viewX0 = gRt->playX0 - 32;
    if (gRt->viewY0 < gRt->playY0 - 24) gRt->viewY0 = gRt->playY0 - 24;
    if (gRt->viewX1 > gRt->playX1 + 32) gRt->viewX1 = gRt->playX1 + 32;
    if (gRt->viewY1 > gRt->playY1 + 24) gRt->viewY1 = gRt->playY1 + 24;

    // 6. spawn points inside the reachable area
    {
        u8 *walk = comp;
        u8 *open = comp + ARENA_MW * ARENA_MH;
        s32 cx = 0, cy = 0, n = 0;
        u8 sx, sy;
        for (y = 0; y < ARENA_MH; y++)
            for (x = 0; x < ARENA_MW; x++)
            {
                walk[y * ARENA_MW + x] = !(gRt->solid[y][x] & RT_SOLID_WALK);
                open[y * ARENA_MW + x] = 0;
                if (walk[y * ARENA_MW + x])
                {
                    cx += x;
                    cy += y;
                    n++;
                }
            }
        for (y = 1; y < ARENA_MH - 1; y++)
            for (x = 1; x < ARENA_MW - 1; x++)
            {
                s32 j, k;
                bool8 o = walk[y * ARENA_MW + x];
                for (j = -1; j <= 1 && o; j++)
                    for (k = -1; k <= 1 && o; k++)
                        if (!walk[(y + j) * ARENA_MW + x + k])
                            o = FALSE;
                open[y * ARENA_MW + x] = o;
            }
        if (n)
        {
            s32 pxl = px - (gx0 + bestOx), pyl = py - (gy0 + bestOy);
            cx /= n;
            cy /= n;
            // player side: near where the player actually stands if it is inside, else below the centre
            if (pxl > 0 && pyl > 0 && pxl < ARENA_MW - 1 && pyl < ARENA_MH - 1 && walk[pyl * ARENA_MW + pxl])
            {
                cx = (cx + pxl) / 2;
                cy = (cy + pyl) / 2;
            }
            // the screen is wide and short: put the sides diagonally apart so both stay in view
            PickSpawn(walk, open, cx - 4, cy + 1, -1, -1, 0, &sx, &sy);
            sSpawnCell[RT_SIDE_PLAYER][0][0] = sx;
            sSpawnCell[RT_SIDE_PLAYER][0][1] = sy;
            PickSpawn(walk, open, cx + 4, cy - 1, sx, sy, 5, &sSpawnCell[RT_SIDE_ENEMY][0][0], &sSpawnCell[RT_SIDE_ENEMY][0][1]);
            PickSpawn(walk, open, sx, sy + 2, sx, sy, 2, &sSpawnCell[RT_SIDE_PLAYER][1][0], &sSpawnCell[RT_SIDE_PLAYER][1][1]);
            PickSpawn(walk, open, sSpawnCell[RT_SIDE_ENEMY][0][0], sSpawnCell[RT_SIDE_ENEMY][0][1] - 2,
                      sSpawnCell[RT_SIDE_ENEMY][0][0], sSpawnCell[RT_SIDE_ENEMY][0][1], 2,
                      &sSpawnCell[RT_SIDE_ENEMY][1][0], &sSpawnCell[RT_SIDE_ENEMY][1][1]);
            sSpawnValid = TRUE;
        }
    }
    gRt->floorKind = water ? 1 : ((MetatileBehavior_IsTallGrass(playerBehav) || MetatileBehavior_IsLongGrass(playerBehav)) ? 2 : 0);
    RtMapCache_End();
    MarkBreakables();
    RtProps_Place(); // after floorKind: nothing is put down on water
    Free(grid);
    Free(sum);
    Free(comp);
    Free(queue);
    return;

fallback:
    RtMapCache_End();
    if (grid) Free(grid);
    if (sum) Free(sum);
    if (comp) Free(comp);
    if (queue) Free(queue);
    RtArena_BuildSynthetic();
    for (y = 0; y < ARENA_MH; y++)
        for (x = 0; x < ARENA_MW; x++)
            if (gRt->solid[y][x])
                gRt->solid[y][x] = RT_SOLID_WALK | RT_SOLID_SHOT;
}

static const u16 *GetMetatileTiles(u16 metatile)
{
    const struct MapLayout *layout = gMapHeader.mapLayout;
    if (metatile < NUM_METATILES_IN_PRIMARY)
        return &layout->primaryTileset->metatiles[metatile * NUM_TILES_PER_METATILE];
    return &layout->secondaryTileset->metatiles[(metatile - NUM_METATILES_IN_PRIMARY) * NUM_TILES_PER_METATILE];
}

static void LoadTileset(const struct Tileset *ts, u16 numTiles, u16 offsetTiles)
{
    void *dst = (void *)(BG_CHAR_ADDR(0) + offsetTiles * TILE_SIZE_4BPP);
    if (ts == NULL)
        return;
    if (ts->isCompressed)
        LZ77UnCompVram(ts->tiles, dst);
    else
        CpuCopy16(ts->tiles, dst, numTiles * TILE_SIZE_4BPP);
}

static void LoadTilesetPal(const struct Tileset *ts, u16 dest, u16 size)
{
    u16 black = RGB_BLACK;
    if (ts == NULL)
        return;
    if (ts->isSecondary == FALSE)
    {
        LoadPalette(&black, dest, PLTT_SIZEOF(1));
        LoadPalette(ts->palettes[0] + 1, dest + 1, size - PLTT_SIZEOF(1));
    }
    else if (ts->isSecondary == TRUE)
    {
        LoadPalette(ts->palettes[NUM_PALS_IN_PRIMARY], dest, size);
    }
    else
    {
        LoadCompressedPalette((const u32 *)ts->palettes, dest, size);
    }
}

static void PutCell(u16 *bg1, u16 *bg2, u16 *bg3, s32 mx, s32 my, u16 metatile)
{
    const u16 *t = GetMetatileTiles(metatile);
    u8 layer = (GetMetatileAttributesById(metatile) & METATILE_ATTR_LAYER_MASK) >> METATILE_ATTR_LAYER_SHIFT;
    s32 k;
    for (k = 0; k < 4; k++)
    {
        s32 tx = mx * 2 + (k & 1);
        s32 ty = my * 2 + (k >> 1);
        s32 idx = (tx >> 5) * 1024 + ty * 32 + (tx & 31);
        u16 lo = t[k], hi = t[k + 4];
        switch (layer)
        {
        case METATILE_LAYER_TYPE_SPLIT:
            bg3[idx] = lo; bg2[idx] = 0; bg1[idx] = hi;
            break;
        case METATILE_LAYER_TYPE_COVERED:
            bg3[idx] = lo; bg2[idx] = hi; bg1[idx] = 0;
            break;
        default: // NORMAL
            bg3[idx] = 0; bg2[idx] = lo; bg1[idx] = hi;
            break;
        }
    }
}

void RtArena_LoadGfx(void)
{
    const struct MapLayout *layout = gMapHeader.mapLayout;
    u16 *bg1 = (u16 *)BG_SCREEN_ADDR(24);
    u16 *bg2 = (u16 *)BG_SCREEN_ADDR(26);
    u16 *bg3 = (u16 *)BG_SCREEN_ADDR(28);
    s32 x, y;

    LoadTileset(layout->primaryTileset, NUM_TILES_IN_PRIMARY, 0);
    LoadTileset(layout->secondaryTileset, NUM_TILES_TOTAL - NUM_TILES_IN_PRIMARY, NUM_TILES_IN_PRIMARY);
    LoadTilesetPal(layout->primaryTileset, BG_PLTT_ID(0), NUM_PALS_IN_PRIMARY * PLTT_SIZE_4BPP);
    LoadTilesetPal(layout->secondaryTileset, BG_PLTT_ID(NUM_PALS_IN_PRIMARY), (NUM_PALS_TOTAL - NUM_PALS_IN_PRIMARY) * PLTT_SIZE_4BPP);

    // the overworld tile 0 is not necessarily blank; keep BG3 transparent in NORMAL cells using an empty tile index.
    CpuFill16(0, bg1, 0x1000 * 3);
    for (y = 0; y < ARENA_MH; y++)
        for (x = 0; x < ARENA_MW; x++)
            PutCell(bg1, bg2, bg3, x, y, gRt->arenaMeta[y][x]);
    // terrain kind of every cell
    for (y = 0; y < ARENA_MH; y++)
        for (x = 0; x < ARENA_MW; x++)
        {
            u8 b = BehaviorOf(gRt->arenaMeta[y][x]), t = RTT_NONE;
            if (MetatileBehavior_IsSurfableWaterOrUnderwater(b))
                t = RTT_WATER;
            else if (MetatileBehavior_IsSandOrDeepSand(b))
                t = RTT_SAND;
            else if (MetatileBehavior_IsTallGrass(b) || MetatileBehavior_IsLongGrass(b))
                t = RTT_GRASS;
            else if (MetatileBehavior_IsIce(b))
                t = RTT_ICE;
            gRt->terrain[y][x] = t;
        }
    InitTilesetAnimations();
}

// ---------------------------------------------------------------------------
// Destructible scenery: small isolated obstacles (trees, rocks, signs...) break under strong hits
// ---------------------------------------------------------------------------
static void MarkBreakables(void)
{
    u16 *q = gRt->scr.pathQueue;
    u16 cnt[16], ids[16];
    u8 nIds = 0, nGrp = 0, best = 0;
    s32 x, y, k;
    memset(gRt->grpId, 0, sizeof(gRt->grpId));
    memset(gRt->grpHp, 0, sizeof(gRt->grpHp));
    memset(gRt->grpHit, 0, sizeof(gRt->grpHit));
    gRt->floorMeta = 0xFFFF;
    for (y = 0; y < ARENA_MH; y++)
        for (x = 0; x < ARENA_MW; x++)
        {
            u16 m = gRt->arenaMeta[y][x];
            if (gRt->solid[y][x] || BehaviorOf(m) != MB_NORMAL
             || (GetMetatileAttributesById(m) & METATILE_ATTR_LAYER_MASK) != (METATILE_LAYER_TYPE_NORMAL << METATILE_ATTR_LAYER_SHIFT))
                continue;
            for (k = 0; k < nIds; k++)
                if (ids[k] == m)
                    break;
            if (k < nIds)
                cnt[k]++;
            else if (nIds < 16)
            {
                ids[nIds] = m;
                cnt[nIds++] = 1;
            }
        }
    if (nIds == 0)
        return;
    for (k = 1; k < nIds; k++)
        if (cnt[k] > cnt[best])
            best = k;
    gRt->floorMeta = ids[best];
    // flood the blocked groups (kept in grpId as 0xFF while they are being looked at)
    for (y = 1; y < ARENA_MH - 1; y++)
        for (x = 1; x < ARENA_MW - 1; x++)
        {
            u16 head = 0, tail = 0;
            bool8 ok = TRUE;
            if (!(gRt->solid[y][x] & RT_SOLID_WALK) || gRt->grpId[y][x])
                continue;
            gRt->grpId[y][x] = 0xFF;
            q[tail++] = y * ARENA_MW + x;
            while (head < tail)
            {
                s32 cx = q[head] % ARENA_MW, cy = q[head] / ARENA_MW;
                head++;
                if (cx <= 1 || cy <= 1 || cx >= ARENA_MW - 2 || cy >= ARENA_MH - 2 || !(gRt->solid[cy][cx] & RT_SOLID_SHOT))
                    ok = FALSE;
                for (k = 0; k < 8; k++)
                {
                    static const s8 sDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
                    static const s8 sDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                    s32 nx = cx + sDx[k], ny = cy + sDy[k];
                    if (nx < 0 || ny < 0 || nx >= ARENA_MW || ny >= ARENA_MH)
                    {
                        ok = FALSE;
                        continue;
                    }
                    if (!(gRt->solid[ny][nx] & RT_SOLID_WALK) || gRt->grpId[ny][nx])
                        continue;
                    gRt->grpId[ny][nx] = 0xFF;
                    q[tail++] = ny * ARENA_MW + nx;
                }
            }
            if (ok && tail <= 8 && nGrp < 23)
            {
                nGrp++;
                gRt->grpHp[nGrp] = 6;
                for (k = 0; k < tail; k++)
                    gRt->grpId[q[k] / ARENA_MW][q[k] % ARENA_MW] = nGrp;
            }
            else
                for (k = 0; k < tail; k++)
                    gRt->grpId[q[k] / ARENA_MW][q[k] % ARENA_MW] = 0xFE;
        }
    for (y = 0; y < ARENA_MH; y++)
        for (x = 0; x < ARENA_MW; x++)
            if (gRt->grpId[y][x] >= 0xFE)
                gRt->grpId[y][x] = 0;
}

static void BreakGroup(u8 id)
{
    u16 *bg1 = (u16 *)BG_SCREEN_ADDR(24);
    u16 *bg2 = (u16 *)BG_SCREEN_ADDR(26);
    u16 *bg3 = (u16 *)BG_SCREEN_ADDR(28);
    s32 x, y;
    for (y = 0; y < ARENA_MH; y++)
        for (x = 0; x < ARENA_MW; x++)
            if (gRt->grpId[y][x] == id)
            {
                gRt->grpId[y][x] = 0;
                gRt->solid[y][x] = 0;
                gRt->terrain[y][x] = RTT_NONE;
                gRt->arenaMeta[y][x] = gRt->floorMeta;
                PutCell(bg1, bg2, bg3, x, y, gRt->floorMeta);
                RtProps_Debris(PROP_ROCK, TO_FX(x * 16 + 8), TO_FX(y * 16 + 14));
            }
    gRt->grpHp[id] = 0;
    RtArena_ResetPaths();
    RtFx_Shake(2, 10);
}

// Returns TRUE when the circle (FX position, pixel radius) touches scenery that can break
bool8 RtArena_HitCells(s32 fx, s32 fy, s32 r, u8 dmg)
{
    s32 x = FROM_FX(fx), y = FROM_FX(fy);
    s32 cx0 = (x - r) >> 4, cx1 = (x + r) >> 4, cy0 = (y - r) >> 4, cy1 = (y + r) >> 4, cx, cy;
    bool8 hit = FALSE;
    if (gRt->floorMeta == 0xFFFF)
        return FALSE;
    for (cy = cy0; cy <= cy1; cy++)
        for (cx = cx0; cx <= cx1; cx++)
        {
            u8 id;
            if (cx < 0 || cy < 0 || cx >= ARENA_MW || cy >= ARENA_MH)
                continue;
            id = gRt->grpId[cy][cx];
            if (id == 0)
                continue;
            hit = TRUE;
            if (gRt->grpHit[id] && (u16)(gRt->frame - gRt->grpHit[id]) < 14)
                continue;
            gRt->grpHit[id] = gRt->frame | 1;
            RtFx_Spawn(FX_SPARK8, FXP_WHITE, TO_FX(cx * 16 + 8), TO_FX(cy * 16 + 8), 8);
            if (gRt->grpHp[id] <= dmg)
                BreakGroup(id);
            else
            {
                gRt->grpHp[id] -= dmg;
                RtFx_Shake(1, 3);
            }
        }
    return hit;
}

// ---------------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------------
bool8 RtArena_PointSolid(s32 px, s32 py)
{
    s32 cx = px >> 4, cy = py >> 4;
    if (cx < 0 || cy < 0 || cx >= ARENA_MW || cy >= ARENA_MH)
        return TRUE;
    return (gRt->solid[cy][cx] & RT_SOLID_WALK) != 0;
}

bool8 RtArena_PointBlocksShot(s32 px, s32 py)
{
    s32 cx = px >> 4, cy = py >> 4;
    if (cx < 0 || cy < 0 || cx >= ARENA_MW || cy >= ARENA_MH)
        return TRUE;
    return (gRt->solid[cy][cx] & RT_SOLID_SHOT) != 0;
}

bool8 RtArena_CircleBlocked(s32 x, s32 y, s32 r)
{
    s32 cx0 = (x - r) >> 4, cx1 = (x + r) >> 4, cy0 = (y - r) >> 4, cy1 = (y + r) >> 4, cx, cy;
    for (cy = cy0; cy <= cy1; cy++)
    {
        for (cx = cx0; cx <= cx1; cx++)
        {
            s32 nx, ny, dx, dy;
            if (cx < 0 || cy < 0 || cx >= ARENA_MW || cy >= ARENA_MH)
                return TRUE;
            if (!(gRt->solid[cy][cx] & RT_SOLID_WALK))
                continue;
            nx = x < cx * 16 ? cx * 16 : (x > cx * 16 + 15 ? cx * 16 + 15 : x);
            ny = y < cy * 16 ? cy * 16 : (y > cy * 16 + 15 ? cy * 16 + 15 : y);
            dx = x - nx;
            dy = y - ny;
            if (dx * dx + dy * dy < r * r)
                return TRUE;
        }
    }
    return RtProps_CircleBlocked(x, y, r);
}

// Push an actor's (feet) circle out of solid cells. Keeps sub-pixel precision unless a push happens.
void RtArena_ResolveActor(struct RtActor *a)
{
    RtProps_PushActor(a);
    {
    s32 r = a->radius > 4 ? a->radius - 2 : a->radius;
    s32 x = FROM_FX(a->x), y = FROM_FX(a->y);
    s32 iter;
    bool8 any = FALSE;
    for (iter = 0; iter < 3; iter++)
    {
        s32 cx0 = (x - r) >> 4, cx1 = (x + r) >> 4, cy0 = (y - r / 2) >> 4, cy1 = (y + r / 2) >> 4, cx, cy;
        bool8 moved = FALSE;
        for (cy = cy0; cy <= cy1; cy++)
        {
            for (cx = cx0; cx <= cx1; cx++)
            {
                s32 nx, ny, dx, dy, d2;
                bool8 solid = (cx < 0 || cy < 0 || cx >= ARENA_MW || cy >= ARENA_MH) ? TRUE : (gRt->solid[cy][cx] & RT_SOLID_WALK) != 0;
                if (!solid)
                    continue;
                nx = x < cx * 16 ? cx * 16 : (x > cx * 16 + 15 ? cx * 16 + 15 : x);
                ny = y < cy * 16 ? cy * 16 : (y > cy * 16 + 15 ? cy * 16 + 15 : y);
                dx = x - nx;
                dy = (y - ny) * 2;
                d2 = dx * dx + dy * dy;
                if (d2 < r * r)
                {
                    if (d2 == 0)
                    {
                        s32 l = x - cx * 16, rr = cx * 16 + 16 - x, u = y - cy * 16, dd = cy * 16 + 16 - y;
                        s32 m = l;
                        if (rr < m) m = rr;
                        if (u < m) m = u;
                        if (dd < m) m = dd;
                        if (m == l) x = cx * 16 - r;
                        else if (m == rr) x = cx * 16 + 16 + r;
                        else if (m == u) y = cy * 16 - r / 2;
                        else y = cy * 16 + 16 + r / 2;
                    }
                    else
                    {
                        s32 d = RtDist(dx, dy);
                        s32 push = r - d + 1;
                        if (d == 0) d = 1;
                        x += dx * push / d;
                        y += (dy / 2) * push / d;
                    }
                    moved = TRUE;
                    any = TRUE;
                }
            }
        }
        if (!moved)
            break;
    }
    if (any)
    {
        a->x = TO_FX(x) + 128;
        a->y = TO_FX(y) + 128;
    }
    // still inside a wall (e.g. after landing from Fly): move to the nearest free cell
    if (RtArena_PointSolid(x, y))
    {
        s32 r2, best = 0x7FFFFFFF, bx = x, by = y, ccx, ccy;
        for (ccy = 0; ccy < ARENA_MH; ccy++)
            for (ccx = 0; ccx < ARENA_MW; ccx++)
            {
                s32 dx, dy;
                if (gRt->solid[ccy][ccx] & RT_SOLID_WALK)
                    continue;
                dx = ccx * 16 + 8 - x;
                dy = ccy * 16 + 8 - y;
                r2 = dx * dx + dy * dy;
                if (r2 < best)
                {
                    best = r2;
                    bx = ccx * 16 + 8;
                    by = ccy * 16 + 8;
                }
            }
        a->x = TO_FX(bx);
        a->y = TO_FX(by);
    }
    }
}

void RtArena_FindSpawn(u8 side, u8 index, s32 *x, s32 *y)
{
    s32 sx, sy;
    if (sSpawnValid)
    {
        *x = TO_FX(sSpawnCell[side][index & 1][0] * 16 + 8);
        *y = TO_FX(sSpawnCell[side][index & 1][1] * 16 + 10);
        return;
    }
    if (side == RT_SIDE_PLAYER)
    {
        sx = ARENA_W / 2 + (index ? 36 : -20);
        sy = ARENA_H * 62 / 100;
    }
    else
    {
        sx = ARENA_W / 2 + (index ? -36 : 20);
        sy = ARENA_H * 36 / 100;
    }
    // nudge out of obstacles
    {
        s32 tries = 0;
        while (RtArena_CircleBlocked(sx, sy, 10) && tries < 20)
        {
            sx += (tries & 1) ? 18 : -18;
            tries++;
        }
    }
    *x = TO_FX(sx);
    *y = TO_FX(sy);
}

// ---------------------------------------------------------------------------
// Path guidance for the AI: breadth-first distance fields towards targets.
// ---------------------------------------------------------------------------
#define PATH_SLOTS 4
#define sPathDist (gRt->scr.pathDist)
static EWRAM_DATA u16 sPathCell[PATH_SLOTS] = {0};
static EWRAM_DATA u32 sPathFrame[PATH_SLOTS] = {0};
static EWRAM_DATA u8 sPathUsed[PATH_SLOTS] = {0};

static bool8 CellWalk(s32 cx, s32 cy)
{
    if (cx < 0 || cy < 0 || cx >= ARENA_MW || cy >= ARENA_MH)
        return FALSE;
    return !(gRt->solid[cy][cx] & RT_SOLID_WALK);
}

#define PW (ARENA_MW + 2)
#define PH (ARENA_MH + 2)
#define sWalkPad (gRt->scr.walkPad)
static EWRAM_DATA bool8 sWalkPadReady = FALSE;

// big scratch buffers live in EWRAM: the IWRAM stack is tiny
#define sPathQueue (gRt->scr.pathQueue)
#define sPathPad (gRt->scr.pathPad)

static void BuildField(u8 slot, s32 tcx, s32 tcy)
{
    u16 *queue = sPathQueue;
    u8 *dpad = sPathPad;
    u16 head = 0, tail = 0;
    s32 i;
    static const s8 sOff[4] = {1, -1, PW, -PW};
    if (!sWalkPadReady)
    {
        s32 x, y;
        memset(sWalkPad, 0, PW * PH);
        for (y = 0; y < ARENA_MH; y++)
            for (x = 0; x < ARENA_MW; x++)
                sWalkPad[(y + 1) * PW + x + 1] = !(gRt->solid[y][x] & RT_SOLID_WALK);
        sWalkPadReady = TRUE;
    }
    if (tcx < 0) tcx = 0;
    if (tcy < 0) tcy = 0;
    if (tcx >= ARENA_MW) tcx = ARENA_MW - 1;
    if (tcy >= ARENA_MH) tcy = ARENA_MH - 1;
    memset(dpad, 0xFF, PW * PH);
    i = (tcy + 1) * PW + tcx + 1;
    dpad[i] = 0;
    queue[tail++] = i;
    while (head < tail)
    {
        u16 c = queue[head++];
        u8 nd = dpad[c] + 1;
        s32 k;
        if (nd == 0xFF)
            continue;
        for (k = 0; k < 4; k++)
        {
            u16 n = c + sOff[k];
            if (!sWalkPad[n] || dpad[n] != 0xFF)
                continue;
            dpad[n] = nd;
            queue[tail++] = n;
        }
    }
    {
        u8 *d = sPathDist[slot];
        s32 y;
        for (y = 0; y < ARENA_MH; y++)
            memcpy(&d[y * ARENA_MW], &dpad[(y + 1) * PW + 1], ARENA_MW);
    }
}

static bool8 LineClear(s32 x0, s32 y0, s32 x1, s32 y1, s32 r)
{
    s32 dx = x1 - x0, dy = y1 - y0, i, n;
    n = ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)) / 12 + 1;
    for (i = 1; i <= n; i++)
    {
        s32 x = x0 + dx * i / n, y = y0 + dy * i / n;
        if (RtArena_PointSolid(x, y) || RtArena_PointSolid(x - r, y) || RtArena_PointSolid(x + r, y))
            return FALSE;
    }
    return TRUE;
}

// Returns TRUE and the direction to walk when the straight way to (tx, ty) is blocked.
bool8 RtArena_PathAngle(s32 fx, s32 fy, s32 tx, s32 ty, s32 r, u8 *ang)
{
    s32 tcx = tx >> 4, tcy = ty >> 4, cx = fx >> 4, cy = fy >> 4, i, slot = -1, k;
    u16 cell;
    u8 best, cur;
    s32 bx = -1, by = -1;
    if (LineClear(fx, fy, tx, ty, r))
        return FALSE;
    cell = tcy * ARENA_MW + tcx;
    for (i = 0; i < PATH_SLOTS; i++)
    {
        s32 ox, oy;
        if (!sPathUsed[i])
            continue;
        ox = sPathCell[i] % ARENA_MW - tcx;
        oy = sPathCell[i] / ARENA_MW - tcy;
        if (ox >= -1 && ox <= 1 && oy >= -1 && oy <= 1)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        u32 oldest = 0xFFFFFFFF;
        for (i = 0; i < PATH_SLOTS; i++)
        {
            if (!sPathUsed[i] || sPathFrame[i] + 1 < oldest + 1)
            {
                if (!sPathUsed[i])
                {
                    slot = i;
                    break;
                }
                if (sPathFrame[i] < oldest)
                {
                    oldest = sPathFrame[i];
                    slot = i;
                }
            }
        }
        BuildField(slot, tcx, tcy);
        sPathUsed[slot] = TRUE;
        sPathCell[slot] = cell;
    }
    sPathFrame[slot] = gRt->frame;
    if (cx < 0 || cy < 0 || cx >= ARENA_MW || cy >= ARENA_MH)
        return FALSE;
    cur = sPathDist[slot][cy * ARENA_MW + cx];
    best = cur;
    for (k = 0; k < 8; k++)
    {
        static const s8 sDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
        static const s8 sDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
        s32 nx = cx + sDx[k], ny = cy + sDy[k];
        u8 nd;
        if (!CellWalk(nx, ny))
            continue;
        if (k >= 4 && (!CellWalk(cx + sDx[k], cy) || !CellWalk(cx, cy + sDy[k])))
            continue;
        nd = sPathDist[slot][ny * ARENA_MW + nx];
        if (nd < best)
        {
            best = nd;
            bx = nx;
            by = ny;
        }
    }
    if (bx < 0)
        return FALSE;
    // look further along the path: aim at the farthest cell we can see in a straight line
    for (i = 0; i < 2; i++)
    {
        s32 nbx = -1, nby = -1;
        u8 nb = best;
        for (k = 0; k < 4; k++)
        {
            s32 nx = bx + (k == 0) - (k == 1), ny = by + (k == 2) - (k == 3);
            if (CellWalk(nx, ny) && sPathDist[slot][ny * ARENA_MW + nx] < nb)
            {
                nb = sPathDist[slot][ny * ARENA_MW + nx];
                nbx = nx;
                nby = ny;
            }
        }
        if (nbx < 0 || !LineClear(fx, fy, nbx * 16 + 8, nby * 16 + 8, r))
            break;
        bx = nbx;
        by = nby;
        best = nb;
    }
    *ang = RtAtan2(bx * 16 + 8 - fx, by * 16 + 8 - fy);
    return TRUE;
}

bool8 RtArena_ShotClear(s32 x0, s32 y0, s32 x1, s32 y1)
{
    s32 dx = x1 - x0, dy = y1 - y0;
    s32 n = ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)) / 8 + 1, i;
    for (i = 1; i < n; i++)
        if (RtArena_PointBlocksShot(x0 + dx * i / n, y0 + dy * i / n))
            return FALSE;
    return TRUE;
}

static void RtArena_ResetPaths(void)
{
    memset(sPathUsed, 0, sizeof(sPathUsed));
    sWalkPadReady = FALSE;
}
