// Real-time battle visual effects: OAM management, particles, palettes, projectile rendering, weather.
#include "global.h"
#include "rt_internal.h"
#include "decompress.h"
#include "gpu_regs.h"
#include "main.h"
#include "palette.h"
#include "random.h"
#include "sound.h"
#include "constants/moves.h"
#include "constants/songs.h"

#include "data/rt/fx_data.h"

// ---------------------------------------------------------------------------
// OAM buckets
// ---------------------------------------------------------------------------
#define BUCKETS 4
#define BUCKET_MAX 96

struct OamEntry
{
    u16 a0, a1, a2;
};

static EWRAM_DATA struct OamEntry sBucket[BUCKETS][BUCKET_MAX] = {0};
static EWRAM_DATA u8 sBucketCount[BUCKETS] = {0};
static EWRAM_DATA struct OamData sScratchOam = {0};
static EWRAM_DATA u8 sScratchBucket = 0;
static EWRAM_DATA bool8 sScratchPending = FALSE;

void RtOam_Reset(void)
{
    s32 i;
    for (i = 0; i < BUCKETS; i++)
        sBucketCount[i] = 0;
    sScratchPending = FALSE;
}

static void FlushScratch(void)
{
    if (sScratchPending)
    {
        u16 *s = (u16 *)&sScratchOam;
        if (sBucketCount[sScratchBucket] < BUCKET_MAX)
        {
            struct OamEntry *e = &sBucket[sScratchBucket][sBucketCount[sScratchBucket]++];
            e->a0 = s[0];
            e->a1 = s[1];
            e->a2 = s[2];
        }
        sScratchPending = FALSE;
    }
}

// Returns scratch storage: the caller writes attr0..2, it is queued on the next call/commit.
struct OamData *RtOam_Next(u8 bucket)
{
    FlushScratch();
    if (bucket >= BUCKETS)
        bucket = BUCKETS - 1;
    if (sBucketCount[bucket] >= BUCKET_MAX)
        return NULL;
    sScratchBucket = bucket;
    sScratchPending = TRUE;
    return &sScratchOam;
}

void RtOam_Commit(void)
{
    s32 b, i, n = 0;
    FlushScratch();
    for (b = 0; b < BUCKETS; b++)
    {
        for (i = 0; i < sBucketCount[b] && n < 128; i++, n++)
        {
            u16 *o = (u16 *)&gMain.oamBuffer[n];
            o[0] = sBucket[b][i].a0;
            o[1] = sBucket[b][i].a1;
            o[2] = sBucket[b][i].a2;
        }
    }
    for (; n < 128; n++)
    {
        u16 *o = (u16 *)&gMain.oamBuffer[n];
        o[0] = 0x0200; // disabled
        o[1] = 0;
        o[2] = 0;
    }
}

s16 RtWorldToScreenX(s32 fx)
{
    return FROM_FX(fx) - gRt->camX;
}

s16 RtWorldToScreenY(s32 fx)
{
    return FROM_FX(fx) - gRt->camY;
}

// ---------------------------------------------------------------------------
// Affine matrices
// ---------------------------------------------------------------------------
void RtFx_ResetAffine(void)
{
    gRt->affineUsed = 0;
}

u8 RtFx_AllocAffine(u8 angle, u8 scaleQ4, bool8 hflip)
{
    u8 idx;
    s32 c, s, sc;
    if (gRt->affineUsed >= 32)
        return 0xFF;
    idx = gRt->affineUsed++;
    if (scaleQ4 == 0)
        scaleQ4 = 1;
    // inverse matrix: rotate by -angle, scale by 1/scale
    c = RtCos(angle);
    s = RtSin(angle);
    sc = 16 * 256 / scaleQ4; // Q8 inverse scale
    gMain.oamBuffer[idx * 4 + 0].affineParam = (c * sc / 256) * (hflip ? -1 : 1);
    gMain.oamBuffer[idx * 4 + 1].affineParam = (s * sc / 256);
    gMain.oamBuffer[idx * 4 + 2].affineParam = (-s * sc / 256) * (hflip ? -1 : 1);
    gMain.oamBuffer[idx * 4 + 3].affineParam = (c * sc / 256);
    return idx;
}

// ---------------------------------------------------------------------------
// Palettes
// ---------------------------------------------------------------------------
#define FX_PAL_SLOTS 11
#define FX_PAL_FIRST 5

