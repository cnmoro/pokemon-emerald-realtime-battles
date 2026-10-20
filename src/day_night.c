// Day / night cycle with colour grading for the overworld.
// The palettes the game builds (gPlttBufferFaded) are left alone; a graded copy is made at the end of
// each overworld frame (only when they or the time of day changed) and that copy is what the VBlank
// sends to the hardware. Text window palettes (BG 13..15) are never graded.
#include "global.h"
#include "day_night.h"
#include "main.h"
#include "overworld.h"
#include "palette.h"
#include "rtc.h"

struct Key
{
    u16 minute;          // minute of the day
    u8 r, g, b;          // channel gains (128 = 1.0)
    u8 desat;            // 0..64: amount of desaturation
    u8 lr, lg, lb;       // lift added to every channel (0..31 scale, x/4)
};

// keyframes, interpolated by minute; the colours are always a little desaturated (less "candy" look)
static const struct Key sKeys[] =
{
    {   0,  78,  86, 112, 24,  0, 1, 5 }, // night
    { 300,  78,  86, 112, 24,  0, 1, 5 },
    { 360, 128, 100, 106, 12,  6, 2, 4 }, // dawn: pink / violet
    { 420, 126, 116, 108, 14,  2, 1, 0 }, // morning: warm
    { 600, 128, 128, 126, 12,  0, 0, 0 }, // day
    { 960, 128, 127, 122, 12,  0, 0, 0 },
    {1050, 130, 114,  92, 14,  3, 1, 0 }, // afternoon: golden
    {1125, 130,  98,  84, 10,  4, 0, 2 }, // sunset: orange / red
    {1170, 100,  82, 112, 18,  1, 0, 5 }, // dusk: purple
    {1230,  78,  86, 112, 24,  0, 1, 5 }, // night
    {1440,  78,  86, 112, 24,  0, 1, 5 },
};
// indoors: only the grading, no time of day
static const struct Key sIndoor = { 0, 126, 126, 124, 12, 0, 0, 0 };

static EWRAM_DATA u16 sGraded[PLTT_SIZE / 2] = {0};
static EWRAM_DATA u8 sLut[3][32] = {0};
static EWRAM_DATA u8 sDesat = 0;
static EWRAM_DATA u32 sSum = 0;
static EWRAM_DATA u16 sKeyMin = 0xFFFF;
static EWRAM_DATA u8 sTimer = 0;
static EWRAM_DATA u8 sIndoorMode = 0xFF;
static EWRAM_DATA bool8 sValid = FALSE;
EWRAM_DATA u32 gDnsFrame = 0;
EWRAM_DATA u16 gDnsDebugMinute = 0; // test harness: 1 + minute of the day to show

static void BuildLut(const struct Key *k)
{
    s32 c, v;
    const u8 *gain = &k->r, *lift = &k->lr;
    for (c = 0; c < 3; c++)
        for (v = 0; v < 32; v++)
        {
            s32 o = (v * gain[c]) / 128 + lift[c] / 4;
            sLut[c][v] = o > 31 ? 31 : o;
        }
    sDesat = k->desat;
}

static void Interp(u16 m, struct Key *out)
{
    s32 i;
    for (i = 0; i + 1 < (s32)ARRAY_COUNT(sKeys); i++)
    {
        const struct Key *a = &sKeys[i], *b = &sKeys[i + 1];
        if (m >= a->minute && m <= b->minute)
        {
            s32 t = b->minute == a->minute ? 0 : (m - a->minute) * 256 / (b->minute - a->minute);
            const u8 *pa = &a->r, *pb = &b->r;
            u8 *po = &out->r;
            s32 k;
            for (k = 0; k < 7; k++)
                po[k] = pa[k] + ((pb[k] - pa[k]) * t) / 256;
            return;
        }
    }
    *out = sKeys[0];
}

static u16 Grade(u16 col)
{
    s32 r = col & 31, g = (col >> 5) & 31, b = (col >> 10) & 31;
    s32 lum = (r * 5 + g * 9 + b * 2) >> 4;
    r += ((lum - r) * sDesat) >> 6;
    g += ((lum - g) * sDesat) >> 6;
    b += ((lum - b) * sDesat) >> 6;
    return sLut[0][r] | (sLut[1][g] << 5) | (sLut[2][b] << 10);
}

void DayNight_FrameEnd(void)
{
    bool8 dirty = FALSE;
    u8 indoor = !IsMapTypeOutdoors(gMapHeader.mapType);
    u32 sum = 0;
    s32 i;
    const u32 *w = (const u32 *)gPlttBufferFaded;

    if (sTimer == 0 || indoor != sIndoorMode)
    {
        struct Key k;
        u16 m;
        sTimer = 120;
        if (indoor)
        {
            m = 0xFFFE;
            k = sIndoor;
        }
        else
        {
            RtcCalcLocalTime();
            m = gLocalTime.hours * 60 + gLocalTime.minutes;
            if (gDnsDebugMinute)
                m = gDnsDebugMinute - 1;
            Interp(m, &k);
        }
        if (m != sKeyMin || indoor != sIndoorMode)
        {
            sKeyMin = m;
            sIndoorMode = indoor;
            BuildLut(&k);
            dirty = TRUE;
        }
    }
    sTimer--;

    for (i = 0; i < PLTT_SIZE / 4; i++)
        sum = ((sum << 1) | (sum >> 31)) + w[i];
    if (sum != sSum || !sValid)
        dirty = TRUE;
    sSum = sum;
    if (dirty)
    {
        for (i = 0; i < PLTT_SIZE / 2; i++)
        {
            // BG palettes 13..15: text boxes and menus
            if (i >= 13 * 16 && i < 16 * 16)
                sGraded[i] = gPlttBufferFaded[i];
            else
                sGraded[i] = Grade(gPlttBufferFaded[i]);
        }
        sValid = TRUE;
    }
    gDnsFrame = gMain.vblankCounter1;
}

// palette for the hardware this VBlank: the graded copy while the overworld is running
const u16 *DayNight_GetTransferBuffer(void)
{
    if (sValid && gMain.vblankCounter1 - gDnsFrame <= 1)
        return sGraded;
    return gPlttBufferFaded;
}
