// Follower POKeMON: the first POKeMON of the party walks behind the player in the overworld.
// It is a free sprite (not an object event) drawn with the real-time battle sprites (PMD walk/idle
// animations, 8 directions) and placed on the path the player walked, 16 px behind.
#include "global.h"
#include "follower.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "field_weather.h"
#include "palette.h"
#include "pokemon.h"
#include "sprite.h"
#include "rt_internal.h"
#include "constants/species.h"

#define TAG_FOLLOWER 0x5A01
#define HIST_LEN 32
#define FOLLOW_DIST 18

struct FollowerHist { s16 x, y; };

static EWRAM_DATA u8 sSpriteId = MAX_SPRITES;
static EWRAM_DATA u16 sGfxDex = 0;
static EWRAM_DATA u8 sShiny = 0;
static EWRAM_DATA u16 sCurFrame = 0xFFFF;
static EWRAM_DATA struct FollowerHist sHist[HIST_LEN] = {0};
static EWRAM_DATA u8 sHistHead = 0;
static EWRAM_DATA u8 sAnim = 0, sAnimFrame = 0, sAnimTimer = 0, sFacing = 0, sStill = 0;
static EWRAM_DATA s16 sLastPx = 0, sLastPy = 0;
static EWRAM_DATA u16 sTileNum = 0;
static EWRAM_DATA u8 sMatrix = 0xFF;
#define FOLLOW_SCALE 160 // drawn at 160/256 of the battle sprite size (closer to the overworld scale)

static void SpriteCB_Follower(struct Sprite *sprite)
{
}

static const struct SpriteTemplate sFollowerTemplate =
{
    .tileTag = TAG_FOLLOWER,
    .paletteTag = TAG_FOLLOWER,
    .oam = &gDummyOamData,
    .anims = gDummySpriteAnimTable,
    .images = NULL,
    .affineAnims = gDummySpriteAffineAnimTable,
    .callback = SpriteCB_Follower,
};

static struct Pokemon *LeadMon(void)
{
    u8 i;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 sp = GetMonData(&gPlayerParty[i], MON_DATA_SPECIES_OR_EGG);
        if (sp == SPECIES_NONE)
            return NULL;
        if (sp == SPECIES_EGG)
            continue;
        if (GetMonData(&gPlayerParty[i], MON_DATA_HP) == 0)
            continue;
        return &gPlayerParty[i];
    }
    return NULL;
}

static bool8 SpriteAlive(void)
{
    return sSpriteId < MAX_SPRITES && gSprites[sSpriteId].inUse && gSprites[sSpriteId].callback == SpriteCB_Follower;
}

void Follower_Hide(void)
{
    if (SpriteAlive())
        gSprites[sSpriteId].invisible = TRUE;
}

static void ResetHistory(s16 x, s16 y, u8 dir)
{
    s32 i;
    s16 bx = x, by = y;
    // start one step behind the player
    switch (dir)
    {
    case DIR_SOUTH: by -= FOLLOW_DIST; break;
    case DIR_NORTH: by += FOLLOW_DIST; break;
    case DIR_WEST:  bx += FOLLOW_DIST; break;
    case DIR_EAST:  bx -= FOLLOW_DIST; break;
    }
    for (i = 0; i < HIST_LEN; i++)
    {
        sHist[i].x = bx;
        sHist[i].y = by;
    }
    sHistHead = 0;
    sHist[0].x = x;
    sHist[0].y = y;
    sLastPx = x;
    sLastPy = y;
}

static bool8 CreateFollower(struct Pokemon *mon)
{
    u8 pal;
    s16 tiles;
    {
        // 64 tiles reserved under the follower tag (the content is replaced right away)
        struct SpriteSheet sheet = {(const void *)gRtSpriteGfx, 64 * TILE_SIZE_4BPP, TAG_FOLLOWER};
        tiles = GetSpriteTileStartByTag(TAG_FOLLOWER);
        if (tiles == (s16)0xFFFF)
            tiles = LoadSpriteSheet(&sheet);
    }
    if (tiles <= 0)
        return FALSE;
    sSpriteId = CreateSprite(&sFollowerTemplate, 0, 0, 0);
    if (sSpriteId >= MAX_SPRITES)
        return FALSE;
    sTileNum = tiles;
    pal = IndexOfSpritePaletteTag(TAG_FOLLOWER);
    if (pal == 0xFF)
        pal = AllocSpritePalette(TAG_FOLLOWER);
    gSprites[sSpriteId].oam.paletteNum = pal;
    gSprites[sSpriteId].oam.tileNum = sTileNum;
    gSprites[sSpriteId].coordOffsetEnabled = TRUE;
    gSprites[sSpriteId].centerToCornerVecX = 0;
    gSprites[sSpriteId].centerToCornerVecY = 0;
    gSprites[sSpriteId].invisible = TRUE;
    sMatrix = AllocOamMatrix();
    sGfxDex = 0xFFFF;
    sCurFrame = 0xFFFF;
    return TRUE;
}

