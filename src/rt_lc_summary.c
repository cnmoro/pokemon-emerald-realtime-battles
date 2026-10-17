// Liquid Crystal build only: the two summary-screen functions the FireRed mod changes (cooldown in seconds
// instead of PP), compiled against FireRed's own summary-screen data (the statics stay at their FireRed
// addresses); tools/lc/patch.py points the original functions here.
#include "global.h"
#include "rt_battle.h"
#include "gflib.h"
#include "pokemon.h"
#include "pokemon_summary_screen.h"
#include "help_system.h"
#include "task.h"
#include "menu_helpers.h"
#include "link.h"
#include "overworld.h"
#include "constants/songs.h"
#include "strings.h"
#include "new_menu_helpers.h"
#include "menu.h"
#include "constants/items.h"
#include "data.h"
#include "item.h"
#include "constants/party_menu.h"
#include "trade.h"
#include "battle_main.h"
#include "scanline_effect.h"
#include "constants/moves.h"
#include "dynamic_placeholder_text_util.h"
#include "constants/region_map_sections.h"
#include "region_map.h"
#include "field_specials.h"
#include "party_menu.h"
#include "constants/battle.h"
#include "event_data.h"
#include "trainer_pokemon_sprites.h"
#include "battle_anim.h"
#include "pokeball.h"
#include "pokemon_icon.h"
#include "battle_interface.h"
#include "mon_markings.h"
#include "pokemon_storage_system.h"
#include "constants/sound.h"

struct PokemonSummaryScreenData
{
    u16 bg1TilemapBuffer[0x800];
    u16 bg2TilemapBuffer[0x800];
    u16 bg3TilemapBuffer[0x800];
    u8 ALIGNED(4) windowIds[7];

    u8 ALIGNED(4) unk3008;
    u8 ALIGNED(4) ballIconSpriteId;
    u8 ALIGNED(4) monPicSpriteId;
    u8 ALIGNED(4) monIconSpriteId;

    u8 ALIGNED(4) inputHandlerTaskId;
    u8 ALIGNED(4) inhibitPageFlipInput;

    u8 ALIGNED(4) numMonPicBounces;

    bool32 isEnemyParty; /* 0x3024 */

    struct PokeSummary
    {
        u8 ALIGNED(4) speciesNameStrBuf[POKEMON_NAME_LENGTH];
        u8 ALIGNED(4) nicknameStrBuf[POKEMON_NAME_LENGTH + 1];
        u8 ALIGNED(4) otNameStrBuf[12];
        u8 ALIGNED(4) otNameStrBufs[2][12];

        u8 ALIGNED(4) dexNumStrBuf[5];
        u8 ALIGNED(4) unk306C[7];
        u8 ALIGNED(4) itemNameStrBuf[ITEM_NAME_LENGTH + 1];

        u8 ALIGNED(4) genderSymbolStrBuf[3];
        u8 ALIGNED(4) levelStrBuf[7];
        u8 ALIGNED(4) curHpStrBuf[9];
        u8 ALIGNED(4) statValueStrBufs[5][5];

        u8 ALIGNED(4) moveCurPpStrBufs[5][11];
        u8 ALIGNED(4) moveMaxPpStrBufs[5][11];
        u8 ALIGNED(4) moveNameStrBufs[5][MOVE_NAME_LENGTH + 1];
        u8 ALIGNED(4) movePowerStrBufs[5][5];
        u8 ALIGNED(4) moveAccuracyStrBufs[5][5];

        u8 ALIGNED(4) expPointsStrBuf[9];
        u8 ALIGNED(4) expToNextLevelStrBuf[9];

        u8 ALIGNED(4) abilityNameStrBuf[13];
        u8 ALIGNED(4) abilityDescStrBuf[52];
    } summary;

    u8 ALIGNED(4) isEgg; /* 0x3200 */
    u8 ALIGNED(4) isBadEgg; /* 0x3204 */

    u8 ALIGNED(4) mode; /* 0x3208 */
    u8 ALIGNED(4) unk320C; /* 0x320C */
    u8 ALIGNED(4) lastIndex; /* 0x3210 */
    u8 ALIGNED(4) curPageIndex; /* 0x3214 */
    u8 ALIGNED(4) unk3218; /* 0x3218 */
    u8 ALIGNED(4) isBoxMon; /* 0x321C */
    u8 ALIGNED(4) monTypes[2]; /* 0x3220 */

    u8 ALIGNED(4) pageFlipDirection; /* 0x3224 */
    u8 ALIGNED(4) unk3228; /* 0x3228 */
    u8 ALIGNED(4) unk322C; /* 0x322C */
    u8 ALIGNED(4) unk3230; /* 0x3230 */

