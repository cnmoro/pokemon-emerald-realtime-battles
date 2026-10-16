// Real-time battle visual effects, move by move (see rt_vfx.h). Everything here is cosmetic.
#include "global.h"
#include "rt_internal.h"
#include "rt_vfx.h"
#include "battle.h"
#include "random.h"
#include "sound.h"
#include "constants/battle_move_effects.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/songs.h"

#include "data/rt/vfx_defs.h"

#define SX(fx) (FROM_FX(fx) - gRt->camX)
#define SY(fx) (FROM_FX(fx) - gRt->camY)

static const struct RtVfxDef *V(u16 move)
{
    return &gRtVfxDefs[move < MOVES_COUNT ? move : 0];
}

static u8 Pal1(u16 move)
{
    return V(move)->pal;
}

static u8 Pal2(u16 move)
{
    return V(move)->pal2;
}

// ---------------------------------------------------------------------------
// particle helpers
// ---------------------------------------------------------------------------
static EWRAM_DATA u8 sPrio = 0; // particle draw bucket: 1 in front of the actors, 4 behind (trails, streams)

static struct RtPart *Sp(u8 shape, u8 pal, s32 x, s32 y, u8 life)
{
    struct RtPart *p = RtFx_Spawn(shape, pal, x, y, life);
    if (p)
        p->prio = sPrio ? sPrio : 1;
    return p;
}

static void Fade(struct RtPart *p, u8 pal2, u8 at2, u8 pal3, u8 at3)
{
    struct RtPartX *x;
    if (!p)
        return;
    x = RtFx_X(p);
    x->pal2 = pal2;
    x->at2 = at2;
    x->pal3 = pal3;
    x->at3 = at3;
}

// particle flying from (x,y) toward angle ang at speed (Q8 px/frame); flat = ground plane (y halved)
static struct RtPart *Fly(u8 shape, u8 pal, s32 x, s32 y, u8 ang, s32 speed, u8 life, bool8 flat)
{
    struct RtPart *p = Sp(shape, pal, x, y, life);
    if (p)
    {
        p->vx = RtCos(ang) * speed / 256;
        p->vy = RtSin(ang) * speed / 256;
        if (flat)
            p->vy /= 2;
    }
    return p;
}

static void Drag(struct RtPart *p, u8 drag)
{
    if (p)
        RtFx_X(p)->drag = drag;
}

static void Scale(struct RtPart *p, u8 scale, s8 dscale)
{
    if (p)
    {
        p->flags |= PF_SCALE;
        p->scale = scale;
        p->dscale = dscale;
    }
}

static void Spin(struct RtPart *p, s8 spin)
{
    if (p)
        RtFx_X(p)->spin = spin;
}

static void Blend(struct RtPart *p)
{
    if (p)
        p->flags |= PF_BLEND;
}

static void FadeOut(struct RtPart *p)
{
    if (p)
        RtFx_X(p)->flags |= PX_FADE;
}

static void Stretch(struct RtPart *p)
{
    if (p)
        RtFx_X(p)->flags |= PX_STRETCH;
}

static void Accel(struct RtPart *p, s8 ax, s8 ay)
{
    if (p)
    {
        RtFx_X(p)->ax = ax;
        RtFx_X(p)->ay = ay;
    }
}

static EWRAM_DATA u32 sHash = 0;
static u8 Rnd(void)
{
    sHash = sHash * 1103515245 + 12345;
    return sHash >> 16;
}

static s32 CenterY(struct RtActor *a)
{
    return a->y - TO_FX(a->height / 2);
}

// ---------------------------------------------------------------------------
// lightning: a jagged polyline drawn with rotated bolt segments (screen space)
// ---------------------------------------------------------------------------
static void BoltLine(s32 x0, s32 y0, s32 x1, s32 y1, u8 pal, u32 seed, s32 jag, u8 branches, u8 bucket)
{
    s32 dx = x1 - x0, dy = y1 - y0;
    s32 len = RtDist(dx, dy);
    s32 n, i;
    s32 px[16], py[16];
    if (len < 4)
        return;
    n = len / 16 + 1;
    if (n > 15)
        n = 15;
    sHash = seed;
    for (i = 0; i <= n; i++)
    {
        s32 j = (i == 0 || i == n) ? 0 : ((s32)(Rnd() % (jag * 2 + 1)) - jag);
        px[i] = x0 + dx * i / n + (-dy * j) / len;
        py[i] = y0 + dy * i / n + (dx * j) / len;
    }
    for (i = 0; i < n; i++)
    {
        s32 sx = px[i + 1] - px[i], sy = py[i + 1] - py[i];
        s32 sl = RtDist(sx, sy);
        u8 ang = RtAtan2(sx, sy);
        u8 shape = sl > 18 ? FX_LBOLT32 : FX_LBOLT16;
        s32 sc = sl * 16 / (shape == FX_LBOLT32 ? 32 : 16);
        if (sc < 8) sc = 8;
        if (sc > 30) sc = 30;
        RtFx_Draw(shape, (seed >> 1) & 1, pal, (px[i] + px[i + 1]) / 2, (py[i] + py[i + 1]) / 2, bucket, ang, sc, RFD_AFFINE);
    }
    // forks
    for (i = 0; i < branches && n > 2; i++)
    {
        s32 k = 1 + Rnd() % (n - 1);
        u8 ang = RtAtan2(dx, dy) + ((Rnd() & 1) ? 36 : -36) + (s8)(Rnd() % 16 - 8);
        s32 l = 10 + Rnd() % 10;
        s32 ex = px[k] + RtCos(ang) * l / 256, ey = py[k] + RtSin(ang) * l / 256;
        RtFx_Draw(FX_LBOLT16, 0, pal, (px[k] + ex) / 2, (py[k] + ey) / 2, bucket, ang, l * 16 / 16, RFD_AFFINE);
    }
}

// ---------------------------------------------------------------------------
// impacts
// ---------------------------------------------------------------------------
static u8 TypeImpact(u8 type)
{
    switch (type)
    {
    case TYPE_FIRE: return VI_FIRE;
    case TYPE_ELECTRIC: return VI_ELEC;
    case TYPE_WATER: return VI_WATER;
    case TYPE_GRASS: return VI_GRASS;
    case TYPE_ICE: return VI_ICE;
    case TYPE_ROCK: return VI_ROCK;
    case TYPE_GROUND: return VI_GROUND;
    case TYPE_PSYCHIC: return VI_PSY;
    case TYPE_GHOST: return VI_GHOST;
    case TYPE_DARK: return VI_DARK;
    case TYPE_FIGHTING: return VI_FIGHT;
    case TYPE_POISON: return VI_POISON;
    case TYPE_DRAGON: return VI_DRAGON;
    case TYPE_STEEL: return VI_STEEL;
    case TYPE_BUG: return VI_BUG;
    case TYPE_FLYING: return VI_FLYING;
    }
    return VI_NORMAL;
}

static void ImpactRing(u8 pal, s32 x, s32 y, u8 scale0, s8 ds, u8 life)
{
    struct RtPart *p = Sp(FX_RING32, pal, x, y, life);
    Scale(p, scale0, ds);
    Blend(p);
}

static void ImpactGlow(u8 pal, s32 x, s32 y, u8 scale0, s8 ds, u8 life)
{
    struct RtPart *p = Sp(FX_GLOW32, pal, x, y, life);
    Scale(p, scale0, ds);
    Blend(p);
}

