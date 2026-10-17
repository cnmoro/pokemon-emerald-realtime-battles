// Real-time battle actors: stats, animation, movement, status, rendering.
#include "global.h"
#include "rt_internal.h"
#include "rt_vfx.h"
#include "battle.h"
#include "item.h"
#include "main.h"
#include "palette.h"
#include "pokemon.h"
#include "random.h"
#include "sound.h"
#include "string_util.h"
#include "constants/abilities.h"
#include "constants/battle.h"
#include "constants/hold_effects.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/species.h"

void Rt_AfterFaintAnim(struct RtActor *a);

static const u8 sText_Paralyzed[] = _("{STR_VAR_1} is paralyzed!\nIt can't move!");
static const u8 sText_WokeUp[] = _("{STR_VAR_1} woke up!");
static const u8 sText_Thawed[] = _("{STR_VAR_1} thawed out!");
static const u8 sText_BerryUsed[] = _("{STR_VAR_1} used its {STR_VAR_2}!");
static const u8 sText_PerishFaint[] = _("{STR_VAR_1}'s perish count fell to 0!");
static const u8 sText_Yawn[] = _("{STR_VAR_1} fell asleep!");

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------
s32 RtSin(u8 a)
{
    return gSineTable[a];
}

s32 RtCos(u8 a)
{
    return gSineTable[(u8)(a + 64)];
}

s32 RtDist(s32 dx, s32 dy)
{
    u32 s;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > 2000 || dy > 2000)
        return dx + dy;
    s = dx * dx + dy * dy;
    return Sqrt(s);
}

// 0..255 angle, 0 = +x (right), 64 = +y (down)
u8 RtAtan2(s32 dx, s32 dy)
{
    s32 ax = dx < 0 ? -dx : dx;
    s32 ay = dy < 0 ? -dy : dy;
    s32 a;
    if (ax == 0 && ay == 0)
        return 0;
    // first octant approximation: atan(y/x) ~ 32 * y/x for y<=x (in 256-units: 45deg = 32)
    if (ax >= ay)
        a = (ay * 32 + ax / 2) / ax;
    else
        a = 64 - (ax * 32 + ay / 2) / ay;
    // refine a bit: atan curve correction (max error ~ 1.5 units)
    if (dx >= 0 && dy >= 0)
        return a;
    if (dx < 0 && dy >= 0)
        return 128 - a;
    if (dx < 0 && dy < 0)
        return 128 + a;
    return (u8)(256 - a);
}

u8 RtAngleToDir(u8 angle)
{
    u8 seg = ((angle + 16) >> 5) & 7;
    return (2 - seg) & 7;
}

u8 RtDirToAngle(u8 dir)
{
    return ((2 - dir) & 7) * 32;
}

void RtActor_SetFacingFromAngle(struct RtActor *a, u8 angle)
{
    a->facing = RtAngleToDir(angle);
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
static const u8 sStageRatios[13][2] =
{
    {10, 40}, {10, 35}, {10, 30}, {10, 25}, {10, 20}, {10, 15}, {10, 10},
    {15, 10}, {20, 10}, {25, 10}, {30, 10}, {35, 10}, {40, 10}
};

u16 RtActor_GetStat(struct RtActor *a, u8 stat)
{
    u32 v;
    s8 st = a->stages[stat];
    switch (stat)
    {
    case STG_ATK: v = a->atk; break;
    case STG_DEF: v = a->def; break;
    case STG_SPE: v = a->spe; break;
    case STG_SPA: v = a->spa; break;
    case STG_SPD: v = a->spd; break;
    default: return 100;
    }
    if (st < -6) st = -6;
    if (st > 6) st = 6;
    v = v * sStageRatios[st + 6][0] / sStageRatios[st + 6][1];
    return v ? v : 1;
}

// movement speed in Q8 px/frame
u8 RtActor_SpeedPx(struct RtActor *a)
{
    u32 s = RtActor_GetStat(a, STG_SPE);
    u32 ratio; // Q8
    s32 v;
    if (gRt->weather == WEATHER_RAIN_RT && a->ability == ABILITY_SWIFT_SWIM)
        s *= 2;
    if (gRt->weather == WEATHER_SUN_RT && a->ability == ABILITY_CHLOROPHYLL)
        s *= 2;
    if (a->holdEffect == HOLD_EFFECT_MACHO_BRACE)
        s /= 2;
    ratio = s * 256 / (a->level * 2 + 10);
    v = 192 + ratio * 140 / 256;
    if (a->mstatus == MST_PAR)
        v = v * 55 / 100;
    if (a->slowTimer)
        v = v * 60 / 100;
    if (a->trap)
        v = v * 35 / 100;
    if (v < 160) v = 160;
    if (v > 255) v = 255;
    if (a->mstatus == MST_PAR && v > 150)
        v = 150;
    return v;
}

bool8 RtActor_IsAlive(struct RtActor *a)
{
    return a->hp > 0 && a->state != AST_FAINT;
}

bool8 RtActor_HasType(struct RtActor *a, u8 type)
{
    return a->types[0] == type || a->types[1] == type;
}

bool8 RtActor_CanAct(struct RtActor *a)
{
    return a->state == AST_NORMAL && a->gcd == 0 && !a->recharge && !a->flinch;
}

u8 RtActor_MoveSlotUsable(struct RtActor *a, u8 slot)
{
    u16 move = a->moves[slot];
    if (move == MOVE_NONE)
        return 0;
    if (a->cd[slot])
        return 0;
    if (a->disable && a->disabledMove == move)
        return 0;
    if (a->taunt && gBattleMoves[move].power == 0)
        return 0;
    if (a->torment && a->lastMove == move)
        return 0;
    if (a->encore && slot != a->encoreSlot)
        return 0;
    if (a->choiceMove && a->choiceMove != move)
        return 0;
    if (move == MOVE_SNORE || move == MOVE_SLEEP_TALK)
        return a->mstatus == MST_SLP ? 1 : 0;
    {
        // imprison
        s32 i, j;
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *o = &gRt->actors[i];
            if (!o->active || o->side == a->side || !o->imprison || o->state == AST_GONE)
                continue;
            for (j = 0; j < MAX_MON_MOVES; j++)
                if (o->moves[j] == move)
                    return 0;
        }
    }
    return 1;
}

