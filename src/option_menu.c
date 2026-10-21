#include "global.h"
#include "gflib.h"
#include "scanline_effect.h"
#include "text_window_graphics.h"
#include "menu.h"
#include "task.h"
#include "overworld.h"
#include "help_system.h"
#include "text_window.h"
#include "strings.h"
#include "field_fadetransition.h"
#include "gba/m4a_internal.h"

// can't include the one in menu_helpers.h since Task_OptionMenu needs bool32 for matching
bool32 IsActiveOverworldLinkBusy(void);

// Menu items
enum
{
    MENUITEM_TEXTSPEED = 0,
    MENUITEM_BATTLESCENE,   // real-time mod: DIFFICULTY (battle scene has no effect in real-time battles)
    MENUITEM_BATTLESTYLE,   // real-time mod: AUTO AIM (battle style has no effect either)
    MENUITEM_EXPSHARE,
    MENUITEM_EXPBONUS,
    MENUITEM_BATTLESPEED,
    MENUITEM_LEVELCAP,
    MENUITEM_AUTOHEAL,
    MENUITEM_SOUND,
    MENUITEM_BUTTONMODE,
    MENUITEM_FRAMETYPE,
    MENUITEM_CANCEL,
    MENUITEM_COUNT
};

// Window Ids
enum
{
    WIN_TEXT_OPTION,
    WIN_OPTIONS
};

// RAM symbols
struct OptionMenu
{
    /*0x00*/ u16 option[MENUITEM_COUNT];
    /*0x0E*/ u16 cursorPos;
    /*0x10*/ u8 loadState;
    /*0x11*/ u8 state;
    /*0x12*/ u8 loadPaletteState;
    u8 scroll;              // first visible row (the list scrolls: 12 options + CANCEL, 10 rows shown)
};

static EWRAM_DATA struct OptionMenu *sOptionMenuPtr = NULL;

//Function Declarataions
static void CB2_InitOptionMenu(void);
static void VBlankCB_OptionMenu(void);
static void OptionMenu_InitCallbacks(void);
static void OptionMenu_SetVBlankCallback(void);
static void CB2_OptionMenu(void);
static void SetOptionMenuTask(void);
static void InitOptionMenuBg(void);
static void OptionMenu_PickSwitchCancel(void);
static void OptionMenu_ResetSpriteData(void);
static bool8 LoadOptionMenuPalette(void);
static void Task_OptionMenu(u8 taskId);
static u8 OptionMenu_ProcessInput(void);
static void BufferOptionMenuString(u8 selection);
static void CloseAndSaveOptionMenu(u8 taskId);
static void PrintOptionMenuHeader(void);
static void DrawOptionMenuBg(void);
static void LoadOptionMenuItemNames(void);
static void UpdateSettingSelectionDisplay(u16 selection);

// Data Definitions
static const struct WindowTemplate sOptionMenuWinTemplates[] =
{
    {
        .bg = 1,
        .tilemapLeft = 2,
        .tilemapTop = 3,
        .width = 1,
        .height = 1,
        .paletteNum = 1,
        .baseBlock = 0x1EF
    },
    {
        .bg = 0,
        .tilemapLeft = 2,
        .tilemapTop = 3,
        .width = 26,
        .height = 16,
        .paletteNum = 1,
        .baseBlock = 0x1
    },
    {
        .bg = 2,
        .tilemapLeft = 0,
        .tilemapTop = 0,
        .width = 30,
        .height = 2,
        .paletteNum = 15,
        .baseBlock = 0x1A1
    },
    DUMMY_WIN_TEMPLATE
};

static const struct BgTemplate sOptionMenuBgTemplates[] =
{
   {
       .bg = 1,
       .charBaseIndex = 1,
       .mapBaseIndex = 30,
       .screenSize = 0,
       .paletteMode = 0,
       .priority = 0,
       .baseTile = 0
   },
   {
       .bg = 0,
       .charBaseIndex = 1,
       .mapBaseIndex = 31,
       .screenSize = 0,
       .paletteMode = 0,
       .priority = 1,
       .baseTile = 0
   },
   {
       .bg = 2,
       .charBaseIndex = 1,
       .mapBaseIndex = 29,
       .screenSize = 0,
       .paletteMode = 0,
       .priority = 2,
       .baseTile = 0
   },
};