u8 RtFx_GetPalSlot(u8 fxPal)
{
    s32 i, pick = -1;
    u32 bestAge = 0;
    if (fxPal == FXP_UI || fxPal >= FXP_COUNT)
        return 4;
    for (i = 0; i < FX_PAL_SLOTS; i++)
    {
        if (gRt->fxPalOwner[i] == fxPal + 1)
        {
            gRt->fxPalLastUse[i] = gRt->frame;
            return FX_PAL_FIRST + i;
        }
    }
    // free slot, else the least recently used one that was not drawn this frame or the previous one
    for (i = 0; i < FX_PAL_SLOTS; i++)
    {
        u32 age;
        if (gRt->fxPalOwner[i] == 0)
        {
            pick = i;
            break;
        }
        age = (u16)((u16)gRt->frame - gRt->fxPalLastUse[i]);
        if (age >= 2 && age > bestAge)
        {
            bestAge = age;
            pick = i;
        }
    }
    if (pick < 0)
    {
        // everything is in use right now: borrow the white palette's slot if present, else slot 0
        for (i = 0; i < FX_PAL_SLOTS; i++)
            if (gRt->fxPalOwner[i] == FXP_WHITE + 1)
                return FX_PAL_FIRST + i;
        return FX_PAL_FIRST;
    }
    gRt->fxPalOwner[pick] = fxPal + 1;
    gRt->fxPalLastUse[pick] = gRt->frame;
    LoadPalette(gRtFxPalettes[fxPal], OBJ_PLTT_ID(FX_PAL_FIRST + pick), PLTT_SIZE_4BPP);
    return FX_PAL_FIRST + pick;
}

void RtFx_Init(void)
{
    s32 i;
    // UI tiles are generated at runtime by rt_ui.c; atlas goes after them
    CpuCopy32(gRtFxAtlasGfx, (void *)(OBJ_VRAM0 + RT_FX_TILE_BASE * TILE_SIZE_4BPP), RT_FX_ATLAS_TILES * TILE_SIZE_4BPP);
    LoadPalette(gRtUiPalette, OBJ_PLTT_ID(4), PLTT_SIZE_4BPP);
    for (i = 0; i < FX_PAL_SLOTS; i++)
        gRt->fxPalOwner[i] = 0;
    for (i = 0; i < RT_MAX_PART; i++)
        gRt->parts[i].life = 0;
}

// ---------------------------------------------------------------------------
// Particles
// ---------------------------------------------------------------------------
struct RtPart *RtFx_Spawn(u8 shape, u8 pal, s32 x, s32 y, u8 life)
{
    s32 i;
    struct RtPart *p = NULL;
    u8 minLife = 255;
    if (shape == FX_NONE || shape >= FX_COUNT)
        return NULL;
    for (i = 0; i < RT_MAX_PART; i++)
    {
        if (gRt->parts[i].life == 0)
        {
            p = &gRt->parts[i];
            break;
        }
        if (gRt->parts[i].life < minLife && !(gRt->parts[i].flags & PF_HUD))
        {
            minLife = gRt->parts[i].life;
            p = &gRt->parts[i];
        }
    }
    if (p == NULL)
        return NULL;
    memset(p, 0, sizeof(*p));
    p->shape = shape;
    p->pal = pal;
    p->x = x;
    p->y = y;
    p->life = life ? life : 1;
    p->maxLife = p->life;
    p->scale = 16;
    p->prio = 1;
    {
        u8 frames = gRtFxShapes[shape].frames;
        p->frameRate = frames > 1 ? (p->life / frames ? p->life / frames : 1) : 0;
        if (frames > 1 && p->frameRate > 6)
            p->frameRate = 6;
    }
    return p;
}

void RtFx_Burst(u8 shape, u8 pal, s32 x, s32 y, u8 n, u8 speed, u8 life)
{
    while (n--)
    {
        struct RtPart *p = RtFx_Spawn(shape, pal, x, y, life - (Random() % (life / 3 + 1)));
        u8 a = Random();
        s32 sp = speed * 256 / 2 + (Random() % (speed * 128 + 1));
        if (!p)
            return;
        p->vx = RtCos(a) * sp / 256;
        p->vy = RtSin(a) * sp / 256;
    }
}

