// Real-time battle arena props: Rock Smash rocks, Cut trees, Strength boulders and moving boxes,
// drawn with the real overworld object graphics. They block movement and shots, break when hit,
// and Pokémon using telekinetic or throwing moves (Psychic, Seismic Toss, Strength...) can pick
// one up and hurl it at the opponent.
#include "global.h"
#include "rt_internal.h"
#include "event_data.h"
#include "fieldmap.h"
#include "metatile_behavior.h"
#include "random.h"
#include "sound.h"
#include "constants/map_types.h"
#include "constants/moves.h"
#include "constants/songs.h"

#define PROP_LIFT_FRAMES 16

static const u8 sPropHp[] = {[PROP_ROCK] = 2, [PROP_TREE] = 3, [PROP_BOULDER] = 4, [PROP_BOX] = 2};
static const u8 sPropRadius[] = {[PROP_ROCK] = 7, [PROP_TREE] = 6, [PROP_BOULDER] = 8, [PROP_BOX] = 7};

u8 RtProps_ShapeOf(u8 kind)
{
    switch (kind)
    {
    case PROP_ROCK: return FX_PROP_ROCK;
    case PROP_TREE: return FX_PROP_TREE;
    case PROP_BOULDER: return FX_PROP_BOULDER;
    case PROP_BOX: return FX_PROP_BOX;
    }
    return FX_NONE;
}

u8 RtProps_PalOf(u8 kind)
{
    switch (kind)
    {
    case PROP_ROCK: return FXP_PROP_ROCK;
    case PROP_TREE: return FXP_PROP_TREE;
    case PROP_BOULDER: return FXP_PROP_BOULDER;
    case PROP_BOX: return FXP_PROP_BOX;
    }
    return FXP_WHITE;
}

