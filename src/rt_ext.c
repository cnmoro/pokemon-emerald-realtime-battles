// Real-time battle extensions: ki (energy), SUPER form and the helpers for the beam struggle.
#include "global.h"
#include "rt_internal.h"
#include "rt_vfx.h"
#include "main.h"
#include "random.h"
#include "sound.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/species.h"
#include "constants/pokemon.h"
#include "data/rt/forms.h"

static const u8 sText_NoKi[] = _("Not enough KI!");
static const u8 sText_SuperUp[] = _("SUPER form!");
static const u8 sText_SuperEnd[] = _("The power fades...");

#define KI_START          300
#define KI_PASSIVE_EVERY  4      // +1 ki every N frames while on foot
#define KI_DASH_COST      40
#define SUPER_KI_NEEDED   800
#define SUPER_FRAMES      720
#define SUPER_HOLD        40     // frames SELECT is held to transform

EWRAM_DATA u8 gRtDebugKi = 0; // debug ROM: 1 = fill the lead's ki, 2 = also transform
extern const u8 gRtDebugEnabled;

static struct RtActorX *X(struct RtActor *a)
{
    return &gRt->ax[a->id];
}

void RtKi_InitActor(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    x->ki = KI_START;
    x->superTimer = 0;
    x->chargeHold = 0;
    x->kiFlash = 0;
}

// ki a move costs: only the heavy hitters cost any
u16 RtKi_MoveCost(u16 move)
{
    u32 pw;
    if (move == MOVE_NONE)
        return 0;
    pw = gBattleMoves[move].power;
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

// SUPER form: Mega / Primal / Deoxys formes where a sprite exists, otherwise a golden power-up
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

// per-frame ki / form upkeep of one actor
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

u16 RtKi_DashCost(void)
{
    return KI_DASH_COST;
}

void RtKi_NoKiToast(struct RtActor *a)
{
    if (a->side == RT_SIDE_PLAYER && a->id == gRt->playerLead && !gRt->toast.active)
        RtUi_Toast(sText_NoKi, 40);
}