void RtFx_Ring(u8 shape, u8 pal, s32 x, s32 y, u8 n, u8 radius, u8 life)
{
    u8 i;
    for (i = 0; i < n; i++)
    {
        u8 a = i * 256 / n;
        struct RtPart *p = RtFx_Spawn(shape, pal, x + RtCos(a) * radius, y + RtSin(a) * radius / 2, life);
        if (!p)
            return;
        p->vx = RtCos(a);
        p->vy = RtSin(a) / 2;
    }
}

void RtFx_Impact(u8 style, u8 pal, s32 x, s32 y)
{
    struct RtPart *p;
    switch (style)
    {
    case RTI_NONE:
        break;
    case RTI_HIT:
        p = RtFx_Spawn(FX_IMPACT32, pal, x, y, 10);
        RtFx_Burst(FX_SPARK8, FXP_WHITE, x, y, 4, 3, 14);
        break;
    case RTI_SLASH:
        RtFx_Spawn(FX_SLASH32, FXP_WHITE, x, y, 10);
        RtFx_Burst(FX_SPARK8, pal, x, y, 4, 3, 12);
        break;
    case RTI_BURN:
        RtFx_Spawn(FX_FLAME32, FXP_FIRE, x, y - TO_FX(4), 14);
        RtFx_Burst(FX_FLAME16, pal, x, y, 4, 2, 16);
        break;
    case RTI_SPLASH:
        RtFx_Burst(FX_DROP8, pal, x, y, 7, 3, 16);
        RtFx_Spawn(FX_BUBBLE16, pal, x, y, 12);
        break;
    case RTI_LEAF:
        RtFx_Burst(FX_LEAF16, pal, x, y, 5, 2, 18);
        break;
    case RTI_SHOCK:
        RtFx_Spawn(FX_IMPACT32, FXP_ELECTRIC, x, y, 8);
        RtFx_Burst(FX_BOLT16, pal, x, y, 4, 2, 12);
        break;
    case RTI_ROCK:
        RtFx_Burst(FX_ROCK16, FXP_ROCK, x, y, 4, 3, 16);
        RtFx_Burst(FX_SMOKE16, FXP_SMOKE, x, y, 3, 1, 18);
        break;
    case RTI_ICE:
        RtFx_Burst(FX_SHARD16, pal, x, y, 5, 3, 16);
        RtFx_Burst(FX_FLAKE8, FXP_ICE, x, y, 3, 2, 18);
        break;
    case RTI_PSY:
        p = RtFx_Spawn(FX_RING32, pal, x, y, 16);
        if (p) { p->flags |= PF_SCALE; p->scale = 8; p->dscale = 2; }
        RtFx_Burst(FX_SPARK8, pal, x, y, 4, 2, 14);
        break;
    case RTI_DARK:
        RtFx_Burst(FX_SMOKE16, FXP_SHADOW, x, y, 5, 2, 18);
        RtFx_Spawn(FX_IMPACT32, pal, x, y, 8);
        break;
    case RTI_POISON:
        RtFx_Burst(FX_ORB8, FXP_POISON, x, y, 6, 2, 18);
        RtFx_Burst(FX_SMOKE16, FXP_POISON, x, y, 2, 1, 18);
        break;
    case RTI_STAR:
        RtFx_Burst(FX_STAR16, pal, x, y, 4, 3, 14);
        RtFx_Spawn(FX_IMPACT32, FXP_GOLD, x, y, 8);
        break;
    case RTI_HEART:
        RtFx_Burst(FX_HEART16, FXP_HEART, x, y, 4, 2, 18);
        break;
    case RTI_WIND:
        RtFx_Burst(FX_FEATHER16, pal, x, y, 3, 2, 16);
        RtFx_Burst(FX_SMOKE16, FXP_WHITE, x, y, 3, 2, 14);
        break;
    }
}

void RtFx_Number(s32 x, s32 y, u16 value, u8 color)
{
    static const u8 sPals[] = {FXP_WHITE, FXP_GOLD, FXP_HEAL, FXP_BLOOD, FXP_WATER};
    u8 digits[5], n = 0, i;
    s32 w;
    if (color >= ARRAY_COUNT(sPals))
        color = 0;
    do
    {
        digits[n++] = value % 10;
        value /= 10;
    } while (value && n < 5);
    w = n * 6;
    for (i = 0; i < n; i++)
    {
        struct RtPart *p = RtFx_Spawn(FX_DIGITS, sPals[color], x - TO_FX(w / 2) + TO_FX((n - 1 - i) * 6), y, 40);
        if (!p)
            return;
        p->frameRate = 0;
        p->frame = digits[i];
        p->vy = -96;
        p->flags |= PF_FLICKER;
        p->prio = 0;
    }
}

