// Real-time battle moves: cooldowns, casting patterns, hitboxes, damage and every move effect.
#include "global.h"
#include "rt_internal.h"
#include "rt_vfx.h"
#include "battle.h"
#include "battle_main.h"
#include "battle_setup.h"
#include "data.h"
#include "event_data.h"
#include "field_weather.h"
#include "item.h"
#include "main.h"
#include "palette.h"
#include "pokedex.h"
#include "pokemon.h"
#include "random.h"
#include "sound.h"
#include "string_util.h"
#include "constants/abilities.h"
#include "constants/battle.h"
#include "constants/battle_move_effects.h"
#include "constants/hold_effects.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/songs.h"
#include "m4a.h"
#include "constants/species.h"
#include "constants/weather.h"

#include "data/rt/move_defs.h"

// Physical or special. Liquid Crystal has the per-move split (byte 10 of the move data: 0 physical,
// 1 special, 2 status); FireRed/Emerald decide by type like generation 3.
bool8 RtMove_IsPhysical(u16 move, u8 type)
{
#if RT_LC
    u8 cat = ((const u8 *)&gBattleMoves[move])[10];
    if (cat < 2)
        return cat == 0;
#endif
    return IS_TYPE_PHYSICAL(type);
}

// a few move sounds loop until stopped (the battle animations end them with stopsound): stop them after a
// moment, and when the battle ends
#define RT_LOOP_SE_FRAMES 60
static EWRAM_DATA u16 sLoopSe = 0;
static EWRAM_DATA u8 sLoopSeTimer = 0;

void Rt_StopLoopSE(void)
{
    if (sLoopSe)
        m4aSongNumStop(sLoopSe);
    sLoopSe = 0;
    sLoopSeTimer = 0;
}

void Rt_UpdateLoopSE(void)
{
    if (sLoopSe && --sLoopSeTimer == 0)
        Rt_StopLoopSE();
}

static void Rt_PlayMoveSE(u16 se)
{
    Rt_StopLoopSE();
    PlaySE(se);
    if (se == SE_M_GUST || se == SE_M_BLIZZARD)
    {
        sLoopSe = se;
        sLoopSeTimer = RT_LOOP_SE_FRAMES;
    }
}

static void ExecuteMove(struct RtActor *a);
void RtMove_CheckBerries(struct RtActor *a);
static void FinishCast(struct RtActor *a, u16 recovery);
EWRAM_DATA u16 gRtCastLog[64][2] = {0};
EWRAM_DATA u8 gRtCastLogN = 0;
static void BeginMove(struct RtActor *a, u16 move, u8 angle, u8 slot);
static bool8 TryHitActor(struct RtProj *p, struct RtActor *atk, struct RtActor *def);
void RtActor_CureStatus(struct RtActor *a);
bool8 RtActor_AnimDone(struct RtActor *a);
u8 RtActor_AnimHitDelay(struct RtActor *a, u8 anim);

static const u8 sText_HurtItself[] = _("It hurt itself in its confusion!");
static const u8 sText_FullyParalyzed[] = _("{STR_VAR_1} is paralyzed!\nIt can't move!");
static const u8 sText_Infatuated[] = _("{STR_VAR_1} is immobilized by love!");
static const u8 sText_Loafing[] = _("{STR_VAR_1} is loafing around!");
static const u8 sText_LostFocus[] = _("{STR_VAR_1} lost its focus and\ncouldn't move!");
static const u8 sText_NothingHappened[] = _("But nothing happened!");
static const u8 sText_Failed[] = _("But it failed!");
static const u8 sText_UsedMove[] = _("{STR_VAR_1} used {STR_VAR_2}!");
static const u8 sText_Transformed[] = _("{STR_VAR_1} transformed!");
static const u8 sText_Damp[] = _("{STR_VAR_1}'s DAMP prevents\nexplosions!");
static const u8 sText_KeptGoing[] = _("{STR_VAR_1} kept going and crashed!");
static const u8 sText_StoleItem[] = _("{STR_VAR_1} stole {STR_VAR_2}!");
static const u8 sText_KnockedOff[] = _("{STR_VAR_1}'s item was knocked off!");
static const u8 sText_Rain[] = _("It started to rain!");
static const u8 sText_Sun[] = _("The sunlight got bright!");
static const u8 sText_Sand[] = _("A sandstorm brewed!");
static const u8 sText_Hail[] = _("It started to hail!");
static const u8 sText_WeatherEnd[] = _("The weather returned to normal.");
static const u8 sText_Intimidate[] = _("{STR_VAR_1}'s INTIMIDATE cuts\nthe foe's ATTACK!");
static const u8 sText_OhKo[] = _("It's a one-hit KO!");
static const u8 sText_Substitute[] = _("{STR_VAR_1} made a SUBSTITUTE!");
static const u8 sText_BideUnleash[] = _("{STR_VAR_1} unleashed energy!");
static const u8 sText_TraceAbility[] = _("{STR_VAR_1} TRACED the foe's ability!");
static const u8 sText_DestinyBond[] = _("{STR_VAR_1} took its foe with it!");
static const u8 sText_PerishAll[] = _("All POKéMON hearing the song\nwill faint soon!");
static const u8 sText_Cured[] = _("A bell chimed!");
static const u8 sText_Wish[] = _("{STR_VAR_1}'s wish came true!");
static const u8 sText_Sketched[] = _("{STR_VAR_1} sketched {STR_VAR_2}!");
static const u8 sText_Learned[] = _("{STR_VAR_1} learned {STR_VAR_2}!");

// ---------------------------------------------------------------------------
// Tables / basic helpers
// ---------------------------------------------------------------------------
u16 RtMove_Cooldown(u16 move, u8 ppBonuses, u8 slot)
{
    u32 pp, cd;
    if (move == MOVE_NONE)
        return 0;
    pp = CalculatePPWithBonus(move, ppBonuses, slot);
    if (pp == 0)
        pp = 1;
    cd = 1950 / pp;          // +25% over the first tuning (1560 / pp)
    if (cd < 38) cd = 38;
    if (cd > 412) cd = 412;
    return cd;
}

static const struct RtMoveDef *Def(u16 move)
{
    return &gRtMoveDefs[move < MOVES_COUNT ? move : MOVE_POUND];
}

static void NameOf(struct RtActor *a, u8 *dst)
{
    GetMonData(a->mon, MON_DATA_NICKNAME, dst);
    StringGet_Nickname(dst);
}

static void ToastName(struct RtActor *a, const u8 *tmpl, u16 frames)
{
    NameOf(a, gStringVar1);
    RtUi_ToastFmt(tmpl, frames);
}

bool8 RtMove_IsOffensive(u16 move)
{
    return gBattleMoves[move].power != 0;
}

// --- melee balance -------------------------------------------------------------------------
// every melee move reaches at least this far (hitbox centre ahead of the body / hitbox radius)
#define MELEE_MIN_LEN    30
#define MELEE_MIN_RADIUS 14
// a melee move used with the target a little out of reach first dashes up to it
#define LUNGE_MAX        64   // px of closing dash at most
#define LUNGE_SPEED      4    // px per frame
// a melee hit landing on an incoming shot in its first frames bats it back (if it hits harder)
#define PARRY_WINDOW     3
// damage taken while performing a melee / rush move (and no knockback)
#define CONTACT_ARMOR    80
// shots lose damage over their flight: full up to 40 % of the range, 75 % at the very end
#define FALLOFF_START    102  // /256 of the flight
#define FALLOFF_MIN      75

static u8 MeleeLen(const struct RtMoveDef *d)
{
    u16 l = d->range + 10;
    return l < MELEE_MIN_LEN ? MELEE_MIN_LEN : (l > 255 ? 255 : l);
}

static u8 MeleeRadius(const struct RtMoveDef *d)
{
    return d->radius < MELEE_MIN_RADIUS ? MELEE_MIN_RADIUS : d->radius;
}

// performing a contact move (melee strike or rush): takes less damage and is not pushed around
bool8 RtMove_ContactArmor(struct RtActor *a)
{
    u8 pat;
    if (a->state != AST_CAST || a->act.move == MOVE_NONE)
        return FALSE;
    pat = Def(a->act.move)->pattern;
    return pat == RTP_MELEE || pat == RTP_RUSH;
}

u8 RtMove_GetRange(u16 move)
{
    const struct RtMoveDef *d = Def(move);
    switch (d->pattern)
    {
    case RTP_MELEE: return MeleeLen(d) + MeleeRadius(d) + 8 + LUNGE_MAX;
    case RTP_RUSH: return (d->speed * d->duration) / 16 + 10;
    case RTP_BULLET:
    case RTP_STREAM:
    case RTP_BEAM:
    case RTP_WAVE:
    case RTP_TARGETED:
    case RTP_ZONE:
    case RTP_LEAP:
    case RTP_BURROW:
        return d->range * 2;
    case RTP_BURST:
    case RTP_CONE:
        return d->range * 2 - 4;
    case RTP_ROLL:
        return 140;
    }
    return 255;
}

static bool8 IsSoundMove(u16 move)
{
    switch (move)
    {
    case MOVE_GROWL: case MOVE_ROAR: case MOVE_SING: case MOVE_SUPERSONIC: case MOVE_SCREECH:
    case MOVE_SNORE: case MOVE_UPROAR: case MOVE_METAL_SOUND: case MOVE_GRASS_WHISTLE: case MOVE_HYPER_VOICE:
    case MOVE_PERISH_SONG: case MOVE_HEAL_BELL:
        return TRUE;
    }
    return FALSE;
}

static bool8 HitsFliers(u16 move)
{
    return move == MOVE_GUST || move == MOVE_TWISTER || move == MOVE_THUNDER || move == MOVE_SKY_UPPERCUT;
}

static u8 HiddenPowerType(struct Pokemon *mon)
{
    u8 t = ((GetMonData(mon, MON_DATA_HP_IV) & 1)
          | ((GetMonData(mon, MON_DATA_ATK_IV) & 1) << 1)
          | ((GetMonData(mon, MON_DATA_DEF_IV) & 1) << 2)
          | ((GetMonData(mon, MON_DATA_SPEED_IV) & 1) << 3)
          | ((GetMonData(mon, MON_DATA_SPATK_IV) & 1) << 4)
          | ((GetMonData(mon, MON_DATA_SPDEF_IV) & 1) << 5)) * 15 / 63 + 1;
    if (t >= TYPE_MYSTERY)
        t++;
    return t;
}

static u8 HiddenPowerPower(struct Pokemon *mon)
{
    return (((GetMonData(mon, MON_DATA_HP_IV) & 2) >> 1)
          | ((GetMonData(mon, MON_DATA_ATK_IV) & 2))
          | ((GetMonData(mon, MON_DATA_DEF_IV) & 2) << 1)
          | ((GetMonData(mon, MON_DATA_SPEED_IV) & 2) << 2)
          | ((GetMonData(mon, MON_DATA_SPATK_IV) & 2) << 3)
          | ((GetMonData(mon, MON_DATA_SPDEF_IV) & 2) << 4)) * 40 / 63 + 30;
}

static bool8 WeatherActive(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        if (a->active && a->state != AST_GONE && RtActor_IsAlive(a)
         && (a->ability == ABILITY_CLOUD_NINE || a->ability == ABILITY_AIR_LOCK))
            return FALSE;
    }
    return gRt->weather != WEATHER_NONE_RT;
}

u8 RtMove_TypeOf(struct RtActor *a, u16 move)
{
    u8 t = gBattleMoves[move].type;
    if (move == MOVE_HIDDEN_POWER)
        t = HiddenPowerType(a->mon);
    else if (move == MOVE_WEATHER_BALL && WeatherActive())
    {
        switch (gRt->weather)
        {
        case WEATHER_RAIN_RT: t = TYPE_WATER; break;
        case WEATHER_SUN_RT: t = TYPE_FIRE; break;
        case WEATHER_SAND_RT: t = TYPE_ROCK; break;
        case WEATHER_HAIL_RT: t = TYPE_ICE; break;
        }
    }
    return t;
}

u8 RtTypeEffectiveness(u8 atkType, struct RtActor *def)
{
    s32 i = 0;
    u16 eff = 10;
    while (gTypeEffectiveness[i] != TYPE_ENDTABLE)
    {
        if (gTypeEffectiveness[i] == TYPE_FORESIGHT)
        {
            if (def->foresight)
                break;
            i += 3;
            continue;
        }
        if (gTypeEffectiveness[i] == atkType)
        {
            if (gTypeEffectiveness[i + 1] == def->types[0])
                eff = eff * gTypeEffectiveness[i + 2] / 10;
            if (def->types[1] != def->types[0] && gTypeEffectiveness[i + 1] == def->types[1])
                eff = eff * gTypeEffectiveness[i + 2] / 10;
        }
        i += 3;
    }
    return eff > 40 ? 40 : eff;
}

static u16 CalcPower(struct RtActor *a, struct RtActor *t, u16 move)
{
    u16 power = gBattleMoves[move].power;
    u8 eff = gBattleMoves[move].effect;
    switch (eff)
    {
    case EFFECT_FLAIL:
        {
            u32 r = a->hp * 48 / (a->maxHp ? a->maxHp : 1);
            power = r <= 1 ? 200 : r <= 4 ? 150 : r <= 9 ? 100 : r <= 16 ? 80 : r <= 32 ? 40 : 20;
        }
        break;
    case EFFECT_ERUPTION:
        power = 150 * a->hp / (a->maxHp ? a->maxHp : 1);
        if (power == 0) power = 1;
        break;
    case EFFECT_RETURN:
        power = 10 * GetMonData(a->mon, MON_DATA_FRIENDSHIP) / 25;
        break;
    case EFFECT_FRUSTRATION:
        power = 10 * (255 - GetMonData(a->mon, MON_DATA_FRIENDSHIP)) / 25;
        break;
    case EFFECT_HIDDEN_POWER:
        power = HiddenPowerPower(a->mon);
        break;
    case EFFECT_MAGNITUDE:
        {
            u8 r = Random() % 100;
            power = r < 5 ? 10 : r < 15 ? 30 : r < 35 ? 50 : r < 65 ? 70 : r < 85 ? 90 : r < 95 ? 110 : 150;
        }
        break;
    case EFFECT_LOW_KICK:
        if (t)
        {
            u16 w = GetPokedexHeightWeight(t->dex, 1);
            power = w < 100 ? 20 : w < 250 ? 40 : w < 500 ? 60 : w < 1000 ? 80 : w < 2000 ? 100 : 120;
        }
        break;
    case EFFECT_WEATHER_BALL:
        if (WeatherActive())
            power *= 2;
        break;
    case EFFECT_FACADE:
        if (a->mstatus)
            power *= 2;
        break;
    case EFFECT_SMELLINGSALT:
        if (t && t->mstatus == MST_PAR)
            power *= 2;
        break;
    case EFFECT_REVENGE:
        if (a->damageTakenThisCast)
            power *= 2;
        break;
    case EFFECT_PURSUIT:
        if (t && t->state == AST_DASH)
            power *= 2;
        break;
    case EFFECT_ROLLOUT:
        {
            u16 n = a->rolloutHits > 4 ? 4 : a->rolloutHits;
            power <<= n;
            if (a->defenseCurl)
                power *= 2;
        }
        break;
    case EFFECT_FURY_CUTTER:
        power <<= (a->furyCutter > 4 ? 4 : a->furyCutter);
        break;
    case EFFECT_TRIPLE_KICK:
        power = 10 * (a->act.hitsDone + 1);
        break;
    case EFFECT_SPIT_UP:
        power = 100 * a->stockpile;
        break;
    case EFFECT_PRESENT:
        power = a->act.power;
        break;
    case EFFECT_FLINCH_MINIMIZE_HIT:
        if (t && t->minimize)
            power *= 2;
        break;
    case EFFECT_GUST:
    case EFFECT_TWISTER:
        if (t && t->invuln == 1)
            power *= 2;
        break;
    case EFFECT_EARTHQUAKE:
        if (t && t->invuln == 2)
            power *= 2;
        break;
    case EFFECT_SECRET_POWER:
        break;
    }
    if (move == MOVE_SURF && t && t->invuln == 3)
        power *= 2;
    if (move == MOVE_WHIRLPOOL && t && t->invuln == 3)
        power *= 2;
    if (move == MOVE_SOLAR_BEAM && WeatherActive() && gRt->weather != WEATHER_SUN_RT)
        power /= 2;
    return power;
}

s32 RtMove_CalcDamage(struct RtActor *atk, struct RtActor *def, u16 move, u16 power, u8 *outEff, u8 *outCrit)
{
    u8 type = RtMove_TypeOf(atk, move);
    bool8 phys = RtMove_IsPhysical(move, type);
    u32 a, d;
    s32 dmg;
    u8 eff, critStage = 0;
    bool8 crit = FALSE;
    s8 aStage, dStage;
    u8 hold = atk->holdEffect;

    eff = RtTypeEffectiveness(type, def);
    if (def->ability == ABILITY_LEVITATE && type == TYPE_GROUND)
        eff = 0;
    if (def->ability == ABILITY_WONDER_GUARD && eff <= 10 && power)
        eff = 0;
    // Future Sight / Doom Desire deal typeless damage (they hit DARK / every type for normal damage)
    if (gBattleMoves[move].effect == EFFECT_FUTURE_SIGHT)
        eff = 10;
    if (outEff)
        *outEff = eff;
    if (eff == 0)
        return 0;

    // critical hit
    if (gBattleMoves[move].effect == EFFECT_HIGH_CRITICAL || gBattleMoves[move].effect == EFFECT_BLAZE_KICK
     || gBattleMoves[move].effect == EFFECT_POISON_TAIL || move == MOVE_AEROBLAST || move == MOVE_CROSS_CHOP)
        critStage++;
    if (atk->focusEnergy)
        critStage += 2;
    if (hold == HOLD_EFFECT_SCOPE_LENS)
        critStage++;
    if (hold == HOLD_EFFECT_LUCKY_PUNCH && atk->species == SPECIES_CHANSEY)
        critStage += 2;
    if (hold == HOLD_EFFECT_STICK && atk->species == SPECIES_FARFETCHD)
        critStage += 2;
    {
        static const u8 sCrit[] = {16, 8, 4, 3, 2};
        if (critStage > 4) critStage = 4;
        if (def->ability != ABILITY_BATTLE_ARMOR && def->ability != ABILITY_SHELL_ARMOR && (Random() % sCrit[critStage]) == 0)
            crit = TRUE;
    }
    if (outCrit)
        *outCrit = crit;

    aStage = phys ? atk->stages[STG_ATK] : atk->stages[STG_SPA];
    dStage = phys ? def->stages[STG_DEF] : def->stages[STG_SPD];
    if (crit && aStage < 0) aStage = 0;
    if (crit && dStage > 0) dStage = 0;
    {
        static const u8 r[13][2] = {{10,40},{10,35},{10,30},{10,25},{10,20},{10,15},{10,10},{15,10},{20,10},{25,10},{30,10},{35,10},{40,10}};
        a = phys ? atk->atk : atk->spa;
        d = phys ? def->def : def->spd;
        a = a * r[aStage + 6][0] / r[aStage + 6][1];
        d = d * r[dStage + 6][0] / r[dStage + 6][1];
    }
    // abilities / items on stats
    if (phys)
    {
        if (atk->ability == ABILITY_HUGE_POWER || atk->ability == ABILITY_PURE_POWER) a *= 2;
        if (atk->ability == ABILITY_HUSTLE) a = a * 150 / 100;
        if (atk->ability == ABILITY_GUTS && atk->mstatus) a = a * 150 / 100;
        if (hold == HOLD_EFFECT_CHOICE_BAND) a = a * 150 / 100;
        if (hold == HOLD_EFFECT_THICK_CLUB && (atk->species == SPECIES_CUBONE || atk->species == SPECIES_MAROWAK)) a *= 2;
        if (def->ability == ABILITY_MARVEL_SCALE && def->mstatus) d = d * 150 / 100;
        if (def->holdEffect == HOLD_EFFECT_METAL_POWDER && def->species == SPECIES_DITTO) d *= 2;
    }
    else
    {
        if (hold == HOLD_EFFECT_SOUL_DEW && (atk->species == SPECIES_LATIAS || atk->species == SPECIES_LATIOS)) a = a * 150 / 100;
        if (def->holdEffect == HOLD_EFFECT_SOUL_DEW && (def->species == SPECIES_LATIAS || def->species == SPECIES_LATIOS)) d = d * 150 / 100;
        if (hold == HOLD_EFFECT_DEEP_SEA_TOOTH && atk->species == SPECIES_CLAMPERL) a *= 2;
        if (def->holdEffect == HOLD_EFFECT_DEEP_SEA_SCALE && def->species == SPECIES_CLAMPERL) d *= 2;
        if (hold == HOLD_EFFECT_LIGHT_BALL && atk->species == SPECIES_PIKACHU) a *= 2;
        if (atk->ability == ABILITY_PLUS || atk->ability == ABILITY_MINUS)
        {
            s32 i;
            for (i = 0; i < RT_MAX_ACTORS; i++)
            {
                struct RtActor *o = &gRt->actors[i];
                if (o != atk && o->active && o->side == atk->side && o->state != AST_GONE
                 && ((atk->ability == ABILITY_PLUS && o->ability == ABILITY_MINUS) || (atk->ability == ABILITY_MINUS && o->ability == ABILITY_PLUS)))
                    a = a * 150 / 100;
            }
        }
    }
    if (def->ability == ABILITY_THICK_FAT && (type == TYPE_FIRE || type == TYPE_ICE))
        a /= 2;
    // pinch abilities
    if (atk->hp <= atk->maxHp / 3)
    {
        if ((atk->ability == ABILITY_OVERGROW && type == TYPE_GRASS) || (atk->ability == ABILITY_BLAZE && type == TYPE_FIRE)
         || (atk->ability == ABILITY_TORRENT && type == TYPE_WATER) || (atk->ability == ABILITY_SWARM && type == TYPE_BUG))
            power = power * 150 / 100;
    }
    // type boosting held items (+10%)
    {
        static const u8 sItemTypes[][2] = {
            {HOLD_EFFECT_BUG_POWER, TYPE_BUG}, {HOLD_EFFECT_STEEL_POWER, TYPE_STEEL}, {HOLD_EFFECT_GROUND_POWER, TYPE_GROUND},
            {HOLD_EFFECT_ROCK_POWER, TYPE_ROCK}, {HOLD_EFFECT_GRASS_POWER, TYPE_GRASS}, {HOLD_EFFECT_DARK_POWER, TYPE_DARK},
            {HOLD_EFFECT_FIGHTING_POWER, TYPE_FIGHTING}, {HOLD_EFFECT_ELECTRIC_POWER, TYPE_ELECTRIC}, {HOLD_EFFECT_WATER_POWER, TYPE_WATER},
            {HOLD_EFFECT_FLYING_POWER, TYPE_FLYING}, {HOLD_EFFECT_POISON_POWER, TYPE_POISON}, {HOLD_EFFECT_ICE_POWER, TYPE_ICE},
            {HOLD_EFFECT_GHOST_POWER, TYPE_GHOST}, {HOLD_EFFECT_PSYCHIC_POWER, TYPE_PSYCHIC}, {HOLD_EFFECT_FIRE_POWER, TYPE_FIRE},
            {HOLD_EFFECT_DRAGON_POWER, TYPE_DRAGON}, {HOLD_EFFECT_NORMAL_POWER, TYPE_NORMAL},
        };
        u32 i;
        for (i = 0; i < ARRAY_COUNT(sItemTypes); i++)
            if (hold == sItemTypes[i][0] && type == sItemTypes[i][1])
                power = power * (100 + atk->holdParam) / 100;
    }
    if (atk->charge && type == TYPE_ELECTRIC)
        power *= 2;
    if (atk->helpingHand)
        power = power * 150 / 100;
    if (atk->flashFire && type == TYPE_FIRE)
        power = power * 150 / 100;
    {
        s32 i;
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *o = &gRt->actors[i];
            if (!o->active || o->state == AST_GONE)
                continue;
            if (o->mudSport && type == TYPE_ELECTRIC) power /= 2;
            if (o->waterSport && type == TYPE_FIRE) power /= 2;
        }
    }
    if (d == 0) d = 1;
    dmg = ((2 * atk->level / 5 + 2) * power * a / d) / 50;
    if (phys && atk->mstatus == MST_BRN && atk->ability != ABILITY_GUTS)
        dmg /= 2;
    if (!crit)
    {
        if (phys && gRt->sides[def->side].reflect)
            dmg /= 2;
        if (!phys && gRt->sides[def->side].lightScreen)
            dmg /= 2;
    }
    dmg += 2;
    if (WeatherActive())
    {
        if (gRt->weather == WEATHER_RAIN_RT)
        {
            if (type == TYPE_WATER) dmg = dmg * 150 / 100;
            if (type == TYPE_FIRE) dmg /= 2;
        }
        else if (gRt->weather == WEATHER_SUN_RT)
        {
            if (type == TYPE_FIRE) dmg = dmg * 150 / 100;
            if (type == TYPE_WATER) dmg /= 2;
        }
    }
    if (crit)
        dmg *= 2;
    if (RtActor_HasType(atk, type) && gBattleMoves[move].effect != EFFECT_FUTURE_SIGHT)
        dmg = dmg * 150 / 100;
    dmg = dmg * eff / 10;
    dmg = dmg * (85 + Random() % 16) / 100;
    if (dmg < 1)
        dmg = 1;
    return dmg;
}