static const u16 sOptionMenuPalette[] = INCBIN_U16("graphics/misc/option_menu.gbapal");
static const u16 sOptionMenuItemCounts[MENUITEM_COUNT] = {4, 3, 2, 6, 8, 5, 2, 2, 2, 3, 10, 0};

// real-time mod rows are 12 px apart (10 rows); text is drawn on a transparent background so a
// row's glyph cell never paints over its neighbours' shadows
#define OPTION_ROW_H 12
#define OPTION_WIN_Y (3 * 8)
#define OPTION_VISIBLE 10
static const u8 sOptionNameColor[] = {TEXT_COLOR_TRANSPARENT, TEXT_COLOR_DARK_GRAY, TEXT_COLOR_LIGHT_GRAY};

static const u8 sText_RtDifficulty[] = _("DIFFICULTY");
static const u8 sText_RtAutoAim[] = _("AUTO AIM");
static const u8 sText_RtExpShare[] = _("EXP SHARE");
static const u8 sText_RtExpBonus[] = _("EXP BONUS");
static const u8 sText_RtBattleSpeed[] = _("BATTLE SPEED");
static const u8 sText_RtLevelCap[] = _("LEVEL CAP");
static const u8 sText_RtAutoHeal[] = _("AUTO HEAL");
static const u8 sText_Easy[] = _("EASY");
static const u8 sText_Medium[] = _("MEDIUM");
static const u8 sText_Hard[] = _("HARD");
static const u8 sText_Off[] = _("OFF");
static const u8 sText_On[] = _("ON");
static const u8 sText_First2[] = _("FIRST 2");
static const u8 sText_First3[] = _("FIRST 3");
static const u8 sText_First4[] = _("FIRST 4");
static const u8 sText_First5[] = _("FIRST 5");
static const u8 sText_All6[] = _("ALL 6");
static const u8 sText_B25[] = _("+25%");
static const u8 sText_B50[] = _("+50%");
static const u8 sText_B100[] = _("+100%");
static const u8 sText_B150[] = _("+150%");
static const u8 sText_B200[] = _("+200%");
static const u8 sText_B300[] = _("+300%");
static const u8 sText_B400[] = _("+400%");
static const u8 sText_S60[] = _("60% SLOWEST");
static const u8 sText_S70[] = _("70%");
static const u8 sText_S80[] = _("80%");
static const u8 sText_S90[] = _("90%");
static const u8 sText_S100[] = _("100% ORIGINAL");
static const u8 *const sDifficultyOptions[] = {sText_Easy, sText_Medium, sText_Hard};
static const u8 *const sAutoAimOptions[] = {sText_Off, sText_On};
static const u8 *const sExpShareOptions[] = {sText_Off, sText_First2, sText_First3, sText_First4, sText_First5, sText_All6};
static const u8 *const sExpBonusOptions[] = {sText_Off, sText_B25, sText_B50, sText_B100, sText_B150, sText_B200, sText_B300, sText_B400};
static const u8 *const sBattleSpeedOptions[] = {sText_S60, sText_S70, sText_S80, sText_S90, sText_S100};
// saved difficulty 0 medium / 1 easy / 2 hard <-> menu 0 easy / 1 medium / 2 hard
static const u8 sDifficultySaveToMenu[] = {1, 0, 2};
static const u8 sDifficultyMenuToSave[] = {1, 0, 2};
// saved battle speed code 0 = 80 % (default), 1 = 60, 2 = 70, 3 = 90, 4 = 100 <-> menu 60..100
static const u8 sSpeedSaveToMenu[] = {2, 0, 1, 3, 4};
static const u8 sSpeedMenuToSave[] = {1, 2, 0, 3, 4};

