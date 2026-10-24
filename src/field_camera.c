#include "global.h"
#include "world_tiles.h"
#include "berry.h"
#include "bike.h"
#include "field_camera.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "event_object_movement.h"
#include "gpu_regs.h"
#include "menu.h"
#include "overworld.h"
#include "rotating_gate.h"
#include "sprite.h"
#include "text.h"

EWRAM_DATA bool8 gUnusedBikeCameraAheadPanback = FALSE;

struct FieldCameraOffset
{
    u16 xPixelOffset; // u16: a 64-tile-wide BG (widescreen) scrolls over 512 pixels
    u8 yPixelOffset;
    u8 xTileOffset;
    u8 yTileOffset;
    bool8 copyBGToVRAM;
};

static void RedrawMapSliceNorth(struct FieldCameraOffset *, const struct MapLayout *);
static void RedrawMapSliceSouth(struct FieldCameraOffset *, const struct MapLayout *);
static void RedrawMapSliceEast(struct FieldCameraOffset *, const struct MapLayout *);
static void RedrawMapSliceWest(struct FieldCameraOffset *, const struct MapLayout *);
static s32 MapPosToBgTilemapOffset(struct FieldCameraOffset *, s32, s32);
static void DrawWholeMapViewInternal(int, int, const struct MapLayout *);
static void DrawMetatileAt(const struct MapLayout *, u32, int, int);
static void DrawMetatile(s32, const u16 *, u32);
static void CameraPanningCB_PanAhead(void);

static struct FieldCameraOffset sFieldCameraOffset;
static s16 sHorizontalCameraPan;
static s16 sVerticalCameraPan;
static bool8 sBikeCameraPanFlag;
static void (*sFieldCameraPanningCallback)(void);

COMMON_DATA struct CameraObject gFieldCamera = {0};
COMMON_DATA u16 gTotalCameraPixelOffsetY = 0;
COMMON_DATA u16 gTotalCameraPixelOffsetX = 0;

static void ResetCameraOffset(struct FieldCameraOffset *cameraOffset)
{
    cameraOffset->xTileOffset = 0;
    cameraOffset->yTileOffset = 0;
    cameraOffset->xPixelOffset = 0;
    cameraOffset->yPixelOffset = 0;
    cameraOffset->copyBGToVRAM = TRUE;
}

// The BG tilemaps hold a ring of metatiles around the camera: 16x16 normally (32x32 tiles), 32x16 in widescreen
// (64x32 tiles), reaching RING_EXT metatiles left of the camera so the wider picture never shows a hole.
static EWRAM_DATA u8 sRingTiles = 0;    // tilemap width in tiles (64 in widescreen, 0/32 normally)
static EWRAM_DATA u8 sRingExt = 0;      // metatiles of the ring left of the camera (pos.x)
#define RING_W   ((s32)(sRingTiles == 64 ? 64 : 32))
#define RING_MT  (RING_W / 2)
#define RING_EXT ((s32)sRingExt)

void FieldCamera_SetWide(bool8 wide)
{
    u8 tiles = wide ? 64 : 32;
    if (RING_W != tiles)
    {
        // a different ring: start it afresh (the whole view is redrawn when the field comes up)
        sFieldCameraOffset.xTileOffset = 0;
        sFieldCameraOffset.xPixelOffset = 0;
    }
    sRingTiles = tiles;
    sRingExt = wide ? 8 : 0;
}

static inline s32 RingCol(s32 xTileOffset, s32 j)
{
    s32 c = (xTileOffset + j * 2) % RING_W;
    return c < 0 ? c + RING_W : c;
}

// tilemap index of tile (col, row): a 64-tile-wide BG is two 32x32 screen blocks side by side
static inline u32 TileIdx(s32 col, s32 row)
{
    return ((col & 32) << 5) + row * 32 + (col & 31);
}

static void AddCameraTileOffset(struct FieldCameraOffset *cameraOffset, u32 xOffset, u32 yOffset)
{
    cameraOffset->xTileOffset += xOffset;
    cameraOffset->xTileOffset %= RING_W;
    cameraOffset->yTileOffset += yOffset;
    cameraOffset->yTileOffset %= 32;
}