// accuracy roll softened for real-time aiming. TRUE = hit
static bool8 AccuracyCheck(struct RtActor *atk, struct RtActor *def, u16 move)
{
    static const u8 r[13][2] = {{33,100},{36,100},{43,100},{50,100},{60,100},{75,100},{1,1},{133,100},{166,100},{2,1},{233,100},{133,50},{3,1}};
    u8 eff = gBattleMoves[move].effect;
    u32 acc = gBattleMoves[move].accuracy;
    s8 st;
    if (eff == EFFECT_ALWAYS_HIT || eff == EFFECT_VITAL_THROW || acc == 0)
        return TRUE;
    if (atk->lockOn || atk->ability == ABILITY_COMPOUND_EYES && Random() % 10 < 3)
        return TRUE;
    if (move == MOVE_THUNDER && gRt->weather == WEATHER_RAIN_RT && WeatherActive())
        return TRUE;
    if (eff == EFFECT_OHKO)
        return TRUE; // handled separately
    acc = 100 - (100 - acc) / 2;
    st = atk->stages[STG_ACC] - def->stages[STG_EVA];
    if (st < -6) st = -6;
    if (st > 6) st = 6;
    acc = acc * r[st + 6][0] / r[st + 6][1];
    if (atk->ability == ABILITY_HUSTLE && RtMove_IsPhysical(move, RtMove_TypeOf(atk, move)))
        acc = acc * 80 / 100;
    if (def->ability == ABILITY_SAND_VEIL && gRt->weather == WEATHER_SAND_RT && WeatherActive())
        acc = acc * 80 / 100;
    if (def->holdEffect == HOLD_EFFECT_EVASION_UP)
        acc = acc * (100 - def->holdParam) / 100;
    if (atk->ability == ABILITY_COMPOUND_EYES)
        acc = acc * 130 / 100;
    return (Random() % 100) < acc;
}

// ---------------------------------------------------------------------------
// Projectiles
// ---------------------------------------------------------------------------
struct RtProj *RtProj_Alloc(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_PROJ; i++)
    {
        if (gRt->proj[i].kind == PK_NONE)
        {
            memset(&gRt->proj[i], 0, sizeof(gRt->proj[i]));
            return &gRt->proj[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Difficulty tuning for the opponents' shots (OPTION -> DIFFICULTY)
// level: 0 easy, 1 medium, 2 hard
static u8 FoeLevel(struct RtActor *a)
{
    u8 d;
    if (a->side != RT_SIDE_ENEMY)
        return 3; // player side: untouched
    d = gSaveBlock2Ptr->optionsRtDifficulty;
    return d == 1 ? 0 : (d == 2 ? 2 : 1);
}

// foes' projectiles / rushes travel slower so they can be read and dodged
static s32 FoeSpeed(struct RtActor *a, s32 sp)
{
    static const u8 sPct[4] = {60, 75, 90, 100};
    sp = sp * sPct[FoeLevel(a)] / 100;
    return sp < 16 ? 16 : sp;
}

// Beams (Ice Beam, Psybeam, Aurora Beam, Thunderbolt...) used to reach their full length almost at once.
// Their tip now travels like a fast shot (slower for foes, by difficulty) and the beam holds a little
// after that, so a beam can be seen coming and sidestepped.
#define BEAM_TIP_SPEED 40 // px/frame * 16 (2.5 px per frame at 100%)
static s32 BeamTipSpeed(struct RtActor *a)
{
    return FoeSpeed(a, BEAM_TIP_SPEED);
}
#define WAVE_SPEED_PCT 70
static u16 BeamFrames(struct RtActor *a, const struct RtMoveDef *d)
{
    s32 sp = BeamTipSpeed(a);
    s32 extend = (d->range * 2 * 16 + sp - 1) / sp;
    s32 hold = (d->duration ? d->duration : 24) / 2;
    return extend + (hold < 8 ? 8 : hold);
}

// a foe's live shot still on its way (not yet hit anything)
bool8 RtMove_HasShotInFlight(u8 owner)
{
    s32 i;
    for (i = 0; i < RT_MAX_PROJ; i++)
    {
        struct RtProj *p = &gRt->proj[i];
        if ((p->kind == PK_BULLET || p->kind == PK_WAVE) && p->owner == owner && !p->hitMask
         && !(p->flags & RTMF_ARC) && p->age < p->life)
            return TRUE;
    }
    return FALSE;
}

static struct RtProj *NewProj(struct RtActor *a, u8 kind, u16 move)
{
    struct RtProj *p = RtProj_Alloc();
    const struct RtMoveDef *d = Def(move);
    if (!p)
        return NULL;
    p->kind = kind;
    p->owner = a->id;
    p->side = a->side;
    p->move = move;
    p->shape = d->shape;
    p->pal = d->pal;
    p->flags = d->flags;
    p->trail = d->trail;
    p->param2 = a->act.castId;
    p->homingTarget = 0xFF;
    return p;
}

static u16 ProjLife(u8 range, u8 speed)
{
    u32 px = range * 2;
    if (speed == 0)
        speed = 16;
    return px * 16 / speed + 1;
}

// A prop lifted by Psychic / Seismic Toss / Strength... is hurled as a projectile of that move.
void RtMove_ThrowProp(struct RtActor *a, u16 move, u8 kind, s32 x, s32 y, s16 z)
{
    struct RtProj *p = NewProj(a, PK_BULLET, move);
    struct RtActor *t = (a->act.targetId < RT_MAX_ACTORS) ? &gRt->actors[a->act.targetId] : NULL;
    s32 sp = 80;
    u8 angle = a->act.angle;
    if (!p)
        return;
    if (t == NULL || !t->active || t->state == AST_GONE || !RtActor_IsAlive(t) || t->side == a->side)
        t = Rt_NearestFoe(a);
    if (t)
        angle = RtAtan2(FROM_FX(t->x - x), FROM_FX(t->y - y));
    p->shape = RtProps_ShapeOf(kind);
    p->pal = RtProps_PalOf(kind);
    p->flags &= ~(RTMF_ARC | RTMF_HOMING | RTMF_BOOMERANG | RTMF_PIERCE | RTMF_FAN | RTMF_BLEND);
    p->trail = FX_NONE;
    p->angle = angle;
    p->x = x;
    p->y = y;
    p->z = z;
    p->vx = RtCos(angle) * sp / 16;
    p->vy = RtSin(angle) * sp / 16;
    p->radius = 10;
    p->life = ProjLife(110, sp);
    p->len = p->life;
    p->param = 16 | 32;          // 16: thrown prop, 32: keep its height
    p->param2 = (a->act.castId & 0xFF) | (kind << 8);
    PlaySE(SE_M_VITAL_THROW2);
    RtActor_SetAnim(a, RTA_ATTACK, FALSE);
}

// group hit masks: hitboxes spawned by the same cast share one mask (streams, multi hitbox patterns)
static bool8 AlreadyHit(struct RtProj *p, struct RtActor *t)
{
    struct RtActor *o = &gRt->actors[p->owner];
    if (p->param & 1) // group mask
    {
        if (o->act.castId == p->param2)
            return (o->act.hitMask & (1 << t->id)) != 0;
    }
    return (p->hitMask & (1 << t->id)) != 0;
}

static void MarkHit(struct RtProj *p, struct RtActor *t)
{
    struct RtActor *o = &gRt->actors[p->owner];
    p->hitMask |= 1 << t->id;
    if ((p->param & 1) && o->act.castId == p->param2)
        o->act.hitMask |= 1 << t->id;
}

static bool8 IsValidTarget(struct RtProj *p, struct RtActor *t)
{
    struct RtActor *o = &gRt->actors[p->owner];
    u8 tgt = gBattleMoves[p->move].target;
    if (!t->active || t->state == AST_GONE || t->state == AST_FAINT || t->state == AST_ENTER || t->state == AST_CAUGHT)
        return FALSE;
    if (t == o)
        return FALSE;
    if (t->side == p->side)
    {
        // only spread moves that hit allies (earthquake etc.) affect teammates
        if (tgt != MOVE_TARGET_FOES_AND_ALLY || p->kind != PK_AREA)
            return FALSE;
    }
    if (t->invuln)
    {
        if (t->invuln == 1 && !HitsFliers(p->move))
            return FALSE;
        if (t->invuln == 2 && p->move != MOVE_EARTHQUAKE && p->move != MOVE_MAGNITUDE && p->move != MOVE_FISSURE)
            return FALSE;
        if (t->invuln == 3 && p->move != MOVE_SURF && p->move != MOVE_WHIRLPOOL)
            return FALSE;
    }
    return TRUE;
}

static void SpawnTrail(struct RtProj *p)
{
    if (RtVfx_ProjTrail(p))
        return;
    if (p->trail == FX_NONE)
    {
        // every projectile leaves a faint trail of sparks in its colour
        if ((p->age % 3) == 1 && p->kind == PK_BULLET && !(p->param & 8))
        {
            struct RtPart *pt = RtFx_Spawn(FX_SPARK8, p->pal, p->x, p->y, 8);
            if (pt)
            {
                pt->z = p->z;
                pt->vx = -p->vx / 8;
                pt->vy = -p->vy / 8;
            }
        }
        return;
    }
    if ((p->age & 3) == 0)
    {
        struct RtPart *pt = RtFx_Spawn(p->trail, p->pal, p->x, p->y, 12);
        if (pt)
        {
            pt->z = p->z;
            pt->flags |= PF_BLEND;
        }
    }
}

static void ProjImpact(struct RtProj *p, s32 x, s32 y)
{
    RtVfx_Impact(p->move, Def(p->move)->impact == RTI_NONE ? RTI_HIT : Def(p->move)->impact, p->pal, x, y);
}

static void KillProj(struct RtProj *p)
{
    if (p->kind == PK_BULLET && (p->param & 16))
        RtProps_Debris(p->param2 >> 8 ? (p->param2 >> 8) : PROP_ROCK, p->x, p->y + TO_FX(p->z));
    p->kind = PK_NONE;
}

static void TurnTowards(struct RtProj *p, u8 rate)
{
    struct RtActor *t = NULL;
    struct RtActor *o = &gRt->actors[p->owner];
    u8 want;
    s8 diff;
    s32 sp;
    if (p->homingTarget == 0xFE)
        return; // lost its target: flies straight on
    if (p->homingTarget < RT_MAX_ACTORS)
        t = &gRt->actors[p->homingTarget];
    // a well-timed dash shakes off homing shots: they lose track and fly straight
    if (t && t->state == AST_DASH)
    {
        p->homingTarget = 0xFE;
        return;
    }
    if (!t || !RtActor_IsAlive(t) || t->state == AST_GONE || t->invuln)
    {
        t = Rt_NearestFoe(o);
        if (!t)
            return;
        p->homingTarget = t->id;
    }
    want = RtAtan2(FROM_FX(t->x - p->x), FROM_FX(t->y - p->y));
    diff = (s8)(want - p->angle);
    if (diff > rate) diff = rate;
    if (diff < -rate) diff = -rate;
    p->angle += diff;
    sp = RtDist(p->vx, p->vy);
    p->vx = RtCos(p->angle) * sp / 256;
    p->vy = RtSin(p->angle) * sp / 256;
}

static void UpdateBullet(struct RtProj *p)
{
    s32 i;
    const struct RtMoveDef *d = Def(p->move);
    struct RtActor *o = &gRt->actors[p->owner];
    if (p->delay)
    {
        p->delay--;
        if (p->delay == 0)
        {
            // launch from the owner's current position
            p->x = o->x + RtCos(p->angle) * (o->radius + 4);
            p->y = o->y + RtSin(p->angle) * (o->radius + 4) / 2;
            Rt_PlayMoveSE(d->sfx);
        }
        return;
    }
    if ((p->flags & RTMF_HOMING) || o->lockOn)
    {
        u8 rate = (p->flags & RTMF_HOMING) ? (d->aux ? d->aux : 3) : 2;
        switch (FoeLevel(o))
        {
        case 0: rate = rate / 3; break;      // easy: barely curves
        case 1: rate = rate / 2; break;      // medium
        case 2: rate = rate * 2 / 3; break;  // hard
        }
        if (rate == 0)
            rate = 1;
        TurnTowards(p, rate);
    }
    if ((p->param & 0x100) && p->life > 1 && p->homingTarget < RT_MAX_ACTORS)
    {
        // lob that lands on its target: steer the landing point to where the target is now
        struct RtActor *t = &gRt->actors[p->homingTarget];
        if (t->active && RtActor_IsAlive(t) && t->state != AST_GONE)
        {
            s32 off = (s8)p->maxRadius;
            s32 tx = t->x + RtCos(p->angle + 64) * off;
            s32 ty = t->y - TO_FX(t->height) / 3 + RtSin(p->angle + 64) * off / 2;
            s32 vx = (tx - p->x) / p->life, vy = (ty - p->y) / p->life;
            if (vx > TO_FX(4)) vx = TO_FX(4);
            if (vx < -TO_FX(4)) vx = -TO_FX(4);
            if (vy > TO_FX(4)) vy = TO_FX(4);
            if (vy < -TO_FX(4)) vy = -TO_FX(4);
            p->vx = vx;
            p->vy = vy;
        }
    }
    p->x += p->vx;
    p->y += p->vy;
    p->age++;
    if (p->flags & RTMF_ARC)
    {
        s32 t = p->age * 256 / (p->len ? p->len : 1); // 0..256
        p->z = 6 + (t * (256 - t)) * 40 / 65536;
    }
    else if (p->param & 32)
    {
        if (p->z > 12)
            p->z--;
    }
    else
    {
        p->z = 10;
    }
    SpawnTrail(p);
    if ((p->flags & RTMF_BOOMERANG) && p->age == p->len / 2)
    {
        p->vx = -p->vx;
        p->vy = -p->vy;
        p->angle += 128;
        p->hitMask = 0;
    }
    if (p->life)
        p->life--;
    // obstacles
    // arena props stop shots (and get damaged)
    if (!(p->flags & RTMF_ARC) && RtProps_HitAt(p->x, p->y, p->radius / 2 + 1, (p->param & 16) ? 4 : (gBattleMoves[p->move].power >= 80 ? 2 : 1))
     && !(p->flags & RTMF_PIERCE))
    {
        ProjImpact(p, p->x, p->y);
        if (p->param & 4)
            goto zone;
        KillProj(p);
        return;
    }
    if (!(p->flags & RTMF_ARC) && RtArena_PointBlocksShot(FROM_FX(p->x), FROM_FX(p->y)))
    {
        ProjImpact(p, p->x, p->y);
        if (p->param & 4)
            goto zone;
        KillProj(p);
        return;
    }
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 dx, dy, r;
        if (!IsValidTarget(p, t) || AlreadyHit(p, t))
            continue;
        if ((p->flags & RTMF_ARC) && p->life > 6 && !(p->param & 4))
            continue; // lobbed: only hits when landing
        dx = FROM_FX(t->x - p->x);
        dy = FROM_FX(t->y - (TO_FX(t->height) / 3) - p->y);
        r = p->radius + t->radius;
        if (dx * dx + dy * dy <= r * r)
        {
            if (p->param & 4)
                goto zone; // zone shells burst on contact
            MarkHit(p, t);
            if (TryHitActor(p, o, t) && !(p->flags & RTMF_PIERCE))
            {
                KillProj(p);
                return;
            }
        }
    }
    if (p->life == 0)
    {
        if (p->param & 4)
            goto zone;
        if (p->flags & RTMF_ARC)
            ProjImpact(p, p->x, p->y);
        KillProj(p);
    }
    return;

zone:
    {
        // turn into a lingering zone
        p->kind = PK_ZONE;
        p->maxRadius = p->radius = d->radius;
        p->life = d->duration * 4;
        p->age = 0;
        p->hitMask = 0;
        p->z = 0;
        p->param &= ~4;
        if (p->move == MOVE_SPIKES)
        {
            u8 side = p->side ^ 1;
            if (gRt->sides[side].spikes < 3)
                gRt->sides[side].spikes++;
        }
        RtFx_Burst(p->shape, p->pal, p->x, p->y, 4, 2, 18);
    }
}

static void UpdateZone(struct RtProj *p)
{
    s32 i;
    struct RtActor *o = &gRt->actors[p->owner];
    p->age++;
    if (p->life)
        p->life--;
    if (p->life == 0)
    {
        KillProj(p);
        return;
    }
    // spikes re-arm periodically
    if (p->move == MOVE_SPIKES && (p->age % 90) == 0)
        p->hitMask = 0;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 dx, dy, r;
        if (!IsValidTarget(p, t) || (p->hitMask & (1 << t->id)))
            continue;
        if (p->move == MOVE_SPIKES && (RtActor_HasType(t, TYPE_FLYING) || t->ability == ABILITY_LEVITATE))
            continue;
        dx = FROM_FX(t->x - p->x);
        dy = FROM_FX(t->y - p->y) * 2;
        r = p->radius + t->radius / 2;
        if (dx * dx + dy * dy <= r * r)
        {
            p->hitMask |= 1 << t->id;
            if (p->move == MOVE_SPIKES)
            {
                s32 dmg = t->maxHp / 10;
                RtActor_Damage(t, dmg ? dmg : 1, o->id, FALSE);
                t->slowTimer = 40;
                RtFx_Burst(FX_SPARK8, FXP_STEEL, t->x, t->y, 3, 2, 12);
                PlaySE(SE_M_VICEGRIP);
            }
            else
            {
                TryHitActor(p, o, t);
            }
        }
    }
    // visual sparkle
    if (RtVfx_ZoneTick(p))
        ;
    else if ((p->age & 7) == 0 && p->move != MOVE_SPIKES)
    {
        u8 a = Random();
        s32 r = Random() % (p->radius + 1);
        RtFx_Spawn(p->shape, p->pal, p->x + RtCos(a) * r, p->y + RtSin(a) * r / 2, 16);
    }
}

// how hard a move hits coming from this POKeMON (power x attacking stat x STAB), for parries
static u32 MoveStrength(struct RtActor *a, u16 move)
{
    u8 type = RtMove_TypeOf(a, move);
    u32 pw = gBattleMoves[move].power;
    u32 s;
    if (pw <= 1)
        pw = 60; // variable power
    s = pw * (RtMove_IsPhysical(move, type) ? a->atk : a->spa);
    if (RtActor_HasType(a, type))
        s = s * 3 / 2;
    return s;
}

// a melee strike meeting an incoming shot in its first frames bats it back at the shooter,
// but only when the strike hits harder than the shot
static void TryParry(struct RtProj *p, struct RtActor *o)
{
    s32 i;
    u32 mine = 0;
    for (i = 0; i < RT_MAX_PROJ; i++)
    {
        struct RtProj *q = &gRt->proj[i];
        struct RtActor *shooter;
        s32 dx, dy, r;
        u8 ang;
        s32 sp;
        if (q->kind != PK_BULLET || q->side == p->side || q->delay || (q->param & (4 | 8)) || q->move == MOVE_NONE)
            continue;
        dx = FROM_FX(q->x - p->x);
        dy = FROM_FX(q->y - p->y);
        r = p->radius + q->radius + 2;
        if (dx * dx + dy * dy > r * r)
            continue;
        shooter = &gRt->actors[q->owner < RT_MAX_ACTORS ? q->owner : 0];
        if (!mine)
            mine = MoveStrength(o, p->move);
        if (mine <= MoveStrength(shooter, q->move))
            continue;
        // batted back toward whoever shot it
        ang = RtAtan2(FROM_FX(shooter->x - q->x), FROM_FX(shooter->y - q->y));
        sp = RtDist(q->vx, q->vy) * 5 / 4;
        q->angle = ang;
        q->vx = RtCos(ang) * sp / 256;
        q->vy = RtSin(ang) * sp / 256;
        q->side = o->side;
        q->owner = o->id;
        q->hitMask = 0;
        q->homingTarget = shooter->id;
        q->age = 0;
        if (q->life < q->len / 2 + 8)
            q->life = q->len / 2 + 8;
        q->flags &= ~RTMF_BOOMERANG;
        RtFx_Burst(FX_SPARK8, FXP_WHITE, q->x, q->y, 6, 3, 14);
        RtFx_Spawn(FX_GLOW16, FXP_GOLD, q->x, q->y, 8);
        RtFx_Word(q->x, q->y - TO_FX(12), FX_W_BLOCK);
        PlaySE(SE_M_REFLECT);
        gRt->pauseFrames = 4;
        {
            extern const u8 gRtDebugEnabled;
            extern u32 gRtDebugStat;
            if (gRtDebugEnabled)
                gRtDebugStat = ((gRtDebugStat & 0xFF) + 1) | (p->owner << 8) | (shooter->id << 12) | ((u32)p->move << 16); // harness: parry log
        }
    }
}

static void UpdateMelee(struct RtProj *p)
{
    s32 i;
    struct RtActor *o = &gRt->actors[p->owner];
    if (p->delay)
    {
        p->delay--;
        if (p->delay == 0)
            Rt_PlayMoveSE(Def(p->move)->sfx);
        return;
    }
    // follow owner (the same reach in every direction)
    {
        s32 reach = o->radius + p->len;
        p->x = o->x + RtCos(p->angle) * reach;
        p->y = o->y + RtSin(p->angle) * reach - TO_FX(o->height / 3);
        p->z = 0;
    }
    p->age++;
    if (p->life)
        p->life--;
    if (p->age <= PARRY_WINDOW)
        TryParry(p, o);
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 dx, dy, r;
        if (!IsValidTarget(p, t) || AlreadyHit(p, t))
            continue;
        dx = FROM_FX(t->x - p->x);
        dy = FROM_FX(t->y - TO_FX(t->height / 3) - p->y);
        r = p->radius + t->radius;
        if (dx * dx + dy * dy <= r * r)
        {
            MarkHit(p, t);
            TryHitActor(p, o, t);
        }
    }
    if (!(p->hitMask & 0x80) && RtProps_HitAt(p->x, p->y + TO_FX(o->height / 3), p->radius, gBattleMoves[p->move].power >= 70 ? 2 : 1))
        p->hitMask |= 0x80;
    if (p->life == 0)
        KillProj(p);
}