static void LoadMonPalette(struct Pokemon *mon, u16 dex, bool8 shiny)
{
    u8 pal = gSprites[sSpriteId].oam.paletteNum;
    LoadPalette(gRtSpeciesPal[dex][shiny ? 1 : 0], OBJ_PLTT_ID(pal), PLTT_SIZE_4BPP);
    UpdateSpritePaletteWithWeather(pal);
}

// point on the walked path FOLLOW_DIST px behind the player
static void PathPoint(s16 *ox, s16 *oy)
{
    s32 left = FOLLOW_DIST, k;
    u8 i = sHistHead;
    s16 x = sHist[i].x, y = sHist[i].y;
    for (k = 1; k < HIST_LEN; k++)
    {
        u8 j = (sHistHead + HIST_LEN - k) % HIST_LEN;
        s32 dx = sHist[j].x - x, dy = sHist[j].y - y;
        s32 d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (d >= left)
        {
            x += dx * left / d;
            y += dy * left / d;
            left = 0;
            break;
        }
        left -= d;
        x = sHist[j].x;
        y = sHist[j].y;
    }
    *ox = x;
    *oy = y;
}

static u8 DirTo8(s32 dx, s32 dy)
{
    // RT facing: 0 down, 1 down-right, 2 right, 3 up-right, 4 up, 5 up-left, 6 left, 7 down-left
    if (dx == 0 && dy == 0)
        return 0xFF;
    if (dx == 0)
        return dy > 0 ? 0 : 4;
    if (dy == 0)
        return dx > 0 ? 2 : 6;
    if (dx > 0)
        return dy > 0 ? 1 : 3;
    return dy > 0 ? 7 : 5;
}

