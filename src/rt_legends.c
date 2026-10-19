#include "global.h"
#include "rt_internal.h"

// Roaming legendaries of the Hoenn/Kanto/Johto version are not part of the Platinum demake yet:
// the hooks stay so the engine and the game scripts keep the same interface.

void RtLegends_OnGameClear(void)
{
}

bool8 RtLegends_TryEncounter(void)
{
    return FALSE;
}

bool8 RtLegends_IsRoamingBattle(void)
{
    return FALSE;
}

bool8 RtLegends_IsLegendBattle(void)
{
    return FALSE;
}

void RtLegends_AfterBattle(u8 outcome)
{
}