static void AddCameraPixelOffset(struct FieldCameraOffset *cameraOffset, u32 xOffset, u32 yOffset)
{
    cameraOffset->xPixelOffset += xOffset;
    cameraOffset->yPixelOffset += yOffset;
}

void ResetFieldCamera(void)
{
    ResetCameraOffset(&sFieldCameraOffset);
}

void FieldUpdateBgTilemapScroll(void)
{
    u32 r4, r5;
    r5 = sFieldCameraOffset.xPixelOffset + sHorizontalCameraPan;
    r4 = sVerticalCameraPan + sFieldCameraOffset.yPixelOffset + 8;

    SetGpuReg(REG_OFFSET_BG1HOFS, r5);
    SetGpuReg(REG_OFFSET_BG1VOFS, r4);
    SetGpuReg(REG_OFFSET_BG2HOFS, r5);
    SetGpuReg(REG_OFFSET_BG2VOFS, r4);
    SetGpuReg(REG_OFFSET_BG3HOFS, r5);
    SetGpuReg(REG_OFFSET_BG3VOFS, r4);
}

void GetCameraOffsetWithPan(s16 *x, s16 *y)
{
    *x = sFieldCameraOffset.xPixelOffset + sHorizontalCameraPan;
    *y = sFieldCameraOffset.yPixelOffset + sVerticalCameraPan + 8;
}

void DrawWholeMapView(void)
{
    WorldTiles_BeginFullRedraw();
    DrawWholeMapViewInternal(gSaveBlock1Ptr->pos.x, gSaveBlock1Ptr->pos.y, gMapHeader.mapLayout);
    sFieldCameraOffset.copyBGToVRAM = TRUE;
}

static void DrawWholeMapViewInternal(int x, int y, const struct MapLayout *mapLayout)
{
    s32 i, j;
    u32 row;

    for (i = 0; i < 32; i += 2)
    {
        row = (sFieldCameraOffset.yTileOffset + i) % 32;
        for (j = -RING_EXT; j < RING_MT - RING_EXT; j++)
            DrawMetatileAt(mapLayout, TileIdx(RingCol(sFieldCameraOffset.xTileOffset, j), row), x + j, y + i / 2);
    }
}

static void RedrawMapSlicesForCameraUpdate(struct FieldCameraOffset *cameraOffset, int x, int y)
{
    const struct MapLayout *mapLayout = gMapHeader.mapLayout;

    if (x > 0)
        RedrawMapSliceWest(cameraOffset, mapLayout);
    if (x < 0)
        RedrawMapSliceEast(cameraOffset, mapLayout);
    if (y > 0)
        RedrawMapSliceNorth(cameraOffset, mapLayout);
    if (y < 0)
        RedrawMapSliceSouth(cameraOffset, mapLayout);
    cameraOffset->copyBGToVRAM = TRUE;
}

static void RedrawMapSliceNorth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    s32 j;
    u32 row = (cameraOffset->yTileOffset + 28) % 32;

    for (j = -RING_EXT; j < RING_MT - RING_EXT; j++)
        DrawMetatileAt(mapLayout, TileIdx(RingCol(cameraOffset->xTileOffset, j), row), gSaveBlock1Ptr->pos.x + j, gSaveBlock1Ptr->pos.y + 14);
}

static void RedrawMapSliceSouth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    s32 j;
    u32 row = cameraOffset->yTileOffset;

    for (j = -RING_EXT; j < RING_MT - RING_EXT; j++)
        DrawMetatileAt(mapLayout, TileIdx(RingCol(cameraOffset->xTileOffset, j), row), gSaveBlock1Ptr->pos.x + j, gSaveBlock1Ptr->pos.y);
}

// the camera moved left: the column entering the ring on the left
static void RedrawMapSliceEast(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    s32 i;
    u32 col = RingCol(cameraOffset->xTileOffset, -RING_EXT);

    for (i = 0; i < 32; i += 2)
        DrawMetatileAt(mapLayout, TileIdx(col, (cameraOffset->yTileOffset + i) % 32), gSaveBlock1Ptr->pos.x - RING_EXT, gSaveBlock1Ptr->pos.y + i / 2);
}

