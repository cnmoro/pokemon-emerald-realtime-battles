// Real-time battle UI: HUD, toasts, pause/bag/party menus, prompts, ball throwing, safari.
#include "global.h"
#include "rt_internal.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_pyramid.h"
#include "battle_setup.h"
#include "battle_tower.h"
#include "bg.h"
#include "data.h"
#include "decompress.h"
#include "item.h"
#include "main.h"
#include "menu.h"
#include "m4a.h"
#include "party_menu.h"
#include "pokedex.h"
#include "palette.h"
#include "pokemon.h"
#include "random.h"
#include "sound.h"
#include "string_util.h"
#include "text.h"
#include "trainer_hill.h"
#include "window.h"
#include "constants/abilities.h"
#include "constants/battle.h"
#include "constants/battle_frontier.h"
#include "constants/battle_pyramid.h"
#include "constants/hold_effects.h"
#include "constants/item_effects.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/trainers.h"

void Rt_AfterFaintAnim(struct RtActor *a);
void RtActor_CureStatus(struct RtActor *a);
extern u8 gNumSafariBalls;

enum
{
    WIN_TOP,
    WIN_BOTTOM,
    WIN_TOAST,
    WIN_MSG,
    WIN_MENU,
    WIN_EXP,
    WIN_COUNT
};

// HUD layout: the top bar (16px) shows the player's POKéMON on the left and the foe on the right,
// the bottom bar (16px) the four moves with their cooldown bars
#define HUD_BOTTOM_Y      (DISPLAY_HEIGHT - HUD_BOTTOM)
#define HUD_ROW2_Y        0
#define HUD_ROW2_TEXT_Y   0
#define HUD_CD_BAR_Y      (HUD_BOTTOM_Y + 8)
#define HUD_SPLIT_X       120 // left half: player, right half: foe(s)
#define HUD_BAR_Y         4   // y of the HP bar sprites (bar pixels on screen rows 7..11)
// singles: name | HP bar | numbers on each side
#define HUD_P_BAR_X       48
#define HUD_P_HP_X        84
#define HUD_E_BAR_X       190
#define HUD_BAR_W         32
// doubles: the two foes get 60px each (name + short bar)
#define HUD_E2_W          60
#define HUD_E2_BAR_W      24

static const struct WindowTemplate sRtWindows[] =
{
    [WIN_TOP]    = { .bg = 0, .tilemapLeft = 0,  .tilemapTop = 0,  .width = 30, .height = 2,  .paletteNum = 15, .baseBlock = 1 },
    [WIN_BOTTOM] = { .bg = 0, .tilemapLeft = 0,  .tilemapTop = 18, .width = 30, .height = 2,  .paletteNum = 15, .baseBlock = 61 },
    [WIN_TOAST]  = { .bg = 0, .tilemapLeft = 1,  .tilemapTop = 2,  .width = 28, .height = 2,  .paletteNum = 15, .baseBlock = 181 },
    [WIN_MSG]    = { .bg = 0, .tilemapLeft = 0,  .tilemapTop = 14, .width = 30, .height = 4,  .paletteNum = 14, .baseBlock = 237 },
    [WIN_MENU]   = { .bg = 0, .tilemapLeft = 15, .tilemapTop = 4,  .width = 14, .height = 9,  .paletteNum = 14, .baseBlock = 357 },
    // end-of-battle EXP panel: shares the tiles of WIN_MSG and WIN_MENU (never shown together)
    [WIN_EXP]    = { .bg = 0, .tilemapLeft = 5,  .tilemapTop = 3,  .width = 20, .height = 10,  .paletteNum = 14, .baseBlock = 237 },
    DUMMY_WIN_TEMPLATE
};

static const u16 sHudPal[16] = {
    RGB(0, 0, 0), RGB(3, 4, 7), RGB(31, 31, 31), RGB(9, 9, 12), RGB(18, 18, 20), RGB(31, 10, 8), RGB(31, 26, 6), RGB(12, 28, 12),
    RGB(14, 22, 31), RGB(25, 14, 31), RGB(31, 18, 6), RGB(6, 6, 9), 0, 0, 0, 0
};
static const u16 sMenuPal[16] = {
    RGB(0, 0, 0), RGB(31, 31, 31), RGB(6, 6, 7), RGB(21, 21, 23), RGB(4, 8, 20), RGB(28, 6, 6), RGB(8, 20, 8), RGB(26, 26, 28),
    0, 0, 0, 0, 0, 0, 0, 0
};

static const u8 sColHud[] = {1, 2, 3};
static const u8 sColHudGray[] = {1, 4, 3};
static const u8 sColHudRed[] = {1, 5, 3};
static const u8 sColHudYellow[] = {1, 6, 3};
static const u8 sColHudGreen[] = {1, 7, 3};
static const u8 sColHudBlue[] = {1, 8, 3};
static const u8 sColHudPurple[] = {1, 9, 3};
static const u8 sColHudOrange[] = {1, 10, 3};
static const u8 sColMenu[] = {1, 2, 3};
static const u8 sColMenuRed[] = {1, 5, 3};
// cursor arrow: transparent background, so the glyph's empty right side doesn't paint over the row's text
static const u8 sColMenuArrow[] = {0, 5, 3};
static const u8 sColMenuGray[] = {1, 3, 7};

static const u8 sText_Lv[] = _("Lv");
static const u8 sText_Slash[] = _("/");
static const u8 sText_PSN[] = _("PSN");
static const u8 sText_BRN[] = _("BRN");
static const u8 sText_PAR[] = _("PAR");
static const u8 sText_SLP[] = _("SLP");
static const u8 sText_FRZ[] = _("FRZ");
static const u8 sText_TOX[] = _("TOX");
static const u8 sText_FNT[] = _("FNT");
static const u8 sButtonA[] = _("A");
static const u8 sButtonB[] = _("B");
static const u8 sButtonL[] = _("L");
static const u8 sButtonR[] = _("R");
static const u8 sText_Ball[] = _("THROW BALL");
static const u8 sText_Cancel[] = _("B:cancel");
static const u8 sText_Resume[] = _("FIGHT");
static const u8 sText_Bag[] = _("BAG");
static const u8 sText_Pokemon[] = _("POKéMON");
static const u8 sText_Run[] = _("RUN");
static const u8 sText_Back[] = _("BACK");
static const u8 sText_NoItems[] = _("No usable items.");
static const u8 sText_ChooseMon[] = _("Choose a POKéMON.");
static const u8 sText_ChooseNext[] = _("Send out which POKéMON?");
static const u8 sText_LearnPrompt[] = _("{STR_VAR_1} wants to learn\n{STR_VAR_2}. Forget which move?");
static const u8 sText_DontLearn[] = _("DON'T LEARN");
static const u8 sText_DidNotLearn[] = _("{STR_VAR_1} did not learn\n{STR_VAR_2}.");
static const u8 sText_ForgotLearned[] = _("{STR_VAR_1} forgot {STR_VAR_3} and\nlearned {STR_VAR_2}!");
static const u8 sText_ConfirmForget[] = _("Forget {STR_VAR_3}\nand learn {STR_VAR_2}?");
static const u8 sText_ConfirmGiveUp[] = _("Stop learning\n{STR_VAR_2}?");
static const u8 sText_ItemUsed[] = _("Used {STR_VAR_2} on {STR_VAR_1}.");
static const u8 sText_NoEffect[] = _("It won't have any effect.");
static const u8 sText_BallBlocked[] = _("The TRAINER blocked the BALL!\nDon't be a thief!");
static const u8 sText_BrokeFree[] = _("Oh no! The POKéMON broke free!");
static const u8 sText_AlmostHad[] = _("Aargh! Almost had it!");
static const u8 sText_AlreadyOut[] = _("{STR_VAR_1} is already in battle!");
static const u8 sText_NoEnergy[] = _("{STR_VAR_1} has no energy\nleft to battle!");
static const u8 sText_BallReady[] = _("Aim and press A to throw!");
static const u8 sText_SafariHelp[] = _("A:BALL B:{POKEBLOCK} L:GO NEAR START:RUN");
static const u8 sText_SafariBalls[] = _("SAFARI BALLS: {STR_VAR_1}");
static const u8 sText_WatchingCarefully[] = _("{STR_VAR_1} is watching carefully!");
static const u8 sText_Eating[] = _("{STR_VAR_1} is eating!");
static const u8 sText_Angry[] = _("{STR_VAR_1} is angry!");
static const u8 sText_NoSwitch[] = _("{STR_VAR_1} can't be switched out!");
static const u8 sText_HpFmt[] = _("{STR_VAR_1}/{STR_VAR_2}");

static bool8 CanSwitchTo(u8 idx, bool8 msg);
static EWRAM_DATA bool8 sTrainerPicShown = FALSE;
static EWRAM_DATA u16 sCdBarDrawn[MAX_MON_MOVES] = {0}; // fill | color << 8 of the cooldown bar tiles
static EWRAM_DATA bool8 sMsgActive = FALSE;
static EWRAM_DATA u8 sMsgBuf[200] = {0};
static EWRAM_DATA bool8 sBatonPending = FALSE;
static EWRAM_DATA s8 sBatonStages[STG_COUNT] = {0};
static EWRAM_DATA s32 sReticleX = 0;
static EWRAM_DATA s32 sReticleY = 0;
static EWRAM_DATA u8 sMoveHudState[MAX_MON_MOVES] = {0};

// ---------------------------------------------------------------------------
// Bars drawn into OBJ tiles
// ---------------------------------------------------------------------------
#define UI_TILE_PLAYER_BAR 256
#define UI_TILE_ENEMY_BAR0 264
#define UI_TILE_ENEMY_BAR1 272
#define UI_TILE_CD0        280
#define UI_TILE_ALLY_BAR   296
#define UI_TILE_KI         300 // 4 tiles: the ki bar (and the beam struggle balance bar while one is going)
static EWRAM_DATA u16 sKiDrawn = 0;

static void SetPixel(u8 *tiles, u8 widthTiles, s32 x, s32 y, u8 c)
{
    u8 *t = tiles + ((y / 8) * widthTiles + (x / 8)) * 32 + (y % 8) * 4 + (x % 8) / 2;
    if (x & 1)
        *t = (*t & 0x0F) | (c << 4);
    else
        *t = (*t & 0xF0) | c;
}

// draws a w-pixel bar (rows 2..5) into a widthTiles x 1 tile strip
static void DrawBar(u16 tileBase, u8 widthTiles, u8 w, u8 fill, u8 color, u8 rows)
{
    u32 bufw[8 * 8];
    s32 tx, y, k;
    s32 top = 8 - rows - 1;
    u32 edgeWord = 0x11111111;
    for (tx = 0; tx < widthTiles; tx++)
    {
        u32 inner = 0, edgeRowWord = 0;
        for (k = 7; k >= 0; k--)
        {
            s32 x = tx * 8 + k;
            u32 c, e;
            if (x > w + 1)
                c = 0, e = 0;
            else if (x == 0 || x == w + 1)
                c = 1, e = 1;
            else
                c = (x - 1 < fill) ? color : 2, e = 1;
            inner = (inner << 4) | c;
            edgeRowWord = (edgeRowWord << 4) | e;
        }
        (void)edgeWord;
        for (y = 0; y < 8; y++)
        {
            u32 v;
            if (y < top)
                v = 0;
            else if (y == top || y == 7)
                v = edgeRowWord;
            else
                v = inner;
            bufw[tx * 8 + y] = v;
        }
    }
    CpuCopy32(bufw, (void *)(OBJ_VRAM0 + tileBase * TILE_SIZE_4BPP), widthTiles * 32);
}
static u8 HpColor(u16 hp, u16 max)
{
    if (max == 0)
        return 8;
    if (hp * 2 > max)
        return 7;
    if (hp * 5 > max)
        return 9;
    return 5;
}
static void DrawHpBar(u16 tileBase, u16 shown, u16 max, u8 widthPx)
{
    u8 fill = max ? (shown * widthPx + max - 1) / max : 0;
    if (shown == 0) fill = 0;
    DrawBar(tileBase, widthPx > 30 ? 8 : 4, widthPx, fill, HpColor(shown, max), 4);
}

