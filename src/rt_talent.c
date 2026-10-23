// Real-time battle extras: dash stamina, talents (Lv 20/40/60 picks) and the rare arena modifiers.
// (Emerald keeps these in rt_ext.c next to the ki system; FireRed has no ki, so they live here.)
#include "global.h"
#include "rt_internal.h"
#include "rt_vfx.h"
#include "battle.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "overworld.h"
#include "pokemon.h"
#include "random.h"
#include "sound.h"
#include "constants/battle.h"
#include "constants/map_types.h"
#include "constants/region_map_sections.h"
#include "constants/songs.h"
#include "constants/weather.h"
#include "constants/moves.h"
#include "constants/species.h"
#include "constants/pokemon.h"
#if RT_LC
#include "data/rt/forms_lc.h"
#else
#include "data/rt/forms.h"
#endif

extern const u8 gRtDebugEnabled;

#define X(a) (&gRt->ax[(a)->id])

// ---------------------------------------------------------------------------
// Dash stamina: a dash costs stamina, which refills by itself shortly after the last dash
// ---------------------------------------------------------------------------
static const u8 sText_NoStamina[] = _("Out of stamina!");

void RtStamina_Init(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    x->stamina = RT_STAMINA_MAX;
    x->staminaWait = 0;
    x->pulse = 0;
    RtKi_InitActor(a);
}

void RtStamina_Tick(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    if (x->dashBuff)
        x->dashBuff--;
    if (x->staminaWait)
        x->staminaWait--;
    else if (x->stamina < RT_STAMINA_MAX)
        x->stamina = x->stamina + 3 > RT_STAMINA_MAX ? RT_STAMINA_MAX : x->stamina + 3;
    RtKi_Tick(a);
}

