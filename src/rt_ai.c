// Real-time battle AI.
#include "global.h"
#include "rt_internal.h"
#include "battle.h"
#include "battle_setup.h"
#include "data.h"
#include "item.h"
#include "pokemon.h"
#include "random.h"
#include "sound.h"
#include "string_util.h"
#include "constants/abilities.h"
#include "constants/battle.h"
#include "constants/battle_ai.h"
#include "constants/battle_move_effects.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/trainers.h"

static EWRAM_DATA u16 sTrainerItems[MAX_TRAINER_ITEMS] = {0};
static EWRAM_DATA bool8 sTrainerItemsLoaded = FALSE;
static EWRAM_DATA u16 sTrainerItemCd = 0;

static const u8 sText_TrainerUsed[] = _("{STR_VAR_1} used {STR_VAR_2}!");

void RtAi_Init(struct RtActor *a)
{
    u8 skill;
    if (gRt->isTrainer && a->side == RT_SIDE_ENEMY)
    {
        skill = 5;
        if (gBattleTypeFlags & (BATTLE_TYPE_FRONTIER | BATTLE_TYPE_TRAINER_HILL))
            skill = 8;
        else if (gTrainerBattleOpponent_A != TRAINER_SECRET_BASE)
        {
            u8 cls = gTrainers[gTrainerBattleOpponent_A].trainerClass;
            u32 flags = gTrainers[gTrainerBattleOpponent_A].aiFlags;
            if (cls == TRAINER_CLASS_LEADER) skill = 8;
            else if (cls == TRAINER_CLASS_ELITE_FOUR) skill = 9;
            else if (cls == TRAINER_CLASS_CHAMPION) skill = 10;
            else if (cls == TRAINER_CLASS_AQUA_LEADER || cls == TRAINER_CLASS_MAGMA_LEADER) skill = 8;
            else if (cls == TRAINER_CLASS_AQUA_ADMIN || cls == TRAINER_CLASS_MAGMA_ADMIN) skill = 7;
            else if (flags & AI_SCRIPT_TRY_TO_FAINT) skill = 6;
            if (a->level < 12 && skill > 4)
                skill -= 2;
        }
    }
    else if (a->side == RT_SIDE_ENEMY)
    {
        skill = 1 + a->level / 18;
        if (skill > 5) skill = 5;
        if (gBattleTypeFlags & (BATTLE_TYPE_LEGENDARY | BATTLE_TYPE_REGI | BATTLE_TYPE_GROUDON | BATTLE_TYPE_KYOGRE | BATTLE_TYPE_RAYQUAZA))
            skill = 8;
    }
    else
    {
        skill = 6;
    }
    // difficulty (OPTIONS menu): easy/hard shift how sharp the opponents are
    if (a->side == RT_SIDE_ENEMY)
    {
        if (gSaveBlock2Ptr->optionsRtDifficulty == 1)
        {
            skill = skill > 3 ? skill - 3 : 1;
            if (skill > 4) skill = 4;
        }
        else if (gSaveBlock2Ptr->optionsRtDifficulty == 2)
        {
            skill += 3;
            if (skill < 5) skill = 5;
            if (skill > 10) skill = 10;
        }
    }
    a->aiSkill = skill;
    a->aiThinkCd = 30 + Random() % 30;
    a->aiStrafe = (Random() & 1) ? 1 : -1;
    a->aiTarget = 0xFF;
}

static struct RtActor *PickTarget(struct RtActor *a)
{
    struct RtActor *t = NULL;
    if (a->aiTarget < RT_MAX_ACTORS)
    {
        t = &gRt->actors[a->aiTarget];
        if (t->active && t->side != a->side && RtActor_IsAlive(t) && t->state != AST_GONE)
        {
            if (t->followMe || (Random() % 64) != 0)
                return t;
        }
    }
    t = Rt_NearestFoe(a);
    // in doubles sometimes switch to the other foe
    if (t && (Random() % 4) == 0)
    {
        s32 i;
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *b = &gRt->actors[i];
            if (b != t && b->active && b->side != a->side && RtActor_IsAlive(b) && b->state != AST_GONE && !b->invuln)
            {
                t = b;
                break;
            }
        }
    }
    a->aiTarget = t ? t->id : 0xFF;
    return t;
}

