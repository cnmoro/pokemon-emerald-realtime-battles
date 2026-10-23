// Real-time battle visual effects: OAM management, particles, palettes, projectile rendering, weather.
#include "global.h"
#include "rt_internal.h"
#include "rt_vfx.h"
#include "decompress.h"
#include "gpu_regs.h"
#include "main.h"
#include "palette.h"
#include "random.h"
#include "sound.h"
#include "trig.h"
#include "constants/moves.h"
#include "constants/songs.h"

#include "data/rt/fx_data.h"

void RtProf(u8 n);

// ---------------------------------------------------------------------------
// OAM buckets
// ---------------------------------------------------------------------------
#define BUCKETS 6
#define BUCKET_MAX 96
// commit order: HUD, key effects (beam cores, move bodies), effects in front, actors/props, effects behind
// (streams, trails), ground marks
static const u8 sBucketOrder[BUCKETS] = {0, 5, 1, 2, 4, 3};
#define FRONT_MAX 36
// the GBA renders sprites in OAM order and silently stops a scanline once its sprite cycles run out, cutting
// whatever comes later in half; cosmetic sprites that would overflow a band of lines are left out instead
#define LINE_BUDGET 1150
#define BAND_LINES 8
#define BANDS (160 / BAND_LINES)
static const u8 sObjW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const u8 sObjH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

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

// returns FALSE (and charges nothing) when the sprite would push a band of scanlines over budget
static bool8 ChargeLines(u16 *band, const struct OamEntry *e, bool8 force)
{
    u8 mode = (e->a0 >> 8) & 3, shp = e->a0 >> 14, size = e->a1 >> 14;
    s32 w, h, y0, y1, cost, b, b0, b1;
    if (mode == 2 || shp > 2)
        return TRUE;
    w = sObjW[shp][size];
    h = sObjH[shp][size];
    if (mode == 3)
    {
        w *= 2;
        h *= 2;
    }
    cost = (mode & 1) ? 10 + 2 * w : w;
    y0 = e->a0 & 0xFF;
    if (y0 >= 160 && y0 + h > 256)
        y0 -= 256;
    y1 = y0 + h;
    if (y0 < 0) y0 = 0;
    if (y1 > 160) y1 = 160;
    if (y1 <= y0)
        return TRUE;
    b0 = y0 / BAND_LINES;
    b1 = (y1 - 1) / BAND_LINES;
    if (!force)
        for (b = b0; b <= b1; b++)
            if (band[b] + cost > LINE_BUDGET)
                return FALSE;
    for (b = b0; b <= b1; b++)
        band[b] += cost;
    return TRUE;
}