// ---------------------------------------------------------------------------
// Init / HUD text
// ---------------------------------------------------------------------------
void RtUi_Init(void)
{
    InitWindows(sRtWindows);
    DeactivateAllTextPrinters();
    LoadPalette(sHudPal, BG_PLTT_ID(15), PLTT_SIZE_4BPP);
    LoadPalette(sMenuPal, BG_PLTT_ID(14), PLTT_SIZE_4BPP);
    FillWindowPixelBuffer(WIN_TOP, PIXEL_FILL(1));
    FillWindowPixelBuffer(WIN_BOTTOM, PIXEL_FILL(1));
    PutWindowTilemap(WIN_TOP);
    PutWindowTilemap(WIN_BOTTOM);
    CopyWindowToVram(WIN_TOP, COPYWIN_FULL);
    CopyWindowToVram(WIN_BOTTOM, COPYWIN_FULL);
    sTrainerPicShown = FALSE;
    sMsgActive = FALSE;
    sBatonPending = FALSE;
    sReticleX = TO_FX(ARENA_W / 2);
    sReticleY = TO_FX(ARENA_H / 2);
    memset(sMoveHudState, 0xFF, sizeof(sMoveHudState));
    memset(sCdBarDrawn, 0xFF, sizeof(sCdBarDrawn));
    sKiDrawn = 0xFFFF;
    {
        extern void RtAi_ResetTrainerItems(void);
        RtAi_ResetTrainerItems();
    }
    gRt->hudDirty = TRUE;
    gRt->moveHudDirty = TRUE;
}

static const u8 *StatusText(u8 st, const u8 **col)
{
    switch (st)
    {
    case MST_PSN: *col = sColHudPurple; return sText_PSN;
    case MST_TOX: *col = sColHudPurple; return sText_TOX;
    case MST_BRN: *col = sColHudOrange; return sText_BRN;
    case MST_PAR: *col = sColHudYellow; return sText_PAR;
    case MST_SLP: *col = sColHudGray; return sText_SLP;
    case MST_FRZ: *col = sColHudBlue; return sText_FRZ;
    }
    return NULL;
}

static void PrintMonHeader(u8 win, struct RtActor *a, u8 x, u8 y)
{
    u8 *s = gStringVar4;
    const u8 *col;
    const u8 *st;
    GetMonData(a->mon, MON_DATA_NICKNAME, gStringVar1);
    StringGet_Nickname(gStringVar1);
    AddTextPrinterParameterized3(win, FONT_SMALL, x, y, sColHud, TEXT_SKIP_DRAW, gStringVar1);
    s = StringCopy(gStringVar4, sText_Lv);
    ConvertIntToDecimalStringN(s, a->level, STR_CONV_MODE_LEFT_ALIGN, 3);
    AddTextPrinterParameterized3(win, FONT_SMALL, x + 50, y, sColHud, TEXT_SKIP_DRAW, gStringVar4);
    st = StatusText(a->hp ? a->mstatus : 0xFF, &col);
    if (a->hp == 0)
    {
        st = sText_FNT;
        col = sColHudRed;
    }
    if (st)
        AddTextPrinterParameterized3(win, FONT_SMALL, x + 50, y + (win == WIN_TOP ? 0 : 0), col, TEXT_SKIP_DRAW, st);
}

// ---------------------------------------------------------------------------
// HUD: list of independent text elements, each redrawn only when its key changes,
// at most one text print per frame (text rendering is expensive on the GBA).
// ---------------------------------------------------------------------------
enum
{
    HE_E0_NAME,
    HE_E1_NAME,
    HE_TOP_EXTRA,
    HE_P_NAME,
    HE_P_HP,
    HE_COUNT
};

struct HudElem
{
    u8 x, w;
};

// all in the top window; doubles have their own table for the foes
static const struct HudElem sHudElems[HE_COUNT] =
{
    [HE_P_NAME]    = {0, HUD_P_BAR_X},
    [HE_P_HP]      = {HUD_P_HP_X, HUD_SPLIT_X - 1 - HUD_P_HP_X},
    [HE_E0_NAME]   = {HUD_SPLIT_X + 1, HUD_E_BAR_X - HUD_SPLIT_X - 1}, // name and level (or status)
    [HE_TOP_EXTRA] = {HUD_E_BAR_X, 240 - HUD_E_BAR_X}, // party dots under the foe's bar
};
static const struct HudElem sHudElemsDouble[HE_COUNT] =
{
    [HE_P_NAME]    = {0, HUD_P_BAR_X},
    [HE_P_HP]      = {HUD_P_HP_X, HUD_SPLIT_X - 1 - HUD_P_HP_X},
    [HE_E0_NAME]   = {HUD_SPLIT_X + 1, HUD_E2_W - HUD_E2_BAR_W - 3},
    [HE_E1_NAME]   = {HUD_SPLIT_X + HUD_E2_W + 1, HUD_E2_W - HUD_E2_BAR_W - 3},
};
#define HUD_ELEMS (gRt->isDouble ? sHudElemsDouble : sHudElems)

static EWRAM_DATA u32 sHudKey[HE_COUNT] = {0};
static EWRAM_DATA u16 sDrawnHp = 0xFFFF;
static EWRAM_DATA u8 sMovesPending = 0;
static EWRAM_DATA u8 sEnemyPartyCountCache = 0;
static EWRAM_DATA u8 sEnemyPartyTotalCache = 0;

static void RefreshEnemyPartyCount(void)
{
    s32 i;
    sEnemyPartyCountCache = Rt_CountUsableParty(RT_SIDE_ENEMY);
    sEnemyPartyTotalCache = 0;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        u16 sp = GetMonData(&gEnemyParty[i], MON_DATA_SPECIES_OR_EGG);
        if (sp != SPECIES_NONE && sp != SPECIES_EGG)
            sEnemyPartyTotalCache++;
    }
}

// a name cut to fit maxW pixels
static const u8 *FitName(u8 *buf, u8 maxW)
{
    u8 len = StringLength(buf);
    while (len > 2 && GetStringWidth(FONT_SMALL, buf, 0) > maxW)
        buf[--len] = EOS;
    return buf;
}

#define HUD_KEY_EMPTY 0xFFFFFFFE

static struct RtActor *HudEnemy(u8 k)
{
    struct RtActor *e = &gRt->actors[2 + k];
    if (!e->active || e->state == AST_GONE)
        return NULL;
    if (!gRt->isDouble && k == 1)
        return NULL;
    return e;
}

static u32 HudElemKey(u8 id)
{
    struct RtActor *p = Rt_PlayerActor(), *e;
    switch (id)
    {
    case HE_E0_NAME:
    case HE_E1_NAME:
        e = HudEnemy(id == HE_E0_NAME ? 0 : 1);
        if (!e)
            return HUD_KEY_EMPTY;
        // singles: level or status after the name; doubles: the name takes the status colour
        return (u32)e->mon ^ (e->species << 20) ^ (e->level << 24) ^ (e->mstatus << 12) ^ ((e->hp != 0) << 11);
    case HE_TOP_EXTRA:
        if (gRt->isSafari)
            return 0x10000 | gRt->safariBalls;
        if (gRt->isTrainer && !gRt->isDouble)
            return 0x20000 | sEnemyPartyCountCache | (sEnemyPartyTotalCache << 4);
        return HUD_KEY_EMPTY;
    case HE_P_NAME:
        if (gRt->isSafari)
            return HUD_KEY_EMPTY;
        return p->active ? (u32)p->mon : HUD_KEY_EMPTY;
    case HE_P_HP:
        if (gRt->isSafari || !p->active)
            return HUD_KEY_EMPTY;
        // the status takes the place of the numbers
        if (p->mstatus && p->hp)
            return 0x80000000 | p->mstatus;
        return p->hp | (p->maxHp << 16);
    }
    return HUD_KEY_EMPTY;
}

static void HudElemDraw(u8 id)
{
    const struct HudElem *el = &HUD_ELEMS[id];
    struct RtActor *p = Rt_PlayerActor(), *e;
    u8 y = 2;
    const u8 *col, *st;
    if (gRt->isSafari && id == HE_TOP_EXTRA)
        FillWindowPixelRect(WIN_TOP, PIXEL_FILL(1), 0, 0, HUD_SPLIT_X - 1, 16);
    else if (el->w)
        FillWindowPixelRect(WIN_TOP, PIXEL_FILL(1), el->x, 0, el->w, 16);
    switch (id)
    {
    case HE_E0_NAME:
    case HE_E1_NAME:
        e = HudEnemy(id == HE_E0_NAME ? 0 : 1);
        if (!e)
            break;
        GetMonData(e->mon, MON_DATA_NICKNAME, gStringVar1);
        StringGet_Nickname(gStringVar1);
        col = sColHud;
        if (gRt->isDouble)
        {
            if (e->hp == 0)
                col = sColHudRed;
            else
                StatusText(e->mstatus, &col);
            FitName(gStringVar1, el->w - 2);
        }
        else
        {
            // level (or status) right-aligned before the bar, the name cut to what is left
            const u8 *tagCol = sColHudGray;
            u8 tagW;
            st = e->hp ? StatusText(e->mstatus, &tagCol) : NULL;
            if (st)
            {
                StringCopy(gStringVar4, st);
            }
            else
            {
                u8 *s = StringCopy(gStringVar4, sText_Lv);
                ConvertIntToDecimalStringN(s, e->level, STR_CONV_MODE_LEFT_ALIGN, 3);
                tagCol = sColHudGray;
            }
            tagW = GetStringWidth(FONT_SMALL, gStringVar4, 0);
            AddTextPrinterParameterized3(WIN_TOP, FONT_SMALL, el->x + el->w - 2 - tagW, y, tagCol, TEXT_SKIP_DRAW, gStringVar4);
            FitName(gStringVar1, el->w - tagW - 5);
        }
        AddTextPrinterParameterized3(WIN_TOP, FONT_SMALL, el->x + 1, y, col, TEXT_SKIP_DRAW, gStringVar1);
        break;
    case HE_TOP_EXTRA:
        if (gRt->isSafari)
        {
            ConvertIntToDecimalStringN(gStringVar1, gRt->safariBalls, STR_CONV_MODE_LEFT_ALIGN, 2);
            StringExpandPlaceholders(gStringVar4, sText_SafariBalls);
            AddTextPrinterParameterized3(WIN_TOP, FONT_SMALL, 2, y, sColHudYellow, TEXT_SKIP_DRAW, gStringVar4);
        }
        else if (gRt->isTrainer && !gRt->isDouble)
        {
            // one dot per POKéMON of the foe's party under its HP bar: red = still able, dark = fainted
            s32 k;
            for (k = 0; k < sEnemyPartyTotalCache && k < PARTY_SIZE; k++)
                FillWindowPixelRect(WIN_TOP, PIXEL_FILL(k < sEnemyPartyCountCache ? 5 : 3), el->x + 1 + k * 6, 13, 4, 2);
        }
        break;
    case HE_P_NAME:
        if (!p->active)
            break;
        GetMonData(p->mon, MON_DATA_NICKNAME, gStringVar1);
        StringGet_Nickname(gStringVar1);
        AddTextPrinterParameterized3(WIN_TOP, FONT_SMALL, 2, y, sColHud, TEXT_SKIP_DRAW, FitName(gStringVar1, el->w - 3));
        break;
    case HE_P_HP:
        if (gRt->isSafari || !p->active)
            break;
        st = p->hp ? StatusText(p->mstatus, &col) : NULL;
        if (st)
        {
            AddTextPrinterParameterized3(WIN_TOP, FONT_SMALL, el->x, y, col, TEXT_SKIP_DRAW, st);
            break;
        }
        ConvertIntToDecimalStringN(gStringVar1, p->hp, STR_CONV_MODE_LEFT_ALIGN, 3);
        ConvertIntToDecimalStringN(gStringVar2, p->maxHp, STR_CONV_MODE_LEFT_ALIGN, 3);
        StringExpandPlaceholders(gStringVar4, sText_HpFmt);
        // numbers of 3 digits do not fit: only the current HP then
        if (GetStringWidth(FONT_SMALL, gStringVar4, 0) > el->w)
            StringCopy(gStringVar4, gStringVar1);
        AddTextPrinterParameterized3(WIN_TOP, FONT_SMALL, el->x, y, p->hp ? sColHud : sColHudRed, TEXT_SKIP_DRAW, gStringVar4);
        sDrawnHp = p->hp;
        break;
    }
}

static void DrawHudDivider(void)
{
    if (!gRt->isSafari)
        FillWindowPixelRect(WIN_TOP, PIXEL_FILL(3), HUD_SPLIT_X - 1, 2, 1, 12);
    if (gRt->isDouble)
        FillWindowPixelRect(WIN_TOP, PIXEL_FILL(3), HUD_SPLIT_X + HUD_E2_W - 1, 2, 1, 12);
}

