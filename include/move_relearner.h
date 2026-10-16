#ifndef GUARD_MOVE_RELEARNER_H
#define GUARD_MOVE_RELEARNER_H

#include "main.h"

void TeachMoveRelearnerMove(void);
void OpenMoveRelearnerFromParty(u8 partySlot, MainCallback returnCB);
void MoveRelearnerShowHideHearts(s32 move);

#endif //GUARD_MOVE_RELEARNER_H