void RtFx_Word(s32 x, s32 y, u8 word)
{
    struct RtPart *p = RtFx_Spawn(word, FXP_GOLD, x, y, 45);
    if (!p)
        return;
    if (word == FX_W_CRIT) p->pal = FXP_BLOOD;
    else if (word == FX_W_SUPER) p->pal = FXP_GOLD;
    else if (word == FX_W_WEAK || word == FX_W_RESIST) p->pal = FXP_WATER;
    else if (word == FX_W_IMMUNE || word == FX_W_MISS || word == FX_W_BLOCK) p->pal = FXP_WHITE;
    else if (word == FX_W_LV_UP) p->pal = FXP_HEAL;
    p->vy = -64;
    p->prio = 0;
}

void RtFx_StatArrows(struct RtActor *a, bool8 up)
{
    s32 i;
    for (i = 0; i < 3; i++)
    {
        struct RtPart *p = RtFx_Spawn(up ? FX_ARROWUP8 : FX_ARROWDN8, up ? FXP_FIRE : FXP_WATER,
                                      a->x + TO_FX((i - 1) * 9), a->y - TO_FX(up ? 4 : a->height + 4) , 30);
        if (!p)
            return;
        p->vy = up ? -160 : 160;
    }
}

void RtFx_Shake(u8 mag, u8 frames)
{
    if (gRt->shakeTimer < frames)
        gRt->shakeTimer = frames;
    if (gRt->shakeMag < mag || gRt->shakeTimer <= 1)
        gRt->shakeMag = mag;
}

void RtFx_Flash(u8 color, u8 frames)
{
    gRt->flashTimer = frames;
    gRt->flashColor = color;
}

void RtFx_UpdateAll(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_PART; i++)
    {
        struct RtPart *p = &gRt->parts[i];
        if (p->life == 0)
            continue;
        p->life--;
        p->x += p->vx;
        p->y += p->vy;
        if (p->vz || p->grav)
        {
            p->vz -= p->grav * 16;
            p->z += p->vz / 256;
            if (p->z < 0)
            {
                p->z = 0;
                p->vz = 0;
            }
        }
        if (p->flags & PF_SCALE)
        {
            s32 sc = p->scale + p->dscale;
            if (sc < 1) sc = 1;
            if (sc > 64) sc = 64;
            p->scale = sc;
        }
        if (p->flags & PF_ROTATE)
            p->angle += 8;
        // drag on bursts
        if (!(p->flags & PF_FIXED))
        {
            p->vx = p->vx * 15 / 16;
            p->vy = p->vy * 15 / 16;
        }
    }
    // screen flash
    if (gRt->flashTimer)
    {
        gRt->flashTimer--;
        SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG1 | BLDCNT_TGT1_BG2 | BLDCNT_TGT1_BG3 | BLDCNT_TGT1_OBJ | BLDCNT_TGT1_BD
                  | (gRt->flashColor ? BLDCNT_EFFECT_DARKEN : BLDCNT_EFFECT_LIGHTEN)
                  | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_BD);
        SetGpuReg(REG_OFFSET_BLDY, gRt->flashTimer > 8 ? 8 : gRt->flashTimer);
    }
    else
    {
        SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_BD);
        SetGpuReg(REG_OFFSET_BLDY, 0);
    }
    RtFx_WeatherUpdate();
}

static const u8 sShapeW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const u8 sShapeH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