// Forces a full redraw of everything (used when windows were cleared).
void RtUi_RefreshHud(void)
{
    s32 i;
    gRt->hudDirty = FALSE;
    RefreshEnemyPartyCount();
    FillWindowPixelBuffer(WIN_TOP, PIXEL_FILL(1));
    DrawHudDivider();
    for (i = 0; i < HE_COUNT; i++)
    {
        sHudKey[i] = HudElemKey(i);
        if (sHudKey[i] != HUD_KEY_EMPTY)
            HudElemDraw(i);
    }
    CopyWindowToVram(WIN_TOP, COPYWIN_GFX);
}

// Incremental HUD update: redraws at most one changed element per call.
static void RtUi_HudStep(void)
{
    s32 i;
    struct RtActor *p = Rt_PlayerActor();
    // counting the party reads encrypted mon data, which is slow: refresh it rarely
    if (gRt->isTrainer && !gRt->isDouble && (gRt->hudDirty || (gRt->frame & 62) == 0))
        RefreshEnemyPartyCount();
    gRt->hudDirty = FALSE;
    for (i = 0; i < HE_COUNT; i++)
    {
        u32 k = HudElemKey(i);
        if (k == sHudKey[i])
            continue;
        // the HP number follows the animated bar, but only every few frames
        if (i == HE_P_HP && p->hp != p->shownHp && (gRt->frame & 6) != 0)
            continue;
        sHudKey[i] = k;
        HudElemDraw(i);
        CopyWindowToVram(WIN_TOP, COPYWIN_GFX);
        return;
    }
}

static u8 MoveCellState(struct RtActor *p, u8 i)
{
    if (p->moves[i] == MOVE_NONE)
        return 0;
    if (gRt->ballReady && i == 0)
        return 4;
    if (RtActor_MoveSlotUsable(p, i))
        return 1;
    if (p->cd[i])
        return 2;
    return 3;
}
static EWRAM_DATA u8 sMoveNameCache[MAX_MON_MOVES][MOVE_NAME_LENGTH + 2] = {0};
static EWRAM_DATA u16 sMoveNameCacheId[MAX_MON_MOVES] = {0};

static void DrawMoveCell(struct RtActor *p, u8 i, u8 st)
{
    static const u8 *const sBtn[] = {sButtonA, sButtonB, sButtonL, sButtonR};
    const u8 *col = st == 1 ? sColHud : (st == 3 ? sColHudRed : (st == 4 ? sColHudYellow : sColHudGray));
    FillWindowPixelRect(WIN_BOTTOM, PIXEL_FILL(1), i * 60, HUD_ROW2_Y, 60, 16 - HUD_ROW2_Y);
    sMoveHudState[i] = st;
    if (st == 0)
        return;
    AddTextPrinterParameterized3(WIN_BOTTOM, FONT_SMALL, i * 60 + 1, HUD_ROW2_TEXT_Y, sColHudYellow, TEXT_SKIP_DRAW, sBtn[i]);
    if (st == 4)
    {
        AddTextPrinterParameterized3(WIN_BOTTOM, FONT_SMALL_NARROW, i * 60 + 7, HUD_ROW2_TEXT_Y, col, TEXT_SKIP_DRAW, sText_Ball);
    }
    else
    {
        u16 move = p->moves[i];
        if (sMoveNameCacheId[i] != move)
        {
            u8 len;
            u8 *buf = sMoveNameCache[i];
            StringCopy(buf, gMoveNames[move]);
            len = StringLength(buf);
            while (len > 3 && GetStringWidth(FONT_SMALL_NARROW, buf, 0) > 52)
                buf[--len] = EOS;
            sMoveNameCacheId[i] = move;
        }
        AddTextPrinterParameterized3(WIN_BOTTOM, FONT_SMALL_NARROW, i * 60 + 7, HUD_ROW2_TEXT_Y, col, TEXT_SKIP_DRAW, sMoveNameCache[i]);
    }
}

void RtUi_RefreshMoves(void)
{
    struct RtActor *p;
    s32 i;
    gRt->moveHudDirty = FALSE;
    sMovesPending = 0;
    if (gRt->isSafari)
    {
        FillWindowPixelRect(WIN_BOTTOM, PIXEL_FILL(1), 0, HUD_ROW2_Y, 240, 16 - HUD_ROW2_Y);
        AddTextPrinterParameterized3(WIN_BOTTOM, FONT_SMALL, 2, HUD_ROW2_TEXT_Y, sColHudYellow, TEXT_SKIP_DRAW, sText_SafariHelp);
        CopyWindowToVram(WIN_BOTTOM, COPYWIN_GFX);
        return;
    }
    p = Rt_PlayerActor();
    for (i = 0; i < MAX_MON_MOVES; i++)
        DrawMoveCell(p, i, MoveCellState(p, i));
    CopyWindowToVram(WIN_BOTTOM, COPYWIN_GFX);
}

// Redraws at most two changed move cells per call.
static void RtUi_MovesStep(void)
{
    struct RtActor *p;
    s32 i, drawn = 0;
    gRt->moveHudDirty = FALSE;
    sMovesPending = 0;
    if (gRt->isSafari)
        return;
    p = Rt_PlayerActor();
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        u8 st = MoveCellState(p, i);
        if (st == sMoveHudState[i] && !(st != 0 && st != 4 && sMoveNameCacheId[i] != p->moves[i]))
            continue;
        if (drawn >= 2)
        {
            sMovesPending = 1;
            break;
        }
        DrawMoveCell(p, i, st);
        drawn++;
    }
    if (drawn)
        CopyWindowToVram(WIN_BOTTOM, COPYWIN_GFX);
}

// ---------------------------------------------------------------------------
// Toasts / message box
// ---------------------------------------------------------------------------
void RtUi_Toast(const u8 *str, u16 frames)
{
    FillWindowPixelBuffer(WIN_TOAST, PIXEL_FILL(11));
    {
        s32 w = GetStringWidth(FONT_SMALL, str, 0);
        s32 x = (28 * 8 - w) / 2;
        if (x < 2) x = 2;
        AddTextPrinterParameterized3(WIN_TOAST, FONT_SMALL, x, 2, (const u8[]){11, 2, 3}, TEXT_SKIP_DRAW, str);
    }
    PutWindowTilemap(WIN_TOAST);
    CopyWindowToVram(WIN_TOAST, COPYWIN_FULL);
    gRt->toast.active = TRUE;
    gRt->toast.timer = frames;
}

void RtUi_ToastFmt(const u8 *tmpl, u16 frames)
{
    StringExpandPlaceholders(gStringVar4, tmpl);
    // only the first line fits in a toast
    {
        u8 *c;
        for (c = gStringVar4; *c != EOS; c++)
            if (*c == CHAR_NEWLINE)
                *c = CHAR_SPACE;
    }
    RtUi_Toast(gStringVar4, frames);
}

void RtUi_HideToast(void)
{
    if (!gRt->toast.active)
        return;
    gRt->toast.active = FALSE;
    ClearWindowTilemap(WIN_TOAST);
    CopyWindowToVram(WIN_TOAST, COPYWIN_MAP);
}

static void DrawFrame(u8 win, u8 w, u8 h)
{
    FillWindowPixelBuffer(win, PIXEL_FILL(1));
    FillWindowPixelRect(win, PIXEL_FILL(4), 0, 0, w * 8, 2);
    FillWindowPixelRect(win, PIXEL_FILL(4), 0, h * 8 - 2, w * 8, 2);
    FillWindowPixelRect(win, PIXEL_FILL(4), 0, 0, 2, h * 8);
    FillWindowPixelRect(win, PIXEL_FILL(4), w * 8 - 2, 0, 2, h * 8);
}

void RtUi_ShowMessageBox(const u8 *str)
{
    DrawFrame(WIN_MSG, 30, 4);
    StringExpandPlaceholders(sMsgBuf, str);
    AddTextPrinterParameterized3(WIN_MSG, FONT_NORMAL, 8, 2, sColMenu, TEXT_SKIP_DRAW, sMsgBuf);
    PutWindowTilemap(WIN_MSG);
    CopyWindowToVram(WIN_MSG, COPYWIN_FULL);
    sMsgActive = TRUE;
}

bool8 RtUi_MessageBoxDone(void)
{
    if (!sMsgActive)
        return TRUE;
    if (IsTextPrinterActive(WIN_MSG))
    {
        if (gMain.newKeys & (A_BUTTON | B_BUTTON))
            gTextFlags.canABSpeedUpPrint = TRUE;
        return FALSE;
    }
    if (gMain.newKeys & (A_BUTTON | B_BUTTON))
    {
        PlaySE(SE_SELECT);
        return TRUE;
    }
    return FALSE;
}

void RtUi_HideMessageBox(void)
{
    sMsgActive = FALSE;
    ClearWindowTilemap(WIN_MSG);
    CopyWindowToVram(WIN_MSG, COPYWIN_MAP);
    // the bottom bar may have been covered
    PutWindowTilemap(WIN_BOTTOM);
    CopyWindowToVram(WIN_BOTTOM, COPYWIN_MAP);
}

// ---------------------------------------------------------------------------
// Trainer picture (outro speech)
// ---------------------------------------------------------------------------
void RtUi_ShowTrainerPic(u16 trainerId)
{
    u16 pic;
    if (trainerId == TRAINER_SECRET_BASE)
        return;
    if (gBattleTypeFlags & BATTLE_TYPE_FRONTIER)
        pic = GetFrontierTrainerFrontSpriteId(trainerId);
    else if (gBattleTypeFlags & BATTLE_TYPE_TRAINER_HILL)
        pic = GetTrainerHillTrainerFrontSpriteId(trainerId);
    else
        pic = gTrainers[trainerId].trainerPic;
    LZ77UnCompWram(gTrainerFrontPicTable[pic].data, gRt->gfxBuf[2]);
    CpuCopy32(gRt->gfxBuf[2], (void *)(OBJ_VRAM0 + 2 * 64 * TILE_SIZE_4BPP), 64 * TILE_SIZE_4BPP);
    LoadCompressedPalette(gTrainerFrontPicPaletteTable[pic].data, OBJ_PLTT_ID(2), PLTT_SIZE_4BPP);
    gRt->actors[2].curFrame = 0xFFFF;
    gRt->actors[2].hidden = TRUE;
    sTrainerPicShown = TRUE;
}

void RtUi_HideTrainerPic(void)
{
    sTrainerPicShown = FALSE;
}

bool8 RtUi_TrainerPicShown(void)
{
    return sTrainerPicShown;
}

// ---------------------------------------------------------------------------
// OBJ part of the HUD
// ---------------------------------------------------------------------------
static void PutObj(u16 tile, u8 shape, u8 size, s32 x, s32 y, u8 pal, u8 prio)
{
    struct OamData *o = RtOam_Next(0);
    u16 *oam;
    if (!o)
        return;
    oam = (u16 *)o;
    oam[0] = (y & 0xFF) | (shape << 14);
    oam[1] = (x & 0x1FF) | (size << 14);
    oam[2] = tile | (prio << 10) | (pal << 12);
}

static void UpdateShownHp(struct RtActor *a)
{
    if (a->shownHp != a->hp)
    {
        s32 step = a->maxHp / 48 + 1;
        if (a->shownHp > a->hp)
            a->shownHp = (a->shownHp - a->hp > step) ? a->shownHp - step : a->hp;
        else
            a->shownHp = (a->hp - a->shownHp > step) ? a->shownHp + step : a->hp;
        a->hpBarDirty = TRUE;
    }
}

