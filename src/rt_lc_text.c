// Liquid Crystal build only: TEXT SPEED "INSTANT" and the 3x faster battle start transition, written against
// FireRed's own text printers / transition tasks (their data stays at the FireRed addresses);
// tools/lc/patch.py points RunTextPrinters, GetTextSpeedSetting and Task_BattleTransition here.
#include "global.h"
#include "task.h"
#include "text.h"
#include "window.h"
#include "sprite.h"
#include "palette.h"

extern struct TextPrinter sTextPrinters[NUM_TEXT_PRINTERS];
extern const u8 sTextSpeedFrameDelays[];

void RtLc_RunTextPrinters(void)
{
    int i;
    // real-time mod: TEXT SPEED "INSTANT" renders a printer until it has to wait (button, pause, sound, end);
    // line scrolls happen in one step
    bool32 instant = gSaveBlock2Ptr != NULL && gSaveBlock2Ptr->optionsTextSpeed == OPTIONS_TEXT_SPEED_INSTANT;

    for (i = 0; i < NUM_TEXT_PRINTERS; ++i)
    {
        if (sTextPrinters[i].active)
        {
            u16 renderCmd;
            u32 guard = 0;
            bool8 printed = FALSE;
            do
            {
                if (instant)
                {
                    sTextPrinters[i].delayCounter = 0;
                    if (sTextPrinters[i].state == RENDER_STATE_SCROLL)
                    {
                        if (sTextPrinters[i].scrollDistance)
                        {
                            ScrollWindow(sTextPrinters[i].printerTemplate.windowId, 0, sTextPrinters[i].scrollDistance, PIXEL_FILL(sTextPrinters[i].printerTemplate.bgColor));
                            sTextPrinters[i].scrollDistance = 0;
                            printed = TRUE;
                        }
                        sTextPrinters[i].state = RENDER_STATE_HANDLE_CHAR;
                    }
                }
                renderCmd = RenderFont(&sTextPrinters[i]);
                switch (renderCmd)
                {
                case RENDER_PRINT:
                    if (instant)
                        printed = TRUE;
                    else
                        CopyWindowToVram(sTextPrinters[i].printerTemplate.windowId, COPYWIN_GFX);
                case RENDER_UPDATE:
                    if (sTextPrinters[i].callback != NULL)
                        sTextPrinters[i].callback(&sTextPrinters[i].printerTemplate, renderCmd);
                    break;
                case RENDER_FINISH:
                    sTextPrinters[i].active = FALSE;
                    break;
                }
            } while (instant && renderCmd != RENDER_FINISH && sTextPrinters[i].active
                     && (sTextPrinters[i].state == RENDER_STATE_HANDLE_CHAR || sTextPrinters[i].state == RENDER_STATE_SCROLL)
                     && ++guard < 0x400);
            if (printed)
                CopyWindowToVram(sTextPrinters[i].printerTemplate.windowId, COPYWIN_GFX);
        }
    }
}

u8 RtLc_GetTextSpeedSetting(void)
{
    if (gSaveBlock2Ptr->optionsTextSpeed > OPTIONS_TEXT_SPEED_INSTANT)
        gSaveBlock2Ptr->optionsTextSpeed = OPTIONS_TEXT_SPEED_MID;
    if (gSaveBlock2Ptr->optionsTextSpeed == OPTIONS_TEXT_SPEED_INSTANT)
        return 1;
    return sTextSpeedFrameDelays[gSaveBlock2Ptr->optionsTextSpeed];
}

// --- battle start transition ---------------------------------------------------------------------------
typedef bool8 (*TransitionStateFunc)(struct Task *task);
extern const TransitionStateFunc sTaskHandlers[];
extern const TaskFunc sTasks_Intro[];
extern const TaskFunc sTasks_Main[];
void Task_BattleTransition_Intro(u8 taskId);

extern bool8 sPlttBufferTransferPending;
static void PaletteFade_StepAgain(void)
{
    if (gPaletteFade.active && gPaletteFade.mode == 0) // NORMAL_FADE
    {
        sPlttBufferTransferPending = FALSE;
        UpdatePaletteFade();
    }
}

static void RunTransitionTaskAgain(TaskFunc func)
{
    u8 id = FindTaskIdByFunc(func);
    if (id != TASK_NONE)
        func(id);
}

static void SpeedUpIntroFlashes(void)
{
    u8 id = FindTaskIdByFunc(Task_BattleTransition_Intro);
    if (id != TASK_NONE && gTasks[id].data[8] == 0)
    {
        gTasks[id].data[4] *= 3;
        gTasks[id].data[5] *= 3;
        gTasks[id].data[8] = 1;
    }
}

void RtLc_Task_BattleTransition(u8 taskId)
{
    u8 k, transitionId;
    while (sTaskHandlers[gTasks[taskId].data[0]](&gTasks[taskId]));
    transitionId = gTasks[taskId].data[1];
    SpeedUpIntroFlashes();
    for (k = 0; k < 2; k++)
    {
        if (sTasks_Intro[transitionId] != NULL)
            RunTransitionTaskAgain(sTasks_Intro[transitionId]);
        RunTransitionTaskAgain(sTasks_Main[transitionId]);
        AnimateSprites();
        PaletteFade_StepAgain();
    }
}

const u8 gText_TextSpeedInstant[] = _("INSTANT");