static const u8 *const sOptionMenuItemsNames[MENUITEM_COUNT] =
{
    [MENUITEM_TEXTSPEED]   = gText_TextSpeed,
    [MENUITEM_BATTLESCENE] = sText_RtDifficulty,
    [MENUITEM_BATTLESTYLE] = sText_RtAutoAim,
    [MENUITEM_EXPSHARE]    = sText_RtExpShare,
    [MENUITEM_EXPBONUS]    = sText_RtExpBonus,
    [MENUITEM_BATTLESPEED] = sText_RtBattleSpeed,
    [MENUITEM_LEVELCAP]    = sText_RtLevelCap,
    [MENUITEM_AUTOHEAL]    = sText_RtAutoHeal,
    [MENUITEM_SOUND]       = gText_Sound,
    [MENUITEM_BUTTONMODE]  = gText_ButtonMode,
    [MENUITEM_FRAMETYPE]   = gText_Frame,
    [MENUITEM_CANCEL]      = gText_OptionMenuCancel,
};

static const u8 *const sTextSpeedOptions[] =
{
    gText_TextSpeedSlow, 
    gText_TextSpeedMid, 
    gText_TextSpeedFast,
    gText_TextSpeedInstant
};

static const u8 *const sBattleSceneOptions[] =
{
    gText_BattleSceneOn, 
    gText_BattleSceneOff
};

static const u8 *const sBattleStyleOptions[] =
{
    gText_BattleStyleShift,
    gText_BattleStyleSet
};

static const u8 *const sSoundOptions[] =
{
    gText_SoundMono, 
    gText_SoundStereo
};

static const u8 *const sButtonTypeOptions[] =
{
    gText_ButtonTypeHelp,
	gText_ButtonTypeLR,
	gText_ButtonTypeLEqualsA
};

static const u8 sOptionMenuPickSwitchCancelTextColor[] = {TEXT_DYNAMIC_COLOR_6, TEXT_COLOR_WHITE, TEXT_COLOR_DARK_GRAY};
static const u8 sOptionMenuTextColor[] = {TEXT_COLOR_TRANSPARENT, TEXT_COLOR_LIGHT_RED, TEXT_COLOR_RED};

// Functions
static void CB2_InitOptionMenu(void)
{
    RunTasks();
    AnimateSprites();
    BuildOamBuffer();
    UpdatePaletteFade();
}

static void VBlankCB_OptionMenu(void)
{
    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
}

void CB2_OptionsMenuFromStartMenu(void)
{
    u8 i;
    
    if (gMain.savedCallback == NULL)
        gMain.savedCallback = CB2_ReturnToFieldWithOpenMenu;
    sOptionMenuPtr = AllocZeroed(sizeof(struct OptionMenu));
    sOptionMenuPtr->loadState = 0;
    sOptionMenuPtr->loadPaletteState = 0;
    sOptionMenuPtr->state = 0;
    sOptionMenuPtr->cursorPos = 0;
    sOptionMenuPtr->option[MENUITEM_TEXTSPEED] = gSaveBlock2Ptr->optionsTextSpeed;
    sOptionMenuPtr->option[MENUITEM_BATTLESCENE] = sDifficultySaveToMenu[gSaveBlock2Ptr->optionsRtDifficulty < 3 ? gSaveBlock2Ptr->optionsRtDifficulty : 0];
    sOptionMenuPtr->option[MENUITEM_BATTLESTYLE] = gSaveBlock2Ptr->optionsRtAutoAim;
    sOptionMenuPtr->option[MENUITEM_EXPSHARE] = gSaveBlock2Ptr->optionsRtExpShare;
    sOptionMenuPtr->option[MENUITEM_EXPBONUS] = gSaveBlock2Ptr->optionsRtExpBonus;
    sOptionMenuPtr->option[MENUITEM_BATTLESPEED] = sSpeedSaveToMenu[gSaveBlock2Ptr->optionsRtBattleSpeed < 5 ? gSaveBlock2Ptr->optionsRtBattleSpeed : 0];
    sOptionMenuPtr->option[MENUITEM_LEVELCAP] = gSaveBlock2Ptr->optionsRtLevelCap & 1;
    sOptionMenuPtr->option[MENUITEM_AUTOHEAL] = gSaveBlock2Ptr->optionsRtAutoHeal & 1;
    sOptionMenuPtr->option[MENUITEM_SOUND] = gSaveBlock2Ptr->optionsSound;
    sOptionMenuPtr->option[MENUITEM_BUTTONMODE] = gSaveBlock2Ptr->optionsButtonMode;
    sOptionMenuPtr->option[MENUITEM_FRAMETYPE] = gSaveBlock2Ptr->optionsWindowFrameType;
    
    for (i = 0; i < MENUITEM_COUNT - 1; i++)
    {
        if (sOptionMenuPtr->option[i] > (sOptionMenuItemCounts[i]) - 1)
            sOptionMenuPtr->option[i] = 0;
    }
    SetHelpContext(HELPCONTEXT_OPTIONS);
    SetMainCallback2(CB2_OptionMenu);
}

