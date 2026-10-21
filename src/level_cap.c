#include "global.h"
#include "event_data.h"
#include "level_cap.h"
#include "constants/flags.h"
#include "constants/pokemon.h"

// cap = level of the strongest POKeMON of the next Gym Leader (then the Champion), so the party can
// match the ace of the next big fight but not overpower it
static const u8 sCaps[] = {
    14, // Brock (Onix)
    21, // Misty (Starmie)
    24, // Lt. Surge (Raichu)
    29, // Erika (Vileplume)
    43, // Koga (Weezing)
    43, // Sabrina (Alakazam)
    47, // Blaine (Arcanine)
    50, // Giovanni (Rhydon)
    63, // Elite Four / Champion (starter)
};

u8 GetLevelCap(void)
{
    u8 i, badges = 0;
    if (!gSaveBlock2Ptr->optionsRtLevelCap)
        return MAX_LEVEL;
    if (FlagGet(FLAG_SYS_GAME_CLEAR))
        return MAX_LEVEL;
    for (i = 0; i < 8; i++)
        if (FlagGet(FLAG_BADGE01_GET + i))
            badges++;
    return sCaps[badges];
}
