// Real-time battle extensions: ki (energy), SUPER form and the helpers for the beam struggle.
#include "global.h"
#include "rt_internal.h"
#include "rt_vfx.h"
#include "main.h"
#include "string_util.h"
#include "pokemon.h"
#include "pokedex.h"
#include "battle_setup.h"
#include "field_player_avatar.h"
#include "constants/region_map_sections.h"
#include "constants/battle.h"
#include "battle.h"
#include "constants/songs.h"
#include "constants/pokedex.h"
#include "random.h"
#include "sound.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/species.h"
#include "constants/abilities.h"
#include "constants/pokemon.h"
#include "data/rt/forms.h"

enum { BLS_NONE, BLS_DESERT, BLS_SEA, BLS_METEOR, BLS_SPIRIT, BLS_FOREST, BLS_FROST, BLS_MAGMA, BLS_STONE, BLS_COUNT };
static const u8 sBlessName[BLS_COUNT][16] = {
    _("None"), _("Desert Wind"), _("Deep Pressure"), _("Meteor Light"), _("Spirit Tribute"),
    _("Forest Vigor"), _("Frost Armor"), _("Magma Heart"), _("Stone Skin")
};
static const u8 sGuardIntro[BLS_COUNT][34] = {
    _(""), _("The Desert Spirit rises!"), _("An ancient guardian of the sea!"), _("A child of the meteors descends!"),
    _("A restless spirit of Mt. Pyre!"), _("The forest elder awakens!"), _("The cave's cold heart wakes!"),
    _("The mountain's fury erupts!"), _("The titan of the deep caves!")
};
static const u8 sText_Blessing[] = _("BLESSING: {STR_VAR_1}");
static const u8 sText_BlessLeft[] = _("{STR_VAR_1}: {STR_VAR_2} battles left");
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
    x->ki = KI_START + (a->side == RT_SIDE_PLAYER ? RtRogue_KiStart() : 0);
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
    RtKi_Gain(a, (15 + pct * 3) * RtKi_RuneGainPct(a) / 100);
    if (src < RT_MAX_ACTORS && gRt->actors[src].active && gRt->actors[src].side != a->side)
        RtKi_Gain(&gRt->actors[src], (25 + pct * 4) * RtKi_RuneGainPct(&gRt->actors[src]) / 100);
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
static const u8 sText_Sudden[] = _("SUDDEN DEATH!");
void RtExt_BuildTick(struct RtActor *a);
void RtKi_Tick(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    RtExt_BuildTick(a);
    RtExt_TerrainTick(a);
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

// ---------------------------------------------------------------------------
// Builds: one rune (passive) + one summoner spell (START held), two presets per Pokemon
// ---------------------------------------------------------------------------
static const u8 sRuneNames[RUNE_COUNT][9] = {
    _("None"), _("Fury"), _("Vampire"), _("Thorns"), _("Guard"), _("Swift"), _("Focus"), _("Ki")
};
static const u8 sSpellNames[SPELL_COUNT][9] = {
    _("None"), _("Barrier"), _("Heal"), _("Purify"), _("Blink"), _("Frenzy"), _("Surge")
};
static const u8 sText_SpellCd[] = _("Spell recharging...");
static const u8 sText_SpellOk[] = _("{STR_VAR_1} cast!");
#define SPELL_CD 2700

const u8 *RtBuild_RuneName(u8 r) { return sRuneNames[r % RUNE_COUNT]; }
const u8 *RtBuild_SpellName(u8 sp) { return sSpellNames[sp % SPELL_COUNT]; }

// the overlay lives in the (never used) link-record area of SaveBlock2
struct RtSave *RtSave_Get(void)
{
    struct RtSave *sv = (struct RtSave *)gSaveBlock2Ptr->hallRecords2P;
    STATIC_ASSERT(sizeof(struct RtSave) <= sizeof(gSaveBlock2Ptr->hallRecords2P), RtSaveFits);
    if (sv->magic != 0x7254)
    {
        CpuFill32(0, sv, sizeof(*sv));
        sv->magic = 0x7254;
        sv->ver = 1;
    }
    return sv;
}

static u16 MonKey(struct Pokemon *mon)
{
    u32 pid = GetMonData(mon, MON_DATA_PERSONALITY);
    u16 k = (pid ^ (pid >> 16)) & 0xFFFF;
    return k ? k : 1;
}

static struct RtBuildEntry *FindEntry(struct Pokemon *mon, bool8 create)
{
    struct RtSave *sv = RtSave_Get();
    u16 k = MonKey(mon);
    s32 i;
    for (i = 0; i < 8; i++)
        if (sv->builds[i].key == k)
            return &sv->builds[i];
    if (!create)
        return NULL;
    i = sv->nextSlot++ & 7;           // round robin: the oldest entry is recycled
    sv->builds[i].key = k;
    sv->builds[i].sel = 0;
    sv->builds[i].p[0] = sv->builds[i].p[1] = 0;
    return &sv->builds[i];
}

u8 RtBuild_Get(struct Pokemon *mon, u8 preset)
{
    struct RtBuildEntry *e = FindEntry(mon, FALSE);
    return e ? e->p[preset & 1] : 0;
}

void RtBuild_Set(struct Pokemon *mon, u8 preset, u8 val)
{
    FindEntry(mon, TRUE)->p[preset & 1] = val;
}

u8 RtBuild_GetSel(struct Pokemon *mon)
{
    struct RtBuildEntry *e = FindEntry(mon, FALSE);
    return e ? e->sel : 0;
}

void RtBuild_SetSel(struct Pokemon *mon, u8 sel)
{
    FindEntry(mon, TRUE)->sel = sel & 1;
}

// foes (and the partner) get a build picked from their stats
static u8 AutoBuild(struct RtActor *a)
{
    u32 h = (a->species * 7 + a->level * 13) % 6;
    u8 rune, spell;
    if (a->atk > a->spa) rune = (h & 1) ? RUNE_FURY : RUNE_VAMP;
    else rune = (h & 1) ? RUNE_FOCUS : RUNE_KI;
    if (a->def + a->spd > a->atk + a->spa) rune = (h & 1) ? RUNE_GUARD : RUNE_THORNS;
    if (a->spe > 100 && h == 4) rune = RUNE_SWIFT;
    spell = SPELL_BARRIER + (h % 3) * 2 % 5;   // barrier / purify / frenzy ...
    if (h == 5) spell = SPELL_HEAL;
    return rune | (spell << 4);
}

void RtBuild_Load(struct RtActor *a, struct Pokemon *mon)
{
    struct RtActorX *x = X(a);
    u8 v;
    if (a->side == RT_SIDE_PLAYER && !gRt->isSafari)
    {
        struct RtBuildEntry *e = FindEntry(mon, FALSE);
        v = e ? e->p[e->sel & 1] : 0;
    }
    else if (gRogueMode == 2 && a->side == RT_SIDE_ENEMY && mon >= gEnemyParty && mon < gEnemyParty + PARTY_SIZE)
        v = gRogueChalBuild[mon - gEnemyParty];
    else
        v = AutoBuild(a);
    x->rune = v & 15;
    x->spell = v >> 4;
    x->spellCd = 600;   // a short opening cooldown so that nothing happens at frame 0
    x->buffTimer = 0;
    x->buffKind = 0;
}

u16 RtKi_RuneGainPct(struct RtActor *a)
{
    return (X(a)->rune == RUNE_KI ? 150 : 100) * (a->side == RT_SIDE_PLAYER && RtSave_Get()->meta[0] == BLS_METEOR ? 150 : 100) / 100
         * (gRogueMode && a->side == RT_SIDE_PLAYER ? RtRogue_KiPct() : 100) / 100;
}

// stat multiplier of rune / temporary buff, in percent
u16 RtExt_BlessStatMul(struct RtActor *a, u8 stat);
u16 RtExt_BlessDmgTakenPct(struct RtActor *a);
u16 RtExt_StatMul(struct RtActor *a, u8 stat)
{
    struct RtActorX *x = X(a);
    u16 m = RtExt_BlessStatMul(a, stat);
    if (gRogueMode) m = m * RtRogue_StatMul(a, stat) / 100;
    bool8 off = stat == STG_ATK || stat == STG_SPA;
    bool8 def = stat == STG_DEF || stat == STG_SPD;
    if (x->rune == RUNE_FURY && off) m = 110;
    else if (x->rune == RUNE_GUARD && def) m = 115;
    else if (x->rune == RUNE_SWIFT && stat == STG_SPE) m = 115;
    if (x->buffTimer && x->buffKind == SPELL_FRENZY)
    {
        if (off) m = m * 130 / 100;
        else if (stat == STG_SPE) m = m * 115 / 100;
    }
    return m;
}

u16 RtExt_CdPct(struct RtActor *a)
{
    u16 p = X(a)->rune == RUNE_FOCUS ? 88 : 100;
    if (gRogueMode && a->side == RT_SIDE_PLAYER) p = p * RtRogue_CdPct() / 100;
    if (X(a)->buffTimer && X(a)->buffKind == SPELL_FRENZY)
        p = p * 85 / 100;
    return p;
}

// called from RtActor_Damage before the damage is applied: barrier, thorns, vampire
void RtExt_OnHit(struct RtActor *v, s32 *amount, u8 src)
{
    struct RtActorX *x = X(v);
    struct RtActor *s = (src < RT_MAX_ACTORS && gRt->actors[src].active && gRt->actors[src].side != v->side) ? &gRt->actors[src] : NULL;
    *amount = *amount * RtExt_BlessDmgTakenPct(v) / 100;
    if (*amount < 1) *amount = 1;
    if (x->buffTimer && x->buffKind == SPELL_BARRIER)
    {
        *amount = (*amount + 1) / 2;
        RtFx_Burst(FX_SPARK8, FXP_WHITE, v->x, v->y - TO_FX(v->height / 2), 2, 1, 10);
    }
    if (!s)
        return;
    if (X(s)->rune == RUNE_VAMP && RtActor_IsAlive(s))
        RtActor_Heal(s, (*amount) / 8 > 0 ? (*amount) / 8 : 1);
    if (s->side == RT_SIDE_PLAYER && RtSave_Get()->meta[0] == BLS_SPIRIT && RtActor_IsAlive(s))
        RtActor_Heal(s, (*amount) / 16 > 0 ? (*amount) / 16 : 1);
    if (x->rune == RUNE_THORNS && v->hp > *amount && RtActor_IsAlive(s) && s->state != AST_GONE)
        RtActor_Damage(s, (*amount) / 8 > 0 ? (*amount) / 8 : 1, RT_MAX_ACTORS, TRUE);
}

static void DoSpell(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    s32 cx = a->x, cy = a->y - TO_FX(a->height / 2);
    switch (x->spell)
    {
    case SPELL_BARRIER:
        x->buffKind = SPELL_BARRIER; x->buffTimer = 240;
        RtFx_Ring(FX_SPARK8, FXP_WHITE, cx, cy, 10, 18, 20);
        break;
    case SPELL_HEAL:
        RtActor_Heal(a, a->maxHp * 30 / 100);
        break;
    case SPELL_PURIFY:
        a->mstatus = MST_NONE;
        a->mstatusTimer = 0;
        a->hpBarDirty = TRUE;
        RtFx_Burst(FX_PLUS8, FXP_HEAL, cx, cy, 6, 2, 24);
        break;
    case SPELL_BLINK:
        {
            s32 step;
            for (step = 80; step >= 16; step -= 8)
            {
                s32 nx = a->x + RtCos(a->aim) * step, ny = a->y + RtSin(a->aim) * step;
                if (!RtArena_CircleBlocked(nx, ny, a->radius))
                {
                    RtFx_Burst(FX_SPARK8, FXP_WATER, cx, cy, 6, 2, 16);
                    a->x = nx; a->y = ny; a->vx = a->vy = 0;
                    RtFx_Burst(FX_SPARK8, FXP_WATER, a->x, a->y - TO_FX(a->height / 2), 6, 2, 16);
                    break;
                }
            }
        }
        break;
    case SPELL_FRENZY:
        x->buffKind = SPELL_FRENZY; x->buffTimer = 360;
        RtFx_Ring(FX_SPARK8, FXP_FIRE, cx, cy, 10, 18, 20);
        break;
    case SPELL_SURGE:
        RtKi_Gain(a, 400);
        RtFx_Ring(FX_SPARK8, FXP_GOLD, cx, cy, 10, 18, 20);
        break;
    default:
        return;
    }
    a->flash = 6;
    x->spellCd = SPELL_CD;
    PlaySE(SE_M_COSMIC_POWER);
    if (a->side == RT_SIDE_PLAYER)
    {
        u8 *d = gStringVar1;
        StringCopy(d, RtBuild_SpellName(x->spell));
        StringExpandPlaceholders(gStringVar4, sText_SpellOk);
        RtUi_Toast(gStringVar4, 40);
    }
}

bool8 RtExt_CastSpell(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    if (x->spell == SPELL_NONE || a->state != AST_NORMAL)
        return FALSE;
    if (x->spellCd)
    {
        if (a->side == RT_SIDE_PLAYER && !gRt->toast.active)
            RtUi_Toast(sText_SpellCd, 30);
        return FALSE;
    }
    DoSpell(a);
    return TRUE;
}

static void AiSpell(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    struct RtActor *t;
    if (x->spell == SPELL_NONE || x->spellCd || a->state != AST_NORMAL)
        return;
    t = Rt_NearestFoe(a);
    if (!t)
        return;
    switch (x->spell)
    {
    case SPELL_HEAL: if (a->hp * 10 > a->maxHp * 4) return; break;
    case SPELL_PURIFY: if (a->mstatus == MST_NONE) return; break;
    case SPELL_BARRIER: if (a->hp * 10 > a->maxHp * 8 || RtDist(t->x - a->x, t->y - a->y) > TO_FX(90)) return; break;
    case SPELL_FRENZY: if (a->hp * 10 < a->maxHp * 4 || RtDist(t->x - a->x, t->y - a->y) > TO_FX(110)) return; break;
    case SPELL_SURGE: if (x->ki > 250) return; break;
    case SPELL_BLINK: if (a->hp * 10 > a->maxHp * 3) return; a->aim += 128; break;
    }
    DoSpell(a);
}

// START: short tap (released within 20 frames) opens the pause menu, hold casts the spell
bool8 RtExt_StartPoll(void)
{
    static EWRAM_DATA u8 sHold = 0;
    struct RtActor *p = Rt_PlayerActor();
    if (gMain.heldKeys & START_BUTTON)
    {
        if (sHold < 0xFE)
        {
            sHold++;
            if (sHold == 20 && p)
            {
                RtExt_CastSpell(p);
                sHold = 0xFF;
            }
        }
        return FALSE;
    }
    if (sHold && sHold < 20)
    {
        sHold = 0;
        return TRUE;
    }
    sHold = 0;
    return FALSE;
}

void RtExt_BuildTick(struct RtActor *a)
{
    struct RtActorX *x = X(a);
    if (x->spellCd) x->spellCd--;
    if (gRogueMode && gRt->frame >= 2700 && a->active && a->hp > 0 && a->state == AST_NORMAL)
    {
        // sudden death: nothing may stalemate (e.g. a Ghost move against a Normal type)
        if (gRt->frame == 2700 && a->id == gRt->playerLead)
            RtUi_Toast(sText_Sudden, 100);
        if (gRt->frame % 60 == 0)
            RtActor_Damage(a, a->maxHp / 40 + 1, 255, TRUE);
    }
    if (gRt->objective && a->side == RT_SIDE_ENEMY && a->id == 2 && gRt->frame == 120)
        RtUi_Toast(sGuardIntro[gRt->objective], 110);
    if (a->side == RT_SIDE_PLAYER && a->id == gRt->playerLead && gRt->frame == 240 && RtSave_Get()->meta[0] && !gRt->objective)
    {
        StringCopy(gStringVar1, sBlessName[RtSave_Get()->meta[0] % BLS_COUNT]);
        ConvertIntToDecimalStringN(gStringVar2, RtSave_Get()->meta[1], STR_CONV_MODE_LEFT_ALIGN, 2);
        StringExpandPlaceholders(gStringVar4, sText_BlessLeft);
        RtUi_Toast(gStringVar4, 80);
    }
    if (a->side == RT_SIDE_PLAYER && a->state == AST_NORMAL && RtSave_Get()->meta[0] == BLS_FOREST && (gRt->frame % 70) == 0 && a->hp < a->maxHp)
        RtActor_Heal(a, a->maxHp / 60 > 0 ? a->maxHp / 60 : 1);
    if (x->buffTimer && --x->buffTimer == 0)
        x->buffKind = 0;
    if (a->side != RT_SIDE_PLAYER || a->id != gRt->playerLead)
        AiSpell(a);
}

// ---------------------------------------------------------------------------
// Evolution in the middle of the fight (right after a level-up)
// ---------------------------------------------------------------------------
extern u8 gLeveledUpInBattle;
static const u8 sText_Evolved[] = _("{STR_VAR_1} evolved into {STR_VAR_2}!");

bool8 RtExt_TryEvolve(struct RtActor *a, struct Pokemon *mon)
{
    struct RtActorX *x = X(a);
    u16 from = a->species, to = GetEvolutionTargetSpecies(mon, EVO_MODE_NORMAL, 0);
    u16 oldMax = a->maxHp;
    u8 nick[POKEMON_NAME_LENGTH + 1];
    bool8 defaultName;
    if (to == SPECIES_NONE || a->state == AST_GONE || a->state == AST_FAINT || !RtActor_IsAlive(a))
        return FALSE;
    if (x->superTimer)
        EndSuper(a);
    GetMonData(mon, MON_DATA_NICKNAME, nick);
    StringGet_Nickname(nick);
    GetSpeciesName(gStringVar3, from);
    defaultName = StringCompare(nick, gStringVar3) == 0;
    StringCopy(gStringVar1, nick);
    SetMonData(mon, MON_DATA_SPECIES, &to);
    GetSpeciesName(gStringVar3, to);
    if (defaultName)
        SetMonData(mon, MON_DATA_NICKNAME, gStringVar3);
    CalculateMonStats(mon);
    GetSetPokedexFlag(SpeciesToNationalPokedexNum(to), FLAG_SET_SEEN);
    GetSetPokedexFlag(SpeciesToNationalPokedexNum(to), FLAG_SET_CAUGHT);
    gLeveledUpInBattle &= ~(1 << a->partyIdx);
    a->species = to;
    a->dex = SpeciesToNationalPokedexNum(to);
    if (a->dex == 0 || a->dex > NATIONAL_DEX_COUNT)
        a->dex = 1;
    a->gfxDex = a->dex;
    a->types[0] = gSpeciesInfo[to].types[0];
    a->types[1] = gSpeciesInfo[to].types[1];
    a->ability = GetMonAbility(mon);
    a->maxHp = GetMonData(mon, MON_DATA_MAX_HP);
    a->hp += a->maxHp - oldMax;
    if (a->hp > a->maxHp) a->hp = a->maxHp;
    a->atk = GetMonData(mon, MON_DATA_ATK);
    a->def = GetMonData(mon, MON_DATA_DEF);
    a->spe = GetMonData(mon, MON_DATA_SPEED);
    a->spa = GetMonData(mon, MON_DATA_SPATK);
    a->spd = GetMonData(mon, MON_DATA_SPDEF);
    a->radius = gRtSpeciesGfx[a->gfxDex].radius;
    a->height = gRtSpeciesGfx[a->gfxDex].height;
    a->palDirty = TRUE;
    a->needLoad = TRUE;
    a->flash = 14;
    a->hpBarDirty = TRUE;
    StringCopy(gStringVar2, gStringVar3);
    StringExpandPlaceholders(gStringVar4, sText_Evolved);
    RtUi_Toast(gStringVar4, 110);
    RtFx_Flash(0, 14);
    RtFx_Shake(3, 24);
    RtFx_Dim(5, 40);
    RtFx_Ring(FX_SPARK8, FXP_WHITE, a->x, a->y - TO_FX(a->height / 2), 14, 28, 24);
    RtFx_Impact(RTI_STAR, FXP_WHITE, a->x, a->y - TO_FX(a->height / 2));
    PlayCry_Normal(to, 0);
    gRt->pauseFrames = 30;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Terrain with its own rules (taken from the metatiles of the arena)
// ---------------------------------------------------------------------------
static const u8 sText_TWater[] = _("WATER: Water fast, Electric +30%, Fire -25%");
static const u8 sText_TSand[]  = _("SAND: slows most, Ground/Rock +15%");
static const u8 sText_TGrass[] = _("GRASS: Grass heals/faster, Fire +20%");
static const u8 sText_TIce[]   = _("ICE: Ice moves +20%, Fire -15%");

static bool8 Airborne(struct RtActor *a)
{
    return RtActor_HasType(a, TYPE_FLYING) || a->ability == ABILITY_LEVITATE || a->invuln == 1 || a->z > 6;
}

u8 RtExt_TerrainAt(struct RtActor *a)
{
    s32 cx = FROM_FX(a->x) >> 4, cy = FROM_FX(a->y) >> 4;
    if (cx < 0 || cy < 0 || cx >= ARENA_MW || cy >= ARENA_MH)
        return RTT_NONE;
    return gRt->terrain[cy][cx];
}

u16 RtExt_TerrainSpeedPct(struct RtActor *a)
{
    u8 t = RtExt_TerrainAt(a);
    if (t == RTT_NONE || Airborne(a))
        return 100;
    switch (t)
    {
    case RTT_WATER:
        if (RtActor_HasType(a, TYPE_WATER) || a->ability == ABILITY_SWIFT_SWIM) return 115;
        return 82;
    case RTT_SAND:
        if (RtActor_HasType(a, TYPE_GROUND) || RtActor_HasType(a, TYPE_ROCK) || a->ability == ABILITY_SAND_VEIL) return 100;
        return 88;
    case RTT_GRASS:
        return RtActor_HasType(a, TYPE_GRASS) ? 110 : 100;
    }
    return 100;
}

u16 RtExt_TerrainDmgPct(struct RtActor *atk, struct RtActor *def, u8 type)
{
    u8 ta = RtExt_TerrainAt(atk), td = RtExt_TerrainAt(def);
    u16 m = 100;
    if (type == TYPE_ELECTRIC && td == RTT_WATER && !Airborne(def)) m = m * 130 / 100;
    if (type == TYPE_FIRE && (ta == RTT_WATER || td == RTT_WATER)) m = m * 75 / 100;
    if (type == TYPE_FIRE && ta == RTT_GRASS) m = m * 120 / 100;
    if (type == TYPE_FIRE && ta == RTT_ICE) m = m * 85 / 100;
    if (type == TYPE_GRASS && ta == RTT_GRASS) m = m * 120 / 100;
    if ((type == TYPE_GROUND || type == TYPE_ROCK) && ta == RTT_SAND) m = m * 115 / 100;
    if (type == TYPE_ICE && ta == RTT_ICE) m = m * 120 / 100;
    if (type == TYPE_WATER && ta == RTT_WATER) m = m * 110 / 100;
    return m;
}

void RtExt_TerrainTick(struct RtActor *a)
{
    u8 t;
    if (a->state != AST_NORMAL)
        return;
    t = RtExt_TerrainAt(a);
    if (t == RTT_GRASS && RtActor_HasType(a, TYPE_GRASS) && (gRt->frame & 63) == 0 && a->hp < a->maxHp)
        RtActor_Heal(a, a->maxHp / 40 > 0 ? a->maxHp / 40 : 1);
    if (a->side == RT_SIDE_PLAYER && a->id == gRt->playerLead && t != gRt->terrainShown)
    {
        gRt->terrainShown = t;
        if (t != RTT_NONE && !gRt->toast.active)
            RtUi_Toast(t == RTT_WATER ? sText_TWater : t == RTT_SAND ? sText_TSand : t == RTT_GRASS ? sText_TGrass : sText_TIce, 70);
    }
}

// ---------------------------------------------------------------------------
// Map objectives: in some places the wild monster is the guardian of the area. The arena is much
// bigger (34x16 metatiles, 3x the normal field) and beating the guardian grants a blessing for a
// number of battles.
// ---------------------------------------------------------------------------


#define BLESS_BATTLES 10

u8 RtExt_BuffId(void) { return RtSave_Get()->meta[0]; }
u8 RtExt_BuffLeft(void) { return RtSave_Get()->meta[1]; }

static u8 BiomeOfMap(void)
{
    u8 sec = gMapHeader.regionMapSectionId;
    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING))
        return BLS_SEA;
    switch (sec)
    {
    case MAPSEC_ROUTE_111: case MAPSEC_DESERT_RUINS: case MAPSEC_DESERT_UNDERPASS:
        return BLS_DESERT;
    case MAPSEC_METEOR_FALLS: case MAPSEC_METEOR_FALLS2: case MAPSEC_MIRAGE_ISLAND:
        return BLS_METEOR;
    case MAPSEC_MT_PYRE: case MAPSEC_ABANDONED_SHIP:
        return BLS_SPIRIT;
    case MAPSEC_PETALBURG_WOODS: case MAPSEC_ROUTE_119: case MAPSEC_ROUTE_120: case MAPSEC_ROUTE_121:
        return BLS_FOREST;
    case MAPSEC_SHOAL_CAVE:
        return BLS_FROST;
    case MAPSEC_FIERY_PATH: case MAPSEC_FIERY_PATH2: case MAPSEC_MT_CHIMNEY: case MAPSEC_JAGGED_PASS: case MAPSEC_JAGGED_PASS2: case MAPSEC_SCORCHED_SLAB:
        return BLS_MAGMA;
    case MAPSEC_GRANITE_CAVE: case MAPSEC_VICTORY_ROAD: case MAPSEC_SEAFLOOR_CAVERN: case MAPSEC_CAVE_OF_ORIGIN:
        return BLS_STONE;
    }
    return BLS_NONE;
}

