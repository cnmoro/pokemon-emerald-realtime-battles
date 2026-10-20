#ifndef GUARD_WORLD_TILES_H
#define GUARD_WORLD_TILES_H

void WorldTiles_ClearRects(void);
void WorldTiles_AddRect(s32 x, s32 y, s32 w, s32 h, const struct Tileset *ts);
void WorldTiles_LoadSecondaryDirect(const struct Tileset *ts);
void WorldTiles_ResetIdentity(void);
void WorldTiles_Cross(void);
bool8 WorldTiles_TakeRedraw(void);
void WorldTiles_BeginFullRedraw(void);
void WorldTiles_FrameEnd(void);
u8 WorldTiles_SetIdxAt(s32 x, s32 y);
const struct Tileset *WorldTiles_TilesetAt(s32 x, s32 y);
const struct Tileset *WorldTiles_SetTileset(u8 si);
void WorldTiles_Translate(const u16 *src, u16 *dst, u8 si);

#endif
