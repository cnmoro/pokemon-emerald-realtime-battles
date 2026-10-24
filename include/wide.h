#ifndef GUARD_WIDE_H
#define GUARD_WIDE_H

// Widescreen support: an emulator that can show a wider picture answers on registers a real GBA does not
// have (0x4000110 reads 'WD', 0x4000112 the widest picture it can show); writing a width to 0x4000114 makes
// it render that many columns, the extra ones split evenly left and right of the normal 240-px picture.
// On real hardware and other emulators nothing changes.
#define WIDE_LIMIT 432

extern u16 gWideW;          // width the game renders for right now (0/240: normal)
extern u8 gFieldWideExt;    // overworld: extra metatiles visible on each side (0 when not wide)

u16 Wide_MaxWidth(void);
void Wide_Request(u16 width);
void Wide_SetMask(u16 bgs);
#define WIDE_L ((s32)((gWideW > DISPLAY_WIDTH ? gWideW : DISPLAY_WIDTH) - DISPLAY_WIDTH) / 2)

#endif // GUARD_WIDE_H