static void OptionMenu_InitCallbacks(void)
{
    SetVBlankCallback(NULL);
    SetHBlankCallback(NULL);
}

static void OptionMenu_SetVBlankCallback(void)
{
    SetVBlankCallback(VBlankCB_OptionMenu);
}

static void CB2_OptionMenu(void)
{
    u8 i, state;
    state = sOptionMenuPtr->state;
    switch (state)
    {
    case 0:
        OptionMenu_InitCallbacks();
        break;
    case 1:
        InitOptionMenuBg();
        break;
    case 2:
        OptionMenu_ResetSpriteData();
        break;
    case 3:
        if (LoadOptionMenuPalette() != TRUE)
            return;
        break;
    case 4:
        PrintOptionMenuHeader();
        break;
    case 5:
        DrawOptionMenuBg();
        break;
    case 6:
        LoadOptionMenuItemNames();
        break;
    case 7:
        for (i = 0; i < MENUITEM_COUNT; i++)
            BufferOptionMenuString(i);
        break;
    case 8:
        UpdateSettingSelectionDisplay(sOptionMenuPtr->cursorPos);
        break;
    case 9:
        OptionMenu_PickSwitchCancel();
        break;
    default:
        SetOptionMenuTask();
		break;
    }
    sOptionMenuPtr->state++;
}

static void SetOptionMenuTask(void)
{
    CreateTask(Task_OptionMenu, 0);
    SetMainCallback2(CB2_InitOptionMenu);
}

static void InitOptionMenuBg(void)
{
    void *dest = (void *)VRAM;
    DmaClearLarge16(3, dest, VRAM_SIZE, 0x1000);    
    DmaClear32(3, (void *)OAM, OAM_SIZE);
    DmaClear16(3, (void *)PLTT, PLTT_SIZE);    
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_0);
    ResetBgsAndClearDma3BusyFlags(0);
    InitBgsFromTemplates(0, sOptionMenuBgTemplates, NELEMS(sOptionMenuBgTemplates));
    ChangeBgX(0, 0, 0);
    ChangeBgY(0, 0, 0);
    ChangeBgX(1, 0, 0);
    ChangeBgY(1, 0, 0);
    ChangeBgX(2, 0, 0);
    ChangeBgY(2, 0, 0);
    ChangeBgX(3, 0, 0);
    ChangeBgY(3, 0, 0);
    InitWindows(sOptionMenuWinTemplates);
    DeactivateAllTextPrinters();
    SetGpuReg(REG_OFFSET_BLDCNT, BLDCNT_TGT1_BG0 | BLDCNT_EFFECT_BLEND | BLDCNT_EFFECT_LIGHTEN);
    SetGpuReg(REG_OFFSET_BLDY, BLDCNT_TGT1_BG1);
    SetGpuReg(REG_OFFSET_WININ, WININ_WIN0_BG0);
    SetGpuReg(REG_OFFSET_WINOUT, WINOUT_WIN01_BG0 | WINOUT_WIN01_BG1 | WINOUT_WIN01_BG2 | WINOUT_WIN01_CLR);
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_1D_MAP | DISPCNT_OBJ_ON | DISPCNT_WIN0_ON);
    ShowBg(0);
    ShowBg(1);
    ShowBg(2);
};

static void OptionMenu_PickSwitchCancel(void)
{
    s32 x;
    x = 0xE4 - GetStringWidth(FONT_SMALL, gText_PickSwitchCancel, 0);
    FillWindowPixelBuffer(2, PIXEL_FILL(15)); 
    AddTextPrinterParameterized3(2, FONT_SMALL, 8, 0, sOptionMenuPickSwitchCancelTextColor, 0, gText_Option);
    AddTextPrinterParameterized3(2, FONT_SMALL, x, 0, sOptionMenuPickSwitchCancelTextColor, 0, gText_PickSwitchCancel);
    PutWindowTilemap(2);
    CopyWindowToVram(2, COPYWIN_FULL);
}