// Draw one FX shape at a screen position (center). Returns FALSE if not drawn.
static bool8 DrawShape(u8 shape, u8 frame, u8 fxPal, s32 sx, s32 sy, u8 bucket, u8 hwPrio, u8 angle, u8 scaleQ4, bool8 blend, bool8 hflip, bool8 useAffine)
{
    const struct RtFxShape *s;
    u8 shp, size, w, h, palSlot;
    struct OamData *o;
    u16 *oam;
    u16 tile;
    if (shape == FX_NONE || shape >= FX_COUNT)
        return FALSE;
    s = &gRtFxShapes[shape];
    shp = s->shapeSize >> 2;
    size = s->shapeSize & 3;
    w = sShapeW[shp][size];
    h = sShapeH[shp][size];
    if (frame >= s->frames)
        frame = s->frames - 1;
    tile = RT_FX_TILE_BASE + s->tile + frame * s->tilesPerFrame;
    if (useAffine)
    {
        if (sx + w < 0 || sx - w > DISPLAY_WIDTH || sy + h < 0 || sy - h > DISPLAY_HEIGHT)
            return FALSE;
    }
    else
    {
        if (sx + w / 2 < 0 || sx - w / 2 > DISPLAY_WIDTH || sy + h / 2 < 0 || sy - h / 2 > DISPLAY_HEIGHT)
            return FALSE;
    }
    palSlot = RtFx_GetPalSlot(fxPal);
    o = RtOam_Next(bucket);
    if (!o)
        return FALSE;
    oam = (u16 *)o;
    if (useAffine)
    {
        u8 m = RtFx_AllocAffine(angle, scaleQ4, hflip);
        if (m == 0xFF)
        {
            useAffine = FALSE;
        }
        else
        {
            s32 x = sx - w, y = sy - h; // double size
            oam[0] = (y & 0xFF) | (3 << 8) | (blend ? (1 << 10) : 0) | (shp << 14);
            oam[1] = (x & 0x1FF) | (m << 9) | (size << 14);
            oam[2] = tile | (hwPrio << 10) | (palSlot << 12);
            return TRUE;
        }
    }
    {
        s32 x = sx - w / 2, y = sy - h / 2;
        oam[0] = (y & 0xFF) | (blend ? (1 << 10) : 0) | (shp << 14);
        oam[1] = (x & 0x1FF) | (size << 14) | (hflip ? (1 << 12) : 0);
        oam[2] = tile | (hwPrio << 10) | (palSlot << 12);
    }
    return TRUE;
}

void RtFx_DrawAll(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_PART; i++)
    {
        struct RtPart *p = &gRt->parts[i];
        s32 sx, sy;
        u8 frame;
        bool8 aff;
        if (p->life == 0)
            continue;
        // out of frame time: drop the remaining (cosmetic) particles this frame
        if ((i & 7) == 7 && RtFrameLinesUsed() > 185 && !(p->flags & PF_HUD))
            break;
        if ((p->flags & PF_FLICKER) && p->life < 10 && (p->life & 1))
            continue;
        if (p->flags & PF_FIXED)
        {
            sx = FROM_FX(p->x);
            sy = FROM_FX(p->y);
        }
        else
        {
            sx = FROM_FX(p->x) - gRt->camX;
            sy = FROM_FX(p->y) - gRt->camY - p->z;
        }
        if (p->frameRate)
            frame = ((p->maxLife - p->life) / p->frameRate) % gRtFxShapes[p->shape].frames;
        else
            frame = p->frame;
        aff = (p->flags & (PF_ROTATE | PF_SCALE)) != 0;
        DrawShape(p->shape, frame, p->pal, sx, sy, (p->flags & PF_GROUND) ? 3 : p->prio,
                  (p->flags & PF_HUD) ? 0 : 2, p->angle, p->scale, (p->flags & PF_BLEND) != 0, (p->flags & PF_HFLIP) != 0, aff);
    }
}

// ---------------------------------------------------------------------------
// Projectile rendering
// ---------------------------------------------------------------------------
static bool8 IsDirectional(u8 shape)
{
    switch (shape)
    {
    case FX_NEEDLE8:
    case FX_HORN16:
    case FX_DRILL16:
    case FX_CRESCENT32:
    case FX_FEATHER16:
    case FX_BEAM16:
    case FX_SLASH32:
    case FX_FANG32:
    case FX_FIST16:
    case FX_WAVE32:
    case FX_COLUMN16:
        return TRUE;
    }
    return FALSE;
}

static u8 AnimFrame(u8 shape, u16 age, u8 rate)
{
    u8 frames = gRtFxShapes[shape].frames;
    if (frames <= 1)
        return 0;
    return (age / rate) % frames;
}