static u16 GuardianSpecies(u8 b)
{
    switch (b)
    {
    case BLS_DESERT: return SPECIES_FLYGON;
    case BLS_SEA: return SPECIES_RELICANTH;
    case BLS_METEOR: return (gSaveBlock2Ptr->playTimeSeconds & 1) ? SPECIES_SOLROCK : SPECIES_LUNATONE;
    case BLS_SPIRIT: return (gSaveBlock2Ptr->playTimeSeconds & 1) ? SPECIES_BANETTE : SPECIES_DUSCLOPS;
    case BLS_FOREST: return SPECIES_SHIFTRY;
    case BLS_FROST: return SPECIES_GLALIE;
    case BLS_MAGMA: return SPECIES_CAMERUPT;
    case BLS_STONE: return SPECIES_AGGRON;
    }
    return SPECIES_NONE;
}

extern u8 gRtDebugObjective;
EWRAM_DATA u8 gRtDebugObjective = 0;

void RtExt_SetupObjective(void)
{
    u8 b;
    u16 sp;
    u8 lvl, i, top = 0;
    if (gBattleTypeFlags & (BATTLE_TYPE_TRAINER | BATTLE_TYPE_SAFARI | BATTLE_TYPE_LEGENDARY | BATTLE_TYPE_ROAMER | BATTLE_TYPE_FIRST_BATTLE
                          | BATTLE_TYPE_LINK | BATTLE_TYPE_FRONTIER | BATTLE_TYPE_DOUBLE | BATTLE_TYPE_REGI | BATTLE_TYPE_KYOGRE_GROUDON
                          | BATTLE_TYPE_RAYQUAZA | BATTLE_TYPE_GROUDON | BATTLE_TYPE_KYOGRE | BATTLE_TYPE_WALLY_TUTORIAL))
        return;
    if (gRogueMode == 2)
        return;
    b = BiomeOfMap();
    if (gRogueMode == 1 && gRogueBoss && b == BLS_NONE)
        b = BLS_FOREST;
    if (b == BLS_NONE)
        return;
    if (gRogueMode == 1)
    {
        if (!gRogueBoss)
            return;
    }
    else if (!(gRtDebugEnabled && gRtDebugObjective) && (Random() % 100) >= 14)
        return;
    sp = GuardianSpecies(b);
    for (i = 0; i < PARTY_SIZE; i++)
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES_OR_EGG) != SPECIES_NONE && GetMonData(&gPlayerParty[i], MON_DATA_LEVEL) > top)
            top = GetMonData(&gPlayerParty[i], MON_DATA_LEVEL);
    lvl = top < 14 ? 14 : (top > 95 ? 100 : top + 4);
    if (gRogueMode == 1)
        lvl = top > 97 ? 100 : top + 3;
    if (gRtDebugEnabled && gRtDebugObjective == 2)
        lvl = 5;
    ZeroEnemyPartyMons();
    CreateMon(&gEnemyParty[0], sp, lvl, USE_RANDOM_IVS, FALSE, 0, OT_ID_PLAYER_ID, 0);
    gRt->objective = b;
    gRt->fieldW = 34;
    gRt->fieldH = 16;
}

