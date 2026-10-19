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

extern u8 gRtFxHeavy;
static EWRAM_DATA u8 sThin = 0;

static struct RtPart *Sp(u8 shape, u8 pal, s32 x, s32 y, u8 life)
{
    struct RtPart *p;
    // the frame is running long: keep only every other cosmetic particle until it recovers
    if (gRtFxHeavy && ((++sThin) & 1))
        return NULL;
    p = RtFx_Spawn(shape, pal, x, y, life);
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
    if (bucket == 1)
        bucket = 5; // the bolt is the move: it gets its scanline time before the particles
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
// afterimages (Agility, Double Team, Quick Attack...)
// ---------------------------------------------------------------------------
#define MAX_GHOSTS 8
struct RtGhost
{
    u8 actor, life, maxLife, mode;
    s32 x, y;    // world position (mode 0) or offset in px (mode 1, orbiting copies)
};
static EWRAM_DATA struct RtGhost sGhost[MAX_GHOSTS] = {0};

void RtVfx_ResetGhosts(void)
{
    memset(sGhost, 0, sizeof(sGhost));
}

static void AddGhost(struct RtActor *a, u8 mode, s32 x, s32 y, u8 life)
{
    s32 i, pick = 0;
    for (i = 0; i < MAX_GHOSTS; i++)
    {
        if (sGhost[i].life == 0)
        {
            pick = i;
            break;
        }
        if (sGhost[i].life < sGhost[pick].life)
            pick = i;
    }
    sGhost[pick].actor = a->id;
    sGhost[pick].mode = mode;
    sGhost[pick].x = x;
    sGhost[pick].y = y;
    sGhost[pick].life = life;
    sGhost[pick].maxLife = life;
}

// n copies spread around the actor that drift outward and fade (Double Team) or a trail of the last positions
void RtVfx_Ghosts(struct RtActor *a, u8 n, u8 life)
{
    u8 k;
    for (k = 0; k < n; k++)
        AddGhost(a, 1, k, 0, life + k * 2);
}

void RtVfx_DrawGhosts(void)
{
    s32 i;
    for (i = 0; i < MAX_GHOSTS; i++)
    {
        struct RtGhost *g = &sGhost[i];
        struct RtActor *a;
        if (g->life == 0)
            continue;
        g->life--;
        a = &gRt->actors[g->actor];
        if (!a->active || a->state == AST_GONE)
        {
            g->life = 0;
            continue;
        }
        if (g->mode == 0)
        {
            if (g->life & 1)
                RtActor_DrawGhost(a, FROM_FX(g->x - a->x), FROM_FX(g->y - a->y));
        }
        else
        {
            // copies sliding out to both sides
            u8 t = g->maxLife - g->life;
            s32 d = 6 + t;
            s32 side = (g->x & 1) ? -1 : 1;
            s32 row = (g->x >> 1) * 6 - 3;
            if (d > 26) d = 26;
            if ((g->life & 1) || g->life > g->maxLife / 2)
                RtActor_DrawGhost(a, side * d, row);
        }
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
        ImpactGlow(FXP_E_HOT, x, y, 6, 1, 10);
        for (k = 0; k < 3; k++)
        {
            p = Sp(FX_FIRE16, FXP_E_FIRE, x + TO_FX((k - 1) * 7), y - TO_FX(2 + (k & 1) * 4), 16);
            if (p) p->vy = -100;
            Fade(p, FXP_E_SMOKE, 3, 0xFF, 0);
        }
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
        ImpactGlow(FXP_E_ELEC, x, y, 6, 1, 8);
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
        ImpactGlow(FXP_E_GRASS, x, y, 5, 1, 8);
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
        Scale(p, 8, 1);
        p = Sp(FX_PSYWAVE32, FXP_E_PINK, x, y, 18);
        Scale(p, 4, 1);
        for (k = 0; k < 5; k++)
            Fly(FX_SPARKLE8, FXP_E_PINK, x, y, Random(), 260, 16, FALSE);
        break;
    case VI_GHOST:
        ImpactGlow(FXP_E_GHOST, x, y, 7, 1, 14);
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
        // white comic burst; fighting moves get a hot red-orange shock ring around it
        p = Sp(FX_BOOM32, FXP_E_WHITE, x, y, 11);
        Scale(p, heavy ? 20 : 15, 1);
        if (vi == VI_FIGHT)
            ImpactRing(FXP_E_FIGHT, x, y, 6, 3, 10);
        for (k = 0; k < 6; k++)
        {
            p = Fly(FX_SPEED16, FXP_E_WHITE, x, y, k * 43 + (Random() & 15), 520, 9, FALSE);
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
        ImpactGlow(FXP_E_DRAGON, x, y, 6, 1, 10);
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

// ---------------------------------------------------------------------------
// hit signatures: the move's own motif stamped on the target when it connects, held long enough to read
// ---------------------------------------------------------------------------
static EWRAM_DATA u8 sHitAtk = 0; // attacker / target of the hit being drawn (0xFF = unknown)
static EWRAM_DATA u8 sHitTgt = 0;

void RtVfx_SetHit(u8 atk, u8 tgt)
{
    sHitAtk = atk;
    sHitTgt = tgt;
}

static struct RtActor *ActorAt(s32 x, s32 y, u8 notId)
{
    s32 i, best = 0x7FFFFFFF;
    struct RtActor *r = NULL;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 d;
        if (!t->active || t->state == AST_GONE || t->id == notId)
            continue;
        d = RtDist(FROM_FX(t->x - x), FROM_FX(t->y - TO_FX(t->height / 2) - y));
        if (d < best)
        {
            best = d;
            r = t;
        }
    }
    return r;
}

static struct RtActor *HitTarget(s32 x, s32 y)
{
    if (sHitTgt < RT_MAX_ACTORS && gRt->actors[sHitTgt].active)
        return &gRt->actors[sHitTgt];
    return ActorAt(x, y, 0xFF);
}

static struct RtActor *HitAttacker(s32 x, s32 y)
{
    struct RtActor *t;
    if (sHitAtk < RT_MAX_ACTORS && gRt->actors[sHitAtk].active)
        return &gRt->actors[sHitAtk];
    t = HitTarget(x, y);
    return ActorAt(x, y, t ? t->id : 0xFF);
}

// solid objects (fists, hands) read better with an outlined palette than with a glowing energy ramp
static u8 SolidPal(u8 pal)
{
    switch (pal)
    {
    case FXP_E_WHITE: case FXP_E_FIGHT: case FXP_E_HOT: return FXP_NORMAL;
    case FXP_E_FIRE: return FXP_FIRE;
    case FXP_E_ICE: return FXP_ICE;
    case FXP_E_ELEC: return FXP_ELECTRIC;
    case FXP_E_STEEL: return FXP_STEEL;
    case FXP_E_SKY: return FXP_FLYING;
    case FXP_E_GHOST: return FXP_GHOST;
    case FXP_E_DARK: return FXP_DARK;
    case FXP_E_GOLD: return FXP_GOLD;
    }
    return pal;
}

// a held motif: fixed frame or animated, faded out over its last third
static struct RtPart *Mark(u8 shape, u8 pal, s32 x, s32 y, u8 life, u8 scale, u8 angle, u8 rate)
{
    struct RtPart *p = Sp(shape, pal, x, y, life);;
    if (p)
    {
        p->angle = angle;
        p->frameRate = rate;
        p->frame = 0;
        RtFx_X(p)->flags |= PX_FADE | PX_HOLD;
    }
    Scale(p, scale, 0);
    return p;
}

// the white comic hit star every solid contact gets
static void HitStar(s32 x, s32 y, u8 size)
{
    struct RtPart *p = Sp(FX_FLARE32, FXP_E_WHITE, x, y, 10);
    Scale(p, size, -1);
    Spin(p, 7);
    p = Sp(FX_GLOW32, FXP_E_WHITE, x, y, 6);
    Scale(p, size / 2, 1);
    Blend(p);
}

static void SpeedBurst(s32 x, s32 y, u8 n, u8 pal, u8 ang0)
{
    u8 k;
    for (k = 0; k < n; k++)
    {
        struct RtPart *p = Fly(FX_SPEED16, pal, x, y, ang0 + k * (256 / n) + (Random() & 15), 560, 9, FALSE);
        Stretch(p);
    }
}

// something glued to an actor (coils, orbiting dizzy stars)
static struct RtPart *Glue(struct RtActor *t, u8 shape, u8 pal, s8 dx, u8 h, u8 life)
{
    struct RtPart *p;
    if (!t)
        return NULL;
    p = Sp(shape, pal, t->x + TO_FX(dx), t->y - TO_FX(h), life);
    if (p)
    {
        struct RtPartX *x = RtFx_X(p);
        x->flags |= PX_FOLLOW;
        x->owner = t->id;
        x->orbitAng = (u8)dx;
        x->orbit = h;
    }
    return p;
}

static void OrbitAround(struct RtActor *t, u8 shape, u8 pal, u8 n, u8 radius, s8 speed, u8 life, u8 h)
{
    u8 k;
    if (!t)
        return;
    for (k = 0; k < n; k++)
    {
        struct RtPart *p = Sp(shape, pal, t->x, t->y - TO_FX(h), life);
        if (p)
        {
            struct RtPartX *x = RtFx_X(p);
            x->flags |= PX_ORBIT;
            x->owner = t->id;
            x->orbit = radius;
            x->orbitAng = k * 256 / n;
            x->ay = speed;
            if (gRtFxShapes[shape].frames > 1)
                p->frameRate = 3;
        }
    }
}

static void Coils(struct RtActor *t, u8 pal)
{
    u8 k, h;
    if (!t)
        return;
    h = t->height;
    for (k = 0; k < 3; k++)
    {
        struct RtPart *p = Glue(t, FX_COIL32, pal, 0, h / 5 + k * h / 4, 44 - k * 4);
        if (p)
        {
            p->frameRate = 5; // loose / squeezed
            RtFx_X(p)->flags |= PX_FADE;
            p->flags |= PF_SCALE;
            p->scale = (u8)(14 + (t->radius > 12 ? 4 : 0) - k);
            p->dscale = 0;
        }
    }
    RtFx_Shake(1, 6);
}

static void Hearts(s32 x, s32 y, u8 n)
{
    u8 k;
    for (k = 0; k < n; k++)
    {
        struct RtPart *p = Fly(FX_HEART16, FXP_HEART, x, y, 160 + k * (96 / (n ? n : 1)) + (Random() & 15), 220, 30, FALSE);
        Drag(p, 14);
        Accel(p, 0, -2);
        FadeOut(p);
    }
}

static void Debris(s32 x, s32 y, u8 shape, u8 pal, u8 n)
{
    u8 k;
    for (k = 0; k < n; k++)
    {
        struct RtPart *p = Fly(shape, pal, x, y, 128 + 24 + Random() % 80 + 32, 420 + (Random() & 255), 24, FALSE);
        Accel(p, 0, 12);
        Drag(p, 15);
        Spin(p, (Random() & 1) ? 12 : -12);
    }
}

// returns TRUE when the signature replaces the generic emblem of the style
static bool8 SigHit(u16 move, const struct RtVfxDef *v, s32 x, s32 y)
{
    struct RtPart *p;
    struct RtActor *t, *a;
    s32 k;
    u8 ang = Random();
    switch (move)
    {
    // ---- open hand
    case MOVE_SLAM:
        Mark(FX_ARC32, FXP_E_WHITE, x, y - TO_FX(6), 22, 32, 64 + 32, 5);
        p = Sp(FX_BOOM32, FXP_E_WHITE, x, y + TO_FX(4), 14);
        Scale(p, 22, 1);
        ImpactRing(FXP_E_DUST, x, y + TO_FX(10), 8, 3, 12);
        RtFx_Shake(2, 8);
        return TRUE;
    case MOVE_POUND:
    case MOVE_DOUBLE_SLAP:
    case MOVE_FAKE_OUT:
    case MOVE_ARM_THRUST:
    case MOVE_SMELLING_SALTS:
        {
            s32 oy = (move == MOVE_DOUBLE_SLAP || move == MOVE_ARM_THRUST) ? TO_FX((Random() % 13) - 6) : 0;
            a = HitAttacker(x, y);
            p = Mark(FX_PALM16, move == MOVE_ARM_THRUST ? FXP_FIGHTING : FXP_GOLD, x - TO_FX(8), y + oy, 24, move == MOVE_SLAM ? 30 : 24, 0, 0);
            if (p)
            {
                p->vx = 170;
                if (a && a->x > x) { p->flags |= PF_HFLIP; p->x += TO_FX(16); p->vx = -170; }
            }
            Drag(p, 12);
            HitStar(x + TO_FX(4), y + oy, move == MOVE_SLAM ? 26 : 20);
            if (move == MOVE_SMELLING_SALTS)
                for (k = 0; k < 4; k++)
                {
                    p = Sp(FX_CRACKLE16, (k & 1) ? FXP_E_BOLT : FXP_E_ELEC, x + TO_FX((Random() % 25) - 12), y + TO_FX((Random() % 21) - 10), 12 + k * 3);
                    if (p) p->frameRate = 2;
                }
            if (move == MOVE_FAKE_OUT)
                RtFx_Flash(0, 3);
        }
        return TRUE;
    // ---- chops
    case MOVE_KARATE_CHOP:
    case MOVE_BRICK_BREAK:
    case MOVE_CROSS_CHOP:
        Mark(FX_CHOP32, FXP_E_WHITE, x, y, 24, 32, 40, 4);
        p = Sp(FX_GLOW32, FXP_E_FIGHT, x, y, 8);
        Scale(p, 10, 1);
        Blend(p);
        if (move == MOVE_CROSS_CHOP)
            Mark(FX_CHOP32, FXP_E_WHITE, x, y, 24, 26, 216, 4);
        if (move == MOVE_BRICK_BREAK)
        {
            Debris(x, y, FX_DEBRIS8, FXP_E_FIGHT, 6);
            Debris(x, y, FX_DEBRIS8, FXP_E_ROCK, 3);
        }
        HitStar(x, y, 18);
        return TRUE;
    // ---- blades
    case MOVE_CUT:
    case MOVE_SLASH:
    case MOVE_FALSE_SWIPE:
    case MOVE_AERIAL_ACE:
    case MOVE_FURY_CUTTER:
    case MOVE_STEEL_WING:
    case MOVE_WING_ATTACK:
    case MOVE_POISON_TAIL:
    case MOVE_IRON_TAIL:
        {
            u8 big = (move == MOVE_IRON_TAIL || move == MOVE_SLASH) ? 32 : 28;
            Mark(FX_ARC32, v->pal2, x, y, 26, big + 4, ang, 6);
            p = Mark(FX_ARC32, v->pal, x, y, 26, big, ang, 6);
            if (move == MOVE_SLASH || move == MOVE_FURY_CUTTER || move == MOVE_AERIAL_ACE)
                Mark(FX_CHOP32, FXP_E_WHITE, x, y, 22, 24, ang + 96, 4);
            if (move == MOVE_AERIAL_ACE)
                SpeedBurst(x, y, 6, FXP_E_WHITE, ang);
            if (move == MOVE_STEEL_WING || move == MOVE_WING_ATTACK)
                for (k = 0; k < 4; k++)
                {
                    p = Fly(FX_FEATHER16, move == MOVE_STEEL_WING ? FXP_STEEL : FXP_FLYING, x, y, Random(), 240, 26, FALSE);
                    Accel(p, 0, 2);
                    Spin(p, 5);
                }
            if (move == MOVE_IRON_TAIL || move == MOVE_STEEL_WING)
                for (k = 0; k < 6; k++)
                {
                    p = Fly(FX_SPARKLE8, (k & 1) ? FXP_E_WHITE : FXP_E_GOLD, x, y, 128 + Random() % 128 + 64, 450 + (Random() & 255), 16, FALSE);
                    Accel(p, 0, 9);
                    Stretch(p);
                }
            if (move == MOVE_POISON_TAIL)
                Debris(x, y, FX_GOO16, FXP_E_POISON, 3);
            HitStar(x, y, 16);
        }
        return TRUE;
    // ---- coils
    case MOVE_BIND:
    case MOVE_WRAP:
    case MOVE_CONSTRICT:
        Coils(HitTarget(x, y), move == MOVE_BIND ? FXP_E_SAND : (move == MOVE_WRAP ? FXP_E_GOLD : FXP_E_PINK));
        HitStar(x, y, 14);
        return TRUE;
    case MOVE_LICK:
        Mark(FX_ARC32, FXP_E_PINK, x, y + TO_FX(4), 26, 28, 192, 6);
        Debris(x, y, FX_GOO16, FXP_E_PINK, 3);
        HitStar(x, y, 14);
        return TRUE;
    case MOVE_ASTONISH:
        p = Mark(FX_WISP16, FXP_E_GHOST, x, y, 24, 34, 0, 3);
        Blend(p);
        t = HitTarget(x, y);
        p = Glue(t, FX_EXCLAIM8, FXP_GOLD, 0, t ? t->height + 10 : 30, 30);
        Scale(p, 26, 0);
        RtFx_Shake(1, 8);
        RtFx_Dim(4, 6);
        return TRUE;
    case MOVE_BONE_CLUB:
    case MOVE_BONE_RUSH:
    case MOVE_BONEMERANG:
        p = Mark(FX_BONE16, FXP_WHITE, x - TO_FX(6), y - TO_FX(6), 24, 28, 192, 0);
        Spin(p, 18);
        if (p) { p->vx = 140; p->vy = 140; }
        p = Sp(FX_BOOM32, FXP_E_DUST, x, y, 10);
        Scale(p, 16, 1);
        HitStar(x, y, 16);
        return TRUE;
    case MOVE_NEEDLE_ARM:
        for (k = 0; k < 8; k++)
        {
            p = Fly(FX_NEEDLE8, FXP_GRASS, x, y, k * 32 + (Random() & 7), 380, 14, FALSE);
            Stretch(p);
        }
        p = Sp(FX_BOOM32, FXP_E_GRASS, x, y, 10);
        Scale(p, 16, 1);
        return TRUE;
    // ---- kicks
    case MOVE_LOW_KICK:
        Mark(FX_ARC32, FXP_E_WHITE, x, y + TO_FX(10), 24, 30, 0, 5);
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_PUFF16, FXP_E_DUST, x, y + TO_FX(12), (k & 1) ? 0 : 128, 160 + k * 30, 22, TRUE);
            Blend(p);
        }
        HitStar(x, y + TO_FX(8), 18);
        return TRUE;
    case MOVE_ROLLING_KICK:
    case MOVE_BLAZE_KICK:
    case MOVE_MEGA_KICK:
    case MOVE_DOUBLE_KICK:
    case MOVE_TRIPLE_KICK:
        p = Mark(FX_ARC32, move == MOVE_BLAZE_KICK ? FXP_E_FIRE : FXP_E_WHITE, x, y, 22, move == MOVE_MEGA_KICK ? 32 : 26, ang, 4);
        Spin(p, move == MOVE_ROLLING_KICK ? 24 : 6);
        p = Sp(FX_BOOM32, move == MOVE_BLAZE_KICK ? FXP_E_HOT : FXP_E_WHITE, x, y, 12);
        Scale(p, move == MOVE_MEGA_KICK ? 24 : 16, 1);
        HitStar(x, y, move == MOVE_MEGA_KICK ? 26 : 18);
        return TRUE;
    case MOVE_STOMP:
        ImpactRing(FXP_E_DUST, x, y + TO_FX(10), 8, 3, 12);
        p = Sp(FX_BOOM32, FXP_E_WHITE, x, y, 12);
        Scale(p, 20, 1);
        Debris(x, y + TO_FX(8), FX_DEBRIS8, FXP_E_ROCK, 4);
        RtFx_Shake(2, 6);
        return TRUE;
    // ---- fists
    case MOVE_THUNDER_PUNCH:
    case MOVE_FIRE_PUNCH:
    case MOVE_ICE_PUNCH:
    case MOVE_MACH_PUNCH:
    case MOVE_COMET_PUNCH:
    case MOVE_MEGA_PUNCH:
    case MOVE_DYNAMIC_PUNCH:
    case MOVE_FOCUS_PUNCH:
    case MOVE_DIZZY_PUNCH:
    case MOVE_ROCK_SMASH:
    case MOVE_SKY_UPPERCUT:
    case MOVE_METEOR_MASH:
    case MOVE_KNOCK_OFF:
    case MOVE_BEAT_UP:
    case MOVE_SUPERPOWER:
        {
            bool8 big = move == MOVE_MEGA_PUNCH || move == MOVE_DYNAMIC_PUNCH || move == MOVE_FOCUS_PUNCH || move == MOVE_METEOR_MASH || move == MOVE_SUPERPOWER;
            u8 fang = move == MOVE_SKY_UPPERCUT ? 192 : 0;
            a = HitAttacker(x, y);
            if (a && a->x > x && fang == 0)
                fang = 128;
            p = Mark(FX_FIST32, SolidPal(v->pal), x - RtCos(fang) * 10, y - RtSin(fang) * 10, 24, big ? 20 : 17, fang, 0);
            if (p) { p->vx = RtCos(fang) * 3 / 4; p->vy = RtSin(fang) * 3 / 4; }
            Drag(p, 12);
            if (v->pal == FXP_E_FIGHT)
                ImpactRing(FXP_E_FIGHT, x, y, 6, 3, 12);
            p = Sp(FX_BOOM32, v->pal == FXP_E_FIGHT ? FXP_E_WHITE : v->pal2, x, y, 12);
            Scale(p, big ? 24 : 16, 1);
            HitStar(x, y, big ? 28 : 18);
            switch (move)
            {
            case MOVE_THUNDER_PUNCH:
                for (k = 0; k < 6; k++)
                {
                    p = Sp(FX_CRACKLE16, (k & 1) ? FXP_E_BOLT : FXP_E_ELEC, x + TO_FX((Random() % 33) - 16), y + TO_FX((Random() % 25) - 12), 10 + k * 3);
                    if (p) p->frameRate = 2;
                }
                for (k = 0; k < 4; k++)
                {
                    p = Fly(FX_LBOLT16, FXP_E_ELEC, x, y, k * 64 + 32 + (Random() & 15), 300, 10, FALSE);
                    Stretch(p);
                }
                break;
            case MOVE_FIRE_PUNCH:
                for (k = 0; k < 6; k++)
                {
                    p = Sp(FX_FIRE16, FXP_E_FIRE, x + TO_FX((k - 3) * 5), y + TO_FX(4 - (k & 1) * 6), 20);
                    if (p) p->vy = -120 - (Random() & 63);
                    Fade(p, FXP_E_SMOKE, 4, 0xFF, 0);
                }
                break;
            case MOVE_ICE_PUNCH:
                p = Mark(FX_ICEBLOCK32, FXP_ICE, x, y, 32, 18, 0, 0);
                Blend(p);
                break;
            case MOVE_MACH_PUNCH:
            case MOVE_COMET_PUNCH:
                SpeedBurst(x, y, 6, FXP_E_WHITE, 0);
                if (move == MOVE_COMET_PUNCH)
                    for (k = 0; k < 3; k++)
                        Fly(FX_STAR16B, FXP_E_GOLD, x, y, Random(), 260, 14, FALSE);
                break;
            case MOVE_DIZZY_PUNCH:
            case MOVE_DYNAMIC_PUNCH:
                t = HitTarget(x, y);
                OrbitAround(t, FX_DIZZY8, FXP_GOLD, 3, 12, 10, 40, t ? t->height + 4 : 20);
                if (move == MOVE_DYNAMIC_PUNCH)
                    ImpactRing(FXP_E_FIGHT, x, y, 8, 3, 14);
                break;
            case MOVE_ROCK_SMASH:
                Debris(x, y, FX_DEBRIS8, FXP_E_ROCK, 6);
                Debris(x, y, FX_ROCK16B, FXP_E_ROCK, 2);
                break;
            case MOVE_SKY_UPPERCUT:
                for (k = 0; k < 5; k++)
                {
                    p = Sp(FX_SPEED16, FXP_E_SKY, x + TO_FX((k - 2) * 6), y + TO_FX(6), 12);
                    if (p) { p->angle = 192; p->vy = -500; }
                    Stretch(p);
                }
                break;
            case MOVE_METEOR_MASH:
                for (k = 0; k < 5; k++)
                    Fly(FX_STAR16B, FXP_E_GOLD, x, y, k * 51, 360, 16, FALSE);
                RtFx_Shake(2, 8);
                break;
            case MOVE_FOCUS_PUNCH:
            case MOVE_MEGA_PUNCH:
            case MOVE_SUPERPOWER:
                ImpactRing(FXP_E_WHITE, x, y, 8, 3, 14);
                if (move != MOVE_MEGA_PUNCH)
                    ImpactRing(FXP_E_FIGHT, x, y, 4, 2, 18);
                RtFx_Shake(2, 10);
                break;
            case MOVE_KNOCK_OFF:
                p = Sp(FX_ORB8, FXP_GOLD, x, y - TO_FX(8), 28);
                if (p) { p->vx = (a && a->x > x) ? -260 : 260; p->vy = -420; }
                Accel(p, 0, 9);
                Drag(p, 16);
                break;
            }
        }
        return TRUE;
    // ---- horns / beaks
    case MOVE_HORN_ATTACK:
    case MOVE_FURY_ATTACK:
    case MOVE_PECK:
    case MOVE_DRILL_PECK:
    case MOVE_HORN_DRILL:
        {
            u8 shp = (move == MOVE_DRILL_PECK || move == MOVE_HORN_DRILL) ? FX_DRILL16 : FX_HORN16;
            u8 fang = 0;
            a = HitAttacker(x, y);
            if (a && a->x > x) fang = 128;
            p = Mark(shp, (move == MOVE_PECK || move == MOVE_DRILL_PECK) ? FXP_GOLD : FXP_WHITE, x - RtCos(fang) * 14, y, 22, 28, fang, shp == FX_DRILL16 ? 2 : 0);
            if (p) p->vx = RtCos(fang) * 3 / 2;
            Drag(p, 12);
            HitStar(x, y, 18);
            SpeedBurst(x, y, 4, FXP_E_WHITE, fang + 96);
        }
        return TRUE;
    // ---- fangs
    case MOVE_HYPER_FANG:
    case MOVE_SUPER_FANG:
    case MOVE_CRUNCH:
        p = Mark(FX_JAWS32, FXP_E_WHITE, x, y - TO_FX(2), 28, move == MOVE_HYPER_FANG ? 28 : 26, 0, 6);
        if (move == MOVE_CRUNCH)
        {
            p = Sp(FX_CRACK32, FXP_E_DARK, x, y, 18);
            Scale(p, 14, 0);
        }
        HitStar(x, y, 16);
        return TRUE;
    case MOVE_CRABHAMMER:
        Mark(FX_JAWS32, FXP_E_WATER, x, y, 26, 28, 64, 6);
        p = Sp(FX_SPLASH32, FXP_E_WATER, x, y + TO_FX(4), 16);
        Scale(p, 20, 0);
        HitStar(x, y, 18);
        return FALSE; // plus the water burst
    // ---- grapples
    case MOVE_SEISMIC_TOSS:
    case MOVE_VITAL_THROW:
    case MOVE_SUBMISSION:
    case MOVE_STRENGTH:
        ImpactRing(FXP_E_DUST, x, y + TO_FX(12), 8, 3, 16);
        for (k = 0; k < 6; k++)
        {
            p = Fly(FX_PUFF16, FXP_E_DUST, x, y + TO_FX(12), k * 43, 200, 22, TRUE);
            Blend(p);
        }
        p = Sp(FX_BOOM32, FXP_E_WHITE, x, y, 12);
        Scale(p, 22, 1);
        RtFx_Shake(2, 10);
        return TRUE;
    case MOVE_RAGE:
    case MOVE_FRUSTRATION:
    case MOVE_REVENGE:
    case MOVE_REVERSAL:
        p = Sp(FX_BOOM32, FXP_E_WHITE, x, y, 12);
        Scale(p, move == MOVE_RAGE ? 16 : 22, 1);
        ImpactRing(FXP_E_RED, x, y, 6, 3, 12);
        HitStar(x, y, 22);
        SpeedBurst(x, y, 8, FXP_E_WHITE, ang);
        if (move != MOVE_REVERSAL && move != MOVE_RAGE)
        {
            a = (move == MOVE_RAGE) ? HitAttacker(x, y) : HitTarget(x, y);
            p = Glue(a ? a : HitTarget(x, y), FX_ANGER16, FXP_E_RED, 8, (a ? a->height : 20) + 2, 30);
            Scale(p, 22, 0);
            if (p) p->frameRate = 0;
        }
        else
            RtFx_Flash(0, 4);
        return TRUE;
    case MOVE_RETURN:
        p = Sp(FX_BOOM32, FXP_E_WHITE, x, y, 12);
        Scale(p, 20, 1);
        Hearts(x, y, 4);
        return TRUE;
    case MOVE_STRUGGLE:
        p = Sp(FX_BOOM32, FXP_E_WHITE, x, y, 10);
        Scale(p, 16, 1);
        for (k = 0; k < 3; k++)
        {
            p = Fly(FX_DROP8, FXP_WATER, x, y - TO_FX(10), 160 + k * 32, 300, 16, FALSE);
            Accel(p, 0, 10);
        }
        return TRUE;
    case MOVE_THIEF:
    case MOVE_COVET:
        a = HitAttacker(x, y);
        p = Sp(FX_BOOM32, move == MOVE_THIEF ? FXP_E_DARK : FXP_E_WHITE, x, y, 10);
        Scale(p, 16, 1);
        if (a)
            for (k = 0; k < 6; k++)
            {
                p = Fly(FX_SPARKLE8, FXP_E_GOLD, x, y, Random(), 200, 30, FALSE);
                if (p)
                {
                    RtFx_X(p)->flags |= PX_HOMEIN;
                    RtFx_X(p)->owner = a->id;
                }
            }
        return TRUE;
    case MOVE_WATERFALL:
        for (k = 0; k < 4; k++)
        {
            p = Sp(FX_JET16, (k & 1) ? FXP_E_FOAM : FXP_E_WATER, x + TO_FX((k - 2) * 5), y + TO_FX(8), 18);
            if (p) { p->angle = 192; p->vy = -360 - k * 40; }
            Scale(p, 18, 0);
        }
        return FALSE; // plus the water burst
    case MOVE_MEGAHORN:
        a = HitAttacker(x, y);
        p = Mark(FX_HORN16, FXP_E_BUG, x - TO_FX((a && a->x > x) ? -16 : 16), y, 24, 34, (a && a->x > x) ? 128 : 0, 0);
        if (p) p->vx = (a && a->x > x) ? -260 : 260;
        Drag(p, 12);
        HitStar(x, y, 26);
        return FALSE;
    case MOVE_RAPID_SPIN:
        p = Mark(FX_ARC32, FXP_E_WHITE, x, y, 22, 28, ang, 4);
        Spin(p, 30);
        HitStar(x, y, 16);
        return TRUE;
    case MOVE_EXTREME_SPEED:
    case MOVE_QUICK_ATTACK:
        SpeedBurst(x, y, 8, FXP_E_WHITE, ang);
        HitStar(x, y, 20);
        return FALSE;
    // ---- status motifs that land on the target
    case MOVE_DISABLE:
    case MOVE_IMPRISON:
        t = HitTarget(x, y);
        p = Glue(t, FX_CROSS16, FXP_BLOOD, 0, t ? t->height / 2 + 2 : 16, 34);
        Scale(p, 28, 0);
        ImpactRing(FXP_E_RED, x, y, 6, 2, 14);
        return TRUE;
    case MOVE_TAUNT:
    case MOVE_TORMENT:
        t = HitTarget(x, y);
        p = Glue(t, FX_ANGER16, FXP_E_RED, -6, t ? t->height + 4 : 24, 40);
        Scale(p, 28, 0);
        p = Glue(t, FX_ANGER16, FXP_E_RED, 10, t ? t->height - 4 : 16, 34);
        Scale(p, 20, 0);
        ImpactRing(FXP_E_RED, x, y, 6, 2, 14);
        if (move == MOVE_TORMENT)
        {
            p = Glue(t, FX_SKULL16, FXP_SHADOW, 0, t ? t->height + 14 : 30, 30);
            Scale(p, 18, 0);
        }
        return TRUE;
    case MOVE_LOCK_ON:
    case MOVE_MIND_READER:
        t = HitTarget(x, y);
        p = Glue(t, FX_TARGET32, move == MOVE_LOCK_ON ? FXP_BLOOD : FXP_PSYCHIC, 0, t ? t->height / 2 : 16, 34);
        Scale(p, 34, -1);
        if (p) { p->frameRate = 0; RtFx_X(p)->spin = 3; }
        return TRUE;
    case MOVE_FORESIGHT:
    case MOVE_ODOR_SLEUTH:
    case MOVE_MEAN_LOOK:
    case MOVE_BLOCK:
        t = HitTarget(x, y);
        if (move == MOVE_BLOCK)
        {
            p = Glue(t, FX_CROSS16, FXP_WHITE, 0, t ? t->height / 2 : 16, 34);
            Scale(p, 30, 0);
            p = Sp(FX_HEX32, FXP_E_WHITE, x, y, 24);
            Scale(p, 20, 0);
            Blend(p);
            return TRUE;
        }
        p = Glue(t, FX_EYE16B, move == MOVE_MEAN_LOOK ? FXP_E_RED : FXP_E_GOLD, 0, t ? t->height + 10 : 28, 36);
        Scale(p, 26, 0);
        if (p) { p->frameRate = 0; p->frame = move == MOVE_MEAN_LOOK; RtFx_X(p)->flags |= PX_FADE; }
        if (move == MOVE_ODOR_SLEUTH)
            for (k = 0; k < 3; k++)
            {
                p = Sp(FX_PSYWAVE32, FXP_E_GOLD, x, y, 12 + k * 5);
                Scale(p, 4 + k * 3, 2);
                Blend(p);
            }
        else
            ImpactRing(move == MOVE_MEAN_LOOK ? FXP_E_DARK : FXP_E_GOLD, x, y, 6, 3, 14);
        if (move == MOVE_MEAN_LOOK)
            RtFx_Dim(5, 16);
        return TRUE;
    case MOVE_YAWN:
        t = HitTarget(x, y);
        for (k = 0; k < 3; k++)
        {
            p = Sp(FX_Z8, FXP_SLEEP, x + TO_FX(k * 6 - 4), y - TO_FX(4 + k * 4), 30 + k * 6);
            if (p) { p->vy = -70; p->vx = 40; }
            Scale(p, 20 + k * 4, 0);
            FadeOut(p);
        }
        return TRUE;
    case MOVE_SPITE:
    case MOVE_NIGHTMARE:
    case MOVE_CURSE:
        t = HitTarget(x, y);
        OrbitAround(t, FX_WISP16, FXP_E_GHOST, 3, 16, 7, 34, t ? t->height / 2 : 16);
        if (move == MOVE_NIGHTMARE)
        {
            p = Glue(t, FX_SKULL16, FXP_SHADOW, 0, t ? t->height + 12 : 28, 34);
            Scale(p, 20, 0);
        }
        RtFx_Dim(4, 12);
        return TRUE;
    case MOVE_ENCORE:
        t = HitTarget(x, y);
        OrbitAround(t, FX_NOTE16, FXP_GOLD, 3, 18, 8, 36, t ? t->height : 20);
        OrbitAround(t, FX_STAR16B, FXP_E_GOLD, 2, 12, -9, 30, t ? t->height + 8 : 26);
        return TRUE;
    case MOVE_TRICK:
    case MOVE_SKILL_SWAP:
    case MOVE_ROLE_PLAY:
        t = HitTarget(x, y);
        OrbitAround(t, FX_ORB16, move == MOVE_ROLE_PLAY ? FXP_E_PINK : FXP_E_GOLD, 1, 14, 12, 30, t ? t->height / 2 : 16);
        OrbitAround(t, FX_ORB16, FXP_E_PSY, 1, 14, -12, 30, t ? t->height / 2 : 16);
        ImpactGlow(FXP_E_PSY, x, y, 6, 1, 12);
        return TRUE;
    case MOVE_SWEET_KISS:
    case MOVE_LOVELY_KISS:
    case MOVE_ATTRACT:
    case MOVE_CHARM:
    case MOVE_FLATTER:
        Hearts(x, y, move == MOVE_ATTRACT ? 6 : 4);
        p = Sp(FX_HEART16, FXP_HEART, x, y, 20);
        Scale(p, 28, 0);
        FadeOut(p);
        return TRUE;
    case MOVE_SPIDER_WEB:
    case MOVE_STRING_SHOT:
        t = HitTarget(x, y);
        p = Glue(t, FX_WEB32, FXP_E_WHITE, 0, t ? t->height / 2 : 16, 44);
        Scale(p, move == MOVE_SPIDER_WEB ? 26 : 20, 0);
        if (p) RtFx_X(p)->flags |= PX_FADE;
        return TRUE;
    case MOVE_PAY_DAY:
        for (k = 0; k < 7; k++)
        {
            p = Fly(FX_COIN8, FXP_GOLD, x, y, 128 + 32 + Random() % 64 + 32, 380 + (Random() & 127), 26, FALSE);
            Accel(p, 0, 10);
            if (p) p->frameRate = 2;
        }
        HitStar(x, y, 14);
        return TRUE;
    case MOVE_TRI_ATTACK:
        {
            static const u8 sVi[3] = {VI_FIRE, VI_ICE, VI_ELEC};
            for (k = 0; k < 3; k++)
            {
                u8 a3 = 192 + k * 85;
                DoImpact(sVi[k], 0, 0, x + RtCos(a3) * 10, y + RtSin(a3) * 10, FALSE);
            }
        }
        return TRUE;
    case MOVE_EGG_BOMB:
    case MOVE_BARRAGE:
        Debris(x, y, FX_DEBRIS8, FXP_E_WHITE, 6);
        p = Sp(FX_BOOM32, FXP_E_HOT, x, y, 12);
        Scale(p, move == MOVE_EGG_BOMB ? 24 : 16, 1);
        HitStar(x, y, 18);
        return TRUE;
    case MOVE_PRESENT:
        for (k = 0; k < 6; k++)
            Fly(FX_STAR16B, (k & 1) ? FXP_E_PINK : FXP_E_GOLD, x, y, k * 43, 300, 18, FALSE);
        p = Sp(FX_BOOM32, FXP_E_GOLD, x, y, 12);
        Scale(p, 18, 1);
        return TRUE;
    case MOVE_CONFUSION:
    case MOVE_EXTRASENSORY:
        for (k = 0; k < 3; k++)
        {
            p = Sp(FX_PSYWAVE32, (k & 1) ? FXP_E_PINK : FXP_E_PSY, x, y, 14 + k * 5);
            Scale(p, 4 + k * 3, 1);
        }
        if (move == MOVE_EXTRASENSORY)
        {
            p = Sp(FX_EYE16B, FXP_E_PSY, x, y - TO_FX(16), 24);
            if (p) { p->frameRate = 0; p->frame = 1; }
            Scale(p, 22, 0);
            FadeOut(p);
        }
        RtFx_Wave(2, 16);
        return FALSE;
    case MOVE_ABSORB:
    case MOVE_MEGA_DRAIN:
    case MOVE_GIGA_DRAIN:
    case MOVE_LEECH_LIFE:
    case MOVE_DREAM_EATER:
        a = HitAttacker(x, y);
        if (a)
            for (k = 0; k < (move == MOVE_GIGA_DRAIN || move == MOVE_DREAM_EATER ? 8 : 5); k++)
            {
                p = Fly((k & 1) ? FX_SPARKLE8 : FX_MOTE8, move == MOVE_DREAM_EATER ? ((k & 1) ? FXP_E_PSY : FXP_E_PINK) : ((k & 1) ? FXP_E_HEAL : FXP_E_GRASS), x, y, Random(), 260 + (Random() & 127), 40, FALSE);
                if (p)
                {
                    RtFx_X(p)->flags |= PX_HOMEIN;
                    RtFx_X(p)->owner = a->id;
                    p->prio = 5;
                }
            }
        return FALSE;
    case MOVE_PSYCHO_BOOST:
        for (k = 0; k < 4; k++)
        {
            p = Sp(FX_PSYWAVE32, (k & 1) ? FXP_E_PINK : FXP_E_PSY, x, y, 16 + k * 5);
            Scale(p, 6 + k * 5, 3);
            Blend(p);
        }
        RtFx_Flash(0, 6);
        RtFx_Shake(2, 12);
        return FALSE;
    case MOVE_PAIN_SPLIT:
        t = HitTarget(x, y);
        OrbitAround(t, FX_GLOW16, FXP_E_RED, 2, 12, 10, 26, t ? t->height / 2 : 16);
        ImpactRing(FXP_E_RED, x, y, 6, 2, 14);
        return TRUE;
    case MOVE_SWAGGER:
        t = HitTarget(x, y);
        p = Glue(t, FX_ANGER16, FXP_E_RED, 0, t ? t->height + 4 : 24, 30);
        Scale(p, 20, 0);
        OrbitAround(t, FX_DIZZY8, FXP_GOLD, 3, 12, 10, 36, t ? t->height + 4 : 20);
        return TRUE;
    }
    return FALSE;
}

static bool8 HitEmblem(u16 move, const struct RtVfxDef *v, s32 x, s32 y)
{
    struct RtPart *p;
    s32 k;
    u8 ang = Random();
    if (SigHit(move, v, x, y))
        return TRUE;
    switch (v->style)
    {
    case VS_SLASH:
        Mark(FX_ARC32, v->pal2, x, y, 26, 30, ang, 6);
        Mark(FX_ARC32, v->pal, x, y, 26, 26, ang, 6);
        HitStar(x, y, 14);
        break;
    case VS_CLAW:
        k = (Random() & 1) ? 0 : 224;
        Mark(FX_CLAW32, v->pal2, x, y, 28, 28, k, 6);
        Mark(FX_CLAW32, v->pal, x, y, 28, 24, k, 6);
        HitStar(x, y, 12);
        break;
    case VS_BITE:
        Mark(FX_JAWS32, v->pal, x, y - TO_FX(2), 28, 26, 0, 6);
        HitStar(x, y, 12);
        break;
    case VS_PUNCH:
        p = Mark(FX_FIST32, SolidPal(v->pal), x - TO_FX(6), y, 24, 18, 0, 0);
        if (p) p->vx = 150;
        Drag(p, 12);
        p = Sp(FX_BOOM32, v->pal2, x + TO_FX(4), y, 12);
        Scale(p, 16, 1);
        HitStar(x, y, 16);
        break;
    case VS_KICK:
        p = Sp(FX_BOOM32, v->pal, x, y, 14);
        Scale(p, (v->flags & VF_BIG) ? 22 : 18, 1);
        HitStar(x, y, (v->flags & VF_BIG) ? 26 : 18);
        break;
    case VS_HORN:
        p = Mark(FX_HORN16, v->pal, x - TO_FX(12), y, 22, 26, 0, 0);
        if (p) p->vx = 300;
        Drag(p, 12);
        HitStar(x, y, 16);
        break;
    case VS_DASH:
        p = Sp(FX_BOOM32, (v->pal == FXP_E_WHITE || v->pal == FXP_E_FIGHT) ? FXP_E_WHITE : v->pal, x, y, 14);
        Scale(p, (v->flags & VF_HEAVY) ? 22 : 17, 1);
        HitStar(x, y, (v->flags & VF_HEAVY) ? 26 : 18);
        if (v->flags & VF_HEAVY)
            ImpactRing(FXP_E_WHITE, x, y, 8, 3, 12);
        break;
    case VS_FIREBALL:
        if (move == MOVE_FIRE_BLAST)
        {
            // the five-armed blaze
            for (k = 0; k < 5; k++)
            {
                u8 a = 192 + k * 51;
                s32 r;
                for (r = 0; r < 3; r++)
                {
                    p = Sp(FX_FIRE16, r == 0 ? FXP_E_HOT : FXP_E_FIRE, x + RtCos(a) * (6 + r * 9), y + RtSin(a) * (6 + r * 9), 24 - r * 2);
                    if (p) { p->vx = RtCos(a) / 3; p->vy = RtSin(a) / 3; }
                    Fade(p, FXP_E_FIRE, 10, FXP_E_SMOKE, 4);
                }
            }
            RtFx_Shake(2, 10);
        }
        break;
    }
    return FALSE;
}

void RtVfx_Impact(u16 move, u8 style, u8 pal, s32 x, s32 y)
{
    const struct RtVfxDef *v = V(move);
    u8 vi = v->impact;
    sPrio = 1;
    if (move == MOVE_NONE || move >= MOVES_COUNT)
    {
        RtFx_Impact(style, pal, x, y);
        sHitAtk = sHitTgt = 0xFF;
        return;
    }
    if (vi == VI_DEFAULT)
        vi = TypeImpact(RTM(move).type);
    if (HitEmblem(move, v, x, y) && RTM(move).power == 0)
        ; // status moves with a motif show only that
    else if (RTM(move).power == 0 && vi == VI_NORMAL)
    {
        // plain status move: a soft ring of sparkles instead of a hit burst
        s32 k;
        ImpactRing(v->pal, x, y, 6, 2, 14);
        for (k = 0; k < 5; k++)
            Fly(FX_SPARKLE8, (k & 1) ? v->pal : v->pal2, x, y, k * 51 + (Random() & 15), 240, 16, FALSE);
    }
    else
        DoImpact(vi, v->pal, v->pal2, x, y, (v->flags & VF_HEAVY) != 0);
    sHitAtk = sHitTgt = 0xFF;
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
        RtFx_Dim(5, 20);
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
    case VS_METEOR:
        {
            s32 k;
            p = Sp(FX_ORB16, v->pal, a->x, CenterY(a), 26);
            if (p) p->vy = -700;
            Drag(p, 16);
            p = Sp(FX_GLOW32, v->pal, a->x, CenterY(a), 26);
            if (p) p->vy = -700;
            Drag(p, 16);
            Blend(p);
            for (k = 0; k < 3; k++)
            {
                p = Sp(FX_PSYWAVE32, v->pal2, a->x, CenterY(a), 14 + k * 5);
                Scale(p, 4 + k * 3, 2);
                Blend(p);
            }
        }
        break;
    case VS_LAND:
        {
            s32 k;
            ImpactRing(v->pal, a->x, a->y, 6, 2, 14);
            for (k = 0; k < 8; k++)
            {
                p = Fly(FX_PUFF16, v->pal2, a->x, a->y, k * 32, 380, 20, TRUE);
                Blend(p);
                Drag(p, 13);
            }
            if (move == MOVE_DIG || move == MOVE_DIVE)
                for (k = 0; k < 8; k++)
                {
                    p = Fly(move == MOVE_DIVE ? FX_FOAM8 : FX_DEBRIS8, move == MOVE_DIVE ? FXP_E_FOAM : FXP_E_MUD, a->x, a->y, 160 + Random() % 64, 500 + (Random() & 255), 24, FALSE);
                    Accel(p, 0, 12);
                }
        }
        break;
    case VS_WEATHER:
        {
            s32 k;
            switch (move)
            {
            case MOVE_SUNNY_DAY:
                RtFx_Flash(0, 10);
                // the sun itself, blazing over the arena, and its rays sweeping down
                p = Sp(FX_GLOW32, FXP_E_SOLAR, TO_FX(gRt->camX + 186), TO_FX(gRt->camY + 46), 60);
                Scale(p, 28, 0);
                Blend(p);
                p = Sp(FX_FLARE32, FXP_E_SOLAR, TO_FX(gRt->camX + 186), TO_FX(gRt->camY + 46), 60);
                Scale(p, 24, 0);
                Spin(p, 2);
                for (k = 0; k < 10; k++)
                {
                    p = Sp(FX_SUNRAY16, FXP_E_SOLAR, TO_FX(gRt->camX + 12 + k * 24), TO_FX(gRt->camY + 10 + (k & 1) * 12), 36);
                    if (p) { p->vy = 300; p->vx = -60; }
                    Blend(p);
                }
                break;
            case MOVE_RAIN_DANCE:
                RtFx_Dim(5, 40);
                for (k = 0; k < 16; k++)
                {
                    p = Sp(FX_RAIN8, FXP_RAIN, TO_FX(gRt->camX + (Random() % 250)), TO_FX(gRt->camY + 16 + (Random() % 60)), 24);
                    if (p) { p->vx = -2 * 256; p->vy = 6 * 256; }
                    Drag(p, 16);
                }
                break;
            case MOVE_SANDSTORM:
                for (k = 0; k < 12; k++)
                {
                    p = Sp(k & 1 ? FX_PUFF16 : FX_GRAIN8, FXP_SAND, TO_FX(gRt->camX - 20 - (Random() % 60)), TO_FX(gRt->camY + 30 + (Random() % 110)), 50);
                    if (p) { p->vx = 5 * 256 + (Random() & 255); p->vy = 40; p->flags |= PF_FLICKER; }
                    Drag(p, 16);
                    Scale(p, 20, 0);
                }
                RtFx_Dim(3, 30);
                break;
            case MOVE_HAIL:
                for (k = 0; k < 16; k++)
                {
                    p = Sp((k & 3) ? FX_HAIL8 : FX_SNOW8, FXP_E_WHITE, TO_FX(gRt->camX + 20 + (Random() % 240)), TO_FX(gRt->camY + 10 + (Random() % 40)), 30);
                    if (p) { p->vx = -256; p->vy = 5 * 256; }
                    Drag(p, 16);
                }
                RtFx_Dim(2, 20);
                break;
            case MOVE_WATER_SPORT:
            case MOVE_MUD_SPORT:
                for (k = 0; k < 14; k++)
                {
                    u8 sh = move == MOVE_WATER_SPORT ? FX_DROP8 : FX_MUD16;
                    p = Sp(sh, move == MOVE_WATER_SPORT ? FXP_WATER : FXP_MUD, TO_FX(gRt->camX + 16 + (Random() % 220)), TO_FX(gRt->camY + 10 + (Random() % 30)), 26 + (Random() & 7));
                    if (p) { p->vy = 4 * 256 + (Random() & 255); }
                    Drag(p, 16);
                }
                for (k = 0; k < 8; k++)
                {
                    p = Fly(move == MOVE_WATER_SPORT ? FX_FOAM8 : FX_DEBRIS8, move == MOVE_WATER_SPORT ? FXP_E_FOAM : FXP_E_MUD, a->x, a->y, 160 + k * 9, 420, 20, FALSE);
                    Accel(p, 0, 12);
                }
                break;
            case MOVE_HEAL_BELL:
                p = Sp(FX_BELL16, FXP_E_GOLD, a->x, a->y - TO_FX(a->height + 14), 40);
                if (p) p->frameRate = 5;
                Scale(p, 22, 0);
                for (k = 0; k < 3; k++)
                {
                    p = Sp(FX_PSYWAVE32, FXP_E_GOLD, a->x, a->y - TO_FX(a->height + 10), 16 + k * 8);
                    Scale(p, 4 + k * 3, 2);
                    Blend(p);
                }
                for (k = 0; k < 6; k++)
                {
                    p = Fly(FX_NOTE16, FXP_GOLD, a->x, CenterY(a), k * 43, 260, 30, FALSE);
                    Drag(p, 14);
                    FadeOut(p);
                }
                break;
            case MOVE_PERISH_SONG:
                RtFx_Dim(6, 30);
                for (k = 0; k < 8; k++)
                {
                    p = Fly(FX_NOTE16, FXP_SHADOW, a->x, CenterY(a), k * 32, 300, 34, FALSE);
                    Drag(p, 14);
                    FadeOut(p);
                }
                p = Sp(FX_SKULL16, FXP_SHADOW, a->x, a->y - TO_FX(a->height + 12), 34);
                Scale(p, 18, 0);
                break;
            case MOVE_HAZE:
                for (k = 0; k < 7; k++)
                {
                    p = Sp(FX_PUFF32, FXP_E_FOAM, TO_FX(gRt->camX + 16 + k * 34), TO_FX(gRt->camY + 50 + (Random() % 70)), 46);
                    if (p) { p->vx = 90; p->frameRate = 10; }
                    Scale(p, 14, 0);
                    Blend(p);
                    FadeOut(p);
                }
                break;
            default:
                for (k = 0; k < 12; k++)
                {
                    p = Fly(k & 1 ? FX_SPARKLE8 : FX_MOTE8, k & 2 ? v->pal : v->pal2, a->x, CenterY(a), k * 21, 420, 30, FALSE);
                    Drag(p, 15);
                }
                ImpactRing(v->pal, a->x, a->y, 6, 3, 16);
                break;
            }
        }
        break;
    case VS_CLOUD:
        {
            s32 k;
            for (k = 0; k < 8; k++)
            {
                p = Fly(k & 1 ? FX_PUFF16 : FX_POWDER8, k & 1 ? v->pal : v->pal2, mx, my, a->act.angle + (s8)(Random() % 33 - 16), 520 + (Random() & 255), 22, TRUE);
                Drag(p, 14);
                if (k & 1) Scale(p, 18, 1);
                else Scale(p, 22, 0);
                FadeOut(p);
            }
        }
        break;
    case VS_EXPLODE:
        RtFx_Flash(0, 8);
        RtFx_Dim(5, 14);
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
            for (k = 0; k < 4; k++)
            {
                p = Fly(FX_PUFF32, FXP_E_SMOKE, a->x, cy, k * 64 + 32, 260, 26, FALSE);
                Blend(p);
                FadeOut(p);
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
            ImpactGlow(FXP_E_FOAM, mx, my, 5, 0, 4);
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
        pt = Sp(FX_EMBER8, v->pal == FXP_E_FIRE ? FXP_E_HOT : v->pal, x + TO_FX((Random() % 7) - 3), y + TO_FX((Random() % 7) - 3), 16);
        Fade(pt, v->pal == FXP_E_FIRE ? FXP_E_FIRE : v->pal2, 10, FXP_E_SMOKE, 4);
        Accel(pt, 0, -2);
        if ((p->age & 3) == 0 && v->pal == FXP_E_FIRE)
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
    case VS_EMBLEM:
        if ((p->age & 1) == 0)
        {
            pt = Sp(FX_SPARKLE8, (p->age & 2) ? v->pal : v->pal2, x + TO_FX((Random() % 7) - 3), y + TO_FX((Random() % 7) - 3), 14);
            FadeOut(pt);
        }
        return FALSE;
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
            // each stream piece draws one of the two strands (alternating), the stream is dense enough for
            // both strands to read while costing half the sprites
            k = p->param2 & 1;
            {
                bool8 front = k ? (dep < 0) : (dep >= 0);
                s32 o = k ? -off : off;
                s32 x = sx + RtCos(perp) * o / 256, y = sy + RtSin(perp) * o / 256;
                RtFx_Draw(FX_JET16, (p->age >> 2) & 1, front ? FXP_E_WATER : FXP_WATER, x, y, 4, (p->angle + (k ? -tilt : tilt)) & 0xFC, front ? sc + 1 : sc - 2, RFD_AFFINE);
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
        if (p->move == MOVE_STRING_SHOT)
            return RtFx_Draw(FX_BEAMTHIN32, 0, FXP_E_WHITE, sx, sy, 4, p->angle, 10, RFD_AFFINE);
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
    // core: rotated segments in the key-effect bucket (they are the move); halo: soft unrotated discs behind
    // it, which cost a fraction of the scanline time of rotated double-size sprites
    s32 x0 = FROM_FX(p->x) - gRt->camX, y0 = FROM_FX(p->y) - gRt->camY;
    s32 step = 32 * coreScale / 16 - 4, d;
    u8 fr = (p->age >> 1) % 3;
    if (step < 10)
        step = 10;
    for (d = step / 2; d < len + step / 2; d += step)
    {
        s32 dd = d > len - step / 2 ? len - step / 2 : d;
        s32 x, y;
        if (dd < step / 3)
            dd = step / 3;
        x = x0 + RtCos(p->angle) * dd / 256;
        y = y0 + RtSin(p->angle) * dd / 256;
        RtFx_Draw(shape, fr, pal, x, y, 5, p->angle, coreScale, RFD_AFFINE);
    }
    if (glowScale)
        for (d = 10 + ((p->age >> 1) & 7); d < len; d += 22)
        {
            s32 x = x0 + RtCos(p->angle) * d / 256, y = y0 + RtSin(p->angle) * d / 256;
            RtFx_Draw(FX_GLOW32, 0, glowPal, x, y, 4, 0, 16, RFD_BLEND);
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
            RtFx_Draw(FX_GLOW32, 0, FXP_E_ELEC, x0, y0, 4, 0, 8 + pulse * 2, RFD_BLEND);
            RtFx_Draw(FX_GLOW32, 0, FXP_E_ELEC, sex, sey, 4, 0, 9 + pulse * 2, RFD_BLEND);
            RtFx_Draw(FX_CRACKLE16, p->age % 3, FXP_E_WHITE, sex, sey, 4, 0, 16, 0);
            return TRUE;
        }
    case VS_HYPER:
        DrawBeamBody(p, FX_BEAM32, FXP_E_HOT, FXP_E_GOLD, 17 + pulse, 26 + pulse * 2, len);
        break;
    case VS_SOLAR:
        DrawBeamBody(p, FX_BEAM32, FXP_E_SOLAR, FXP_E_GOLD, 20 + pulse, 30 + pulse * 2, len);
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
        if (p->move == MOVE_HYDRO_CANNON)
        {
            s32 d;
            u8 perp = p->angle + 64;
            DrawBeamBody(p, FX_BEAM32, FXP_WATER, FXP_E_WATER, 18 + pulse, 1, len);
            for (d = 12 + (p->age * 5) % 20; d < len; d += 20)
            {
                s32 o = RtSin(d * 9 - p->age * 20) * 7 / 256;
                s32 x = x0 + RtCos(p->angle) * d / 256 + RtCos(perp) * o / 256, y = y0 + RtSin(p->angle) * d / 256 + RtSin(perp) * o / 256;
                RtFx_Draw(FX_FOAM8, 0, FXP_E_FOAM, x, y, 5, 0, 16, 0);
                RtFx_Draw(FX_FOAM8, 1, FXP_E_WATER, x - RtCos(perp) * o / 128, y - RtSin(perp) * o / 128, 5, 0, 16, 0);
            }
            break;
        }
        if (p->move == MOVE_AEROBLAST)
        {
            s32 d;
            DrawBeamBody(p, FX_BEAMTHIN32, FXP_E_WHITE, FXP_E_SKY, 16, 1, len);
            for (d = (p->age * 6) % 24; d < len; d += 24)
            {
                s32 x = x0 + RtCos(p->angle) * d / 256, y = y0 + RtSin(p->angle) * d / 256;
                RtFx_Draw(FX_TWISTER32, (p->age >> 1) % 3, FXP_E_SKY, x, y, 4, p->angle + 64, 12, RFD_AFFINE | RFD_BLEND);
            }
            break;
        }
        DrawBeamBody(p, FX_BEAM32, v->pal, v->pal2, 14 + pulse, 20, len);
        break;
    }
    // muzzle and tip flares
    RtFx_Draw(FX_FLARE32, p->age % 3, v->pal, x0, y0, 4, p->age * 6, (v->flags & VF_BIG) ? 20 : 15, RFD_AFFINE);
    RtFx_Draw(FX_GLOW32, 0, v->pal2, sex, sey, 4, 0, (v->flags & VF_BIG) ? 14 : 10, RFD_BLEND);
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
        RtFx_Draw(FX_ARC32, fr, v->pal2, sx, sy, 1, p->angle, 26, RFD_AFFINE | RFD_BLEND);
        return RtFx_Draw(FX_ARC32, fr, v->pal, sx, sy, 1, p->angle, 22, RFD_AFFINE);
    case VS_CLAW:
        RtFx_Draw(FX_CLAW32, fr, v->pal2, sx, sy, 1, p->angle, 24, RFD_AFFINE | RFD_BLEND);
        return RtFx_Draw(FX_CLAW32, fr, v->pal, sx, sy, 1, p->angle, 20, RFD_AFFINE);
    case VS_BITE:
        return RtFx_Draw(FX_JAWS32, fr, v->pal, sx, sy, 1, 0, 22, RFD_AFFINE);
    case VS_KICK:
        RtFx_Draw(FX_GLOW32, 0, v->pal2, sx, sy, 1, 0, 12 + fr * 3, RFD_BLEND);
        return RtFx_Draw(FX_BOOM32, fr, v->pal, sx, sy, 1, p->angle, 12 + fr * 2, RFD_AFFINE);
    case VS_PUNCH:
        RtFx_Draw(FX_GLOW32, 0, v->pal2, sx, sy, 1, 0, 14, RFD_BLEND);
        return RtFx_Draw(FX_FIST32, 0, v->pal, sx, sy, 1, p->angle, 16 + fr * 2, RFD_AFFINE);
    case VS_HORN:
        RtFx_Draw(FX_SPEED16, 0, FXP_E_WHITE, sx - RtCos(p->angle) * 10 / 256, sy - RtSin(p->angle) * 10 / 256, 1, p->angle, 16, RFD_AFFINE);
        return RtFx_Draw(FX_HORN16, 0, v->pal, sx, sy, 1, p->angle, 18, RFD_AFFINE);
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
        {
            // a wall of breaking waves: curling crests side by side along the whole front, foam on the lips
            bool8 across = RtCos(p->angle) > 181 || RtCos(p->angle) < -181; // travelling mostly sideways
            u8 flip = RtCos(p->angle) < 0 ? RFD_HFLIP : 0;
            step = across ? 12 : 22;
            n = (half * 2) / step + 1;
            if (n > 8)
                n = 8;
            for (i = 0; i < n; i++)
            {
                s32 k = n == 1 ? 0 : -half + (half * 2) * i / (n - 1);
                s32 st = RtSin(i * 90 + p->age * 10) * 4 / 256; // ragged front
                s32 x = sx + RtCos(perp) * k / 256 + RtCos(p->angle) * st / 256, y = sy + RtSin(perp) * k / 256 + RtSin(p->angle) * st / 512;
                RtFx_Draw(FX_SURGE32, ((p->age >> 2) + i) % 3, v->pal, x, y - 10, i == n / 2 ? 5 : 4, 0, 16, flip);
            }
            return TRUE;
        }
    case VS_QUAKE:
        return TRUE;
    case VS_SOUND:
        if (p->move == MOVE_SILVER_WIND)
        {
            step = 14;
            n = (half * 2) / step + 1;
            for (i = 0; i < n; i++)
            {
                s32 k = n == 1 ? 0 : -half + (half * 2) * i / (n - 1);
                s32 x = sx + RtCos(perp) * k / 256 + RtSin(i * 70 + p->age * 12) * 4 / 256, y = sy + RtSin(perp) * k / 256;
                RtFx_Draw(FX_SPEED16, (p->age >> 1) & 1, FXP_E_WHITE, x, y - 8, 4, p->angle, 26, RFD_AFFINE);
                if ((i + p->age) & 1)
                    RtFx_Draw(FX_SPARKLE8, (p->age >> 1) & 3, FXP_E_WHITE, x - RtCos(p->angle) * 8 / 256, y - 12, 4, 0, 16, 0);
            }
            return TRUE;
        }
        if (p->shape == FX_NOTE16)
        {
            step = 16;
            n = (half * 2) / step + 1;
            for (i = 0; i < n; i++)
            {
                s32 k = n == 1 ? 0 : -half + (half * 2) * i / (n - 1);
                s32 x = sx + RtCos(perp) * k / 256, y = sy + RtSin(perp) * k / 256 + RtSin(p->age * 12 + i * 60) * 4 / 256;
                RtFx_Draw(FX_NOTE16, (p->age >> 3) & 1, v->pal, x, y - 10, 4, 0, 20, RFD_AFFINE);
            }
            return TRUE;
        }
        step = 20;
        n = (half * 2) / step + 1;
        for (i = 0; i < n; i++)
        {
            s32 k = n == 1 ? 0 : -half + (half * 2) * i / (n - 1);
            s32 x = sx + RtCos(perp) * k / 256, y = sy + RtSin(perp) * k / 256;
            RtFx_Draw(FX_PSYWAVE32, (p->age >> 2) & 1, v->pal, x, y - 8, 4, p->angle, 16, RFD_AFFINE);
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

static bool8 DrawZone(struct RtProj *p, const struct RtVfxDef *v, s32 sx, s32 sy)
{
    s32 k, n;
    switch (v->style)
    {
    case VS_VORTEX:
        // a ring of fire / water / sand spinning around the spot, back half behind, front half in front
        n = 8;
        for (k = 0; k < n; k++)
        {
            u8 a = k * 256 / n + p->age * 6;
            s32 r = p->maxRadius;
            s32 x = sx + RtCos(a) * r / 256, y = sy + RtSin(a) * r / 512;
            u8 shape = v->pal == FXP_E_FIRE ? FX_FIRE16 : (v->pal == FXP_E_WATER ? FX_JET16 : FX_PUFF16);
            u8 fr = (p->age / 3 + k) % gRtFxShapes[shape].frames;
            if (shape == FX_JET16)
                RtFx_Draw(shape, fr, (k & 1) ? FXP_E_WATER : FXP_E_FOAM, x, y - 6, RtSin(a) > 0 ? 1 : 4, a + 64, 16, RFD_AFFINE);
            else
                RtFx_Draw(shape, fr, v->pal == FXP_E_SAND ? FXP_SAND : v->pal, x, y - 6 - (k & 1) * 4, RtSin(a) > 0 ? 1 : 4, 0, shape == FX_PUFF16 ? 22 : 16, (shape == FX_PUFF16 && v->pal != FXP_E_SAND) ? RFD_BLEND : 0);
        }
        return TRUE;
    case VS_CLOUD:
        n = p->maxRadius > 18 ? 5 : 4;
        for (k = 0; k < n; k++)
        {
            u8 a = k * 256 / n + p->age * 2;
            s32 r = p->maxRadius * ((k * 37 + p->age / 2) % 16) / 16;
            s32 x = sx + RtCos(a) * r / 256, y = sy + RtSin(a) * r / 512;
            RtFx_Draw(FX_PUFF16, (p->age / 8 + k) % 3, v->pal, x, y - 6, 4, 0, 20 + (k & 1) * 8, RFD_BLEND);
        }
        RtFx_Draw(FX_SPARKLE8, (p->age >> 1) & 3, v->pal2, sx + RtSin(p->age * 7) * p->maxRadius / 512, sy - 8, 4, 0, 16, 0);
        return TRUE;
    }
    return FALSE;
}

bool8 RtVfx_ZoneTick(struct RtProj *p)
{
    const struct RtVfxDef *v = V(p->move);
    struct RtPart *pt;
    sPrio = 4;
    switch (v->style)
    {
    case VS_VORTEX:
        if ((p->age & 3) == 0)
        {
            u8 a = Random();
            if (v->pal == FXP_E_FIRE)
            {
                pt = Sp(FX_EMBER8, FXP_E_HOT, p->x + RtCos(a) * p->maxRadius, p->y + RtSin(a) * p->maxRadius / 2, 18);
                Accel(pt, 0, -4);
                Fade(pt, FXP_E_FIRE, 10, FXP_E_SMOKE, 4);
            }
            else
            {
                pt = Sp(v->pal == FXP_E_WATER ? FX_FOAM8 : FX_GRAIN8, v->pal == FXP_E_WATER ? FXP_E_FOAM : FXP_SAND, p->x + RtCos(a) * p->maxRadius, p->y + RtSin(a) * p->maxRadius / 2, 16);
                Accel(pt, 0, -2);
            }
        }
        return TRUE;
    case VS_CLOUD:
        if ((p->age & 7) == 0)
        {
            u8 a = Random();
            s32 r = Random() % (p->radius + 1);
            pt = Sp(FX_POWDER8, v->pal2, p->x + RtCos(a) * r, p->y + RtSin(a) * r / 2, 20);
            if (pt) pt->vy = -60;
        }
        return TRUE;
    }
    return FALSE;
}

bool8 RtVfx_Cone(struct RtActor *a, u16 move)
{
    const struct RtVfxDef *v = V(move);
    const struct RtMoveDef *d = &gRtMoveDefs[move];
    struct RtPart *p;
    s32 k, range = d->range * 2;
    u8 dur = d->duration ? d->duration : 16;
    s32 mx = a->x + RtCos(a->act.angle) * a->radius, my = CenterY(a) + RtSin(a->act.angle) * a->radius / 2;
    sPrio = 1;
    switch (move)
    {
    case MOVE_TAIL_WHIP:
        for (k = 0; k < 2; k++)
        {
            p = Sp(FX_ARC32, FXP_E_WHITE, a->x - RtCos(a->act.angle) * a->radius, a->y - TO_FX(6), 12 + k * 6);
            if (p) { p->angle = a->act.angle + 128 + (k ? 40 : -40); p->frameRate = 4; }
            Scale(p, 22, 0);
        }
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_ARROWDN8, FXP_WATER, mx, my, a->act.angle + (s8)(Random() % 33 - 16), range * 256 / dur, dur, TRUE);
            Scale(p, 22, 0);
        }
        return TRUE;
    case MOVE_FEATHER_DANCE:
    case MOVE_TICKLE:
        for (k = 0; k < (move == MOVE_TICKLE ? 5 : 10); k++)
        {
            p = Fly(FX_FEATHER16, move == MOVE_TICKLE ? FXP_GOLD : FXP_WHITE, mx, my, a->act.angle + (s8)(Random() % 49 - 24), range * 256 / (dur + 8) * (160 + (Random() & 95)) / 256, dur + 10 + (Random() & 7), TRUE);
            Drag(p, 15);
            Spin(p, (k & 1) ? 6 : -6);
            FadeOut(p);
        }
        if (move == MOVE_TICKLE)
            for (k = 0; k < 6; k++)
                Fly(FX_SPARKLE8, FXP_E_GOLD, mx, my, a->act.angle + (s8)(Random() % 49 - 24), range * 256 / dur, dur, TRUE);
        return TRUE;
    case MOVE_FAKE_TEARS:
        for (k = 0; k < 8; k++)
        {
            p = Fly(FX_DROP8, FXP_WATER, a->x + TO_FX((k & 1) ? 6 : -6), a->y - TO_FX(a->height * 2 / 3), (k & 1) ? 224 - k * 4 : 160 + k * 4, 300, 22, FALSE);
            Accel(p, 0, 9);
        }
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_DROP8, FXP_WATER, mx, my, a->act.angle + (s8)(Random() % 33 - 16), range * 256 / dur, dur, TRUE);
            Scale(p, 20, 0);
        }
        return TRUE;
    case MOVE_KINESIS:
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_PSYWAVE32, (k & 1) ? FXP_E_PINK : FXP_E_PSY, mx, my, a->act.angle, range * 256 / (dur + 4), dur + 4 + k * 3, TRUE);
            if (p) { p->angle = a->act.angle; p->x -= p->vx * k * 3; p->y -= p->vy * k * 3; }
            Scale(p, 6 + k * 2, 1);
            Blend(p);
        }
        RtFx_Wave(2, 20);
        return TRUE;
    case MOVE_SWAGGER:
        for (k = 0; k < 6; k++)
        {
            p = Fly((k & 1) ? FX_SPARKLE8 : FX_ARROWUP8, (k & 1) ? FXP_E_GOLD : FXP_BLOOD, mx, my, a->act.angle + (s8)(Random() % 41 - 20), range * 256 / dur, dur, TRUE);
            Scale(p, 20, 0);
        }
        p = Sp(FX_FLARE32, FXP_E_GOLD, a->x, a->y - TO_FX(a->height), 14);
        Spin(p, 6);
        return TRUE;
    }
    switch (v->style)
    {
    case VS_SPRAY:
        for (k = 0; k < 14; k++)
        {
            s8 off = (s8)((Random() % (d->spread * 2 + 1)) - d->spread);
            u8 ang = a->act.angle + off;
            s32 sp = range * 256 / dur * (180 + (Random() & 63)) / 256;
            u8 shape = (k & 1) ? FX_PUFF16 : d->shape;
            if (v->pal == FXP_E_ICE && !(k & 1))
                shape = FX_SNOW8;
            p = Fly(shape, (k & 1) ? v->pal2 : v->pal, mx, my, ang, sp, dur + (Random() & 7), TRUE);
            Drag(p, 15);
            if (shape == FX_PUFF16) { if (v->pal != FXP_E_SAND) Blend(p); Scale(p, 12, 1); }
            if (shape == FX_SNOW8) Spin(p, 10);
        }
        if (v->pal == FXP_E_ICE || v->pal == FXP_E_SKY)
            for (k = 0; k < 5; k++)
            {
                p = Fly(FX_SPEED16, FXP_E_WHITE, mx, my, a->act.angle + (s8)(Random() % 33 - 16), range * 256 / 10, 10, TRUE);
                Stretch(p);
            }
        return TRUE;
    case VS_SHOUT:
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_PSYWAVE32, v->pal, mx, my, a->act.angle, range * 256 / (dur + 4), dur + 4 + k * 3, TRUE);
            if (p)
            {
                p->angle = a->act.angle;
                Scale(p, 6 + k * 2, 1);
                Blend(p);
                RtFx_X(p)->drag = 16;
                p->life = dur + k * 4;
                p->maxLife = p->life;
                // stagger the rings
                p->x -= p->vx * k * 3;
                p->y -= p->vy * k * 3;
            }
        }
        if (move == MOVE_METAL_SOUND)
            for (k = 0; k < 5; k++)
            {
                p = Fly(FX_LBOLT16, FXP_E_STEEL, mx, my, a->act.angle + (s8)(Random() % 41 - 20), range * 256 / (dur + 2), dur, TRUE);
                Stretch(p);
            }
        if (move == MOVE_SNORE)
            for (k = 0; k < 4; k++)
            {
                p = Fly(FX_Z8, FXP_SLEEP, mx, my, a->act.angle + (s8)(Random() % 41 - 20), range * 256 / (dur + 6), dur + 8, TRUE);
                Scale(p, 20, 0);
            }
        if (v->flags & VF_SHAKE1)
            RtFx_Shake(1, 12);
        return TRUE;
    case VS_GAZE:
        p = Sp(FX_EYE16B, v->pal, a->x, CenterY(a) - TO_FX(18), 30);
        Scale(p, 18, 1);
        if (p) { p->frameRate = 0; p->frame = 1; RtFx_X(p)->flags |= PX_FADE; }
        ImpactGlow(v->pal, a->x, CenterY(a) - TO_FX(18), 6, 1, 14);
        for (k = 0; k < 6; k++)
        {
            p = Fly(FX_SPARKLE8, v->pal, mx, my, a->act.angle + (s8)(Random() % 41 - 20), range * 256 / dur, dur, TRUE);
            Drag(p, 16);
        }
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_SPEED16, v->pal, mx, my, a->act.angle + (s8)(k * 8 - 12), range * 256 / (dur > 6 ? dur - 4 : dur), dur, TRUE);
            Stretch(p);
        }
        RtFx_Dim(3, 8);
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
            if (p->move == MOVE_EGG_BOMB || p->move == MOVE_BARRAGE)
            {
                RtFx_Draw(FX_EGG16, 0, FXP_E_WHITE, sx, sy, 5, p->age * 9, p->move == MOVE_EGG_BOMB ? 24 : 18, RFD_AFFINE);
                return TRUE;
            }
            RtFx_Draw(FX_GLOW32, 0, v->pal2, sx, sy, 4, 0, 14 + ((p->age >> 1) & 1) * 2, RFD_BLEND);
            RtFx_Draw(FX_ORB16, (p->age >> 2) & 1, v->pal, sx, sy, 5, 0, (v->flags & VF_BIG) ? 22 : 16, 0);
            return TRUE;
        case VS_FIREBALL:
            RtFx_Draw(FX_GLOW32, 0, v->pal == FXP_E_FIRE ? FXP_E_HOT : v->pal, sx, sy, 4, 0, (v->flags & VF_BIG) ? 22 : 14, RFD_BLEND);
            RtFx_Draw((v->flags & VF_BIG) ? FX_FIRE32 : FX_FIRE16, (p->age >> 1) % ((v->flags & VF_BIG) ? 4 : 6), v->pal, sx, sy, 4, p->angle - 64, 16, RFD_AFFINE);
            return TRUE;
        case VS_ELECSHOT:
            if (v->flags & VF_BIG)
            {
                RtFx_Draw(FX_GLOW32, 0, FXP_E_ELEC, sx, sy, 4, 0, 24, RFD_BLEND);
                RtFx_Draw(FX_ORB16, (p->age >> 2) & 1, FXP_E_ELEC, sx, sy, 5, 0, 22, 0);
                RtFx_Draw(FX_CRACKLE16, (p->age >> 1) % 3, (p->age & 2) ? FXP_E_WHITE : FXP_E_BOLT, sx, sy, 5, p->age * 20, 28, RFD_AFFINE);
                return TRUE;
            }
            if (p->age > 2)
            {
                s32 bx = sx - (p->vx * 9 >> 8), by = sy - (p->vy * 9 >> 8);
                BoltLine(bx, by, sx, sy, (p->age & 2) ? FXP_E_ELEC : FXP_E_BOLT, (p->age >> 1) * 2654435761u + (u32)p->param2 * 77, 4, 0, 4);
            }
            RtFx_Draw(FX_GLOW32, 0, FXP_E_ELEC, sx, sy, 4, 0, 12, RFD_BLEND);
            RtFx_Draw(FX_CRACKLE16, (p->age >> 1) % 3, (p->age & 2) ? FXP_E_WHITE : FXP_E_ELEC, sx, sy, 4, p->age * 20, 20, RFD_AFFINE);
            return TRUE;
        case VS_DARKORB:
            if (p->move == MOVE_SHADOW_PUNCH)
            {
                RtFx_Draw(FX_GLOW32, 0, FXP_E_GHOST, sx, sy, 4, 0, 18, RFD_BLEND);
                return RtFx_Draw(FX_FIST32, 0, FXP_E_GHOST, sx, sy, 5, p->angle, 18, RFD_AFFINE);
            }
            RtFx_Draw(FX_GLOW32, 0, FXP_E_GHOST, sx, sy, 4, 0, (v->flags & VF_HEAVY) ? 24 : 20, RFD_BLEND);
            RtFx_Draw(FX_DORB32, (p->age >> 2) % 3, FXP_E_GHOST, sx, sy, 5, p->age * 4, (v->flags & VF_HEAVY) ? 18 : 14, RFD_AFFINE);
            return TRUE;
        case VS_GOO:
        case VS_MUDBALL:
            RtFx_Draw(FX_GOO16, (p->age >> 2) % 3, v->pal, sx, sy, 5, 0, (v->flags & VF_BIG) ? 24 : 19, RFD_AFFINE);
            return TRUE;
        case VS_LEAF:
            return RtFx_Draw(FX_LEAF16B, (p->age >> 1) & 3, v->pal, sx, sy, 4, 0, 16, 0);
        case VS_STAR:
            RtFx_Draw(FX_GLOW32, 0, FXP_E_GOLD, sx, sy, 4, 0, 10, RFD_BLEND);
            return RtFx_Draw(FX_STAR16B, 0, FXP_E_GOLD, sx, sy, 5, p->age * 10, 24, RFD_AFFINE);
        case VS_SHARD:
            return RtFx_Draw(FX_SHARD16B, 0, v->pal, sx, sy, 5, p->angle, 23, RFD_AFFINE);
        case VS_ROCK:
            return RtFx_Draw(FX_ROCK16B, (p->age >> 3) & 1, FXP_E_ROCK, sx, sy, 5, p->age * 8, (v->flags & VF_BIG) ? 26 : 21, RFD_AFFINE);
        case VS_BLADE:
            RtFx_Draw(FX_ARC32, 1, v->pal, sx, sy, 5, p->angle, 21, RFD_AFFINE);
            return RtFx_Draw(FX_ARC32, 1, v->pal2, sx - (p->vx >> 7), sy - (p->vy >> 7), 4, p->angle, 24, RFD_AFFINE | RFD_BLEND);
        case VS_WATERBALL:
            RtFx_Draw(FX_GLOW32, 0, FXP_E_WATER, sx, sy, 4, 0, 14, RFD_BLEND);
            return RtFx_Draw(FX_BUBBLE16, (p->age >> 2) & 1, FXP_WATER, sx, sy, 4, 0, 20, RFD_AFFINE);
        case VS_BUBBLE:
            return RtFx_Draw(FX_BUBBLE16, (p->age >> 3) & 1, FXP_WATER, sx + RtSin(p->age * 12) * 3 / 256, sy, 4, 0, 16, 0);
        case VS_NEEDLE:
            return RtFx_Draw(FX_NEEDLE8, 0, (p->pal == FXP_BUG || p->pal == FXP_POISON || p->pal == FXP_GRASS) ? p->pal : FXP_WHITE, sx, sy, 5, p->angle, 30, RFD_AFFINE);
        case VS_EMBLEM:
            if (p->shape == FX_BONE16)
                return RtFx_Draw(FX_BONE16, 0, FXP_WHITE, sx, sy, 5, p->age * 24, 26, RFD_AFFINE);
            RtFx_Draw(FX_GLOW32, 0, v->pal, sx, sy, 4, 0, 8, RFD_BLEND);
            if (p->shape != FX_NONE)
                RtFx_Draw(p->shape, (p->age >> 2) % gRtFxShapes[p->shape].frames, p->pal, sx, sy, 4, 0, gRtFxShapes[p->shape].w <= 8 ? 28 : 22, RFD_AFFINE);
            return TRUE;
        case VS_TWISTER:
            RtFx_Draw(FX_TWISTER32, (p->age >> 1) % 3, v->pal, sx, sy - 8, 5, 0, 21, RFD_AFFINE);
            return TRUE;
        }
        return FALSE;
    case PK_BEAM:
        if (p->delay)
            return FALSE;
        return DrawBeamStyle(p, v);
    case PK_ZONE:
        return DrawZone(p, v, sx, sy);
    case PK_STRIKE:
        if (!p->delay && (p->param & 0x80))
        {
            // the bolt of Thunder: from above the screen down to the strike point, re-jagged every 2 frames
            u32 seed = (p->age >> 1) * 2246822519u + p->x;
            s32 top = -8, x0 = sx + (s32)((seed >> 8) % 41) - 20;
            BoltLine(x0, top, sx, sy - 6, (p->age & 2) ? FXP_E_ELEC : FXP_E_WHITE, seed, 10, 3, 1);
            BoltLine(x0 + 2, top, sx + 1, sy - 6, FXP_E_ELEC, seed, 10, 0, 1);
            if (p->age & 1)
                BoltLine(x0 - 10, top, sx, sy - 6, FXP_E_BOLT, seed ^ 0x77, 14, 1, 1);
            RtFx_Draw(FX_GLOW32, 0, FXP_E_ELEC, sx, sy - 6, 1, 0, 10 - (p->age > 6 ? 6 : p->age) / 2, RFD_BLEND);
            return TRUE;
        }
        return FALSE;
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
        DoImpact(v->impact == VI_DEFAULT ? TypeImpact(RTM(p->move).type) : v->impact, v->pal, v->pal2, TO_FX(ex), TO_FX(ey), FALSE);
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
        if ((p->age & 3) == 0)
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
            // the bolt itself is drawn by RtVfx_DrawProj while the strike lingers
            RtFx_Flash(1, 4);
            RtFx_Dim(9, 16);
            pt = Sp(FX_CRACK32, FXP_E_DARK, p->x, p->y, 60);
            if (pt) pt->flags |= PF_GROUND;
            DoImpact(VI_ELEC, v->pal, v->pal2, p->x, p->y - TO_FX(8), TRUE);
            ImpactRing(FXP_E_ELEC, p->x, p->y, 8, 1, 12);
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
        DoImpact(VI_ICE, v->pal, v->pal2, p->x, p->y - TO_FX(6), FALSE);
        return TRUE;
    case VS_METEOR:
        // a pillar of light falling from the sky onto the spot
        for (k = 0; k < 6; k++)
        {
            pt = Sp(FX_BEAM32, k & 1 ? v->pal2 : v->pal, p->x, p->y - TO_FX(8 + k * 22), 16 - k);
            if (pt) { pt->angle = 64; pt->flags |= PF_SCALE; pt->scale = 22 - k; pt->dscale = -1; pt->prio = 5; }
        }
        ImpactGlow(v->pal2, p->x, p->y - TO_FX(10), 12, 1, 18);
        ImpactRing(v->pal, p->x, p->y, 8, 3, 16);
        DoImpact(v->impact == VI_DEFAULT ? TypeImpact(RTM(p->move).type) : v->impact, v->pal, v->pal2, p->x, p->y - TO_FX(6), TRUE);
        RtFx_Flash(0, 8);
        RtFx_Shake(2, 12);
        return TRUE;
    case VS_PSYBLAST:
        RtFx_Wave(4, 40);
        DoImpact(VI_PSY, v->pal, v->pal2, p->x, p->y - TO_FX(10), TRUE);
        ImpactGlow(FXP_E_PSY, p->x, p->y - TO_FX(10), 8, 1, 16);
        for (k = 0; k < 3; k++)
        {
            pt = Sp(FX_PSYWAVE32, k == 1 ? FXP_E_PINK : FXP_E_PSY, p->x, p->y - TO_FX(10), 14 + k * 5);
            Scale(pt, 4 + k * 4, 2);
        }
        return TRUE;
    case VS_PILLAR:
        for (k = 0; k < 5; k++)
        {
            pt = Sp(v->pal == FXP_E_WATER ? FX_JET16 : (v->pal == FXP_E_GRASS ? FX_LEAF16B : FX_FIRE32), v->pal, p->x + TO_FX((Random() % 9) - 4), p->y - TO_FX(6 + k * 10), 18);
            if (pt) { pt->vy = -200 - k * 40; pt->angle = 192; pt->flags |= PF_SCALE; pt->scale = 16; pt->dscale = 0; }
        }
        DoImpact(v->impact == VI_DEFAULT ? TypeImpact(RTM(p->move).type) : v->impact, v->pal, v->pal2, p->x, p->y, TRUE);
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
            ImpactGlow(v->pal2, p->x, p->y - TO_FX(8), 7, 1, 12);
            if (p->move == MOVE_SHEER_COLD)
            {
                s32 k;
                RtFx_Flash(0, 6);
                for (k = 0; k < 8; k++)
                {
                    pt = Fly(FX_CRYSTAL16, FXP_E_ICE, p->x, p->y - TO_FX(8), k * 32, 360, 30, FALSE);
                    if (pt) pt->angle = k * 32;
                    Drag(pt, 13);
                    FadeOut(pt);
                }
                pt = Sp(FX_ICEBLOCK32, FXP_ICE, p->x, p->y - TO_FX(12), 30);
                Scale(pt, 22, 0);
                Blend(pt);
            }
            if (p->move == MOVE_TEETER_DANCE)
                OrbitAround(&gRt->actors[p->owner], FX_DIZZY8, FXP_GOLD, 4, 18, 9, 36, gRt->actors[p->owner].height + 4);
        }
        if (p->move == MOVE_SHEER_COLD && (p->age & 1))
        {
            a = Random();
            pt = Fly(FX_SNOW8, FXP_E_WHITE, p->x + RtCos(a) * p->radius, p->y + RtSin(a) * p->radius / 2 - TO_FX(6), a, 160, 18, TRUE);
            Spin(pt, 9);
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
// ---- motifs for self-targeted moves
static void MOrbit(struct RtActor *a, u8 shape, u8 pal, u8 n, u8 radius, s8 speed, s8 radial, u8 life)
{
    u8 k;
    for (k = 0; k < n; k++)
    {
        struct RtPart *p = Sp(shape, pal, a->x, CenterY(a), life);
        if (p)
        {
            struct RtPartX *x = RtFx_X(p);
            x->flags |= PX_ORBIT;
            x->owner = a->id;
            x->orbit = radius;
            x->orbitAng = k * 256 / n;
            x->ay = speed;
            x->ax = radial;
            if (gRtFxShapes[shape].frames > 1)
                p->frameRate = 3;
        }
    }
}

static void MRise(struct RtActor *a, u8 shape, u8 pal, u8 pal2, u8 n, s16 vy, u8 life)
{
    u8 k;
    for (k = 0; k < n; k++)
    {
        u8 ang = k * (256 / n) + (Random() & 15);
        struct RtPart *p = Sp(shape, (k & 1) ? pal2 : pal, a->x + RtCos(ang) * (a->radius + 2), a->y + RtSin(ang) * (a->radius + 2) / 2 - TO_FX(Random() % (a->height / 2 + 1)), life + (k & 3) * 3);
        if (p)
        {
            p->vy = vy - (Random() & 63);
            if (gRtFxShapes[shape].frames > 1)
                p->frameRate = 3;
            FadeOut(p);
        }
    }
}

static void MFlameAura(struct RtActor *a, u8 pal)
{
    u8 k;
    for (k = 0; k < 8; k++)
    {
        s32 side = (k & 1) ? 1 : -1;
        struct RtPart *p = Sp(FX_FIRE16, pal, a->x + TO_FX(side * (a->radius / 2 + (Random() % 5))), a->y - TO_FX(Random() % (a->height / 2 + 2)), 16 + (k & 3) * 3);
        if (p)
        {
            p->vy = -180 - (Random() & 127);
            p->frameRate = 3;
        }
    }
}

static void MRings(struct RtActor *a, u8 pal, u8 n)
{
    u8 k;
    for (k = 0; k < n; k++)
    {
        struct RtPart *p = Sp(FX_PSYWAVE32, pal, a->x, a->y - TO_FX(a->height + 2), 14 + k * 6);
        Scale(p, 4 + k * 2, 1);
        Blend(p);
    }
}

static void MGlint(struct RtActor *a, u8 pal)
{
    struct RtPart *p = Sp(FX_FLARE32, pal, a->x + TO_FX(a->radius / 2), a->y - TO_FX(a->height * 3 / 4), 14);
    Spin(p, 6);
    p = Sp(FX_FLARE32, FXP_E_WHITE, a->x - TO_FX(a->radius / 2), a->y - TO_FX(a->height / 3), 10);
    Spin(p, -8);
}

static void MRain(struct RtActor *a, u8 shape, u8 pal, u8 n, u8 life)
{
    // things falling onto the actor from above (sunlight, a wish star, drops)
    u8 k;
    for (k = 0; k < n; k++)
    {
        struct RtPart *p = Sp(shape, pal, a->x + TO_FX((Random() % 41) - 20), CenterY(a) - TO_FX(36 + (Random() & 15)), life);
        if (p)
        {
            p->vy = 320;
            RtFx_X(p)->drag = 16;
            if (gRtFxShapes[shape].frames > 1)
                p->frameRate = 3;
        }
    }
}

bool8 RtVfx_SelfCast(struct RtActor *a, u16 move)
{
    const struct RtVfxDef *v = V(move);
    s32 cy = CenterY(a);
    struct RtPart *p;
    s32 k;
    sPrio = 1;
    if (v->style == VS_NONE)
        return FALSE;
    p = Sp(FX_RING32, v->pal, a->x, a->y, 14);
    Scale(p, 6, 1);
    if (p) p->flags |= PF_GROUND | PF_BLEND;
    switch (move)
    {
    case MOVE_SWORDS_DANCE:
        MOrbit(a, FX_ARC32, FXP_E_STEEL, 2, 20, 10, 0, 40);
        MOrbit(a, FX_ARC32, FXP_E_WHITE, 1, 12, -12, 0, 30);
        MGlint(a, FXP_E_WHITE);
        ImpactGlow(FXP_E_STEEL, a->x, cy, 6, 1, 20);
        break;
    case MOVE_GROWTH:
    case MOVE_SYNTHESIS:
    case MOVE_NATURE_POWER:
        MRise(a, FX_LEAF16B, FXP_E_GRASS, FXP_E_HEAL, 10, -140, 30);
        p = Sp(FX_GLOW32, FXP_E_HEAL, a->x, cy, 30);
        Scale(p, 6, 1);
        Blend(p);
        if (move == MOVE_GROWTH)
            MOrbit(a, FX_ARROWUP8, FXP_E_GRASS, 3, 16, 6, 0, 30);
        break;
    case MOVE_INGRAIN:
        for (k = 0; k < 6; k++)
        {
            p = Sp(FX_LEAF16B, FXP_E_GRASS, a->x + TO_FX((k - 3) * 6), a->y + TO_FX(2), 70);
            if (p) { p->angle = 64 + (k - 3) * 10; p->frameRate = 0; p->frame = k & 3; p->flags |= PF_GROUND; }
        }
        MRise(a, FX_SPARKLE8, FXP_E_HEAL, FXP_E_GRASS, 6, -120, 18);
        break;
    case MOVE_MEDITATE:
    case MOVE_CALM_MIND:
    case MOVE_PSYCH_UP:
        MRings(a, move == MOVE_PSYCH_UP ? FXP_E_PSY : FXP_E_SKY, 3);
        MOrbit(a, FX_SPARKLE8, move == MOVE_PSYCH_UP ? FXP_E_PINK : FXP_E_WHITE, 6, 18, 6, -1, 34);
        p = Sp(FX_GLOW32, move == MOVE_PSYCH_UP ? FXP_E_PSY : FXP_E_SKY, a->x, cy, 30);
        Scale(p, 8, 0);
        Blend(p);
        break;
    case MOVE_AMNESIA:
        MOrbit(a, FX_QMARK8, FXP_E_SKY, 3, 12, 6, 0, 44);
        p = Sp(FX_QMARK8, FXP_E_WHITE, a->x, a->y - TO_FX(a->height + 10), 40);
        Scale(p, 28, 0);
        FadeOut(p);
        break;
    case MOVE_AGILITY:
    case MOVE_DOUBLE_TEAM:
        RtVfx_Ghosts(a, move == MOVE_DOUBLE_TEAM ? 6 : 4, 36);
        for (k = 0; k < 6; k++)
        {
            p = Sp(FX_SPEED16, FXP_E_WHITE, a->x + TO_FX((k & 1) ? 14 : -14), a->y - TO_FX(4 + k * 4), 16 + k * 3);
            if (p) { p->vx = (k & 1) ? 300 : -300; p->angle = (k & 1) ? 0 : 128; }
            Stretch(p);
        }
        break;
    case MOVE_MINIMIZE:
        for (k = 0; k < 3; k++)
        {
            p = Sp(FX_RING32, FXP_E_WHITE, a->x, cy, 18 + k * 6);
            Scale(p, 24 + k * 6, -1);
            Blend(p);
        }
        MOrbit(a, FX_ARROWDN8, FXP_E_WHITE, 4, 20, 0, -4, 24);
        break;
    case MOVE_TELEPORT:
        MRise(a, FX_SPARKLE8, FXP_E_PSY, FXP_E_PINK, 10, -200, 18);
        MRings(a, FXP_E_PSY, 2);
        break;
    case MOVE_FOCUS_ENERGY:
    case MOVE_BULK_UP:
    case MOVE_RAGE:
    case MOVE_BIDE:
        MFlameAura(a, move == MOVE_BIDE || move == MOVE_FOCUS_ENERGY ? FXP_E_GOLD : FXP_E_FIGHT);
        MFlameAura(a, move == MOVE_BIDE || move == MOVE_FOCUS_ENERGY ? FXP_E_WHITE : FXP_E_RED);
        p = Sp(FX_GLOW32, move == MOVE_BIDE || move == MOVE_FOCUS_ENERGY ? FXP_E_GOLD : FXP_E_FIGHT, a->x, cy, 30);
        Scale(p, 8, 0);
        Blend(p);
        if (move == MOVE_FOCUS_ENERGY)
            MOrbit(a, FX_SPARKLE8, FXP_E_GOLD, 8, 28, 6, -4, 26);
        RtFx_Shake(1, 8);
        break;
    case MOVE_DRAGON_DANCE:
        MFlameAura(a, FXP_E_DRAGON);
        MFlameAura(a, FXP_E_GHOST);
        MOrbit(a, FX_FIRE16, FXP_E_DRAGON, 4, 20, 10, 0, 40);
        p = Sp(FX_GLOW32, FXP_E_DRAGON, a->x, cy, 34);
        Scale(p, 10, 0);
        Blend(p);
        break;
    case MOVE_HARDEN:
    case MOVE_IRON_DEFENSE:
        p = Glue(a, FX_HEX32, move == MOVE_IRON_DEFENSE ? FXP_STEEL : FXP_E_STEEL, 0, a->height / 2, 36);
        Scale(p, move == MOVE_IRON_DEFENSE ? 22 : 18, 0);
        if (p) RtFx_X(p)->flags |= PX_FADE;
        if (move == MOVE_HARDEN)
            MOrbit(a, FX_SPARKLE8, FXP_E_WHITE, 6, 16, 9, -1, 30);
        MGlint(a, FXP_E_WHITE);
            for (k = 0; k < 4; k++)
            {
                // a metallic shine sweeping across
                p = Sp(FX_SPEED16, FXP_E_WHITE, a->x - TO_FX(16), a->y - TO_FX(a->height - k * 5), 14 + k * 2);
                if (p) { p->vx = 520; p->angle = 0; }
                Stretch(p);
            }
        break;
    case MOVE_WITHDRAW:
        MRings(a, FXP_E_WATER, 2);
        for (k = 0; k < 8; k++)
        {
            p = Fly(FX_FOAM8, (k & 1) ? FXP_E_FOAM : FXP_E_WATER, a->x, cy, k * 32, 300, 18, FALSE);
            Accel(p, 0, 8);
        }
        break;
    case MOVE_DEFENSE_CURL:
        Coils(a, FXP_E_WHITE);
        MGlint(a, FXP_E_WHITE);
        break;
    case MOVE_ACID_ARMOR:
        MRise(a, FX_GOO16, FXP_E_POISON, FXP_E_POISON, 6, 60, 20);
        ImpactGlow(FXP_E_POISON, a->x, cy, 8, 1, 16);
        break;
    case MOVE_SHARPEN:
        MOrbit(a, FX_TRI16, FXP_E_STEEL, 3, 16, 10, -1, 24);
        MGlint(a, FXP_E_WHITE);
        break;
    case MOVE_REST:
    case MOVE_SLEEP_TALK:
        MRise(a, FX_Z8, FXP_SLEEP, FXP_E_SKY, 4, -90, 34);
        break;
    case MOVE_SUBSTITUTE:
        p = Sp(FX_DOLL16, FXP_NORMAL, a->x + TO_FX(14), a->y - TO_FX(6), 40);
        Scale(p, 8, 1);
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_PUFF16, FXP_E_SMOKE, a->x + TO_FX(14), a->y - TO_FX(6), k * 64 + 32, 120, 18, FALSE);
            Blend(p);
        }
        break;
    case MOVE_CURSE:
    case MOVE_DESTINY_BOND:
    case MOVE_GRUDGE:
        MOrbit(a, FX_WISP16, FXP_E_GHOST, 4, 20, 7, -1, 34);
        p = Sp(FX_SKULL16, FXP_E_GHOST, a->x, a->y - TO_FX(a->height + 6), 30);
        Scale(p, 10, 1);
        RtFx_Dim(4, 16);
        break;
    case MOVE_WISH:
    case MOVE_ASSIST:
        if (move == MOVE_WISH)
            MRain(a, FX_STAR16B, FXP_E_GOLD, 3, 16);
        MOrbit(a, FX_STAR16B, FXP_E_GOLD, 5, 20, 8, -1, 30);
        break;
    case MOVE_MORNING_SUN:
        MRain(a, FX_SUNRAY16, FXP_E_SOLAR, 6, 16);
        ImpactGlow(FXP_E_SOLAR, a->x, cy, 8, 1, 18);
        break;
    case MOVE_MOONLIGHT:
        p = Sp(FX_MOON16, FXP_E_SKY, a->x, a->y - TO_FX(a->height + 14), 34);
        Scale(p, 18, 0);
        MRise(a, FX_SPARKLE8, FXP_E_SKY, FXP_E_WHITE, 8, -100, 20);
        break;
    case MOVE_CHARGE:
        MOrbit(a, FX_CRACKLE16, FXP_E_ELEC, 3, 16, 9, 0, 44);
        MOrbit(a, FX_CRACKLE16, FXP_E_BOLT, 2, 10, -11, 0, 36);
        p = Sp(FX_GLOW32, FXP_E_ELEC, a->x, cy, 40);
        Scale(p, 9, 0);
        Blend(p);
        for (k = 0; k < 4; k++)
        {
            p = Fly(FX_LBOLT16, FXP_E_ELEC, a->x, cy, k * 64 + 32, 260, 12, FALSE);
            Stretch(p);
        }
        break;
    case MOVE_STOCKPILE:
        MOrbit(a, FX_MOTE8, FXP_E_GOLD, 8, 26, 9, -3, 30);
        p = Sp(FX_GLOW32, FXP_E_GOLD, a->x, cy, 30);
        Scale(p, 4, 1);
        Blend(p);
        break;
    case MOVE_RECYCLE:
        MOrbit(a, FX_ARC32, FXP_E_HEAL, 3, 18, 10, 0, 42);
        MRise(a, FX_SPARKLE8, FXP_E_HEAL, FXP_E_WHITE, 4, -80, 24);
        break;
    case MOVE_BELLY_DRUM:
        MRings(a, FXP_E_RED, 3);
        for (k = 0; k < 3; k++)
        {
            p = Sp(FX_BOOM32, FXP_E_WHITE, a->x + TO_FX(k * 8 - 8), cy, 8 + k * 6);
            Scale(p, 10 + k * 4, 1);
        }
        RtFx_Shake(2, 14);
        break;
    case MOVE_HOWL:
        MRings(a, FXP_E_WHITE, 3);
        MFlameAura(a, FXP_E_RED);
        for (k = 0; k < 8; k++)
        {
            p = Fly(FX_SPEED16, FXP_E_WHITE, a->x, a->y - TO_FX(a->height), 128 + k * 16, 420, 12, FALSE);
            Stretch(p);
        }
        RtFx_Shake(1, 12);
        break;
    case MOVE_TAIL_GLOW:
        p = Sp(FX_GLOW32, FXP_E_GOLD, a->x - TO_FX(8), a->y - TO_FX(6), 40);
        Scale(p, 12, 0);
        Blend(p);
        p = Sp(FX_FLARE32, FXP_E_GOLD, a->x - TO_FX(8), a->y - TO_FX(6), 30);
        Spin(p, 6);
        MRise(a, FX_SPARKLE8, FXP_E_GOLD, FXP_E_PINK, 8, -100, 26);
        break;
    case MOVE_FOLLOW_ME:
        p = Sp(FX_EXCLAIM8, FXP_GOLD, a->x, a->y - TO_FX(a->height + 10), 36);
        Scale(p, 26, 0);
        MRain(a, FX_SUNRAY16, FXP_E_GOLD, 5, 20);
        MRings(a, FXP_E_GOLD, 2);
        break;
    case MOVE_HELPING_HAND:
        p = Sp(FX_PALM16, FXP_GOLD, a->x - TO_FX(16), a->y - TO_FX(a->height + 8), 24);
        if (p) { p->vx = 200; p->angle = 0; }
        Drag(p, 13);
        Scale(p, 22, 0);
        p = Sp(FX_PALM16, FXP_GOLD, a->x + TO_FX(16), a->y - TO_FX(a->height + 8), 24);
        if (p) { p->vx = -200; p->flags |= PF_HFLIP; }
        Drag(p, 13);
        Scale(p, 22, 0);
        MRise(a, FX_SPARKLE8, FXP_E_GOLD, FXP_E_WHITE, 6, -120, 22);
        break;
    case MOVE_SNATCH:
        for (k = 0; k < 2; k++)
        {
            p = Sp(FX_PALM16, FXP_E_DARK, a->x, cy - TO_FX(k * 10), 18);
            if (p) { p->vx = RtCos(a->aim) * 2; p->vy = RtSin(a->aim); if (RtCos(a->aim) < 0) p->flags |= PF_HFLIP; }
            Scale(p, 22, 0);
        }
        MOrbit(a, FX_WISP16, FXP_E_GHOST, 3, 18, 8, 0, 30);
        break;
    case MOVE_IMPRISON:
        MOrbit(a, FX_CROSS16, FXP_PSYCHIC, 4, 20, 6, 0, 40);
        MRings(a, FXP_E_PSY, 2);
        break;
    case MOVE_MIRROR_MOVE:
    case MOVE_METRONOME:
    case MOVE_SLACK_OFF:
        if (move == MOVE_SLACK_OFF)
            MRise(a, FX_Z8, FXP_SLEEP, FXP_SLEEP, 3, -70, 34);
        else
        {
            p = Sp(FX_VEIL16, FXP_E_SKY, a->x + TO_FX(14), cy, 30);
            if (p) p->frameRate = 8;
            MGlint(a, FXP_E_WHITE);
            if (move == MOVE_METRONOME)
                MOrbit(a, FX_NOTE16, FXP_GOLD, 3, 18, 9, 0, 34);
        }
        break;
    case MOVE_SOFT_BOILED:
        p = Sp(FX_EGG16, FXP_E_WHITE, a->x, a->y - TO_FX(a->height + 10), 34);
        Scale(p, 22, 0);
        FadeOut(p);
        break;
    case MOVE_REFRESH:
        for (k = 0; k < 8; k++)
        {
            p = Sp(FX_BUBBLE16, FXP_WATER, a->x + TO_FX((Random() % 25) - 12), a->y - TO_FX(Random() % (a->height + 1)), 26 + (k & 3) * 3);
            if (p) { p->vy = -140 - (Random() & 63); p->frameRate = 6; }
        }
        break;
    case MOVE_SPLASH:
        for (k = 0; k < 8; k++)
        {
            p = Fly(FX_FOAM8, (k & 1) ? FXP_E_FOAM : FXP_E_WATER, a->x, a->y, 160 + k * 8, 400, 20, FALSE);
            Accel(p, 0, 12);
        }
        break;
    case MOVE_MIST:
        for (k = 0; k < 10; k++)
        {
            p = Fly(FX_PUFF32, (k & 1) ? FXP_E_FOAM : FXP_E_WHITE, a->x, cy, k * 26, 380 + (k & 3) * 60, 56, TRUE);
            Blend(p);
            Drag(p, 14);
            Scale(p, 20 + (k & 1) * 4, 0);
            if (p) { p->frameRate = 12; }
        }
        MRise(a, FX_SPARKLE8, FXP_E_ICE, FXP_E_WHITE, 6, -60, 30);
        break;
    case MOVE_COSMIC_POWER:
        MOrbit(a, FX_STAR16B, FXP_E_PSY, 5, 20, 8, -1, 36);
        p = Sp(FX_MOON16, FXP_E_SKY, a->x, a->y - TO_FX(a->height + 14), 34);
        Scale(p, 16, 0);
        ImpactGlow(FXP_E_PSY, a->x, cy, 8, 0, 24);
        break;
    case MOVE_CAMOUFLAGE:
    case MOVE_CONVERSION:
    case MOVE_CONVERSION_2:
        {
            static const u8 sCols[] = {FXP_E_FIRE, FXP_E_WATER, FXP_E_GRASS, FXP_E_ELEC, FXP_E_PSY, FXP_E_ICE};
            for (k = 0; k < 6; k++)
                MOrbit(a, FX_ORB16, sCols[k], 1, 20, move == MOVE_CONVERSION_2 ? -8 : 8, -1, 30 + k * 2);
        }
        break;
    }
    switch (v->style)
    {
    case VS_SHIELD:
        if (move == MOVE_DETECT)
        {
            // a glint in the eye and a flash of foresight: no dome
            p = Sp(FX_EYE16B, FXP_E_GOLD, a->x, a->y - TO_FX(a->height + 12), 34);
            Scale(p, 30, 0);
            if (p) { p->frameRate = 0; p->frame = 1; }
            FadeOut(p);
            MGlint(a, FXP_E_GOLD);
            for (k = 0; k < 8; k++)
            {
                p = Fly(FX_SPEED16, FXP_E_GOLD, a->x, cy, k * 32, 380, 12, FALSE);
                Stretch(p);
            }
            break;
        }
        if (move == MOVE_COUNTER)
        {
            MFlameAura(a, FXP_E_RED);
            MGlint(a, FXP_E_WHITE);
            for (k = 0; k < 6; k++)
            {
                p = Fly(FX_SPEED16, FXP_E_FIGHT, a->x, cy, k * 43, 400, 12, FALSE);
                Stretch(p);
            }
            break;
        }
        if (move == MOVE_ENDURE)
        {
            MFlameAura(a, FXP_E_HOT);
            MFlameAura(a, FXP_E_GOLD);
            p = Sp(FX_GLOW32, FXP_E_HOT, a->x, cy, 34);
            Scale(p, 10, 0);
            Blend(p);
            RtFx_Shake(1, 8);
            break;
        }
        if (move == MOVE_MIRROR_COAT)
        {
            for (k = -1; k <= 1; k++)
            {
                p = Sp(FX_VEIL16, FXP_E_PINK, a->x + TO_FX(k * 12), cy - TO_FX(k == 0 ? 4 : 0), 40);
                if (p) { p->frameRate = 6; RtFx_X(p)->flags |= PX_FADE; }
            }
            MGlint(a, FXP_E_WHITE);
            break;
        }
        if (move == MOVE_MAGIC_COAT)
        {
            for (k = -1; k <= 1; k++)
            {
                p = Glue(a, FX_VEIL16, FXP_E_GOLD, k * 14, a->height / 2 + (k ? 8 : 4), 40 - (k ? 4 : 0));
                if (p) { p->frameRate = 7; p->prio = k ? 4 : 1; RtFx_X(p)->flags |= PX_FADE; }
            }
            MRings(a, FXP_E_PSY, 2);
            break;
        }
        p = Glue(a, FX_HEX32, v->pal, 0, a->height / 2, 34);
        Scale(p, 8, 2);
        if (p) { p->dscale = 2; p->frameRate = 4; }
        p = Glue(a, FX_HEX32, v->pal2, 0, a->height / 2, 32);
        Scale(p, 24, 0);
        if (p) RtFx_X(p)->flags |= PX_FADE;
        break;
    case VS_WALL:
        {
            // a curved screen of soft glass panes in front of the user, its sheen sweeping across
            for (k = -1; k <= 1; k++)
            {
                // three glass panes standing around the user, the middle one in front
                p = Sp(FX_VEIL16, v->pal, a->x + TO_FX(k * 17), cy + TO_FX(k == 0 ? 5 : -1), 46 - (k ? 4 : 0));
                if (p) { p->frameRate = 8; p->prio = k == 0 ? 1 : 4; RtFx_X(p)->flags |= PX_FADE; }
            }
            p = Sp(FX_GLOW32, v->pal, a->x, cy, 30);
            Scale(p, 14, 0);
            Blend(p);
            MRise(a, FX_SPARKLE8, v->pal, FXP_E_WHITE, 5, -80, 24);
        }
        break;
    case VS_HEAL:
        {
            // the glow takes the colour of the source of the healing
            u8 hp = FXP_E_HEAL;
            switch (move)
            {
            case MOVE_MORNING_SUN: case MOVE_WISH: hp = FXP_E_GOLD; break;
            case MOVE_MOONLIGHT: case MOVE_REST: case MOVE_SLACK_OFF: hp = FXP_E_SKY; break;
            case MOVE_MILK_DRINK: case MOVE_SOFT_BOILED: case MOVE_SWALLOW: hp = FXP_E_WHITE; break;
            case MOVE_REFRESH: hp = FXP_E_WATER; break;
            }
            ImpactGlow(hp, a->x, cy, 6, 1, 20);
        }
        for (k = 0; k < 10; k++)
        {
            p = Sp((k & 1) ? FX_PLUS8 : FX_SPARKLE8, (k & 1) ? FXP_HEAL : FXP_E_HEAL, a->x + TO_FX((Random() % 29) - 14), a->y - TO_FX(Random() % (a->height + 1)), 24 + (k & 3) * 3);
            if (p) p->vy = -150 - (Random() & 63);
        }
        break;
    case VS_AURA:
    default:
        for (k = 0; k < 6; k++)
        {
            u8 ang = k * 42 + (Random() & 15);
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
    if ((a->act.timer & 3) == 1)
        AddGhost(a, 0, a->x, a->y, 10);
    p = Sp(FX_SPEED16, FXP_E_WHITE, a->x - RtCos(a->act.angle) * a->radius, CenterY(a) + TO_FX((Random() % 13) - 6), 8);
    if (p) { p->angle = a->act.angle; p->flags |= PF_SCALE; p->scale = 16; RtFx_X(p)->flags |= PX_STRETCH; p->vx = RtCos(a->act.angle) / 2; p->vy = RtSin(a->act.angle) / 2; }
    vi = v->impact == VI_DEFAULT ? TypeImpact(RTM(move).type) : v->impact;
    if ((a->act.timer & 3) == 0 && vi != VI_ELEC)
    {
        p = Sp(FX_PUFF16, vi == VI_WATER ? FXP_E_FOAM : FXP_E_DUST, a->x, a->y, 16);
        Blend(p);
    }
    if (vi == VI_ELEC && (a->act.timer & 3) == 0)
    {
        p = Sp(FX_GLOW32, FXP_E_ELEC, a->x, CenterY(a), 6);
        Scale(p, 12, 0);
        Blend(p);
    }
    if (vi == VI_DRAGON)
    {
        p = Sp(FX_FIRE16, FXP_E_DRAGON, a->x + TO_FX((Random() % 17) - 8), CenterY(a) + TO_FX((Random() % 13) - 6), 14);
        Accel(p, 0, -3);
        Fade(p, FXP_E_GHOST, 5, 0xFF, 0);
    }
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
        if (a->act.timer & 1)
        {
            p = Sp(FX_JET16, FXP_E_WATER, a->x - RtCos(a->act.angle) * a->radius, CenterY(a), 10);
            if (p) p->angle = a->act.angle;
            Scale(p, 20, 0);
        }
        break;
    case VI_DRAGON:
    case VI_PSY:
    case VI_GHOST:
    case VI_STEEL:
    case VI_ICE:
    case VI_BUG:
        p = Sp(FX_MOTE8, v->pal, a->x + TO_FX((Random() % 17) - 8), CenterY(a) + TO_FX((Random() % 13) - 6), 14);
        FadeOut(p);
        break;
    case VI_DARK:
        if (a->act.timer & 1)
        {
            p = Sp(FX_WISP16, FXP_E_DARK, a->x + TO_FX((Random() % 13) - 6), CenterY(a) + TO_FX((Random() % 13) - 6), 16);
            Accel(p, 0, -2);
            FadeOut(p);
        }
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
            pt = Fly(FX_FOAM8, v->pal == FXP_E_MUDW ? FXP_E_DUST : FXP_E_FOAM, p->x + RtCos(perp) * k, p->y + RtSin(perp) * k - TO_FX(18), 192 + (s8)(Random() % 41 - 20), 300, 16, FALSE);
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
    case VS_QUAKE:
        if ((p->age & 3) == 0)
        {
            pt = Sp(FX_CRACK32, FXP_E_DARK, p->x, p->y, 50);
            if (pt) { pt->flags |= PF_GROUND; pt->frame = (p->age >> 2) & 1; pt->frameRate = 0; pt->angle = p->angle; }
        }
        if ((p->age & 1) == 0)
        {
            k = (Random() % (p->len * 2 + 1)) - p->len;
            pt = Fly(FX_DEBRIS8, FXP_E_ROCK, p->x + RtCos(perp) * k, p->y + RtSin(perp) * k / 2, 192 + (s8)(Random() % 33 - 16), 400, 20, FALSE);
            Accel(pt, 0, 12);
            if (pt) pt->frameRate = 3;
        }
        RtFx_Shake(1, 4);
        return TRUE;
    }
    return FALSE;
}