static void DrawBeam(struct RtProj *p)
{
    struct RtActor *o = &gRt->actors[p->owner];
    s32 len = p->len, d;
    s32 ox = FROM_FX(p->x), oy = FROM_FX(p->y);
    u8 fr = AnimFrame(p->shape, p->age, 3);
    u8 step = p->shape == FX_BEAM16 || p->shape == FX_BUBBLE16 ? 12 : 16;
    bool8 rot = IsDirectional(p->shape);
    (void)o;
    for (d = 8; d <= len; d += step)
    {
        s32 x = ox + RtCos(p->angle) * d / 256 - gRt->camX;
        s32 y = oy + RtSin(p->angle) * d / 256 - gRt->camY - p->z;
        DrawShape(p->shape, (fr + d / step) % (gRtFxShapes[p->shape].frames), p->pal, x, y, 1, 2, p->angle, 16,
                  (p->flags & RTMF_BLEND) != 0, FALSE, rot);
    }
    // glowing head
    {
        s32 x = ox + RtCos(p->angle) * len / 256 - gRt->camX;
        s32 y = oy + RtSin(p->angle) * len / 256 - gRt->camY - p->z;
        DrawShape(FX_ORB16, p->age & 1, p->pal, x, y, 1, 2, 0, 16, FALSE, FALSE, FALSE);
        x = ox - gRt->camX;
        y = oy - gRt->camY - p->z;
        DrawShape(FX_GLOW16, p->age & 1, p->pal, x, y, 1, 2, 0, 16, TRUE, FALSE, FALSE);
    }
}

static void DrawWave(struct RtProj *p)
{
    s32 half = p->len, k;
    u8 perp = p->angle + 64;
    s32 cx = FROM_FX(p->x) - gRt->camX, cy = FROM_FX(p->y) - gRt->camY;
    u8 fr = AnimFrame(p->shape, p->age, 4);
    u8 w = gRtFxShapes[p->shape].w;
    s32 step = w > 16 ? 24 : 14;
    bool8 rot = IsDirectional(p->shape);
    u8 scale = 16;
    s32 n, i;
    // while the wave is still narrow its pieces are drawn smaller too
    if (p->maxRadius && half < p->maxRadius)
    {
        scale = 7 + 9 * half / p->maxRadius;
        step = step * scale / 16;
    }
    if (step < 6)
        step = 6;
    n = (half * 2) / step + 1;
    for (i = 0; i < n; i++)
    {
        k = n == 1 ? 0 : -half + (half * 2) * i / (n - 1);
        {
            s32 x = cx + RtCos(perp) * k / 256;
            s32 y = cy + RtSin(perp) * k / 256;
            DrawShape(p->shape, fr, p->pal, x, y, 1, 2, p->angle + 64, scale, (p->flags & RTMF_BLEND) != 0, FALSE, rot || scale != 16);
        }
    }
}

