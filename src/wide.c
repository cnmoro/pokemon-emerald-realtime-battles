#include "global.h"
#include "wide.h"

#define REG_WIDE_MAGIC (*(vu16 *)(REG_BASE + 0x110))
#define REG_WIDE_MAX   (*(vu16 *)(REG_BASE + 0x112))
#define REG_WIDE_REQ   (*(vu16 *)(REG_BASE + 0x114))
#define REG_WIDE_MASK  (*(vu16 *)(REG_BASE + 0x116))

EWRAM_DATA u16 gWideW = 0;
EWRAM_DATA u8 gFieldWideExt = 0;

// boot (power on or soft reset): the emulator may still hold the width of the game that ran before; start at 240
void Wide_Init(void)
{
    gWideW = DISPLAY_WIDTH;
    gFieldWideExt = 0;
    if (REG_WIDE_MAGIC == 0x5744)
    {
        REG_WIDE_REQ = DISPLAY_WIDTH;
        REG_WIDE_MASK = 0;
    }
}

u16 Wide_MaxWidth(void)
{
    u16 w;
    if (REG_WIDE_MAGIC != 0x5744)
        return DISPLAY_WIDTH;
    w = REG_WIDE_MAX;
    if (w < DISPLAY_WIDTH || w > WIDE_LIMIT)
        return DISPLAY_WIDTH;
    return w & ~1;
}

void Wide_Request(u16 width)
{
    if (width < DISPLAY_WIDTH)
        width = DISPLAY_WIDTH;
    if ((gWideW > DISPLAY_WIDTH || width > DISPLAY_WIDTH) && REG_WIDE_MAGIC == 0x5744)
    {
        REG_WIDE_REQ = width;
        REG_WIDE_MASK = 0;
    }
    gWideW = width;
}

// BGs (bits 0-3) the emulator keeps out of the side margins (a 32-tile-wide layer would repeat there)
void Wide_SetMask(u16 bgs)
{
    if (REG_WIDE_MAGIC == 0x5744)
        REG_WIDE_MASK = bgs;
}
