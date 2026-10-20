// Open-world support: secondary tilesets of neighbouring maps share the VRAM tile area and the
// secondary palette slots through a small cache. Tiles/palettes are loaded on demand when a
// metatile is drawn and evicted (when no longer referenced by the BG tilemaps) to make room.
#include "global.h"
#include "fieldmap.h"
#include "field_camera.h"
#include "malloc.h"
#include "palette.h"
#include "field_weather.h"
#include "tileset_anims.h"
#include "bg.h"
#include "world_tiles.h"

#define WT_NSETS     3
#define WT_SLOTS     496          // allocatable tile slots (secondary tiles 0-495); 496-511 stay with the current map
#define WT_UNLOADED  511
#define WT_NOOWNER   0xFFFF
#define WT_NOPAL     0xFF
#define WT_MAXRECT   4

struct WtSet
{
    const struct Tileset *ts;
    ALIGNED(4) u8 lo[512];
    ALIGNED(4) u8 hi[64];
    u8 pal[NUM_PALS_TOTAL - NUM_PALS_IN_PRIMARY];
};

struct WtRect
{
    s16 x, y, w, h;
    const struct Tileset *ts;
    u8 set;
};

static EWRAM_DATA struct WtSet sSet[WT_NSETS] = {0};
static EWRAM_DATA u16 sOwner[WT_SLOTS] = {0};
static EWRAM_DATA u8 sPalOwner[NUM_PALS_TOTAL - NUM_PALS_IN_PRIMARY] = {0};
static EWRAM_DATA struct WtRect sRect[WT_MAXRECT] = {0};
static EWRAM_DATA u32 sPin[WT_SLOTS / 32] = {0};
static EWRAM_DATA u32 sRef[WT_SLOTS / 32] = {0};
static EWRAM_DATA u8 sRefPal = 0;
static EWRAM_DATA u8 sRefValid = 0;
static EWRAM_DATA u8 sNumRect = 0;
static EWRAM_DATA u8 sCur = 0;
static EWRAM_DATA u8 sActive = 0;
static EWRAM_DATA u8 sNeedRedraw = 0;
static EWRAM_DATA u16 sCursor = 0;
static EWRAM_DATA u8 sPalCursor = 0;
static EWRAM_DATA u8 *sTmp = NULL;
static EWRAM_DATA const struct Tileset *sTmpTs = NULL;
static EWRAM_DATA u32 sTmpSize = 0;

#define NPAL (NUM_PALS_TOTAL - NUM_PALS_IN_PRIMARY)

struct WtPin { TilesetCB cb; u16 a0, a1, b0, b1; };

static const struct WtPin sPins[] =
{
    { InitTilesetAnim_Rustboro,   96, 160, 448, 452 },
    { InitTilesetAnim_Dewford,   170, 176,   0,   0 },
    { InitTilesetAnim_Slateport, 224, 228,   0,   0 },
    { InitTilesetAnim_Mauville,  224, 256,   0,   0 },
    { InitTilesetAnim_Lavaridge, 160, 164, 288, 296 },
    { InitTilesetAnim_EverGrande,128, 160,   0,   0 },
    { InitTilesetAnim_Pacifidlog,464, 496,   0,   0 },
    { InitTilesetAnim_Sootopolis,240, 336,   0,   0 },
};

static u16 WtGet(u8 s, u16 t)
{
    return sSet[s].lo[t] | (((sSet[s].hi[t >> 3] >> (t & 7)) & 1) << 8);
}

static void WtPut(u8 s, u16 t, u16 v)
{
    sSet[s].lo[t] = v & 0xFF;
    if (v & 0x100)
        sSet[s].hi[t >> 3] |= 1 << (t & 7);
    else
        sSet[s].hi[t >> 3] &= ~(1 << (t & 7));
}