// returns TRUE and the threat direction if a hostile hitbox will reach us soon
static bool8 FindThreat(struct RtActor *a, u8 *awayAngle, u8 *urgency)
{
    s32 i;
    s32 ax = FROM_FX(a->x), ay = FROM_FX(a->y);
    for (i = 0; i < RT_MAX_PROJ; i++)
    {
        struct RtProj *p = &gRt->proj[i];
        s32 px, py, vx, vy, t, cx, cy, dist;
        if (p->kind == PK_NONE || p->side == a->side)
            continue;
        px = FROM_FX(p->x);
        py = FROM_FX(p->y);
        switch (p->kind)
        {
        case PK_BULLET:
        case PK_WAVE:
            if (p->delay)
                continue;
            vx = p->vx;
            vy = p->vy;
            if (vx == 0 && vy == 0)
                continue;
            // time of closest approach (frames) using Q8 velocities
            {
                s32 dx = ax - px, dy = ay - py;
                s32 vv = (vx * vx + vy * vy) / 256;
                if (vv == 0)
                    continue;
                t = (dx * vx + dy * vy) / vv;
                if (t < 0 || t > 26)
                    continue;
                cx = px + vx * t / 256;
                cy = py + vy * t / 256;
                dist = RtDist(ax - cx, ay - cy);
                if (dist <= p->radius + a->radius + (p->kind == PK_WAVE ? p->len : 4))
                {
                    // dodge perpendicular to the projectile path, to the side we're already on
                    u8 dir = RtAtan2(vx, vy);
                    s32 side = (-(dy) * vx + dx * vy);
                    *awayAngle = dir + (side >= 0 ? 64 : -64);
                    *urgency = 26 - t;
                    return TRUE;
                }
            }
            break;
        case PK_STRIKE:
        case PK_FUTURE:
        case PK_AREA:
            dist = RtDist(ax - px, (ay - py) * 2);
            if (dist <= (p->kind == PK_AREA ? p->maxRadius : p->radius) + a->radius + 6 && (p->kind == PK_AREA || p->delay < 40))
            {
                *awayAngle = RtAtan2(ax - px, ay - py);
                *urgency = p->kind == PK_AREA ? 20 : (40 - p->delay) / 2;
                return TRUE;
            }
            break;
        case PK_BEAM:
            {
                s32 along;
                s32 dx = ax - px, dy = ay - py;
                along = (dx * RtCos(p->angle) + dy * RtSin(p->angle)) / 256;
                if (along > 0 && along < 260)
                {
                    s32 perp = (-dx * RtSin(p->angle) + dy * RtCos(p->angle)) / 256;
                    if ((perp < 0 ? -perp : perp) < p->radius + a->radius + 10)
                    {
                        *awayAngle = p->angle + (perp >= 0 ? 64 : -64);
                        *urgency = 20;
                        return TRUE;
                    }
                }
            }
            break;
        }
    }
    // melee threat: a foe close by and casting
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *b = &gRt->actors[i];
        if (!b->active || b->side == a->side || b->state != AST_CAST)
            continue;
        if (RtDist(FROM_FX(b->x) - ax, FROM_FX(b->y) - ay) < 48)
        {
            *awayAngle = RtAtan2(ax - FROM_FX(b->x), ay - FROM_FX(b->y));
            *urgency = 10;
            return TRUE;
        }
    }
    return FALSE;
}