bool8 RtStamina_Spend(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    u16 cost = RtTalent_Has(a, TALENT_LIGHT_STEP) ? RT_STAMINA_DASH * 3 / 4 : RT_STAMINA_DASH;
    if (x->stamina < cost)
    {
        if (a->side == RT_SIDE_PLAYER && a->id == gRt->playerLead && !gRt->toast.active)
            RtUi_Toast(sText_NoStamina, 30);
        return FALSE;
    }
    x->stamina -= cost;
    x->dashBuff = 240;
    x->staminaWait = 40;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Talents: one of two at Lv 20, 40 and 60, chosen in the SUMMARY; the choice of each tier is one of
// the never-distributed ribbon bits
// ---------------------------------------------------------------------------
void RtTalent_Init(struct RtActor *a, struct Pokemon *mon)
{
    static const u8 sTierLv[3] = {20, 40, 60};
    u8 bits = GetMonData(mon, MON_DATA_UNUSED_RIBBONS), i;
    struct RtActorX *x = X(a);
    x->talents = 0;
    x->dashBuff = 0;
    for (i = 0; i < 3; i++)
        if (a->level >= sTierLv[i])
            x->talents |= 1 << (i * 2 + ((bits >> i) & 1));
}

bool8 RtTalent_Has(struct RtActor *a, u8 talent)
{
    return (X(a)->talents >> talent) & 1;
}

u16 RtTalent_StatMul(struct RtActor *a, u8 stat)
{
    u16 m = 100;
    if (stat == STG_ATK || stat == STG_SPA)
    {
        if (X(a)->dashBuff && RtTalent_Has(a, TALENT_MOMENTUM))
            m = m * 110 / 100;
        if (a->hp * 3 < a->maxHp && RtTalent_Has(a, TALENT_LAST_STAND))
            m = m * 120 / 100;
    }
    return m;
}

u16 RtExt_CdPct(struct RtActor *a)
{
    return RtTalent_Has(a, TALENT_QUICK_MIND) ? 90 : 100;
}

// called from RtActor_Damage before the damage is applied
void RtTalent_OnHit(struct RtActor *v, s32 *amount, u8 src)
{
    struct RtActor *s = (src < RT_MAX_ACTORS && gRt->actors[src].active && gRt->actors[src].side != v->side) ? &gRt->actors[src] : NULL;
    if (RtTalent_Has(v, TALENT_THICK_HIDE))
        *amount = *amount * 92 / 100;
    if (*amount < 1)
        *amount = 1;
    if (s && *amount >= v->hp && RtTalent_Has(s, TALENT_PREDATOR) && RtActor_IsAlive(s))
        RtActor_Heal(s, s->maxHp / 10 + 1);
}

// ---------------------------------------------------------------------------
// Arena modifiers: rare (12 %), and only where they fit the place
// ---------------------------------------------------------------------------
static const u8 sText_ModEruption[] = _("The ground is boiling: ERUPTIONS!");
static const u8 sText_ModFog[] = _("A thick FOG covers the field!");
static const u8 sText_ModWind[] = _("A strong WIND blows across the field!");

EWRAM_DATA u8 gRtDebugArenaMod = 0; // test harness: force a modifier

void RtMod_Setup(void)
{
    gRt->arenaMod = ARENA_MOD_NONE;
    gRt->modTimer = 0;
    if (gRt->isSafari || (gBattleTypeFlags & (BATTLE_TYPE_LINK | BATTLE_TYPE_FIRST_BATTLE | BATTLE_TYPE_OLD_MAN_TUTORIAL
        | BATTLE_TYPE_POKEDUDE | BATTLE_TYPE_LEGENDARY | BATTLE_TYPE_GHOST)))
        return;
    if (gRtDebugEnabled && gRtDebugArenaMod)
        gRt->arenaMod = gRtDebugArenaMod;
    else
    {
        // eruptions around the volcanoes (Cinnabar, Mt. Ember), fog where the weather is foggy or in the
        // haunted / misty places, wind out on the sea routes
        u8 sec = gMapHeader.regionMapSectionId, w = gMapHeader.weather, mod = ARENA_MOD_NONE;
        if (sec == MAPSEC_CINNABAR_ISLAND || sec == MAPSEC_POKEMON_MANSION || sec == MAPSEC_MT_EMBER || sec == MAPSEC_KINDLE_ROAD)
            mod = ARENA_MOD_ERUPTION;
        else if (w == WEATHER_FOG_HORIZONTAL || w == WEATHER_FOG_DIAGONAL || sec == MAPSEC_POKEMON_TOWER
         || sec == MAPSEC_LOST_CAVE || sec == MAPSEC_BERRY_FOREST)
            mod = ARENA_MOD_FOG;
        else if (w == WEATHER_SANDSTORM || gMapHeader.mapType == MAP_TYPE_OCEAN_ROUTE
         || sec == MAPSEC_ROUTE_19 || sec == MAPSEC_ROUTE_20 || sec == MAPSEC_ROUTE_21
         || sec == MAPSEC_BOND_BRIDGE || sec == MAPSEC_RESORT_GORGEOUS || sec == MAPSEC_WATER_LABYRINTH
         || sec == MAPSEC_OUTCAST_ISLAND || sec == MAPSEC_GREEN_PATH || sec == MAPSEC_WATER_PATH
         || sec == MAPSEC_TREASURE_BEACH || sec == MAPSEC_CAPE_BRINK
         || TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING))
            mod = ARENA_MOD_WIND;
        if (mod == ARENA_MOD_NONE || (Random() % 100) >= 12)
            return;
        gRt->arenaMod = mod;
    }
    gRt->windAng = Random() & 0xFF;
    gRt->modTimer = 180;
}

u16 RtMod_RangePct(void)
{
    return gRt->arenaMod == ARENA_MOD_FOG ? 75 : 100;
}