static void DoImpact(u8 vi, u8 pal, u8 pal2, s32 x, s32 y, bool8 heavy)
{
    struct RtPart *p;
    s32 k, n = heavy ? 2 : 1;
    switch (vi)
    {
    case VI_NONE:
        return;
    case VI_FIRE:
        ImpactGlow(FXP_E_HOT, x, y, 10, 2, 10);
        p = Sp(FX_FIRE32, FXP_E_FIRE, x, y - TO_FX(4), 16);
        Scale(p, 12, 1);
        for (k = 0; k < 5 * n; k++)
        {
            p = Fly(FX_EMBER8, FXP_E_HOT, x, y, Random(), 300 + (Random() & 255), 16 + (Random() & 7), FALSE);
            Accel(p, 0, -3);
            Fade(p, FXP_E_FIRE, 10, FXP_E_SMOKE, 4);
        }
        for (k = 0; k < 2; k++)
        {
            p = Fly(FX_PUFF16, FXP_E_SMOKE, x + TO_FX((Random() % 13) - 6), y, 192, 90, 26, FALSE);
            Blend(p);
            Drag(p, 16);
        }
        break;
    case VI_ELEC:
        ImpactGlow(FXP_E_ELEC, x, y, 10, 2, 8);
        p = Sp(FX_FLARE32, FXP_E_ELEC, x, y, 10);
        Spin(p, 6);
        for (k = 0; k < 3 * n; k++)
        {
            p = Sp(FX_CRACKLE16, (k & 1) ? FXP_E_BOLT : FXP_E_ELEC, x + TO_FX((Random() % 25) - 12), y + TO_FX((Random() % 21) - 10), 10 + (Random() & 3));
            if (p)
                p->frameRate = 2;
        }
        for (k = 0; k < 4 * n; k++)
        {
            p = Fly(FX_SPARKLE8, FXP_E_ELEC, x, y, Random(), 400 + (Random() & 255), 14, FALSE);
            Drag(p, 13);
        }
        break;
    case VI_WATER:
        p = Sp(FX_SPLASH32, FXP_E_WATER, x, y + TO_FX(4), 16);
        ImpactRing(FXP_E_FOAM, x, y, 8, 2, 10);
        for (k = 0; k < 6 * n; k++)
        {
            p = Fly(FX_FOAM8, (k & 1) ? FXP_E_FOAM : FXP_E_WATER, x, y, 128 + 16 + Random() % 96 + 64, 350 + (Random() & 255), 18, FALSE);
            Accel(p, 0, 10);
            Drag(p, 15);
        }
        break;
    case VI_GRASS:
        ImpactGlow(FXP_E_GRASS, x, y, 8, 2, 8);
        for (k = 0; k < 5 * n; k++)
        {
            p = Fly(FX_LEAF16B, FXP_E_GRASS, x, y, Random(), 300 + (Random() & 255), 20 + (Random() & 7), FALSE);
            Spin(p, (Random() & 1) ? 10 : -10);
            Drag(p, 14);
            FadeOut(p);
        }
        for (k = 0; k < 3; k++)
            Fly(FX_SPARKLE8, FXP_E_HEAL, x, y, Random(), 300, 14, FALSE);
        break;
    case VI_ICE:
        p = Sp(FX_FLARE32, FXP_E_ICE, x, y, 10);
        Spin(p, -5);
        for (k = 0; k < 6 * n; k++)
        {
            p = Fly(FX_SHARD16B, FXP_E_ICE, x, y, k * 43 + (Random() & 15), 420 + (Random() & 127), 14, FALSE);
            Stretch(p);
            Drag(p, 13);
        }
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_SNOW8, FXP_E_ICE, x, y, Random(), 200, 22, FALSE);
            Spin(p, 6);
            Accel(p, 0, 2);
        }
        break;
    case VI_ROCK:
    case VI_GROUND:
        p = Sp(FX_BOOM32, vi == VI_ROCK ? FXP_E_ROCK : FXP_E_DUST, x, y, 10);
        for (k = 0; k < 6 * n; k++)
        {
            p = Fly(FX_DEBRIS8, vi == VI_ROCK ? FXP_E_ROCK : FXP_E_MUD, x, y, 128 + Random() % 128 + 64, 380 + (Random() & 255), 22, FALSE);
            Accel(p, 0, 12);
            Drag(p, 15);
            if (p)
                p->frameRate = 3;
        }
        for (k = 0; k < 2 + n; k++)
        {
            p = Fly(FX_PUFF16, FXP_E_DUST, x + TO_FX((Random() % 17) - 8), y + TO_FX(6), (Random() & 1) ? 0 : 128, 120, 24, TRUE);
            Blend(p);
        }
        break;
    case VI_PSY:
        p = Sp(FX_PSYWAVE32, FXP_E_PSY, x, y, 14);
        Scale(p, 8, 2);
        Blend(p);
        p = Sp(FX_PSYWAVE32, FXP_E_PINK, x, y, 18);
        Scale(p, 4, 2);
        Blend(p);
        for (k = 0; k < 5; k++)
            Fly(FX_SPARKLE8, FXP_E_PINK, x, y, Random(), 260, 16, FALSE);
        break;
    case VI_GHOST:
        ImpactGlow(FXP_E_GHOST, x, y, 12, 1, 14);
        for (k = 0; k < 5 * n; k++)
        {
            p = Fly(FX_WISP16, FXP_E_GHOST, x, y, Random(), 220, 22, FALSE);
            Accel(p, 0, -4);
            FadeOut(p);
        }
        break;
    case VI_DARK:
        p = Sp(FX_BOOM32, FXP_E_DARK, x, y, 10);
        for (k = 0; k < 3 * n; k++)
        {
            p = Fly(FX_PUFF16, FXP_E_DARK, x, y, Random(), 160, 20, FALSE);
        }
        break;
    case VI_FIGHT:
    case VI_NORMAL:
    case VI_DEFAULT:
        p = Sp(FX_BOOM32, vi == VI_FIGHT ? FXP_E_FIGHT : FXP_E_HOT, x, y, 9);
        if (heavy)
            Scale(p, 18, 1);
        for (k = 0; k < 6; k++)
        {
            p = Fly(FX_SPEED16, FXP_E_WHITE, x, y, k * 43 + (Random() & 15), 520, 8, FALSE);
            Stretch(p);
        }
        break;
    case VI_POISON:
        for (k = 0; k < 4 * n; k++)
        {
            p = Fly(FX_GOO16, FXP_E_POISON, x, y, 128 + 32 + Random() % 64 + 32, 330 + (Random() & 127), 18, FALSE);
            Accel(p, 0, 10);
            if (p)
                p->frameRate = 4;
        }
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_MOTE8, FXP_E_POISON, x + TO_FX((Random() % 17) - 8), y, 192, 150, 24, FALSE);
            FadeOut(p);
        }
        break;
    case VI_DRAGON:
        p = Sp(FX_FLARE32, FXP_E_DRAGON, x, y, 12);
        Spin(p, 8);
        ImpactGlow(FXP_E_DRAGON, x, y, 10, 2, 10);
        for (k = 0; k < 5 * n; k++)
        {
            p = Fly(FX_FIRE16, FXP_E_DRAGON, x, y, Random(), 260 + (Random() & 127), 16, FALSE);
            Accel(p, 0, -3);
        }
        break;
    case VI_STEEL:
        p = Sp(FX_FLARE32, FXP_E_STEEL, x, y, 10);
        for (k = 0; k < 7 * n; k++)
        {
            p = Fly(FX_SPARKLE8, (k & 1) ? FXP_E_WHITE : FXP_E_GOLD, x, y, 128 + Random() % 128 + 64, 450 + (Random() & 255), 16, FALSE);
            Accel(p, 0, 9);
            Stretch(p);
        }
        break;
    case VI_BUG:
        p = Sp(FX_BOOM32, FXP_E_BUG, x, y, 8);
        Scale(p, 12, 0);
        for (k = 0; k < 5; k++)
            Fly(FX_SPARKLE8, FXP_E_BUG, x, y, Random(), 320, 14, FALSE);
        break;
    case VI_FLYING:
        for (k = 0; k < 3; k++)
        {
            p = Fly(FX_PUFF16, FXP_E_SKY, x, y, k * 85 + (Random() & 31), 200, 18, FALSE);
            Blend(p);
        }
        for (k = 0; k < 3; k++)
        {
            p = Fly(FX_FEATHER16, FXP_FLYING, x, y, Random(), 220, 26, FALSE);
            Accel(p, 0, 2);
            Spin(p, 4);
        }
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_SPEED16, FXP_E_WHITE, x, y, k * 64 + 32, 480, 8, FALSE);
            Stretch(p);
        }
        break;
    }
    (void)pal;
    (void)pal2;
}

void RtVfx_Impact(u16 move, u8 style, u8 pal, s32 x, s32 y)
{
    sPrio = 1;
    const struct RtVfxDef *v = V(move);
    u8 vi = v->impact;
    if (move == MOVE_NONE || move >= MOVES_COUNT)
    {
        RtFx_Impact(style, pal, x, y);
        return;
    }
    if (vi == VI_DEFAULT)
        vi = TypeImpact(gBattleMoves[move].type);
    DoImpact(vi, v->pal, v->pal2, x, y, (v->flags & VF_HEAVY) != 0);
}