static u8 StatusFromMon(u32 status, u16 *timer, u8 *toxicN)
{
    if (status & STATUS1_SLEEP)
    {
        *timer = (status & STATUS1_SLEEP) * 75;
        return MST_SLP;
    }
    if (status & STATUS1_TOXIC_POISON)
    {
        *toxicN = 1;
        return MST_TOX;
    }
    if (status & STATUS1_POISON) return MST_PSN;
    if (status & STATUS1_BURN) return MST_BRN;
    if (status & STATUS1_FREEZE)
    {
        *timer = 180 + Random() % 120;
        return MST_FRZ;
    }
    if (status & STATUS1_PARALYSIS) return MST_PAR;
    return MST_NONE;
}

void RtActor_InitFromMon(struct RtActor *a, struct Pokemon *mon, u8 side, u8 partyIdx, u8 slot)
{
    s32 i;
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    u8 ppBonuses = GetMonData(mon, MON_DATA_PP_BONUSES);
    u8 participated = (side == RT_SIDE_ENEMY && a->active && a->partyIdx == partyIdx) ? a->participated : 0;

    memset(a, 0, sizeof(*a));
    a->active = TRUE;
    a->side = side;
    a->partyIdx = partyIdx;
    a->id = slot;
    a->mon = mon;
    a->species = species;
    a->dex = SpeciesToNationalPokedexNum(species);
    if (a->dex == 0 || a->dex > NATIONAL_DEX_COUNT)
        a->dex = 1;
    a->gfxDex = a->dex;
    a->shiny = IsMonShiny(mon);
    a->vramSlot = slot;
    a->palSlot = slot;
    a->level = GetMonData(mon, MON_DATA_LEVEL);
    a->types[0] = gSpeciesInfo[species].types[0];
    a->types[1] = gSpeciesInfo[species].types[1];
    a->ability = GetMonAbility(mon);
    a->heldItem = GetMonData(mon, MON_DATA_HELD_ITEM);
    a->holdEffect = GetItemHoldEffect(a->heldItem);
    a->holdParam = GetItemHoldEffectParam(a->heldItem);
    a->hp = GetMonData(mon, MON_DATA_HP);
    a->maxHp = GetMonData(mon, MON_DATA_MAX_HP);
    a->shownHp = a->hp;
    a->atk = GetMonData(mon, MON_DATA_ATK);
    a->def = GetMonData(mon, MON_DATA_DEF);
    a->spe = GetMonData(mon, MON_DATA_SPEED);
    a->spa = GetMonData(mon, MON_DATA_SPATK);
    a->spd = GetMonData(mon, MON_DATA_SPDEF);
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        a->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i);
        a->cdMax[i] = RtMove_Cooldown(a->moves[i], ppBonuses, i);
        a->cd[i] = 0;
    }
    a->mstatus = StatusFromMon(GetMonData(mon, MON_DATA_STATUS), &a->mstatusTimer, &a->toxicN);
    a->radius = gRtSpeciesGfx[a->dex].radius;
    a->height = gRtSpeciesGfx[a->dex].height;
    a->state = AST_GONE;
    a->hidden = TRUE;
    a->curFrame = 0xFFFF;
    a->inSlot = -1;
    a->aiTarget = 0xFF;
    a->lastHitBy = 0xFF;
    a->trapBy = 0xFF;
    a->participated = participated;
    a->hpBarDirty = TRUE;
    a->palDirty = TRUE;
    RtActor_SetAnim(a, RTA_IDLE, TRUE);
    gRt->hudDirty = TRUE;
}