void RtUi_DrawObjs(void)
{
    s32 i;
    struct RtActor *p;

    if (sTrainerPicShown)
        PutObj(2 * 64, 0, 3, 168, 36, 2, 0);

    if (sMsgActive && gRt->state != RTS_FIGHT)
        return;

    // enemy bars in the top window
    for (i = 2; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *e = &gRt->actors[i];
        u16 tile = i == 2 ? UI_TILE_ENEMY_BAR0 : UI_TILE_ENEMY_BAR1;
        if (!e->active || e->state == AST_GONE || (!gRt->isDouble && i == 3))
            continue;
        UpdateShownHp(e);
        if (gRt->isDouble)
        {
            s32 bx = HUD_SPLIT_X + (i - 2) * HUD_E2_W + HUD_E2_W - HUD_E2_BAR_W - 3;
            if (e->hpBarDirty)
            {
                DrawHpBar(tile, e->shownHp, e->maxHp, HUD_E2_BAR_W);
                e->hpBarDirty = FALSE;
            }
            PutObj(tile, 1, 1, bx, HUD_BAR_Y, 4, 0);
        }
        else
        {
            if (e->hpBarDirty)
            {
                DrawHpBar(tile, e->shownHp, e->maxHp, HUD_BAR_W);
                e->hpBarDirty = FALSE;
            }
            PutObj(tile, 1, 1, HUD_E_BAR_X, HUD_BAR_Y, 4, 0);
            PutObj(tile + 4, 1, 1, HUD_E_BAR_X + 32, HUD_BAR_Y, 4, 0);
        }
    }
    if (gRt->isSafari)
    {
        // reticle
        s32 x = FROM_FX(sReticleX) - gRt->camX - 8, y = FROM_FX(sReticleY) - gRt->camY - 8;
        struct OamData *o = RtOam_Next(0);
        if (o)
        {
            u16 *oam = (u16 *)o;
            oam[0] = (y & 0xFF) | (0 << 14);
            oam[1] = (x & 0x1FF) | (1 << 14);
            oam[2] = (RT_FX_TILE_BASE + gRtFxShapes[FX_RETICLE16].tile) | (0 << 10) | (4 << 12);
        }
        return;
    }
    // player bar
    p = Rt_PlayerActor();
    if (p->active)
    {
        UpdateShownHp(p);
        if (p->hpBarDirty)
        {
            DrawHpBar(UI_TILE_PLAYER_BAR, p->shownHp, p->maxHp, HUD_BAR_W);
            p->hpBarDirty = FALSE;
            (void)sDrawnHp;
        }
        PutObj(UI_TILE_PLAYER_BAR, 1, 1, HUD_P_BAR_X, HUD_BAR_Y, 4, 0);
        PutObj(UI_TILE_PLAYER_BAR + 4, 1, 1, HUD_P_BAR_X + 32, HUD_BAR_Y, 4, 0);
        // ki bar under the HP bar (or the struggle balance while a beam struggle is on)
        {
            struct RtActorX *x = &gRt->ax[p->id];
            u8 fill, color;
            u16 key;
            if (gRt->str.on)
            {
                s32 b = gRt->str.bal + RT_STR_MAX; // 0..2*MAX, A = the player's side winning at the right
                struct RtActor *oa = &gRt->actors[gRt->str.ownA];
                fill = b * 30 / (RT_STR_MAX * 2);
                if (oa->side != RT_SIDE_PLAYER)
                    fill = 30 - fill;
                color = 5;
            }
            else
            {
                fill = x->ki * 30 / RT_KI_MAX;
                color = x->superTimer ? 10 : (x->ki >= 800 && (gRt->frame & 8) ? 2 : 6);
            }
            key = fill | (color << 8) | (gRt->str.on << 12);
            if (sKiDrawn != key)
            {
                sKiDrawn = key;
                DrawBar(UI_TILE_KI, 4, 30, fill, color, 3);
            }
            PutObj(UI_TILE_KI, 1, 1, HUD_P_BAR_X, HUD_BAR_Y + 7, 4, 0);
        }
    }
    // ally floating bar
    if (gRt->isDouble)
    {
        struct RtActor *al = &gRt->actors[gRt->playerLead == 0 ? 1 : 0];
        if (al->active && al->state != AST_GONE && !al->hidden)
        {
            UpdateShownHp(al);
            if (al->hpBarDirty)
            {
                DrawHpBar(UI_TILE_ALLY_BAR, al->shownHp, al->maxHp, 24);
                al->hpBarDirty = FALSE;
            }
            PutObj(UI_TILE_ALLY_BAR, 1, 1, FROM_FX(al->x) - gRt->camX - 13, FROM_FX(al->y) - gRt->camY - al->height - 10, 4, 0);
        }
    }
    // cooldown bars
    if (p->active && !gRt->isSafari)
    {
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            u8 st = MoveCellState(p, i), fill, color;
            if (st != sMoveHudState[i])
                gRt->moveHudDirty = TRUE;
            if (p->moves[i] == MOVE_NONE)
                continue;
            if (p->cdMax[i])
                fill = 30 - (u32)p->cd[i] * 30 / p->cdMax[i];
            else
                fill = 30;
            if (p->cd[i] > p->cdMax[i])
                fill = 0;
            color = st == 1 ? 7 : (st == 3 ? 5 : 11);
            // one bar per frame, and only when it changed
            if ((gRt->frame & 3) == i && sCdBarDrawn[i] != (fill | (color << 8)))
            {
                sCdBarDrawn[i] = fill | (color << 8);
                DrawBar(UI_TILE_CD0 + i * 4, 4, 30, fill, color, 2);
            }
            PutObj(UI_TILE_CD0 + i * 4, 1, 1, i * 60 + 16, HUD_CD_BAR_Y, 4, 0);
        }
    }
    // aim reticle for held move buttons
    if (p->active && p->aiming && p->aimHold > 8)
    {
        s32 k;
        for (k = 1; k <= 4; k++)
        {
            s32 x = FROM_FX(p->x) + RtCos(p->aim) * (k * 14) / 256 - gRt->camX - 4;
            s32 y = FROM_FX(p->y) - p->height / 3 + RtSin(p->aim) * (k * 14) / 256 - gRt->camY - 4;
            struct OamData *o = RtOam_Next(0);
            if (o)
            {
                u16 *oam = (u16 *)o;
                oam[0] = (y & 0xFF);
                oam[1] = (x & 0x1FF);
                oam[2] = (RT_FX_TILE_BASE + gRtFxShapes[FX_AIMDOT8].tile) | (1 << 10) | (4 << 12);
            }
        }
    }
}

static void RtUi_MovesStep(void);
// scanlines elapsed since the start of this frame's vblank (0..227)
u16 RtFrameLinesUsed(void)
{
    u16 v = REG_VCOUNT & 0xFF;
    return v >= 160 ? v - 160 : v + 68;
}
void RtUi_Update(void)
{
    if (gRt->toast.active)
    {
        if (gRt->toast.timer)
            gRt->toast.timer--;
        if (gRt->toast.timer == 0)
            RtUi_HideToast();
    }
    { extern void RtProf(u8); RtProf(15); }
    // text rendering is slow: only do it when this frame still has time left
    // (never starve it though: after a few busy frames, draw anyway)
    {
        static EWRAM_DATA u8 sTextStarve = 0;
        bool8 go = RtFrameLinesUsed() < 150 || sTextStarve >= 6;
        sTextStarve = go ? 0 : sTextStarve + 1;
        if (!go)
            goto skip_text;
    }
    {
        if ((gRt->frame & 1) == 0)
            RtUi_HudStep();
        else if (gRt->moveHudDirty || sMovesPending)
            RtUi_MovesStep();
    }
skip_text:
    // low health beep
    if (!gRt->isSafari && gRt->state == RTS_FIGHT)
    {
        struct RtActor *p = Rt_PlayerActor();
        static EWRAM_DATA bool8 sLowHpPlaying = FALSE;
        bool8 low = p->active && p->state != AST_GONE && p->hp && p->hp * 5 <= p->maxHp;
        if (low && !sLowHpPlaying)
        {
            m4aSongNumStart(SE_LOW_HEALTH);
            sLowHpPlaying = TRUE;
        }
        else if (!low && sLowHpPlaying)
        {
            m4aSongNumStop(SE_LOW_HEALTH);
            sLowHpPlaying = FALSE;
        }
    }
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------
enum
{
    MENU_MAIN,
    MENU_BAG,
    MENU_PARTY,
    MENU_ITEM_TARGET,
    MENU_PARTY_FORCED,
    MENU_LEARN,
    MENU_MESSAGE,
};

#define MENU_ROWS 5

static bool8 IsBattlePyramid(void)
{
    return CurrentBattlePyramidLocation() != PYRAMID_LOCATION_NONE;
}

static u16 GetBagQty(u16 item)
{
    if (IsBattlePyramid())
    {
        u8 lvl = gSaveBlock2Ptr->frontier.lvlMode;
        s32 i;
        for (i = 0; i < PYRAMID_BAG_ITEMS_COUNT; i++)
            if (gSaveBlock2Ptr->frontier.pyramidBag.itemId[lvl][i] == item)
                return gSaveBlock2Ptr->frontier.pyramidBag.quantity[lvl][i];
        return 0;
    }
    return CountTotalItemQuantityInBag(item);
}

static void RemoveItem(u16 item)
{
    // the plain POKe BALL is never used up: the player always keeps at least one
    if (item == ITEM_POKE_BALL && !IsBattlePyramid())
        return;
    if (IsBattlePyramid())
        RemovePyramidBagItem(item, 1);
    else
        RemoveBagItem(item, 1);
}

static bool8 IsBattleItem(u16 item)
{
    u8 pocket = GetItemPocket(item);
    if (item == ITEM_NONE)
        return FALSE;
    if (pocket == POCKET_POKE_BALLS)
        return TRUE;
    if (GetItemBattleUsage(item))
        return TRUE;
    return FALSE;
}

static void BuildBagList(void)
{
    u8 n = 0;
    if (IsBattlePyramid())
    {
        u8 lvl = gSaveBlock2Ptr->frontier.lvlMode;
        s32 i;
        for (i = 0; i < PYRAMID_BAG_ITEMS_COUNT && n < ARRAY_COUNT(gRt->menuItems); i++)
        {
            u16 item = gSaveBlock2Ptr->frontier.pyramidBag.itemId[lvl][i];
            if (IsBattleItem(item) && gSaveBlock2Ptr->frontier.pyramidBag.quantity[lvl][i])
                gRt->menuItems[n++] = item;
        }
    }
    else
    {
        static const u8 sPockets[] = {POCKET_ITEMS, POCKET_POKE_BALLS, POCKET_BERRIES};
        u32 k, i;
        for (k = 0; k < ARRAY_COUNT(sPockets); k++)
        {
            struct BagPocket *pocket = &gBagPockets[sPockets[k] - 1];
            for (i = 0; i < pocket->capacity && n < ARRAY_COUNT(gRt->menuItems); i++)
            {
                u16 item = pocket->itemSlots[i].itemId;
                if (item && IsBattleItem(item) && BagGetQuantityByPocketPosition(sPockets[k], i))
                    gRt->menuItems[n++] = item;
            }
        }
    }
    gRt->menuCount = n;
}

static void PrintMenuTitle(const u8 *str)
{
    AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 6, 3, sColMenuGray, TEXT_SKIP_DRAW, str);
}