// the camera moved right: the column entering the ring on the right
static void RedrawMapSliceWest(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    s32 i, j = RING_MT - 2 - RING_EXT;
    u32 col = RingCol(cameraOffset->xTileOffset, j);

    for (i = 0; i < 32; i += 2)
        DrawMetatileAt(mapLayout, TileIdx(col, (cameraOffset->yTileOffset + i) % 32), gSaveBlock1Ptr->pos.x + j, gSaveBlock1Ptr->pos.y + i / 2);
}

void CurrentMapDrawMetatileAt(int x, int y)
{
    int offset = MapPosToBgTilemapOffset(&sFieldCameraOffset, x, y);

    if (offset >= 0)
    {
        DrawMetatileAt(gMapHeader.mapLayout, offset, x, y);
        sFieldCameraOffset.copyBGToVRAM = TRUE;
    }
}

void DrawDoorMetatileAt(int x, int y, u16 *tiles)
{
    int offset = MapPosToBgTilemapOffset(&sFieldCameraOffset, x, y);

    if (offset >= 0)
    {
        u16 tmp[NUM_TILES_PER_METATILE];

        WorldTiles_Translate(tiles, tmp, WorldTiles_SetIdxAt(x + 0, y + 0));
        DrawMetatile(METATILE_LAYER_TYPE_COVERED, tmp, offset);
        sFieldCameraOffset.copyBGToVRAM = TRUE;
    }
}

static void DrawMetatileAt(const struct MapLayout *mapLayout, u32 offset, int x, int y)
{
    u16 metatileId = MapGridGetMetatileIdAt(x, y);
    const u16 *metatiles;
    u16 tmp[NUM_TILES_PER_METATILE];
    u8 si = WorldTiles_SetIdxAt(x, y);

    if (metatileId > NUM_METATILES_TOTAL)
        metatileId = 0;
    if (metatileId < NUM_METATILES_IN_PRIMARY)
    {
        metatiles = mapLayout->primaryTileset->metatiles;
    }
    else
    {
        const struct Tileset *ts = WorldTiles_TilesetAt(x, y);

        if (ts == NULL || ts->isSecondary != TRUE)
            ts = mapLayout->secondaryTileset;
        metatiles = ts->metatiles;
        metatileId -= NUM_METATILES_IN_PRIMARY;
    }
    WorldTiles_Translate(metatiles + metatileId * NUM_TILES_PER_METATILE, tmp, si);
    DrawMetatile(MapGridGetMetatileLayerTypeAt(x, y), tmp, offset);
}