// ambient look of major status conditions on the Pokemon itself
void RtVfx_StatusTick(struct RtActor *a)
{
    struct RtPart *p;
    s32 bx, by;
    if (a->hidden || a->state == AST_GONE || a->state == AST_FAINT)
        return;
    bx = a->x + TO_FX((s32)(Random() % (a->radius + 1)) - a->radius / 2);
    by = a->y - TO_FX(Random() % (a->height + 1));
    sPrio = 1;
    switch (a->mstatus)
    {
    case MST_BRN:
        if (((gRt->frame + a->id * 5) & 11) == 0)
        {
            p = Sp(FX_EMBER8, FXP_E_HOT, bx, by, 18);
            Accel(p, 0, -3);
            Fade(p, FXP_E_FIRE, 10, FXP_E_SMOKE, 4);
        }
        break;
    case MST_PSN:
    case MST_TOX:
        if (((gRt->frame + a->id * 7) & (a->mstatus == MST_TOX ? 7 : 15)) == 0)
        {
            p = Sp(FX_MOTE8, FXP_E_POISON, bx, by, 22);
            if (p) p->vy = -90;
            FadeOut(p);
        }
        break;
    case MST_PAR:
        if (((gRt->frame + a->id * 3) % 24) == 0)
        {
            p = Sp(FX_CRACKLE16, (gRt->frame & 8) ? FXP_E_ELEC : FXP_E_BOLT, bx, by, 6);
            if (p) p->frameRate = 2;
        }
        break;
    case MST_FRZ:
        if (((gRt->frame + a->id * 3) & 15) == 0)
            Sp(FX_SPARKLE8, FXP_E_ICE, bx, by, 14);
        break;
    }
}

// Bide: the user braces, a golden aura flickering around it while it stores the damage
void RtVfx_BideTick(struct RtActor *a, u16 t)
{
    struct RtPart *p;
    sPrio = 1;
    if ((t & 3) == 0)
    {
        p = Sp(FX_FIRE16, (t & 4) ? FXP_E_GOLD : FXP_E_HOT, a->x + TO_FX((Random() % 21) - 10), a->y - TO_FX(Random() % (a->height / 2 + 2)), 16);
        if (p) { p->vy = -160; p->frameRate = 3; }
        FadeOut(p);
    }
    if ((t & 15) == 0)
    {
        p = Sp(FX_GLOW32, FXP_E_GOLD, a->x, CenterY(a), 14);
        Scale(p, 10, 0);
        Blend(p);
        RtFx_Shake(1, 3);
    }
}
