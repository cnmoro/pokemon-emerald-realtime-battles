#ifndef GUARD_RT_BATTLE_H
#define GUARD_RT_BATTLE_H

// Real-time arena battle system.
// Public entry points used by battle_main.c.

bool8 RtBattle_IsSupported(void);
void CB2_InitRtBattle(void);
u16 RtBattle_GetMoveCooldownFrames(u16 move, u8 ppBonuses, u8 slot);

#endif // GUARD_RT_BATTLE_H