void RtActor_SyncToMon(struct RtActor *a)
{
    u32 st = 0;
    u16 hp = a->hp;
    if (!a->mon)
        return;
    SetMonData(a->mon, MON_DATA_HP, &hp);
    switch (a->mstatus)
    {
    case MST_SLP:
        st = (a->mstatusTimer / 75) + 1;
        if (st > 7) st = 7;
        break;
    case MST_PSN: st = STATUS1_POISON; break;
    case MST_TOX: st = STATUS1_POISON; break; // toxic reverts to poison outside battle
    case MST_BRN: st = STATUS1_BURN; break;
    case MST_FRZ: st = STATUS1_FREEZE; break;
    case MST_PAR: st = STATUS1_PARALYSIS; break;
    }
    if (a->hp == 0)
        st = 0;
    SetMonData(a->mon, MON_DATA_STATUS, &st);
    if (a->side == RT_SIDE_PLAYER)
    {
        u16 item = a->heldItem;
        if (!(gRt->isTrainer && (gBattleTypeFlags & BATTLE_TYPE_FRONTIER)))
            SetMonData(a->mon, MON_DATA_HELD_ITEM, &item);
    }
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------
void RtActor_SetAnim(struct RtActor *a, u8 anim, bool8 loop)
{
    if (a->anim == anim && a->animLoop == loop && loop)
        return;
    a->anim = anim;
    a->animFrame = 0;
    a->animTimer = 0;
    a->animLoop = loop;
}

static u16 FrameIndex(struct RtActor *a, bool8 *hflip)
{
    const struct RtSpeciesGfx *g = &gRtSpeciesGfx[a->gfxDex];
    u8 dir = a->facing & 7, row;
    u8 cnt = g->count[a->anim];
    *hflip = FALSE;
    if (dir <= 4)
        row = dir;
    else
    {
        row = 8 - dir; // 5->3, 6->2, 7->1
        *hflip = TRUE;
    }
    if (a->anim == RTA_SLEEP)
    {
        row = 0;
        *hflip = FALSE;
    }
    if (cnt == 0)
        cnt = 1;
    return g->start[a->anim] + row * cnt + (a->animFrame < cnt ? a->animFrame : cnt - 1);
}

void RtActor_UpdateAnim(struct RtActor *a)
{
    const struct RtSpeciesGfx *g = &gRtSpeciesGfx[a->gfxDex];
    u8 cnt = g->count[a->anim];
    bool8 hf;
    u16 idx = FrameIndex(a, &hf);
    u8 dur = gRtFrames[idx].dur;
    if (dur == 0)
        dur = 4;
    if (a->state == AST_STUN && a->mstatus == MST_FRZ)
        return; // frozen solid
    a->animTimer++;
    if (a->animTimer >= dur)
    {
        a->animTimer = 0;
        if (a->animFrame + 1 < cnt)
            a->animFrame++;
        else if (a->animLoop)
            a->animFrame = 0;
    }
}

// frames from the start of an animation until its "hit" frame (PMD AnimData HitFrame)
u8 RtActor_AnimHitDelay(struct RtActor *a, u8 anim)
{
    const struct RtSpeciesGfx *g = &gRtSpeciesGfx[a->gfxDex];
    u8 dir = a->facing & 7, row, k;
    u16 total = 0;
    u8 cnt = g->count[anim] ? g->count[anim] : 1;
    row = dir <= 4 ? dir : 8 - dir;
    if (anim == RTA_SLEEP)
        row = 0;
    for (k = 0; k < g->hit[anim] && k < cnt; k++)
        total += gRtFrames[g->start[anim] + row * cnt + k].dur;
    if (total > 24)
        total = 24;
    return total;
}

bool8 RtActor_AnimDone(struct RtActor *a)
{
    const struct RtSpeciesGfx *g = &gRtSpeciesGfx[a->gfxDex];
    return !a->animLoop && a->animFrame + 1 >= g->count[a->anim];
}

bool8 RtUi_TrainerPicShown(void);

void RtActor_LoadPalette(struct RtActor *a)
{
    LoadPalette(gRtSpeciesPal[a->gfxDex][a->shiny ? 1 : 0], OBJ_PLTT_ID(a->palSlot), PLTT_SIZE_4BPP);
}

static const u16 sWhitePal[16] = {
    0, RGB_WHITE, RGB_WHITE, RGB_WHITE, RGB_WHITE, RGB_WHITE, RGB_WHITE, RGB_WHITE,
    RGB_WHITE, RGB_WHITE, RGB_WHITE, RGB_WHITE, RGB_WHITE, RGB_WHITE, RGB_WHITE, RGB_WHITE
};

static void UpdatePaletteFx(struct RtActor *a)
{
    // never touch palettes while the screen fades (it would pop the sprite back to full colour)
    if (gPaletteFade.active || gRt->state == RTS_EXIT || (gRt->state == RTS_OUTRO && gRt->subState == 6))
        return;
    if (a->flash)
    {
        a->flash--;
        if (a->flash & 2)
            LoadPalette(sWhitePal, OBJ_PLTT_ID(a->palSlot), PLTT_SIZE_4BPP);
        else
            RtActor_LoadPalette(a);
        if (a->flash == 0)
            a->palDirty = TRUE;
        return;
    }
    if (a->palDirty)
    {
        RtActor_LoadPalette(a);
        a->palDirty = FALSE;
    }
    // status tint pulse
    if ((gRt->frame & 3) == 0)
    {
        u16 color = 0;
        u8 amt = 0;
        u8 ph = (gRt->frame >> 2) & 15;
        u8 wave = ph < 8 ? ph : 15 - ph;
        switch (a->mstatus)
        {
        case MST_PSN:
        case MST_TOX: color = RGB(20, 4, 24); amt = wave; break;
        case MST_BRN: color = RGB(31, 8, 4); amt = wave; break;
        case MST_PAR: color = RGB(31, 28, 4); amt = wave; break;
        case MST_FRZ: color = RGB(16, 26, 31); amt = 9; break;
        }
        if (a->substituteHp && !a->mstatus)
        {
            color = RGB(31, 31, 31);
            amt = 3;
        }
        if (amt)
        {
            RtActor_LoadPalette(a);
            BlendPalette(OBJ_PLTT_ID(a->palSlot), 16, amt, color);
        }
        else if (a->mstatus == MST_NONE)
        {
            // nothing
        }
    }
}

void RtActor_LoadGfxIfNeeded(struct RtActor *a)
{
    bool8 hf;
    u16 idx;
    const struct RtFrame *f;
    static const u8 sTiles[3][4] = {{1, 4, 16, 64}, {2, 4, 8, 32}, {2, 4, 8, 32}};
    u8 shape, size;

    // the trainer picture of the outro borrows the tiles and palette of the first enemy slot: leave them alone
    if (RtUi_TrainerPicShown() && (a->palSlot == 2 || a->vramSlot == 2))
        return;
    UpdatePaletteFx(a);
    if (a->hidden && a->state == AST_GONE)
        return;
    idx = FrameIndex(a, &hf);
    if (idx == a->curFrame)
        return;
    if (gRt->gfxPending[a->vramSlot])
        return; // wait for vblank to consume previous frame
    {
        // spread decompression over frames: at most two sprite frames per game frame
        static EWRAM_DATA u32 sBudgetFrame = 0;
        static EWRAM_DATA u8 sBudgetUsed = 0;
        if (sBudgetFrame != gRt->frame)
        {
            sBudgetFrame = gRt->frame;
            sBudgetUsed = 0;
        }
        if (sBudgetUsed >= 2 || (sBudgetUsed == 1 && RtFrameLinesUsed() > 110))
            return;
        sBudgetUsed++;
    }
    f = &gRtFrames[idx];
    LZ77UnCompWram((const u32 *)((const u8 *)gRtSpriteGfx + f->offset), gRt->gfxBuf[a->vramSlot]);
    shape = f->shapeSize >> 2;
    size = f->shapeSize & 3;
    gRt->gfxPendingSize[a->vramSlot] = sTiles[shape][size] * TILE_SIZE_4BPP;
    gRt->gfxPending[a->vramSlot] = TRUE;
    a->curFrame = idx;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
static void NameToVar1(struct RtActor *a)
{
    GetMonData(a->mon, MON_DATA_NICKNAME, gStringVar1);
    StringGet_Nickname(gStringVar1);
}

void RtActor_ApplyStatus(struct RtActor *a, u8 status, u8 src)
{
    struct RtActor *s = (src < RT_MAX_ACTORS) ? &gRt->actors[src] : NULL;
    if (!RtActor_IsAlive(a) || a->state == AST_GONE)
        return;
    if (a->mstatus != MST_NONE)
        return;
    if (a->substituteHp && s && s != a)
        return;
    if (gRt->sides[a->side].safeguard && s && s->side != a->side)
        return;
    switch (status)
    {
    case MST_SLP:
        if (a->ability == ABILITY_INSOMNIA || a->ability == ABILITY_VITAL_SPIRIT)
            return;
        if (a->uproar)
            return;
        a->mstatusTimer = (2 + Random() % 4) * 75;
        if (a->ability == ABILITY_EARLY_BIRD)
            a->mstatusTimer /= 2;
        RtFx_Spawn(FX_Z8, FXP_SLEEP, a->x, a->y - TO_FX(a->height), 40);
        break;
    case MST_PSN:
    case MST_TOX:
        if (RtActor_HasType(a, TYPE_POISON) || RtActor_HasType(a, TYPE_STEEL) || a->ability == ABILITY_IMMUNITY)
            return;
        a->toxicN = 1;
        a->dotTimer = 0;
        RtFx_Burst(FX_ORB8, FXP_POISON, a->x, a->y - TO_FX(a->height / 2), 6, 2, 24);
        break;
    case MST_BRN:
        if (RtActor_HasType(a, TYPE_FIRE) || a->ability == ABILITY_WATER_VEIL)
            return;
        a->dotTimer = 0;
        RtFx_Burst(FX_FLAME16, FXP_FIRE, a->x, a->y - TO_FX(a->height / 2), 5, 2, 24);
        break;
    case MST_FRZ:
        if (RtActor_HasType(a, TYPE_ICE) || a->ability == ABILITY_MAGMA_ARMOR || gRt->weather == WEATHER_SUN_RT)
            return;
        a->mstatusTimer = 160 + Random() % 140;
        RtFx_Spawn(FX_ICEBLOCK32, FXP_ICE, a->x, a->y - TO_FX(12), 20);
        PlaySE(SE_M_ICY_WIND);
        break;
    case MST_PAR:
        if (a->ability == ABILITY_LIMBER)
            return;
        RtFx_Burst(FX_BOLT16, FXP_ELECTRIC, a->x, a->y - TO_FX(a->height / 2), 4, 2, 20);
        break;
    default:
        return;
    }
    a->mstatus = status;
    a->palDirty = TRUE;
    gRt->hudDirty = TRUE;
    // synchronize
    if (a->ability == ABILITY_SYNCHRONIZE && s && s != a && (status == MST_PSN || status == MST_TOX || status == MST_BRN || status == MST_PAR))
        RtActor_ApplyStatus(s, status == MST_TOX ? MST_PSN : status, a->id);
    if (status == MST_SLP || status == MST_FRZ)
    {
        a->state = AST_STUN;
        a->stateTimer = a->mstatusTimer;
        RtActor_SetAnim(a, status == MST_SLP ? RTA_SLEEP : RTA_HURT, TRUE);
        a->act.move = MOVE_NONE;
    }
}

void RtActor_CureStatus(struct RtActor *a)
{
    if (a->mstatus == MST_SLP || a->mstatus == MST_FRZ)
    {
        if (a->state == AST_STUN)
        {
            a->state = AST_NORMAL;
            a->stateTimer = 0;
            RtActor_SetAnim(a, RTA_IDLE, TRUE);
        }
    }
    a->mstatus = MST_NONE;
    a->mstatusTimer = 0;
    a->toxicN = 0;
    a->palDirty = TRUE;
    gRt->hudDirty = TRUE;
}

void RtActor_Heal(struct RtActor *a, s32 amount)
{
    if (!RtActor_IsAlive(a) || amount <= 0)
        return;
    if (a->hp + amount > a->maxHp)
        amount = a->maxHp - a->hp;
    if (amount <= 0)
        return;
    a->hp += amount;
    a->hpBarDirty = TRUE;
    gRt->hudDirty = TRUE;
    RtFx_Number(a->x, a->y - TO_FX(a->height + 4), amount, 2);
    RtFx_Burst(FX_PLUS8, FXP_HEAL, a->x, a->y - TO_FX(a->height / 2), 4, 1, 30);
}

void RtActor_Faint(struct RtActor *a)
{
    if (a->state == AST_FAINT || a->state == AST_GONE)
        return;
    a->hp = 0;
    a->state = AST_FAINT;
    a->stateTimer = 50;
    a->act.move = MOVE_NONE;
    a->hidden = FALSE;
    a->invuln = 0;
    a->z = 0;
    RtActor_SetAnim(a, RTA_HURT, FALSE);
    PlayCry_ByMode(a->species, a->side == RT_SIDE_PLAYER ? -25 : 25, CRY_MODE_FAINT);
    a->hpBarDirty = TRUE;
    gRt->hudDirty = TRUE;
    RtActor_SyncToMon(a);
    // destiny bond / grudge handled by move module through lastHitBy
    {
        extern void RtMove_OnFaint(struct RtActor *a);
        RtMove_OnFaint(a);
    }
    Rt_OnActorFainted(a);
}

// Generic damage (no type logic). Used for DoT and recoil etc.
void RtActor_Damage(struct RtActor *a, s32 amount, u8 src, bool8 direct)
{
    if (!RtActor_IsAlive(a) || a->state == AST_GONE)
        return;
    if (amount <= 0)
        amount = 1;
    if (amount >= a->hp)
    {
        a->hp = 0;
        a->hpBarDirty = TRUE;
        RtActor_Faint(a);
        return;
    }
    a->hp -= amount;
    a->hpBarDirty = TRUE;
    gRt->hudDirty = TRUE;
    if (!direct)
        RtFx_Number(a->x, a->y - TO_FX(a->height + 4), amount, 3);
    // berries
    {
        extern void RtMove_CheckBerries(struct RtActor *a);
        RtMove_CheckBerries(a);
    }
}

bool8 RtActor_ChangeStage(struct RtActor *a, u8 stage, s8 delta, u8 src, bool8 showFx)
{
    s8 old = a->stages[stage];
    struct RtActor *s = (src < RT_MAX_ACTORS) ? &gRt->actors[src] : NULL;
    if (delta < 0 && s && s->side != a->side)
    {
        if (gRt->sides[a->side].mist)
            return FALSE;
        if (a->ability == ABILITY_CLEAR_BODY || a->ability == ABILITY_WHITE_SMOKE)
            return FALSE;
        if (a->ability == ABILITY_HYPER_CUTTER && stage == STG_ATK)
            return FALSE;
        if (a->ability == ABILITY_KEEN_EYE && stage == STG_ACC)
            return FALSE;
        if (a->substituteHp)
            return FALSE;
    }
    a->stages[stage] += delta;
    if (a->stages[stage] > 6) a->stages[stage] = 6;
    if (a->stages[stage] < -6) a->stages[stage] = -6;
    if (old == a->stages[stage])
    {
        if (showFx)
            RtFx_Word(a->x, a->y - TO_FX(a->height + 6), FX_W_MAX);
        return FALSE;
    }
    if (showFx)
    {
        RtFx_StatArrows(a, delta > 0);
        PlaySE(delta > 0 ? SE_M_STAT_INCREASE : SE_M_STAT_DECREASE);
    }
    if (stage == STG_ACC && delta < 0)
        a->blindTimer = 300;
    return TRUE;
}

static void BerryUsed(struct RtActor *a)
{
    NameToVar1(a);
    CopyItemName(a->heldItem, gStringVar2);
    {
        static const u8 tmpl[] = _("{STR_VAR_1} used its {STR_VAR_2}!");
        RtUi_ToastFmt(tmpl, 70);
    }
    a->consumedItem = a->heldItem;
    a->heldItem = ITEM_NONE;
    a->holdEffect = 0;
    a->holdParam = 0;
}

void RtMove_CheckBerries(struct RtActor *a)
{
    if (!RtActor_IsAlive(a) || a->heldItem == ITEM_NONE)
        return;
    switch (a->holdEffect)
    {
    case HOLD_EFFECT_RESTORE_HP:
        if (a->hp <= a->maxHp / 2)
        {
            u16 amt = a->holdParam;
            RtActor_Heal(a, amt);
            BerryUsed(a);
        }
        break;
    case HOLD_EFFECT_CONFUSE_SPICY:
    case HOLD_EFFECT_CONFUSE_DRY:
    case HOLD_EFFECT_CONFUSE_SWEET:
    case HOLD_EFFECT_CONFUSE_BITTER:
    case HOLD_EFFECT_CONFUSE_SOUR:
        if (a->hp <= a->maxHp / 2)
        {
            RtActor_Heal(a, a->maxHp / a->holdParam);
            BerryUsed(a);
        }
        break;
    case HOLD_EFFECT_ATTACK_UP:
    case HOLD_EFFECT_DEFENSE_UP:
    case HOLD_EFFECT_SPEED_UP:
    case HOLD_EFFECT_SP_ATTACK_UP:
    case HOLD_EFFECT_SP_DEFENSE_UP:
        if (a->hp <= a->maxHp / a->holdParam)
        {
            static const u8 map[] = {STG_ATK, STG_DEF, STG_SPE, STG_SPA, STG_SPD};
            RtActor_ChangeStage(a, map[a->holdEffect - HOLD_EFFECT_ATTACK_UP], 1, a->id, TRUE);
            BerryUsed(a);
        }
        break;
    case HOLD_EFFECT_CRITICAL_UP:
        if (a->hp <= a->maxHp / a->holdParam)
        {
            a->focusEnergy = 1;
            BerryUsed(a);
        }
        break;
    case HOLD_EFFECT_RANDOM_STAT_UP:
        if (a->hp <= a->maxHp / a->holdParam)
        {
            RtActor_ChangeStage(a, Random() % 5, 2, a->id, TRUE);
            BerryUsed(a);
        }
        break;
    case HOLD_EFFECT_CURE_PAR:
        if (a->mstatus == MST_PAR) { RtActor_CureStatus(a); BerryUsed(a); }
        break;
    case HOLD_EFFECT_CURE_SLP:
        if (a->mstatus == MST_SLP) { RtActor_CureStatus(a); BerryUsed(a); }
        break;
    case HOLD_EFFECT_CURE_PSN:
        if (a->mstatus == MST_PSN || a->mstatus == MST_TOX) { RtActor_CureStatus(a); BerryUsed(a); }
        break;
    case HOLD_EFFECT_CURE_BRN:
        if (a->mstatus == MST_BRN) { RtActor_CureStatus(a); BerryUsed(a); }
        break;
    case HOLD_EFFECT_CURE_FRZ:
        if (a->mstatus == MST_FRZ) { RtActor_CureStatus(a); BerryUsed(a); }
        break;
    case HOLD_EFFECT_CURE_CONFUSION:
        if (a->confuse) { a->confuse = 0; BerryUsed(a); }
        break;
    case HOLD_EFFECT_CURE_STATUS:
        if (a->mstatus || a->confuse) { RtActor_CureStatus(a); a->confuse = 0; BerryUsed(a); }
        break;
    case HOLD_EFFECT_CURE_ATTRACT:
        if (a->attract) { a->attract = 0; BerryUsed(a); }
        break;
    case HOLD_EFFECT_RESTORE_STATS:
        {
            s32 i;
            bool8 any = FALSE;
            for (i = 0; i < STG_COUNT; i++)
                if (a->stages[i] < 0) { a->stages[i] = 0; any = TRUE; }
            if (any)
                BerryUsed(a);
        }
        break;
    }
}

#define T(x) if (a->x) a->x--

void RtActor_TickStatus(struct RtActor *a)
{
    struct RtActor *src;
    if (!RtActor_IsAlive(a) || a->state == AST_GONE || a->state == AST_ENTER)
        return;

    T(gcd); T(dashCd); T(iframes); T(flinch); T(taunt); T(encore); T(torment); T(protect);
    T(magicCoat); T(snatch); T(lockOn); T(helpingHand); T(counterWin); T(destinyBond); T(grudge);
    T(rage); T(minimize); T(foresight); T(slowTimer); T(blindTimer); T(confuseSelfHitCd);
    T(meanLook); T(aiDodgeCd); T(aiThinkCd); T(aiItemCd); T(truantTimer); T(charge);
    if (a->protect == 0)
        a->protectKind = 0;
    if (a->disable)
    {
        a->disable--;
        if (!a->disable) a->disabledMove = 0;
    }
    if (a->encore == 0)
        a->encoreSlot = 0;
    {
        s32 i;
        for (i = 0; i < MAX_MON_MOVES; i++)
            if (a->cd[i]) a->cd[i]--;
    }
    if (a->confuse)
    {
        a->confuse--;
        if ((gRt->frame & 15) == 0)
            RtFx_Spawn(FX_DIZZY8, FXP_GOLD, a->x + TO_FX((Random() % 16) - 8), a->y - TO_FX(a->height + 2), 16);
    }
    if (a->attract && (gRt->frame & 31) == 0)
        RtFx_Spawn(FX_HEART16, FXP_HEART, a->x, a->y - TO_FX(a->height + 4), 24);
    if (a->attract)
        a->attract--;

    RtVfx_StatusTick(a);
    // major status
    switch (a->mstatus)
    {
    case MST_SLP:
        if (a->mstatusTimer)
            a->mstatusTimer--;
        if ((gRt->frame & 31) == 0)
            RtFx_Spawn(FX_Z8, FXP_SLEEP, a->x + TO_FX(6), a->y - TO_FX(a->height), 40);
        if (a->nightmare && ++a->dotTimer >= 240)
        {
            a->dotTimer = 0;
            RtActor_Damage(a, a->maxHp / 4, 0xFF, FALSE);
        }
        if (a->mstatusTimer == 0 && !a->uproar)
        {
            NameToVar1(a);
            RtUi_ToastFmt(sText_WokeUp, 60);
            a->nightmare = 0;
            RtActor_CureStatus(a);
        }
        break;
    case MST_FRZ:
        if (a->mstatusTimer)
            a->mstatusTimer--;
        if (a->mstatusTimer == 0)
        {
            NameToVar1(a);
            RtUi_ToastFmt(sText_Thawed, 60);
            RtActor_CureStatus(a);
        }
        break;
    case MST_PSN:
    case MST_BRN:
        if (++a->dotTimer >= 240)
        {
            a->dotTimer = 0;
            RtActor_Damage(a, a->maxHp / 8, 0xFF, FALSE);
            RtFx_Burst(a->mstatus == MST_BRN ? FX_FLAME16 : FX_ORB8, a->mstatus == MST_BRN ? FXP_FIRE : FXP_POISON, a->x, a->y - TO_FX(a->height / 2), 3, 1, 20);
        }
        break;
    case MST_TOX:
        if (++a->dotTimer >= 200)
        {
            a->dotTimer = 0;
            RtActor_Damage(a, a->maxHp * a->toxicN / 16, 0xFF, FALSE);
            if (a->toxicN < 15)
                a->toxicN++;
            RtFx_Burst(FX_ORB8, FXP_POISON, a->x, a->y - TO_FX(a->height / 2), 4, 1, 20);
        }
        break;
    }
    if (!RtActor_IsAlive(a))
        return;

    // shed skin
    if (a->ability == ABILITY_SHED_SKIN && a->mstatus && ++a->shedSkinTimer >= 240)
    {
        a->shedSkinTimer = 0;
        if (Random() % 3 == 0)
            RtActor_CureStatus(a);
    }
    // speed boost
    if (a->ability == ABILITY_SPEED_BOOST && ++a->speedBoostTimer >= 480)
    {
        a->speedBoostTimer = 0;
        RtActor_ChangeStage(a, STG_SPE, 1, a->id, TRUE);
    }
    // leftovers / ingrain / rain dish
    if (++a->regenTimer >= 240)
    {
        a->regenTimer = 0;
        if (a->holdEffect == HOLD_EFFECT_LEFTOVERS && a->hp < a->maxHp)
            RtActor_Heal(a, a->maxHp / 16 ? a->maxHp / 16 : 1);
        if (a->ingrain && a->hp < a->maxHp)
            RtActor_Heal(a, a->maxHp / 16 ? a->maxHp / 16 : 1);
        if (a->ability == ABILITY_RAIN_DISH && gRt->weather == WEATHER_RAIN_RT && a->hp < a->maxHp)
            RtActor_Heal(a, a->maxHp / 16 ? a->maxHp / 16 : 1);
    }
    // leech seed
    if (a->leechSeed)
    {
        a->leechSeed--;
        if ((a->leechSeed % 150) == 0)
        {
            s32 amt = a->maxHp / 8 ? a->maxHp / 8 : 1;
            src = &gRt->actors[a->seededBy];
            RtFx_Burst(FX_SEED8, FXP_GRASS, a->x, a->y - TO_FX(a->height / 2), 4, 2, 20);
            RtActor_Damage(a, amt, a->seededBy, FALSE);
            if (src->active && RtActor_IsAlive(src) && src->state != AST_GONE)
            {
                if (a->ability == ABILITY_LIQUID_OOZE)
                    RtActor_Damage(src, amt, a->id, FALSE);
                else
                    RtActor_Heal(src, amt);
            }
        }
    }
    // trapping moves
    if (a->trap)
    {
        a->trap--;
        if ((a->trap % 120) == 0)
            RtActor_Damage(a, a->maxHp / 16 ? a->maxHp / 16 : 1, a->trapBy, FALSE);
    }
    // curse
    if (a->cursed && ++a->dotTimer >= 240 && a->mstatus != MST_PSN && a->mstatus != MST_BRN && a->mstatus != MST_TOX && a->mstatus != MST_SLP)
    {
        a->dotTimer = 0;
        RtFx_Spawn(FX_SKULL16, FXP_SHADOW, a->x, a->y - TO_FX(a->height + 4), 30);
        RtActor_Damage(a, a->maxHp / 4, 0xFF, FALSE);
    }
    // yawn
    if (a->yawn)
    {
        a->yawn--;
        if (a->yawn == 0)
        {
            NameToVar1(a);
            RtUi_ToastFmt(sText_Yawn, 60);
            RtActor_ApplyStatus(a, MST_SLP, 0xFF);
        }
    }
    // perish song
    if (a->perish)
    {
        a->perish--;
        if ((a->perish % 120) == 0 && a->perish)
            RtFx_Number(a->x, a->y - TO_FX(a->height + 10), a->perish / 120, 4);
        if (a->perish == 0)
        {
            NameToVar1(a);
            RtUi_ToastFmt(sText_PerishFaint, 70);
            RtActor_Damage(a, a->hp, 0xFF, TRUE);
        }
    }
    if (a->uproar)
        a->uproar--;
    if (a->rampage)
    {
        a->rampage--;
        if (a->rampage == 0)
        {
            a->rampageMove = 0;
            if (a->ability != ABILITY_OWN_TEMPO)
                a->confuse = 180 + Random() % 120;
        }
    }
    if (a->bide)
    {
        a->bide--;
    }
}

// ---------------------------------------------------------------------------
// Send out / withdraw
// ---------------------------------------------------------------------------
void RtActor_SendOut(struct RtActor *a, u8 partyIdx, bool8 instant)
{
    a->state = instant ? AST_NORMAL : AST_ENTER;
    a->stateTimer = instant ? 0 : 28;
    a->hidden = !instant;
    a->curFrame = 0xFFFF;
    a->palDirty = TRUE;
    a->iframes = 30;
    RtActor_SetAnim(a, RTA_IDLE, TRUE);
    if (!instant)
    {
        RtFx_Spawn(FX_BALL16, FXP_UI, a->x, a->y, 12);
        PlaySE(SE_BALL_OPEN);
    }
    a->hpBarDirty = TRUE;
    gRt->hudDirty = TRUE;
    // participation tracking
    {
        s32 i;
        for (i = 0; i < RT_MAX_ACTORS; i++)
        {
            struct RtActor *b = &gRt->actors[i];
            if (!b->active || b->side == a->side || b->state == AST_GONE)
                continue;
            if (a->side == RT_SIDE_PLAYER)
            {
                b->participated |= 1 << a->partyIdx;
                gRt->fought |= 1 << a->partyIdx;
            }
            else
            {
                a->participated |= 1 << b->partyIdx;
                gRt->fought |= 1 << b->partyIdx;
            }
        }
    }
    // spikes damage
    if (a->side < 2 && gRt->sides[a->side].spikes && !RtActor_HasType(a, TYPE_FLYING) && a->ability != ABILITY_LEVITATE)
    {
        u8 l = gRt->sides[a->side].spikes;
        s32 dmg = a->maxHp / (l == 1 ? 8 : (l == 2 ? 6 : 4));
        RtActor_Damage(a, dmg ? dmg : 1, 0xFF, FALSE);
    }
}

void RtActor_Withdraw(struct RtActor *a)
{
    RtActor_SyncToMon(a);
    RtFx_Burst(FX_GLOW16, FXP_BLOOD, a->x, a->y - TO_FX(a->height / 2), 5, 2, 18);
    PlaySE(SE_BALL_THROW);
    a->state = AST_GONE;
    a->hidden = TRUE;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------
static void SeparateActors(struct RtActor *a)
{
    s32 i;
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *b = &gRt->actors[i];
        s32 dx, dy, d, min;
        if (b == a || !b->active || b->state == AST_GONE || b->state == AST_FAINT || b->invuln || a->invuln)
            continue;
        dx = FROM_FX(a->x - b->x);
        dy = FROM_FX(a->y - b->y) * 2;
        min = (a->radius + b->radius) * 3 / 4;
        d = RtDist(dx, dy);
        if (d < min)
        {
            s32 push = (min - d) * 128; // half overlap in Q8
            if (d == 0)
            {
                dx = (a->id < b->id) ? -1 : 1;
                dy = 0;
                d = 1;
            }
            a->x += dx * push / d;
            a->y += (dy / 2) * push / d;
        }
    }
}

static void ApplyMovement(struct RtActor *a, s16 mx, s16 my, u16 speedQ8)
{
    if (mx || my)
    {
        a->x += mx * speedQ8 / 256;
        a->y += my * speedQ8 / 256;
        if (a->state == AST_NORMAL)
        {
            RtActor_SetFacingFromAngle(a, RtAtan2(mx, my));
            if (a->anim != RTA_WALK)
                RtActor_SetAnim(a, RTA_WALK, TRUE);
        }
    }
    else if (a->state == AST_NORMAL && a->anim == RTA_WALK)
    {
        RtActor_SetAnim(a, RTA_IDLE, TRUE);
    }
}

void RtActor_Update(struct RtActor *a)
{
    if (!a->active)
        return;
    // safety net: an actor at 0 HP must always go through the faint sequence
    if (a->hp == 0 && a->state != AST_FAINT && a->state != AST_GONE && a->state != AST_NONE
     && a->state != AST_CAUGHT && a->state != AST_ENTER && a->state != AST_RETURN)
        RtActor_Faint(a);

    switch (a->state)
    {
    case AST_GONE:
    case AST_NONE:
        return;
    case AST_ENTER:
        if (a->stateTimer == 20)
        {
            a->hidden = FALSE;
            a->flash = 10;
            RtFx_Impact(RTI_STAR, FXP_WHITE, a->x, a->y - TO_FX(a->height / 2));
            PlayCry_Normal(a->species, a->side == RT_SIDE_PLAYER ? -25 : 25);
        }
        if (a->stateTimer)
            a->stateTimer--;
        if (a->stateTimer == 0)
        {
            a->state = AST_NORMAL;
            a->hidden = FALSE;
        }
        return;
    case AST_FAINT:
        if (a->stateTimer)
            a->stateTimer--;
        a->z = 0;
        if (a->stateTimer < 30 && (a->stateTimer & 3) == 0)
            a->hidden = !a->hidden;
        if (a->stateTimer == 0)
        {
            a->hidden = TRUE;
            Rt_AfterFaintAnim(a);
        }
        return;
    case AST_CAUGHT:
        a->hidden = TRUE;
        return;
    case AST_RETURN:
        a->state = AST_GONE;
        a->hidden = TRUE;
        return;
    }

    RtActor_TickStatus(a);
    if (!RtActor_IsAlive(a) || a->state == AST_FAINT || a->state == AST_GONE)
        return;

    // knockback
    if (a->vx || a->vy)
    {
        a->x += a->vx;
        a->y += a->vy;
        a->vx = a->vx * 3 / 4;
        a->vy = a->vy * 3 / 4;
        if (a->vx > -16 && a->vx < 16) a->vx = 0;
        if (a->vy > -16 && a->vy < 16) a->vy = 0;
    }

    switch (a->state)
    {
    case AST_NORMAL:
        if (a->recharge)
        {
            a->recharge--;
            break;
        }
        if (a->flinch)
            break;
        if (a->bide)
            break;
        if (a->ingrain)
        {
            // rooted: can't move but can attack
            a->inMoveX = a->inMoveY = 0;
        }
        if (a->rampage && a->inSlot < 0 && a->gcd == 0)
        {
            // rampaging mons keep attacking the nearest foe
            struct RtActor *t = Rt_NearestFoe(a);
            if (t)
            {
                u8 ang = RtAtan2(FROM_FX(t->x - a->x), FROM_FX(t->y - a->y));
                RtMove_StartCalled(a, a->rampageMove, ang);
                break;
            }
        }
        if (a->inDash && a->dashCd == 0 && !a->ingrain && !a->trap)
        {
            u8 ang;
            if (a->inMoveX || a->inMoveY)
                ang = RtAtan2(a->inMoveX, a->inMoveY);
            else
                ang = RtDirToAngle(a->facing);
            a->state = AST_DASH;
            a->stateTimer = 10;
            a->iframes = 14;
            a->dashCd = 42;
            a->aim = ang;
            RtActor_SetFacingFromAngle(a, ang);
            RtActor_SetAnim(a, RTA_WALK, TRUE);
            PlaySE(SE_M_DOUBLE_TEAM);
            RtFx_Burst(gRt->floorKind == 1 ? FX_DROP8 : FX_SMOKE16, gRt->floorKind == 1 ? FXP_WATER : FXP_SMOKE, a->x, a->y, 3, 1, 14);
            break;
        }
        if (a->inSlot >= 0 && RtActor_CanAct(a))
        {
            u8 slot = a->inSlot;
            if (RtMove_TryStart(a, slot, a->inAim))
                break;
        }
        ApplyMovement(a, a->inMoveX, a->inMoveY, RtActor_SpeedPx(a));
        break;
    case AST_DASH:
        {
            s32 sp = 4 * 256; // 4 px/frame
            a->x += RtCos(a->aim) * sp / 256;
            a->y += RtSin(a->aim) * sp / 256;
            if ((a->stateTimer & 1) == 0)
            {
                struct RtPart *p = RtFx_Spawn(FX_GLOW16, FXP_WHITE, a->x, a->y - TO_FX(a->height / 2), 8);
                if (p)
                    p->flags |= PF_BLEND;
            }
            if (--a->stateTimer == 0)
            {
                a->state = AST_NORMAL;
                RtActor_SetAnim(a, RTA_IDLE, TRUE);
            }
        }
        break;
    case AST_HURT:
        if (a->stateTimer)
            a->stateTimer--;
        if (a->stateTimer == 0)
        {
            a->state = AST_NORMAL;
            RtActor_SetAnim(a, RTA_IDLE, TRUE);
        }
        break;
    case AST_STUN:
        if (a->mstatus == MST_SLP || a->mstatus == MST_FRZ)
        {
            // player can mash to recover faster
            if (a->id == gRt->playerLead && a->side == RT_SIDE_PLAYER && (gMain.newKeysRaw & (A_BUTTON | B_BUTTON | L_BUTTON | R_BUTTON)))
            {
                if (a->mstatusTimer > 6)
                    a->mstatusTimer -= 6;
                a->x += TO_FX((Random() & 1) ? 1 : -1);
            }
            if (a->mstatus == MST_FRZ && (gRt->frame & 15) == 0)
                RtFx_Spawn(FX_SHARD16, FXP_ICE, a->x + TO_FX((Random() % 16) - 8), a->y - TO_FX(Random() % (a->height + 1)), 14);
            break;
        }
        if (a->stateTimer)
            a->stateTimer--;
        if (a->stateTimer == 0)
        {
            a->state = AST_NORMAL;
            RtActor_SetAnim(a, RTA_IDLE, TRUE);
        }
        break;
    case AST_CAST:
        RtMove_UpdateCast(a);
        break;
    }

    if (a->state != AST_GONE && !a->invuln)
    {
        SeparateActors(a);
        RtArena_ResolveActor(a);
    }
    else if (a->invuln)
    {
        // keep inside arena bounds while airborne / underground
        s32 x = FROM_FX(a->x), y = FROM_FX(a->y);
        if (x < ARENA_BORDER * 16 + 8) x = ARENA_BORDER * 16 + 8;
        if (x > ARENA_W - ARENA_BORDER * 16 - 8) x = ARENA_W - ARENA_BORDER * 16 - 8;
        if (y < ARENA_BORDER * 16 + 8) y = ARENA_BORDER * 16 + 8;
        if (y > ARENA_H - ARENA_BORDER * 16 - 4) y = ARENA_H - ARENA_BORDER * 16 - 4;
        a->x = TO_FX(x);
        a->y = TO_FX(y);
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static const u8 sShapeW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const u8 sShapeH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

// afterimage of the actor's current pose, glowing (semi-transparent), drawn behind it
void RtActor_DrawGhost(struct RtActor *a, s32 dx, s32 dy)
{
    const struct RtFrame *f;
    u8 shape, size, w, h;
    bool8 hf;
    u16 *oam;
    s32 sx, sy, x, y;
    if (!a->active || a->state == AST_GONE || a->hidden || a->curFrame == 0xFFFF)
        return;
    sx = FROM_FX(a->x) - gRt->camX + dx;
    sy = FROM_FX(a->y) - gRt->camY + dy;
    f = &gRtFrames[a->curFrame];
    FrameIndex(a, &hf);
    shape = f->shapeSize >> 2;
    size = f->shapeSize & 3;
    w = sShapeW[shape][size];
    h = sShapeH[shape][size];
    x = hf ? (sx - (f->x + w)) : (sx + f->x);
    y = sy - a->z + f->y;
    if (x + w < 0 || x > DISPLAY_WIDTH || y + h < 0 || y > DISPLAY_HEIGHT)
        return;
    oam = (u16 *)RtOam_Next(4);
    if (!oam)
        return;
    oam[0] = (y & 0xFF) | (shape << 14) | (1 << 10);
    oam[1] = (x & 0x1FF) | (size << 14) | (hf ? (1 << 12) : 0);
    oam[2] = (a->vramSlot * 64) | (2 << 10) | (a->palSlot << 12);
}

void RtActor_Draw(struct RtActor *a)
{
    s32 sx, sy;
    const struct RtFrame *f;
    u8 shape, size, w, h;
    bool8 hf;
    struct OamData *o;
    u16 *oam;

    if (!a->active || a->state == AST_GONE)
        return;
    sx = FROM_FX(a->x) - gRt->camX;
    sy = FROM_FX(a->y) - gRt->camY;

    // sprite
    if (!a->hidden && a->curFrame != 0xFFFF && !(a->invuln == 2 || a->invuln == 3))
    {
        bool8 blink = (a->state == AST_DASH) || (a->iframes > 14 && (gRt->frame & 2));
        f = &gRtFrames[a->curFrame];
        FrameIndex(a, &hf);
        shape = f->shapeSize >> 2;
        size = f->shapeSize & 3;
        w = sShapeW[shape][size];
        h = sShapeH[shape][size];
        {
            s32 x = hf ? (sx - (f->x + w)) : (sx + f->x);
            s32 y = sy - a->z + f->y;
            if (a->state == AST_FAINT)
                y += (50 - a->stateTimer) / 4;
            if (!(x + w < 0 || x > DISPLAY_WIDTH || y + h < 0 || y > DISPLAY_HEIGHT) && !(blink && (gRt->frame & 1)))
            {
                o = RtOam_Next(2);
                if (o)
                {
                    oam = (u16 *)o;
                    oam[0] = (y & 0xFF) | (shape << 14) | (a->state == AST_DASH ? (1 << 10) : 0);
                    oam[1] = (x & 0x1FF) | (size << 14) | (hf ? (1 << 12) : 0);
                    oam[2] = (a->vramSlot * 64) | (2 << 10) | (a->palSlot << 12);
                }
            }
        }
    }

    // shadow (always under)
    if (a->state != AST_FAINT || a->stateTimer > 20)
    {
        u8 shadow = gRtSpeciesGfx[a->gfxDex].shadow;
        u8 fx = shadow == 0 ? FX_SHADOW16 : (shadow == 1 ? FX_SHADOW32 : FX_SHADOW32L);
        const struct RtFxShape *s = &gRtFxShapes[fx];
        s32 x = sx - s->w / 2, y = sy - s->h / 2 + 1;
        if (a->invuln == 2 || a->invuln == 3)
            fx = FX_SHADOW16;
        if (!(x + s->w < 0 || x > DISPLAY_WIDTH || y + s->h < 0 || y > DISPLAY_HEIGHT))
        {
            o = RtOam_Next(3);
            if (o)
            {
                oam = (u16 *)o;
                oam[0] = (y & 0xFF) | ((s->shapeSize >> 2) << 14);
                oam[1] = (x & 0x1FF) | ((s->shapeSize & 3) << 14);
                oam[2] = (RT_FX_TILE_BASE + s->tile) | (2 << 10) | (4 << 12);
            }
        }
    }
}