static void UpdateRushBox(struct RtProj *p)
{
    s32 i;
    struct RtActor *o = &gRt->actors[p->owner];
    if (o->state != AST_CAST || o->act.castId != p->param2)
    {
        KillProj(p);
        return;
    }
    p->x = o->x + RtCos(o->act.angle) * o->radius / 2;
    p->y = o->y + RtSin(o->act.angle) * o->radius / 4 - TO_FX(o->height / 3);
    p->age++;
    if (!(p->hitMask & 0x80) && RtProps_HitAt(p->x, p->y + TO_FX(o->height / 3), p->radius, 2))
    {
        p->hitMask |= 0x80;
        if (Def(p->move)->pattern != RTP_ROLL)
            o->act.timer = 0xFFF0; // crashed into it
    }
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 dx, dy, r;
        if (!IsValidTarget(p, t) || AlreadyHit(p, t))
            continue;
        dx = FROM_FX(t->x - p->x);
        dy = FROM_FX(t->y - TO_FX(t->height / 3) - p->y);
        r = p->radius + t->radius;
        if (dx * dx + dy * dy <= r * r)
        {
            MarkHit(p, t);
            if (TryHitActor(p, o, t))
            {
                o->act.hitsDone++;
                if (Def(p->move)->pattern == RTP_ROLL)
                {
                    // bounce off and keep rolling; can hit again after a moment
                    o->act.angle += 128;
                    p->param |= 2;
                    p->hitMask = 0;
                    o->act.hitMask = 0;
                    o->rolloutHits++;
                    o->act.subTimer = 16;
                }
                else if (!(p->flags & RTMF_PIERCE))
                {
                    // stop the rush with a small bounce back
                    o->act.timer = 0xFFF0;
                    o->vx = -RtCos(o->act.angle) * 2;
                    o->vy = -RtSin(o->act.angle) * 2;
                }
            }
        }
    }
}

static s32 DistToSegment(s32 px, s32 py, s32 ax, s32 ay, u8 angle, s32 len, s32 *along)
{
    // pixels; segment from (ax,ay) in direction angle with length len
    s32 dx = px - ax, dy = py - ay;
    s32 c = RtCos(angle), s = RtSin(angle);
    s32 t = (dx * c + dy * s) / 256;
    s32 perp = (-dx * s + dy * c) / 256;
    if (along)
        *along = t;
    if (t < 0)
        return RtDist(dx, dy);
    if (t > len)
        return RtDist(px - (ax + c * len / 256), py - (ay + s * len / 256));
    return perp < 0 ? -perp : perp;
}

static void UpdateBeam(struct RtProj *p)
{
    s32 i, max;
    struct RtActor *o = &gRt->actors[p->owner];
    const struct RtMoveDef *d = Def(p->move);
    if (o->state != AST_CAST || o->act.castId != p->param2)
    {
        KillProj(p);
        return;
    }
    p->angle = o->act.angle;
    p->x = o->x + RtCos(p->angle) * (o->radius / 2);
    p->y = o->y + RtSin(p->angle) * (o->radius / 2) / 2 - TO_FX(o->height / 2);
    p->age++;
    max = d->range * 2;
    // the tip advances p->vx (px/frame * 16) every frame
    p->len = 8 + p->age * p->vx / 16;
    if (p->len > max)
        p->len = max;
    // stop at obstacles
    {
        s32 k;
        for (k = 8; k < p->len; k += 8)
        {
            s32 x = FROM_FX(p->x) + RtCos(p->angle) * k / 256;
            s32 y = FROM_FX(p->y) + TO_FX(0) + RtSin(p->angle) * k / 256 + o->height / 2;
            if (RtArena_PointBlocksShot(x, y))
            {
                p->len = k;
                if ((p->age & 3) == 0)
                    RtFx_Burst(FX_SPARK8, p->pal, TO_FX(x), TO_FX(y - o->height / 2), 2, 2, 10);
                break;
            }
        }
    }
    // sparkles along the beam and at its tip
    if (RtVfx_BeamTick(p))
        ;
    else if ((p->age & 1) == 0 && p->len > 16)
    {
        s32 k = 8 + Random() % (p->len - 8);
        struct RtPart *sp = RtFx_Spawn(FX_SPARK8, p->pal, p->x + RtCos(p->angle) * k, p->y + RtSin(p->angle) * k + TO_FX(o->height / 2), 10);
        if (sp)
        {
            u8 perp = p->angle + ((Random() & 1) ? 64 : 192);
            sp->vx = RtCos(perp) / 2;
            sp->vy = RtSin(perp) / 2;
            sp->z = o->height / 2;
        }
    }
    if ((p->age & 3) == 2 && gRtVfxDefs[p->move].style == VS_NONE)
    {
        s32 tx = p->x + RtCos(p->angle) * p->len, ty = p->y + RtSin(p->angle) * p->len + TO_FX(o->height / 2);
        RtFx_Burst(FX_SPARK8, p->pal, tx, ty - TO_FX(o->height / 2), 2, 3, 10);
    }
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 dist;
        if (!IsValidTarget(p, t) || AlreadyHit(p, t))
            continue;
        dist = DistToSegment(FROM_FX(t->x), FROM_FX(t->y) - t->height / 3, FROM_FX(p->x), FROM_FX(p->y), p->angle, p->len, NULL);
        if (dist <= p->radius + t->radius)
        {
            MarkHit(p, t);
            TryHitActor(p, o, t);
        }
    }
    if (p->life)
        p->life--;
    if (p->life == 0)
        KillProj(p);
}

static void UpdateWave(struct RtProj *p)
{
    s32 i;
    struct RtActor *o = &gRt->actors[p->owner];
    p->x += p->vx;
    p->y += p->vy;
    p->age++;
    if (p->maxRadius && p->len < p->maxRadius)
    {
        s32 start = p->maxRadius / 5 > 4 ? p->maxRadius / 5 : 4;
        s32 len = start + (p->maxRadius - start) * p->age / p->param2;
        p->len = len > p->maxRadius ? p->maxRadius : len;
    }
    if (RtVfx_WaveTick(p))
        ;
    else if ((p->age & 3) == 0)
    {
        s32 k = (Random() % (p->len * 2 + 1)) - p->len;
        u8 perp = p->angle + 64;
        RtFx_Spawn(Def(p->move)->trail != FX_NONE ? Def(p->move)->trail : FX_SPARK8, p->pal,
                   p->x + RtCos(perp) * k, p->y + RtSin(perp) * k, 12);
    }
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 along, perp;
        s32 dx, dy;
        if (!IsValidTarget(p, t) || AlreadyHit(p, t))
            continue;
        dx = FROM_FX(t->x - p->x);
        dy = FROM_FX(t->y - p->y);
        along = (dx * RtCos(p->angle) + dy * RtSin(p->angle)) / 256;
        perp = (-dx * RtSin(p->angle) + dy * RtCos(p->angle)) / 256;
        if (perp < 0) perp = -perp;
        if (along < 0) along = -along;
        if (along <= p->radius + t->radius / 2 && perp <= p->len + t->radius)
        {
            MarkHit(p, t);
            TryHitActor(p, o, t);
        }
    }
    if (p->life)
        p->life--;
    if (p->life == 0)
        KillProj(p);
}

static void UpdateArea(struct RtProj *p)
{
    s32 i;
    struct RtActor *o = &gRt->actors[p->owner];
    const struct RtMoveDef *d = Def(p->move);
    if (p->delay)
    {
        p->delay--;
        return;
    }
    p->age++;
    {
        u32 maxR = p->maxRadius;
        u32 dur = d->duration ? d->duration : 16;
        p->radius = maxR * (p->age > dur ? dur : p->age) / dur;
    }
    if (!(p->hitMask & 0x80) && p->age > 2)
    {
        // props caught in the blast
        s32 k;
        for (k = 0; k < RT_MAX_PROPS; k++)
        {
            struct RtProp *pr = &gRt->props[k];
            s32 dx, dy;
            if (pr->kind == PROP_NONE || pr->state != PRS_REST)
                continue;
            dx = FROM_FX(pr->x - p->x);
            dy = FROM_FX(pr->y - p->y) * 2;
            if (RtDist(dx, dy) <= p->radius)
                RtProps_HitAt(pr->x, pr->y - TO_FX(6), 2, gBattleMoves[p->move].power >= 80 ? 3 : 1);
        }
    }
    // particles along the ring
    if (RtVfx_AreaTick(p))
        ;
    else if ((p->age % 2) == 0)
    {
        u8 n = 3, k;
        for (k = 0; k < n; k++)
        {
            u8 a = Random();
            struct RtPart *pt = RtFx_Spawn(p->shape, p->pal, p->x + RtCos(a) * p->radius, p->y + RtSin(a) * p->radius / 2, 14);
            if (pt)
            {
                pt->vx = RtCos(a) / 2;
                pt->vy = RtSin(a) / 4;
            }
        }
    }
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 dx, dy;
        if (!IsValidTarget(p, t) || (p->hitMask & (1 << t->id)))
            continue;
        if (t == o)
            continue;
        dx = FROM_FX(t->x - p->x);
        dy = FROM_FX(t->y - p->y) * 2;
        if (RtDist(dx, dy) <= p->radius + t->radius / 2)
        {
            p->hitMask |= 1 << t->id;
            TryHitActor(p, o, t);
        }
    }
    if (p->age >= (d->duration ? d->duration : 16) + 4)
        KillProj(p);
}

static void StrikeAt(struct RtProj *p)
{
    s32 i;
    struct RtActor *o = &gRt->actors[p->owner];
    const struct RtMoveDef *d = Def(p->move);
    if (RtVfx_Strike(p))
    {
        Rt_PlayMoveSE(d->sfx);
        RtProps_HitAt(p->x, p->y - TO_FX(6), p->radius, 3);
        goto hits;
    }
    if (p->shape == FX_BOLTV)
    {
        s32 k;
        struct RtPart *b;
        for (k = 0; k < 4; k++)
        {
            b = RtFx_Spawn(FX_BOLTV, k & 1 ? FXP_WHITE : p->pal, p->x + TO_FX((k & 1) ? 2 : 0), p->y - TO_FX(16 + k * 28), 12);
            if (b) b->prio = 1;
        }
        b = RtFx_Spawn(FX_RING32, p->pal, p->x, p->y, 14);
        if (b) { b->flags |= PF_SCALE | PF_GROUND; b->scale = 10; b->dscale = 3; }
        RtFx_Flash(0, 8);
    }
    else if (p->shape == FX_COLUMN16)
    {
        struct RtPart *b = RtFx_Spawn(FX_COLUMN16, p->pal, p->x, p->y - TO_FX(14), 18);
        if (b) b->vy = -64;
    }
    RtVfx_Impact(p->move, d->impact, p->pal, p->x, p->y);
    RtFx_Shake(2, 8);
    Rt_PlayMoveSE(d->sfx);
    RtProps_HitAt(p->x, p->y - TO_FX(6), p->radius, 3);
hits:
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *t = &gRt->actors[i];
        s32 dx, dy;
        if (!IsValidTarget(p, t) || (p->hitMask & (1 << t->id)))
            continue;
        if ((p->param & 1) && o->act.castId == p->param2 && (o->act.hitMask & (1 << t->id)))
            continue;
        dx = FROM_FX(t->x - p->x);
        dy = FROM_FX(t->y - p->y) * 2;
        if (RtDist(dx, dy) <= p->radius + t->radius)
        {
            MarkHit(p, t);
            if (p->kind == PK_FUTURE)
            {
                // pre-computed damage ignores type changes since cast
                u8 eff = RtTypeEffectiveness(gBattleMoves[p->move].type, t);
                s32 dmg = p->power * eff / 10;
                if (eff)
                {
                    t->lastHitBy = o->id;
                    RtActor_Damage(t, dmg ? dmg : 1, o->id, FALSE);
                    t->flash = 8;
                }
                else
                {
                    RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_IMMUNE);
                }
            }
            else
            {
                TryHitActor(p, o, t);
            }
        }
    }
}

static void UpdateStrike(struct RtProj *p)
{
    p->age++;
    if (p->delay)
    {
        // the attack builds up over the marked spot
        if (p->delay <= 30 && (p->age & 3) == 0 && p->kind == PK_STRIKE)
        {
            u8 ang = Random();
            s32 r = p->radius + 10;
            u8 shp = (p->shape == FX_BOLTV) ? FX_SPARK8 : (p->shape == FX_COLUMN16 ? FX_ORB8 : FX_SPARK8);
            struct RtPart *pt = RtFx_Spawn(shp, p->pal, p->x + RtCos(ang) * r, p->y + RtSin(ang) * r / 2, 12);
            if (pt)
            {
                pt->vx = -RtCos(ang) * r / 12;
                pt->vy = -RtSin(ang) * r / 24;
                pt->prio = 3;
            }
        }
        p->delay--;
        if (p->delay == 0)
        {
            StrikeAt(p);
            if (gRtVfxDefs[p->move].style == VS_THUNDER)
            {
                p->param |= 0x80; // stays a few frames for the bolt to be drawn
                p->life = 14;
                p->age = 0;
                return;
            }
            KillProj(p);
        }
    }
    else if ((p->param & 0x80) && p->life)
    {
        p->life--;
    }
    else
    {
        KillProj(p);
    }
}