void RtFx_DrawProjectiles(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_PROJ; i++)
    {
        struct RtProj *p = &gRt->proj[i];
        s32 sx, sy;
        if (p->kind == PK_NONE)
            continue;
        sx = FROM_FX(p->x) - gRt->camX;
        sy = FROM_FX(p->y) - gRt->camY;
        switch (p->kind)
        {
        case PK_BULLET:
            if (p->delay)
                break;
            if (p->flags & RTMF_ARC)
            {
                // shadow on the ground
                DrawShape(FX_SHADOW16, 0, FXP_UI, sx, sy + 2, 3, 2, 0, 16, TRUE, FALSE, FALSE);
            }
            DrawShape(p->shape, AnimFrame(p->shape, p->age, 4), p->pal, sx, sy - p->z, 1, 2, p->angle,
                      16, (p->flags & RTMF_BLEND) != 0, FALSE, IsDirectional(p->shape));
            break;
        case PK_SAFARI:
            DrawShape(FX_SHADOW16, 0, FXP_UI, sx, sy + 2, 3, 2, 0, 16, TRUE, FALSE, FALSE);
            DrawShape(p->shape, AnimFrame(p->shape, p->age, 4), p->pal, sx, sy - p->z, 1, 2, 0, 16, FALSE, FALSE, FALSE);
            break;
        case PK_BALL:
            DrawShape(FX_SHADOW16, 0, FXP_UI, sx, sy + 2, 3, 2, 0, 16, TRUE, FALSE, FALSE);
            DrawShape(FX_BALL16, p->frame, FXP_UI, sx, sy - p->z, 1, 2, 0, 16, FALSE, FALSE, FALSE);
            break;
        case PK_MELEE:
        case PK_RUSHBOX:
            if (p->shape != FX_NONE && p->life)
            {
                u8 total = p->param2 ? p->param2 : 8;
                u8 frames = gRtFxShapes[p->shape].frames;
                u8 fr = frames > 1 ? (p->age * frames / total) : 0;
                bool8 rot = IsDirectional(p->shape);
                DrawShape(p->shape, fr, p->pal, sx, sy - p->z, 1, 2, p->angle, 16 + (p->age < 3 ? -4 : 0), FALSE,
                          FALSE, rot);
            }
            break;
        case PK_BEAM:
            if (!p->delay)
                DrawBeam(p);
            break;
        case PK_WAVE:
            if (!p->delay)
                DrawWave(p);
            break;
        case PK_AREA:
            if (!p->delay)
            {
                u8 sc = p->radius * 16 / 14;
                if (sc < 4) sc = 4;
                if (sc > 40) sc = 40;
                DrawShape(FX_RING32, 0, p->pal, sx, sy, 3, 2, 0, sc, TRUE, FALSE, TRUE);
            }
            break;
        case PK_STRIKE:
        case PK_FUTURE:
            if (p->delay)
            {
                // warning marker on the ground, blinking faster near impact
                if (p->delay > 20 || (p->age & 2))
                    DrawShape(FX_TARGET32, (p->age >> 3) & 1, p->kind == PK_FUTURE ? FXP_PSYCHIC : FXP_BLOOD, sx, sy, 3, 2, 0, 16, TRUE, FALSE, FALSE);
                if (p->delay <= 14 && p->shape != FX_BOLTV && p->shape != FX_COLUMN16)
                {
                    // falling object
                    s32 h = p->delay * 8;
                    DrawShape(p->shape, AnimFrame(p->shape, p->age, 3), p->pal, sx, sy - h - 6, 1, 2, 0, 16, FALSE, FALSE, FALSE);
                }
            }
            break;
        case PK_ZONE:
            {
                s32 k, n = p->maxRadius > 18 ? 5 : 3;
                bool8 still = (p->shape == FX_SPIKES16);
                for (k = 0; k < n; k++)
                {
                    u8 a = k * 256 / n + (still ? 0 : p->age * 3);
                    s32 r = still ? p->maxRadius * 2 / 3 : (p->maxRadius * ((k * 37 + p->age) % 16)) / 16;
                    s32 x = sx + RtCos(a) * r / 256;
                    s32 y = sy + RtSin(a) * r / 512;
                    DrawShape(p->shape, AnimFrame(p->shape, p->age + k * 3, 4), p->pal, x, y - (still ? 0 : 4), still ? 3 : 1, 2,
                              0, 16, (p->flags & RTMF_BLEND) != 0 || p->shape == FX_SMOKE16, FALSE, FALSE);
                }
                if (still)
                    DrawShape(FX_SPIKES16, 0, p->pal, sx, sy, 3, 2, 0, 16, FALSE, FALSE, FALSE);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Weather
// ---------------------------------------------------------------------------
void RtFx_WeatherUpdate(void)
{
    struct RtPart *p;
    if (gRt->weather == WEATHER_NONE_RT)
        return;
    switch (gRt->weather)
    {
    case WEATHER_RAIN_RT:
        if ((gRt->frame & 1) == 0)
        {
            p = RtFx_Spawn(FX_RAIN8, FXP_RAIN, TO_FX(Random() % 260), TO_FX(Random() % 40), 30);
            if (p)
            {
                p->flags |= PF_FIXED;
                p->vx = -2 * 256;
                p->vy = 6 * 256;
                p->prio = 1;
            }
        }
        break;
    case WEATHER_SAND_RT:
        if ((gRt->frame & 1) == 0)
        {
            p = RtFx_Spawn(FX_GRAIN8, FXP_SAND, TO_FX(0), TO_FX(Random() % 150), 60);
            if (p)
            {
                p->flags |= PF_FIXED;
                p->vx = 5 * 256;
                p->vy = 64;
                p->prio = 1;
            }
        }
        break;
    case WEATHER_HAIL_RT:
        if ((gRt->frame & 3) == 0)
        {
            p = RtFx_Spawn(FX_HAIL8, FXP_ICE, TO_FX(Random() % 260), TO_FX(Random() % 30), 40);
            if (p)
            {
                p->flags |= PF_FIXED;
                p->vx = -256;
                p->vy = 4 * 256;
                p->prio = 1;
            }
        }
        break;
    case WEATHER_SUN_RT:
        if ((gRt->frame & 15) == 0)
        {
            p = RtFx_Spawn(FX_SPARK8, FXP_GOLD, TO_FX(Random() % 240), TO_FX(16 + Random() % 128), 24);
            if (p)
                p->flags |= PF_FIXED;
        }
        break;
    }
}