void RtMod_Tick(void)
{
    s32 i;
    if (!gRt->arenaMod)
        return;
    if (gRt->frame == 60)
    {
        static const u8 *const sTexts[] = {NULL, sText_ModEruption, sText_ModFog, sText_ModWind};
        RtUi_Toast(sTexts[gRt->arenaMod], 120);
    }
    switch (gRt->arenaMod)
    {
    case ARENA_MOD_ERUPTION:
        // a warning circle, then a burst of fire there one second later
        if (gRt->modTimer)
            gRt->modTimer--;
        if (gRt->modTimer == 60)
        {
            struct RtActor *t = &gRt->actors[Random() % RT_MAX_ACTORS];
            if (!t->active || t->state == AST_GONE)
                t = Rt_PlayerActor();
            gRt->modX = t->x + TO_FX((s32)(Random() % 41) - 20);
            gRt->modY = t->y + TO_FX((s32)(Random() % 41) - 20);
        }
        if (gRt->modTimer > 0 && gRt->modTimer <= 60 && (gRt->modTimer & 7) == 0)
        {
            struct RtPart *p = RtFx_Spawn(FX_TARGET32, FXP_FIRE, gRt->modX, gRt->modY, 8);
            if (p)
                p->flags |= PF_GROUND | PF_BLEND;
        }
        if (gRt->modTimer == 0)
        {
            RtFx_Impact(RTI_BURN, FXP_FIRE, gRt->modX, gRt->modY);
            RtFx_Burst(FX_SPARK8, FXP_FIRE, gRt->modX, gRt->modY, 8, 2, 20);
            PlaySE(SE_M_EXPLOSION);
            for (i = 0; i < RT_MAX_ACTORS; i++)
            {
                struct RtActor *a = &gRt->actors[i];
                if (!a->active || a->state == AST_GONE || !RtActor_IsAlive(a) || a->invuln)
                    continue;
                if (RtDist(a->x - gRt->modX, a->y - gRt->modY) > TO_FX(22))
                    continue;
                RtActor_Damage(a, a->maxHp / 10 + 1, RT_MAX_ACTORS, FALSE);
                if (RtActor_IsAlive(a) && a->mstatus == MST_NONE && (Random() % 100) < 30 && !RtActor_HasType(a, TYPE_FIRE))
                    RtActor_ApplyStatus(a, MST_BRN, RT_MAX_ACTORS);
            }
            gRt->modTimer = 200 + Random() % 120;
        }
        break;
    case ARENA_MOD_FOG:
        if ((gRt->frame & 7) == 0)
        {
            struct RtPart *p = RtFx_Spawn(FX_SMOKE16, FXP_WHITE, TO_FX(gRt->camX + Random() % DISPLAY_WIDTH), TO_FX(gRt->camY + Random() % DISPLAY_HEIGHT), 60);
            if (p)
            {
                p->flags |= PF_BLEND;
                p->vx = 40;
            }
        }
        break;
    case ARENA_MOD_WIND:
        {
            s32 wx = RtCos(gRt->windAng), wy = RtSin(gRt->windAng);
            // projectiles drift with the wind
            for (i = 0; i < RT_MAX_PROJ; i++)
            {
                struct RtProj *p = &gRt->proj[i];
                if (p->kind == PK_BULLET && !p->delay)
                {
                    p->x += wx / 4;
                    p->y += wy / 4;
                }
            }
            // so do the fighters, a little
            if ((gRt->frame & 1) == 0)
                for (i = 0; i < RT_MAX_ACTORS; i++)
                {
                    struct RtActor *a = &gRt->actors[i];
                    if (a->active && a->state == AST_NORMAL && RtActor_IsAlive(a))
                    {
                        a->x += wx / 8;
                        a->y += wy / 8;
                    }
                }
            if ((gRt->frame & 3) == 0)
            {
                struct RtPart *p = RtFx_Spawn(FX_SPARK8, FXP_WHITE, TO_FX(gRt->camX + Random() % DISPLAY_WIDTH), TO_FX(gRt->camY + Random() % DISPLAY_HEIGHT), 30);
                if (p)
                {
                    p->vx = wx * 3;
                    p->vy = wy * 3;
                    p->flags |= PF_BLEND;
                }
            }
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Ki and SUPER form (ported from Emerald): ki charges on foot and from hits, the heavy hitters cost
// some, and with 800 ki holding SELECT turns the POKeMON into its SUPER form for a while (Mega /
// Primal / Deoxys formes where a sprite exists, otherwise a golden power-up).
// ---------------------------------------------------------------------------
static const u8 sText_SuperUp[] = _("SUPER form!");
static const u8 sText_SuperEnd[] = _("The power fades...");

#define KI_START          300
#define KI_PASSIVE_EVERY  4      // +1 ki every N frames while on foot
#define SUPER_KI_NEEDED   800
#define SUPER_FRAMES      720
#define SUPER_HOLD        40     // frames SELECT is held to transform

#ifdef RTM
#define KI_MOVE_POWER(m) (RTM(m).power)
#else
#define KI_MOVE_POWER(m) (gBattleMoves[m].power)
#endif

EWRAM_DATA u8 gRtDebugKi = 0; // debug ROM: 1 = fill the lead's ki, 2 = also transform

void RtKi_InitActor(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    x->ki = KI_START;
    x->superTimer = 0;
    x->chargeHold = 0;
    x->kiFlash = 0;
    x->superKind = 0;
    x->oldGfx = 0;
}

// ki a move costs: only the heavy hitters cost any
u16 RtKi_MoveCost(u16 move)
{
    u32 pw;
    if (move == MOVE_NONE)
        return 0;
    pw = KI_MOVE_POWER(move);
    if (pw == 0)
        return 0;
    if (pw == 1)
        return 90;   // variable power
    if (pw < 85)
        return 0;
    if (pw < 110)
        return 100;
    if (pw < 140)
        return 200;
    return 300;
}

bool8 RtKi_CanCast(struct RtActor *a, u16 move)
{
    u16 cost = RtKi_MoveCost(move);
    if (X(a)->superTimer)
        cost /= 2;
    return X(a)->ki >= cost;
}

bool8 RtKi_Spend(struct RtActor *a, u16 cost)
{
    struct RtActorX *x = X(a);
    if (x->superTimer)
        cost /= 2;
    if (x->ki < cost)
        return FALSE;
    x->ki -= cost;
    return TRUE;
}

void RtKi_Gain(struct RtActor *a, u16 amount)
{
    struct RtActorX *x = X(a);
    u32 v = x->ki + amount;
    if (v > RT_KI_MAX)
        v = RT_KI_MAX;
    if (x->ki < SUPER_KI_NEEDED && v >= SUPER_KI_NEEDED && !x->superTimer)
        x->kiFlash = 30;
    x->ki = v;
}

// hits charge ki: the attacker a little, the one who was hit a bit less
void RtKi_OnDamage(struct RtActor *a, s32 amount, u8 src)
{
    u32 pct = a->maxHp ? (u32)amount * 100 / a->maxHp : 0;
    if (pct > 60)
        pct = 60;
    RtKi_Gain(a, 15 + pct * 3);
    if (src < RT_MAX_ACTORS && gRt->actors[src].active && gRt->actors[src].side != a->side)
        RtKi_Gain(&gRt->actors[src], 25 + pct * 4);
}

static u16 PickForm(struct RtActor *a)
{
    s32 i, n = 0;
    u16 opt[4];
    for (i = 0; i < RT_FORM_COUNT && n < 4; i++)
        if (sRtFormGfx[i].species == a->species && sRtFormGfx[i].gfx >= RT_FORM_BASE)
            opt[n++] = sRtFormGfx[i].gfx;
    if (n == 0)
        return 0;
    if (a->species == SPECIES_DEOXYS)
    {
        struct RtActor *t = Rt_NearestFoe(a);
        if (a->hp * 5 < a->maxHp * 2 && n > 1)
            return opt[1];                    // Defense forme when worn down
        if (t && n > 2 && t->spe > a->spe)
            return opt[2];                    // Speed forme against a faster foe
        return opt[0];
    }
    if (n > 1)
        return opt[a->spa > a->atk ? n - 1 : 0];
    return opt[0];
}

static void ApplyFormType(struct RtActor *a, u16 gfx)
{
    struct RtActorX *x = X(a);
    u8 t0 = a->types[0], t1 = a->types[1];
    x->oldType[0] = t0;
    x->oldType[1] = t1;
    if (a->species == SPECIES_CHARIZARD && gfx == RT_FORM_BASE + 1)
        t1 = TYPE_DRAGON;
    else if (a->species == SPECIES_SHARPEDO)
        t1 = TYPE_DARK;
    else if (a->species == SPECIES_GROUDON)
        t1 = TYPE_FIRE;
    else if (a->species == SPECIES_ALTARIA)
        t1 = TYPE_NORMAL;
#ifdef SPECIES_LUCARIO
    else if (a->species == SPECIES_LOPUNNY)
        t1 = TYPE_FIGHTING;
#endif
    a->types[0] = t0;
    a->types[1] = t1;
}

static void EndSuper(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    x->superTimer = 0;
    if (x->oldGfx)
    {
        a->gfxDex = x->oldGfx;
        a->types[0] = x->oldType[0];
        a->types[1] = x->oldType[1];
        a->radius = gRtSpeciesGfx[a->gfxDex].radius;
        a->height = gRtSpeciesGfx[a->gfxDex].height;
        a->palDirty = TRUE;
        a->needLoad = TRUE;
        x->oldGfx = 0;
    }
    x->superKind = 0;
    a->flash = 6;
    RtFx_Burst(FX_SPARK8, FXP_WHITE, a->x, a->y - TO_FX(a->height / 2), 6, 2, 16);
    if (a->side == RT_SIDE_PLAYER)
        RtUi_Toast(sText_SuperEnd, 50);
}

static void StartSuper(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    u16 form = PickForm(a);
    x->superTimer = SUPER_FRAMES;
    x->superKind = 0;
    x->ki = x->ki > 400 ? x->ki - 400 : 0;
    if (form)
    {
        x->oldGfx = a->gfxDex;
        a->gfxDex = form;
        ApplyFormType(a, form);
        a->radius = gRtSpeciesGfx[form].radius;
        a->height = gRtSpeciesGfx[form].height;
        a->shiny = 0;
        a->palDirty = TRUE;
        a->needLoad = TRUE;
        if (a->species == SPECIES_DEOXYS)
            x->superKind = form == RT_FORM_BASE + 49 ? 1 : (form == RT_FORM_BASE + 50 ? 2 : 3);
    }
    a->flash = 12;
    RtFx_Flash(0, 8);
    RtFx_Shake(3, 20);
    RtFx_Dim(5, 24);
    RtFx_Ring(FX_SPARK8, FXP_GOLD, a->x, a->y - TO_FX(a->height / 2), 12, 24, 22);
    RtFx_Impact(RTI_STAR, FXP_GOLD, a->x, a->y - TO_FX(a->height / 2));
    PlaySE(SE_M_COSMIC_POWER);
    gRt->pauseFrames = 10;
    if (a->side == RT_SIDE_PLAYER)
        RtUi_Toast(sText_SuperUp, 70);
}

bool8 RtExt_TrySuper(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    if (x->superTimer || x->ki < SUPER_KI_NEEDED || a->state != AST_NORMAL)
        return FALSE;
    StartSuper(a);
    return TRUE;
}

// per-frame ki / form upkeep of one actor (called from RtStamina_Tick)
void RtKi_Tick(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    if (gRtDebugEnabled && gRtDebugKi && a->side == RT_SIDE_PLAYER && a->id == gRt->playerLead)
    {
        x->ki = RT_KI_MAX;
        if (gRtDebugKi == 2)
            RtExt_TrySuper(a);
        gRtDebugKi = 0;
    }
    if (a->state == AST_NORMAL && (gRt->frame % KI_PASSIVE_EVERY) == 0 && x->ki < RT_KI_MAX)
        x->ki++;
    if (x->kiFlash)
        x->kiFlash--;
    if (x->superTimer)
    {
        x->superTimer--;
        if (x->superTimer == 0)
            EndSuper(a);
        else if ((gRt->frame & 3) == 0)
        {
            struct RtPart *p = RtFx_Spawn(FX_SPARK8, FXP_GOLD, a->x + TO_FX((s32)(Random() % 17) - 8), a->y - TO_FX(Random() % (a->height + 1)), 14);
            if (p)
                p->vy = -TO_FX(1) / 2;
        }
    }
    // the AI transforms as soon as it can, once its HP has dropped a bit
    if (a->side != RT_SIDE_PLAYER || a->id != gRt->playerLead)
    {
        if (!x->superTimer && x->ki >= SUPER_KI_NEEDED && a->hp * 3 < a->maxHp * 2 && a->state == AST_NORMAL)
            RtExt_TrySuper(a);
    }
}

// player input: SELECT held = transform (when ready), tap = dash
void RtExt_PlayerInput(struct RtActor *a, u16 held, u16 pressed)
{
    struct RtActorX *x = X(a);
    bool8 ready = x->ki >= SUPER_KI_NEEDED && !x->superTimer;
    if (held & SELECT_BUTTON)
    {
        if (x->chargeHold < 0xFFFF)
            x->chargeHold++;
        if (ready && x->chargeHold >= SUPER_HOLD)
        {
            RtExt_TrySuper(a);
            x->chargeHold = 0xFFFF; // wait for the release
        }
        else if (ready && x->chargeHold > 8 && (x->chargeHold & 3) == 0)
            RtFx_Burst(FX_SPARK8, FXP_GOLD, a->x, a->y - TO_FX(a->height / 2), 2, 1, 14); // charging aura
        // a ready charge is decided on release (tap = dash); otherwise dash right away
        if (!ready && (pressed & SELECT_BUTTON))
            a->inDash = TRUE;
    }
    else
    {
        if (ready && x->chargeHold > 0 && x->chargeHold < 14)
            a->inDash = TRUE;
        x->chargeHold = 0;
    }
}