// ---------------------------------------------------------------------------
// charge-up (moves with a long windup)
// ---------------------------------------------------------------------------
bool8 RtVfx_Charge(struct RtActor *a, u16 move, u16 t, u16 windup)
{
    sPrio = 1;
    const struct RtVfxDef *v = V(move);
    s32 cy = CenterY(a);
    struct RtPart *p;
    u8 pal = v->pal;
    if (v->style == VS_NONE && pal == 0)
        return FALSE;
    if (t == 1)
    {
        p = Sp(FX_RING32, pal, a->x, a->y, windup > 40 ? 40 : windup);
        Scale(p, 30, -1);
        if (p)
            p->flags |= PF_GROUND | PF_BLEND;
        PlaySE(SE_M_CHARGE);
        if (v->flags & VF_DIM)
            RtFx_Dim(3, windup);
    }
    if (v->flags & VF_DIM)
        RtFx_Dim(3 + (t * 4) / (windup ? windup : 1), 4);
    // motes and sparks drawn in from all around
    if ((t & 1) == 0)
    {
        u8 ang = Random();
        s32 r = 30 + (Random() & 15);
        u8 life = 14;
        p = Sp((t & 2) ? FX_SPARKLE8 : FX_MOTE8, (t & 4) ? pal : v->pal2, a->x + RtCos(ang) * r, cy + RtSin(ang) * r / 2, life);
        if (p)
        {
            p->vx = -RtCos(ang) * r / life;
            p->vy = -RtSin(ang) * r / 2 / life;
            RtFx_X(p)->drag = 16;
        }
    }
    // a growing glow at the core
    if ((t & 3) == 0)
    {
        u8 sc = 4 + (t * 9) / (windup ? windup : 1);
        p = Sp(FX_GLOW32, pal, a->x, cy, 5);
        Scale(p, sc, 0);
        Blend(p);
    }
    switch (v->style)
    {
    case VS_SOLAR:
        if ((t & 7) == 0)
        {
            // sunlight pouring down onto the user
            p = Sp(FX_SUNRAY16, FXP_E_SOLAR, a->x + TO_FX((Random() % 33) - 16), cy - TO_FX(40), 20);
            if (p)
                p->vy = 400;
            Blend(p);
        }
        break;
    case VS_THUNDER:
    case VS_LIGHTNING:
        if ((t & 3) == 2)
        {
            p = Sp(FX_CRACKLE16, (t & 4) ? FXP_E_BOLT : FXP_E_ELEC, a->x + TO_FX((Random() % 25) - 12), cy + TO_FX((Random() % 21) - 10), 6);
            if (p)
                p->frameRate = 2;
        }
        break;
    case VS_PUNCH:
        if ((t & 3) == 0)
        {
            p = Sp(FX_FIRE16, pal, a->x + TO_FX((Random() % 21) - 10), a->y, 16);
            if (p)
                p->vy = -200;
            Blend(p);
        }
        break;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// release: muzzle flash and screen effects
// ---------------------------------------------------------------------------
void RtVfx_Execute(struct RtActor *a, u16 move)
{
    sPrio = 1;
    const struct RtVfxDef *v = V(move);
    s32 mx = a->x + RtCos(a->act.angle) * (a->radius + 4), my = CenterY(a) + RtSin(a->act.angle) * (a->radius + 4) / 2;
    struct RtPart *p;
    if (v->flags & VF_SHAKE2)
        RtFx_Shake(3, 16);
    else if (v->flags & VF_SHAKE1)
        RtFx_Shake(1, 10);
    if (v->flags & VF_DIM)
        RtFx_Dim(6, 30);
    if (v->flags & VF_WAVE)
        RtFx_Wave(3, 50);
    if (v->flags & VF_FLASH)
        RtFx_Flash(0, 6);
    switch (v->style)
    {
    case VS_LIGHTNING:
    case VS_BEAM:
    case VS_ICEBEAM:
    case VS_AURORA:
    case VS_PSYBEAM:
    case VS_SIGNAL:
    case VS_SOLAR:
    case VS_HYPER:
    case VS_HELIX:
    case VS_FLAMES:
    case VS_DBREATH:
    case VS_WATERJET:
    case VS_ORB:
    case VS_FIREBALL:
    case VS_DARKORB:
    case VS_ELECSHOT:
        p = Sp(FX_FLARE32, v->pal, mx, my, 10);
        Spin(p, 5);
        if (v->flags & VF_BIG)
            Scale(p, 18, 0);
        ImpactGlow(v->pal, mx, my, (v->flags & VF_BIG) ? 10 : 7, 1, 8);
        break;
    case VS_QUAKE:
        RtFx_Shake(4, 30);
        break;
    case VS_EXPLODE:
        RtFx_Flash(0, 12);
        RtFx_Dim(8, 20);
        {
            s32 k;
            s32 cy = CenterY(a);
            p = Sp(FX_BOOM32, FXP_E_HOT, a->x, cy, 18);
            Scale(p, 20, 2);
            for (k = 0; k < 8; k++)
            {
                p = Fly(FX_FIRE32, FXP_E_FIRE, a->x, cy, k * 32, 380, 22, FALSE);
                Fade(p, FXP_E_SMOKE, 8, 0xFF, 0);
                Accel(p, 0, -2);
            }
            for (k = 0; k < 6; k++)
            {
                p = Fly(FX_PUFF32, FXP_E_SMOKE, a->x, cy, k * 43, 200, 36, FALSE);
                Blend(p);
            }
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// streams: nozzle effects while channelling
// ---------------------------------------------------------------------------
void RtVfx_StreamTick(struct RtActor *a, u16 move, u16 t)
{
    sPrio = 4;
    const struct RtVfxDef *v = V(move);
    s32 mx = a->x + RtCos(a->act.angle) * (a->radius + 4), my = CenterY(a) + RtSin(a->act.angle) * (a->radius + 4) / 2;
    struct RtPart *p;
    switch (v->style)
    {
    case VS_FLAMES:
    case VS_DBREATH:
        if ((t & 1) == 0)
        {
            p = Fly(FX_EMBER8, v->style == VS_DBREATH ? FXP_E_DRAGON : FXP_E_HOT, mx, my, a->act.angle + (s8)(Random() % 41 - 20), 500, 14, FALSE);
            Fade(p, v->style == VS_DBREATH ? FXP_E_GHOST : FXP_E_FIRE, 6, 0xFF, 0);
        }
        if ((t & 3) == 0)
            ImpactGlow(v->style == VS_DBREATH ? FXP_E_DRAGON : FXP_E_HOT, mx, my, 9 + (t & 4), 0, 4);
        break;
    case VS_HELIX:
    case VS_WATERJET:
        if ((t & 1) == 0)
        {
            p = Fly(FX_FOAM8, FXP_E_FOAM, mx, my, a->act.angle + 64 + ((Random() & 1) ? 0 : 128), 200 + (Random() & 127), 10, FALSE);
            Accel(p, 0, 6);
        }
        if ((t & 3) == 0)
            ImpactGlow(FXP_E_FOAM, mx, my, 8, 0, 4);
        break;
    case VS_GAS:
    case VS_SANDS:
    case VS_SNOWS:
    case VS_POWDERS:
    case VS_BUBBLES:
        break;
    }
}

// ---------------------------------------------------------------------------
// projectile trails
// ---------------------------------------------------------------------------
bool8 RtVfx_ProjTrail(struct RtProj *p)
{
    sPrio = 4;
    const struct RtVfxDef *v = V(p->move);
    struct RtPart *pt;
    s32 x = p->x, y = p->y - TO_FX(p->z);
    if (p->param & 8)
        return v->style != VS_NONE; // stream particles: drawn, no trail
    switch (v->style)
    {
    case VS_NONE:
        return FALSE;
    case VS_ORB:
    case VS_WATERBALL:
    case VS_BUBBLE:
        if ((p->age & 1) == 0)
        {
            pt = Sp(FX_MOTE8, (p->age & 2) ? v->pal : v->pal2, x + TO_FX((Random() % 7) - 3), y + TO_FX((Random() % 7) - 3), 14);
            FadeOut(pt);
            if (pt) { pt->vx = -p->vx / 6; pt->vy = -p->vy / 6; }
        }
        return TRUE;
    case VS_FIREBALL:
        pt = Sp(FX_EMBER8, FXP_E_HOT, x + TO_FX((Random() % 7) - 3), y + TO_FX((Random() % 7) - 3), 16);
        Fade(pt, FXP_E_FIRE, 10, FXP_E_SMOKE, 4);
        Accel(pt, 0, -2);
        if ((p->age & 3) == 0)
        {
            pt = Sp(FX_PUFF16, FXP_E_SMOKE, x, y, 20);
            Blend(pt);
            if (pt) pt->vy = -60;
        }
        return TRUE;
    case VS_ELECSHOT:
        if ((p->age & 1) == 0)
        {
            pt = Fly(FX_SPARKLE8, (p->age & 2) ? FXP_E_ELEC : FXP_E_BOLT, x, y, Random(), 160, 10, FALSE);
        }
        return TRUE;
    case VS_DARKORB:
        if ((p->age & 1) == 0)
        {
            pt = Sp(FX_WISP16, FXP_E_GHOST, x + TO_FX((Random() % 9) - 4), y + TO_FX((Random() % 9) - 4), 18);
            Accel(pt, 0, -3);
            FadeOut(pt);
        }
        return TRUE;
    case VS_GOO:
    case VS_MUDBALL:
        if ((p->age & 3) == 0)
        {
            pt = Sp(FX_MOTE8, v->pal, x, y, 16);
            Accel(pt, 0, 10);
        }
        return TRUE;
    case VS_LEAF:
    case VS_STAR:
        if ((p->age & 1) == 0)
        {
            static const u8 sRainbow[] = {FXP_E_GOLD, FXP_E_PINK, FXP_E_SKY, FXP_E_HEAL};
            u8 pal = v->style == VS_STAR ? sRainbow[(p->age >> 1) & 3] : v->pal2;
            pt = Sp(FX_SPARKLE8, pal, x + TO_FX((Random() % 5) - 2), y + TO_FX((Random() % 5) - 2), 14);
        }
        return TRUE;
    case VS_SHARD:
        if ((p->age & 1) == 0)
        {
            pt = Sp(FX_SNOW8, FXP_E_ICE, x + TO_FX((Random() % 7) - 3), y + TO_FX((Random() % 7) - 3), 16);
            Spin(pt, 5);
        }
        return TRUE;
    case VS_ROCK:
        if ((p->age & 3) == 0)
        {
            pt = Sp(FX_PUFF16, FXP_E_DUST, x, y, 14);
            Blend(pt);
        }
        return TRUE;
    case VS_BLADE:
    case VS_NEEDLE:
        if ((p->age & 1) == 0)
        {
            pt = Sp(FX_SPEED16, FXP_E_WHITE, x - p->vx * 2, y - p->vy * 2, 6);
            if (pt) { pt->angle = p->angle; Stretch(pt); pt->vx = p->vx / 4; pt->vy = p->vy / 4; }
        }
        return TRUE;
    case VS_TWISTER:
        if ((p->age & 1) == 0)
        {
            pt = Sp((p->age & 2) ? FX_LEAF16B : FX_DEBRIS8, (p->age & 2) ? FXP_E_GRASS : FXP_E_ROCK, x, y - TO_FX(8), 20);
            Spin(pt, 12);
            if (pt) { pt->vx = p->vx + RtCos(p->age * 24) / 2; pt->vy = p->vy - 160; }
        }
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// drawing of projectiles
// ---------------------------------------------------------------------------
static u8 StreamScale(struct RtProj *p, u8 lo, u8 hi)
{
    u16 len = p->len ? p->len : 1;
    u16 age = p->age > len ? len : p->age;
    return lo + (hi - lo) * age / len;
}

static bool8 DrawStream(struct RtProj *p, const struct RtVfxDef *v, s32 sx, s32 sy)
{
    u16 len = p->len ? p->len : 1;
    u8 frac = p->age * 16 / len; // 0..16
    u8 sc;
    switch (v->style)
    {
    case VS_FLAMES:
        {
            u8 pal = frac < 5 ? FXP_E_HOT : (frac < 12 ? FXP_E_FIRE : FXP_E_SMOKE);
            sc = StreamScale(p, 9, 24);
            if (frac >= 13)
                return RtFx_Draw(FX_PUFF16, 1, FXP_E_SMOKE, sx, sy, 4, 0, sc, RFD_BLEND);
            RtFx_Draw(frac < 8 ? FX_FIRE16 : FX_FIRE32, (p->age + p->param2) & 3, pal, sx, sy, 4, p->angle + 64, frac < 8 ? sc : sc * 2 / 3, RFD_AFFINE);
            if ((p->age & 3) == 0)
                RtFx_Draw(FX_GLOW32, 0, FXP_E_HOT, sx, sy, 4, 0, sc * 2 / 3, RFD_BLEND);
            return TRUE;
        }
    case VS_DBREATH:
        {
            // dense, straight column of blue-violet dragonfire
            sc = StreamScale(p, 12, 20);
            if ((p->age & 3) == 0)
                RtFx_Draw(FX_GLOW32, 0, FXP_E_DRAGON, sx, sy, 4, 0, sc * 2 / 3, RFD_BLEND);
            RtFx_Draw(frac < 7 ? FX_FIRE16 : FX_FIRE32, (p->age >> 1) % (frac < 7 ? 6 : 4), frac < 11 ? FXP_E_DRAGON : FXP_E_GHOST, sx, sy, 4, p->angle + 64, frac < 7 ? sc : sc * 2 / 3, RFD_AFFINE);
            return TRUE;
        }
    case VS_HELIX:
        {
            // two water jets braided around the aim line: each piece follows the tangent of its strand, the
            // strand in front is lighter than the one behind
            u8 perp = p->angle + 64;
            u8 ph = p->age * 20 + gRt->frame * 9;
            s32 amp = 3 + (p->age > 12 ? 12 : p->age) * 9 / 12;
            s32 off = RtSin(ph) * amp / 256;
            s32 dep = RtCos(ph);
            s8 tilt = (s8)(RtCos(ph) * 20 / 256);
            s32 k;
            sc = StreamScale(p, 13, 19);
            for (k = 1; k >= 0; k--)
            {
                bool8 front = k ? (dep < 0) : (dep >= 0);
                s32 o = k ? -off : off;
                s32 x = sx + RtCos(perp) * o / 256, y = sy + RtSin(perp) * o / 256;
                if (front)
                    continue;
                RtFx_Draw(FX_JET16, (p->age >> 2) & 1, FXP_WATER, x, y, 4, p->angle + (k ? -tilt : tilt), sc - 2, RFD_AFFINE);
            }
            for (k = 0; k < 2; k++)
            {
                bool8 front = k ? (dep < 0) : (dep >= 0);
                s32 o = k ? -off : off;
                s32 x = sx + RtCos(perp) * o / 256, y = sy + RtSin(perp) * o / 256;
                if (!front)
                    continue;
                RtFx_Draw(FX_JET16, (p->age >> 2) & 1, FXP_E_WATER, x, y, 4, p->angle + (k ? -tilt : tilt), sc + 1, RFD_AFFINE);
            }
            return TRUE;
        }
    case VS_WATERJET:
        sc = StreamScale(p, 12, 20);
        RtFx_Draw(FX_JET16, (p->age >> 2) & 1, (p->age & 2) ? FXP_E_WATER : FXP_E_FOAM, sx, sy, 4, p->angle, sc, RFD_AFFINE);
        return TRUE;
    case VS_BUBBLES:
        {
            s32 w = RtSin(p->age * 24 + p->param2 * 40) * 3 / 256;
            sc = StreamScale(p, 10, 20);
            RtFx_Draw(FX_BUBBLE16, (p->age >> 3) & 1, FXP_WATER, sx + w, sy - w, 4, 0, sc, RFD_AFFINE);
            return TRUE;
        }
    case VS_GAS:
        sc = StreamScale(p, 10, 28);
        return RtFx_Draw(frac < 8 ? FX_PUFF16 : FX_PUFF32, frac < 8 ? (frac >> 2) : ((frac - 8) >> 2) % 3, v->pal, sx, sy, 4, 0, frac < 8 ? sc : sc / 2 + 4, RFD_BLEND);
    case VS_POWDERS:
        RtFx_Draw(FX_SPARKLE8, (p->age >> 1) & 3, v->pal, sx + RtSin(p->age * 20) * 4 / 256, sy, 4, 0, 16, 0);
        return RtFx_Draw(FX_POWDER8, 0, v->pal2, sx - RtSin(p->age * 20) * 4 / 256, sy + 3, 4, 0, 20, RFD_AFFINE);
    case VS_SNOWS:
        RtFx_Draw(FX_SNOW8, (p->age >> 2) & 1, FXP_E_ICE, sx, sy, 4, p->age * 12, 20, RFD_AFFINE);
        if ((p->age & 1) == 0)
            RtFx_Draw(FX_PUFF16, 0, FXP_E_ICE, sx, sy, 4, 0, StreamScale(p, 8, 20), RFD_BLEND);
        return TRUE;
    case VS_SANDS:
        RtFx_Draw(FX_PUFF16, (frac >> 3) & 1, FXP_E_SAND, sx, sy, 4, 0, StreamScale(p, 10, 22), RFD_BLEND);
        return RtFx_Draw(FX_GRAIN8, 0, FXP_SAND, sx + 3, sy - 2, 4, 0, 16, 0);
    }
    return FALSE;
}

// beam origin / end (screen) and where it really ends: the first foe in its way
static s32 BeamEnd(struct RtProj *p, s32 *ex, s32 *ey)
{
    struct RtActor *o = &gRt->actors[p->owner];
    s32 len = p->len, i;
    s32 x0 = FROM_FX(p->x), y0 = FROM_FX(p->y);
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 dx, dy, along, perp;
        if (!t->active || t->side == o->side || t->state == AST_GONE || !RtActor_IsAlive(t) || t->hidden)
            continue;
        dx = FROM_FX(t->x) - x0;
        dy = FROM_FX(t->y) - t->height / 2 - y0;
        along = (dx * RtCos(p->angle) + dy * RtSin(p->angle)) / 256;
        perp = (-dx * RtSin(p->angle) + dy * RtCos(p->angle)) / 256;
        if (perp < 0) perp = -perp;
        if (along > 8 && along < len && perp <= t->radius + p->radius)
            len = along;
    }
    *ex = x0 + RtCos(p->angle) * len / 256;
    *ey = y0 + RtSin(p->angle) * len / 256;
    return len;
}

static void DrawBeamBody(struct RtProj *p, u8 shape, u8 pal, u8 glowPal, u8 coreScale, u8 glowScale, s32 len)
{
    s32 x0 = FROM_FX(p->x) - gRt->camX, y0 = FROM_FX(p->y) - gRt->camY;
    s32 step = 32 * coreScale / 16 - 6, d;
    u8 fr = (p->age >> 1) % 3;
    if (step < 10)
        step = 10;
    for (d = step / 2; d < len + step / 2; d += step)
    {
        s32 dd = d > len - step / 2 ? len - step / 2 : d;
        s32 x = x0 + RtCos(p->angle) * dd / 256, y = y0 + RtSin(p->angle) * dd / 256;
        if (dd < 0)
            dd = 0;
        if (glowScale)
            RtFx_Draw(shape, fr, glowPal, x, y, 4, p->angle, glowScale, RFD_BLEND | RFD_AFFINE);
        RtFx_Draw(shape, fr, pal, x, y, 4, p->angle, coreScale, RFD_AFFINE);
    }
}

static bool8 DrawBeamStyle(struct RtProj *p, const struct RtVfxDef *v)
{
    s32 ex, ey;
    s32 len = BeamEnd(p, &ex, &ey);
    s32 x0 = FROM_FX(p->x) - gRt->camX, y0 = FROM_FX(p->y) - gRt->camY;
    s32 sex = ex - gRt->camX, sey = ey - gRt->camY;
    u8 pulse = (p->age & 2) ? 1 : 0;
    switch (v->style)
    {
    case VS_LIGHTNING:
        {
            u32 seed = (p->age >> 1) * 2654435761u + p->owner * 97;
            BoltLine(x0, y0, sex, sey, (p->age & 2) ? FXP_E_ELEC : FXP_E_BOLT, seed, 7, 2, 1);
            if (p->age & 1)
                BoltLine(x0, y0, sex, sey, FXP_E_WHITE, seed ^ 0x5A5A, 9, 1, 1);
            RtFx_Draw(FX_GLOW32, 0, FXP_E_ELEC, x0, y0, 4, 0, 12 + pulse * 3, RFD_BLEND);
            RtFx_Draw(FX_GLOW32, 0, FXP_E_ELEC, sex, sey, 4, 0, 14 + pulse * 3, RFD_BLEND);
            RtFx_Draw(FX_CRACKLE16, p->age % 3, FXP_E_WHITE, sex, sey, 4, 0, 16, 0);
            return TRUE;
        }
    case VS_HYPER:
        DrawBeamBody(p, FX_BEAM32, FXP_E_HOT, FXP_E_GOLD, 17 + pulse, 26 + pulse * 2, len);
        break;
    case VS_SOLAR:
        DrawBeamBody(p, FX_BEAM32, FXP_E_SOLAR, FXP_E_SOLAR, 20 + pulse, 30 + pulse * 2, len);
        break;
    case VS_ICEBEAM:
        DrawBeamBody(p, FX_BEAMTHIN32, FXP_E_ICE, FXP_E_ICE, 16, 24, len);
        {
            // crystals along the beam
            s32 d;
            for (d = 18; d < len; d += 22)
            {
                s32 x = x0 + RtCos(p->angle) * d / 256, y = y0 + RtSin(p->angle) * d / 256;
                u8 a = p->angle + ((d / 22) & 1 ? 40 : -40);
                RtFx_Draw(FX_CRYSTAL16, (d / 22 + (p->age >> 2)) & 1, FXP_E_ICE, x, y, 4, a, 14, RFD_AFFINE);
            }
        }
        break;
    case VS_AURORA:
        {
            static const u8 sCycle[] = {FXP_E_AURORA, FXP_E_PINK, FXP_E_HEAL, FXP_E_SKY};
            DrawBeamBody(p, FX_BEAM32, sCycle[(p->age >> 2) & 3], sCycle[((p->age >> 2) + 2) & 3], 15, 22, len);
        }
        break;
    case VS_PSYBEAM:
        DrawBeamBody(p, FX_BEAMTHIN32, FXP_E_PSY, FXP_E_PINK, 16, 22, len);
        {
            s32 d;
            for (d = (p->age * 4) % 24; d < len; d += 24)
            {
                s32 x = x0 + RtCos(p->angle) * d / 256, y = y0 + RtSin(p->angle) * d / 256;
                RtFx_Draw(FX_PSYWAVE32, 0, FXP_E_PINK, x, y, 4, p->angle, 9, RFD_BLEND | RFD_AFFINE);
            }
        }
        break;
    case VS_SIGNAL:
        DrawBeamBody(p, FX_BEAMTHIN32, (p->age & 2) ? FXP_E_BUG : FXP_E_PINK, FXP_E_SKY, 16, 22, len);
        break;
    case VS_BEAM:
    default:
        DrawBeamBody(p, FX_BEAM32, v->pal, v->pal2, 14 + pulse, 20, len);
        break;
    }
    // muzzle and tip flares
    RtFx_Draw(FX_FLARE32, p->age % 3, v->pal, x0, y0, 4, p->age * 6, (v->flags & VF_BIG) ? 20 : 15, RFD_AFFINE);
    RtFx_Draw(FX_GLOW32, 0, v->pal2, sex, sey, 4, 0, (v->flags & VF_BIG) ? 24 : 16, RFD_BLEND);
    RtFx_Draw(FX_FLARE32, (p->age + 1) % 3, v->pal, sex, sey, 4, -p->age * 7, 16, RFD_AFFINE);
    return TRUE;
}

static bool8 DrawMelee(struct RtProj *p, const struct RtVfxDef *v, s32 sx, s32 sy)
{
    u8 total = p->param2 ? 8 : 8;
    u8 fr = p->age * 3 / (total + 1);
    if (fr > 2)
        fr = 2;
    switch (v->style)
    {
    case VS_SLASH:
        RtFx_Draw(FX_ARC32, fr, v->pal2, sx, sy, 4, p->angle, 22, RFD_AFFINE | RFD_BLEND);
        return RtFx_Draw(FX_ARC32, fr, v->pal, sx, sy, 4, p->angle, 18, RFD_AFFINE);
    case VS_CLAW:
        RtFx_Draw(FX_CLAW32, fr, v->pal2, sx, sy, 4, p->angle, 21, RFD_AFFINE | RFD_BLEND);
        return RtFx_Draw(FX_CLAW32, fr, v->pal, sx, sy, 4, p->angle, 17, RFD_AFFINE);
    case VS_BITE:
        return RtFx_Draw(FX_JAWS32, fr, v->pal, sx, sy, 4, 0, 16, 0);
    case VS_KICK:
        RtFx_Draw(FX_GLOW32, 0, v->pal2, sx, sy, 4, 0, 12 + fr * 3, RFD_BLEND);
        return RtFx_Draw(FX_BOOM32, fr, v->pal, sx, sy, 4, p->angle, 12 + fr * 2, RFD_AFFINE);
    case VS_PUNCH:
        RtFx_Draw(FX_GLOW32, 0, v->pal2, sx, sy, 4, 0, 14, RFD_BLEND);
        return RtFx_Draw(FX_FIST32, 0, v->pal, sx, sy, 4, p->angle, 12 + fr * 2, RFD_AFFINE);
    case VS_HORN:
        RtFx_Draw(FX_SPEED16, 0, FXP_E_WHITE, sx - RtCos(p->angle) * 10 / 256, sy - RtSin(p->angle) * 10 / 256, 4, p->angle, 16, RFD_AFFINE);
        return RtFx_Draw(FX_HORN16, 0, v->pal, sx, sy, 4, p->angle, 18, RFD_AFFINE);
    }
    return FALSE;
}

static bool8 DrawWaveStyle(struct RtProj *p, const struct RtVfxDef *v, s32 sx, s32 sy)
{
    s32 half = p->len, i, n, step;
    u8 perp = p->angle + 64;
    switch (v->style)
    {
    case VS_SURF:
        step = 22;
        n = (half * 2) / step + 1;
        for (i = 0; i < n; i++)
        {
            s32 k = n == 1 ? 0 : -half + (half * 2) * i / (n - 1);
            s32 x = sx + RtCos(perp) * k / 256, y = sy + RtSin(perp) * k / 256;
            RtFx_Draw(FX_CREST32, ((p->age >> 2) + i) % 3, FXP_E_WATER, x, y - 6, 4, p->angle + 64, 16, RFD_AFFINE);
        }
        return TRUE;
    case VS_SOUND:
        step = 20;
        n = (half * 2) / step + 1;
        for (i = 0; i < n; i++)
        {
            s32 k = n == 1 ? 0 : -half + (half * 2) * i / (n - 1);
            s32 x = sx + RtCos(perp) * k / 256, y = sy + RtSin(perp) * k / 256;
            RtFx_Draw(FX_PSYWAVE32, (p->age >> 2) & 1, v->pal, x, y - 8, 4, p->angle, 12, RFD_AFFINE | RFD_BLEND);
        }
        return TRUE;
    case VS_HEAT:
        step = 16;
        n = (half * 2) / step + 1;
        for (i = 0; i < n; i++)
        {
            s32 k = n == 1 ? 0 : -half + (half * 2) * i / (n - 1);
            s32 x = sx + RtCos(perp) * k / 256, y = sy + RtSin(perp) * k / 256;
            RtFx_Draw(FX_FIRE16, ((p->age >> 1) + i) % 6, FXP_E_FIRE, x, y - 6, 4, 0, 16, RFD_BLEND);
        }
        return TRUE;
    }
    return FALSE;
}

bool8 RtVfx_DrawProj(struct RtProj *p, s32 sx, s32 sy)
{
    const struct RtVfxDef *v = V(p->move);
    if (p->move == MOVE_NONE || v->style == VS_NONE)
        return FALSE;
    switch (p->kind)
    {
    case PK_BULLET:
        if (p->delay)
            return FALSE;
        sy -= p->z;
        if (p->param & 8)
            return DrawStream(p, v, sx, sy);
        switch (v->style)
        {
        case VS_ORB:
            RtFx_Draw(FX_GLOW32, 0, v->pal2, sx, sy, 4, 0, 14 + ((p->age >> 1) & 1) * 2, RFD_BLEND);
            RtFx_Draw(FX_ORB16, (p->age >> 2) & 1, v->pal, sx, sy, 4, 0, (v->flags & VF_BIG) ? 22 : 16, 0);
            return TRUE;
        case VS_FIREBALL:
            RtFx_Draw(FX_GLOW32, 0, FXP_E_HOT, sx, sy, 4, 0, (v->flags & VF_BIG) ? 22 : 14, RFD_BLEND);
            RtFx_Draw((v->flags & VF_BIG) ? FX_FIRE32 : FX_FIRE16, (p->age >> 1) % ((v->flags & VF_BIG) ? 4 : 6), v->pal, sx, sy, 4, p->angle - 64, 16, RFD_AFFINE);
            return TRUE;
        case VS_ELECSHOT:
            RtFx_Draw(FX_GLOW32, 0, FXP_E_ELEC, sx, sy, 4, 0, 12, RFD_BLEND);
            RtFx_Draw(FX_CRACKLE16, (p->age >> 1) % 3, (p->age & 2) ? FXP_E_WHITE : FXP_E_ELEC, sx, sy, 4, p->age * 20, 20, RFD_AFFINE);
            return TRUE;
        case VS_DARKORB:
            RtFx_Draw(FX_GLOW32, 0, FXP_E_GHOST, sx, sy, 4, 0, 20, RFD_BLEND);
            RtFx_Draw(FX_DORB32, (p->age >> 2) % 3, FXP_E_GHOST, sx, sy, 4, p->age * 4, 13, RFD_AFFINE);
            return TRUE;
        case VS_GOO:
        case VS_MUDBALL:
            RtFx_Draw(FX_GOO16, (p->age >> 2) % 3, v->pal, sx, sy, 4, 0, (v->flags & VF_BIG) ? 22 : 16, 0);
            return TRUE;
        case VS_LEAF:
            return RtFx_Draw(FX_LEAF16B, (p->age >> 1) & 3, v->pal, sx, sy, 4, 0, 16, 0);
        case VS_STAR:
            RtFx_Draw(FX_GLOW32, 0, FXP_E_GOLD, sx, sy, 4, 0, 10, RFD_BLEND);
            return RtFx_Draw(FX_STAR16B, 0, FXP_E_GOLD, sx, sy, 4, p->age * 10, 16, RFD_AFFINE);
        case VS_SHARD:
            return RtFx_Draw(FX_SHARD16B, 0, v->pal, sx, sy, 4, p->angle, 18, RFD_AFFINE);
        case VS_ROCK:
            return RtFx_Draw(FX_ROCK16B, (p->age >> 3) & 1, FXP_E_ROCK, sx, sy, 4, p->age * 8, (v->flags & VF_BIG) ? 24 : 16, RFD_AFFINE);
        case VS_BLADE:
            RtFx_Draw(FX_ARC32, 1, v->pal, sx, sy, 4, p->angle, 16, RFD_AFFINE);
            return RtFx_Draw(FX_ARC32, 1, v->pal2, sx - (p->vx >> 7), sy - (p->vy >> 7), 4, p->angle, 18, RFD_AFFINE | RFD_BLEND);
        case VS_WATERBALL:
            RtFx_Draw(FX_GLOW32, 0, FXP_E_WATER, sx, sy, 4, 0, 14, RFD_BLEND);
            return RtFx_Draw(FX_BUBBLE16, (p->age >> 2) & 1, FXP_WATER, sx, sy, 4, 0, 20, RFD_AFFINE);
        case VS_BUBBLE:
            return RtFx_Draw(FX_BUBBLE16, (p->age >> 3) & 1, FXP_WATER, sx + RtSin(p->age * 12) * 3 / 256, sy, 4, 0, 16, 0);
        case VS_NEEDLE:
            return FALSE;
        case VS_TWISTER:
            RtFx_Draw(FX_TWISTER32, (p->age >> 1) % 3, v->pal, sx, sy - 6, 4, 0, 16, 0);
            return TRUE;
        }
        return FALSE;
    case PK_BEAM:
        if (p->delay)
            return FALSE;
        return DrawBeamStyle(p, v);
    case PK_MELEE:
        if (p->delay || !p->life)
            return FALSE;
        return DrawMelee(p, v, sx, sy - p->z);
    case PK_WAVE:
        if (p->delay)
            return FALSE;
        return DrawWaveStyle(p, v, sx, sy);
    }
    return FALSE;
}

bool8 RtVfx_BeamTick(struct RtProj *p)
{
    sPrio = 1;
    const struct RtVfxDef *v = V(p->move);
    struct RtActor *o = &gRt->actors[p->owner];
    s32 ex, ey, len;
    struct RtPart *pt;
    if (v->style == VS_NONE)
        return FALSE;
    len = BeamEnd(p, &ex, &ey);
    // particles shed along the beam
    if ((p->age & 1) == 0 && len > 16)
    {
        s32 k = 8 + Random() % (len - 8);
        u8 perp = p->angle + ((Random() & 1) ? 64 : 192);
        u8 shape = FX_SPARKLE8, pal = v->pal;
        switch (v->style)
        {
        case VS_ICEBEAM: shape = FX_SNOW8; break;
        case VS_LIGHTNING: shape = FX_CRACKLE16; pal = FXP_E_ELEC; break;
        case VS_SOLAR: pal = FXP_E_SOLAR; break;
        case VS_PSYBEAM: pal = FXP_E_PINK; break;
        }
        pt = Fly(shape, pal, p->x + RtCos(p->angle) * k, p->y + RtSin(p->angle) * k, perp, 120, 12, FALSE);
        if (pt && shape == FX_CRACKLE16)
            pt->frameRate = 2;
    }
    // continuous burst at the tip
    if ((p->age & 3) == 2)
        DoImpact(v->impact == VI_DEFAULT ? TypeImpact(gBattleMoves[p->move].type) : v->impact, v->pal, v->pal2, TO_FX(ex), TO_FX(ey), FALSE);
    (void)o;
    return TRUE;
}

// ---------------------------------------------------------------------------
// strikes from above (Thunder, Rock Slide, Blizzard...)
// ---------------------------------------------------------------------------
bool8 RtVfx_StrikeWarn(struct RtProj *p)
{
    sPrio = 1;
    const struct RtVfxDef *v = V(p->move);
    s32 sx = SX(p->x), sy = SY(p->y);
    switch (v->style)
    {
    case VS_ROCKFALL:
        if (p->delay <= 16)
        {
            s32 h = p->delay * 9;
            RtFx_Draw(FX_SHADOW16, 0, FXP_UI, sx, sy + 2, 3, 0, 16 + (16 - p->delay), RFD_AFFINE);
            RtFx_Draw((v->flags & VF_BIG) ? FX_ROCK32B : FX_ROCK16B, 0, FXP_E_ROCK, sx, sy - h - 8, 4, p->age * 6, 16, RFD_AFFINE);
            return TRUE;
        }
        return FALSE;
    case VS_METEOR:
        if (p->delay <= 20)
        {
            s32 h = p->delay * 8;
            RtFx_Draw(FX_GLOW32, 0, v->pal, sx + h / 3, sy - h - 8, 4, 0, 16, RFD_BLEND);
            RtFx_Draw(FX_STAR16B, 0, v->pal2, sx + h / 3, sy - h - 8, 4, p->age * 12, 16, RFD_AFFINE);
            return TRUE;
        }
        return FALSE;
    case VS_BLIZZARD:
        if ((p->age & 1) == 0)
        {
            struct RtPart *pt = Sp(FX_SNOW8, FXP_E_ICE, p->x + TO_FX((Random() % 41) - 20) + TO_FX(30), p->y - TO_FX(50 + (Random() & 15)), 20);
            if (pt) { pt->vx = -400; pt->vy = 600; }
            Spin(pt, 10);
        }
        return FALSE;
    }
    return FALSE;
}

bool8 RtVfx_Strike(struct RtProj *p)
{
    sPrio = 1;
    const struct RtVfxDef *v = V(p->move);
    struct RtPart *pt;
    s32 k;
    switch (v->style)
    {
    case VS_THUNDER:
        {
            // a huge bolt from the top of the screen: drawn as particles of bolt segments that live a few frames
            s32 sx = SX(p->x), sy = SY(p->y);
            s32 y = sy, x = sx;
            u32 seed = Random();
            sHash = seed;
            for (k = 0; k < 8 && y > -16; k++)
            {
                s32 ny = y - 18, nx = x + (s32)(Rnd() % 15) - 7;
                u8 ang = RtAtan2(nx - x, ny - y);
                pt = Sp(FX_LBOLT32, (k & 1) ? FXP_E_WHITE : FXP_E_ELEC, TO_FX((x + nx) / 2 + gRt->camX), TO_FX((y + ny) / 2 + gRt->camY), 10);
                if (pt)
                {
                    pt->angle = ang;
                    pt->flags |= PF_SCALE;
                    pt->scale = 18;
                    pt->dscale = 0;
                    pt->frameRate = 2;
                    pt->prio = 1;
                }
                x = nx;
                y = ny;
            }
            RtFx_Flash(0, 10);
            RtFx_Dim(8, 12);
            ImpactGlow(FXP_E_ELEC, p->x, p->y, 24, 2, 12);
            pt = Sp(FX_CRACK32, FXP_E_DARK, p->x, p->y, 60);
            if (pt) pt->flags |= PF_GROUND;
            DoImpact(VI_ELEC, v->pal, v->pal2, p->x, p->y - TO_FX(8), TRUE);
            ImpactRing(FXP_E_ELEC, p->x, p->y, 10, 3, 14);
            RtFx_Shake(3, 12);
        }
        return TRUE;
    case VS_ROCKFALL:
        DoImpact(VI_ROCK, v->pal, v->pal2, p->x, p->y, (v->flags & VF_BIG) != 0);
        pt = Sp(FX_CRACK32, FXP_E_DARK, p->x, p->y, 40);
        if (pt) pt->flags |= PF_GROUND;
        RtFx_Shake(2, 8);
        return TRUE;
    case VS_BLIZZARD:
        DoImpact(VI_ICE, v->pal, v->pal2, p->x, p->y - TO_FX(6), TRUE);
        return TRUE;
    case VS_METEOR:
        DoImpact(VI_PSY, v->pal, v->pal2, p->x, p->y - TO_FX(6), TRUE);
        RtFx_Flash(0, 6);
        return TRUE;
    case VS_PSYBLAST:
        RtFx_Wave(4, 40);
        DoImpact(VI_PSY, v->pal, v->pal2, p->x, p->y - TO_FX(10), TRUE);
        ImpactGlow(FXP_E_PSY, p->x, p->y - TO_FX(10), 14, 3, 16);
        for (k = 0; k < 3; k++)
        {
            pt = Sp(FX_PSYWAVE32, k == 1 ? FXP_E_PINK : FXP_E_PSY, p->x, p->y - TO_FX(10), 14 + k * 5);
            Scale(pt, 4 + k * 4, 3);
            Blend(pt);
        }
        return TRUE;
    case VS_PILLAR:
        for (k = 0; k < 5; k++)
        {
            pt = Sp(v->pal == FXP_E_WATER ? FX_JET16 : (v->pal == FXP_E_GRASS ? FX_LEAF16B : FX_FIRE32), v->pal, p->x + TO_FX((Random() % 9) - 4), p->y - TO_FX(6 + k * 10), 18);
            if (pt) { pt->vy = -200 - k * 40; pt->angle = 192; pt->flags |= PF_SCALE; pt->scale = 16; pt->dscale = 0; }
        }
        DoImpact(v->impact == VI_DEFAULT ? TypeImpact(gBattleMoves[p->move].type) : v->impact, v->pal, v->pal2, p->x, p->y, TRUE);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// bursts around the user (Earthquake, Explosion...)
// ---------------------------------------------------------------------------
bool8 RtVfx_AreaTick(struct RtProj *p)
{
    sPrio = 1;
    const struct RtVfxDef *v = V(p->move);
    struct RtPart *pt;
    u8 a;
    switch (v->style)
    {
    case VS_QUAKE:
        if ((p->age & 3) == 1)
        {
            a = Random();
            pt = Sp(FX_CRACK32, FXP_E_DARK, p->x + RtCos(a) * p->radius, p->y + RtSin(a) * p->radius / 2, 40);
            if (pt) { pt->flags |= PF_GROUND; pt->frame = Random() & 1; pt->frameRate = 0; }
        }
        if ((p->age & 1) == 0)
        {
            a = Random();
            pt = Fly(FX_DEBRIS8, FXP_E_ROCK, p->x + RtCos(a) * p->radius, p->y + RtSin(a) * p->radius / 2, 192 + (s8)(Random() % 33 - 16), 450, 22, FALSE);
            Accel(pt, 0, 14);
            if (pt) pt->frameRate = 3;
            pt = Sp(FX_PUFF16, FXP_E_DUST, p->x + RtCos(a + 30) * p->radius, p->y + RtSin(a + 30) * p->radius / 2, 22);
            Blend(pt);
        }
        return TRUE;
    case VS_EXPLODE:
        if ((p->age & 1) == 0)
        {
            a = Random();
            pt = Sp((p->age & 2) ? FX_FIRE32 : FX_PUFF32, (p->age & 2) ? FXP_E_FIRE : FXP_E_SMOKE, p->x + RtCos(a) * p->radius, p->y + RtSin(a) * p->radius / 2 - TO_FX(8), 18);
            if (pt) pt->vy = -150;
            if (!(p->age & 2)) Blend(pt);
        }
        return TRUE;
    case VS_PETALS:
        if ((p->age & 1) == 0)
        {
            a = Random();
            pt = Sp(FX_PETAL8, v->pal, p->x, p->y, 30);
            if (pt)
            {
                struct RtPartX *x = RtFx_X(pt);
                x->flags |= PX_ORBIT;
                x->owner = p->owner;
                x->orbit = 6;
                x->orbitAng = a;
                x->ay = 7;
                x->ax = 3;
                pt->frameRate = 3;
            }
        }
        return TRUE;
    case VS_NOVA:
        if (p->age == 1)
        {
            ImpactRing(v->pal, p->x, p->y - TO_FX(8), 6, 4, 16);
            ImpactGlow(v->pal2, p->x, p->y - TO_FX(8), 12, 2, 12);
        }
        if ((p->age & 1) == 0)
        {
            a = Random();
            Fly(FX_SPARKLE8, v->pal, p->x + RtCos(a) * p->radius, p->y + RtSin(a) * p->radius / 2 - TO_FX(6), a, 200, 14, TRUE);
        }
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// self-targeted moves
// ---------------------------------------------------------------------------
bool8 RtVfx_SelfCast(struct RtActor *a, u16 move)
{
    sPrio = 1;
    const struct RtVfxDef *v = V(move);
    s32 cy = CenterY(a);
    struct RtPart *p;
    s32 k;
    if (v->style == VS_NONE)
        return FALSE;
    p = Sp(FX_RING32, v->pal, a->x, a->y, 18);
    Scale(p, 6, 2);
    if (p) p->flags |= PF_GROUND | PF_BLEND;
    switch (v->style)
    {
    case VS_SHIELD:
        p = Sp(FX_HEX32, v->pal, a->x, cy, 30);
        Scale(p, 8, 2);
        Blend(p);
        if (p) { p->dscale = 2; p->frameRate = 4; }
        p = Sp(FX_HEX32, v->pal2, a->x, cy, 24);
        Scale(p, 26, 0);
        Blend(p);
        break;
    case VS_WALL:
        for (k = -1; k <= 1; k++)
        {
            p = Sp(FX_PANE16, v->pal, a->x + TO_FX(k * 14), cy - TO_FX(2), 34);
            Blend(p);
            if (p) { p->vy = -30; RtFx_X(p)->flags |= PX_FADE; }
        }
        break;
    case VS_HEAL:
        ImpactGlow(FXP_E_HEAL, a->x, cy, 10, 1, 20);
        for (k = 0; k < 10; k++)
        {
            p = Sp((k & 1) ? FX_PLUS8 : FX_SPARKLE8, (k & 1) ? FXP_HEAL : FXP_E_HEAL, a->x + TO_FX((Random() % 29) - 14), a->y - TO_FX(Random() % (a->height + 1)), 24 + (k & 3) * 3);
            if (p) p->vy = -150 - (Random() & 63);
        }
        break;
    case VS_AURA:
    default:
        ImpactGlow(v->pal, a->x, cy, 12, 1, 16);
        for (k = 0; k < 10; k++)
        {
            u8 ang = k * 25 + (Random() & 15);
            p = Sp((k & 1) ? FX_SPARKLE8 : FX_MOTE8, (k & 2) ? v->pal : v->pal2, a->x + RtCos(ang) * (a->radius + 4), a->y + RtSin(ang) * (a->radius + 4) / 2, 22 + (k & 3) * 4);
            if (p) { p->vy = -120 - (Random() & 63); FadeOut(p); }
        }
        break;
    }
    a->flash = 6;
    return TRUE;
}

// ---------------------------------------------------------------------------
// rushes: speed lines, afterimage dust, elemental trails
// ---------------------------------------------------------------------------
bool8 RtVfx_RushTick(struct RtActor *a, u16 move)
{
    sPrio = 4;
    const struct RtVfxDef *v = V(move);
    struct RtPart *p;
    u8 vi;
    if (v->style == VS_NONE)
        return FALSE;
    p = Sp(FX_SPEED16, FXP_E_WHITE, a->x - RtCos(a->act.angle) * a->radius, CenterY(a) + TO_FX((Random() % 13) - 6), 8);
    if (p) { p->angle = a->act.angle; p->flags |= PF_SCALE; p->scale = 16; RtFx_X(p)->flags |= PX_STRETCH; p->vx = RtCos(a->act.angle) / 2; p->vy = RtSin(a->act.angle) / 2; }
    if ((a->act.timer & 3) == 0)
    {
        p = Sp(FX_PUFF16, FXP_E_DUST, a->x, a->y, 16);
        Blend(p);
    }
    vi = v->impact == VI_DEFAULT ? TypeImpact(gBattleMoves[move].type) : v->impact;
    switch (vi)
    {
    case VI_FIRE:
        p = Sp(FX_FIRE16, FXP_E_FIRE, a->x + TO_FX((Random() % 17) - 8), CenterY(a) + TO_FX((Random() % 13) - 6), 14);
        Fade(p, FXP_E_SMOKE, 4, 0xFF, 0);
        Accel(p, 0, -3);
        break;
    case VI_ELEC:
        if (a->act.timer & 1)
        {
            p = Sp(FX_CRACKLE16, (a->act.timer & 2) ? FXP_E_ELEC : FXP_E_BOLT, a->x + TO_FX((Random() % 21) - 10), CenterY(a) + TO_FX((Random() % 17) - 8), 6);
            if (p) p->frameRate = 2;
        }
        break;
    case VI_WATER:
        p = Fly(FX_FOAM8, FXP_E_FOAM, a->x, CenterY(a), a->act.angle + 128 + (s8)(Random() % 61 - 30), 250, 14, FALSE);
        Accel(p, 0, 8);
        break;
    case VI_DRAGON:
    case VI_PSY:
    case VI_GHOST:
    case VI_STEEL:
    case VI_ICE:
        p = Sp(FX_MOTE8, v->pal, a->x + TO_FX((Random() % 17) - 8), CenterY(a) + TO_FX((Random() % 13) - 6), 14);
        FadeOut(p);
        break;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// traveling waves (Surf, sound moves, Heat Wave)
// ---------------------------------------------------------------------------
bool8 RtVfx_WaveTick(struct RtProj *p)
{
    sPrio = 4;
    const struct RtVfxDef *v = V(p->move);
    struct RtPart *pt;
    s32 k;
    u8 perp = p->angle + 64;
    switch (v->style)
    {
    case VS_SURF:
        if ((p->age & 1) == 0)
        {
            k = (Random() % (p->len * 2 + 1)) - p->len;
            pt = Fly(FX_FOAM8, FXP_E_FOAM, p->x + RtCos(perp) * k, p->y + RtSin(perp) * k - TO_FX(10), 192 + (s8)(Random() % 41 - 20), 300, 16, FALSE);
            Accel(pt, 0, 9);
        }
        return TRUE;
    case VS_HEAT:
        RtFx_Wave(2, 6);
        if ((p->age & 1) == 0)
        {
            k = (Random() % (p->len * 2 + 1)) - p->len;
            pt = Sp(FX_EMBER8, FXP_E_HOT, p->x + RtCos(perp) * k, p->y + RtSin(perp) * k, 18);
            Fade(pt, FXP_E_FIRE, 10, 0xFF, 0);
            Accel(pt, 0, -3);
        }
        return TRUE;
    case VS_SOUND:
        return FALSE;
    }
    return FALSE;
}

void RtVfx_StatusTick(struct RtActor *a)
{
    (void)a;
}