void Follower_Update(void)
{
    struct Pokemon *mon;
    struct ObjectEvent *pl;
    struct Sprite *ps, *fs;
    s16 px, py, fx, fy;
    u16 dex, idx;
    bool8 hf = FALSE;
    const struct RtSpeciesGfx *g;
    const struct RtFrame *f;
    u8 dir, row, cnt, shape, size;

    if (gPlayerAvatar.spriteId >= MAX_SPRITES || !gSprites[gPlayerAvatar.spriteId].inUse)
        return;
    mon = LeadMon();
    pl = &gObjectEvents[gPlayerAvatar.objectEventId];
    ps = &gSprites[gPlayerAvatar.spriteId];
    if (!mon || (gPlayerAvatar.flags & (PLAYER_AVATAR_FLAG_SURFING | PLAYER_AVATAR_FLAG_UNDERWATER)) || ps->invisible || pl->invisible)
    {
        Follower_Hide();
        if (mon)
            ResetHistory(ps->x, ps->y + 12, pl->facingDirection);
        return;
    }
    if (!SpriteAlive())
    {
        if (!CreateFollower(mon))
            return;
        ResetHistory(ps->x, ps->y + 12, pl->facingDirection);
    }
    fs = &gSprites[sSpriteId];
    dex = SpeciesToNationalPokedexNum(GetMonData(mon, MON_DATA_SPECIES));
    if (dex == 0 || dex > 386)
    {
        fs->invisible = TRUE;
        return;
    }
    if (dex != sGfxDex || IsMonShiny(mon) != sShiny)
    {
        sGfxDex = dex;
        sShiny = IsMonShiny(mon);
        sCurFrame = 0xFFFF;
        LoadMonPalette(mon, dex, sShiny);
    }

    // path of the player's feet
    px = ps->x;
    py = ps->y + 12;
    if (px - sLastPx > 32 || sLastPx - px > 32 || py - sLastPy > 32 || sLastPy - py > 32)
        ResetHistory(px, py, pl->facingDirection); // warp / map change
    if (px != sLastPx || py != sLastPy)
    {
        sHistHead = (sHistHead + 1) % HIST_LEN;
        sHist[sHistHead].x = px;
        sHist[sHistHead].y = py;
        sStill = 0;
    }
    else if (sStill < 255)
    {
        sStill++;
    }
    sLastPx = px;
    sLastPy = py;

    PathPoint(&fx, &fy);
    dir = DirTo8(fx - fs->data[0], fy - fs->data[1]);
    if (dir != 0xFF)
        sFacing = dir;
    else if (sStill > 8)
    {
        // standing still: look at the player
        u8 d2 = DirTo8(px - fx, py - fy);
        if (d2 != 0xFF)
            sFacing = d2;
    }
    {
        bool8 moving = fx != fs->data[0] || fy != fs->data[1];
        u8 anim = moving ? RTA_WALK : RTA_IDLE;
        if (anim != sAnim)
        {
            sAnim = anim;
            sAnimFrame = 0;
            sAnimTimer = 0;
        }
    }
    fs->data[0] = fx;
    fs->data[1] = fy;

    g = &gRtSpeciesGfx[dex];
    cnt = g->count[sAnim] ? g->count[sAnim] : 1;
    if (sFacing <= 4)
        row = sFacing;
    else
    {
        row = 8 - sFacing;
        hf = TRUE;
    }
    if (sAnimFrame >= cnt)
        sAnimFrame = 0;
    idx = g->start[sAnim] + row * cnt + sAnimFrame;
    f = &gRtFrames[idx];
    if (++sAnimTimer >= (f->dur ? f->dur : 4))
    {
        sAnimTimer = 0;
        sAnimFrame = (sAnimFrame + 1) % cnt;
    }
    shape = (f->shapeSize >> 2) & 3;
    size = f->shapeSize & 3;
    if (idx != sCurFrame)
    {
        sCurFrame = idx;
        LZ77UnCompVram((const u32 *)((const u8 *)gRtSpriteGfx + f->offset), (void *)(OBJ_VRAM0 + sTileNum * TILE_SIZE_4BPP));
    }
    {
        static const u8 sW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
        static const u8 sH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
        u8 w = sW[shape][size], h = sH[shape][size];
        s32 cx = hf ? -(f->x + w / 2) : f->x + w / 2, cy = f->y + h / 2;
        fs->oam.shape = shape;
        fs->oam.size = size;
        if (sMatrix < 32)
        {
            // shrunk around the feet with an affine (double size) sprite
            s16 inv = 256 * 256 / FOLLOW_SCALE;
            SetOamMatrix(sMatrix, hf ? -inv : inv, 0, 0, inv);
            fs->oam.affineMode = ST_OAM_AFFINE_DOUBLE;
            fs->oam.matrixNum = sMatrix;
            fs->x = fx + cx * FOLLOW_SCALE / 256 - w;
            fs->y = fy + cy * FOLLOW_SCALE / 256 - h;
        }
        else
        {
            fs->oam.affineMode = ST_OAM_AFFINE_OFF;
            fs->oam.matrixNum = hf ? ST_OAM_HFLIP : 0;
            fs->x = hf ? fx - (f->x + w) : fx + f->x;
            fs->y = fy + f->y;
        }
    }
    fs->oam.priority = ps->oam.priority;
    // drawn behind the player when above it on screen, in front when below
    fs->subpriority = (fy < py) ? ps->subpriority + 1 : (ps->subpriority ? ps->subpriority - 1 : 0);
    fs->invisible = FALSE;
}

// walking with the follower: the lead POKeMON grows fond of the player a little faster
void Follower_OnStep(void)
{
    struct Pokemon *mon = LeadMon();
    static EWRAM_DATA u8 sSteps = 0;
    if (!mon)
        return;
    if (++sSteps >= 32)
    {
        u8 fr = GetMonData(mon, MON_DATA_FRIENDSHIP);
        sSteps = 0;
        if (fr < MAX_FRIENDSHIP)
        {
            fr++;
            SetMonData(mon, MON_DATA_FRIENDSHIP, &fr);
        }
    }
}