static void OptionMenu_ResetSpriteData(void)
{
    ResetSpriteData();
    ResetPaletteFade();
    FreeAllSpritePalettes();
    ResetTasks();
    ScanlineEffect_Stop();
}

static bool8 LoadOptionMenuPalette(void)
{
    switch (sOptionMenuPtr->loadPaletteState)
    {
    case 0:
        LoadBgTiles(1, GetUserWindowGraphics(sOptionMenuPtr->option[MENUITEM_FRAMETYPE])->tiles, 0x120, 0x1DD);
        break;
    case 1:
        LoadPalette(GetUserWindowGraphics(sOptionMenuPtr->option[MENUITEM_FRAMETYPE])->palette, BG_PLTT_ID(2), PLTT_SIZE_4BPP);
        break;
    case 2:
        LoadPalette(sOptionMenuPalette, BG_PLTT_ID(1), sizeof(sOptionMenuPalette));
        LoadPalette(GetTextWindowPalette(2), BG_PLTT_ID(15), PLTT_SIZE_4BPP);
        break;
    case 3:
        LoadStdWindowGfxOnBg(1, 0x1E6, BG_PLTT_ID(3));
        break;
    default:
        return TRUE;
    }
    sOptionMenuPtr->loadPaletteState++;
    return FALSE;
}

static void Task_OptionMenu(u8 taskId)
{
    switch (sOptionMenuPtr->loadState)
    {
    case 0:
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0x10, 0, RGB_BLACK);
        OptionMenu_SetVBlankCallback();
        sOptionMenuPtr->loadState++;
        break;
    case 1:
        if (gPaletteFade.active)
            return;
        sOptionMenuPtr->loadState++;
        break;
    case 2:
        if (((bool32)IsActiveOverworldLinkBusy()) == TRUE)
            break;
        switch (OptionMenu_ProcessInput())
        {
        case 0:
            break;
        case 1:
            sOptionMenuPtr->loadState++;
            break;
        case 2:
            LoadBgTiles(1, GetUserWindowGraphics(sOptionMenuPtr->option[MENUITEM_FRAMETYPE])->tiles, 0x120, 0x1DD);
            LoadPalette(GetUserWindowGraphics(sOptionMenuPtr->option[MENUITEM_FRAMETYPE])->palette, BG_PLTT_ID(2), PLTT_SIZE_4BPP);
            BufferOptionMenuString(sOptionMenuPtr->cursorPos);
            break;
        case 3:
            UpdateSettingSelectionDisplay(sOptionMenuPtr->cursorPos);
            break;
        case 4:
            BufferOptionMenuString(sOptionMenuPtr->cursorPos);
            break;
        }
        break;
    case 3:
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 0x10, RGB_BLACK);
        sOptionMenuPtr->loadState++;
        break;
    case 4:
        if (gPaletteFade.active)
            return;
        sOptionMenuPtr->loadState++;
        break;
    case 5:
        CloseAndSaveOptionMenu(taskId);
        break;
    }
}