static u16 ScoreMove(struct RtActor *a, struct RtActor *t, u8 slot, s32 dist)
{
    u16 move = a->moves[slot];
    const struct BattleMove *bm = &gBattleMoves[move];
    s32 score = 0;
    u8 range = RtMove_GetRange(move);
    if (!RtActor_MoveSlotUsable(a, slot))
        return 0;
    // wild POKeMON don't escape (Teleport, or Roar/Whirlwind ending the battle) in the first 8 seconds,
    // so there is time to catch them; afterwards they try only now and then
    if (a->side == RT_SIDE_ENEMY && !gRt->isTrainer
     && (bm->effect == EFFECT_TELEPORT || bm->effect == EFFECT_ROAR)
     && (gRt->frame - a->sentOutFrame < 480 || (Random() % 4) != 0))
        return 0;
    if (bm->power)
    {
        u8 type = RtMove_TypeOf(a, move);
        u8 eff = RtTypeEffectiveness(type, t);
        if ((t->ability == ABILITY_LEVITATE && type == TYPE_GROUND) || (t->ability == ABILITY_VOLT_ABSORB && type == TYPE_ELECTRIC)
         || (t->ability == ABILITY_WATER_ABSORB && type == TYPE_WATER) || (t->ability == ABILITY_FLASH_FIRE && type == TYPE_FIRE))
            eff = 0;
        if (eff == 0 && a->aiSkill >= 3)
            return 1;
        score = bm->power > 1 ? bm->power : 60;
        score = score * (eff ? eff : 5) / 10;
        if (RtActor_HasType(a, type))
            score = score * 3 / 2;
        if (a->aiSkill < 3)
            score = 60 + Random() % 60;
        if (dist > range + 12)
        {
            // out of range: only worth it for long reach patterns
            score /= 4;
        }
        if (bm->effect == EFFECT_EXPLOSION && a->hp > a->maxHp / 3)
            score /= 8;
        if (bm->effect == EFFECT_DREAM_EATER && t->mstatus != MST_SLP)
            return 1;
        if (bm->effect == EFFECT_OHKO && t->level > a->level)
            return 1;
        if (bm->effect == EFFECT_RECHARGE && t->hp > t->maxHp / 2 && a->aiSkill >= 7)
            score /= 2;
        if (bm->effect == EFFECT_FAKE_OUT && gRt->frame - a->sentOutFrame > 200)
            return 1;
        score += 20;
    }
    else
    {
        u8 eff = bm->effect;
        score = 30;
        switch (eff)
        {
        case EFFECT_SLEEP:
            score = (t->mstatus || t->ability == ABILITY_INSOMNIA || t->ability == ABILITY_VITAL_SPIRIT) ? 1 : 70;
            break;
        case EFFECT_POISON:
        case EFFECT_TOXIC:
            score = (t->mstatus || RtActor_HasType(t, TYPE_POISON) || RtActor_HasType(t, TYPE_STEEL) || t->ability == ABILITY_IMMUNITY) ? 1 : 70;
            break;
        case EFFECT_PARALYZE:
            score = (t->mstatus || t->ability == ABILITY_LIMBER || (move == MOVE_THUNDER_WAVE && (RtActor_HasType(t, TYPE_GROUND) || t->ability == ABILITY_VOLT_ABSORB))) ? 1 : 70;
            break;
        case EFFECT_WILL_O_WISP:
            score = (t->mstatus || RtActor_HasType(t, TYPE_FIRE) || t->ability == ABILITY_WATER_VEIL || t->ability == ABILITY_FLASH_FIRE) ? 1 : 70;
            break;
        case EFFECT_CONFUSE:
        case EFFECT_SWAGGER:
        case EFFECT_FLATTER:
            score = t->confuse ? 1 : 45;
            break;
        case EFFECT_ATTRACT:
            score = t->attract ? 1 : 30;
            break;
        case EFFECT_TAUNT:
        case EFFECT_TORMENT:
        case EFFECT_DISABLE:
        case EFFECT_ENCORE:
            score = (t->taunt || t->disable || t->encore) ? 1 : 25;
            break;
        case EFFECT_LEECH_SEED:
            score = (t->leechSeed || RtActor_HasType(t, TYPE_GRASS)) ? 1 : 55;
            break;
        case EFFECT_RESTORE_HP:
        case EFFECT_SOFTBOILED:
        case EFFECT_MORNING_SUN:
        case EFFECT_SYNTHESIS:
        case EFFECT_MOONLIGHT:
        case EFFECT_REST:
        case EFFECT_SWALLOW:
            score = a->hp < a->maxHp / 3 ? 110 : (a->hp < a->maxHp / 2 ? 60 : 1);
            break;
        case EFFECT_PROTECT:
        case EFFECT_ENDURE:
            score = 10;
            break;
        case EFFECT_SUBSTITUTE:
            score = (a->substituteHp || a->hp < a->maxHp / 2) ? 1 : 35;
            break;
        case EFFECT_REFLECT:
            score = gRt->sides[a->side].reflect ? 1 : 40;
            break;
        case EFFECT_LIGHT_SCREEN:
            score = gRt->sides[a->side].lightScreen ? 1 : 40;
            break;
        case EFFECT_RAIN_DANCE:
            score = gRt->weather == WEATHER_RAIN_RT ? 1 : 30;
            break;
        case EFFECT_SUNNY_DAY:
            score = gRt->weather == WEATHER_SUN_RT ? 1 : 30;
            break;
        case EFFECT_SANDSTORM:
            score = gRt->weather == WEATHER_SAND_RT ? 1 : 25;
            break;
        case EFFECT_HAIL:
            score = gRt->weather == WEATHER_HAIL_RT ? 1 : 25;
            break;
        case EFFECT_ROAR:
            score = gRt->isTrainer ? 20 : 2;
            break;
        case EFFECT_TELEPORT:
            score = gRt->isTrainer ? 10 : (a->side == RT_SIDE_ENEMY && a->hp < a->maxHp / 4 ? 40 : 1);
            break;
        case EFFECT_SPLASH:
            score = 5;
            break;
        case EFFECT_COUNTER:
        case EFFECT_MIRROR_COAT:
            score = dist < 60 ? 30 : 5;
            break;
        case EFFECT_BATON_PASS:
            score = 1;
            break;
        case EFFECT_PERISH_SONG:
            score = 15;
            break;
        case EFFECT_SPIKES:
            score = gRt->sides[t->side].spikes >= 3 ? 1 : 30;
            break;
        default:
            if (eff >= EFFECT_ATTACK_UP && eff <= EFFECT_EVASION_UP)
                score = a->stages[eff - EFFECT_ATTACK_UP] >= 2 ? 2 : 40;
            else if (eff >= EFFECT_ATTACK_UP_2 && eff <= EFFECT_EVASION_UP_2)
                score = a->stages[eff - EFFECT_ATTACK_UP_2] >= 2 ? 2 : 50;
            else if (eff >= EFFECT_ATTACK_DOWN && eff <= EFFECT_EVASION_DOWN)
                score = t->stages[eff - EFFECT_ATTACK_DOWN] <= -2 ? 2 : 35;
            else if (eff >= EFFECT_ATTACK_DOWN_2 && eff <= EFFECT_EVASION_DOWN_2)
                score = t->stages[eff - EFFECT_ATTACK_DOWN_2] <= -2 ? 2 : 40;
            break;
        }
        if (dist > range + 20 && gRtMoveDefs[move].pattern != RTP_SELF && gRtMoveDefs[move].pattern != RTP_FIELD)
            score /= 3;
        if (a->aiSkill < 3)
            score = 20 + Random() % 40;
    }
    if (score < 1)
        score = 1;
    if (score > 0xFFFF)
        score = 0xFFFF;
    return score;
}