static void DrawMetatile(s32 metatileLayerType, const u16 *tiles, u32 offset)
{
    switch (metatileLayerType)
    {
    case METATILE_LAYER_TYPE_SPLIT:
        // Draw metatile's bottom layer to the bottom background layer.
        gOverworldTilemapBuffer_Bg3[offset] = tiles[0];
        gOverworldTilemapBuffer_Bg3[offset + 1] = tiles[1];
        gOverworldTilemapBuffer_Bg3[offset + 0x20] = tiles[2];
        gOverworldTilemapBuffer_Bg3[offset + 0x21] = tiles[3];

        // Draw transparent tiles to the middle background layer.
        gOverworldTilemapBuffer_Bg2[offset] = 0;
        gOverworldTilemapBuffer_Bg2[offset + 1] = 0;
        gOverworldTilemapBuffer_Bg2[offset + 0x20] = 0;
        gOverworldTilemapBuffer_Bg2[offset + 0x21] = 0;

        // Draw metatile's top layer to the top background layer.
        gOverworldTilemapBuffer_Bg1[offset] = tiles[4];
        gOverworldTilemapBuffer_Bg1[offset + 1] = tiles[5];
        gOverworldTilemapBuffer_Bg1[offset + 0x20] = tiles[6];
        gOverworldTilemapBuffer_Bg1[offset + 0x21] = tiles[7];
        break;
    case METATILE_LAYER_TYPE_COVERED:
        // Draw metatile's bottom layer to the bottom background layer.
        gOverworldTilemapBuffer_Bg3[offset] = tiles[0];
        gOverworldTilemapBuffer_Bg3[offset + 1] = tiles[1];
        gOverworldTilemapBuffer_Bg3[offset + 0x20] = tiles[2];
        gOverworldTilemapBuffer_Bg3[offset + 0x21] = tiles[3];

        // Draw metatile's top layer to the middle background layer.
        gOverworldTilemapBuffer_Bg2[offset] = tiles[4];
        gOverworldTilemapBuffer_Bg2[offset + 1] = tiles[5];
        gOverworldTilemapBuffer_Bg2[offset + 0x20] = tiles[6];
        gOverworldTilemapBuffer_Bg2[offset + 0x21] = tiles[7];

        // Draw transparent tiles to the top background layer.
        gOverworldTilemapBuffer_Bg1[offset] = 0;
        gOverworldTilemapBuffer_Bg1[offset + 1] = 0;
        gOverworldTilemapBuffer_Bg1[offset + 0x20] = 0;
        gOverworldTilemapBuffer_Bg1[offset + 0x21] = 0;
        break;
    case METATILE_LAYER_TYPE_NORMAL:
        // Draw garbage to the bottom background layer.
        gOverworldTilemapBuffer_Bg3[offset] = 0x3014;
        gOverworldTilemapBuffer_Bg3[offset + 1] = 0x3014;
        gOverworldTilemapBuffer_Bg3[offset + 0x20] = 0x3014;
        gOverworldTilemapBuffer_Bg3[offset + 0x21] = 0x3014;

        // Draw metatile's bottom layer to the middle background layer.
        gOverworldTilemapBuffer_Bg2[offset] = tiles[0];
        gOverworldTilemapBuffer_Bg2[offset + 1] = tiles[1];
        gOverworldTilemapBuffer_Bg2[offset + 0x20] = tiles[2];
        gOverworldTilemapBuffer_Bg2[offset + 0x21] = tiles[3];

        // Draw metatile's top layer to the top background layer, which covers object event sprites.
        gOverworldTilemapBuffer_Bg1[offset] = tiles[4];
        gOverworldTilemapBuffer_Bg1[offset + 1] = tiles[5];
        gOverworldTilemapBuffer_Bg1[offset + 0x20] = tiles[6];
        gOverworldTilemapBuffer_Bg1[offset + 0x21] = tiles[7];
        break;
    }
    ScheduleBgCopyTilemapToVram(1);
    ScheduleBgCopyTilemapToVram(2);
    ScheduleBgCopyTilemapToVram(3);
}

static s32 MapPosToBgTilemapOffset(struct FieldCameraOffset *cameraOffset, s32 x, s32 y)
{
    x -= gSaveBlock1Ptr->pos.x;
    if (x < -RING_EXT || x >= RING_MT - RING_EXT)
        return -1;
    x = RingCol(cameraOffset->xTileOffset, x);

    y = (y - gSaveBlock1Ptr->pos.y) * 2;
    if (y >= 32 || y < 0)
        return -1;
    y = y + cameraOffset->yTileOffset;
    if (y >= 32)
        y -= 32;

    return TileIdx(x, y);
}

static void CameraUpdateCallback(struct CameraObject *fieldCamera)
{
    if (fieldCamera->spriteId != 0)
    {
        fieldCamera->movementSpeedX = gSprites[fieldCamera->spriteId].sCamera_MoveX;
        fieldCamera->movementSpeedY = gSprites[fieldCamera->spriteId].sCamera_MoveY;
    }
}

void ResetCameraUpdateInfo(void)
{
    gFieldCamera.movementSpeedX = 0;
    gFieldCamera.movementSpeedY = 0;
    gFieldCamera.x = 0;
    gFieldCamera.y = 0;
    gFieldCamera.spriteId = 0;
    gFieldCamera.callback = NULL;
}

u32 InitCameraUpdateCallback(u8 trackedSpriteId)
{
    if (gFieldCamera.spriteId != 0)
        DestroySprite(&gSprites[gFieldCamera.spriteId]);
    gFieldCamera.spriteId = AddCameraObject(trackedSpriteId);
    gFieldCamera.callback = CameraUpdateCallback;
    return 0;
}