static void WtClearSet(u8 s)
{
    s32 i;

    for (i = 0; i < WT_SLOTS; i++)
        if (sOwner[i] != WT_NOOWNER && (sOwner[i] >> 9) == s)
            sOwner[i] = WT_NOOWNER;
    for (i = 0; i < NPAL; i++)
        if (sPalOwner[i] != WT_NOPAL && (sPalOwner[i] >> 3) == s)
            sPalOwner[i] = WT_NOPAL;
    CpuFill32(0xFFFFFFFF, sSet[s].lo, sizeof(sSet[s].lo));
    CpuFill32(0xFFFFFFFF, sSet[s].hi, sizeof(sSet[s].hi));
    for (i = 0; i < NPAL; i++)
        sSet[s].pal[i] = WT_NOPAL;
    sSet[s].ts = NULL;
}

static bool8 WtPinned(u16 slot)
{
    return (sPin[slot >> 5] >> (slot & 31)) & 1;
}

static void WtComputePins(void)
{
    s32 i;
    u16 s;
    TilesetCB cb = gMapHeader.mapLayout->secondaryTileset->callback;

    CpuFill32(0, sPin, sizeof(sPin));
    for (i = 0; i < (s32)ARRAY_COUNT(sPins); i++)
    {
        if (sPins[i].cb != cb)
            continue;
        for (s = sPins[i].a0; s < sPins[i].a1 && s < WT_SLOTS; s++)
            sPin[s >> 5] |= 1u << (s & 31);
        for (s = sPins[i].b0; s < sPins[i].b1 && s < WT_SLOTS; s++)
            sPin[s >> 5] |= 1u << (s & 31);
        break;
    }
}

static void WtFreeTmp(void)
{
    if (sTmp != NULL)
        Free(sTmp);
    sTmp = NULL;
    sTmpTs = NULL;
}

static void WtLoadTile(u8 si, u16 tile, u16 slot)
{
    const struct Tileset *ts = sSet[si].ts;
    const u8 *src = NULL;
    u32 off = tile * TILE_SIZE_4BPP;
    void *dst = (void *)(BG_VRAM + TILE_OFFSET_4BPP(NUM_TILES_IN_PRIMARY + slot));

    if (ts == NULL || ts->tiles == NULL)
        return;
    if (!ts->isCompressed)
    {
        src = (const u8 *)ts->tiles + off;
    }
    else
    {
        if (sTmpTs != ts)
        {
            u32 size = (*ts->tiles) >> 8;

            if (size > 0x4000)
                return;
            if (sTmp == NULL)
                sTmp = Alloc(0x4000);
            if (sTmp == NULL)
                return;
            LZ77UnCompWram(ts->tiles, sTmp);
            sTmpTs = ts;
            sTmpSize = size;
        }
        if (off + TILE_SIZE_4BPP > sTmpSize)
        {
            CpuFill32(0, dst, TILE_SIZE_4BPP);
            return;
        }
        src = sTmp + off;
    }
    CpuCopy32(src, dst, TILE_SIZE_4BPP);
}

static void WtBuildRef(void)
{
    s32 bg, i;

    CpuFill32(0, sRef, sizeof(sRef));
    sRefPal = 0;
    for (bg = 1; bg <= 3; bg++)
    {
        const u16 *buf = GetBgTilemapBuffer(bg);

        if (buf == NULL)
            continue;
        for (i = 0; i < 1024; i++)
        {
            u16 e = buf[i];
            u16 t = e & 0x3FF;
            u16 p = e >> 12;

            if (t >= NUM_TILES_IN_PRIMARY && t < NUM_TILES_IN_PRIMARY + WT_SLOTS)
            {
                t -= NUM_TILES_IN_PRIMARY;
                sRef[t >> 5] |= 1u << (t & 31);
            }
            if (p >= NUM_PALS_IN_PRIMARY && p < NUM_PALS_TOTAL)
                sRefPal |= 1 << (p - NUM_PALS_IN_PRIMARY);
        }
    }
    sRefValid = TRUE;
}