static bool8 IsSolidProp(struct RtProp *p)
{
    return p->kind != PROP_NONE && p->state == PRS_REST;
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------
void RtProps_Place(void)
{
    u8 kinds[3], nKinds = 0, want, tries = 0, n = 0;
    s32 sx[2], sy[2], k;
    u16 playerMeta = MapGridGetMetatileIdAt(gSaveBlock1Ptr->pos.x + MAP_OFFSET, gSaveBlock1Ptr->pos.y + MAP_OFFSET);
    u8 behav = playerMeta < NUM_METATILES_TOTAL ? (GetMetatileAttributesById(playerMeta) & METATILE_ATTR_BEHAVIOR_MASK) : 0;

    memset(gRt->props, 0, sizeof(gRt->props));
    if (gRt->floorKind == 1 || gRt->isSafari) // on water / safari: nothing to put down
        return;
    switch (gMapHeader.mapType)
    {
    case MAP_TYPE_INDOOR:
    case MAP_TYPE_SECRET_BASE:
        kinds[nKinds++] = PROP_BOX;
        break;
    case MAP_TYPE_UNDERGROUND:
        kinds[nKinds++] = PROP_ROCK;
        kinds[nKinds++] = PROP_BOULDER;
        break;
    case MAP_TYPE_CITY:
    case MAP_TYPE_TOWN:
        kinds[nKinds++] = PROP_BOX;
        kinds[nKinds++] = PROP_ROCK;
        break;
    case MAP_TYPE_UNDERWATER:
        kinds[nKinds++] = PROP_ROCK;
        break;
    default:
        if (MetatileBehavior_IsSandOrDeepSand(behav) || MetatileBehavior_IsAshGrass(behav))
        {
            kinds[nKinds++] = PROP_ROCK;
            kinds[nKinds++] = PROP_BOULDER;
        }
        else
        {
            kinds[nKinds++] = PROP_TREE;
            kinds[nKinds++] = PROP_ROCK;
            kinds[nKinds++] = PROP_BOULDER;
        }
        break;
    }
    RtArena_FindSpawn(RT_SIDE_PLAYER, 0, &sx[0], &sy[0]);
    RtArena_FindSpawn(RT_SIDE_ENEMY, 0, &sx[1], &sy[1]);
    want = 3 + Random() % 3;
    while (n < want && tries < 80)
    {
        s32 cw = (gRt->playX1 - gRt->playX0) / 16, ch = (gRt->playY1 - gRt->playY0) / 16;
        s32 cx, cy, px, py;
        bool8 ok = TRUE;
        tries++;
        if (cw < 4 || ch < 4)
            break;
        cx = 1 + Random() % (cw - 2);
        cy = 1 + Random() % (ch - 2);
        px = gRt->playX0 + cx * 16 + 8;
        py = gRt->playY0 + cy * 16 + 12;
        for (k = 0; k < 2 && ok; k++)
        {
            s32 dx = px - FROM_FX(sx[k]), dy = py - FROM_FX(sy[k]);
            if (dx * dx + dy * dy < 40 * 40)
                ok = FALSE;
        }
        for (k = 0; k < n && ok; k++)
        {
            s32 dx = px - FROM_FX(gRt->props[k].x), dy = py - FROM_FX(gRt->props[k].y);
            if (dx * dx + dy * dy < 34 * 34)
                ok = FALSE;
        }
        if (!ok || RtArena_PointSolid(px, py))
            continue;
        gRt->props[n].kind = kinds[Random() % nKinds];
        gRt->props[n].state = PRS_REST;
        gRt->props[n].hp = sPropHp[gRt->props[n].kind];
        gRt->props[n].x = TO_FX(px);
        gRt->props[n].y = TO_FX(py);
        n++;
    }
}

// ---------------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------------
bool8 RtProps_CircleBlocked(s32 x, s32 y, s32 r)
{
    s32 i;
    if (gRt == NULL)
        return FALSE;
    for (i = 0; i < RT_MAX_PROPS; i++)
    {
        struct RtProp *p = &gRt->props[i];
        s32 dx, dy, rr;
        if (!IsSolidProp(p))
            continue;
        dx = x - FROM_FX(p->x);
        dy = (y - FROM_FX(p->y)) * 2;
        rr = r + sPropRadius[p->kind];
        if (dx * dx + dy * dy < rr * rr)
            return TRUE;
    }
    return FALSE;
}

void RtProps_PushActor(struct RtActor *a)
{
    s32 i;
    for (i = 0; i < RT_MAX_PROPS; i++)
    {
        struct RtProp *p = &gRt->props[i];
        s32 dx, dy, d, rr;
        if (!IsSolidProp(p))
            continue;
        dx = FROM_FX(a->x - p->x);
        dy = FROM_FX(a->y - p->y) * 2;
        rr = (a->radius > 4 ? a->radius - 2 : a->radius) + sPropRadius[p->kind];
        if (dx * dx + dy * dy >= rr * rr)
            continue;
        d = RtDist(dx, dy);
        if (d == 0)
        {
            dx = 1;
            d = 1;
        }
        a->x += dx * (rr - d + 1) * 256 / d;
        a->y += (dy / 2) * (rr - d + 1) * 256 / d;
    }
}

void RtProps_Debris(u8 kind, s32 x, s32 y)
{
    switch (kind)
    {
    case PROP_ROCK:
    case PROP_BOULDER:
        RtFx_Burst(FX_ROCK16, FXP_ROCK, x, y - TO_FX(6), kind == PROP_BOULDER ? 7 : 5, 3, 18);
        RtFx_Burst(FX_SMOKE16, FXP_SMOKE, x, y - TO_FX(4), 3, 1, 18);
        PlaySE(SE_M_ROCK_THROW);
        break;
    case PROP_TREE:
        RtFx_Burst(FX_LEAF16, FXP_GRASS, x, y - TO_FX(8), 6, 2, 20);
        RtFx_Burst(FX_ROCK16, FXP_MUD, x, y - TO_FX(4), 3, 2, 16);
        PlaySE(SE_M_CUT);
        break;
    case PROP_BOX:
        RtFx_Burst(FX_ROCK16, FXP_MUD, x, y - TO_FX(6), 6, 3, 18);
        RtFx_Burst(FX_SMOKE16, FXP_SMOKE, x, y - TO_FX(4), 3, 1, 18);
        PlaySE(SE_M_STRENGTH);
        break;
    }
}

static void BreakProp(struct RtProp *p)
{
    if (p->kind == PROP_ROCK || p->kind == PROP_TREE)
    {
        // the real Rock Smash / Cut animation frames
        p->state = PRS_BREAKING;
        p->timer = 0;
    }
    else
    {
        p->kind = PROP_NONE;
    }
    RtProps_Debris(p->kind == PROP_NONE ? PROP_BOX : p->kind, p->x, p->y);
}

// Damage any prop overlapping the circle. Returns TRUE if one was hit.
bool8 RtProps_HitAt(s32 fx, s32 fy, s32 r, u8 dmg)
{
    s32 i;
    bool8 hit = RtArena_HitCells(fx, fy + TO_FX(6), r + 1, dmg);
    for (i = 0; i < RT_MAX_PROPS; i++)
    {
        struct RtProp *p = &gRt->props[i];
        s32 dx, dy, rr;
        if (!IsSolidProp(p))
            continue;
        dx = FROM_FX(fx - p->x);
        dy = FROM_FX(fy - (p->y - TO_FX(6))) * 2;
        rr = r + sPropRadius[p->kind] + 2;
        if (dx * dx + dy * dy >= rr * rr)
            continue;
        hit = TRUE;
        if (p->hurtCd)
            continue;
        p->hurtCd = 12;
        RtFx_Spawn(FX_SPARK8, FXP_WHITE, p->x, p->y - TO_FX(8), 8);
        if (p->hp <= dmg)
        {
            p->hp = 0;
            BreakProp(p);
        }
        else
        {
            p->hp -= dmg;
            RtFx_Shake(1, 3);
        }
    }
    return hit;
}

// ---------------------------------------------------------------------------
// Picking up and throwing
// ---------------------------------------------------------------------------
static bool8 IsThrowMove(u16 move, bool8 *telekinetic)
{
    switch (move)
    {
    case MOVE_PSYCHIC:
    case MOVE_CONFUSION:
    case MOVE_EXTRASENSORY:
        *telekinetic = TRUE;
        return TRUE;
    case MOVE_SEISMIC_TOSS:
    case MOVE_STRENGTH:
    case MOVE_VITAL_THROW:
    case MOVE_ROCK_THROW:
    case MOVE_ANCIENT_POWER:
        *telekinetic = FALSE;
        return TRUE;
    }
    return FALSE;
}

bool8 RtProps_TryGrab(struct RtActor *a, u16 move)
{
    bool8 tele = FALSE;
    s32 i, best = -1, bestD = 0x7FFFFFFF;
    if (!IsThrowMove(move, &tele))
        return FALSE;
    for (i = 0; i < RT_MAX_PROPS; i++)
    {
        struct RtProp *p = &gRt->props[i];
        s32 dx, dy, d, reach;
        if (!IsSolidProp(p) || p->kind == PROP_TREE) // trees are rooted
            continue;
        if ((move == MOVE_ROCK_THROW || move == MOVE_ANCIENT_POWER) && p->kind == PROP_BOX)
            continue;
        dx = FROM_FX(p->x - a->x);
        dy = FROM_FX(p->y - a->y);
        d = RtDist(dx, dy * 2);
        reach = tele ? 110 : a->radius + 26;
        if (d > reach)
            continue;
        if (!tele)
        {
            // must be roughly in front
            s8 diff = (s8)(RtAtan2(dx, dy) - a->act.angle);
            if (diff > 56 || diff < -56)
                continue;
        }
        if (d < bestD)
        {
            bestD = d;
            best = i;
        }
    }
    if (best < 0)
        return FALSE;
    gRt->props[best].state = PRS_LIFTED;
    gRt->props[best].owner = a->id;
    gRt->props[best].move = move;
    gRt->props[best].timer = 0;
    RtFx_Burst(tele ? FX_GLOW16 : FX_SMOKE16, tele ? FXP_PSYCHIC : FXP_SMOKE, gRt->props[best].x, gRt->props[best].y - TO_FX(6), 4, 1, 14);
    PlaySE(tele ? SE_M_PSYBEAM2 : SE_M_STRENGTH);
    return TRUE;
}

void RtProps_Update(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_PROPS; i++)
    {
        struct RtProp *p = &gRt->props[i];
        if (p->kind == PROP_NONE)
            continue;
        if (p->hurtCd)
            p->hurtCd--;
        switch (p->state)
        {
        case PRS_BREAKING:
            if (++p->timer >= 18)
                p->kind = PROP_NONE;
            break;
        case PRS_LIFTED:
            {
                struct RtActor *o = &gRt->actors[p->owner];
                s32 tx, ty, tz;
                p->timer++;
                if (!o->active || o->state == AST_GONE || o->state == AST_FAINT || !RtActor_IsAlive(o))
                {
                    // dropped
                    p->z = 0;
                    BreakProp(p);
                    break;
                }
                // float to a spot above the user's head
                tx = o->x;
                ty = o->y + TO_FX(1);
                tz = o->height + 8 + ((p->timer >> 2) & 1);
                p->x += (tx - p->x) / 4;
                p->y += (ty - p->y) / 4;
                p->z += (tz - p->z + 2) / 3;
                if ((p->timer & 3) == 0)
                {
                    struct RtPart *g = RtFx_Spawn(FX_SPARK8, FXP_PSYCHIC, p->x, p->y - TO_FX(p->z), 8);
                    if (g)
                        g->vy = 64;
                }
                if (p->timer >= PROP_LIFT_FRAMES)
                {
                    RtMove_ThrowProp(o, p->move, p->kind, p->x, p->y, p->z);
                    p->kind = PROP_NONE;
                }
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void RtProps_DrawOne(u8 i)
{
    struct RtProp *p = &gRt->props[i];
    const struct RtFxShape *s;
    struct OamData *o;
    u16 *oam;
    u8 shape = RtProps_ShapeOf(p->kind), palSlot, frame = 0;
    s32 sx, sy;
    if (shape == FX_NONE)
        return;
    s = &gRtFxShapes[shape];
    if (p->state == PRS_BREAKING)
    {
        frame = 1 + p->timer / 9;
        if (frame >= s->frames)
            frame = s->frames - 1;
    }
    else if (p->hurtCd > 8 && (p->hurtCd & 1))
    {
        return; // hit blink
    }
    sx = FROM_FX(p->x) - gRt->camX - 8;
    sy = FROM_FX(p->y) - gRt->camY - 14 - p->z;
    if (sx < -16 || sx > DISPLAY_WIDTH || sy < -16 || sy > DISPLAY_HEIGHT)
        return;
    palSlot = RtFx_GetPalSlot(RtProps_PalOf(p->kind));
    o = RtOam_Next(p->state == PRS_LIFTED ? 1 : 2);
    if (o)
    {
        oam = (u16 *)o;
        oam[0] = (sy & 0xFF) | (0 << 14);
        oam[1] = (sx & 0x1FF) | (1 << 14);
        oam[2] = (RT_FX_TILE_BASE + s->tile + frame * s->tilesPerFrame) | (2 << 10) | (palSlot << 12);
    }
    // shadow on the ground
    if (p->state != PRS_BREAKING)
    {
        const struct RtFxShape *sh = &gRtFxShapes[FX_SHADOW16];
        s32 x = FROM_FX(p->x) - gRt->camX - 8, y = FROM_FX(p->y) - gRt->camY - 3;
        o = RtOam_Next(3);
        if (o)
        {
            oam = (u16 *)o;
            oam[0] = (y & 0xFF) | ((sh->shapeSize >> 2) << 14);
            oam[1] = (x & 0x1FF) | ((sh->shapeSize & 3) << 14);
            oam[2] = (RT_FX_TILE_BASE + sh->tile) | (2 << 10) | (4 << 12);
        }
    }
}