static u8 PreferredRange(struct RtActor *a, struct RtActor *t)
{
    // keep the distance where the best ready offensive move can hit
    u8 i, bestRange = 70;
    u16 best = 0;
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        u16 m = a->moves[i];
        u16 s;
        if (!m || gBattleMoves[m].power == 0)
            continue;
        s = gBattleMoves[m].power + (a->cd[i] ? 0 : 60);
        if (s > best)
        {
            best = s;
            bestRange = RtMove_GetRange(m);
        }
    }
    if (bestRange > 150)
        bestRange = 150;
    return bestRange * 2 / 3;
}

// Path direction towards the target, refreshed every 6 frames per actor (it is not cheap).
static bool8 PathTo(struct RtActor *a, struct RtActor *t, u8 *ang)
{
    if (((gRt->frame + a->id * 2) & 7) == 0 || a->aiPathAge == 0)
    {
        u8 pa;
        a->aiPathOn = RtArena_PathAngle(FROM_FX(a->x), FROM_FX(a->y), FROM_FX(t->x), FROM_FX(t->y), a->radius / 2 + 2, &pa);
        a->aiPathAng = pa;
        a->aiPathAge = 1;
    }
    if (a->aiPathOn)
        *ang = a->aiPathAng;
    return a->aiPathOn;
}

static void SteerAround(struct RtActor *a, u8 *ang)
{
    // probe ahead; rotate until free
    static const s8 sOffsets[] = {0, 32, -32, 64, -64, 96, -96};
    u8 i;
    s32 ax = FROM_FX(a->x), ay = FROM_FX(a->y);
    for (i = 0; i < ARRAY_COUNT(sOffsets); i++)
    {
        u8 t = *ang + sOffsets[i];
        s32 px = ax + RtCos(t) * 14 / 256;
        s32 py = ay + RtSin(t) * 14 / 256;
        if (!RtArena_CircleBlocked(px, py, a->radius / 2 + 2))
        {
            *ang = t;
            return;
        }
    }
}

