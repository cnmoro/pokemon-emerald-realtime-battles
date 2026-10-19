#include "global.h"
#include "move.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "rt_compat.h"

#include "data/rt/vanilla_moves.h"

EWRAM_DATA struct RtMoveData gRtMoves[RT_MOVES_COUNT] = {0};
static EWRAM_DATA bool8 sInit = FALSE;

bool8 RtMove_IsPhysicalMove(u16 move)
{
    return GetMoveCategory(move) == DAMAGE_CATEGORY_PHYSICAL;
}

// first non-"self" and first "self" additional effect of a move
static void FindExtra(u16 move, enum MoveEffect *foe, u8 *foeChance, enum MoveEffect *self, u8 *selfChance, enum MoveEffect *foe2)
{
    u32 i, n = GetMoveAdditionalEffectCount(move);
    *foe = *self = *foe2 = 0;
    *foeChance = *selfChance = 0;
    for (i = 0; i < n; i++)
    {
        const struct AdditionalEffect *e = &gMovesInfo[move].additionalEffects[i];
        if (e->self)
        {
            if (!*self) { *self = e->moveEffect; *selfChance = e->chance; }
        }
        else if (!*foe)
        {
            *foe = e->moveEffect;
            *foeChance = e->chance;
        }
        else if (!*foe2)
            *foe2 = e->moveEffect;
    }
}

static u8 LegacyEffectFor(u16 move, u8 *chance, u8 *power)
{
    enum BattleMoveEffects e = GetMoveEffect(move);
    enum MoveEffect foe, self, foe2;
    u8 foeChance, selfChance;
    FindExtra(move, &foe, &foeChance, &self, &selfChance, &foe2);
    *chance = foeChance;
    if (GetMovePower(move) == 1)
        *power = 60; // variable-power moves the engine does not know: a plain 60 power hit

    switch (e)
    {
    case EFFECT_ROOST: case EFFECT_RESTORE_HP: return RTEFF_RESTORE_HP;
    case EFFECT_SPEED_UP_2: return RTEFF_SPEED_UP_2;
    case EFFECT_SPECIAL_ATTACK_UP_2: return RTEFF_SPECIAL_ATTACK_UP_2;
    case EFFECT_COSMIC_POWER: return RTEFF_COSMIC_POWER;
    case EFFECT_TRICK: return RTEFF_TRICK;
    case EFFECT_LOW_KICK: return RTEFF_LOW_KICK;
    case EFFECT_REVENGE: return RTEFF_REVENGE;
    case EFFECT_RECOIL: return RTEFF_RECOIL;
    case EFFECT_ABSORB: return RTEFF_ABSORB;
    case EFFECT_MIRACLE_EYE: return RTEFF_FORESIGHT;
    case EFFECT_STEALTH_ROCK: case EFFECT_TOXIC_SPIKES: return RTEFF_SPIKES;
    case EFFECT_SEMI_INVULNERABLE: case EFFECT_DOUBLE_POWER_ON_ARG_STATUS: case EFFECT_HIT_ESCAPE:
    case EFFECT_SUCKER_PUNCH: case EFFECT_POWER_BASED_ON_TARGET_HP: case EFFECT_LAST_RESORT:
    case EFFECT_PAYBACK: case EFFECT_ASSURANCE: case EFFECT_PUNISHMENT: case EFFECT_CHANGE_TYPE_ON_ITEM:
    case EFFECT_BRINE: case EFFECT_GYRO_BALL: case EFFECT_NATURAL_GIFT: case EFFECT_FLING: case EFFECT_TRUMP_CARD:
    case EFFECT_METAL_BURST:
        if (GetMovePower(move) == 0 || e == EFFECT_METAL_BURST)
            return RTEFF_SPLASH;
        return RTEFF_HIT;
    case EFFECT_HIT:
        break;
    default:
        // support/status moves without a counterpart in the engine do nothing but play their animation
        return GetMovePower(move) ? RTEFF_HIT : RTEFF_SPLASH;
    }

    // EFFECT_HIT: pick the closest legacy "hit with side effect"
    switch (foe)
    {
    case MOVE_EFFECT_BURN: return RTEFF_BURN_HIT;
    case MOVE_EFFECT_FREEZE: return RTEFF_FREEZE_HIT;
    case MOVE_EFFECT_PARALYSIS: return RTEFF_PARALYZE_HIT;
    case MOVE_EFFECT_POISON: case MOVE_EFFECT_TOXIC: return RTEFF_POISON_HIT;
    case MOVE_EFFECT_FLINCH: return RTEFF_FLINCH_HIT;
    case MOVE_EFFECT_CONFUSION: return RTEFF_CONFUSE_HIT;
    case MOVE_EFFECT_ATK_MINUS_1: return RTEFF_ATTACK_DOWN_HIT;
    case MOVE_EFFECT_DEF_MINUS_1: return RTEFF_DEFENSE_DOWN_HIT;
    case MOVE_EFFECT_SPD_MINUS_1: return RTEFF_SPEED_DOWN_HIT;
    case MOVE_EFFECT_SP_ATK_MINUS_1: return RTEFF_SPECIAL_ATTACK_DOWN_HIT;
    case MOVE_EFFECT_SP_DEF_MINUS_1: return RTEFF_SPECIAL_DEFENSE_DOWN_HIT;
    case MOVE_EFFECT_ACC_MINUS_1: return RTEFF_ACCURACY_DOWN_HIT;
    case MOVE_EFFECT_WRAP: return RTEFF_TRAP;
    default: break;
    }
    switch (self)
    {
    case MOVE_EFFECT_RECHARGE: return RTEFF_RECHARGE;
    case MOVE_EFFECT_ALL_STATS_UP: return RTEFF_ALL_STATS_UP_HIT;
    case MOVE_EFFECT_SP_ATK_MINUS_2: return RTEFF_OVERHEAT;
    case MOVE_EFFECT_DEF_SPDEF_DOWN: return RTEFF_SUPERPOWER;
    default: break;
    }
    if (GetMoveCriticalHitStage(move) > 0 && GetMoveCriticalHitStage(move) < 4)
        return RTEFF_HIGH_CRITICAL;
    return RTEFF_HIT;
}

void RtMoves_Init(void)
{
    u32 m;
    if (sInit)
        return;
    sInit = TRUE;
    for (m = 1; m < RT_MOVES_COUNT; m++)
    {
        struct RtMoveData *d = &gRtMoves[m];
        if (m < ARRAY_COUNT(sRtVanillaMoves))
        {
            *d = sRtVanillaMoves[m];
        }
        else
        {
            u8 chance = 0, power = GetMovePower(m);
            d->effect = LegacyEffectFor(m, &chance, &power);
            d->power = power > 255 ? 255 : power;
            d->type = GetMoveType(m);
            d->accuracy = GetMoveAccuracy(m);
            d->pp = GetMovePP(m);
            d->secondaryEffectChance = chance;
            d->priority = GetMovePriority(m);
            d->target = GetMoveTarget(m) == MOVE_TARGET_FOES_AND_ALLY ? RT_TARGET_FOES_AND_ALLY : 0;
            d->flags = (MoveMakesContact(m) ? 1 << 0 : 0) | (MoveIgnoresProtect(m) ? 0 : 1 << 1)
                     | (MoveCanBeBouncedBack(m) ? 1 << 2 : 0) | (MoveCanBeSnatched(m) ? 1 << 3 : 0)
                     | 1 << 4 | (MoveIgnoresKingsRock(m) ? 0 : 1 << 5);
        }
    }
}