static void DrawMenu(void)
{
    s32 i, row;
    DrawFrame(WIN_MENU, 14, 9);
    switch (gRt->menuKind)
    {
    case MENU_MAIN:
        {
            static const u8 *const sOpts[] = {sText_Resume, sText_Bag, sText_Pokemon, sText_Run};
            for (i = 0; i < 4; i++)
                AddTextPrinterParameterized3(WIN_MENU, FONT_NORMAL, 16, 6 + i * 16, sColMenu, TEXT_SKIP_DRAW, sOpts[i]);
            AddTextPrinterParameterized3(WIN_MENU, FONT_NORMAL, 6, 6 + gRt->menuCursor * 16, sColMenuArrow, TEXT_SKIP_DRAW, (const u8[]){CHAR_RIGHT_ARROW, EOS});
        }
        break;
    case MENU_BAG:
        if (gRt->menuCount == 0)
        {
            AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 6, 20, sColMenu, TEXT_SKIP_DRAW, sText_NoItems);
            break;
        }
        for (row = 0; row < MENU_ROWS; row++)
        {
            u8 idx = gRt->menuScroll + row;
            u8 *s;
            if (idx >= gRt->menuCount)
                break;
            CopyItemName(gRt->menuItems[idx], gStringVar1);
            AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 14, 3 + row * 13, sColMenu, TEXT_SKIP_DRAW, gStringVar1);
            s = gStringVar2;
            *s++ = CHAR_x;
            ConvertIntToDecimalStringN(s, GetBagQty(gRt->menuItems[idx]), STR_CONV_MODE_LEFT_ALIGN, 3);
            AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 88, 3 + row * 13, sColMenuGray, TEXT_SKIP_DRAW, gStringVar2);
        }
        AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 5, 3 + (gRt->menuCursor - gRt->menuScroll) * 13, sColMenuArrow, TEXT_SKIP_DRAW, (const u8[]){CHAR_RIGHT_ARROW, EOS});
        {
            // description of selected item in the message box area
        }
        break;
    case MENU_PARTY:
    case MENU_ITEM_TARGET:
    case MENU_PARTY_FORCED:
        for (row = 0; row < MENU_ROWS + 1 && row < PARTY_SIZE; row++)
        {
            struct Pokemon *mon = &gPlayerParty[row];
            u16 sp = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);
            const u8 *col = sColMenu;
            if (sp == SPECIES_NONE)
                continue;
            if (gRt->partner && row >= 3)
                col = sColMenuGray;
            GetMonData(mon, MON_DATA_NICKNAME, gStringVar1);
            StringGet_Nickname(gStringVar1);
            if (GetMonData(mon, MON_DATA_HP) == 0 || sp == SPECIES_EGG)
                col = sColMenuGray;
            AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 12, 1 + row * 12, col, TEXT_SKIP_DRAW, gStringVar1);
            if (sp != SPECIES_EGG)
            {
                ConvertIntToDecimalStringN(gStringVar1, GetMonData(mon, MON_DATA_HP), STR_CONV_MODE_RIGHT_ALIGN, 3);
                ConvertIntToDecimalStringN(gStringVar2, GetMonData(mon, MON_DATA_MAX_HP), STR_CONV_MODE_LEFT_ALIGN, 3);
                StringExpandPlaceholders(gStringVar4, sText_HpFmt);
                AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 62, 1 + row * 12, col, TEXT_SKIP_DRAW, gStringVar4);
            }
        }
        AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 4, 1 + gRt->menuCursor * 12, sColMenuArrow, TEXT_SKIP_DRAW, (const u8[]){CHAR_RIGHT_ARROW, EOS});
        break;
    case MENU_LEARN:
        {
            struct Pokemon *mon = &gPlayerParty[gRt->pendingLearnMons[0]];
            for (row = 0; row < MAX_MON_MOVES; row++)
                AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 14, 3 + row * 13, sColMenu, TEXT_SKIP_DRAW, gMoveNames[GetMonData(mon, MON_DATA_MOVE1 + row)]);
            AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 14, 3 + 4 * 13, sColMenuRed, TEXT_SKIP_DRAW, sText_DontLearn);
            AddTextPrinterParameterized3(WIN_MENU, FONT_SMALL, 5, 3 + gRt->menuCursor * 13, sColMenuArrow, TEXT_SKIP_DRAW, (const u8[]){CHAR_RIGHT_ARROW, EOS});
        }
        break;
    }
    PutWindowTilemap(WIN_MENU);
    CopyWindowToVram(WIN_MENU, COPYWIN_FULL);
}

static void CloseMenu(void)
{
    ClearWindowTilemap(WIN_MENU);
    CopyWindowToVram(WIN_MENU, COPYWIN_MAP);
    if (sMsgActive)
        RtUi_HideMessageBox();
}

void RtUi_OpenPauseMenu(void)
{
    struct RtActor *p = Rt_PlayerActor();
    p->aiming = 0;
    gRt->menuKind = MENU_MAIN;
    gRt->menuCursor = 0;
    gRt->menuScroll = 0;
    PlaySE(SE_SELECT);
    DrawMenu();
}

void RtUi_OpenPartyForced(void)
{
    u8 i;
    gRt->menuKind = MENU_PARTY_FORCED;
    gRt->menuCursor = 0;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (CanSwitchTo(i, FALSE))
        {
            gRt->menuCursor = i;
            break;
        }
    }
    RtUi_ShowMessageBox(sText_ChooseNext);
    DrawMenu();
}

void RtUi_BatonPass(struct RtActor *a)
{
    if (a->side == RT_SIDE_PLAYER && a->id == gRt->playerLead)
    {
        if (Rt_CountUsableParty(RT_SIDE_PLAYER) > 1)
        {
            memcpy(sBatonStages, a->stages, sizeof(sBatonStages));
            sBatonPending = TRUE;
            gRt->switchRequest = TRUE;
        }
        return;
    }
    if (a->side == RT_SIDE_ENEMY && gRt->isTrainer)
    {
        s8 stages[STG_COUNT];
        u8 before = a->partyIdx;
        memcpy(stages, a->stages, sizeof(stages));
        RtActor_Withdraw(a);
        Rt_EnemySendNext(a);
        if (a->partyIdx != before)
            memcpy(a->stages, stages, sizeof(stages));
        else
            RtActor_SendOut(a, before, TRUE);
    }
}

static bool8 CanSwitchTo(u8 idx, bool8 msg)
{
    struct Pokemon *mon = &gPlayerParty[idx];
    s32 i;
    if (gRt->partner && idx >= 3)
        return FALSE;
    if (GetMonData(mon, MON_DATA_SPECIES_OR_EGG) == SPECIES_NONE || GetMonData(mon, MON_DATA_SPECIES_OR_EGG) == SPECIES_EGG)
        return FALSE;
    if (GetMonData(mon, MON_DATA_HP) == 0)
    {
        if (msg)
        {
            GetMonData(mon, MON_DATA_NICKNAME, gStringVar1);
            StringGet_Nickname(gStringVar1);
            RtUi_ToastFmt(sText_NoEnergy, 60);
        }
        return FALSE;
    }
    for (i = 0; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *a = &gRt->actors[i];
        if (a->active && a->side == RT_SIDE_PLAYER && a->partyIdx == idx && a->state != AST_GONE)
        {
            if (msg)
            {
                GetMonData(mon, MON_DATA_NICKNAME, gStringVar1);
                StringGet_Nickname(gStringVar1);
                RtUi_ToastFmt(sText_AlreadyOut, 60);
            }
            return FALSE;
        }
    }
    return TRUE;
}

static bool8 IsTrapped(struct RtActor *p)
{
    s32 i;
    if (p->holdEffect == HOLD_EFFECT_CAN_ALWAYS_RUN)
        return FALSE;
    if (p->meanLook || p->trap || p->ingrain)
        return TRUE;
    for (i = 2; i < RT_MAX_ACTORS; i++)
    {
        struct RtActor *f = &gRt->actors[i];
        if (!f->active || f->state == AST_GONE || !RtActor_IsAlive(f))
            continue;
        if (f->ability == ABILITY_SHADOW_TAG
         || (f->ability == ABILITY_ARENA_TRAP && !RtActor_HasType(p, TYPE_FLYING) && p->ability != ABILITY_LEVITATE)
         || (f->ability == ABILITY_MAGNET_PULL && RtActor_HasType(p, TYPE_STEEL)))
            return TRUE;
    }
    return FALSE;
}

// returns TRUE if consumed
static bool8 UseItemOn(u16 item, u8 partyIdx)
{
    struct Pokemon *mon = &gPlayerParty[partyIdx];
    struct RtActor *a = NULL;
    s32 i;
    for (i = 0; i < RT_MAX_ACTORS; i++)
        if (gRt->actors[i].active && gRt->actors[i].side == RT_SIDE_PLAYER && gRt->actors[i].partyIdx == partyIdx && gRt->actors[i].state != AST_GONE)
            a = &gRt->actors[i];
    if (a)
        RtActor_SyncToMon(a);
    // PP restoring items are meaningless with cooldowns: they reset all cooldowns instead
    if (GetItemEffectType(item) == ITEM_EFFECT_HEAL_PP)
    {
        if (!a)
            return FALSE;
        for (i = 0; i < MAX_MON_MOVES; i++)
            a->cd[i] = 0;
        PlaySE(SE_USE_ITEM);
        return TRUE;
    }
    if (ExecuteTableBasedItemEffect(mon, item, partyIdx, 0))
    {
        // confusion and infatuation live on the real-time actor, not in gBattleMons, so the item table
        // cannot see them: cure them here (Yellow/Red Flute, Persim/Lum Berry, Full Heal, Full Restore...)
        bool8 curesConfusion = item == ITEM_YELLOW_FLUTE || item == ITEM_PERSIM_BERRY || item == ITEM_LUM_BERRY
                            || item == ITEM_FULL_HEAL || item == ITEM_FULL_RESTORE || item == ITEM_HEAL_POWDER || item == ITEM_LAVA_COOKIE;
        if (a && curesConfusion && a->confuse)
        {
            a->confuse = 0;
            RtFx_Burst(FX_PLUS8, FXP_HEAL, a->x, a->y - TO_FX(a->height / 2), 5, 1, 30);
            PlaySE(SE_USE_ITEM);
            gRt->hudDirty = TRUE;
            gBattleResults.numHealingItemsUsed++;
            return TRUE;
        }
        if (a && item == ITEM_RED_FLUTE && a->attract)
        {
            a->attract = 0;
            RtFx_Burst(FX_PLUS8, FXP_HEAL, a->x, a->y - TO_FX(a->height / 2), 5, 1, 30);
            PlaySE(SE_USE_ITEM);
            gRt->hudDirty = TRUE;
            gBattleResults.numHealingItemsUsed++;
            return TRUE;
        }
        RtUi_Toast(sText_NoEffect, 60);
        return FALSE;
    }
    PlaySE(SE_USE_ITEM);
    if (a)
    {
        u16 newHp = GetMonData(mon, MON_DATA_HP);
        u32 st = GetMonData(mon, MON_DATA_STATUS);
        if (newHp > a->hp)
        {
            RtFx_Number(a->x, a->y - TO_FX(a->height + 4), newHp - a->hp, 2);
            RtFx_Burst(FX_PLUS8, FXP_HEAL, a->x, a->y - TO_FX(a->height / 2), 5, 1, 30);
        }
        a->hp = newHp;
        a->hpBarDirty = TRUE;
        if (st == 0 && a->mstatus)
            RtActor_CureStatus(a);
        if (item == ITEM_FULL_HEAL || item == ITEM_FULL_RESTORE || item == ITEM_PERSIM_BERRY || item == ITEM_LUM_BERRY || item == ITEM_HEAL_POWDER || item == ITEM_LAVA_COOKIE)
            a->confuse = 0;
    }
    else if (GetMonData(mon, MON_DATA_HP) && gRt->actors[gRt->playerLead].state == AST_GONE)
    {
        // revived the fainted lead's replacement candidate: nothing else to do
    }
    gRt->hudDirty = TRUE;
    if (item == ITEM_REVIVE || item == ITEM_MAX_REVIVE || item == ITEM_REVIVAL_HERB)
        gBattleResults.numRevivesUsed++;
    else
        gBattleResults.numHealingItemsUsed++;
    return TRUE;
}