static u8 LeadAim(struct RtActor *a, struct RtActor *t, u16 move)
{
    u8 ang = RtAtan2(FROM_FX(t->x - a->x), FROM_FX(t->y - a->y));
    const struct RtMoveDef *d = &gRtMoveDefs[move];
    if (a->aiSkill >= 6 && (d->pattern == RTP_BULLET || d->pattern == RTP_WAVE) && d->speed)
    {
        // predict target movement from its input direction
        s32 dist = RtDist(FROM_FX(t->x - a->x), FROM_FX(t->y - a->y));
        s32 frames = dist * 16 / d->speed;
        s32 sp = RtActor_SpeedPx(t);
        s32 px = FROM_FX(t->x) + t->inMoveX * sp / 256 * frames / 256;
        s32 py = FROM_FX(t->y) + t->inMoveY * sp / 256 * frames / 256;
        ang = RtAtan2(px - FROM_FX(a->x), py - FROM_FX(a->y));
    }
    // imperfect aim for weak AIs
    if (a->aiSkill < 8)
    {
        u8 err = (10 - a->aiSkill) * 2;
        ang += (s8)((Random() % (err * 2 + 1)) - err);
    }
    return ang;
}

void RtAi_Update(struct RtActor *a)
{
    struct RtActor *t;
    s32 dx, dy, dist;
    u8 ang, threatAng, urgency;
    s16 mx = 0, my = 0;

    a->inSlot = -1;
    a->inDash = FALSE;
    if (a->state != AST_NORMAL && a->state != AST_CAST)
    {
        a->inMoveX = a->inMoveY = 0;
        return;
    }
    if (gRt->isSafari && a->side == RT_SIDE_ENEMY)
    {
        // safari mons just wander
        if (a->aiTimer == 0)
        {
            a->aiTimer = 40 + Random() % 60;
            a->aiStrafe = Random();
        }
        a->aiTimer--;
        if ((a->aiTimer & 0x30) == 0)
        {
            a->inMoveX = a->inMoveY = 0;
        }
        else
        {
            ang = (u8)a->aiStrafe;
            SteerAround(a, &ang);
            a->inMoveX = RtCos(ang);
            a->inMoveY = RtSin(ang);
        }
        return;
    }
    t = PickTarget(a);
    if (!t)
    {
        a->inMoveX = a->inMoveY = 0;
        return;
    }
    dx = FROM_FX(t->x - a->x);
    dy = FROM_FX(t->y - a->y);
    dist = RtDist(dx, dy);
    ang = RtAtan2(dx, dy);
    // effective distance to the target's body
    dist -= t->radius;
    if (dist < 0)
        dist = 0;

    {
        extern const u8 gRtDebugEnabled;
        extern u16 gRtDebugTestMove;
        if (gRtDebugEnabled && gRtDebugTestMove)
        {
            // move test mode: the enemy is a training dummy, the player walks into range and casts
            u8 range = RtMove_GetRange(a->moves[0]);
            if (range > 90)
                range = 90;
            if (gRtMoveDefs[a->moves[0]].pattern == RTP_MELEE)
                range = range * 2 / 3;
            a->inMoveX = a->inMoveY = 0;
            if (a->side == RT_SIDE_ENEMY)
                return;
            if (dist > range)
            {
                u8 pang = ang;
                PathTo(a, t, &pang);
                a->inMoveX = RtCos(pang);
                a->inMoveY = RtSin(pang);
                return;
            }
            if (a->state == AST_NORMAL && a->gcd == 0 && RtActor_MoveSlotUsable(a, 0))
            {
                a->inSlot = 0;
                a->inAim = ang;
            }
            return;
        }
    }
    // dodging
    if (a->aiDodgeCd == 0 && FindThreat(a, &threatAng, &urgency))
    {
        u8 chance = a->aiSkill * 9;
        if (a->dashCd == 0 && urgency > 8 && (Random() % 100) < chance)
        {
            a->inDash = TRUE;
            a->inMoveX = RtCos(threatAng);
            a->inMoveY = RtSin(threatAng);
            a->aiDodgeCd = 50 - a->aiSkill * 3;
            return;
        }
        if ((Random() % 100) < chance + 20)
        {
            SteerAround(a, &threatAng);
            a->inMoveX = RtCos(threatAng);
            a->inMoveY = RtSin(threatAng);
            a->aiDodgeCd = 6;
            return;
        }
        a->aiDodgeCd = 30;
    }

    // attack decision
    if (a->aiThinkCd == 0 && a->state == AST_NORMAL && a->gcd == 0)
    {
        u8 i, bestSlot = 0xFF;
        u16 best = 0;
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            u16 s = ScoreMove(a, t, i, dist);
            s = s * (80 + Random() % 40) / 100;
            if (s > best)
            {
                best = s;
                bestSlot = i;
            }
        }
        a->aiThinkCd = 8 + (10 - a->aiSkill) * 5 + Random() % 12;
        if (bestSlot != 0xFF && best > 8)
        {
            u16 move = a->moves[bestSlot];
            u8 range = RtMove_GetRange(move);
            u8 pat = gRtMoveDefs[move].pattern;
            if (dist <= range + 10 || pat == RTP_SELF || pat == RTP_FIELD)
            {
                a->inSlot = bestSlot;
                a->inAim = LeadAim(a, t, move);
                return;
            }
        }
    }

    // movement: hold preferred range and strafe
    {
        u8 want = PreferredRange(a, t);
        u8 mang;
        if ((Random() % 128) == 0)
            a->aiStrafe = -a->aiStrafe;
        bool8 noLine = ((gRt->frame + a->id) & 3) == 0 ? !RtArena_ShotClear(FROM_FX(a->x), FROM_FX(a->y) - 8, FROM_FX(t->x), FROM_FX(t->y) - 8) : a->aiNoLine;
        a->aiNoLine = noLine;
        if (dist > want + 16 || noLine)
        {
            u8 pang;
            mang = ang + a->aiStrafe * 16;
            // walk around walls/trees instead of pushing into them
            if (PathTo(a, t, &pang))
                mang = pang;
        }
        else if (dist < want - 16)
            mang = ang + 128 - a->aiStrafe * 20;
        else
            mang = ang + a->aiStrafe * 64;
        SteerAround(a, &mang);
        mx = RtCos(mang);
        my = RtSin(mang);
        // weaker AIs pause sometimes
        if (a->aiSkill < 4 && ((gRt->frame + a->id * 37) % 120) < 30)
            mx = my = 0;
    }
    a->inMoveX = mx;
    a->inMoveY = my;
    if (a->confuse)
    {
        a->inMoveX = -a->inMoveX;
        a->inMoveY = -a->inMoveY;
    }
    // face the target while idle-strafing
    if (!mx && !my)
        RtActor_SetFacingFromAngle(a, ang);
}