static u8 OptionMenu_ProcessInput(void)
{ 
    u16 current;
    u16 *curr;
    if (JOY_REPT(DPAD_RIGHT))
    {
        current = sOptionMenuPtr->option[(sOptionMenuPtr->cursorPos)];
        if (current == (sOptionMenuItemCounts[sOptionMenuPtr->cursorPos] - 1))
            sOptionMenuPtr->option[sOptionMenuPtr->cursorPos] = 0;
        else
            sOptionMenuPtr->option[sOptionMenuPtr->cursorPos] = current + 1;
        if (sOptionMenuPtr->cursorPos == MENUITEM_FRAMETYPE)
            return 2;
        else
            return 4;
    }
    else if (JOY_REPT(DPAD_LEFT))
    {
        curr = &sOptionMenuPtr->option[sOptionMenuPtr->cursorPos];
        if (*curr == 0)
            *curr = sOptionMenuItemCounts[sOptionMenuPtr->cursorPos] - 1;
        else
            --*curr;
        
        if (sOptionMenuPtr->cursorPos == MENUITEM_FRAMETYPE)
            return 2;
        else
            return 4;
    }
    else if (JOY_REPT(DPAD_UP))
    {
        if (sOptionMenuPtr->cursorPos == MENUITEM_TEXTSPEED)
            sOptionMenuPtr->cursorPos = MENUITEM_CANCEL;
        else
            sOptionMenuPtr->cursorPos = sOptionMenuPtr->cursorPos - 1;
        return 3;        
    }
    else if (JOY_REPT(DPAD_DOWN))
    {
        if (sOptionMenuPtr->cursorPos == MENUITEM_CANCEL)
            sOptionMenuPtr->cursorPos = MENUITEM_TEXTSPEED;
        else
            sOptionMenuPtr->cursorPos = sOptionMenuPtr->cursorPos + 1;
        return 3;
    }
    else if (JOY_NEW(B_BUTTON) || JOY_NEW(A_BUTTON))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static void BufferOptionMenuString(u8 selection)
{
    u8 str[20];
    u8 buf[12];
    u8 dst[3];
    u8 x, y;
    
    memcpy(dst, sOptionMenuTextColor, 3);
    x = 0x76;
    if (selection < sOptionMenuPtr->scroll || selection >= sOptionMenuPtr->scroll + OPTION_VISIBLE)
        return;
    y = OPTION_ROW_H * (selection - sOptionMenuPtr->scroll) + 1;
    FillWindowPixelRect(1, 1, x, y + 2, 0xD0 - x, OPTION_ROW_H);

    switch (selection)
    {
    case MENUITEM_TEXTSPEED:
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, sTextSpeedOptions[sOptionMenuPtr->option[selection]]);
        break;
    case MENUITEM_BATTLESCENE:
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, sDifficultyOptions[sOptionMenuPtr->option[selection]]);
        break;
    case MENUITEM_BATTLESTYLE:
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, sAutoAimOptions[sOptionMenuPtr->option[selection]]);
        break;
    case MENUITEM_EXPSHARE:
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, sExpShareOptions[sOptionMenuPtr->option[selection]]);
        break;
    case MENUITEM_EXPBONUS:
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, sExpBonusOptions[sOptionMenuPtr->option[selection]]);
        break;
    case MENUITEM_BATTLESPEED:
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, sBattleSpeedOptions[sOptionMenuPtr->option[selection]]);
        break;
    case MENUITEM_LEVELCAP:
    case MENUITEM_AUTOHEAL:
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, sAutoAimOptions[sOptionMenuPtr->option[selection]]);
        break;
    case MENUITEM_SOUND:
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, sSoundOptions[sOptionMenuPtr->option[selection]]);
        break;
    case MENUITEM_BUTTONMODE:
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, sButtonTypeOptions[sOptionMenuPtr->option[selection]]);
        break;
    case MENUITEM_FRAMETYPE:
        StringCopy(str, gText_FrameType);
        ConvertIntToDecimalStringN(buf, sOptionMenuPtr->option[selection] + 1, 1, 2);
        StringAppendN(str, buf, 3);
        AddTextPrinterParameterized3(1, FONT_NORMAL, x, y, dst, -1, str);
        break;
    default:
        break;
    }
    PutWindowTilemap(1);
    CopyWindowToVram(1, COPYWIN_FULL);
}