static bool8 UseItemDirect(u16 item)
{
    struct RtActor *p = Rt_PlayerActor();
    switch (item)
    {
    case ITEM_X_ATTACK: RtActor_ChangeStage(p, STG_ATK, 1, p->id, TRUE); return TRUE;
    case ITEM_X_DEFEND: RtActor_ChangeStage(p, STG_DEF, 1, p->id, TRUE); return TRUE;
    case ITEM_X_SPEED: RtActor_ChangeStage(p, STG_SPE, 1, p->id, TRUE); return TRUE;
    case ITEM_X_SPECIAL: RtActor_ChangeStage(p, STG_SPA, 1, p->id, TRUE); return TRUE;
    case ITEM_X_ACCURACY: RtActor_ChangeStage(p, STG_ACC, 1, p->id, TRUE); return TRUE;
    case ITEM_DIRE_HIT: p->focusEnergy = 1; RtFx_StatArrows(p, TRUE); return TRUE;
    case ITEM_GUARD_SPEC: gRt->sides[RT_SIDE_PLAYER].mist = 1200; RtFx_Spawn(FX_SMOKE16, FXP_WHITE, p->x, p->y, 30); return TRUE;
    case ITEM_POKE_DOLL:
    case ITEM_FLUFFY_TAIL:
        if (!gRt->isTrainer)
        {
            Rt_EndBattle(B_OUTCOME_RAN);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

static void SelectBagItem(u16 item)
{
    u8 pocket = GetItemPocket(item);
    struct RtActor *p = Rt_PlayerActor();
    if (pocket == POCKET_POKE_BALLS)
    {
        if (gRt->isTrainer)
        {
            RemoveItem(item);
            CloseMenu();
            RtUi_Toast(sText_BallBlocked, 90);
            gRt->menuKind = MENU_MESSAGE;
            return;
        }
        gRt->ballItem = item;
        gRt->ballReady = TRUE;
        CloseMenu();
        RtUi_Toast(sText_BallReady, 70);
        RtUi_RefreshMoves();
        gRt->menuKind = MENU_MESSAGE;
        return;
    }
    if (UseItemDirect(item))
    {
        RemoveItem(item);
        CloseMenu();
        gRt->menuKind = MENU_MESSAGE;
        PlaySE(SE_USE_ITEM);
        return;
    }
    // PP restoring items reset the cooldowns of the active mon directly
    if (GetItemEffectType(item) == ITEM_EFFECT_HEAL_PP)
    {
        if (UseItemOn(item, p->partyIdx))
        {
            RemoveItem(item);
            CloseMenu();
            gRt->menuKind = MENU_MESSAGE;
        }
        return;
    }
    // medicine needs a target
    gRt->menuItems[39] = item;
    gRt->menuKind = MENU_ITEM_TARGET;
    gRt->menuCursor = Rt_PlayerActor()->partyIdx;
    DrawMenu();
}

// simple YES/NO box in the menu window
static EWRAM_DATA u8 sYesNoCursor = 0;
static const u8 sText_Yes[] = _("YES");
static const u8 sText_No[] = _("NO");

static void DrawYesNo(void)
{
    FillWindowPixelBuffer(WIN_MENU, PIXEL_FILL(0));
    FillWindowPixelRect(WIN_MENU, PIXEL_FILL(4), 56, 32, 56, 40);
    FillWindowPixelRect(WIN_MENU, PIXEL_FILL(1), 58, 34, 52, 36);
    AddTextPrinterParameterized3(WIN_MENU, FONT_NORMAL, 74, 36, sColMenu, TEXT_SKIP_DRAW, sText_Yes);
    AddTextPrinterParameterized3(WIN_MENU, FONT_NORMAL, 74, 52, sColMenu, TEXT_SKIP_DRAW, sText_No);
    AddTextPrinterParameterized3(WIN_MENU, FONT_NORMAL, 64, 36 + sYesNoCursor * 16, sColMenuArrow, TEXT_SKIP_DRAW, (const u8[]){CHAR_RIGHT_ARROW, EOS});
    PutWindowTilemap(WIN_MENU);
    CopyWindowToVram(WIN_MENU, COPYWIN_FULL);
}

void RtUi_OpenYesNo(void)
{
    sYesNoCursor = 0;
    DrawYesNo();
}

static void OpenYesNoAt(u8 cursor)
{
    sYesNoCursor = cursor;
    DrawYesNo();
}

// 0 = pending, 1 = yes, 2 = no
u8 RtUi_UpdateYesNo(void)
{
    if (IsTextPrinterActive(WIN_MSG))
        return 0;
    if (gMain.newKeys & (DPAD_UP | DPAD_DOWN))
    {
        sYesNoCursor ^= 1;
        PlaySE(SE_SELECT);
        DrawYesNo();
    }
    else if (gMain.newKeys & A_BUTTON)
    {
        PlaySE(SE_SELECT);
        ClearWindowTilemap(WIN_MENU);
        CopyWindowToVram(WIN_MENU, COPYWIN_MAP);
        return sYesNoCursor ? 2 : 1;
    }
    else if (gMain.newKeys & B_BUTTON)
    {
        PlaySE(SE_SELECT);
        ClearWindowTilemap(WIN_MENU);
        CopyWindowToVram(WIN_MENU, COPYWIN_MAP);
        return 2;
    }
    return 0;
}

// learn-move prompt guards: it can pop up mid-fight while the move buttons are being mashed,
// so input is ignored for a moment, A/B must be released first, and every choice is confirmed
static EWRAM_DATA u8 sLearnLock = 0;
static EWRAM_DATA bool8 sLearnNeedRelease = FALSE;
static EWRAM_DATA bool8 sLearnConfirm = FALSE;
static EWRAM_DATA u8 sLearnChoice = 0;

void RtUi_OpenLearnPrompt(void)
{
    sLearnLock = 40;
    sLearnNeedRelease = TRUE;
    sLearnConfirm = FALSE;
    GetMonData(&gPlayerParty[gRt->pendingLearnMons[0]], MON_DATA_NICKNAME, gStringVar1);
    StringGet_Nickname(gStringVar1);
    StringCopy(gStringVar2, gMoveNames[gRt->pendingLearnMoves[0]]);
    RtUi_ShowMessageBox(sText_LearnPrompt);
    gRt->menuKind = MENU_LEARN;
    gRt->menuCursor = 0;
    DrawMenu();
}

static void PopLearn(void)
{
    u8 i;
    for (i = 1; i < gRt->pendingLearnCount; i++)
    {
        gRt->pendingLearnMoves[i - 1] = gRt->pendingLearnMoves[i];
        gRt->pendingLearnMons[i - 1] = gRt->pendingLearnMons[i];
    }
    gRt->pendingLearnCount--;
}

static bool8 LearnApply(u8 choice);

static void LearnNamesToVars(void)
{
    struct Pokemon *mon = &gPlayerParty[gRt->pendingLearnMons[0]];
    GetMonData(mon, MON_DATA_NICKNAME, gStringVar1);
    StringGet_Nickname(gStringVar1);
    StringCopy(gStringVar2, gMoveNames[gRt->pendingLearnMoves[0]]);
    if (sLearnChoice < MAX_MON_MOVES)
        StringCopy(gStringVar3, gMoveNames[GetMonData(mon, MON_DATA_MOVE1 + sLearnChoice)]);
}

bool8 RtUi_UpdatePrompt(void)
{
    if (sLearnLock)
    {
        sLearnLock--;
        return FALSE;
    }
    if (sLearnNeedRelease)
    {
        if (gMain.heldKeys & (A_BUTTON | B_BUTTON))
            return FALSE;
        sLearnNeedRelease = FALSE;
    }
    if (sLearnConfirm)
    {
        u8 r = RtUi_UpdateYesNo();
        if (r == 0)
            return FALSE;
        sLearnConfirm = FALSE;
        if (r == 2)
        {
            // back to the move list, cursor where it was
            u8 cursor = gRt->menuCursor;
            RtUi_OpenLearnPrompt();
            sLearnLock = 8;
            gRt->menuCursor = cursor;
            DrawMenu();
            return FALSE;
        }
        return LearnApply(sLearnChoice);
    }
    if (gMain.newKeys & DPAD_UP)
    {
        gRt->menuCursor = gRt->menuCursor ? gRt->menuCursor - 1 : 4;
        PlaySE(SE_SELECT);
        DrawMenu();
    }
    else if (gMain.newKeys & DPAD_DOWN)
    {
        gRt->menuCursor = gRt->menuCursor < 4 ? gRt->menuCursor + 1 : 0;
        PlaySE(SE_SELECT);
        DrawMenu();
    }
    else if (gMain.newKeys & (A_BUTTON | B_BUTTON))
    {
        PlaySE(SE_SELECT);
        sLearnChoice = (gMain.newKeys & B_BUTTON) ? MAX_MON_MOVES : gRt->menuCursor;
        LearnNamesToVars();
        RtUi_ShowMessageBox(sLearnChoice < MAX_MON_MOVES ? sText_ConfirmForget : sText_ConfirmGiveUp);
        OpenYesNoAt(1); // NO preselected: a stray press never changes the moves
        sLearnConfirm = TRUE;
        sLearnNeedRelease = TRUE;
        sLearnLock = 12;
    }
    return FALSE;
}

static bool8 LearnApply(u8 choice)
{
    {
        u8 monIdx = gRt->pendingLearnMons[0];
        u16 move = gRt->pendingLearnMoves[0];
        struct Pokemon *mon = &gPlayerParty[monIdx];
        GetMonData(mon, MON_DATA_NICKNAME, gStringVar1);
        StringGet_Nickname(gStringVar1);
        StringCopy(gStringVar2, gMoveNames[move]);
        if (choice < MAX_MON_MOVES)
        {
            s32 i;
            StringCopy(gStringVar3, gMoveNames[GetMonData(mon, MON_DATA_MOVE1 + choice)]);
            {
                // keep PP bonuses consistent for that slot
                u8 bonuses = GetMonData(mon, MON_DATA_PP_BONUSES);
                bonuses &= gPPUpClearMask[choice];
                SetMonData(mon, MON_DATA_PP_BONUSES, &bonuses);
            }
            SetMonMoveSlot(mon, move, choice);
            RtUi_ToastFmt(sText_ForgotLearned, 90);
            for (i = 0; i < RT_MAX_ACTORS; i++)
            {
                struct RtActor *a = &gRt->actors[i];
                if (a->active && a->side == RT_SIDE_PLAYER && a->partyIdx == monIdx)
                {
                    a->moves[choice] = move;
                    a->cd[choice] = 0;
                    a->cdMax[choice] = RtMove_Cooldown(move, GetMonData(mon, MON_DATA_PP_BONUSES), choice);
                }
            }
            RtUi_RefreshMoves();
        }
        else
        {
            RtUi_ToastFmt(sText_DidNotLearn, 70);
        }
        PopLearn();
        if (gRt->pendingLearnCount)
        {
            RtUi_OpenLearnPrompt();
            return FALSE;
        }
        CloseMenu();
        return TRUE;
    }
    return FALSE;
}

// returns TRUE when the menu closes
bool8 RtUi_UpdateMenu(void)
{
    u8 max;
    if (gRt->menuKind == MENU_MESSAGE)
        return TRUE;
    switch (gRt->menuKind)
    {
    case MENU_MAIN: max = 4; break;
    case MENU_BAG: max = gRt->menuCount; break;
    default: max = PARTY_SIZE; break;
    }
    if (gMain.newKeys & DPAD_UP)
    {
        if (max)
            gRt->menuCursor = gRt->menuCursor ? gRt->menuCursor - 1 : max - 1;
        PlaySE(SE_SELECT);
    }
    else if (gMain.newKeys & DPAD_DOWN)
    {
        if (max)
            gRt->menuCursor = gRt->menuCursor + 1 < max ? gRt->menuCursor + 1 : 0;
        PlaySE(SE_SELECT);
    }
    else if (gMain.newKeys & B_BUTTON || (gRt->menuKind == MENU_MAIN && (gMain.newKeys & START_BUTTON)))
    {
        PlaySE(SE_SELECT);
        if (gRt->menuKind == MENU_MAIN)
        {
            CloseMenu();
            return TRUE;
        }
        if (gRt->menuKind == MENU_PARTY_FORCED)
            return FALSE;
        gRt->menuKind = (gRt->menuKind == MENU_ITEM_TARGET) ? MENU_BAG : MENU_MAIN;
        gRt->menuCursor = 0;
        gRt->menuScroll = 0;
        if (sMsgActive)
            RtUi_HideMessageBox();
        DrawMenu();
        return FALSE;
    }
    else if (gMain.newKeys & A_BUTTON)
    {
        PlaySE(SE_SELECT);
        switch (gRt->menuKind)
        {
        case MENU_MAIN:
            switch (gRt->menuCursor)
            {
            case 0:
                CloseMenu();
                return TRUE;
            case 1:
                if (gBattleTypeFlags & (BATTLE_TYPE_FIRST_BATTLE | BATTLE_TYPE_FRONTIER_NO_PYRAMID))
                    return FALSE;
                BuildBagList();
                gRt->menuKind = MENU_BAG;
                gRt->menuCursor = 0;
                gRt->menuScroll = 0;
                break;
            case 2:
                gRt->menuKind = MENU_PARTY;
                gRt->menuCursor = Rt_PlayerActor()->partyIdx;
                RtUi_ShowMessageBox(sText_ChooseMon);
                break;
            case 3:
                CloseMenu();
                Rt_TryRun();
                return TRUE;
            }
            break;
        case MENU_BAG:
            if (gRt->menuCount)
            {
                SelectBagItem(gRt->menuItems[gRt->menuCursor]);
                if (gRt->menuKind == MENU_MESSAGE)
                    return TRUE;
                return FALSE;
            }
            break;
        case MENU_ITEM_TARGET:
            {
                u16 item = gRt->menuItems[39];
                if (GetMonData(&gPlayerParty[gRt->menuCursor], MON_DATA_SPECIES_OR_EGG) == SPECIES_NONE)
                    return FALSE;
                if (UseItemOn(item, gRt->menuCursor))
                {
                    RemoveItem(item);
                    GetMonData(&gPlayerParty[gRt->menuCursor], MON_DATA_NICKNAME, gStringVar1);
                    StringGet_Nickname(gStringVar1);
                    CopyItemName(item, gStringVar2);
                    CloseMenu();
                    RtUi_ToastFmt(sText_ItemUsed, 70);
                    // using an item costs a moment: brief global cooldown
                    Rt_PlayerActor()->gcd = 30;
                    return TRUE;
                }
            }
            break;
        case MENU_PARTY:
        case MENU_PARTY_FORCED:
            {
                struct RtActor *p = Rt_PlayerActor();
                if (gRt->menuKind == MENU_PARTY && IsTrapped(p) && p->state != AST_GONE && RtActor_IsAlive(p))
                {
                    GetMonData(p->mon, MON_DATA_NICKNAME, gStringVar1);
                    StringGet_Nickname(gStringVar1);
                    RtUi_ToastFmt(sText_NoSwitch, 60);
                    return FALSE;
                }
                if (!CanSwitchTo(gRt->menuCursor, TRUE))
                    return FALSE;
                CloseMenu();
                {
                    bool8 baton = sBatonPending;
                    sBatonPending = FALSE;
                    Rt_PlayerSwitchTo(gRt->menuCursor);
                    if (baton)
                        memcpy(Rt_PlayerActor()->stages, sBatonStages, sizeof(sBatonStages));
                }
                return TRUE;
            }
        }
        DrawMenu();
        return FALSE;
    }
    else
    {
        return FALSE;
    }
    // scroll handling for bag
    if (gRt->menuKind == MENU_BAG)
    {
        if (gRt->menuCursor < gRt->menuScroll)
            gRt->menuScroll = gRt->menuCursor;
        if (gRt->menuCursor >= gRt->menuScroll + MENU_ROWS)
            gRt->menuScroll = gRt->menuCursor - MENU_ROWS + 1;
    }
    DrawMenu();
    return FALSE;
}

// ---------------------------------------------------------------------------
// Poké Balls
// ---------------------------------------------------------------------------
static u16 BallMultiplier(u16 ball, struct RtActor *t)
{
    switch (ball)
    {
    case ITEM_GREAT_BALL: return 15;
    case ITEM_ULTRA_BALL: return 20;
    case ITEM_SAFARI_BALL: return 15;
    case ITEM_NET_BALL: return (RtActor_HasType(t, TYPE_WATER) || RtActor_HasType(t, TYPE_BUG)) ? 30 : 10;
    case ITEM_DIVE_BALL: return (gBattleEnvironment == BATTLE_ENVIRONMENT_UNDERWATER) ? 35 : 10;
    case ITEM_NEST_BALL:
        if (t->level < 40)
        {
            u16 m = 40 - t->level;
            return m < 10 ? 10 : m;
        }
        return 10;
    case ITEM_REPEAT_BALL:
        return GetSetPokedexFlag(SpeciesToNationalPokedexNum(t->species), FLAG_GET_CAUGHT) ? 30 : 10;
    case ITEM_TIMER_BALL:
        {
            u32 m = 10 + gRt->frame / 600;
            return m > 40 ? 40 : m;
        }
    }
    return 10;
}

static u8 CountShakes(u16 ball, struct RtActor *t)
{
    u32 a, b;
    u8 shakes;
    u16 rate = gSpeciesInfo[t->species].catchRate;
    if (ball == ITEM_MASTER_BALL)
        return 4;
    if (gRt->isSafari)
        rate = rate * (gRt->safariCatchMod ? gRt->safariCatchMod : 10) / 10;
    a = (3 * t->maxHp - 2 * t->hp) * rate * BallMultiplier(ball, t) / 10 / (3 * t->maxHp);
    if (t->mstatus == MST_SLP || t->mstatus == MST_FRZ)
        a *= 2;
    else if (t->mstatus)
        a = a * 15 / 10;
    if (a >= 255)
        return 4;
    if (a == 0)
        a = 1;
    b = 16711680 / a;
    b = Sqrt(b);
    b = Sqrt(b);
    b = 1048560 / b;
    // catch chance = all four shake checks; every ball gets +10% of it, and never less than 2%;
    // every ball that already broke free this battle adds +1% (a real-time throw costs far more
    // than a turn, so a 1 HP legend at 2% a ball could eat hundreds of them)
    {
        u32 b2 = (b * b) >> 16;
        u32 p = (b2 * b2) >> 16;           // Q16 probability of the original formula
        p = p * 110 / 100;
        if (p < 65536 * 2 / 100)
            p = 65536 * 2 / 100;
        p += (u32)gRt->ballFails * 65536 / 100;
        if (p >= 65536 || Random() < p)
            return 4;
    }
    // failed: how many times it wobbled before breaking free (for the animation)
    for (shakes = 0; shakes < 3 && Random() < b; shakes++)
        ;
    return shakes;
}

u8 RtUi_DebugCountShakes(u16 ball, struct RtActor *t)
{
    return CountShakes(ball, t);
}

void RtMove_ThrowBall(struct RtActor *a, u16 ball, u8 angle)
{
    struct RtProj *p = RtProj_Alloc();
    s32 sp = 3 * 256;
    if (!p)
        return;
    if (!gRt->isSafari)
    {
        if (GetBagQty(ball) == 0)
            return;
        RemoveItem(ball);
    }
    p->kind = PK_BALL;
    p->owner = a ? a->id : 0;
    p->side = RT_SIDE_PLAYER;
    p->param = ball;
    if (a)
    {
        p->x = a->x;
        p->y = a->y;
        p->vx = RtCos(angle) * sp / 256;
        p->vy = RtSin(angle) * sp / 256;
        p->life = 50;
    }
    else
    {
        // safari: from the bottom of the screen to the reticle
        s32 dx, dy, dist;
        p->x = TO_FX(gRt->camX + DISPLAY_WIDTH / 2);
        p->y = TO_FX(gRt->camY + DISPLAY_HEIGHT - 36);
        dx = FROM_FX(sReticleX - p->x);
        dy = FROM_FX(sReticleY - p->y);
        dist = RtDist(dx, dy);
        if (dist == 0) dist = 1;
        p->vx = dx * sp / dist;
        p->vy = dy * sp / dist;
        p->life = dist * 256 / sp + 1;
    }
    p->len = p->life;
    p->radius = 8;
    p->frame = 0;
    PlaySE(SE_BALL_THROW);
}

void RtMove_UpdateBall(struct RtProj *p)
{
    struct RtActor *t = &gRt->actors[2];
    p->age++;
    switch (p->param2)
    {
    case 0: // flying
        {
            s32 i;
            s32 tt = p->age * 256 / (p->len ? p->len : 1);
            if (!gRt->isSafari)
            {
                // the ball curves toward the closest wild POKeMON in front of it (moving targets
                // were dodging most straight throws)
                s32 best = 0x7FFFFFFF;
                struct RtActor *tgt = NULL;
                u8 cur = RtAtan2(p->vx, p->vy);
                for (i = 2; i < RT_MAX_ACTORS; i++)
                {
                    struct RtActor *e = &gRt->actors[i];
                    s32 dx, dy, d;
                    if (!e->active || e->state == AST_GONE || !RtActor_IsAlive(e) || e->invuln)
                        continue;
                    dx = FROM_FX(e->x - p->x);
                    dy = FROM_FX(e->y - TO_FX(e->height / 3) - p->y);
                    d = dx * dx + dy * dy;
                    if (d < best && (u8)(RtAtan2(dx, dy) - cur + 48) <= 96)
                    {
                        best = d;
                        tgt = e;
                    }
                }
                if (tgt)
                {
                    u8 want = RtAtan2(FROM_FX(tgt->x - p->x), FROM_FX(tgt->y - TO_FX(tgt->height / 3) - p->y));
                    s8 diff = (s8)(want - cur);
                    if (diff > 4) diff = 4;
                    if (diff < -4) diff = -4;
                    cur += diff;
                    p->vx = RtCos(cur) * 3;
                    p->vy = RtSin(cur) * 3;
                }
            }
            p->x += p->vx;
            p->y += p->vy;
            p->z = 4 + tt * (256 - tt) * 30 / 65536;
            p->frame = 0;
            for (i = 2; i < RT_MAX_ACTORS; i++)
            {
                struct RtActor *e = &gRt->actors[i];
                s32 dx, dy, r;
                if (!e->active || e->state == AST_GONE || !RtActor_IsAlive(e) || e->invuln)
                    continue;
                dx = FROM_FX(e->x - p->x);
                dy = FROM_FX(e->y - TO_FX(e->height / 3) - p->y);
                r = e->radius + p->radius;
                if (dx * dx + dy * dy <= r * r)
                {
                    // captured into the ball
                    gRt->actors[2] = gRt->actors[2]; // keep layout
                    p->homingTarget = i;
                    p->x = e->x;
                    p->y = e->y - TO_FX(4);
                    p->z = 10;
                    e->state = AST_CAUGHT;
                    e->hidden = TRUE;
                    e->act.move = MOVE_NONE;
                    RtFx_Burst(FX_GLOW16, FXP_WHITE, e->x, e->y - TO_FX(e->height / 2), 6, 2, 16);
                    PlaySE(SE_BALL_OPEN);
                    p->param2 = 1;
                    p->age = 0;
                    p->delay = CountShakes(p->param, e);
                    if (p->param < ITEM_ULTRA_BALL + 12 && p->param >= ITEM_MASTER_BALL && p->param != ITEM_MASTER_BALL)
                    {
                        u8 bi = ItemIdToBallId(p->param);
                        if (bi && bi - 1 < ARRAY_COUNT(gBattleResults.catchAttempts) && gBattleResults.catchAttempts[bi - 1] < 255)
                            gBattleResults.catchAttempts[bi - 1]++;
                    }
                    if (p->param == ITEM_MASTER_BALL)
                        gBattleResults.usedMasterBall = TRUE;
                    return;
                }
            }
            if (p->life)
                p->life--;
            if (p->life == 0)
            {
                // missed: bounce away
                RtFx_Burst(FX_SPARK8, FXP_WHITE, p->x, p->y, 3, 2, 12);
                p->kind = PK_NONE;
            }
        }
        break;
    case 1: // drop + shakes
        {
            struct RtActor *e = &gRt->actors[p->homingTarget];
            u8 shakeIdx = p->age / 36;
            (void)t;
            if (p->z > 0 && p->age < 12)
                p->z--;
            if (p->age < 30)
            {
                p->frame = 0;
                break;
            }
            if (shakeIdx <= 3)
            {
                u8 k = (p->age % 36);
                if (shakeIdx > p->delay || (shakeIdx == 3 && p->delay < 4))
                {
                    // break free
                    e->state = AST_NORMAL;
                    e->hidden = FALSE;
                    e->flash = 10;
                    e->iframes = 30;
                    RtFx_Burst(FX_GLOW16, FXP_WHITE, e->x, e->y - TO_FX(e->height / 2), 6, 2, 16);
                    PlaySE(SE_BALL_OPEN);
                    RtUi_Toast(p->delay >= 2 ? sText_AlmostHad : sText_BrokeFree, 70);
                    if (gRt->ballFails < 255)
                        gRt->ballFails++;
                    p->kind = PK_NONE;
                    break;
                }
                if (k == 4)
                    PlaySE(SE_BALL);
                p->frame = (k < 8) ? 2 : (k < 16 ? 3 : 0);
                if (shakeIdx == 3 && k > 30 && p->delay >= 4)
                {
                    // caught!
                    PlaySE(SE_BALL_TRADE);
                    RtFx_Burst(FX_STAR16, FXP_GOLD, p->x, p->y - TO_FX(p->z), 6, 2, 30);
                    if (e->id != 2)
                    {
                        // move caught mon into slot 2 for the outro logic
                        struct RtActor tmp = gRt->actors[2];
                        gRt->actors[2] = *e;
                        *e = tmp;
                    }
                    p->kind = PK_NONE;
                    Rt_HandleCaught();
                }
            }
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Safari Zone
// ---------------------------------------------------------------------------
void RtMove_UpdateSafariThrow(struct RtProj *p)
{
    struct RtActor *e = &gRt->actors[2];
    s32 tt = p->age * 256 / (p->len ? p->len : 1);
    p->age++;
    p->x += p->vx;
    p->y += p->vy;
    p->z = 4 + tt * (256 - tt) * 24 / 65536;
    if (p->life)
        p->life--;
    if (p->life == 0)
    {
        s32 dist = RtDist(FROM_FX(e->x - p->x), FROM_FX(e->y - p->y));
        GetMonData(e->mon, MON_DATA_NICKNAME, gStringVar1);
        StringGet_Nickname(gStringVar1);
        if (p->param == 1)
        {
            // pokeblock: lower flee chance if close enough
            RtFx_Burst(FX_HEART16, FXP_HEART, p->x, p->y, 4, 1, 24);
            if (dist < 40)
            {
                gRt->safariFleeMod = gRt->safariFleeMod > 2 ? gRt->safariFleeMod - 2 : 1;
                gRt->safariCatchMod = gRt->safariCatchMod > 5 ? gRt->safariCatchMod - 2 : gRt->safariCatchMod;
                RtUi_ToastFmt(sText_Eating, 60);
            }
        }
        p->kind = PK_NONE;
    }
}

void RtUi_SafariInput(void)
{
    struct RtActor *e = &gRt->actors[2];
    u16 held = gMain.heldKeysRaw, pressed = gMain.newKeysRaw;
    s32 sp = 3 * 256;
    if (held & DPAD_LEFT) sReticleX -= sp;
    if (held & DPAD_RIGHT) sReticleX += sp;
    if (held & DPAD_UP) sReticleY -= sp;
    if (held & DPAD_DOWN) sReticleY += sp;
    if (sReticleX < TO_FX(24)) sReticleX = TO_FX(24);
    if (sReticleX > TO_FX(ARENA_W - 24)) sReticleX = TO_FX(ARENA_W - 24);
    if (sReticleY < TO_FX(24)) sReticleY = TO_FX(24);
    if (sReticleY > TO_FX(ARENA_H - 24)) sReticleY = TO_FX(ARENA_H - 24);
    if (gRt->safariCatchMod == 0)
    {
        gRt->safariCatchMod = 10;
        gRt->safariFleeMod = 10;
    }
    // camera follows the reticle
    {
        s32 tx = FROM_FX(sReticleX) - DISPLAY_WIDTH / 2, ty = FROM_FX(sReticleY) - DISPLAY_HEIGHT / 2;
        RtArena_ClampCamera(&tx, &ty);
        gRt->camX += (tx - gRt->camX) / 6;
        gRt->camY += (ty - gRt->camY) / 6;
    }
    if (e->state == AST_CAUGHT)
        return;
    if (pressed & A_BUTTON)
    {
        bool8 busy = FALSE;
        s32 i;
        for (i = 0; i < RT_MAX_PROJ; i++)
            if (gRt->proj[i].kind == PK_BALL)
                busy = TRUE;
        if (!busy && gRt->safariBalls)
        {
            gRt->safariBalls--;
            gRt->ballItem = ITEM_SAFARI_BALL;
            RtMove_ThrowBall(NULL, ITEM_SAFARI_BALL, 0);
            gRt->hudDirty = TRUE;
        }
    }
    else if (pressed & B_BUTTON)
    {
        struct RtProj *p = RtProj_Alloc();
        if (p)
        {
            s32 dx, dy, dist;
            p->kind = PK_SAFARI;
            p->param = 1;
            p->shape = FX_HEART16;
            p->pal = FXP_HEART;
            p->x = TO_FX(gRt->camX + DISPLAY_WIDTH / 2);
            p->y = TO_FX(gRt->camY + DISPLAY_HEIGHT - 36);
            dx = FROM_FX(sReticleX - p->x);
            dy = FROM_FX(sReticleY - p->y);
            dist = RtDist(dx, dy);
            if (dist == 0) dist = 1;
            p->vx = dx * 640 / dist;
            p->vy = dy * 640 / dist;
            p->life = dist * 256 / 640 + 1;
            p->len = p->life;
            PlaySE(SE_BALL_THROW);
        }
    }
    else if (pressed & L_BUTTON)
    {
        // go near: easier to catch, more likely to flee
        gRt->safariCatchMod = gRt->safariCatchMod < 30 ? gRt->safariCatchMod + 5 : 30;
        gRt->safariFleeMod = gRt->safariFleeMod < 30 ? gRt->safariFleeMod + 5 : 30;
        GetMonData(e->mon, MON_DATA_NICKNAME, gStringVar1);
        StringGet_Nickname(gStringVar1);
        RtUi_ToastFmt(sText_Angry, 50);
        PlaySE(SE_EXP);
    }
    else if (pressed & (START_BUTTON | R_BUTTON))
    {
        Rt_EndBattle(B_OUTCOME_RAN);
        return;
    }
    // flee check every 4 seconds
    if (++gRt->safariFleeTimer >= 240)
    {
        u32 rate = gSpeciesInfo[e->species].safariZoneFleeRate * gRt->safariFleeMod / 10;
        gRt->safariFleeTimer = 0;
        if ((Random() % 255) < rate * 2)
        {
            Rt_EndBattle(B_OUTCOME_MON_FLED);
            return;
        }
        GetMonData(e->mon, MON_DATA_NICKNAME, gStringVar1);
        StringGet_Nickname(gStringVar1);
        if (Random() % 3 == 0)
            RtUi_ToastFmt(sText_WatchingCarefully, 50);
    }
    if (gRt->safariBalls == 0)
    {
        bool8 busy = FALSE;
        s32 i;
        for (i = 0; i < RT_MAX_PROJ; i++)
            if (gRt->proj[i].kind == PK_BALL)
                busy = TRUE;
        if (!busy)
            Rt_EndBattle(B_OUTCOME_NO_SAFARI_BALLS);
    }
}

// ---------------------------------------------------------------------------
// End-of-battle EXP panel: every party POKeMON that gained EXP, its bar filling
// from where it was when the battle started to where it is now (with level-ups)
// ---------------------------------------------------------------------------
static EWRAM_DATA u8 sExpSlots[PARTY_SIZE] = {0};
static EWRAM_DATA u8 sExpCount = 0;
static EWRAM_DATA u16 sExpTimer = 0;
static EWRAM_DATA u16 sExpDur = 0;   // frames the bars take to fill: one per pixel, like the original

// position of a POKeMON's EXP bar in pixels, counting a full bar per level (so level-ups add up)
static u32 ExpBarPixels(u16 species, u32 exp)
{
    u8 growth = gSpeciesInfo[species].growthRate;
    u8 lv = 1;
    u32 lo, hi;
    while (lv < MAX_LEVEL && gExperienceTables[growth][lv + 1] <= exp)
        lv++;
    if (lv >= MAX_LEVEL)
        return lv * 46;
    lo = gExperienceTables[growth][lv];
    hi = gExperienceTables[growth][lv + 1];
    return lv * 46 + (exp - lo) * 46 / (hi - lo);
}

static const u8 sText_ExpTitle[] = _("EXP. POINTS");
static const u8 sText_ExpPlus[] = _("+{STR_VAR_1}");

// transparent background: the rows are only 10px apart and must not paint over each other
static const u8 sColExp[] = {0, 2, 3};
static const u8 sColExpRed[] = {0, 5, 3};
static const u8 sColExpGray[] = {0, 3, 7};

static void DrawExpPanel(u16 t)
{
    s32 i;
    {
        // frame only as tall as the list; the rest of the window stays see-through
        s32 h = 13 + sExpCount * 10 + 4;
        FillWindowPixelBuffer(WIN_EXP, PIXEL_FILL(0));
        FillWindowPixelRect(WIN_EXP, PIXEL_FILL(1), 0, 0, 160, h);
        FillWindowPixelRect(WIN_EXP, PIXEL_FILL(4), 0, 0, 160, 2);
        FillWindowPixelRect(WIN_EXP, PIXEL_FILL(4), 0, h - 2, 160, 2);
        FillWindowPixelRect(WIN_EXP, PIXEL_FILL(4), 0, 0, 2, h);
        FillWindowPixelRect(WIN_EXP, PIXEL_FILL(4), 158, 0, 2, h);
    }
    AddTextPrinterParameterized3(WIN_EXP, FONT_SMALL, 6, 2, sColExpGray, TEXT_SKIP_DRAW, sText_ExpTitle);
    for (i = 0; i < sExpCount; i++)
    {
        u8 slot = sExpSlots[i];
        struct Pokemon *mon = &gPlayerParty[slot];
        u16 species = GetMonData(mon, MON_DATA_SPECIES);
        u8 growth = gSpeciesInfo[species].growthRate;
        u32 e0 = gRt->expStart[slot], e1 = GetMonData(mon, MON_DATA_EXP);
        u32 shown = (t >= sExpDur) ? e1 : e0 + (e1 - e0) * t / sExpDur;
        u8 lv = 1;
        u32 lo, hi, fill;
        s32 y = 13 + i * 10;
        while (lv < MAX_LEVEL && gExperienceTables[growth][lv + 1] <= shown)
            lv++;
        lo = gExperienceTables[growth][lv];
        hi = lv < MAX_LEVEL ? gExperienceTables[growth][lv + 1] : lo + 1;
        fill = lv < MAX_LEVEL ? (shown - lo) * 46 / (hi - lo) : 46;
        GetMonData(mon, MON_DATA_NICKNAME, gStringVar1);
        StringGet_Nickname(gStringVar1);
        AddTextPrinterParameterized3(WIN_EXP, FONT_SMALL, 6, y, sColExp, TEXT_SKIP_DRAW, gStringVar1);
        gStringVar2[0] = CHAR_L;
        gStringVar2[1] = CHAR_v;
        ConvertIntToDecimalStringN(gStringVar2 + 2, lv, STR_CONV_MODE_LEFT_ALIGN, 3);
        AddTextPrinterParameterized3(WIN_EXP, FONT_SMALL, 54, y, sColExp, TEXT_SKIP_DRAW, gStringVar2);
        // the bar
        FillWindowPixelRect(WIN_EXP, PIXEL_FILL(2), 78, y + 3, 48, 5);
        FillWindowPixelRect(WIN_EXP, PIXEL_FILL(3), 79, y + 4, 46, 3);
        if (fill)
            FillWindowPixelRect(WIN_EXP, PIXEL_FILL(4), 79, y + 4, fill, 3);
        ConvertIntToDecimalStringN(gStringVar1, e1 - e0, STR_CONV_MODE_LEFT_ALIGN, 6);
        StringExpandPlaceholders(gStringVar4, sText_ExpPlus);
        AddTextPrinterParameterized3(WIN_EXP, FONT_SMALL, 130, y, sColExpRed, TEXT_SKIP_DRAW, gStringVar4);
    }
    PutWindowTilemap(WIN_EXP);
    CopyWindowToVram(WIN_EXP, COPYWIN_FULL);
}

bool8 RtUi_ExpPanelStart(void)
{
    s32 i;
    sExpCount = 0;
    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE || GetMonData(mon, MON_DATA_IS_EGG))
            continue;
        if (GetMonData(mon, MON_DATA_EXP) > gRt->expStart[i])
            sExpSlots[sExpCount++] = i;
    }
    if (sExpCount == 0)
        return FALSE;
    sExpTimer = 0;
    sExpDur = 4;
    for (i = 0; i < sExpCount; i++)
    {
        struct Pokemon *mon = &gPlayerParty[sExpSlots[i]];
        u16 species = GetMonData(mon, MON_DATA_SPECIES);
        u32 px = ExpBarPixels(species, GetMonData(mon, MON_DATA_EXP)) - ExpBarPixels(species, gRt->expStart[sExpSlots[i]]);
        if (px > sExpDur)
            sExpDur = px;
    }
    if (sExpDur > 120)
        sExpDur = 120;
    DrawExpPanel(0);
    // as in the original: one rising tone while the bars fill, cut when they stop
    PlaySE(SE_EXP);
    return TRUE;
}

bool8 RtUi_ExpPanelUpdate(void)
{
    bool8 press = (gMain.newKeys & (A_BUTTON | B_BUTTON)) != 0;
    if (press && sExpTimer < sExpDur)
    {
        // A while the bars fill: jump to the end
        sExpTimer = sExpDur;
        DrawExpPanel(sExpDur);
        m4aSongNumStop(SE_EXP);
        return FALSE;
    }
    if (sExpTimer < sExpDur)
    {
        sExpTimer += (gMain.heldKeys & A_BUTTON) ? 3 : 1;
        if (sExpTimer >= sExpDur)
        {
            sExpTimer = sExpDur;
            m4aSongNumStop(SE_EXP);
        }
        DrawExpPanel(sExpTimer);
        return FALSE;
    }
    sExpTimer += (gMain.heldKeys & A_BUTTON) ? 3 : 1;
    if ((sExpTimer > sExpDur && press) || sExpTimer > sExpDur + 90)
    {
        m4aSongNumStop(SE_EXP);
        ClearWindowTilemap(WIN_EXP);
        CopyWindowToVram(WIN_EXP, COPYWIN_MAP);
        return TRUE;
    }
    return FALSE;
}