void RtProj_UpdateAll(void)
{
    s32 i;
    for (i = 0; i < RT_MAX_PROJ; i++)
    {
        struct RtProj *p = &gRt->proj[i];
        switch (p->kind)
        {
        case PK_BULLET: UpdateBullet(p); break;
        case PK_MELEE: UpdateMelee(p); break;
        case PK_RUSHBOX: UpdateRushBox(p); break;
        case PK_BEAM: UpdateBeam(p); break;
        case PK_WAVE: UpdateWave(p); break;
        case PK_AREA: UpdateArea(p); break;
        case PK_STRIKE:
        case PK_FUTURE: UpdateStrike(p); break;
        case PK_ZONE: UpdateZone(p); break;
        case PK_BALL:
            {
                extern void RtMove_UpdateBall(struct RtProj *p);
                RtMove_UpdateBall(p);
            }
            break;
        case PK_SAFARI:
            {
                extern void RtMove_UpdateSafariThrow(struct RtProj *p);
                RtMove_UpdateSafariThrow(p);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Hit resolution
// ---------------------------------------------------------------------------
static void ShowEffectiveness(struct RtActor *t, u8 eff)
{
    if (eff > 10)
    {
        RtFx_Word(t->x, t->y - TO_FX(t->height + 12), FX_W_SUPER);
        PlaySE(SE_SUPER_EFFECTIVE);
    }
    else if (eff < 10 && eff > 0)
    {
        RtFx_Word(t->x, t->y - TO_FX(t->height + 12), FX_W_RESIST);
        PlaySE(SE_NOT_EFFECTIVE);
    }
    else if (eff == 10)
    {
        PlaySE(SE_EFFECTIVE);
    }
}

static void Knockback(struct RtActor *atk, struct RtActor *t, s32 strength)
{
    u8 ang;
    if (RtMove_ContactArmor(t))
        return; // committed to a contact move: holds its ground
    ang = RtAtan2(FROM_FX(t->x - atk->x), FROM_FX(t->y - atk->y));
    t->vx += RtCos(ang) * strength;
    t->vy += RtSin(ang) * strength;
}

static void HitStun(struct RtActor *t, u8 frames)
{
    if (t->state == AST_NORMAL || t->state == AST_HURT)
    {
        t->state = AST_HURT;
        t->stateTimer = frames;
        RtActor_SetAnim(t, RTA_HURT, FALSE);
    }
}

static bool8 SecondaryRoll(struct RtActor *atk, struct RtActor *t, u16 move)
{
    u32 chance = gBattleMoves[move].secondaryEffectChance;
    if (t && t->ability == ABILITY_SHIELD_DUST && t != atk)
        return FALSE;
    if (chance == 0)
        return FALSE;
    if (atk->ability == ABILITY_SERENE_GRACE)
        chance *= 2;
    return (Random() % 100) < chance;
}

static void ConfuseTarget(struct RtActor *t)
{
    if (t->ability == ABILITY_OWN_TEMPO || t->confuse)
        return;
    if (t->substituteHp)
        return;
    t->confuse = 150 + Random() % 150;
    RtFx_Burst(FX_DIZZY8, FXP_GOLD, t->x, t->y - TO_FX(t->height), 4, 1, 30);
}

static void FlinchTarget(struct RtActor *t)
{
    if (t->ability == ABILITY_INNER_FOCUS)
        return;
    t->flinch = 24;
    HitStun(t, 20);
}

static void TrapTarget(struct RtActor *atk, struct RtActor *t, u16 move)
{
    if (t->trap)
        return;
    t->trap = 240 + Random() % 180;
    if (atk->holdEffect == HOLD_EFFECT_NONE && 0)
        t->trap += 0;
    t->trapBy = atk->id;
    t->trapMove = move;
}

static void ApplyStatDown(struct RtActor *atk, struct RtActor *t, u8 stage, s8 n)
{
    RtActor_ChangeStage(t, stage, -n, atk->id, TRUE);
    if (stage == STG_SPE)
        t->slowTimer = 90;
}

static void Secondary(struct RtActor *atk, struct RtActor *t, u16 move, s32 dmg)
{
    u8 eff = gBattleMoves[move].effect;
    if (!RtActor_IsAlive(t) && eff != EFFECT_ABSORB && eff != EFFECT_DREAM_EATER && eff != EFFECT_RECOIL && eff != EFFECT_DOUBLE_EDGE
     && eff != EFFECT_PAY_DAY && eff != EFFECT_THIEF && eff != EFFECT_SUPERPOWER && eff != EFFECT_OVERHEAT && eff != EFFECT_ATTACK_UP_HIT
     && eff != EFFECT_DEFENSE_UP_HIT && eff != EFFECT_ALL_STATS_UP_HIT)
        return;
    switch (eff)
    {
    case EFFECT_POISON_HIT:
    case EFFECT_TWINEEDLE:
    case EFFECT_POISON_TAIL:
        if (SecondaryRoll(atk, t, move)) RtActor_ApplyStatus(t, MST_PSN, atk->id);
        break;
    case EFFECT_POISON_FANG:
        if (SecondaryRoll(atk, t, move)) RtActor_ApplyStatus(t, MST_TOX, atk->id);
        break;
    case EFFECT_BURN_HIT:
    case EFFECT_BLAZE_KICK:
    case EFFECT_THAW_HIT:
        if (SecondaryRoll(atk, t, move)) RtActor_ApplyStatus(t, MST_BRN, atk->id);
        break;
    case EFFECT_FREEZE_HIT:
        if (SecondaryRoll(atk, t, move)) RtActor_ApplyStatus(t, MST_FRZ, atk->id);
        break;
    case EFFECT_PARALYZE_HIT:
    case EFFECT_THUNDER:
        if (SecondaryRoll(atk, t, move)) RtActor_ApplyStatus(t, MST_PAR, atk->id);
        break;
    case EFFECT_TRI_ATTACK:
        if (SecondaryRoll(atk, t, move))
        {
            static const u8 s[] = {MST_PAR, MST_BRN, MST_FRZ};
            RtActor_ApplyStatus(t, s[Random() % 3], atk->id);
        }
        break;
    case EFFECT_CONFUSE_HIT:
        if (SecondaryRoll(atk, t, move)) ConfuseTarget(t);
        break;
    case EFFECT_FLINCH_HIT:
    case EFFECT_FLINCH_MINIMIZE_HIT:
    case EFFECT_TWISTER:
    case EFFECT_SNORE:
        if (SecondaryRoll(atk, t, move)) FlinchTarget(t);
        break;
    case EFFECT_FAKE_OUT:
        FlinchTarget(t);
        break;
    case EFFECT_ATTACK_DOWN_HIT: if (SecondaryRoll(atk, t, move)) ApplyStatDown(atk, t, STG_ATK, 1); break;
    case EFFECT_DEFENSE_DOWN_HIT: if (SecondaryRoll(atk, t, move)) ApplyStatDown(atk, t, STG_DEF, 1); break;
    case EFFECT_SPEED_DOWN_HIT: if (SecondaryRoll(atk, t, move)) ApplyStatDown(atk, t, STG_SPE, 1); break;
    case EFFECT_SPECIAL_ATTACK_DOWN_HIT: if (SecondaryRoll(atk, t, move)) ApplyStatDown(atk, t, STG_SPA, 1); break;
    case EFFECT_SPECIAL_DEFENSE_DOWN_HIT: if (SecondaryRoll(atk, t, move)) ApplyStatDown(atk, t, STG_SPD, 1); break;
    case EFFECT_ACCURACY_DOWN_HIT: if (SecondaryRoll(atk, t, move)) ApplyStatDown(atk, t, STG_ACC, 1); break;
    case EFFECT_EVASION_DOWN_HIT: if (SecondaryRoll(atk, t, move)) ApplyStatDown(atk, t, STG_EVA, 1); break;
    case EFFECT_ATTACK_UP_HIT: if (SecondaryRoll(atk, NULL, move)) RtActor_ChangeStage(atk, STG_ATK, 1, atk->id, TRUE); break;
    case EFFECT_DEFENSE_UP_HIT: if (SecondaryRoll(atk, NULL, move)) RtActor_ChangeStage(atk, STG_DEF, 1, atk->id, TRUE); break;
    case EFFECT_ALL_STATS_UP_HIT:
        if (SecondaryRoll(atk, NULL, move))
        {
            u8 k;
            for (k = STG_ATK; k <= STG_SPD; k++)
                RtActor_ChangeStage(atk, k, 1, atk->id, k == STG_ATK);
        }
        break;
    case EFFECT_ABSORB:
    case EFFECT_DREAM_EATER:
        {
            s32 h = dmg / 2 ? dmg / 2 : 1;
            if (t->ability == ABILITY_LIQUID_OOZE)
                RtActor_Damage(atk, h, t->id, FALSE);
            else
                RtActor_Heal(atk, h);
            RtFx_Burst(FX_ORB8, FXP_HEAL, t->x, t->y - TO_FX(t->height / 2), 5, 1, 24);
        }
        break;
    case EFFECT_RECOIL:
        if (atk->ability != ABILITY_ROCK_HEAD || move == MOVE_STRUGGLE)
            RtActor_Damage(atk, dmg / 4 ? dmg / 4 : 1, t->id, FALSE);
        break;
    case EFFECT_DOUBLE_EDGE:
        if (atk->ability != ABILITY_ROCK_HEAD)
            RtActor_Damage(atk, dmg / 3 ? dmg / 3 : 1, t->id, FALSE);
        break;
    case EFFECT_TRAP:
        TrapTarget(atk, t, move);
        break;
    case EFFECT_PAY_DAY:
        gRt->payDay += atk->level * 5;
        RtFx_Burst(FX_COIN8, FXP_GOLD, t->x, t->y - TO_FX(t->height / 2), 4, 2, 24);
        break;
    case EFFECT_RAPID_SPIN:
        atk->leechSeed = 0;
        atk->trap = 0;
        gRt->sides[atk->side].spikes = 0;
        break;
    case EFFECT_KNOCK_OFF:
        if (t->heldItem && t->ability != ABILITY_STICKY_HOLD)
        {
            NameOf(t, gStringVar1);
            RtUi_ToastFmt(sText_KnockedOff, 60);
            t->heldItem = ITEM_NONE;
            t->holdEffect = 0;
        }
        break;
    case EFFECT_THIEF:
        if (t->heldItem && !atk->heldItem && t->ability != ABILITY_STICKY_HOLD && t->heldItem != ITEM_ENIGMA_BERRY)
        {
            NameOf(atk, gStringVar1);
            CopyItemName(t->heldItem, gStringVar2);
            RtUi_ToastFmt(sText_StoleItem, 60);
            atk->heldItem = t->heldItem;
            atk->holdEffect = GetItemHoldEffect(atk->heldItem);
            atk->holdParam = GetItemHoldEffectParam(atk->heldItem);
            t->heldItem = ITEM_NONE;
            t->holdEffect = 0;
        }
        break;
    case EFFECT_SUPERPOWER:
        RtActor_ChangeStage(atk, STG_ATK, -1, atk->id, TRUE);
        RtActor_ChangeStage(atk, STG_DEF, -1, atk->id, FALSE);
        break;
    case EFFECT_OVERHEAT:
        RtActor_ChangeStage(atk, STG_SPA, -2, atk->id, TRUE);
        break;
    case EFFECT_BRICK_BREAK:
        gRt->sides[t->side].reflect = 0;
        gRt->sides[t->side].lightScreen = 0;
        break;
    case EFFECT_SMELLINGSALT:
        if (t->mstatus == MST_PAR)
            RtActor_CureStatus(t);
        break;
    case EFFECT_SECRET_POWER:
        if (SecondaryRoll(atk, t, move))
        {
            switch (gBattleEnvironment)
            {
            case BATTLE_ENVIRONMENT_GRASS: case BATTLE_ENVIRONMENT_LONG_GRASS: RtActor_ApplyStatus(t, MST_SLP, atk->id); break;
            case BATTLE_ENVIRONMENT_SAND: ApplyStatDown(atk, t, STG_ACC, 1); break;
            case BATTLE_ENVIRONMENT_UNDERWATER: ApplyStatDown(atk, t, STG_DEF, 1); break;
            case BATTLE_ENVIRONMENT_WATER: ApplyStatDown(atk, t, STG_ATK, 1); break;
            case BATTLE_ENVIRONMENT_POND: ApplyStatDown(atk, t, STG_SPE, 1); break;
            case BATTLE_ENVIRONMENT_MOUNTAIN: ConfuseTarget(t); break;
            case BATTLE_ENVIRONMENT_CAVE: FlinchTarget(t); break;
            default: RtActor_ApplyStatus(t, MST_PAR, atk->id); break;
            }
        }
        break;
    case EFFECT_RAGE:
        atk->rage = 600;
        break;
    }
    // thaw on fire hit
    if (t->mstatus == MST_FRZ && RtMove_TypeOf(atk, move) == TYPE_FIRE)
    {
        RtActor_CureStatus(t);
    }
    // king's rock
    if (atk->holdEffect == HOLD_EFFECT_FLINCH && (gBattleMoves[move].flags & FLAG_KINGS_ROCK_AFFECTED) && (Random() % 100) < atk->holdParam)
        FlinchTarget(t);
}

static void ContactAbilities(struct RtActor *atk, struct RtActor *t, u16 move)
{
    if (!(gBattleMoves[move].flags & FLAG_MAKES_CONTACT) || !RtActor_IsAlive(atk))
        return;
    switch (t->ability)
    {
    case ABILITY_STATIC:
        if (Random() % 3 == 0) RtActor_ApplyStatus(atk, MST_PAR, t->id);
        break;
    case ABILITY_FLAME_BODY:
        if (Random() % 3 == 0) RtActor_ApplyStatus(atk, MST_BRN, t->id);
        break;
    case ABILITY_POISON_POINT:
        if (Random() % 3 == 0) RtActor_ApplyStatus(atk, MST_PSN, t->id);
        break;
    case ABILITY_EFFECT_SPORE:
        if (Random() % 10 == 0)
        {
            static const u8 s[] = {MST_PSN, MST_PAR, MST_SLP};
            RtActor_ApplyStatus(atk, s[Random() % 3], t->id);
        }
        break;
    case ABILITY_ROUGH_SKIN:
        RtActor_Damage(atk, atk->maxHp / 16 ? atk->maxHp / 16 : 1, t->id, FALSE);
        break;
    case ABILITY_CUTE_CHARM:
        if (Random() % 3 == 0 && !atk->attract && atk->ability != ABILITY_OBLIVIOUS)
        {
            u8 g1 = GetMonGender(atk->mon), g2 = GetMonGender(t->mon);
            if (g1 != MON_GENDERLESS && g2 != MON_GENDERLESS && g1 != g2)
            {
                atk->attract = 600;
                atk->attractBy = t->id;
            }
        }
        break;
    }
}

// Status-only move effects on a target.
static bool8 StatusEffectOnTarget(struct RtActor *atk, struct RtActor *t, u16 move)
{
    u8 eff = gBattleMoves[move].effect;
    u8 type = RtMove_TypeOf(atk, move);
    if (t->substituteHp && eff != EFFECT_ROAR && eff != EFFECT_TRANSFORM)
    {
        RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_BLOCK);
        return FALSE;
    }
    switch (eff)
    {
    case EFFECT_SLEEP:
        if (move == MOVE_GRASS_WHISTLE || move == MOVE_SING)
            if (t->ability == ABILITY_SOUNDPROOF) return FALSE;
        RtActor_ApplyStatus(t, MST_SLP, atk->id);
        break;
    case EFFECT_POISON:
        RtActor_ApplyStatus(t, MST_PSN, atk->id);
        break;
    case EFFECT_TOXIC:
        RtActor_ApplyStatus(t, MST_TOX, atk->id);
        break;
    case EFFECT_PARALYZE:
        if (move == MOVE_THUNDER_WAVE && (RtActor_HasType(t, TYPE_GROUND) || t->ability == ABILITY_VOLT_ABSORB))
        {
            RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_IMMUNE);
            return FALSE;
        }
        RtActor_ApplyStatus(t, MST_PAR, atk->id);
        break;
    case EFFECT_WILL_O_WISP:
        RtActor_ApplyStatus(t, MST_BRN, atk->id);
        break;
    case EFFECT_CONFUSE:
        if (move == MOVE_SUPERSONIC && t->ability == ABILITY_SOUNDPROOF) return FALSE;
        ConfuseTarget(t);
        break;
    case EFFECT_SWAGGER:
        RtActor_ChangeStage(t, STG_ATK, 2, atk->id, TRUE);
        ConfuseTarget(t);
        break;
    case EFFECT_FLATTER:
        RtActor_ChangeStage(t, STG_SPA, 1, atk->id, TRUE);
        ConfuseTarget(t);
        break;
    case EFFECT_TEETER_DANCE:
        ConfuseTarget(t);
        break;
    case EFFECT_ATTACK_DOWN: ApplyStatDown(atk, t, STG_ATK, 1); break;
    case EFFECT_DEFENSE_DOWN: ApplyStatDown(atk, t, STG_DEF, 1); break;
    case EFFECT_SPEED_DOWN: ApplyStatDown(atk, t, STG_SPE, 1); break;
    case EFFECT_SPECIAL_ATTACK_DOWN: ApplyStatDown(atk, t, STG_SPA, 1); break;
    case EFFECT_SPECIAL_DEFENSE_DOWN: ApplyStatDown(atk, t, STG_SPD, 1); break;
    case EFFECT_ACCURACY_DOWN: ApplyStatDown(atk, t, STG_ACC, 1); break;
    case EFFECT_EVASION_DOWN: ApplyStatDown(atk, t, STG_EVA, 1); break;
    case EFFECT_ATTACK_DOWN_2: ApplyStatDown(atk, t, STG_ATK, 2); break;
    case EFFECT_DEFENSE_DOWN_2: ApplyStatDown(atk, t, STG_DEF, 2); break;
    case EFFECT_SPEED_DOWN_2: ApplyStatDown(atk, t, STG_SPE, 2); break;
    case EFFECT_SPECIAL_ATTACK_DOWN_2: ApplyStatDown(atk, t, STG_SPA, 2); break;
    case EFFECT_SPECIAL_DEFENSE_DOWN_2: ApplyStatDown(atk, t, STG_SPD, 2); break;
    case EFFECT_ACCURACY_DOWN_2: ApplyStatDown(atk, t, STG_ACC, 2); break;
    case EFFECT_EVASION_DOWN_2: ApplyStatDown(atk, t, STG_EVA, 2); break;
    case EFFECT_TICKLE:
        ApplyStatDown(atk, t, STG_ATK, 1);
        ApplyStatDown(atk, t, STG_DEF, 1);
        break;
    case EFFECT_LEECH_SEED:
        if (RtActor_HasType(t, TYPE_GRASS) || t->leechSeed)
        {
            RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_IMMUNE);
            return FALSE;
        }
        t->leechSeed = 900;
        t->seededBy = atk->id;
        break;
    case EFFECT_DISABLE:
        if (t->lastMove && !t->disable)
        {
            t->disable = 420;
            t->disabledMove = t->lastMove;
        }
        break;
    case EFFECT_ENCORE:
        if (t->lastMove && !t->encore)
        {
            s32 i;
            for (i = 0; i < MAX_MON_MOVES; i++)
                if (t->moves[i] == t->lastMove)
                {
                    t->encore = 360;
                    t->encoreSlot = i;
                }
        }
        break;
    case EFFECT_TAUNT:
        t->taunt = 300;
        break;
    case EFFECT_TORMENT:
        t->torment = 600;
        break;
    case EFFECT_ATTRACT:
        {
            u8 g1 = GetMonGender(atk->mon), g2 = GetMonGender(t->mon);
            if (t->ability == ABILITY_OBLIVIOUS || g1 == MON_GENDERLESS || g2 == MON_GENDERLESS || g1 == g2)
            {
                RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_IMMUNE);
                return FALSE;
            }
            t->attract = 600;
            t->attractBy = atk->id;
        }
        break;
    case EFFECT_YAWN:
        if (!t->mstatus && !t->yawn)
            t->yawn = 120;
        break;
    case EFFECT_MEAN_LOOK:
        t->meanLook = 3600;
        break;
    case EFFECT_LOCK_ON:
        atk->lockOn = 240;
        break;
    case EFFECT_FORESIGHT:
        t->foresight = 1800;
        t->stages[STG_EVA] = 0;
        break;
    case EFFECT_NIGHTMARE:
        if (t->mstatus == MST_SLP)
            t->nightmare = 1;
        break;
    case EFFECT_SPITE:
        {
            s32 i;
            for (i = 0; i < MAX_MON_MOVES; i++)
                if (t->moves[i] == t->lastMove && t->lastMove)
                    t->cd[i] += t->cdMax[i] * 2;
        }
        break;
    case EFFECT_PAIN_SPLIT:
        {
            u16 avg = (atk->hp + t->hp) / 2;
            atk->hp = avg > atk->maxHp ? atk->maxHp : avg;
            t->hp = avg > t->maxHp ? t->maxHp : avg;
            atk->hpBarDirty = t->hpBarDirty = TRUE;
            gRt->hudDirty = TRUE;
        }
        break;
    case EFFECT_TRICK:
        if (t->ability != ABILITY_STICKY_HOLD && (atk->heldItem || t->heldItem) && !(gBattleTypeFlags & BATTLE_TYPE_FRONTIER))
        {
            u16 it = atk->heldItem;
            atk->heldItem = t->heldItem;
            t->heldItem = it;
            atk->holdEffect = GetItemHoldEffect(atk->heldItem);
            atk->holdParam = GetItemHoldEffectParam(atk->heldItem);
            t->holdEffect = GetItemHoldEffect(t->heldItem);
            t->holdParam = GetItemHoldEffectParam(t->heldItem);
        }
        break;
    case EFFECT_ROLE_PLAY:
        if (t->ability != ABILITY_WONDER_GUARD)
            atk->ability = t->ability;
        break;
    case EFFECT_SKILL_SWAP:
        if (t->ability != ABILITY_WONDER_GUARD && atk->ability != ABILITY_WONDER_GUARD)
        {
            u8 ab = atk->ability;
            atk->ability = t->ability;
            t->ability = ab;
        }
        break;
    case EFFECT_MIMIC:
    case EFFECT_SKETCH:
        if (t->lastMove && t->lastMove != MOVE_SKETCH && t->lastMove != MOVE_STRUGGLE && t->lastMove != MOVE_MIMIC)
        {
            u8 slot = atk->act.slot;
            if (slot < MAX_MON_MOVES)
            {
                atk->moves[slot] = t->lastMove;
                atk->cdMax[slot] = RtMove_Cooldown(t->lastMove, 0, slot);
                if (eff == EFFECT_SKETCH && atk->side == RT_SIDE_PLAYER && !(gBattleTypeFlags & BATTLE_TYPE_FRONTIER))
                    SetMonMoveSlot(atk->mon, t->lastMove, slot);
                NameOf(atk, gStringVar1);
                StringCopy(gStringVar2, gMoveNames[t->lastMove]);
                RtUi_ToastFmt(eff == EFFECT_SKETCH ? sText_Sketched : sText_Learned, 70);
                RtUi_RefreshMoves();
            }
        }
        break;
    case EFFECT_ROAR:
        if (t->ingrain || t->ability == ABILITY_SUCTION_CUPS)
            return FALSE;
        if (!gRt->isTrainer && !(gBattleTypeFlags & BATTLE_TYPE_FIRST_BATTLE))
        {
            Rt_EndBattle(t->side == RT_SIDE_ENEMY ? B_OUTCOME_MON_TELEPORTED : B_OUTCOME_PLAYER_TELEPORTED);
            return TRUE;
        }
        // trainers: huge knockback and stun
        Knockback(atk, t, 10);
        HitStun(t, 40);
        break;
    case EFFECT_PSYCH_UP:
        memcpy(atk->stages, t->stages, sizeof(atk->stages));
        RtFx_StatArrows(atk, TRUE);
        break;
    case EFFECT_TRANSFORM:
        {
            s32 i;
            atk->transformSpecies = t->species;
            atk->gfxDex = t->gfxDex;
            atk->types[0] = t->types[0];
            atk->types[1] = t->types[1];
            atk->atk = t->atk;
            atk->def = t->def;
            atk->spa = t->spa;
            atk->spd = t->spd;
            atk->spe = t->spe;
            atk->ability = t->ability;
            memcpy(atk->stages, t->stages, sizeof(atk->stages));
            for (i = 0; i < MAX_MON_MOVES; i++)
            {
                atk->moves[i] = t->moves[i];
                atk->cdMax[i] = RtMove_Cooldown(t->moves[i], 0, i);
                atk->cd[i] = 0;
            }
            atk->radius = gRtSpeciesGfx[atk->gfxDex].radius;
            atk->height = gRtSpeciesGfx[atk->gfxDex].height;
            atk->curFrame = 0xFFFF;
            atk->palDirty = TRUE;
            NameOf(atk, gStringVar1);
            RtUi_ToastFmt(sText_Transformed, 60);
            RtUi_RefreshMoves();
        }
        break;
    default:
        return TRUE;
    }
    (void)type;
    return TRUE;
}