// Trainers use their items (potions etc.) like in the original game.
void RtAi_TrainerItems(void)
{
    s32 i, j;
    if (!gRt->isTrainer || (gBattleTypeFlags & (BATTLE_TYPE_FRONTIER | BATTLE_TYPE_TRAINER_HILL)) || gTrainerBattleOpponent_A == TRAINER_SECRET_BASE)
        return;
    if (!sTrainerItemsLoaded || gRt->frame < 5)
    {
        for (j = 0; j < MAX_TRAINER_ITEMS; j++)
            sTrainerItems[j] = gTrainers[gTrainerBattleOpponent_A].items[j];
        sTrainerItemsLoaded = TRUE;
        sTrainerItemCd = 0;
    }
    if (sTrainerItemCd)
    {
        sTrainerItemCd--;
        return;
    }
    for (i = 2; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        if (!a->active || a->state == AST_GONE || !RtActor_IsAlive(a))
            continue;
        for (j = 0; j < MAX_TRAINER_ITEMS; j++)
        {
            u16 item = sTrainerItems[j];
            s32 heal = 0;
            bool8 cure = FALSE;
            if (item == ITEM_NONE)
                continue;
            switch (item)
            {
            case ITEM_POTION: heal = 20; break;
            case ITEM_SUPER_POTION: heal = 50; break;
            case ITEM_HYPER_POTION: heal = 200; break;
            case ITEM_MAX_POTION: heal = a->maxHp; break;
            case ITEM_FULL_RESTORE: heal = a->maxHp; cure = TRUE; break;
            case ITEM_FULL_HEAL: cure = TRUE; break;
            }
            if ((heal && a->hp < a->maxHp / 4) || (cure && !heal && a->mstatus))
            {
                extern void RtActor_CureStatus(struct RtActor *a);
                if (heal)
                    RtActor_Heal(a, heal);
                if (cure)
                {
                    RtActor_CureStatus(a);
                    a->confuse = 0;
                }
                sTrainerItems[j] = ITEM_NONE;
                {
                    u8 *d = gStringVar1;
                    d = StringCopy(d, gTrainerClassNames[gTrainers[gTrainerBattleOpponent_A].trainerClass]);
                    *d++ = CHAR_SPACE;
                    StringCopy(d, gTrainers[gTrainerBattleOpponent_A].trainerName);
                }
                CopyItemName(item, gStringVar2);
                RtUi_ToastFmt(sText_TrainerUsed, 70);
                PlaySE(SE_USE_ITEM);
                sTrainerItemCd = 240;
                return;
            }
        }
    }
}

void RtAi_ResetTrainerItems(void)
{
    sTrainerItemsLoaded = FALSE;
}