static void CloseAndSaveOptionMenu(u8 taskId)
{
    gFieldCallback = FieldCB_DefaultWarpExit;
    SetMainCallback2(gMain.savedCallback);
    FreeAllWindowBuffers();
    gSaveBlock2Ptr->optionsTextSpeed = sOptionMenuPtr->option[MENUITEM_TEXTSPEED];
    gSaveBlock2Ptr->optionsRtDifficulty = sDifficultyMenuToSave[sOptionMenuPtr->option[MENUITEM_BATTLESCENE]];
    gSaveBlock2Ptr->optionsRtAutoAim = sOptionMenuPtr->option[MENUITEM_BATTLESTYLE];
    gSaveBlock2Ptr->optionsRtExpShare = sOptionMenuPtr->option[MENUITEM_EXPSHARE];
    gSaveBlock2Ptr->optionsRtExpBonus = sOptionMenuPtr->option[MENUITEM_EXPBONUS];
    gSaveBlock2Ptr->optionsRtBattleSpeed = sSpeedMenuToSave[sOptionMenuPtr->option[MENUITEM_BATTLESPEED]];
    gSaveBlock2Ptr->optionsRtLevelCap = sOptionMenuPtr->option[MENUITEM_LEVELCAP];
    gSaveBlock2Ptr->optionsRtAutoHeal = sOptionMenuPtr->option[MENUITEM_AUTOHEAL];
    gSaveBlock2Ptr->optionsSound = sOptionMenuPtr->option[MENUITEM_SOUND];
    gSaveBlock2Ptr->optionsButtonMode = sOptionMenuPtr->option[MENUITEM_BUTTONMODE];
    gSaveBlock2Ptr->optionsWindowFrameType = sOptionMenuPtr->option[MENUITEM_FRAMETYPE];
    SetPokemonCryStereo(gSaveBlock2Ptr->optionsSound);
    FREE_AND_SET_NULL(sOptionMenuPtr);
    DestroyTask(taskId);
}

static void PrintOptionMenuHeader(void)
{
    // the title is printed in the top bar (OptionMenu_PickSwitchCancel)
}

static void DrawOptionMenuBg(void)
{
    u8 h;
    h = 2;
    
    // real-time mod: the OPTION title moved to the top bar, the list box takes the whole screen
    FillBgTilemapBufferRect(1, 0x1DD, 1, 2, 1, 1, h);
    FillBgTilemapBufferRect(1, 0x1DE, 2, 2, 0x1A, 1, h);
    FillBgTilemapBufferRect(1, 0x1DF, 0x1C, 2, 1, 1, h);
    FillBgTilemapBufferRect(1, 0x1E0, 1, 3, 1, 0x10, h);
    FillBgTilemapBufferRect(1, 0x1E2, 0x1C, 3, 1, 0x10, h);
    FillBgTilemapBufferRect(1, 0x1E3, 1, 0x13, 1, 1, h);
    FillBgTilemapBufferRect(1, 0x1E4, 2, 0x13, 0x1A, 1, h);
    FillBgTilemapBufferRect(1, 0x1E5, 0x1C, 0x13, 1, 1, h);
    CopyBgTilemapBufferToVram(1);
}

static void LoadOptionMenuItemNames(void)
{
    u8 i;
    
    FillWindowPixelBuffer(1, PIXEL_FILL(1));
    for (i = sOptionMenuPtr->scroll; i < MENUITEM_COUNT && i < sOptionMenuPtr->scroll + OPTION_VISIBLE; i++)
    {
        AddTextPrinterParameterized3(WIN_OPTIONS, FONT_NORMAL, 8, OPTION_ROW_H * (i - sOptionMenuPtr->scroll) + 1, sOptionNameColor, TEXT_SKIP_DRAW, sOptionMenuItemsNames[i]);
    }
}

static void UpdateSettingSelectionDisplay(u16 selection)
{
    u16 maxLetterHeight, y;
    
    maxLetterHeight = OPTION_ROW_H;
    // keep the cursor row on screen: scroll the list (and redraw it) when it leaves the window
    if (selection < sOptionMenuPtr->scroll || selection >= sOptionMenuPtr->scroll + OPTION_VISIBLE)
    {
        u8 i;
        if (selection < sOptionMenuPtr->scroll)
            sOptionMenuPtr->scroll = selection;
        else
            sOptionMenuPtr->scroll = selection - OPTION_VISIBLE + 1;
        LoadOptionMenuItemNames();
        for (i = 0; i < MENUITEM_COUNT; i++)
            BufferOptionMenuString(i);
    }
    y = (selection - sOptionMenuPtr->scroll) * OPTION_ROW_H + OPTION_WIN_Y + 3;
    SetGpuReg(REG_OFFSET_WIN0V, WIN_RANGE(y, y + maxLetterHeight));
    SetGpuReg(REG_OFFSET_WIN0H, WIN_RANGE(0x10, 0xE0));
}
