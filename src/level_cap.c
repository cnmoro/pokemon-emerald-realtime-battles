#include "global.h"
#include "event_data.h"
#include "level_cap.h"
#include "constants/flags.h"
#include "constants/pokemon.h"

// cap = level of the strongest POKeMON of the next Gym Leader (then the Champion), so the party can
// match the ace of the next big fight but not overpower it
static const u8 sCaps[] = {
    15, // Roxanne
    19, // Brawly
    24, // Wattson
    29, // Flannery
    31, // Norman
    33, // Winona
    42, // Tate & Liza
    46, // Juan
    58, // Elite Four / Wallace
};

u8 GetLevelCap(void)
{
    u8 i, badges = 0;
    if (!gSaveBlock2Ptr->optionsRtLevelCap)
        return MAX_LEVEL;
    if (FlagGet(FLAG_IS_CHAMPION))
        return MAX_LEVEL;
    for (i = 0; i < 8; i++)
        if (FlagGet(FLAG_BADGE01_GET + i))
            badges++;
    return sCaps[badges];
}