    u8 ALIGNED(4) lockMovesFlag; /* 0x3234 */

    u8 ALIGNED(4) whichBgLayerToTranslate; /* 0x3238 */
    u8 ALIGNED(4) skillsPageBgNum; /* 0x323C */
    u8 ALIGNED(4) infoAndMovesPageBgNum; /* 0x3240 */
    u8 ALIGNED(4) flippingPages; /* 0x3244 */
    u8 ALIGNED(4) unk3248; /* 0x3248 */
    s16 ALIGNED(4) flipPagesBgHofs; /* 0x324C */

    u16 moveTypes[5]; /* 0x3250 */
    u16 moveIds[5]; /* 0x325A */
    u8 ALIGNED(4) numMoves; /* 0x3264 */
    u8 ALIGNED(4) isSwappingMoves; /* 0x3268 */

    u8 ALIGNED(4) curMonStatusAilment; /* 0x326C */

    u8 ALIGNED(4) state3270; /* 0x3270 */
    u8 ALIGNED(4) summarySetupStep; /* 0x3274 */
    u8 ALIGNED(4) loadBgGfxStep; /* 0x3278 */
    u8 ALIGNED(4) spriteCreationStep; /* 0x327C */
    u8 ALIGNED(4) bufferStringsStep; /* 0x3280 */
    u8 ALIGNED(4) state3284; /* 0x3284 */
    u8 ALIGNED(4) selectMoveInputHandlerState; /* 0x3288 */
    u8 ALIGNED(4) switchMonTaskState; /* 0x328C */

    struct Pokemon currentMon; /* 0x3290 */

    union
    {
        struct Pokemon * mons;
        struct BoxPokemon * boxMons;
    } monList;

    MainCallback savedCallback;
    struct Sprite *markingSprite;

    u8 ALIGNED(4) lastPageFlipDirection; /* 0x3300 */
    u8 ALIGNED(4) unk3304; /* 0x3304 */
};

struct Struct203B144
{
    u16 unk00;
    u16 curHpStr;
    u16 atkStr;
    u16 defStr;
    u16 spAStr;
    u16 spDStr;
    u16 speStr;
    u16 expStr;
    u16 toNextLevel;

    u16 curPp[5];
    u16 maxPp[5];

    u16 unk26;
};


#define POKESUM_WIN_RIGHT_PANE 3
u16 GetMonMoveBySlotId(struct Pokemon *mon, u8 moveSlot);
u16 GetMonPpByMoveSlot(struct Pokemon *mon, u8 moveSlot);
extern struct PokemonSummaryScreenData *sMonSummaryScreen;
extern struct Struct203B144 *sMonSkillsPrinterXpos;
extern const u8 sPrintMoveTextColors[][3];
#define GetRightAlignXpos_NDigits(a, b) ((6 * (a)) - StringLength((b)) * 6)
#define GetMoveNamePrinterYpos(x) ((x) * 28 + 5)
#define GetMovePpPrinterYpos(x) ((x) * 28 + 16)

static u8 *FormatCooldown(u8 *dst, u16 frames)
{
    u32 tenths = (frames * 10 + 30) / 60;
    dst = ConvertIntToDecimalStringN(dst, tenths / 10, STR_CONV_MODE_LEFT_ALIGN, 2);
    *dst++ = CHAR_PERIOD;
    dst = ConvertIntToDecimalStringN(dst, tenths % 10, STR_CONV_MODE_LEFT_ALIGN, 1);
    *dst++ = CHAR_s;
    *dst = EOS;
    return dst;
}