void CameraUpdate(void)
{
    int deltaX;
    int deltaY;
    int curMovementOffsetY;
    int curMovementOffsetX;
    int movementSpeedX;
    int movementSpeedY;

    if (gFieldCamera.callback != NULL)
        gFieldCamera.callback(&gFieldCamera);
    movementSpeedX = gFieldCamera.movementSpeedX;
    movementSpeedY = gFieldCamera.movementSpeedY;
    deltaX = 0;
    deltaY = 0;
    curMovementOffsetX = gFieldCamera.x;
    curMovementOffsetY = gFieldCamera.y;


    if (curMovementOffsetX == 0 && movementSpeedX != 0)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    if (curMovementOffsetY == 0 && movementSpeedY != 0)
    {
        if (movementSpeedY > 0)
            deltaY = 1;
        else
            deltaY = -1;
    }
    if (curMovementOffsetX != 0 && curMovementOffsetX == -movementSpeedX)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    if (curMovementOffsetY != 0 && curMovementOffsetY == -movementSpeedY)
    {
        if (movementSpeedY > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }

    gFieldCamera.x += movementSpeedX;
    gFieldCamera.x %= 16;
    gFieldCamera.y += movementSpeedY;
    gFieldCamera.y %= 16;

    if (deltaX != 0 || deltaY != 0)
    {
        CameraMove(deltaX, deltaY);
        UpdateObjectEventsForCameraUpdate(deltaX, deltaY);
        RotatingGatePuzzleCameraUpdate(deltaX, deltaY);
        SetBerryTreesSeen();
        AddCameraTileOffset(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
        RedrawMapSlicesForCameraUpdate(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
        if (WorldTiles_TakeRedraw())
            DrawWholeMapView();
    }

    AddCameraPixelOffset(&sFieldCameraOffset, movementSpeedX, movementSpeedY);
    gTotalCameraPixelOffsetX -= movementSpeedX;
    gTotalCameraPixelOffsetY -= movementSpeedY;
}

void MoveCameraAndRedrawMap(int deltaX, int deltaY) //unused
{
    CameraMove(deltaX, deltaY);
    UpdateObjectEventsForCameraUpdate(deltaX, deltaY);
    DrawWholeMapView();
    gTotalCameraPixelOffsetX -= deltaX * 16;
    gTotalCameraPixelOffsetY -= deltaY * 16;
}

void SetCameraPanningCallback(void (*callback)(void))
{
    sFieldCameraPanningCallback = callback;
}

void SetCameraPanning(s16 horizontal, s16 vertical)
{
    sHorizontalCameraPan = horizontal;
    sVerticalCameraPan = vertical + 32;
}

void InstallCameraPanAheadCallback(void)
{
    sFieldCameraPanningCallback = CameraPanningCB_PanAhead;
    sBikeCameraPanFlag = FALSE;
    sHorizontalCameraPan = 0;
    sVerticalCameraPan = 32;
}

void UpdateCameraPanning(void)
{
    if (sFieldCameraPanningCallback != NULL)
        sFieldCameraPanningCallback();
    //Update sprite offset of overworld objects
    gSpriteCoordOffsetX = gTotalCameraPixelOffsetX - sHorizontalCameraPan;
    gSpriteCoordOffsetY = gTotalCameraPixelOffsetY - sVerticalCameraPan - 8;
}

static void CameraPanningCB_PanAhead(void)
{
    u8 var;

    if (gUnusedBikeCameraAheadPanback == FALSE)
    {
        InstallCameraPanAheadCallback();
    }
    else
    {
        // this code is never reached
        if (gPlayerAvatar.tileTransitionState == T_TILE_TRANSITION)
        {
            sBikeCameraPanFlag ^= 1;
            if (sBikeCameraPanFlag == FALSE)
                return;
        }
        else
        {
            sBikeCameraPanFlag = FALSE;
        }

        var = GetPlayerMovementDirection();
        if (var == 2)
        {
            if (sVerticalCameraPan > -8)
                sVerticalCameraPan -= 2;
        }
        else if (var == 1)
        {
            if (sVerticalCameraPan < 72)
                sVerticalCameraPan += 2;
        }
        else if (sVerticalCameraPan < 32)
        {
            sVerticalCameraPan += 2;
        }
        else if (sVerticalCameraPan > 32)
        {
            sVerticalCameraPan -= 2;
        }
    }
}