// the guardian is bigger and starts charged up
void RtExt_GuardianInit(struct RtActor *a)
{
    if (gRogueMode && a->side == RT_SIDE_PLAYER)
    {
        a->maxHp = a->maxHp * RtRogue_HpPct() / 100;
        a->hp = a->hp * RtRogue_HpPct() / 100;
        a->shownHp = a->hp;
    }
    if (!gRt->objective || a->side != RT_SIDE_ENEMY || a->id != 2)
        return;
    a->maxHp = a->maxHp * 5 / 2;
    a->hp = a->maxHp;
    a->shownHp = a->hp;
    X(a)->ki = 650;
    X(a)->spell = SPELL_BARRIER;
    X(a)->spellCd = 900;
}

void RtExt_OnBattleEnd(u8 outcome)
{
    struct RtSave *sv = RtSave_Get();
    if (outcome != B_OUTCOME_WON && outcome != B_OUTCOME_CAUGHT)
        return;
    if (gRt->objective)
    {
        sv->meta[0] = gRt->objective;
        sv->meta[1] = BLESS_BATTLES;
        PlayFanfare(MUS_OBTAIN_ITEM);
        StringCopy(gStringVar1, sBlessName[gRt->objective]);
        StringExpandPlaceholders(gStringVar4, sText_Blessing);
        RtUi_Toast(gStringVar4, 120);
    }
    else if (sv->meta[1])
    {
        if (--sv->meta[1] == 0)
            sv->meta[0] = 0;
    }
}

// blessing modifiers for the player's side
u16 RtExt_BlessStatMul(struct RtActor *a, u8 stat)
{
    u8 b;
    bool8 off = stat == STG_ATK || stat == STG_SPA, def = stat == STG_DEF || stat == STG_SPD;
    if (a->side != RT_SIDE_PLAYER)
        return 100;
    b = RtSave_Get()->meta[0];
    switch (b)
    {
    case BLS_DESERT: return stat == STG_SPE ? 112 : 100;
    case BLS_SEA: return def ? 110 : 100;
    case BLS_MAGMA: return off ? 110 : 100;
    }
    return 100;
}

u16 RtExt_BlessDmgTakenPct(struct RtActor *a)
{
    u8 b;
    if (a->side != RT_SIDE_PLAYER)
        return 100;
    b = RtSave_Get()->meta[0];
    return b == BLS_FROST ? 92 : (b == BLS_STONE ? 90 : 100);
}
