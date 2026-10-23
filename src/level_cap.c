#include "global.h"
#include "event_data.h"
#include "level_cap.h"
#include "constants/flags.h"
#include "constants/pokemon.h"

// cap = level of the strongest POKeMON of the next Gym Leader (then the Champion), so the party can
// match the ace of the next big fight but not overpower it (Platinum order and teams)
static const u8 sCaps[] = {
    14, // Roark (Cranidos)
    22, // Gardenia (Roserade)
    26, // Fantina (Mismagius)
    32, // Maylene (Lucario)
    37, // Crasher Wake (Floatzel)
    41, // Byron (Bastiodon)
    44, // Candice (Froslass)
    50, // Volkner (Electivire)
    62, // Elite Four / Champion Cynthia (Garchomp)
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