static bool8 TryHitActor(struct RtProj *p, struct RtActor *atk, struct RtActor *t)
{
    u16 move = p->move;
    const struct BattleMove *bm = &gBattleMoves[move];
    u8 type = RtMove_TypeOf(atk, move);
    u8 eff = 10, crit = FALSE;
    s32 dmg = 0;
    u16 power;
    u8 effect = bm->effect;

    if (!RtActor_IsAlive(atk) && effect != EFFECT_EXPLOSION && p->kind != PK_ZONE && p->kind != PK_STRIKE)
        return FALSE;
    t->lastHitBy = atk->id;

    // Protect / Detect
    if (t->protect && t->protectKind == 0 && (bm->flags & FLAG_PROTECT_AFFECTED))
    {
        RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_BLOCK);
        RtFx_Spawn(FX_SHIELD32, FXP_HEAL, t->x, t->y - TO_FX(t->height / 2), 8);
        PlaySE(SE_M_DETECT);
        return TRUE;
    }
    // Magic Coat reflects status moves
    if (bm->power == 0 && t->magicCoat && (bm->flags & FLAG_MAGIC_COAT_AFFECTED))
    {
        t->magicCoat = 0;
        RtFx_Spawn(FX_SHIELD32, FXP_GOLD, t->x, t->y - TO_FX(t->height / 2), 10);
        StatusEffectOnTarget(t, atk, move);
        return TRUE;
    }
    if (IsSoundMove(move) && t->ability == ABILITY_SOUNDPROOF)
    {
        RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_IMMUNE);
        return TRUE;
    }
    // accuracy
    if (!AccuracyCheck(atk, t, move))
    {
        RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_MISS);
        atk->furyCutter = 0;
        return TRUE;
    }

    // absorb abilities
    if (bm->power || effect == EFFECT_PARALYZE)
    {
        if (type == TYPE_ELECTRIC && t->ability == ABILITY_VOLT_ABSORB && t->side != atk->side)
        {
            RtActor_Heal(t, t->maxHp / 4);
            return TRUE;
        }
        if (type == TYPE_WATER && t->ability == ABILITY_WATER_ABSORB && t->side != atk->side)
        {
            RtActor_Heal(t, t->maxHp / 4);
            return TRUE;
        }
        if (type == TYPE_FIRE && t->ability == ABILITY_FLASH_FIRE && t->mstatus != MST_FRZ)
        {
            t->flashFire = 1;
            RtFx_Burst(FX_FLAME16, FXP_FIRE, t->x, t->y - TO_FX(t->height / 2), 4, 2, 20);
            RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_IMMUNE);
            return TRUE;
        }
    }

    // pure status move
    if (bm->power == 0)
    {
        bool8 ok = StatusEffectOnTarget(atk, t, move);
        RtVfx_SetHit(atk->id, t->id);
    RtVfx_Impact(move, Def(move)->impact, p->pal, t->x, t->y - TO_FX(t->height / 2));
        return ok || TRUE;
    }

    // counter / mirror coat window: negate and reflect double
    if (t->counterWin)
    {
        bool8 phys = RtMove_IsPhysical(move, type);
        if ((t->counterKind == 0 && phys) || (t->counterKind == 1 && !phys))
        {
            s32 d = RtMove_CalcDamage(atk, t, move, CalcPower(atk, t, move), NULL, NULL);
            t->counterWin = 0;
            RtFx_Impact(RTI_STAR, FXP_FIGHTING, atk->x, atk->y - TO_FX(atk->height / 2));
            RtActor_Damage(atk, d * 2, t->id, FALSE);
            atk->flash = 8;
            Knockback(t, atk, 6);
            PlaySE(SE_M_VITAL_THROW);
            return TRUE;
        }
    }

    // fixed / special damage
    switch (effect)
    {
    case EFFECT_LEVEL_DAMAGE:
        dmg = atk->level;
        eff = RtTypeEffectiveness(type, t) ? 10 : 0;
        break;
    case EFFECT_DRAGON_RAGE:
        dmg = 40;
        eff = RtTypeEffectiveness(type, t) ? 10 : 0;
        break;
    case EFFECT_SONICBOOM:
        dmg = 20;
        eff = RtTypeEffectiveness(type, t) ? 10 : 0;
        break;
    case EFFECT_PSYWAVE:
        dmg = atk->level * (Random() % 11 * 10 + 50) / 100;
        eff = RtTypeEffectiveness(type, t) ? 10 : 0;
        break;
    case EFFECT_SUPER_FANG:
        dmg = t->hp / 2 ? t->hp / 2 : 1;
        eff = RtTypeEffectiveness(type, t) ? 10 : 0;
        break;
    case EFFECT_ENDEAVOR:
        dmg = t->hp > atk->hp ? t->hp - atk->hp : 0;
        eff = RtTypeEffectiveness(type, t) ? 10 : 0;
        if (dmg == 0)
        {
            RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_MISS);
            return TRUE;
        }
        break;
    case EFFECT_OHKO:
        eff = RtTypeEffectiveness(type, t);
        if (eff && (t->ability == ABILITY_STURDY || t->level > atk->level || (Random() % 100) >= (u32)(atk->level - t->level + 30)))
        {
            RtFx_Word(t->x, t->y - TO_FX(t->height + 8), t->ability == ABILITY_STURDY ? FX_W_BLOCK : FX_W_MISS);
            return TRUE;
        }
        dmg = t->hp;
        if (eff)
            RtUi_Toast(sText_OhKo, 70);
        break;
    case EFFECT_DREAM_EATER:
        if (t->mstatus != MST_SLP)
        {
            RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_IMMUNE);
            return TRUE;
        }
        // fallthrough
    default:
        power = CalcPower(atk, t, move);
        if (effect == EFFECT_PRESENT && atk->act.power == 0)
        {
            RtActor_Heal(t, t->maxHp / 4);
            return TRUE;
        }
        dmg = RtMove_CalcDamage(atk, t, move, power, &eff, &crit);
        break;
    }
    if (dmg > 0 && effect != EFFECT_OHKO)
    {
        // shots lose some punch over a long flight
        if (p->kind == PK_BULLET && !(p->flags & RTMF_ARC) && !(p->param & (4 | 8)) && p->len)
        {
            u32 f = (u32)p->age * 256 / p->len;
            if (f > 256)
                f = 256;
            if (f > FALLOFF_START)
                dmg = dmg * (100 - (100 - FALLOFF_MIN) * (f - FALLOFF_START) / (256 - FALLOFF_START)) / 100;
        }
        // contact armor
        if (RtMove_ContactArmor(t) && t != atk)
            dmg = dmg * CONTACT_ARMOR / 100;
        if (dmg < 1)
            dmg = 1;
    }
    if (effect == EFFECT_FAKE_OUT && gRt->frame - atk->sentOutFrame > 240)
    {
        RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_MISS);
        return TRUE;
    }
    if (eff == 0)
    {
        RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_IMMUNE);
        if (effect == EFFECT_RECOIL_IF_MISS)
            RtActor_Damage(atk, atk->maxHp / 8, atk->id, FALSE);
        return TRUE;
    }
    if (effect == EFFECT_FALSE_SWIPE && dmg >= t->hp)
        dmg = t->hp - 1;

    // substitute
    if (t->substituteHp && t != atk)
    {
        if (dmg >= t->substituteHp)
        {
            t->substituteHp = 0;
            RtFx_Burst(FX_DOLL16, FXP_NORMAL, t->x, t->y - TO_FX(t->height / 2), 3, 2, 20);
            t->palDirty = TRUE;
        }
        else
        {
            t->substituteHp -= dmg;
        }
        RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_BLOCK);
        RtVfx_SetHit(atk->id, t->id);
    RtVfx_Impact(move, Def(move)->impact, p->pal, t->x, t->y - TO_FX(t->height / 2));
        return TRUE;
    }

    // bide absorbs damage to release later
    if (t->bide)
        t->bideDmg += dmg;

    // endure / focus band
    if (dmg >= t->hp)
    {
        bool8 endure = t->protect && t->protectKind == 1;
        if (endure || (t->holdEffect == HOLD_EFFECT_FOCUS_BAND && (Random() % 100) < t->holdParam))
        {
            static const u8 sText_FocusBand[] = _("{STR_VAR_1} hung on using its FOCUS BAND!");
            dmg = t->hp - 1;
            RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_BLOCK);
            if (!endure)
                ToastName(t, sText_FocusBand, 70);
        }
    }

    // apply
    t->damageTakenThisCast += dmg;
    t->lastDamageTaken = dmg;
    t->lastDamageCat = RtMove_IsPhysical(move, type) ? 0 : 1;
    if (t->side == RT_SIDE_PLAYER)
        gBattleResults.playerMonWasDamaged = TRUE;
    RtVfx_SetHit(atk->id, t->id);
    RtVfx_Impact(move, Def(move)->impact, p->pal, t->x, t->y - TO_FX(t->height / 2));
    if (crit)
        RtFx_Word(t->x, t->y - TO_FX(t->height + 20), FX_W_CRIT);
    ShowEffectiveness(t, eff);
    t->flash = 6;
    if (dmg >= t->hp)
    {
        t->hp = 0;
        RtFx_Number(t->x, t->y - TO_FX(t->height + 4), dmg, crit ? 3 : (eff > 10 ? 1 : 0));
        Knockback(atk, t, 8);
        RtFx_Shake(3, 12);
        gRt->pauseFrames = 6;
        RtActor_Faint(t);
    }
    else
    {
        bool8 big = (dmg * 4 >= t->maxHp) || crit || eff > 10;
        t->hp -= dmg;
        t->hpBarDirty = TRUE;
        gRt->hudDirty = TRUE;
        RtFx_Number(t->x, t->y - TO_FX(t->height + 4), dmg, crit ? 3 : (eff > 10 ? 1 : 0));
        Knockback(atk, t, big ? 6 : 3);
        HitStun(t, big ? 16 : 8);
        if (big)
        {
            RtFx_Shake(2, 6);
            gRt->pauseFrames = 3;
        }
        if (t->rage)
            RtActor_ChangeStage(t, STG_ATK, 1, t->id, TRUE);
        RtMove_CheckBerries(t);
    }
    if (t->mstatus == MST_FRZ && type == TYPE_FIRE)
        RtActor_CureStatus(t);
    if (effect == EFFECT_FURY_CUTTER)
        atk->furyCutter++;
    if (atk->holdEffect == HOLD_EFFECT_SHELL_BELL && dmg > 0)
        RtActor_Heal(atk, dmg / atk->holdParam ? dmg / atk->holdParam : 1);
    Secondary(atk, t, move, dmg);
    ContactAbilities(atk, t, move);
    // grudge / destiny bond are processed on faint
    return TRUE;
}

void RtMove_OnActorHit(struct RtActor *atk, struct RtActor *def, u16 move, struct RtProj *p)
{
    TryHitActor(p, atk, def);
}

void RtMove_OnFaint(struct RtActor *a)
{
    struct RtActor *k;
    if (a->lastHitBy >= RT_MAX_ACTORS)
        return;
    k = &gRt->actors[a->lastHitBy];
    if (k == a || !k->active || !RtActor_IsAlive(k))
        return;
    if (a->destinyBond && k->side != a->side)
    {
        NameOf(a, gStringVar1);
        RtUi_ToastFmt(sText_DestinyBond, 70);
        RtActor_Damage(k, k->hp, a->id, TRUE);
    }
    if (a->grudge && k->side != a->side)
    {
        s32 i;
        for (i = 0; i < MAX_MON_MOVES; i++)
            if (k->moves[i] == k->lastMove)
                k->cd[i] = 0xFFFF / 2;
    }
}

// ---------------------------------------------------------------------------
// Casting
// ---------------------------------------------------------------------------
static u8 PickTargetAngle(struct RtActor *a, u8 angle, s32 range, s32 *tx, s32 *ty, u8 *tid)
{
    // assisted target point along the aim direction
    s32 i, best = 0x7FFF;
    struct RtActor *bt = NULL;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *b = &gRt->actors[i];
        s32 dx, dy, d;
        s8 diff;
        if (!b->active || b->side == a->side || !RtActor_IsAlive(b) || b->state == AST_GONE)
            continue;
        dx = FROM_FX(b->x - a->x);
        dy = FROM_FX(b->y - a->y);
        d = RtDist(dx, dy);
        diff = (s8)(RtAtan2(dx, dy) - angle);
        if (diff < 0) diff = -diff;
        if (diff <= 26 && d <= range + 24 && d < best)
        {
            best = d;
            bt = b;
        }
    }
    if (bt)
    {
        *tx = bt->x;
        *ty = bt->y;
        *tid = bt->id;
        return RtAtan2(FROM_FX(bt->x - a->x), FROM_FX(bt->y - a->y));
    }
    *tx = a->x + RtCos(angle) * range;
    *ty = a->y + RtSin(angle) * range;
    *tid = 0xFF;
    {
        // clamp inside arena
        s32 x = FROM_FX(*tx), y = FROM_FX(*ty);
        if (x < ARENA_BORDER * 16 + 8) x = ARENA_BORDER * 16 + 8;
        if (x > ARENA_W - ARENA_BORDER * 16 - 8) x = ARENA_W - ARENA_BORDER * 16 - 8;
        if (y < ARENA_BORDER * 16 + 8) y = ARENA_BORDER * 16 + 8;
        if (y > ARENA_H - ARENA_BORDER * 16 - 8) y = ARENA_H - ARENA_BORDER * 16 - 8;
        *tx = TO_FX(x);
        *ty = TO_FX(y);
    }
    return angle;
}

static u16 ResolveCalledMove(struct RtActor *a, u16 move)
{
    switch (gBattleMoves[move].effect)
    {
    case EFFECT_METRONOME:
        return RtMove_PickMetronome();
    case EFFECT_MIRROR_MOVE:
        {
            struct RtActor *t = Rt_NearestFoe(a);
            if (t && t->lastMove && (gBattleMoves[t->lastMove].flags & FLAG_MIRROR_MOVE_AFFECTED))
                return t->lastMove;
            return MOVE_NONE;
        }
    case EFFECT_SLEEP_TALK:
        {
            u16 opts[MAX_MON_MOVES];
            u8 n = 0, i;
            for (i = 0; i < MAX_MON_MOVES; i++)
            {
                u16 m = a->moves[i];
                if (m && m != MOVE_SLEEP_TALK && m != MOVE_BIDE && m != MOVE_FOCUS_PUNCH && gBattleMoves[m].effect != EFFECT_SEMI_INVULNERABLE
                 && gBattleMoves[m].effect != EFFECT_SOLAR_BEAM && gBattleMoves[m].effect != EFFECT_RAZOR_WIND && gBattleMoves[m].effect != EFFECT_SKULL_BASH
                 && gBattleMoves[m].effect != EFFECT_SKY_ATTACK && m != MOVE_METRONOME && m != MOVE_ASSIST && m != MOVE_MIRROR_MOVE)
                    opts[n++] = m;
            }
            return n ? opts[Random() % n] : MOVE_NONE;
        }
    case EFFECT_NATURE_POWER:
        switch (gBattleEnvironment)
        {
        case BATTLE_ENVIRONMENT_GRASS: return MOVE_STUN_SPORE;
        case BATTLE_ENVIRONMENT_LONG_GRASS: return MOVE_RAZOR_LEAF;
        case BATTLE_ENVIRONMENT_SAND: return MOVE_EARTHQUAKE;
        case BATTLE_ENVIRONMENT_UNDERWATER: return MOVE_HYDRO_PUMP;
        case BATTLE_ENVIRONMENT_WATER: return MOVE_SURF;
        case BATTLE_ENVIRONMENT_POND: return MOVE_BUBBLE_BEAM;
        case BATTLE_ENVIRONMENT_MOUNTAIN: return MOVE_ROCK_SLIDE;
        case BATTLE_ENVIRONMENT_CAVE: return MOVE_SHADOW_BALL;
        default: return MOVE_SWIFT;
        }
    case EFFECT_ASSIST:
        {
            u16 opts[20];
            u8 n = 0, i, j;
            struct Pokemon *party = a->side == RT_SIDE_PLAYER ? gPlayerParty : gEnemyParty;
            for (i = 0; i < PARTY_SIZE; i++)
            {
                if (i == a->partyIdx || GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) == SPECIES_NONE
                 || GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) == SPECIES_EGG)
                    continue;
                for (j = 0; j < MAX_MON_MOVES && n < ARRAY_COUNT(opts); j++)
                {
                    u16 m = GetMonData(&party[i], MON_DATA_MOVE1 + j);
                    if (m && m != MOVE_ASSIST && m != MOVE_METRONOME && m != MOVE_MIRROR_MOVE && m != MOVE_SLEEP_TALK && m != MOVE_COUNTER
                     && m != MOVE_MIRROR_COAT && m != MOVE_PROTECT && m != MOVE_DETECT && m != MOVE_ENDURE && m != MOVE_TRANSFORM)
                        opts[n++] = m;
                }
            }
            return n ? opts[Random() % n] : MOVE_NONE;
        }
    }
    return move;
}

u16 RtMove_PickMetronome(void)
{
    u16 m;
    do
    {
        m = 1 + Random() % (MOVES_COUNT - 1);
    } while (m == MOVE_METRONOME || m == MOVE_STRUGGLE || m == MOVE_SKETCH || m == MOVE_MIMIC || m == MOVE_COUNTER
          || m == MOVE_MIRROR_COAT || m == MOVE_PROTECT || m == MOVE_DETECT || m == MOVE_ENDURE || m == MOVE_DESTINY_BOND
          || m == MOVE_SLEEP_TALK || m == MOVE_THIEF || m == MOVE_FOLLOW_ME || m == MOVE_SNATCH || m == MOVE_HELPING_HAND
          || m == MOVE_COVET || m == MOVE_TRICK || m == MOVE_FOCUS_PUNCH || m == MOVE_ASSIST || m == MOVE_MIRROR_MOVE);
    return m;
}

static bool8 FoeHasPressure(struct RtActor *a)
{
    s32 i;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *o = &gRt->actors[i];
        if (o->active && o->side != a->side && o->state != AST_GONE && RtActor_IsAlive(o) && o->ability == ABILITY_PRESSURE)
            return TRUE;
    }
    return FALSE;
}

bool8 RtMove_TryStart(struct RtActor *a, u8 slot, u8 angle)
{
    u16 move;
    if (slot >= MAX_MON_MOVES)
        return FALSE;
    if (!RtActor_MoveSlotUsable(a, slot))
        return FALSE;
    move = a->moves[slot];

    // truant: every other move the mon loafs around
    if (a->ability == ABILITY_TRUANT && a->truantTimer)
    {
        ToastName(a, sText_Loafing, 50);
        a->gcd = 20;
        return FALSE;
    }
    // infatuation
    if (a->attract && (Random() & 1))
    {
        ToastName(a, sText_Infatuated, 50);
        a->gcd = 40;
        a->cd[slot] = a->cdMax[slot] / 2;
        return FALSE;
    }
    // full paralysis
    if (a->mstatus == MST_PAR && (Random() % 4) == 0)
    {
        ToastName(a, sText_FullyParalyzed, 50);
        a->state = AST_STUN;
        a->stateTimer = 40;
        RtActor_SetAnim(a, RTA_HURT, FALSE);
        RtFx_Burst(FX_BOLT16, FXP_ELECTRIC, a->x, a->y - TO_FX(a->height / 2), 4, 2, 16);
        a->cd[slot] = a->cdMax[slot] / 2;
        return TRUE;
    }
    // confusion: chance to hurt itself
    if (a->confuse && (Random() % 3) == 0)
    {
        s32 dmg = ((2 * a->level / 5 + 2) * 40 * a->atk / (a->def ? a->def : 1)) / 50 + 2;
        RtUi_Toast(sText_HurtItself, 50);
        RtActor_Damage(a, dmg ? dmg : 1, a->id, FALSE);
        a->cd[slot] = a->cdMax[slot] / 2;
        if (!RtActor_IsAlive(a))
            return TRUE; // knocked itself out: keep the faint state
        a->state = AST_STUN;
        a->stateTimer = 30;
        a->flash = 6;
        RtActor_SetAnim(a, RTA_HURT, FALSE);
        RtFx_Burst(FX_DIZZY8, FXP_GOLD, a->x, a->y - TO_FX(a->height), 4, 1, 24);
        a->cd[slot] = a->cdMax[slot] / 2;
        return TRUE;
    }

    // accuracy debuff: aim wobble
    if (a->stages[STG_ACC] < 0)
        angle += (s8)((Random() % (1 - a->stages[STG_ACC] * 8)) - (-a->stages[STG_ACC] * 4));

    a->cd[slot] = a->cdMax[slot];
    {
        static const u8 sCdPct[4] = {160, 130, 110, 100}; // foes recharge slower on easier settings
        a->cd[slot] = (u32)a->cd[slot] * sCdPct[FoeLevel(a)] / 100;
    }
    if (FoeHasPressure(a))
        a->cd[slot] += a->cdMax[slot] / 2;
    if (a->holdEffect == HOLD_EFFECT_QUICK_CLAW && (Random() % 100) < a->holdParam)
    {
        static const u8 sText_QuickClaw[] = _("{STR_VAR_1}'s QUICK CLAW readied its next move!");
        a->cd[slot] = a->cd[slot] / 2;
        NameOf(a, gStringVar1);
        RtUi_ToastFmt(sText_QuickClaw, 60);
        RtFx_Burst(FX_PLUS8, FXP_GOLD, a->x, a->y - TO_FX(a->height / 2), 4, 1, 24);
    }
    a->cd[slot] = (u32)a->cd[slot] * RtExt_CdPct(a) / 100;
    if (a->holdEffect == HOLD_EFFECT_CHOICE_BAND && !a->choiceMove)
        a->choiceMove = move;
    if (a->ability == ABILITY_TRUANT)
        a->truantTimer = a->cdMax[slot] + 30;
    if (move != MOVE_FURY_CUTTER)
        a->furyCutter = 0;
    if (move != MOVE_ROLLOUT && move != MOVE_ICE_BALL)
        a->rolloutHits = 0;
    BeginMove(a, move, angle, slot);
    return TRUE;
}