void RtOam_Commit(void)
{
    s32 b, i, n = 0;
    s32 room, k;
    u16 band[BANDS];
    bool8 budget;
    FlushScratch();
    n = 0;
    for (k = 0; k < BUCKETS; k++)
        n += sBucketCount[k];
    // few sprites: no line can overflow, skip the bookkeeping
    budget = n > 40;
    n = 0;
    for (i = 0; i < BANDS; i++)
        band[i] = 0;
    // reserve the cycles of the HUD and the actors first: effects may only use what is left
    if (budget)
    {
        for (i = 0; i < sBucketCount[0]; i++)
            ChargeLines(band, &sBucket[0][i], TRUE);
        for (i = 0; i < sBucketCount[2]; i++)
            ChargeLines(band, &sBucket[2][i], TRUE);
    }
    // the GBA draws sprites in OAM order and drops whatever goes past its per-scanline budget, so the HUD and
    // the actors come before the bulk of the effects; front effects (impacts, numbers) are capped
    for (k = 0; k < BUCKETS; k++)
    {
        s32 lim;
        b = sBucketOrder[k];
        lim = sBucketCount[b];
        if (b == 1 && lim > FRONT_MAX)
            lim = FRONT_MAX;
        if (b == 4)
        {
            room = 128 - n - sBucketCount[3];
            if (lim > room)
                lim = room < 0 ? 0 : room;
        }
        for (i = 0; i < lim && n < 128; i++)
        {
            u16 *o = (u16 *)&gMain.oamBuffer[n];
            // the HUD and the actors always get their cycles
            if (budget && b != 0 && b != 2 && !ChargeLines(band, &sBucket[b][i], FALSE))
                continue;
            o[0] = sBucket[b][i].a0;
            o[1] = sBucket[b][i].a1;
            o[2] = sBucket[b][i].a2;
            n++;
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

static EWRAM_DATA u16 sAffKey[32] = {0};
static EWRAM_DATA u16 sAffFrame[32] = {0}; // frame stamp: a slot is live only if stamped this frame

u8 RtFx_AllocAffine(u8 angle, u8 scaleQ4, bool8 hflip)
{
    u8 idx, k;
    s32 c, s, sc;
    u16 key = angle | ((scaleQ4 & 0x7F) << 8) | (hflip ? 0x8000 : 0);
    u16 fr = (u16)gRt->frame;
    // sprites sharing rotation and scale share a matrix: open-addressed lookup (a few probes) instead of a
    // linear scan, the cost of which added up with dozens of rotating particles
    u8 h = (angle * 7 + scaleQ4 * 13 + (hflip ? 5 : 0)) & 31;
    for (k = 0; k < 6; k++)
    {
        idx = (h + k) & 31;
        if (sAffFrame[idx] != fr || gRt->affineUsed == 0)
            break; // free slot
        if (sAffKey[idx] == key)
            return idx;
    }
    if (k == 6 || gRt->affineUsed >= 32)
    {
        // crowded: reuse a live matrix with the same scale and a close angle
        u8 best = 0xFF, bd = 255;
        for (idx = 0; idx < 32; idx++)
        {
            u8 d;
            if (sAffFrame[idx] != fr || (sAffKey[idx] & 0xFF00) != (key & 0xFF00))
                continue;
            d = (u8)((sAffKey[idx] & 0xFF) - angle);
            if (d > 128) d = 256 - d;
            if (d < bd)
            {
                bd = d;
                best = idx;
            }
        }
        return bd <= 12 ? best : 0xFF;
    }
    if (gRt->affineUsed == 0)
    {
        // first matrix of the frame: forget last frame's slots
        u8 j;
        for (j = 0; j < 32; j++)
            sAffFrame[j] = fr - 1;
        idx = h;
    }
    gRt->affineUsed++;

    sAffKey[idx] = key;
    sAffFrame[idx] = fr;
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
    RtFx_DynReset();
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
    {
        struct RtPartX *x = &gRt->partsX[p - gRt->parts];
        memset(x, 0, sizeof(*x));
        x->pal2 = x->pal3 = 0xFF;
    }
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

struct RtPartX *RtFx_X(struct RtPart *p)
{
    return &gRt->partsX[p - gRt->parts];
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
    if (style != RTI_NONE)
    {
        // shockwave ring
        p = RtFx_Spawn(FX_RING32, pal, x, y, 10);
        if (p)
        {
            p->flags |= PF_SCALE;
            p->scale = 6;
            p->dscale = 3;
            p->prio = 1;
        }
    }
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

// ---------------------------------------------------------------------------
// Per-scanline brightness (HBlank DMA into BLDY): soft dimming around the playable field,
// whole-screen flashes and dramatic darkening of the arena. Windows are not used, so
// semi-transparent sprites (glows, shadows) are alpha blended everywhere.
// ---------------------------------------------------------------------------
#define sLineBldy (gRt->lineBldy) // heap (gRt): EWRAM is full
static EWRAM_DATA u8 sLineFront = 0;
static EWRAM_DATA s16 sLineHofs[3] = {0};

#define EDGE_DIM   8    // BLDY of the scenery outside the field (matches the EVB of the dark edge sprites)
#define EDGE_GRAD  8   // px of soft transition (half inside the field)

void RtFx_LineVBlank(void)
{
    u16 *b = sLineBldy[sLineFront];
    REG_DMA0CNT_H = 0;
    REG_BLDY = b[0];
    REG_DMA0SAD = (u32)&b[1];
    REG_DMA0DAD = (u32)&REG_BLDY;
    REG_DMA0CNT = 1 | ((u32)(DMA_DEST_RELOAD | DMA_SRC_INC | DMA_REPEAT | DMA_16BIT | DMA_START_HBLANK | DMA_ENABLE) << 16);
}

static void WaveTimerStop(void);
void RtFx_LineStop(void)
{
    WaveTimerStop();
    REG_DMA0CNT_H = 0;
    REG_BLDY = 0;
    SetHBlankCallback(NULL);
    DisableInterrupts(INTR_FLAG_HBLANK);
}

// heat-haze / psychic distortion: the arena is shifted sideways in bands of 4 scanlines. A per-line HBlank
// interrupt (228 a frame) ate up to half of the CPU on slower builds; timer 2 fires every 4 lines instead
// (4 * 1232 cycles: 57 periods make exactly one frame, so it stays in step with the screen)
#define WAVE_TIMER_PERIOD (4 * 1232)
extern IntrFunc gIntrTable[];
static void TimerCB_Wave(void)
{
    u16 v = REG_VCOUNT + 2;
    s16 o = 0;
    if (v >= HUD_TOP && v < DISPLAY_HEIGHT - HUD_BOTTOM)
        o = (gSineTable[(u8)(v * 6 + gRt->frame * 5)] * gRt->waveAmp) >> 8;
    REG_BG1HOFS = sLineHofs[0] + o;
    REG_BG2HOFS = sLineHofs[0] + o;
    REG_BG3HOFS = sLineHofs[0] + o;
}

static void WaveTimerStart(void)
{
    if (REG_TM2CNT_H & TIMER_ENABLE)
        return;
    gIntrTable[7] = TimerCB_Wave; // INTR_FLAG_TIMER2
    REG_TM2CNT_L = (u16)(0x10000 - WAVE_TIMER_PERIOD);
    REG_TM2CNT_H = TIMER_ENABLE | TIMER_INTR_ENABLE | TIMER_1CLK;
    EnableInterrupts(INTR_FLAG_TIMER2);
}

static void WaveTimerStop(void)
{
    if (!(REG_TM2CNT_H & TIMER_ENABLE))
        return;
    REG_TM2CNT_H = 0;
    DisableInterrupts(INTR_FLAG_TIMER2);
    REG_BG1HOFS = sLineHofs[0];
    REG_BG2HOFS = sLineHofs[0];
    REG_BG3HOFS = sLineHofs[0];
}

void RtFx_Dim(u8 level, u8 hold)
{
    if (level > 10)
        level = 10;
    if (level >= gRt->dimLevel)
    {
        gRt->dimLevel = level;
        if (hold > gRt->dimHold)
            gRt->dimHold = hold;
    }
}

void RtFx_Wave(u8 amp, u8 frames)
{
    if (amp > gRt->waveAmp)
        gRt->waveAmp = amp;
    if (frames > gRt->waveTimer)
        gRt->waveTimer = frames;
}

static void BuildLineTable(void)
{
    u8 back = sLineFront ^ 1;
    u16 *b = sLineBldy[back];
    s32 y;
    if (gRt->flashTimer)
    {
        u16 v = gRt->flashTimer > 8 ? 8 : gRt->flashTimer;
        SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG1 | BLDCNT_TGT1_BG2 | BLDCNT_TGT1_BG3 | BLDCNT_TGT1_OBJ | BLDCNT_TGT1_BD
                  | (gRt->flashColor ? BLDCNT_EFFECT_DARKEN : BLDCNT_EFFECT_LIGHTEN)
                  | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_BD);
        for (y = 0; y < DISPLAY_HEIGHT + 2; y++)
            b[y] = v;
    }
    else
    {
        // edge dimming: EDGE_DIM above and below the field with an EDGE_GRAD ramp a few px inside it, the
        // field rows at the dim level. Filled in runs (constant stretches) instead of per-line math.
        s32 top = gRt->playY0 - gRt->camY, bot = gRt->playY1 - gRt->camY;
        u16 dim = gRt->dimLevel > 8 ? 8 : gRt->dimLevel;
        u16 hi = dim > EDGE_DIM ? dim : EDGE_DIM;
        s32 n = DISPLAY_HEIGHT + 2;
        s32 r0 = top - EDGE_GRAD / 2, r1 = top + EDGE_GRAD / 2;       // top ramp [r0, r1)
        s32 q0 = bot - 1 - EDGE_GRAD / 2, q1 = bot - 1 + EDGE_GRAD / 2; // bottom ramp
        SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG1 | BLDCNT_TGT1_BG2 | BLDCNT_TGT1_BG3 | BLDCNT_TGT1_BD | BLDCNT_EFFECT_DARKEN
                  | BLDCNT_TGT2_BG1 | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_BG3 | BLDCNT_TGT2_BD);
        // constant runs
        {
            s32 a0 = r0 < 0 ? 0 : (r0 > n ? n : r0);
            s32 a1 = r1 < 0 ? 0 : (r1 > n ? n : r1);
            s32 c0 = q0 < 0 ? 0 : (q0 > n ? n : q0);
            s32 c1 = q1 < 0 ? 0 : (q1 > n ? n : q1);
            if (a0 > 0)
                CpuFill16(hi, b, a0 * 2);
            if (c0 > a1)
                CpuFill16(dim, b + a1, (c0 - a1) * 2);
            if (n > c1)
                CpuFill16(hi, b + c1, (n - c1) * 2);
            // ramps
            for (y = a0; y < n; y++)
            {
                s32 d;
                u16 v;
                if (y == a1 && y < c0)
                    y = c0;
                if (y >= c1)
                    break;
                d = top - y;
                if (y - bot + 1 > d)
                    d = y - bot + 1;
                d += EDGE_GRAD / 2;
                v = d <= 0 ? 0 : (d >= EDGE_GRAD ? EDGE_DIM : (d * EDGE_DIM + EDGE_GRAD / 2) / EDGE_GRAD);
                if (v < dim)
                    v = dim;
                b[y] = v;
            }
        }
    }
    sLineFront = back;
}

void RtFx_UpdateAll(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_PART; i++)
    {
        struct RtPart *p = &gRt->parts[i];
        struct RtPartX *x;
        if (p->life == 0)
            continue;
        x = &gRt->partsX[i];
        p->life--;
        if (x->pal2 != 0xFF && p->life <= x->at2)
        {
            p->pal = x->pal2;
            x->pal2 = 0xFF;
        }
        if (x->pal3 != 0xFF && p->life <= x->at3)
        {
            p->pal = x->pal3;
            x->pal3 = 0xFF;
        }
        if (x->flags & (PX_ORBIT | PX_FOLLOW | PX_HOMEIN))
        {
            struct RtActor *o = &gRt->actors[x->owner < RT_MAX_ACTORS ? x->owner : 0];
            if (x->flags & PX_ORBIT)
            {
                // ay = angular speed, ax = radial speed (px per 4 frames)
                s32 r = x->orbit;
                x->orbitAng += x->ay;
                if (x->ax && (gRt->frame & 3) == 0)
                {
                    r += x->ax;
                    x->orbit = r < 0 ? 0 : (r > 250 ? 250 : r);
                }
                p->x = o->x + RtCos(x->orbitAng) * x->orbit;
                p->y = o->y - TO_FX(o->height / 2) + RtSin(x->orbitAng) * x->orbit / 2;
                goto anim;
            }
            if (x->flags & PX_FOLLOW)
            {
                // glued to the owner: orbitAng = x offset (s8 px), orbit = height above the feet (px)
                p->x = o->x + TO_FX((s8)x->orbitAng);
                p->y = o->y - TO_FX(x->orbit);
                goto anim;
            }
            if (x->flags & PX_HOMEIN)
            {
                s32 tx = o->x, ty = o->y - TO_FX(o->height / 2);
                p->vx += (tx - p->x) / 24 / 16;
                p->vy += (ty - p->y) / 24 / 16;
                if (RtDist(FROM_FX(tx - p->x), FROM_FX(ty - p->y)) < 6)
                    p->life = p->life > 2 ? 2 : p->life;
            }
        }
        p->vx += x->ax * 4;
        p->vy += x->ay * 4;
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
        // drag on bursts
        if (!(p->flags & PF_FIXED))
        {
            u8 dr = x->drag ? x->drag : 15;
            if (dr < 16)
            {
                p->vx = p->vx * dr / 16;
                p->vy = p->vy * dr / 16;
            }
        }
anim:
        if (p->flags & PF_SCALE)
        {
            s32 sc = p->scale + p->dscale;
            if (sc < 1) sc = 1;
            if (sc > 64) sc = 64;
            p->scale = sc;
        }
        if (x->spin)
            p->angle += x->spin;
        else if (p->flags & PF_ROTATE)
            p->angle += 8;
    }
    if (gRt->flashTimer)
        gRt->flashTimer--;
    if (gRt->dimHold)
        gRt->dimHold--;
    else if (gRt->dimLevel && (gRt->frame & 1))
        gRt->dimLevel--;
    // distortion wave
    if (gRt->waveTimer)
    {
        gRt->waveTimer--;
        if (gRt->waveTimer < 16 && gRt->waveAmp > 0 && (gRt->frame & 3) == 0)
            gRt->waveAmp--;
        sLineHofs[0] = gRt->camX;
        if (gRt->waveAmp)
            WaveTimerStart();
    }
    if (!gRt->waveTimer || !gRt->waveAmp)
    {
        gRt->waveAmp = 0;
        WaveTimerStop();
        SetHBlankCallback(NULL);
        DisableInterrupts(INTR_FLAG_HBLANK);
    }
    SetGpuReg(REG_OFFSET_BLDALPHA, BLDALPHA_BLEND(9, 16)); // additive light: glows only brighten
    RtProf(11);
    BuildLineTable();
    RtProf(12);
    RtFx_WeatherUpdate();
}

// ---------------------------------------------------------------------------
// Streamed FX shapes: a VRAM cache after the static atlas, LRU-evicted
// ---------------------------------------------------------------------------
extern const u32 gRtFxDynGfx[];
#define DYN_BASE  (RT_FX_TILE_BASE + RT_FX_ATLAS_TILES)
#define DYN_END   1024
static EWRAM_DATA u16 sDynStart[FX_COUNT] = {0};   // 0 = not resident, else VRAM tile + 1
static EWRAM_DATA u16 sDynUse[FX_COUNT] = {0};     // frame of last use
static EWRAM_DATA u8 sDynList[FX_COUNT] = {0};     // resident shapes
static EWRAM_DATA u8 sDynCount = 0;

void RtFx_DynReset(void)
{
    s32 i;
    for (i = 0; i < FX_COUNT; i++)
        sDynStart[i] = 0;
    sDynCount = 0;
}

static u16 DynFindGap(u16 len)
{
    // resident intervals sorted by start (insertion sort, the list is short)
    u16 st[FX_COUNT], en[FX_COUNT];
    s32 i, j, n = 0;
    u16 pos = DYN_BASE;
    for (i = 0; i < sDynCount; i++)
    {
        u8 sh = sDynList[i];
        u16 a = sDynStart[sh] - 1;
        u16 b = a + gRtFxShapes[sh].frames * gRtFxShapes[sh].tilesPerFrame;
        for (j = n; j > 0 && st[j - 1] > a; j--)
        {
            st[j] = st[j - 1];
            en[j] = en[j - 1];
        }
        st[j] = a;
        en[j] = b;
        n++;
    }
    for (i = 0; i < n; i++)
    {
        if (st[i] >= pos + len)
            return pos;
        if (en[i] > pos)
            pos = en[i];
    }
    if (pos + len <= DYN_END)
        return pos;
    return 0;
}

static u16 DynTile(u8 shape)
{
    const struct RtFxShape *s = &gRtFxShapes[shape];
    u16 len, at;
    if (sDynStart[shape])
    {
        sDynUse[shape] = gRt->frame;
        return sDynStart[shape] - 1;
    }
    len = s->frames * s->tilesPerFrame;
    for (;;)
    {
        s32 i, victim = -1;
        u16 oldest = 0;
        at = DynFindGap(len);
        if (at)
            break;
        // evict the least recently used shape not drawn this frame or the previous one
        for (i = 0; i < sDynCount; i++)
        {
            u8 sh = sDynList[i];
            u16 age = (u16)gRt->frame - sDynUse[sh];
            if (age >= 2 && age >= oldest)
            {
                oldest = age;
                victim = i;
            }
        }
        if (victim < 0)
            return 0xFFFF;
        sDynStart[sDynList[victim]] = 0;
        sDynList[victim] = sDynList[--sDynCount];
    }
    CpuFastCopy(gRtFxDynGfx + (s->tile & 0x7FFF) * 8, (void *)(OBJ_VRAM0 + at * TILE_SIZE_4BPP), len * TILE_SIZE_4BPP);
    sDynStart[shape] = at + 1;
    sDynUse[shape] = gRt->frame;
    sDynList[sDynCount++] = shape;
    return at;
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
    if (s->tile & 0x8000)
    {
        tile = DynTile(shape);
        if (tile == 0xFFFF)
            return FALSE;
        tile += frame * s->tilesPerFrame;
    }
    else
    {
        tile = RT_FX_TILE_BASE + s->tile + frame * s->tilesPerFrame;
    }
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
    // single-frame shapes animated by mirroring
    if (shape == FX_TORNADO32 || shape == FX_AURA32)
        hflip ^= (gRt->frame >> 2) & 1;
    palSlot = RtFx_GetPalSlot(fxPal);
    o = RtOam_Next(bucket);
    if (!o)
        return FALSE;
    oam = (u16 *)o;
    if (useAffine)
    {
        u8 m;
        if (scaleQ4 > 32)
            scaleQ4 = 32; // the double-size area holds at most 2x
        m = RtFx_AllocAffine(angle, scaleQ4, hflip);
        if (m == 0xFF)
        {
            useAffine = FALSE;
        }
        else if (angle == 0 && scaleQ4 <= 16)
        {
            s32 x = sx - w / 2, y = sy - h / 2; // normal size: half the scanline cost
            oam[0] = (y & 0xFF) | (1 << 8) | (blend ? (1 << 10) : 0) | (shp << 14);
            oam[1] = (x & 0x1FF) | (m << 9) | (size << 14);
            oam[2] = tile | (hwPrio << 10) | (palSlot << 12);
            return TRUE;
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

bool8 RtFx_Draw(u8 shape, u8 frame, u8 pal, s32 sx, s32 sy, u8 bucket, u8 angle, u8 scale, u8 flags)
{
    return DrawShape(shape, frame, pal, sx, sy, bucket, 2, angle, scale ? scale : 16, (flags & RFD_BLEND) != 0,
                     (flags & RFD_HFLIP) != 0, (flags & RFD_AFFINE) != 0 || (scale && scale != 16));
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
        if ((i & 3) == 3 && RtFrameLinesUsed() > 172 && !(p->flags & PF_HUD))
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
            s32 px = p->x, py = p->y;
            // smooth motion below 100% BATTLE SPEED: drawn part of the way back along the last step
            if (gRt->lerpT < 100)
            {
                struct RtPartX *xx = &gRt->partsX[i];
                if (xx->flags & (PX_ORBIT | PX_FOLLOW))
                {
                    s32 dx, dy;
                    RtLerp_ActorDelta(xx->owner, &dx, &dy);
                    px += dx;
                    py += dy;
                }
                else
                {
                    s32 back = 256 - gRt->lerpQ8;
                    px -= (p->vx * back) >> 8;
                    py -= (p->vy * back) >> 8;
                }
            }
            sx = FROM_FX(px) - gRt->camX;
            sy = FROM_FX(py) - gRt->camY - p->z;
        }
        {
            struct RtPartX *x = &gRt->partsX[i];
            if (p->frameRate)
            {
                u16 f = (p->maxLife - p->life) / p->frameRate;
                u8 nf = gRtFxShapes[p->shape].frames;
                frame = (x->flags & PX_HOLD) ? (f >= nf ? nf - 1 : f) : f % nf;
            }
            else
            {
                frame = p->frame;
            }
            bool8 blend = (p->flags & PF_BLEND) != 0;
            u8 ang = p->angle;
            u8 w = gRtFxShapes[p->shape].w;
            aff = (p->flags & (PF_ROTATE | PF_SCALE)) != 0 || x->spin;
            if (x->flags & PX_STRETCH)
            {
                // the direction of travel changes slowly: refresh it every 4th frame
                if (((gRt->frame + i) & 3) == 0 || p->angle == 0)
                    p->angle = RtAtan2(p->vx, p->vy);
                ang = p->angle;
                aff = TRUE;
            }
            // rotation of tiny sprites is invisible but costs a rotated double-size sprite: drawn flat
            if (aff && w <= 8 && !(p->flags & PF_SCALE) && !(x->flags & PX_STRETCH))
                aff = FALSE;
            // fewer distinct rotations share affine matrices
            if (aff && w <= 16)
                ang &= 0xF8;
            if ((x->flags & PX_FADE) && p->life * 3 < p->maxLife)
                blend = TRUE;
            DrawShape(p->shape, frame, p->pal, sx, sy, (p->flags & PF_GROUND) ? 3 : p->prio,
                      (p->flags & PF_HUD) ? 0 : 2, ang, p->scale, blend, (p->flags & PF_HFLIP) != 0, aff);
        }
    }
}

// dark, dithered, semi-transparent columns over the scenery left/right of the field (the rows above and
// below are dimmed per scanline)
void RtFx_DrawEdges(void)
{
    // during a screen flash or a dramatic dim the whole screen is affected alike: the dithered side columns
    // would otherwise stand out as un-dimmed (or un-flashed) panels
    if (gRt->flashTimer || gRt->dimLevel >= 2 || !gRt->sideDim)
        return;
    {
    const struct RtFxShape *s = &gRtFxShapes[FX_EDGEDIM];
    s32 l = gRt->playX0 - gRt->camX, r = gRt->playX1 - gRt->camX, y;
    u16 tile = RT_FX_TILE_BASE + s->tile;
    for (y = HUD_TOP - 8; y < DISPLAY_HEIGHT - HUD_BOTTOM; y += 64)
    {
        u16 *oam;
        if (l > -4)
        {
            // left of the field: ramp faces right (flipped)
            oam = (u16 *)RtOam_Next(3);
            if (oam)
            {
                s32 x = l - 28; // the ramp starts 4 px inside the field
                oam[0] = (y & 0xFF) | (2 << 14);
                oam[1] = (x & 0x1FF) | (1 << 12) | (3 << 14);
                oam[2] = tile | (1 << 10) | (4 << 12);
            }
        }
        if (r < DISPLAY_WIDTH + 4)
        {
            oam = (u16 *)RtOam_Next(3);
            if (oam)
            {
                oam[0] = (y & 0xFF) | (2 << 14);
                oam[1] = ((r - 4) & 0x1FF) | (3 << 14);
                oam[2] = tile | (1 << 10) | (4 << 12);
            }
        }
    }
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
        DrawShape(p->shape, (fr + d / step) % (gRtFxShapes[p->shape].frames), p->pal, x, y, 4, 2, p->angle, 16,
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
            DrawShape(p->shape, fr, p->pal, x, y, 4, 2, p->angle + 64, scale, (p->flags & RTMF_BLEND) != 0, FALSE, rot || scale != 16);
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
        if (RtVfx_DrawProj(p, sx, sy))
            continue;
        if ((p->kind == PK_STRIKE || p->kind == PK_FUTURE) && p->delay && RtVfx_StrikeWarn(p))
        {
            if (p->delay > 20 || (p->age & 2))
                DrawShape(FX_TARGET32, (p->age >> 3) & 1, p->kind == PK_FUTURE ? FXP_PSYCHIC : FXP_BLOOD, sx, sy, 3, 2, 0, 16, TRUE, FALSE, FALSE);
            continue;
        }
        switch (p->kind)
        {
        case PK_BULLET:
            if (p->delay)
                break;
            if (p->flags & RTMF_ARC)
            {
                // shadow on the ground
                DrawShape(FX_SHADOW16, 0, FXP_UI, sx, sy + 2, 3, 2, 0, 16, FALSE, FALSE, FALSE);
            }
            {
                // small projectiles are drawn a bit bigger; stream particles grow as they travel
                u8 sc = 16;
                bool8 aff = IsDirectional(p->shape);
                if (p->param & 8)
                {
                    sc = 9 + (p->age * 12) / (p->len ? p->len : 1);
                    if (sc > 22) sc = 22;
                    aff = TRUE;
                }
                else if (gRtFxShapes[p->shape].w <= 8)
                {
                    sc = 24;
                    aff = TRUE;
                }
                DrawShape(p->shape, (p->param & 16) ? 0 : AnimFrame(p->shape, p->age, 4), p->pal, sx, sy - p->z, 4, 2,
                          IsDirectional(p->shape) ? p->angle : 0, sc, (p->flags & RTMF_BLEND) != 0, FALSE, aff);
            }
            break;
        case PK_SAFARI:
            DrawShape(FX_SHADOW16, 0, FXP_UI, sx, sy + 2, 3, 2, 0, 16, FALSE, FALSE, FALSE);
            DrawShape(p->shape, AnimFrame(p->shape, p->age, 4), p->pal, sx, sy - p->z, 4, 2, 0, 16, FALSE, FALSE, FALSE);
            break;
        case PK_BALL:
            DrawShape(FX_SHADOW16, 0, FXP_UI, sx, sy + 2, 3, 2, 0, 16, FALSE, FALSE, FALSE);
            DrawShape(FX_BALL16, p->frame, FXP_UI, sx, sy - p->z, 4, 2, 0, 16, FALSE, FALSE, FALSE);
            break;
        case PK_MELEE:
        case PK_RUSHBOX:
            if (p->shape != FX_NONE && p->life)
            {
                u8 total = p->param2 ? p->param2 : 8;
                u8 frames = gRtFxShapes[p->shape].frames;
                u8 fr = frames > 1 ? (p->age * frames / total) : 0;
                bool8 rot = IsDirectional(p->shape);
                DrawShape(p->shape, fr, p->pal, sx, sy - p->z, 4, 2, p->angle, 16 + (p->age < 3 ? -4 : 0), FALSE,
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
                bool8 still = (p->shape == FX_SPIKES16);
                s32 k, n = still ? 3 : (p->maxRadius > 18 ? 5 : 3);
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