void RtLc_BufferMonMoveI(u8 i)
{
    if (i < 4)
        sMonSummaryScreen->moveIds[i] = GetMonMoveBySlotId(&sMonSummaryScreen->currentMon, i);

    if (sMonSummaryScreen->moveIds[i] == 0)
    {
        StringCopy(sMonSummaryScreen->summary.moveNameStrBufs[i], gText_PokeSum_OneHyphen);
        StringCopy(sMonSummaryScreen->summary.moveCurPpStrBufs[i], gText_PokeSum_TwoHyphens);
        StringCopy(sMonSummaryScreen->summary.movePowerStrBufs[i], gText_ThreeHyphens);
        StringCopy(sMonSummaryScreen->summary.moveAccuracyStrBufs[i], gText_ThreeHyphens);
        sMonSkillsPrinterXpos->curPp[i] = 0xff;
        sMonSkillsPrinterXpos->maxPp[i] = 0xff;
        return;
    }

    sMonSummaryScreen->numMoves++;
    sMonSummaryScreen->moveTypes[i] = gBattleMoves[sMonSummaryScreen->moveIds[i]].type;
    StringCopy(sMonSummaryScreen->summary.moveNameStrBufs[i], gMoveNames[sMonSummaryScreen->moveIds[i]]);

    // real-time battles use cooldowns instead of PP: the cooldown in seconds
    if (i >= 4 && sMonSummaryScreen->mode == PSS_MODE_SELECT_MOVE)
        FormatCooldown(sMonSummaryScreen->summary.moveCurPpStrBufs[i], RtBattle_GetMoveCooldownFrames(sMonSummaryScreen->moveIds[i], 0, 0));
    else
        FormatCooldown(sMonSummaryScreen->summary.moveCurPpStrBufs[i],
                       RtBattle_GetMoveCooldownFrames(sMonSummaryScreen->moveIds[i], GetMonData(&sMonSummaryScreen->currentMon, MON_DATA_PP_BONUSES), i));
    sMonSummaryScreen->summary.moveMaxPpStrBufs[i][0] = EOS;

    sMonSkillsPrinterXpos->curPp[i] = GetRightAlignXpos_NDigits(2, sMonSummaryScreen->summary.moveCurPpStrBufs[i]);
    sMonSkillsPrinterXpos->maxPp[i] = GetRightAlignXpos_NDigits(2, sMonSummaryScreen->summary.moveMaxPpStrBufs[i]);

    if (gBattleMoves[sMonSummaryScreen->moveIds[i]].power <= 1)
        StringCopy(sMonSummaryScreen->summary.movePowerStrBufs[i], gText_ThreeHyphens);
    else
        ConvertIntToDecimalStringN(sMonSummaryScreen->summary.movePowerStrBufs[i], gBattleMoves[sMonSummaryScreen->moveIds[i]].power, STR_CONV_MODE_RIGHT_ALIGN, 3);

    if (gBattleMoves[sMonSummaryScreen->moveIds[i]].accuracy == 0)
        StringCopy(sMonSummaryScreen->summary.moveAccuracyStrBufs[i], gText_ThreeHyphens);
    else
        ConvertIntToDecimalStringN(sMonSummaryScreen->summary.moveAccuracyStrBufs[i], gBattleMoves[sMonSummaryScreen->moveIds[i]].accuracy, STR_CONV_MODE_RIGHT_ALIGN, 3);
}

void RtLc_PokeSum_PrintMoveName(u8 i)
{
    u8 colorIdx = 0;
    u8 curPP = GetMonPpByMoveSlot(&sMonSummaryScreen->currentMon, i);
    u16 move = sMonSummaryScreen->moveIds[i];
    u8 ppBonuses = GetMonData(&sMonSummaryScreen->currentMon, MON_DATA_PP_BONUSES);
    u8 maxPP = CalculatePPWithBonus(move, ppBonuses, i);

    if (i == 4)
        curPP = maxPP;

    AddTextPrinterParameterized3(sMonSummaryScreen->windowIds[POKESUM_WIN_RIGHT_PANE], FONT_NORMAL, 3, GetMoveNamePrinterYpos(i), sPrintMoveTextColors[0], TEXT_SKIP_DRAW, sMonSummaryScreen->summary.moveNameStrBufs[i]);

    if (sMonSummaryScreen->moveIds[i] == 0 || (curPP == maxPP))
      colorIdx = 0;
    else if (curPP == 0)
      colorIdx = 3;
    else if (maxPP == 3)
    {
        if (curPP == 2)
          colorIdx = 2;
        else if (curPP == 1)
          colorIdx = 1;
    }
    else if (maxPP == 2)
    {
        if (curPP == 1)
          colorIdx = 1;
    }
    else
    {
        if (curPP <= (maxPP / 4))
          colorIdx = 2;
        else if (curPP <= (maxPP / 2))
          colorIdx = 1;
    }

    // cooldown (seconds) where the PP were, right-aligned to the old PP column
    colorIdx = 0;
    AddTextPrinterParameterized3(sMonSummaryScreen->windowIds[POKESUM_WIN_RIGHT_PANE], FONT_NORMAL,
        80 - GetStringWidth(FONT_NORMAL, sMonSummaryScreen->summary.moveCurPpStrBufs[i], 0), GetMovePpPrinterYpos(i),
        sPrintMoveTextColors[colorIdx], TEXT_SKIP_DRAW, sMonSummaryScreen->summary.moveCurPpStrBufs[i]);
}