void RtMove_StartCalled(struct RtActor *a, u16 move, u8 angle)
{
    if (move == MOVE_NONE)
        return;
    BeginMove(a, move, angle, 0xFF);
}

static void BeginMove(struct RtActor *a, u16 move, u8 angle, u8 slot)
{
    const struct RtMoveDef *d;
    u16 orig = move;
    s32 tx, ty;
    u8 tid;

    // moves that call other moves
    move = ResolveCalledMove(a, move);
    if (move == MOVE_NONE)
    {
        RtUi_Toast(sText_Failed, 40);
        a->gcd = 20;
        return;
    }
    d = Def(move);

    if (a->side == RT_SIDE_PLAYER)
        gBattleResults.lastUsedMovePlayer = move;
    else
        gBattleResults.lastUsedMoveOpponent = move;

    {
        extern const u8 gRtDebugEnabled;
        if (gRtDebugEnabled && gRtCastLogN < 64 && a->side == RT_SIDE_ENEMY)
        {
            gRtCastLog[gRtCastLogN][0] = gRt->frame;
            gRtCastLog[gRtCastLogN][1] = move | (a->id << 12);
            gRtCastLogN++;
        }
    }
    a->lastMove = move;
    gRt->lastMoveUsed = move;
    a->state = AST_CAST;
    memset(&a->act, 0, sizeof(a->act));
    a->act.move = move;
    a->act.slot = slot;
    a->act.calledFrom = orig;
    a->act.castId = ++a->castSerial;
    a->act.angle = PickTargetAngle(a, angle, RtMove_GetRange(move), &tx, &ty, &tid);
    a->act.tx = tx;
    a->act.ty = ty;
    a->act.targetId = tid;
    // Leech Seed is planted on the foe: when the aim cone finds nobody, it goes to the nearest foe in reach
    if (gBattleMoves[move].effect == EFFECT_LEECH_SEED && tid == 0xFF && a->side == RT_SIDE_PLAYER)
    {
        struct RtActor *f = Rt_NearestFoe(a);
        if (f != NULL)
        {
            s32 fdx = FROM_FX(f->x - a->x), fdy = FROM_FX(f->y - a->y);
            if (RtDist(fdx, fdy) <= RtMove_GetRange(move) + 24)
            {
                a->act.angle = RtAtan2(fdx, fdy);
                a->act.tx = f->x;
                a->act.ty = f->y;
                a->act.targetId = f->id;
            }
        }
    }
    a->act.sx = a->x;
    a->act.sy = a->y;
    a->act.aimBase = a->act.angle;
    // melee with the target a little out of reach: dash up to it first
    if (d->pattern == RTP_MELEE && tid < RT_MAX_ACTORS && !a->trap && !a->ingrain)
    {
        struct RtActor *t = &gRt->actors[tid];
        s32 dist = RtDist(FROM_FX(t->x - a->x), FROM_FX(t->y - a->y));
        s32 stop = a->radius + MeleeLen(d);
        if (!t->invuln && dist > stop + 4 && dist <= stop + LUNGE_MAX)
        {
            a->act.lunge = (dist - stop) / LUNGE_SPEED + 1;
            a->act.lungeTarget = tid;
        }
    }
    a->damageTakenThisCast = 0;
    a->aim = a->act.angle;
    if (d->pattern != RTP_SELF && d->pattern != RTP_FIELD)
        RtActor_SetFacingFromAngle(a, a->act.angle);
    if (a->act.lunge)
        RtActor_SetAnim(a, RTA_WALK, TRUE);
    else
        RtActor_SetAnim(a, d->windup > 16 ? RTA_CHARGE : (d->anim == RTA_WALK ? RTA_WALK : d->anim), d->windup > 16);
    a->gcd = 0;

    // hit counts
    a->act.hitsWanted = d->count;
    if (d->count == 0)
    {
        if (gBattleMoves[move].effect == EFFECT_BEAT_UP)
        {
            u8 i, n = 0;
            struct Pokemon *party = a->side == RT_SIDE_PLAYER ? gPlayerParty : gEnemyParty;
            for (i = 0; i < PARTY_SIZE; i++)
                if (GetMonData(&party[i], MON_DATA_HP) && !GetMonData(&party[i], MON_DATA_STATUS)
                 && GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE && GetMonData(&party[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_EGG)
                    n++;
            a->act.hitsWanted = n ? n : 1;
        }
        else
        {
            u8 r = Random() % 8;
            a->act.hitsWanted = r < 3 ? 2 : r < 6 ? 3 : r < 7 ? 4 : 5;
        }
    }
    if (move == MOVE_PRESENT)
    {
        u8 r = Random() % 10;
        a->act.power = r < 4 ? 40 : r < 7 ? 80 : r < 8 ? 120 : 0;
    }

    // name toast for big / special moves
    if (d->windup >= 30 || d->pattern == RTP_FIELD || orig != move)
    {
        NameOf(a, gStringVar1);
        StringCopy(gStringVar2, gMoveNames[move]);
        RtUi_ToastFmt(sText_UsedMove, 50);
    }

    // explosion with Damp around
    if (gBattleMoves[move].effect == EFFECT_EXPLOSION)
    {
        s32 i;
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *o = &gRt->actors[i];
            if (o->active && o->state != AST_GONE && RtActor_IsAlive(o) && o->ability == ABILITY_DAMP)
            {
                NameOf(o, gStringVar1);
                RtUi_ToastFmt(sText_Damp, 60);
                FinishCast(a, 10);
                return;
            }
        }
    }

    // Spit Up with nothing stockpiled fails
    if (gBattleMoves[move].effect == EFFECT_SPIT_UP && a->stockpile == 0)
    {
        RtUi_Toast(sText_Failed, 40);
        FinishCast(a, 10);
        return;
    }

    // semi-invulnerable starts immediately
    if (d->pattern == RTP_LEAP)
    {
        a->invuln = 1;
        Rt_PlayMoveSE(d->sfx);
    }
    else if (d->pattern == RTP_BURROW)
    {
        a->invuln = (move == MOVE_DIVE) ? 3 : 2;
        Rt_PlayMoveSE(d->sfx);
        RtFx_Burst(move == MOVE_DIVE ? FX_DROP8 : FX_ROCK16, move == MOVE_DIVE ? FXP_WATER : FXP_GROUND, a->x, a->y, 6, 2, 18);
    }
    // sun skips solar beam charge
    if (gBattleMoves[move].effect == EFFECT_SOLAR_BEAM && gRt->weather == WEATHER_SUN_RT && WeatherActive())
        a->act.timer = d->windup;
    // bide begins
    if (gBattleMoves[move].effect == EFFECT_BIDE)
    {
        a->bide = 120;
        a->bideDmg = 0;
    }
    if (gBattleMoves[move].effect == EFFECT_FOCUS_PUNCH)
        RtFx_Spawn(FX_AURA32, FXP_FIGHTING, a->x, a->y - TO_FX(a->height / 2), 60);
    if (gBattleMoves[move].effect == EFFECT_SKULL_BASH)
        RtActor_ChangeStage(a, STG_DEF, 1, a->id, TRUE);
}

// shared pause after any move before the next one can start (all moves, both sides)
#define RT_MOVE_GCD 18

static void FinishCast(struct RtActor *a, u16 recovery)
{
    u8 eff = gBattleMoves[a->act.move].effect;
    if (recovery && recovery < RT_MOVE_GCD)
        recovery = RT_MOVE_GCD;
    a->invuln = 0;
    a->hidden = FALSE;
    a->z = 0;
    if (eff == EFFECT_RECHARGE && a->act.hitMask)
        a->recharge = 70;
    if (eff == EFFECT_RAMPAGE && !a->rampage)
    {
        a->rampage = 150 + Random() % 90;
        a->rampageMove = a->act.move;
    }
    if (eff == EFFECT_UPROAR && !a->uproar)
    {
        s32 i;
        a->uproar = 300;
        for (i = 0; i < RT_MAX_ACTORS; i++)
            if (gRt->actors[i].active && gRt->actors[i].mstatus == MST_SLP)
                gRt->actors[i].mstatusTimer = 0;
    }
    if (eff == EFFECT_SPIT_UP || eff == EFFECT_SWALLOW)
    {
        if (a->stockpile)
        {
            RtActor_ChangeStage(a, STG_DEF, -a->stockpile, a->id, FALSE);
            RtActor_ChangeStage(a, STG_SPD, -a->stockpile, a->id, FALSE);
        }
        a->stockpile = 0;
    }
    a->helpingHand = 0;
    if (a->act.move != MOVE_CHARGE && RtMove_TypeOf(a, a->act.move) == TYPE_ELECTRIC)
        a->charge = 0;
    a->act.move = MOVE_NONE;
    a->gcd = recovery;
    if (a->state == AST_FAINT || a->state == AST_GONE)
        ;
    else if (a->mstatus == MST_SLP || a->mstatus == MST_FRZ)
    {
        a->state = AST_STUN;
        RtActor_SetAnim(a, a->mstatus == MST_SLP ? RTA_SLEEP : RTA_HURT, TRUE);
    }
    else if (a->state == AST_CAST)
    {
        a->state = AST_NORMAL;
        RtActor_SetAnim(a, RTA_IDLE, TRUE);
    }
}

static void ChargeVisual(struct RtActor *a, const struct RtMoveDef *d)
{
    // energy gathering into the user: particles converge, a glow grows, a ring on the ground
    u16 t = a->act.timer;
    s32 cy = a->y - TO_FX(a->height / 2);
    if (t == 1)
    {
        struct RtPart *r = RtFx_Spawn(FX_RING32, d->pal, a->x, a->y, 16);
        PlaySE(SE_M_CHARGE);
        if (r)
        {
            r->flags |= PF_SCALE | PF_GROUND;
            r->scale = 26;
            r->dscale = -1;
        }
    }
    if ((t & 1) == 0)
    {
        u8 ang = Random();
        s32 r = 26 + (Random() & 7);
        u8 life = 12;
        struct RtPart *p = RtFx_Spawn((t & 2) ? FX_SPARK8 : FX_ORB8, d->pal, a->x + RtCos(ang) * r, cy + RtSin(ang) * r / 2, life);
        if (p)
        {
            p->vx = -RtCos(ang) * r / life;
            p->vy = -RtSin(ang) * r / 2 / life;
        }
    }
    if ((t & 7) == 4)
    {
        struct RtPart *g = RtFx_Spawn(FX_GLOW16, d->pal, a->x, cy, 8);
        if (g)
        {
            g->flags |= PF_SCALE;
            g->scale = 8 + (t > 40 ? 16 : t * 2 / 5);
            g->dscale = 1;
        }
    }
}

// Visual for self-targeted moves (buffs, heals, barriers...)
static void SelfCastFx(struct RtActor *a, const struct RtMoveDef *d)
{
    s32 cy = a->y - TO_FX(a->height / 2);
    s32 k;
    struct RtPart *p;
    // ring expanding on the ground
    p = RtFx_Spawn(FX_RING32, d->pal, a->x, a->y, 18);
    if (p)
    {
        p->flags |= PF_SCALE | PF_GROUND;
        p->scale = 6;
        p->dscale = 2;
    }
    // the move's emblem pops up over the user
    if (d->shape == FX_SHIELD32 || d->shape == FX_AURA32 || d->shape == FX_SLASH32 || d->shape == FX_DOLL16
     || d->shape == FX_PSYRING16 || d->shape == FX_GLOW16 || d->shape == FX_MUD16)
    {
        p = RtFx_Spawn(d->shape, d->pal, a->x, cy, 26);
        if (p)
        {
            p->flags |= PF_SCALE | PF_FLICKER;
            p->scale = 6;
            p->dscale = 1;
            p->prio = 1;
        }
    }
    // a column of rising particles around the user
    for (k = 0; k < 8; k++)
    {
        u8 ang = k * 32 + (Random() & 15);
        u8 shape = (d->shape == FX_SHIELD32 || d->shape == FX_AURA32 || d->shape == FX_SLASH32 || d->shape == FX_GLOW16
                    || d->shape == FX_PSYRING16 || d->shape == FX_DOLL16) ? ((k & 1) ? FX_SPARK8 : FX_ORB8) : d->shape;
        p = RtFx_Spawn(shape, d->pal, a->x + RtCos(ang) * (a->radius + 4) / 1, a->y + RtSin(ang) * (a->radius + 4) / 2 - TO_FX(2), 22 + (k & 3) * 4);
        if (p)
        {
            p->vy = -80 - (Random() & 63);
        }
    }
    a->flash = 6;
}

static void SpawnBullet(struct RtActor *a, const struct RtMoveDef *d, u8 angle, u8 delay)
{
    struct RtProj *p = NewProj(a, PK_BULLET, a->act.move);
    s32 sp = FoeSpeed(a, d->speed ? d->speed : 48);
    if (!p)
        return;
    p->angle = angle;
    p->x = a->x + RtCos(angle) * (a->radius + 4);
    p->y = a->y + RtSin(angle) * (a->radius + 4) / 2;
    p->vx = RtCos(angle) * sp / 16;
    p->vy = RtSin(angle) * sp / 16;
    p->radius = d->radius;
    p->life = ProjLife(d->range, sp);
    p->len = p->life;
    p->delay = delay;
    p->homingTarget = a->act.targetId;
    // visual multi-projectiles of single-hit moves share one hit per target
    switch (gBattleMoves[a->act.move].effect)
    {
    case EFFECT_MULTI_HIT:
    case EFFECT_DOUBLE_HIT:
    case EFFECT_TWINEEDLE:
    case EFFECT_TRIPLE_KICK:
    case EFFECT_BEAT_UP:
        break;
    default:
        p->param |= 1;
        break;
    }
    if (p->flags & RTMF_ARC)
    {
        // lobbed shots land at the target point
        s32 dist = RtDist(FROM_FX(a->act.tx - a->x), FROM_FX(a->act.ty - a->y));
        if (a->act.targetId != 0xFF && dist > 8)
        {
            p->life = dist * 16 / sp + 1;
            p->len = p->life;
            // the player's Leech Seed follows the foe while in the air and lands on it (seeds a few px apart)
            if (gBattleMoves[a->act.move].effect == EFFECT_LEECH_SEED && a->side == RT_SIDE_PLAYER && a->act.targetId < RT_MAX_ACTORS)
            {
                p->param |= 0x100;
                p->homingTarget = a->act.targetId;
                p->maxRadius = (u8)(s8)((s8)(angle - a->act.angle) / 2);
            }
        }
    }
}

static void ExecuteMove(struct RtActor *a)
{
    const struct RtMoveDef *d = Def(a->act.move);
    u16 move = a->act.move;
    u8 eff = gBattleMoves[move].effect;
    struct RtProj *p;
    u8 i;

    // telekinetic / throwing moves pick up a nearby arena prop and hurl it instead
    if (RtProps_TryGrab(a, move))
    {
        if (d->anim != RTA_WALK)
            RtActor_SetAnim(a, RTA_CHARGE, FALSE);
        return;
    }
    if (d->pattern != RTP_MELEE && d->pattern != RTP_BULLET)
        Rt_PlayMoveSE(d->sfx);
    if (d->anim != RTA_WALK)
        RtActor_SetAnim(a, d->anim, FALSE);
    RtVfx_Execute(a, move);

    switch (d->pattern)
    {
    case RTP_MELEE:
        {
            // land the hitbox on the animation's impact frame (the PMD attack animation lunges forward)
            u8 hitDelay = RtActor_AnimHitDelay(a, RTA_ATTACK);
            for (i = 0; i < a->act.hitsWanted; i++)
            {
                p = NewProj(a, PK_MELEE, move);
                if (!p)
                    break;
                p->angle = a->act.angle;
                p->len = MeleeLen(d);
                p->radius = MeleeRadius(d);
                p->life = d->duration ? d->duration : 6;
                p->param2 = a->act.castId;
                p->delay = 1 + hitDelay + i * d->spread;
            }
        }
        break;
    case RTP_RUSH:
        p = NewProj(a, PK_RUSHBOX, move);
        if (p)
        {
            p->radius = d->radius;
            p->shape = FX_NONE;
        }
        if (gBattleMoves[move].priority > 0)
            a->iframes = d->duration + 4;
        break;
    case RTP_BULLET:
        if (p = NULL, d->flags & RTMF_FAN)
        {
            s32 n = a->act.hitsWanted;
            for (i = 0; i < n; i++)
            {
                s8 off = (s8)((i * 2 - (n - 1)) * d->spread / 2);
                SpawnBullet(a, d, a->act.angle + off, 0);
            }
            Rt_PlayMoveSE(d->sfx);
        }
        else
        {
            for (i = 0; i < a->act.hitsWanted; i++)
            {
                u8 jitter = (d->flags & RTMF_HOMING) ? 0 : d->aux;
                s8 off = jitter ? (s8)((Random() % (jitter * 2 + 1)) - jitter) : 0;
                SpawnBullet(a, d, a->act.angle + off, i == 0 ? 0 : i * d->spread);
            }
            Rt_PlayMoveSE(d->sfx);
        }
        break;
    case RTP_STREAM:
        // emission happens during the active phase
        break;
    case RTP_BEAM:
        p = NewProj(a, PK_BEAM, move);
        if (p)
        {
            p->angle = a->act.angle;
            p->radius = d->radius;
            p->vx = BeamTipSpeed(a);
            p->life = BeamFrames(a, d);
            p->len = 8;
            p->param |= 1;
        }
        RtFx_Shake(1, d->duration);
        break;
    case RTP_BURST:
        p = NewProj(a, PK_AREA, move);
        if (p)
        {
            p->x = a->x;
            p->y = a->y;
            p->maxRadius = d->range * 2;
            p->radius = 0;
        }
        if (move == MOVE_EARTHQUAKE || move == MOVE_MAGNITUDE || eff == EFFECT_EXPLOSION || move == MOVE_BLAST_BURN)
            RtFx_Shake(4, d->duration + 10);
        if (eff == EFFECT_EXPLOSION)
        {
            RtFx_Flash(0, 10);
            RtFx_Impact(RTI_BURN, FXP_FIRE, a->x, a->y - TO_FX(a->height / 2));
        }
        if (eff == EFFECT_MEMENTO)
        {
            s32 k;
            for (k = 0; k < RT_MAX_ACTORS; k++)
            {
                struct RtActor *t = &gRt->actors[k];
                if (t->active && t->side != a->side && t->state != AST_GONE && RtActor_IsAlive(t))
                {
                    ApplyStatDown(a, t, STG_ATK, 2);
                    ApplyStatDown(a, t, STG_SPA, 2);
                }
            }
        }
        break;
    case RTP_TARGETED:
        {
            u8 n = a->act.hitsWanted ? a->act.hitsWanted : 1;
            for (i = 0; i < n; i++)
            {
                p = NewProj(a, eff == EFFECT_FUTURE_SIGHT ? PK_FUTURE : PK_STRIKE, move);
                if (!p)
                    break;
                p->x = a->act.tx;
                p->y = a->act.ty;
                if (i > 0 || n > 1)
                {
                    u8 ang = Random();
                    s32 r = Random() % (d->spread + 1);
                    p->x += RtCos(ang) * r;
                    p->y += RtSin(ang) * r / 2;
                }
                p->radius = d->radius;
                p->delay = d->windup + i * 5;
                p->param |= 1; // one hit per target for the whole cast
                if (eff == EFFECT_FUTURE_SIGHT)
                {
                    struct RtActor *t = a->act.targetId < RT_MAX_ACTORS ? &gRt->actors[a->act.targetId] : Rt_NearestFoe(a);
                    if (t)
                    {
                        u8 e;
                        s32 dmg = RtMove_CalcDamage(a, t, move, gBattleMoves[move].power, &e, NULL);
                        p->power = e ? dmg * 10 / e : dmg;
                    }
                    else
                    {
                        p->power = a->level;
                    }
                    p->param &= ~1;
                }
            }
        }
        break;
    case RTP_WAVE:
        p = NewProj(a, PK_WAVE, move);
        if (p)
        {
            // waves (Psywave, Supersonic, Hyper Voice...) travel at 70% of their table speed
            s32 sp = FoeSpeed(a, (d->speed ? d->speed : 40) * WAVE_SPEED_PCT / 100);
            p->angle = a->act.angle;
            p->x = a->x + RtCos(p->angle) * (a->radius + 6);
            p->y = a->y + RtSin(p->angle) * (a->radius + 6) / 2;
            p->vx = RtCos(p->angle) * sp / 16;
            p->vy = RtSin(p->angle) * sp / 16;
            p->radius = d->radius;
            p->life = ProjLife(d->range, sp);
            // the wave leaves the user narrow and widens as it travels (cone shape)
            p->maxRadius = d->aux;
            p->len = d->aux / 5 > 4 ? d->aux / 5 : 4;
            p->param2 = p->life * 3 / 5;
            if (p->param2 < 6)
                p->param2 = 6;
        }
        break;
    case RTP_CONE:
        {
            s32 k;
            s32 range = d->range * 2;
            for (k = RtVfx_Cone(a, move) ? 8 : 0; k < 8; k++)
            {
                s8 off = (s8)((Random() % (d->spread * 2 + 1)) - d->spread);
                u8 ang = a->act.angle + off;
                struct RtPart *pt = RtFx_Spawn(d->shape, d->pal, a->x + RtCos(ang) * a->radius, a->y + RtSin(ang) * a->radius / 2 - TO_FX(a->height / 2), d->duration ? d->duration : 16);
                if (pt)
                {
                    pt->vx = RtCos(ang) * range / (d->duration ? d->duration : 16);
                    pt->vy = RtSin(ang) * range / (d->duration ? d->duration : 16) / 2;
                    pt->flags |= (d->shape == FX_RING32 || d->shape == FX_RING16) ? PF_BLEND : 0;
                }
            }
            for (k = 0; k < RT_MAX_ACTORS; k++)
            {
                struct RtActor *t = &gRt->actors[k];
                struct RtProj tmp;
                s32 dx, dy, dist;
                s8 diff;
                memset(&tmp, 0, sizeof(tmp));
                tmp.kind = PK_MELEE;
                tmp.owner = a->id;
                tmp.side = a->side;
                tmp.move = move;
                tmp.pal = d->pal;
                if (!IsValidTarget(&tmp, t))
                    continue;
                dx = FROM_FX(t->x - a->x);
                dy = FROM_FX(t->y - a->y);
                dist = RtDist(dx, dy);
                diff = (s8)(RtAtan2(dx, dy) - a->act.angle);
                if (diff < 0) diff = -diff;
                if (dist <= range + t->radius && (diff <= d->spread || dist < a->radius + t->radius + 4))
                    TryHitActor(&tmp, a, t);
            }
        }
        break;
    case RTP_ZONE:
        {
            s32 sp = FoeSpeed(a, d->speed ? d->speed : 40);
            p = NewProj(a, PK_BULLET, move);
            if (p)
            {
                s32 dist = RtDist(FROM_FX(a->act.tx - a->x), FROM_FX(a->act.ty - a->y));
                p->angle = RtAtan2(FROM_FX(a->act.tx - a->x), FROM_FX(a->act.ty - a->y));
                p->x = a->x;
                p->y = a->y;
                p->vx = RtCos(p->angle) * sp / 16;
                p->vy = RtSin(p->angle) * sp / 16;
                p->radius = 6;
                if (dist > d->range * 2)
                    dist = d->range * 2;
                p->life = dist * 16 / sp + 1;
                p->len = p->life;
                p->flags |= RTMF_ARC;
                p->param |= 4; // becomes a zone
            }
        }
        break;
    case RTP_SELF:
    case RTP_FIELD:
        if (d->pattern == RTP_SELF && !RtVfx_SelfCast(a, move))
            SelfCastFx(a, d);
        RtMove_ApplySelfEffect(a, move);
        if (d->pattern == RTP_FIELD)
            RtFx_Flash(0, 3); // a short soft flash: the weather / field visuals carry the move
        break;
    case RTP_LEAP:
    case RTP_BURROW:
        // landing hit
        p = NewProj(a, PK_AREA, move);
        if (p)
        {
            p->x = a->x;
            p->y = a->y;
            p->maxRadius = d->radius + 6;
            p->shape = d->pattern == RTP_BURROW ? (move == MOVE_DIVE ? FX_DROP8 : FX_ROCK16) : FX_SMOKE16;
            p->pal = d->pattern == RTP_BURROW ? (move == MOVE_DIVE ? FXP_WATER : FXP_GROUND) : FXP_SMOKE;
        }
        RtFx_Shake(3, 10);
        RtFx_Impact(d->impact, d->pal, a->x, a->y);
        PlaySE(SE_M_MEGA_KICK2);
        break;
    case RTP_ROLL:
        p = NewProj(a, PK_RUSHBOX, move);
        if (p)
        {
            p->radius = d->radius;
            p->shape = FX_NONE;
        }
        break;
    }

    // user effects that happen on execution (Memento: the user faints like with Explosion)
    if (eff == EFFECT_EXPLOSION || eff == EFFECT_MEMENTO)
    {
        a->hp = 0;
        a->hpBarDirty = TRUE;
        RtActor_Faint(a);
    }
}

// steering helper during channelled moves (player input / AI target tracking)
// Turns a channelled move (beam, stream) toward the stick / the target by up to `rate` per frame,
// but never more than maxDev (256 = full turn) away from the direction it was cast in.
static void SteerChannelLimited(struct RtActor *a, u8 rate, u8 maxDev);
static void SteerChannel(struct RtActor *a, u8 rate)
{
    SteerChannelLimited(a, rate, 128);
}

static void SteerChannelLimited(struct RtActor *a, u8 rate, u8 maxDev)
{
    u8 want = a->act.angle;
    if (a->id == gRt->playerLead && a->side == RT_SIDE_PLAYER && !gRt->isSafari)
    {
        if (a->inMoveX || a->inMoveY)
            want = RtAtan2(a->inMoveX, a->inMoveY);
    }
    else
    {
        struct RtActor *t = a->aiTarget < RT_MAX_ACTORS ? &gRt->actors[a->aiTarget] : Rt_NearestFoe(a);
        if (t && RtActor_IsAlive(t))
            want = RtAtan2(FROM_FX(t->x - a->x), FROM_FX(t->y - a->y));
    }
    {
        s8 diff = (s8)(want - a->act.angle);
        s8 dev;
        if (diff > rate) diff = rate;
        if (diff < -rate) diff = -rate;
        dev = (s8)(a->act.angle + diff - a->act.aimBase);
        if (maxDev < 128 && dev > (s8)maxDev)
            diff -= dev - maxDev;
        else if (maxDev < 128 && dev < -(s8)maxDev)
            diff += -maxDev - dev;
        a->act.angle += diff;
        RtActor_SetFacingFromAngle(a, a->act.angle);
    }
}

// closing dash before a melee strike: re-aims at the target every frame, stops at striking range,
// at a wall, or when the target is gone
static void MeleeLunge(struct RtActor *a, const struct RtMoveDef *d)
{
    struct RtActor *t = &gRt->actors[a->act.lungeTarget < RT_MAX_ACTORS ? a->act.lungeTarget : 0];
    s32 dist, stop, step, nx, ny;
    u8 ang;
    a->act.lunge--;
    if (!t->active || t->side == a->side || !RtActor_IsAlive(t) || t->state == AST_GONE || t->invuln)
    {
        a->act.lunge = 0;
        return;
    }
    dist = RtDist(FROM_FX(t->x - a->x), FROM_FX(t->y - a->y));
    stop = a->radius + MeleeLen(d);
    if (dist <= stop + 2)
    {
        a->act.lunge = 0;
        return;
    }
    ang = RtAtan2(FROM_FX(t->x - a->x), FROM_FX(t->y - a->y));
    step = dist - stop < LUNGE_SPEED ? dist - stop : LUNGE_SPEED;
    nx = a->x + RtCos(ang) * step;
    ny = a->y + RtSin(ang) * step;
    if (RtArena_CircleBlocked(FROM_FX(nx), FROM_FX(ny), a->radius / 2))
    {
        a->act.lunge = 0;
        return;
    }
    a->x = nx;
    a->y = ny;
    a->act.angle = ang;
    a->aim = ang;
    RtActor_SetFacingFromAngle(a, ang);
    if ((a->act.lunge & 1) == 0)
    {
        struct RtPart *pt = RtFx_Spawn(gRt->floorKind == 1 ? FX_DROP8 : FX_SMOKE16, gRt->floorKind == 1 ? FXP_WATER : FXP_SMOKE, a->x, a->y, 10);
        if (pt)
            pt->flags |= PF_BLEND;
    }
}

void RtMove_UpdateCast(struct RtActor *a)
{
    const struct RtMoveDef *d = Def(a->act.move);
    u8 eff = gBattleMoves[a->act.move].effect;
    u16 windup = d->windup;

    if (a->act.move == MOVE_NONE)
    {
        FinishCast(a, 0);
        return;
    }
    if (d->pattern == RTP_TARGETED)
        windup = 10; // warnings are on the projectile, the user just gestures

    if (a->act.phase == 0 && a->act.lunge)
    {
        MeleeLunge(a, d);
        return;
    }
    a->act.timer++;
    switch (a->act.phase)
    {
    case 0: // windup
        if (eff == EFFECT_FOCUS_PUNCH && a->damageTakenThisCast)
        {
            ToastName(a, sText_LostFocus, 50);
            FinishCast(a, 20);
            return;
        }
        if (eff == EFFECT_BIDE)
        {
            if (a->bide == 0)
            {
                // release
                struct RtActor *t = a->lastHitBy < RT_MAX_ACTORS ? &gRt->actors[a->lastHitBy] : Rt_NearestFoe(a);
                ToastName(a, sText_BideUnleash, 50);
                RtFx_Flash(0, 6);
                RtFx_Impact(RTI_STAR, FXP_GOLD, a->x, a->y - TO_FX(a->height / 2));
                if (t && t->side != a->side && RtActor_IsAlive(t) && a->bideDmg && t->protect && t->protectKind == 0)
                {
                    RtFx_Word(t->x, t->y - TO_FX(t->height + 8), FX_W_BLOCK);
                    PlaySE(SE_M_DETECT);
                }
                else if (t && t->side != a->side && RtActor_IsAlive(t) && a->bideDmg)
                {
                    RtFx_Impact(RTI_HIT, FXP_GOLD, t->x, t->y - TO_FX(t->height / 2));
                    RtActor_Damage(t, a->bideDmg * 2, a->id, FALSE);
                }
                FinishCast(a, 10);
                return;
            }
            RtVfx_BideTick(a, a->act.timer);
            return;
        }
        if (d->pattern == RTP_LEAP)
        {
            // rise, glide toward target, fall
            u16 t = a->act.timer;
            struct RtActor *tg = a->act.targetId < RT_MAX_ACTORS ? &gRt->actors[a->act.targetId] : NULL;
            if (t < 14)
                a->z = t * 7;
            else if (t + 8 >= windup)
                a->z = (windup - t) * 12;
            else
                a->z = 100;
            if (a->z > 60)
                a->hidden = TRUE;
            else
                a->hidden = FALSE;
            if (tg && RtActor_IsAlive(tg) && tg->state != AST_GONE)
            {
                a->act.tx = tg->x;
                a->act.ty = tg->y;
            }
            if (t > 10)
            {
                s32 dx = FROM_FX(a->act.tx - a->x), dy = FROM_FX(a->act.ty - a->y);
                s32 dist = RtDist(dx, dy);
                s32 sp = d->speed ? d->speed : 40;
                if (dist > 2)
                {
                    s32 step = sp * 256 / 16;
                    if (dist * 256 < step)
                        step = dist * 256;
                    a->x += dx * step / dist;
                    a->y += dy * step / dist;
                }
            }
            if (a->act.timer >= windup)
            {
                a->z = 0;
                a->invuln = 0;
                a->hidden = FALSE;
                ExecuteMove(a);
                a->act.phase = 2;
                a->act.timer = 0;
            }
            return;
        }
        if (d->pattern == RTP_BURROW)
        {
            struct RtActor *tg = a->act.targetId < RT_MAX_ACTORS ? &gRt->actors[a->act.targetId] : NULL;
            if (tg && RtActor_IsAlive(tg) && tg->state != AST_GONE)
            {
                a->act.tx = tg->x;
                a->act.ty = tg->y;
            }
            if (a->act.timer > 8 && a->act.timer + 6 < windup)
            {
                s32 dx = FROM_FX(a->act.tx - a->x), dy = FROM_FX(a->act.ty - a->y);
                s32 dist = RtDist(dx, dy);
                s32 sp = d->speed ? d->speed : 40;
                if (dist > 2)
                {
                    s32 step = sp * 256 / 16;
                    if (dist * 256 < step)
                        step = dist * 256;
                    a->x += dx * step / dist;
                    a->y += dy * step / dist;
                }
                if ((a->act.timer & 3) == 0)
                    RtFx_Spawn(a->act.move == MOVE_DIVE ? FX_BUBBLE16 : FX_SMOKE16, a->act.move == MOVE_DIVE ? FXP_WATER : FXP_MUD, a->x, a->y, 14);
            }
            if (a->act.timer >= windup)
            {
                a->invuln = 0;
                a->hidden = FALSE;
                ExecuteMove(a);
                a->act.phase = 2;
                a->act.timer = 0;
            }
            return;
        }
        if (windup > 12 && !RtVfx_Charge(a, a->act.move, a->act.timer, windup))
            ChargeVisual(a, d);
        if (d->pattern == RTP_MELEE && d->aux && a->act.timer <= windup)
        {
            // lunge forward during windup
            s32 step = d->aux * 256 / (windup ? windup : 1);
            a->x += RtCos(a->act.angle) * step / 256;
            a->y += RtSin(a->act.angle) * step / 512;
        }
        if (a->act.timer >= windup)
        {
            ExecuteMove(a);
            if (a->state != AST_CAST)
                return;
            a->act.phase = 1;
            a->act.timer = 0;
        }
        break;
    case 1: // active
        switch (d->pattern)
        {
        case RTP_RUSH:
            {
                s32 sp = FoeSpeed(a, d->speed ? d->speed : 64);
                if (a->act.timer >= 0xFF00 || a->act.timer > d->duration)
                {
                    a->act.phase = 2;
                    a->act.timer = 0;
                    if (!a->act.hitMask && eff == EFFECT_RECOIL_IF_MISS)
                    {
                        ToastName(a, sText_KeptGoing, 50);
                        RtActor_Damage(a, a->maxHp / 8, a->id, FALSE);
                    }
                    break;
                }
                a->x += RtCos(a->act.angle) * sp / 16;
                a->y += RtSin(a->act.angle) * sp / 16;
                if (RtArena_CircleBlocked(FROM_FX(a->x), FROM_FX(a->y), a->radius / 2))
                {
                    a->act.timer = 0xFF00;
                    RtFx_Shake(1, 4);
                }
                if (RtVfx_RushTick(a, a->act.move))
                    ;
                else if ((a->act.timer & 1) == 0)
                {
                    struct RtPart *pt = RtFx_Spawn(d->trail != FX_NONE ? d->trail : FX_SMOKE16, d->trail != FX_NONE ? d->pal : FXP_SMOKE,
                                                   a->x, a->y - TO_FX(d->trail != FX_NONE ? a->height / 2 : 0), 10);
                    if (pt) pt->flags |= PF_BLEND;
                }
            }
            break;
        case RTP_STREAM:
            {
                u8 rate = d->aux ? d->aux : 3;
                // streams can be turned a little (about 20 degrees each way), not swept onto the target
                if (d->flags & RTMF_ROOT)
                    SteerChannelLimited(a, 1, 14);
                RtVfx_StreamTick(a, a->act.move, a->act.timer);
                if ((a->act.timer % rate) == 0)
                {
                    struct RtProj *p = NewProj(a, PK_BULLET, a->act.move);
                    if (p)
                    {
                        s32 sp = FoeSpeed(a, d->speed ? d->speed : 56);
                        s8 off = d->spread ? (s8)((Random() % (d->spread * 2 + 1)) - d->spread) : 0;
                        p->angle = a->act.angle + off;
                        p->x = a->x + RtCos(p->angle) * (a->radius + 2);
                        p->y = a->y + RtSin(p->angle) * (a->radius + 2) / 2;
                        p->vx = RtCos(p->angle) * sp / 16;
                        p->vy = RtSin(p->angle) * sp / 16;
                        p->radius = d->radius;
                        p->life = ProjLife(d->range, sp);
                        p->len = p->life;
                        p->param |= 1 | 8; // 8: stream particle (drawn growing)
                        p->flags |= RTMF_PIERCE;
                        p->flags &= ~RTMF_ARC;
                    }
                    if ((a->act.timer % (rate * 4)) == 0)
                        Rt_PlayMoveSE(d->sfx);
                }
                if (a->act.timer >= d->duration)
                {
                    a->act.phase = 2;
                    a->act.timer = 0;
                }
            }
            break;
        case RTP_BEAM:
            // the player can nudge its beam (slowly, about 17 degrees each way); a foe's beam keeps the
            // direction it was fired in
            SteerChannelLimited(a, (a->side == RT_SIDE_PLAYER && (a->act.timer & 1)) ? 1 : 0, 12);
            if (a->act.timer >= BeamFrames(a, d))
            {
                a->act.phase = 2;
                a->act.timer = 0;
            }
            break;
        case RTP_ROLL:
            {
                s32 sp = (d->speed ? d->speed : 40) + a->rolloutHits * 8;
                SteerChannel(a, 4);
                if (a->act.subTimer)
                    a->act.subTimer--;
                // AUTO AIM (and the AI): after a bounce the ball curves back towards the nearest foe
                else if (a->side != RT_SIDE_PLAYER || gSaveBlock2Ptr->optionsRtAutoAim)
                {
                    struct RtActor *t = Rt_NearestFoe(a);
                    if (t)
                    {
                        u8 want = RtAtan2(FROM_FX(t->x - a->x), FROM_FX(t->y - a->y));
                        s8 diff = (s8)(want - a->act.angle);
                        if (diff > 6) diff = 6;
                        if (diff < -6) diff = -6;
                        a->act.angle += diff;
                    }
                }
                a->x += RtCos(a->act.angle) * sp / 16;
                a->y += RtSin(a->act.angle) * sp / 16;
                if (RtArena_CircleBlocked(FROM_FX(a->x), FROM_FX(a->y), a->radius / 2))
                {
                    a->act.angle += 128;
                    a->x += RtCos(a->act.angle) * 3;
                    a->y += RtSin(a->act.angle) * 3;
                    RtFx_Shake(1, 4);
                }
                if ((a->act.timer & 3) == 0)
                    RtFx_Spawn(a->act.move == MOVE_ICE_BALL ? FX_SHARD16 : FX_SMOKE16, a->act.move == MOVE_ICE_BALL ? FXP_ICE : FXP_SMOKE, a->x, a->y, 12);
                if (a->act.timer >= d->duration || a->rolloutHits >= 5)
                {
                    a->rolloutHits = 0;
                    a->act.phase = 2;
                    a->act.timer = 0;
                }
            }
            break;
        default:
            {
                // wait for the animation / hitboxes to finish
                u16 activeFrames = 6;
                if (d->pattern == RTP_MELEE)
                {
                    activeFrames = (a->act.hitsWanted - 1) * d->spread + (d->duration ? d->duration : 6) + 2 + RtActor_AnimHitDelay(a, RTA_ATTACK);
                    // replay the strike animation for every hit of multi-hit moves
                    if (a->act.hitsWanted > 1 && a->act.timer > 1 && ((a->act.timer - 1) % d->spread) == 0
                     && (a->act.timer - 1) / d->spread < a->act.hitsWanted)
                        RtActor_SetAnim(a, RTA_ATTACK, FALSE);
                }
                else if (d->pattern == RTP_BULLET && !(d->flags & RTMF_FAN))
                    activeFrames = (a->act.hitsWanted - 1) * d->spread + 6;
                else if (d->pattern == RTP_BURST)
                    activeFrames = d->duration / 2;
                else if (d->pattern == RTP_SELF || d->pattern == RTP_FIELD)
                    activeFrames = 10;
                if (d->pattern == RTP_SELF && gRtVfxDefs[a->act.move].style == VS_NONE && (a->act.timer & 3) == 0 && a->act.timer < d->duration)
                {
                    struct RtPart *pt = RtFx_Spawn(d->shape, d->pal, a->x + TO_FX((Random() % 24) - 12), a->y - TO_FX(Random() % (a->height + 1)), 18);
                    if (pt)
                        pt->vy = -128;
                }
                if (a->act.timer >= activeFrames)
                {
                    a->act.phase = 2;
                    a->act.timer = 0;
                }
            }
            break;
        }
        break;
    case 2: // recovery
        if (a->act.timer >= 8)
            FinishCast(a, 12);
        break;
    }
}

// ---------------------------------------------------------------------------
// Self / field effects
// ---------------------------------------------------------------------------
static void RtSetWeather(u8 w, struct RtActor *src)
{
    static const u8 *const msgs[] = {NULL, sText_Rain, sText_Sun, sText_Sand, sText_Hail};
    gRt->weather = w;
    gRt->weatherTimer = 1500;
    if (src && w == WEATHER_RAIN_RT && src->holdEffect == 0 && 0)
        gRt->weatherTimer = 1500;
    if (msgs[w])
        RtUi_Toast(msgs[w], 70);
    {
        // castform forecast
        s32 i;
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *a = &gRt->actors[i];
            if (a->active && a->ability == ABILITY_FORECAST)
            {
                u8 t = TYPE_NORMAL;
                if (w == WEATHER_RAIN_RT) t = TYPE_WATER;
                else if (w == WEATHER_SUN_RT) t = TYPE_FIRE;
                else if (w == WEATHER_HAIL_RT) t = TYPE_ICE;
                a->types[0] = a->types[1] = t;
            }
        }
    }
}