static u16 WtAlloc(void)
{
    s32 pass, n;

    if (!sRefValid)
        WtBuildRef();
    for (pass = 0; pass < 3; pass++)
    {
        for (n = 0; n < WT_SLOTS; n++)
        {
            u16 slot = sCursor + n;

            if (slot >= WT_SLOTS)
                slot -= WT_SLOTS;
            if (WtPinned(slot) || ((sRef[slot >> 5] >> (slot & 31)) & 1))
                continue;
            if (pass == 0 && sOwner[slot] != WT_NOOWNER)
                continue;
            if (pass == 1 && sOwner[slot] != WT_NOOWNER && (sOwner[slot] >> 9) == sCur)
                continue;
            if (sOwner[slot] != WT_NOOWNER)
                WtPut(sOwner[slot] >> 9, sOwner[slot] & 0x1FF, WT_UNLOADED);
            sOwner[slot] = WT_NOOWNER;
            sCursor = slot + 1;
            if (sCursor >= WT_SLOTS)
                sCursor = 0;
            return slot;
        }
    }
    return 0xFFFF;
}

static u16 WtTileSlot(u8 si, u16 t)
{
    u16 v = WtGet(si, t);
    u16 slot;

    if (v != WT_UNLOADED)
    {
        sRef[v >> 5] |= 1u << (v & 31);
        return v;
    }
    slot = WtAlloc();
    if (slot == 0xFFFF)
        return 0; // out of room: draw something harmless (tile of slot 0)
    WtPut(si, t, slot);
    sOwner[slot] = (si << 9) | t;
    sRef[slot >> 5] |= 1u << (slot & 31);
    WtLoadTile(si, t, slot);
    return slot;
}

static u8 WtPalSlot(u8 si, u8 p)
{
    u8 v = sSet[si].pal[p];
    s32 pass, n;

    if (v != WT_NOPAL)
    {
        sRefPal |= 1 << v;
        return v;
    }
    if (!sRefValid)
        WtBuildRef();
    for (pass = 0; pass < 3; pass++)
    {
        for (n = 0; n < NPAL; n++)
        {
            u8 slot = (sPalCursor + n) % NPAL;

            if ((sRefPal >> slot) & 1)
                continue;
            if (pass == 0 && sPalOwner[slot] != WT_NOPAL)
                continue;
            if (pass == 1 && sPalOwner[slot] != WT_NOPAL && (sPalOwner[slot] >> 3) == sCur)
                continue;
            if (sPalOwner[slot] != WT_NOPAL)
                sSet[sPalOwner[slot] >> 3].pal[sPalOwner[slot] & 7] = WT_NOPAL;
            sPalOwner[slot] = (si << 3) | p;
            sSet[si].pal[p] = slot;
            sRefPal |= 1 << slot;
            sPalCursor = (slot + 1) % NPAL;
            LoadPalette(sSet[si].ts->palettes[NUM_PALS_IN_PRIMARY + p], BG_PLTT_ID(NUM_PALS_IN_PRIMARY + slot), PLTT_SIZE_4BPP);
            ApplyWeatherColorMapToPal(NUM_PALS_IN_PRIMARY + slot);
            return slot;
        }
    }
    return p; // no free slot: keep the original index
}

// ---------------------------------------------------------------------------------------------

void WorldTiles_ClearRects(void)
{
    sNumRect = 0;
}

void WorldTiles_AddRect(s32 x, s32 y, s32 w, s32 h, const struct Tileset *ts)
{
    if (sNumRect >= WT_MAXRECT)
        return;
    sRect[sNumRect].x = x;
    sRect[sNumRect].y = y;
    sRect[sNumRect].w = w;
    sRect[sNumRect].h = h;
    sRect[sNumRect].ts = ts;
    sRect[sNumRect].set = 0;
    sNumRect++;
}

static s32 WtFindSet(const struct Tileset *ts)
{
    s32 i;

    for (i = 0; i < WT_NSETS; i++)
        if (sSet[i].ts == ts)
            return i;
    return -1;
}

