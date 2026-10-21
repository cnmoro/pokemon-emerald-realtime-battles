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
#if RT_LC
        // Liquid Crystal's own map sections: eruptions on Cinnabar, fog in the foggy weather, Ilex Forest, the
        // Lake of Rage and the Tin Tower, wind on the sea (Whirl Islands, Routes 40 / 41 and 19-21) and when surfing
        if (sec == 0x60 /* CINNABAR ISLAND */)
            mod = ARENA_MOD_ERUPTION;
        else if (w == WEATHER_FOG_HORIZONTAL || w == WEATHER_FOG_DIAGONAL || sec == 0x7E /* ILEX FOREST */
         || sec == 0xBB /* LAKE OF RAGE */ || sec == 0xB8 /* TIN TOWER */)
            mod = ARENA_MOD_FOG;
        else if (w == WEATHER_SANDSTORM || gMapHeader.mapType == MAP_TYPE_OCEAN_ROUTE
         || sec == 0x9C /* WHIRL ISLANDS */ || sec == 0xA5 || sec == 0xA8 /* ROUTE 40 */ || sec == 0xB3 /* ROUTE 41 */
         || sec == 0x77 || sec == 0x78 || sec == 0x79 /* ROUTES 19-21 */
         || TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_SURFING))
            mod = ARENA_MOD_WIND;
#else
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
#endif
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