static void HealFraction(struct RtActor *a, u8 num, u8 den)
{
    s32 h = a->maxHp * num / den;
    if (a->hp >= a->maxHp)
    {
        RtUi_Toast(sText_Failed, 40);
        return;
    }
    RtActor_Heal(a, h ? h : 1);
}

void RtMove_ApplySelfEffect(struct RtActor *a, u16 move)
{
    u8 eff = gBattleMoves[move].effect;
    s32 i;

    // snatch: an enemy waiting with snatch steals self-buffs
    if (gBattleMoves[move].flags & FLAG_SNATCH_AFFECTED)
    {
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *o = &gRt->actors[i];
            if (o->active && o->side != a->side && o->snatch && RtActor_IsAlive(o))
            {
                o->snatch = 0;
                RtFx_Spawn(FX_FIST16, FXP_DARK, a->x, a->y - TO_FX(a->height / 2), 14);
                a = o;
                break;
            }
        }
    }

    switch (eff)
    {
    case EFFECT_ATTACK_UP: RtActor_ChangeStage(a, STG_ATK, 1, a->id, TRUE); break;
    case EFFECT_DEFENSE_UP: RtActor_ChangeStage(a, STG_DEF, 1, a->id, TRUE); break;
    case EFFECT_SPEED_UP: RtActor_ChangeStage(a, STG_SPE, 1, a->id, TRUE); break;
    case EFFECT_SPECIAL_ATTACK_UP: RtActor_ChangeStage(a, STG_SPA, 1, a->id, TRUE); break;
    case EFFECT_SPECIAL_DEFENSE_UP: RtActor_ChangeStage(a, STG_SPD, 1, a->id, TRUE); break;
    case EFFECT_ACCURACY_UP: RtActor_ChangeStage(a, STG_ACC, 1, a->id, TRUE); break;
    case EFFECT_EVASION_UP: RtActor_ChangeStage(a, STG_EVA, 1, a->id, TRUE); break;
    case EFFECT_ATTACK_UP_2: RtActor_ChangeStage(a, STG_ATK, 2, a->id, TRUE); break;
    case EFFECT_DEFENSE_UP_2: RtActor_ChangeStage(a, STG_DEF, 2, a->id, TRUE); break;
    case EFFECT_SPEED_UP_2: RtActor_ChangeStage(a, STG_SPE, 2, a->id, TRUE); break;
    case EFFECT_SPECIAL_ATTACK_UP_2: RtActor_ChangeStage(a, STG_SPA, 2, a->id, TRUE); break;
    case EFFECT_SPECIAL_DEFENSE_UP_2: RtActor_ChangeStage(a, STG_SPD, 2, a->id, TRUE); break;
    case EFFECT_ACCURACY_UP_2: RtActor_ChangeStage(a, STG_ACC, 2, a->id, TRUE); break;
    case EFFECT_EVASION_UP_2: RtActor_ChangeStage(a, STG_EVA, 2, a->id, TRUE); break;
    case EFFECT_DEFENSE_CURL:
        RtActor_ChangeStage(a, STG_DEF, 1, a->id, TRUE);
        a->defenseCurl = 1;
        break;
    case EFFECT_MINIMIZE:
        RtActor_ChangeStage(a, STG_EVA, 1, a->id, TRUE);
        a->minimize = 0xFFFF;
        break;
    case EFFECT_BULK_UP:
        RtActor_ChangeStage(a, STG_ATK, 1, a->id, TRUE);
        RtActor_ChangeStage(a, STG_DEF, 1, a->id, FALSE);
        break;
    case EFFECT_CALM_MIND:
        RtActor_ChangeStage(a, STG_SPA, 1, a->id, TRUE);
        RtActor_ChangeStage(a, STG_SPD, 1, a->id, FALSE);
        break;
    case EFFECT_DRAGON_DANCE:
        RtActor_ChangeStage(a, STG_ATK, 1, a->id, TRUE);
        RtActor_ChangeStage(a, STG_SPE, 1, a->id, FALSE);
        break;
    case EFFECT_COSMIC_POWER:
        RtActor_ChangeStage(a, STG_DEF, 1, a->id, TRUE);
        RtActor_ChangeStage(a, STG_SPD, 1, a->id, FALSE);
        break;
    case EFFECT_BELLY_DRUM:
        if (a->hp > a->maxHp / 2 && a->stages[STG_ATK] < 6)
        {
            RtActor_Damage(a, a->maxHp / 2, a->id, FALSE);
            a->stages[STG_ATK] = 6;
            RtFx_StatArrows(a, TRUE);
        }
        else
        {
            RtUi_Toast(sText_Failed, 40);
        }
        break;
    case EFFECT_FOCUS_ENERGY:
        a->focusEnergy = 1;
        break;
    case EFFECT_CHARGE:
        a->charge = 600;
        break;
    case EFFECT_STOCKPILE:
        if (a->stockpile < 3)
        {
            a->stockpile++;
            RtActor_ChangeStage(a, STG_DEF, 1, a->id, TRUE);
            RtActor_ChangeStage(a, STG_SPD, 1, a->id, FALSE);
        }
        break;
    case EFFECT_SWALLOW:
        if (a->stockpile)
            HealFraction(a, a->stockpile == 1 ? 1 : (a->stockpile == 2 ? 1 : 1), a->stockpile == 1 ? 4 : (a->stockpile == 2 ? 2 : 1));
        else
            RtUi_Toast(sText_Failed, 40);
        break;
    case EFFECT_RESTORE_HP:
    case EFFECT_SOFTBOILED:
        HealFraction(a, 1, 2);
        break;
    case EFFECT_MORNING_SUN:
    case EFFECT_SYNTHESIS:
    case EFFECT_MOONLIGHT:
        if (!WeatherActive())
            HealFraction(a, 1, 2);
        else if (gRt->weather == WEATHER_SUN_RT)
            HealFraction(a, 2, 3);
        else
            HealFraction(a, 1, 4);
        break;
    case EFFECT_REST:
        if (a->hp < a->maxHp && a->ability != ABILITY_INSOMNIA && a->ability != ABILITY_VITAL_SPIRIT)
        {
            RtActor_Heal(a, a->maxHp);
            a->mstatus = MST_NONE;
            RtActor_ApplyStatus(a, MST_SLP, a->id);
            a->mstatusTimer = 150;
            a->stateTimer = 150;
        }
        else
        {
            RtUi_Toast(sText_Failed, 40);
        }
        break;
    case EFFECT_PROTECT:
        {
            u8 chance;
            // the chain only counts Protects used back to back (within 3 s of the last one)
            if ((u32)(gRt->frame - a->protectLastFrame) > 180)
                a->protectChain = 0;
            a->protectLastFrame = gRt->frame;
            chance = 100 >> (a->protectChain > 3 ? 3 : a->protectChain);
            if (gRt->frame - a->sentOutFrame < 0 || (Random() % 100) < chance)
            {
                a->protect = 50;
                a->protectKind = (move == MOVE_ENDURE) ? 1 : 0;
                a->protectChain++;
            }
            else
            {
                a->protectChain = 0;
                RtUi_Toast(sText_Failed, 40);
            }
        }
        break;
    case EFFECT_ENDURE:
        a->protect = 50;
        a->protectKind = 1;
        break;
    case EFFECT_SUBSTITUTE:
        if (!a->substituteHp && a->hp > a->maxHp / 4)
        {
            RtActor_Damage(a, a->maxHp / 4, a->id, TRUE);
            a->substituteHp = a->maxHp / 4 + 1;
            NameOf(a, gStringVar1);
            RtUi_ToastFmt(sText_Substitute, 60);
            a->palDirty = TRUE;
        }
        else
        {
            RtUi_Toast(sText_Failed, 40);
        }
        break;
    case EFFECT_REFLECT:
        gRt->sides[a->side].reflect = 1200;
        break;
    case EFFECT_LIGHT_SCREEN:
        gRt->sides[a->side].lightScreen = 1200;
        break;
    case EFFECT_MIST:
        gRt->sides[a->side].mist = 1200;
        break;
    case EFFECT_SAFEGUARD:
        gRt->sides[a->side].safeguard = 1200;
        break;
    case EFFECT_HAZE:
        for (i = 0; i < RT_MAX_ACTORS; i++)
            memset(gRt->actors[i].stages, 0, sizeof(gRt->actors[i].stages));
        break;
    case EFFECT_RAIN_DANCE: RtSetWeather(WEATHER_RAIN_RT, a); break;
    case EFFECT_SUNNY_DAY: RtSetWeather(WEATHER_SUN_RT, a); break;
    case EFFECT_SANDSTORM: RtSetWeather(WEATHER_SAND_RT, a); break;
    case EFFECT_HAIL: RtSetWeather(WEATHER_HAIL_RT, a); break;
    case EFFECT_MUD_SPORT: a->mudSport = 1; break;
    case EFFECT_WATER_SPORT: a->waterSport = 1; break;
    case EFFECT_TELEPORT:
        if (!gRt->isTrainer && !(gBattleTypeFlags & BATTLE_TYPE_FIRST_BATTLE))
        {
            Rt_EndBattle(a->side == RT_SIDE_PLAYER ? B_OUTCOME_PLAYER_TELEPORTED : B_OUTCOME_MON_FLED);
        }
        else
        {
            // blink to a random free spot
            s32 tries;
            for (tries = 0; tries < 20; tries++)
            {
                s32 x = ARENA_BORDER * 16 + 16 + Random() % (ARENA_W - ARENA_BORDER * 32 - 32);
                s32 y = ARENA_BORDER * 16 + 16 + Random() % (ARENA_H - ARENA_BORDER * 32 - 32);
                if (!RtArena_CircleBlocked(x, y, a->radius))
                {
                    RtFx_Burst(FX_GLOW16, FXP_PSYCHIC, a->x, a->y - TO_FX(a->height / 2), 5, 2, 16);
                    a->x = TO_FX(x);
                    a->y = TO_FX(y);
                    RtFx_Burst(FX_GLOW16, FXP_PSYCHIC, a->x, a->y - TO_FX(a->height / 2), 5, 2, 16);
                    break;
                }
            }
        }
        break;
    case EFFECT_SPLASH:
        a->z = 6;
        RtUi_Toast(sText_NothingHappened, 40);
        break;
    case EFFECT_CONVERSION:
        {
            u8 opts[MAX_MON_MOVES], n = 0;
            for (i = 0; i < MAX_MON_MOVES; i++)
                if (a->moves[i] && !RtActor_HasType(a, gBattleMoves[a->moves[i]].type) && a->moves[i] != MOVE_CURSE)
                    opts[n++] = gBattleMoves[a->moves[i]].type;
            if (n)
                a->types[0] = a->types[1] = opts[Random() % n];
        }
        break;
    case EFFECT_CONVERSION_2:
        if (a->lastHitBy < RT_MAX_ACTORS)
        {
            u16 m = gRt->actors[a->lastHitBy].lastMove;
            if (m)
            {
                u8 at = gBattleMoves[m].type, t;
                for (t = 0; t < NUMBER_OF_MON_TYPES; t++)
                {
                    struct RtActor tmp;
                    tmp.types[0] = tmp.types[1] = t;
                    tmp.foresight = 0;
                    if (t != TYPE_MYSTERY && RtTypeEffectiveness(at, &tmp) < 10 && (Random() & 1))
                    {
                        a->types[0] = a->types[1] = t;
                        break;
                    }
                }
            }
        }
        break;
    case EFFECT_CAMOUFLAGE:
        {
            static const u8 sEnvTypes[] = {TYPE_GRASS, TYPE_GRASS, TYPE_GROUND, TYPE_WATER, TYPE_WATER, TYPE_WATER, TYPE_ROCK, TYPE_ROCK, TYPE_NORMAL, TYPE_NORMAL};
            a->types[0] = a->types[1] = sEnvTypes[gBattleEnvironment < ARRAY_COUNT(sEnvTypes) ? gBattleEnvironment : 9];
        }
        break;
    case EFFECT_INGRAIN:
        a->ingrain = 1;
        break;
    case EFFECT_WISH:
        if (!gRt->sides[a->side].wish)
        {
            gRt->sides[a->side].wish = 120;
            gRt->sides[a->side].wishHp = a->maxHp / 2;
            gRt->sides[a->side].wishTarget = a->id;
        }
        break;
    case EFFECT_HEAL_BELL:
        {
            struct Pokemon *party = a->side == RT_SIDE_PLAYER ? gPlayerParty : gEnemyParty;
            u32 zero = 0;
            for (i = 0; i < PARTY_SIZE; i++)
                SetMonData(&party[i], MON_DATA_STATUS, &zero);
            for (i = 0; i < RT_MAX_ACTORS; i++)
                if (gRt->actors[i].active && gRt->actors[i].side == a->side && gRt->actors[i].mstatus)
                    RtActor_CureStatus(&gRt->actors[i]);
            RtUi_Toast(sText_Cured, 50);
        }
        break;
    case EFFECT_REFRESH:
        if (a->mstatus == MST_PSN || a->mstatus == MST_TOX || a->mstatus == MST_BRN || a->mstatus == MST_PAR)
            RtActor_CureStatus(a);
        break;
    case EFFECT_BATON_PASS:
        {
            extern void RtUi_BatonPass(struct RtActor *a);
            RtUi_BatonPass(a);
        }
        break;
    case EFFECT_DESTINY_BOND:
        a->destinyBond = 300;
        break;
    case EFFECT_GRUDGE:
        a->grudge = 600;
        break;
    case EFFECT_IMPRISON:
        a->imprison = 1;
        break;
    case EFFECT_SNATCH:
        a->snatch = 60;
        break;
    case EFFECT_MAGIC_COAT:
        a->magicCoat = 60;
        break;
    case EFFECT_FOLLOW_ME:
        a->followMe = 1;
        for (i = 0; i < RT_MAX_ACTORS; i++)
            if (gRt->actors[i].active && gRt->actors[i].side != a->side)
                gRt->actors[i].aiTarget = a->id;
        break;
    case EFFECT_HELPING_HAND:
        for (i = 0; i < RT_MAX_ACTORS; i++)
            if (gRt->actors[i].active && gRt->actors[i].side == a->side && &gRt->actors[i] != a)
                gRt->actors[i].helpingHand = 1;
        break;
    case EFFECT_CURSE:
        if (RtActor_HasType(a, TYPE_GHOST))
        {
            struct RtActor *t = Rt_NearestFoe(a);
            if (t)
            {
                RtActor_Damage(a, a->maxHp / 2, a->id, FALSE);
                t->cursed = 1;
                RtFx_Spawn(FX_SKULL16, FXP_SHADOW, t->x, t->y - TO_FX(t->height + 6), 40);
            }
        }
        else
        {
            RtActor_ChangeStage(a, STG_ATK, 1, a->id, TRUE);
            RtActor_ChangeStage(a, STG_DEF, 1, a->id, FALSE);
            RtActor_ChangeStage(a, STG_SPE, -1, a->id, FALSE);
        }
        break;
    case EFFECT_PERISH_SONG:
        RtUi_Toast(sText_PerishAll, 70);
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *t = &gRt->actors[i];
            if (t->active && t->state != AST_GONE && RtActor_IsAlive(t) && t->ability != ABILITY_SOUNDPROOF && !t->perish)
                t->perish = 360;
        }
        break;
    case EFFECT_COUNTER:
        a->counterWin = 45;
        a->counterKind = 0;
        break;
    case EFFECT_MIRROR_COAT:
        a->counterWin = 45;
        a->counterKind = 1;
        break;
    case EFFECT_RECYCLE:
        if (!a->heldItem && a->consumedItem)
        {
            a->heldItem = a->consumedItem;
            a->holdEffect = GetItemHoldEffect(a->heldItem);
            a->holdParam = GetItemHoldEffectParam(a->heldItem);
            a->consumedItem = 0;
        }
        else
        {
            RtUi_Toast(sText_Failed, 40);
        }
        break;
    case EFFECT_TRANSFORM:
        {
            struct RtActor *t = Rt_NearestFoe(a);
            if (t)
                StatusEffectOnTarget(a, t, move);
        }
        break;
    case EFFECT_PSYCH_UP:
        {
            struct RtActor *t = Rt_NearestFoe(a);
            if (t)
                memcpy(a->stages, t->stages, sizeof(a->stages));
        }
        break;
    case EFFECT_BIDE:
    case EFFECT_METRONOME:
    case EFFECT_MIRROR_MOVE:
    case EFFECT_SLEEP_TALK:
    case EFFECT_ASSIST:
    case EFFECT_NATURE_POWER:
        break;
    }
}