// Make the set list match: current secondary tileset + the ones seen in the connection strips.
static void WtSyncSets(void)
{
    const struct Tileset *want[WT_NSETS];
    s32 n = 0, i, j;
    const struct Tileset *cur = gMapHeader.mapLayout->secondaryTileset;

    want[n++] = cur;
    for (i = 0; i < sNumRect; i++)
    {
        const struct Tileset *ts = sRect[i].ts;
        bool8 dup = FALSE;

        if (ts == NULL || ts->isSecondary != TRUE)
            continue;
        for (j = 0; j < n; j++)
            if (want[j] == ts)
                dup = TRUE;
        if (!dup && n < WT_NSETS)
            want[n++] = ts;
    }
    for (i = 0; i < WT_NSETS; i++)
    {
        bool8 keep = FALSE;

        for (j = 0; j < n; j++)
            if (want[j] == sSet[i].ts)
                keep = TRUE;
        if (!keep)
            WtClearSet(i);
    }
    for (j = 0; j < n; j++)
    {
        if (WtFindSet(want[j]) < 0)
        {
            for (i = 0; i < WT_NSETS; i++)
            {
                if (sSet[i].ts == NULL)
                {
                    WtClearSet(i);
                    sSet[i].ts = want[j];
                    break;
                }
            }
        }
    }
    sCur = WtFindSet(cur);
    for (i = 0; i < sNumRect; i++)
    {
        s32 f = WtFindSet(sRect[i].ts);

        sRect[i].set = (f < 0 || sRect[i].ts->isSecondary != TRUE) ? sCur : f;
    }
}

// Copies the whole secondary tileset straight into VRAM (no DMA queue, so tiles cached
// afterwards cannot be overwritten by a late request).
void WorldTiles_LoadSecondaryDirect(const struct Tileset *ts)
{
    void *dst = (void *)(BG_VRAM + TILE_OFFSET_4BPP(NUM_TILES_IN_PRIMARY));
    u32 total = (NUM_TILES_TOTAL - NUM_TILES_IN_PRIMARY) * TILE_SIZE_4BPP;

    if (ts == NULL || ts->tiles == NULL)
        return;
    if (!ts->isCompressed)
    {
        CpuCopy32(ts->tiles, dst, total);
        return;
    }
    else
    {
        u32 size = (*ts->tiles) >> 8;
        u8 *tmp;

        if (size > 0x4000)
            size = 0x4000;
        tmp = Alloc(0x4000);
        if (tmp == NULL)
            return;
        LZ77UnCompWram(ts->tiles, tmp);
        CpuCopy32(tmp, dst, size);
        if (size < total)
            CpuFill32(0, (u8 *)dst + size, total - size);
        Free(tmp);
    }
}

// Called after the vanilla code loaded both tilesets of the current map.
void WorldTiles_ResetIdentity(void)
{
    s32 i;

    WtFreeTmp();
    for (i = 0; i < WT_NSETS; i++)
        WtClearSet(i);
    sActive = TRUE;
    WtSyncSets();
    WtComputePins();
    for (i = 0; i < WT_SLOTS; i++)
    {
        WtPut(sCur, i, i);
        sOwner[i] = (sCur << 9) | i;
    }
    for (i = 0; i < NPAL; i++)
    {
        sSet[sCur].pal[i] = i;
        sPalOwner[i] = (sCur << 3) | i;
    }
    CpuFill32(0, sRef, sizeof(sRef));
    sRefPal = 0;
    sRefValid = TRUE;
    sNeedRedraw = FALSE;
}

static void WtPinSlots(void)
{
    s32 i;

    for (i = 0; i < WT_SLOTS; i++)
    {
        u16 v;

        if (!WtPinned(i))
            continue;
        // evict whatever sits here
        if (sOwner[i] != WT_NOOWNER && !((sOwner[i] >> 9) == sCur && (sOwner[i] & 0x1FF) == i))
        {
            WtPut(sOwner[i] >> 9, sOwner[i] & 0x1FF, WT_UNLOADED);
            sOwner[i] = WT_NOOWNER;
        }
        // the tile itself may live elsewhere
        v = WtGet(sCur, i);
        if (v != WT_UNLOADED && v != i)
        {
            sOwner[v] = WT_NOOWNER;
            WtPut(sCur, i, WT_UNLOADED);
        }
        if (WtGet(sCur, i) != i)
        {
            WtPut(sCur, i, i);
            sOwner[i] = (sCur << 9) | i;
            WtLoadTile(sCur, i, i);
        }
    }
    // tiles 496-511 live in the reserved slots (door graphics etc.)
    for (i = WT_SLOTS; i < 512; i++)
        WtLoadTile(sCur, i, i);
}