void RtMove_OnEnterField(struct RtActor *a)
{
    s32 i;
    a->sentOutFrame = gRt->frame;
    if (!RtActor_IsAlive(a))
        return;
    switch (a->ability)
    {
    case ABILITY_INTIMIDATE:
        NameOf(a, gStringVar1);
        RtUi_ToastFmt(sText_Intimidate, 60);
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *t = &gRt->actors[i];
            if (t->active && t->side != a->side && t->state != AST_GONE && RtActor_IsAlive(t))
                RtActor_ChangeStage(t, STG_ATK, -1, a->id, TRUE);
        }
        break;
    case ABILITY_DRIZZLE:
        RtSetWeather(WEATHER_RAIN_RT, a);
        gRt->weatherTimer = 0;
        break;
    case ABILITY_DROUGHT:
        RtSetWeather(WEATHER_SUN_RT, a);
        gRt->weatherTimer = 0;
        break;
    case ABILITY_SAND_STREAM:
        RtSetWeather(WEATHER_SAND_RT, a);
        gRt->weatherTimer = 0;
        break;
    case ABILITY_TRACE:
        {
            struct RtActor *t = Rt_NearestFoe(a);
            if (t && t->ability != ABILITY_TRACE && t->ability != ABILITY_NONE)
            {
                a->ability = t->ability;
                NameOf(a, gStringVar1);
                RtUi_ToastFmt(sText_TraceAbility, 60);
            }
        }
        break;
    case ABILITY_FORECAST:
        if (WeatherActive())
            RtSetWeather(gRt->weather, NULL);
        break;
    }
    // natural cure is applied when withdrawing (handled by sync)
}

void RtMove_InitWeatherFromField(void)
{
    switch (GetCurrentWeather())
    {
    case WEATHER_RAIN:
    case WEATHER_RAIN_THUNDERSTORM:
    case WEATHER_DOWNPOUR:
        if (gRt->weather == WEATHER_NONE_RT)
        {
            gRt->weather = WEATHER_RAIN_RT;
            gRt->weatherTimer = 0;
        }
        break;
    case WEATHER_SANDSTORM:
        if (gRt->weather == WEATHER_NONE_RT)
        {
            gRt->weather = WEATHER_SAND_RT;
            gRt->weatherTimer = 0;
        }
        break;
    case WEATHER_DROUGHT:
        if (gRt->weather == WEATHER_NONE_RT)
        {
            gRt->weather = WEATHER_SUN_RT;
            gRt->weatherTimer = 0;
        }
        break;
    }
}

void RtMove_TickFuture(void)
{
    // future hits are regular PK_FUTURE projectiles; nothing to do here
}

void RtMove_DoFieldTick(void)
{
    s32 i, s;
    for (s = 0; s < 2; s++)
    {
        struct RtSideState *side = &gRt->sides[s];
        if (side->reflect) side->reflect--;
        if (side->lightScreen) side->lightScreen--;
        if (side->mist) side->mist--;
        if (side->safeguard) side->safeguard--;
        if (side->wish)
        {
            side->wish--;
            if (side->wish == 0)
            {
                struct RtActor *t = NULL;
                for (i = 0; i < RT_MAX_ACTORS; i++)
                {
                    struct RtActor *b = &gRt->actors[i];
                    if (b->active && b->side == s && b->state != AST_GONE && RtActor_IsAlive(b))
                    {
                        if (t == NULL || b->id == side->wishTarget)
                            t = b;
                    }
                }
                if (t)
                {
                    NameOf(&gRt->actors[side->wishTarget], gStringVar1);
                    RtUi_ToastFmt(sText_Wish, 50);
                    RtActor_Heal(t, side->wishHp);
                }
            }
        }
    }
    if (gRt->weather != WEATHER_NONE_RT && gRt->weatherTimer)
    {
        gRt->weatherTimer--;
        if (gRt->weatherTimer == 0)
        {
            gRt->weather = WEATHER_NONE_RT;
            RtUi_Toast(sText_WeatherEnd, 50);
            RtSetWeather(WEATHER_NONE_RT, NULL);
            gRt->weather = WEATHER_NONE_RT;
        }
    }
    // weather damage
    if ((gRt->frame % 240) == 0 && WeatherActive() && (gRt->weather == WEATHER_SAND_RT || gRt->weather == WEATHER_HAIL_RT))
    {
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *a = &gRt->actors[i];
            if (!a->active || a->state == AST_GONE || !RtActor_IsAlive(a) || a->invuln)
                continue;
            if (gRt->weather == WEATHER_SAND_RT)
            {
                if (RtActor_HasType(a, TYPE_ROCK) || RtActor_HasType(a, TYPE_GROUND) || RtActor_HasType(a, TYPE_STEEL) || a->ability == ABILITY_SAND_VEIL)
                    continue;
            }
            else
            {
                if (RtActor_HasType(a, TYPE_ICE))
                    continue;
            }
            RtActor_Damage(a, a->maxHp / 16 ? a->maxHp / 16 : 1, 0xFF, FALSE);
        }
    }
}