// Called when the camera crosses into a connected map.
void WorldTiles_Cross(void)
{
    const struct Tileset *cur = gMapHeader.mapLayout->secondaryTileset;
    s32 i;

    if (!sActive || WtFindSet(cur) < 0 || cur->isSecondary != TRUE)
    {
        // unknown tileset: do it the vanilla way
        CopySecondaryTilesetToVramUsingHeap(gMapHeader.mapLayout);
        LoadSecondaryTilesetPalette(gMapHeader.mapLayout);
        WorldTiles_ResetIdentity();
        sNeedRedraw = TRUE;
        return;
    }
    WtSyncSets();
    WtComputePins();
    WtBuildRef();
    WtPinSlots();
    for (i = 0; i < NPAL; i++)
        if (sSet[sCur].pal[i] == WT_NOPAL)
            ; // loaded lazily
    sNeedRedraw = TRUE;
}

bool8 WorldTiles_TakeRedraw(void)
{
    bool8 r = sNeedRedraw;

    sNeedRedraw = FALSE;
    return r;
}

void WorldTiles_BeginFullRedraw(void)
{
    // The whole tilemap is rewritten: only what gets drawn from now on counts as referenced.
    CpuFill32(0, sRef, sizeof(sRef));
    sRefPal = 0;
    sRefValid = TRUE;
}

void WorldTiles_FrameEnd(void)
{
    sRefValid = FALSE;
    if (sTmp != NULL)
        WtFreeTmp();
}

u8 WorldTiles_SetIdxAt(s32 x, s32 y)
{
    s32 i;

    for (i = 0; i < sNumRect; i++)
        if (x >= sRect[i].x && x < sRect[i].x + sRect[i].w && y >= sRect[i].y && y < sRect[i].y + sRect[i].h)
            return sRect[i].set;
    return sCur;
}

const struct Tileset *WorldTiles_TilesetAt(s32 x, s32 y)
{
    s32 i;

    for (i = 0; i < sNumRect; i++)
        if (x >= sRect[i].x && x < sRect[i].x + sRect[i].w && y >= sRect[i].y && y < sRect[i].y + sRect[i].h)
            return sRect[i].ts;
    return NULL;
}

const struct Tileset *WorldTiles_SetTileset(u8 si)
{
    return sSet[si].ts;
}

void WorldTiles_Translate(const u16 *src, u16 *dst, u8 si)
{
    s32 i;

    if (!sActive || sSet[si].ts == NULL)
    {
        for (i = 0; i < NUM_TILES_PER_METATILE; i++)
            dst[i] = src[i];
        return;
    }
    if (!sRefValid)
        WtBuildRef();
    for (i = 0; i < NUM_TILES_PER_METATILE; i++)
    {
        u16 e = src[i];
        u16 tile = e & 0x3FF;
        u16 pal = e >> 12;

        if (tile >= NUM_TILES_IN_PRIMARY && !(si == sCur && tile >= NUM_TILES_IN_PRIMARY + WT_SLOTS))
            tile = NUM_TILES_IN_PRIMARY + WtTileSlot(si, tile - NUM_TILES_IN_PRIMARY);
        if (pal >= NUM_PALS_IN_PRIMARY && pal < NUM_PALS_TOTAL)
            pal = NUM_PALS_IN_PRIMARY + WtPalSlot(si, pal - NUM_PALS_IN_PRIMARY);
        dst[i] = (e & 0x0C00) | tile | (pal << 12);
    }
}
