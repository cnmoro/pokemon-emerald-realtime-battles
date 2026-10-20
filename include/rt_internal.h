#ifndef GUARD_RT_INTERNAL_H
#define GUARD_RT_INTERNAL_H

#include "global.h"
#include "rt_battle.h"
#include "palette.h"
#include "trig.h"
#include "constants/rgb.h"
#include "util.h"
#include "constants/characters.h"

// ---------------------------------------------------------------------------
// Fixed point helpers (Q8)
// ---------------------------------------------------------------------------
#define FXS 8
#define RT_UI_TILE_BASE 256
#define RT_FX_TILE_BASE 304
#define TO_FX(v) ((s32)(v) << FXS)
#define FROM_FX(v) ((s32)(v) >> FXS)

// ---------------------------------------------------------------------------
// World / arena
// ---------------------------------------------------------------------------
#define ARENA_MW 40            // metatiles (the BG tilemap is a 32x16 ring that follows the camera)
#define ARENA_MH 22
#define FIELD_W 18             // battle field, metatiles (always this size)
#define FIELD_H 10
#define ARENA_W (ARENA_MW * 16) // pixels
#define ARENA_H (ARENA_MH * 16)
#define ARENA_BORDER 1         // metatiles of border on each side
#define RT_SOLID_WALK 1
#define RT_SOLID_SHOT 2
#define HUD_TOP 16
#define HUD_BOTTOM 16

#define MAX_OBSTACLES 24

struct RtObstacle
{
    s16 x0, y0, x1, y1; // pixels, AABB
};

// ---------------------------------------------------------------------------
// Sprite data generated from PMD SpriteCollab
// ---------------------------------------------------------------------------
enum
{
    RTA_WALK,
    RTA_IDLE,
    RTA_ATTACK,
    RTA_SHOOT,
    RTA_HURT,
    RTA_SLEEP,
    RTA_CHARGE,
    RTA_COUNT
};

struct RtFrame
{
    u32 offset;   // into gRtSpriteGfx (LZ77)
    s8 x, y;      // top-left relative to anchor
    u8 shapeSize; // shape << 2 | size
    u8 dur;
};

struct RtSpeciesGfx
{
    u16 start[RTA_COUNT];
    u8 count[RTA_COUNT];
    u8 hit[RTA_COUNT];
    u8 shadow;
    u8 radius;
    u8 height;
};

extern const u32 gRtSpriteGfx[];
extern const struct RtFrame gRtFrames[];
extern const struct RtSpeciesGfx gRtSpeciesGfx[];
extern const u16 gRtSpeciesPal[][2][16];

// ---------------------------------------------------------------------------
// FX atlas
// ---------------------------------------------------------------------------
struct RtFxShape
{
    u16 tile;      // tile offset inside atlas
    u8 shapeSize;  // shape << 2 | size
    u8 frames;
    u8 tilesPerFrame;
    u8 w, h;       // pixel dims
};

extern const u32 gRtFxAtlasGfx[];
extern const struct RtFxShape gRtFxShapes[];
extern const u16 gRtFxPalettes[][16];
extern const u16 gRtUiPalette[16];

#include "rt_fx_ids.h"

// impact styles
enum
{
    RTI_NONE,
    RTI_HIT,
    RTI_SLASH,
    RTI_BURN,
    RTI_SPLASH,
    RTI_LEAF,
    RTI_SHOCK,
    RTI_ROCK,
    RTI_ICE,
    RTI_PSY,
    RTI_DARK,
    RTI_POISON,
    RTI_STAR,
    RTI_HEART,
    RTI_WIND,
};

// ---------------------------------------------------------------------------
// Moves
// ---------------------------------------------------------------------------
enum
{
    RTP_SELF,       // buffs/heals/barriers on self
    RTP_MELEE,      // short arc in front
    RTP_RUSH,       // user charges forward
    RTP_BULLET,     // projectile(s)
    RTP_STREAM,     // continuous stream of short-lived hitboxes
    RTP_BEAM,       // line beam
    RTP_BURST,      // expanding area around user
    RTP_TARGETED,   // strike(s) at aim point after delay
    RTP_WAVE,       // wide traveling wave
    RTP_CONE,       // instant cone in front
    RTP_ZONE,       // lingering zone at aim point
    RTP_LEAP,       // jump/fly, then slam down at target point
    RTP_BURROW,     // go underground / dive, emerge under target
    RTP_ROLL,       // steerable rolling attack
    RTP_FIELD,      // whole-field effect (weather, haze...)
    RTP_COUNT
};

// RtMoveDef.flags
#define RTMF_HOMING    (1 << 0)
#define RTMF_PIERCE    (1 << 1)
#define RTMF_ARC       (1 << 2)   // lobbed (visual height)
#define RTMF_ROOT      (1 << 3)   // user cannot move while active
#define RTMF_FAN       (1 << 4)   // bullets in a fan instead of a sequence
#define RTMF_BOOMERANG (1 << 5)
#define RTMF_GROUND    (1 << 6)   // drawn under actors
#define RTMF_BLEND     (1 << 7)   // semi-transparent visuals

struct RtMoveDef
{
    u8 pattern;
    u8 shape;
    u8 pal;
    u8 speed;     // px/frame * 16
    u8 range;     // px / 2
    u8 radius;    // px
    u8 count;
    u8 spread;
    u8 windup;
    u8 duration;
    u8 trail;
    u8 impact;
    u8 flags;
    u8 anim;
    u8 aux;
    u8 pad;
    u16 sfx;
};

extern const struct RtMoveDef gRtMoveDefs[];

// ---------------------------------------------------------------------------
// Actors
// ---------------------------------------------------------------------------
#define RT_MAX_ACTORS 4
#define RT_SIDE_PLAYER 0
#define RT_SIDE_ENEMY 1

enum
{
    AST_NONE,
    AST_ENTER,      // being sent out
    AST_NORMAL,     // free to act
    AST_CAST,       // executing a move (anim lock)
    AST_DASH,
    AST_HURT,
    AST_STUN,       // sleep/freeze/paralysis/flinch/recharge
    AST_FAINT,
    AST_RETURN,     // being withdrawn
    AST_GONE,       // not on field (inside ball / off field)
    AST_CAUGHT,     // inside a thrown ball
};

// stat stage indices
enum
{
    STG_ATK,
    STG_DEF,
    STG_SPE,
    STG_SPA,
    STG_SPD,
    STG_ACC,
    STG_EVA,
    STG_COUNT
};

// major status (mirrors game)
enum
{
    MST_NONE,
    MST_SLP,
    MST_PSN,
    MST_BRN,
    MST_FRZ,
    MST_PAR,
    MST_TOX,
};

struct RtActiveMove
{
    u16 move;
    u8 slot;       // 0-3, 0xFF for called moves
    u8 phase;
    u16 timer;
    u8 angle;
    u8 count;
    s32 tx, ty;    // target point (FX)
    u8 hitMask;
    u8 targetId;
    s32 sx, sy;    // start pos
    u8 castId;
    u8 hitsDone;
    u8 hitsWanted;
    u8 subTimer;
    u16 power;
    u16 calledFrom;
    u8 lunge;       // melee: frames of the closing dash left before the strike
    u8 lungeTarget;
    u8 aimBase;     // direction the move was cast in (beams / streams can only be turned a little from it)
};

struct RtActor
{
    u8 active;
    u8 side;
    u8 partyIdx;     // index in party array
    u8 id;           // index in actor array
    struct Pokemon *mon;
    u16 species;
    u16 dex;
    u16 gfxDex;      // may differ while transformed
    u8 shiny;
    u8 state;
    u16 stateTimer;

    // physics
    s32 x, y;        // FX world position (anchor at feet)
    s32 vx, vy;      // FX knockback velocity
    s16 z;           // pixels above ground
    u8 facing;       // 0-7 (PMD order)
    u8 aim;          // 0-255 angle
    u8 radius;
    u8 height;
    u8 hidden;       // not drawn (underground / in air)
    u8 invuln;       // semi-invulnerable kind: 0 none, 1 air, 2 underground, 3 underwater

    // animation
    u8 anim;
    u8 animFrame;
    u8 animTimer;
    u8 animLoop;
    u16 curFrame;    // global frame index loaded
    u8 needLoad;
    u8 flash;        // white flash frames
    u8 palDirty;

    // stats
    u8 level;
    u8 types[2];
    u8 ability;
    u8 holdEffect;
    u8 holdParam;
    u16 heldItem;
    u16 hp, maxHp;
    u16 atk, def, spe, spa, spd;
    s8 stages[STG_COUNT];
    u16 moves[MAX_MON_MOVES];
    u16 cd[MAX_MON_MOVES];
    u16 cdMax[MAX_MON_MOVES];
    u16 gcd;
    u16 dashCd;
    u16 iframes;

    // major status
    u8 mstatus;
    u16 mstatusTimer;
    u16 dotTimer;
    u8 toxicN;

    // volatile effects (timers in frames)
    u16 confuse;
    u16 flinch;
    u16 recharge;
    u16 trap;          // bound by Wrap etc.
    u8 trapBy;
    u16 trapMove;
    u16 leechSeed;
    u8 seededBy;
    u16 taunt;
    u16 disable;
    u16 disabledMove;
    u16 encore;
    u8 encoreSlot;
    u16 torment;
    u16 lastMove;
    u16 yawn;
    u16 perish;
    u16 protect;
    u8 protectKind;    // 0 protect/detect, 1 endure
    u8 protectChain;
    u32 protectLastFrame; // gRt->frame of the last Protect / Detect (the chain resets after 3 s)
    u16 magicCoat;
    u16 snatch;
    u16 lockOn;
    u16 charge;        // Charge move boost
    u16 helpingHand;
    u16 rampage;
    u16 rampageMove;
    u16 uproar;
    u16 bide;
    u16 bideDmg;
    u16 counterWin;    // counter / mirror coat window
    u8 counterKind;
    u16 destinyBond;
    u16 grudge;
    u16 rage;
    u16 ingrain;
    u16 nightmare;
    u16 cursed;
    u16 substituteHp;
    u16 minimize;
    u16 foresight;
    u16 stockpile;
    u16 defenseCurl;
    u16 focusEnergy;
    u16 attract;
    u8 attractBy;
    u16 flashFire;
    u16 meanLook;
    u16 imprison;
    u16 rolloutHits;
    u16 furyCutter;
    u16 transformSpecies;
    u16 mudSport, waterSport;
    u16 choiceMove;
    u16 truantTimer;
    u16 speedBoostTimer;
    u16 regenTimer;
    u16 shedSkinTimer;
    u16 slowTimer;     // cobweb / string shot slow
    u16 blindTimer;    // accuracy debuff visual
    u16 confuseSelfHitCd;
    u8 participated;   // bitmask of enemy actors faced (for exp)
    u8 lastHitBy;
    u16 lastDamageTaken;
    u8 lastDamageCat;  // 0 phys 1 spec
    u16 damageTakenThisCast;
    u8 wishPending;
    u16 consumedItem;
    u32 sentOutFrame;
    u8 followMe;
    u8 castSerial;
    u8 lastHitTimer;
    u8 enteredAbilityDone;

    // control
    s16 inMoveX, inMoveY; // desired direction (-256..256)
    s8 inSlot;           // move slot to cast (-1 none)
    u8 inDash;
    u8 inAim;             // aim angle requested
    u8 aiming;            // player: holding a move button
    u8 aimHold;

    struct RtActiveMove act;

    // AI
    u8 aiState;
    u16 aiTimer;
    u8 aiTarget;
    s8 aiStrafe;
    u8 aiSkill;           // 0..10
    u16 aiDodgeCd;
    u16 aiThinkCd;
    u16 aiItemCd;
    s16 aiGoalX, aiGoalY;

    // render
    u8 vramSlot;
    u8 palSlot;
    u8 hpBarDirty;
    u16 shownHp;
    u8 aiNoLine;      // AI: no clear shot line to its target
    u8 aiPathOn, aiPathAng, aiPathAge;
    u8 leftoversShown;   // the LEFTOVERS message was shown for this send-out
};

// ---------------------------------------------------------------------------
// Hitboxes / projectiles
// ---------------------------------------------------------------------------
#define RT_MAX_PROJ 40

enum
{
    PK_NONE,
    PK_BULLET,
    PK_MELEE,     // short-lived hitbox following owner
    PK_AREA,      // expanding circle
    PK_STRIKE,    // delayed area strike at point
    PK_BEAM,      // line from owner
    PK_WAVE,      // wide moving segment
    PK_ZONE,      // lingering area
    PK_BALL,      // thrown poké ball
    PK_RUSHBOX,   // hitbox attached to rushing owner
    PK_FUTURE,    // delayed strike (future sight / doom desire)
    PK_SAFARI,    // safari bait / mud
};

struct RtProj
{
    u8 kind;
    u8 owner;
    u8 side;
    u8 hitMask;
    u16 move;
    u16 life;
    u16 age;
    s32 x, y, vx, vy;
    s16 z;
    u8 angle;
    u8 radius;
    u8 maxRadius;
    u8 shape;
    u8 pal;
    u8 frame;
    u8 flags;
    u8 delay;
    u8 trail;
    u8 homingTarget;
    u16 param;
    u16 param2;
    u16 len;       // beam length / wave half-width (px)
    u16 power;     // pre-computed for future sight etc
};

// ---------------------------------------------------------------------------
// Particles (visual only)
// ---------------------------------------------------------------------------
#define RT_MAX_PART 128

struct RtPart
{
    u8 shape;
    u8 pal;
    u8 life;
    u8 maxLife;
    s32 x, y;
    s16 vx, vy;      // Q8 px/frame
    s16 z, vz;       // pixels, Q8 vz
    s8 grav;
    u8 frameRate;
    u8 flags;        // PF_*
    u8 affine;       // 0 none, else matrix idx+1
    u8 angle;
    u8 scale;        // Q4 (16 = 1.0)
    s8 dscale;
    u8 prio;         // draw bucket: 0 hud, 1 above actors, 3 ground
    u8 frame;        // fixed frame when frameRate == 0
};

#define PF_BLEND   (1 << 0)
#define PF_GROUND  (1 << 1)
#define PF_ROTATE  (1 << 2)
#define PF_SCALE   (1 << 3)
#define PF_HUD     (1 << 4)
#define PF_FLICKER (1 << 5)
#define PF_HFLIP   (1 << 6)
#define PF_FIXED   (1 << 7)  // screen space

// extra particle parameters (gRt->partsX[i] mirrors gRt->parts[i])
struct RtPartX
{
    u8 pal2, pal3;   // palette changes over life (0xFF = none)
    u8 at2, at3;     // ...when life drops to these values
    u8 flags;        // PX_*
    s8 spin;         // affine angle per frame
    u8 drag;         // 0 = default (15/16); else velocity *= drag/16
    s8 ax, ay;       // acceleration Q8 px/frame^2 (/16)
    u8 orbit;        // orbit radius for PX_ORBIT
    u8 orbitAng;
    u8 owner;        // actor id for PX_ORBIT / PX_FOLLOW
};
#define PX_ORBIT    (1 << 0)  // circles around its owner
#define PX_FOLLOW   (1 << 1)  // position is relative to its owner
#define PX_HOMEIN   (1 << 2)  // drifts toward its owner (absorb effects)
#define PX_STRETCH  (1 << 3)  // oriented along its velocity
#define PX_FADE     (1 << 4)  // semi-transparent in the last third of its life
#define PX_HOLD     (1 << 5)  // animation stops on its last frame instead of looping

// ---------------------------------------------------------------------------
// Global battle state
// ---------------------------------------------------------------------------
enum
{
    RTS_INIT,
    RTS_INTRO,
    RTS_FIGHT,
    RTS_MENU,
    RTS_PROMPT,
    RTS_OUTRO,
    RTS_EXIT,
};

#define WEATHER_NONE_RT 0
#define WEATHER_RAIN_RT 1
#define WEATHER_SUN_RT 2
#define WEATHER_SAND_RT 3
#define WEATHER_HAIL_RT 4

struct RtToast
{
    u8 active;
    u16 timer;
};

struct RtSideState
{
    u16 reflect;
    u16 lightScreen;
    u16 mist;
    u16 safeguard;
    u8 spikes;           // layers
    u16 wish;            // timer
    u16 wishHp;
    u8 wishTarget;
};

struct RtFutureHit
{
    u8 active;
    u8 owner;
    u16 move;
    u16 timer;
    s32 x, y;
    u16 dmg;
};

#define RT_MAX_PROPS 6
enum
{
    PROP_NONE,
    PROP_ROCK,
    PROP_TREE,
    PROP_BOULDER,
    PROP_BOX,
};
enum
{
    PRS_REST,
    PRS_LIFTED,
    PRS_BREAKING,
};
struct RtProp
{
    u8 kind;
    u8 state;
    u8 hp;
    u8 owner;
    u8 timer;
    u8 hurtCd;
    u16 move;
    s32 x, y;     // Q8, feet
    s16 z;
};

// ---------------------------------------------------------------------------
// Extended mechanics: ki, SUPER form, beam struggle (see rt_ext.c / rt_moves.c)
// ---------------------------------------------------------------------------
#define RT_KI_MAX 1000
#define RT_STR_MAX 600     // beam struggle balance needed to win

struct RtActorX
{
    u16 ki;            // 0..RT_KI_MAX
    u16 superTimer;    // frames of SUPER form left (0 = normal)
    u16 chargeHold;    // frames SELECT has been held (transformation)
    u16 buffTimer;     // objective buff frames left
    u8 buffKind;
    u8 superKind;      // Deoxys forme: 1 attack, 2 defense, 3 speed
    u8 kiFlash;        // HUD: frames to flash the ki bar
    u16 oldGfx;        // gfx index before a form change
    u8 oldType[2];     // types before a form change
};

struct RtStruggle
{
    u8 on;
    u8 projA, projB;   // indices into gRt->proj
    u8 ownA, ownB;     // actor ids
    s16 bal;           // + = A winning
    u16 timer;
    s32 px, py;        // clash point (Q8)
    u8 pressA, pressB; // presses this frame
    u8 mashA, mashB;   // smoothed mash rate (HUD)
};

struct RtBattle
{
    u8 state;
    u8 subState;
    u16 timer;
    u32 frame;
    u8 isTrainer;
    u8 isDouble;
    u8 isSafari;
    u8 twoOpponents;
    u8 partner;          // in-game partner battle
    u8 outcome;
    u8 fought;          // player party slots that took part in the battle (bit per slot)
    u8 weather;
    u16 weatherTimer;
    u8 arenaTheme;
    s16 camX, camY;
    s16 shakeTimer;
    u8 shakeMag;
    u8 flashTimer;
    u8 flashColor;
    u8 slowmo;
    u16 pauseFrames;     // hit-stop

    struct RtActor actors[RT_MAX_ACTORS];
    struct RtProj proj[RT_MAX_PROJ];
    struct RtPart parts[RT_MAX_PART];
    struct RtSideState sides[2];
    struct RtFutureHit future[4];

    u16 arenaMeta[ARENA_MH][ARENA_MW];
    u8 solid[ARENA_MH][ARENA_MW];
    u8 floorKind;        // 0 ground, 1 water, 2 tallgrass...
    s16 viewX0, viewY0, viewX1, viewY1; // camera limits (px)
    s16 playX0, playY0, playX1, playY1; // the playable field (px); the rest is dimmed scenery

    // party bookkeeping
    struct Pokemon *playerParty;
    struct Pokemon *enemyParty;
    u8 playerLead;       // actor index of controlled mon
    u8 enemyUsed;        // bitmask of enemy party mons sent out
    u8 expShareMask;
    u16 payDay;
    u8 runAttempts;
    u8 caught;
    u8 nickPending;     // the caught POKeMON gets its nickname after the battle is torn down
    u16 ballItem;
    u8 ballReady;        // player has a ball equipped to throw
    u8 safariBalls;
    u8 safariCatchMod;   // modifies catch rate (rock/bait)
    u8 safariFleeMod;
    u16 safariFleeTimer;
    u8 lastUsedMoveSide[2];
    u16 lastMoveUsed;    // global (for Mirror Move fallback)
    u8 lostItems[PARTY_SIZE];

    // pending
    u8 pendingLearnMon;
    u16 pendingLearnMoves[8];
    u8 pendingLearnMons[8];
    u8 pendingLearnCount;
    u8 switchRequest;     // player must choose next mon (party idx or 0xFF)
    u8 forcedSwitch;
    u8 menuReturnState;

    // FX palette slots
    u8 fxPalOwner[11];    // which fx pal id is loaded in slot 5+i (0xFF free)
    u8 fxPalRef[11];
    u16 fxPalLastUse[11];

    // UI
    u8 hudDirty;
    u8 moveHudDirty;
    struct RtToast toast;
    u8 menuCursor;
    u8 menuPage;
    u8 menuScroll;
    u8 menuKind;
    u16 menuItems[40];
    u8 menuCount;
    u8 targetEnemy;      // which enemy shown in HUD

    // frame staging buffer for actor gfx
    u32 gfxBuf[RT_MAX_ACTORS][512];
    u32 decompBuf[528];
    u8 gfxPending[RT_MAX_ACTORS];
    u16 gfxPendingSize[RT_MAX_ACTORS];

    // affine matrices allocation this frame
    u8 affineUsed;

    u16 bg0Tilemap[32 * 32];

    // arena props (rocks, cuttable trees, boulders, boxes)
    struct RtProp props[RT_MAX_PROPS];

    // scratch memory of the arena builder and the AI path finder (kept off EWRAM statics)
    struct
    {
        u8 hgt[80];
        u8 ledge[ARENA_MH][ARENA_MW];
        u32 cost[(ARENA_MH + 1) * (ARENA_MW + 1)];
        u8 clean[ARENA_MH][ARENA_MW];
        u8 objId[ARENA_MH][ARENA_MW];
        u16 objSize[256];
        u8 objEdge[256];
        u8 objErase[256];
        u16 objFill[256];  // ground that replaces an erased object (the one around it)
        u8 objVote[256];
        u8 erased[ARENA_MH][ARENA_MW];
        u8 group[ARENA_MH * ARENA_MW];
        u8 seen[ARENA_MH * ARENA_MW];
        u8 pathDist[4][ARENA_MH * ARENA_MW];
        u8 walkPad[(ARENA_MW + 2) * (ARENA_MH + 2)];
        u16 pathQueue[ARENA_MH * ARENA_MW];
        u8 pathPad[(ARENA_MW + 2) * (ARENA_MH + 2)];
    } scr;

    u32 expStart[PARTY_SIZE]; // party EXP when the battle started (for the EXP panel at the end)
    u8 expPanelDone;

    // --- visual layer (keep new fields at the end: test scripts use fixed offsets above) ---
    struct RtPartX partsX[RT_MAX_PART]; // extra per-particle parameters (same index as parts[])
    u8 dimLevel;         // dramatic darkening of the arena (0..10), decays
    u8 dimHold;          // frames to hold dimLevel before it decays
    u8 waveAmp;          // psychic heat-haze distortion amplitude (px)
    u8 waveTimer;        // frames left
    u8 vfxPalCycle;      // misc frame counter for palette flicker
    u8 sideDim;          // dither-dim the scenery left/right of the field (no ring of border blocks there)
    u8 ballFails;        // balls that hit but broke free this battle: each one raises the next catch chance
    u8 simAcc;           // battle pace: the fight simulation advances RtBattleSpeedPct() of every 100 frames
    bool8 simStep;       // this frame advances the fight (FALSE: only camera / HUD / drawing)
    u16 latchNew, latchNewRaw; // buttons pressed on frames that did not advance the fight
    // smooth motion below 100% BATTLE SPEED: actors and shots are drawn between their position before
    // and after the last fight step (lerpT % of the way), so frames without a step still move
    s32 lerpActX[RT_MAX_ACTORS], lerpActY[RT_MAX_ACTORS];
    s32 lerpProjX[RT_MAX_PROJ], lerpProjY[RT_MAX_PROJ];
    u8 lerpT;            // 100 = draw the current positions
    u16 lerpQ8;          // lerpT as 0..256

    // --- extended mechanics (always at the end) ---
    struct RtActorX ax[RT_MAX_ACTORS];
    u8 terrain[ARENA_MH][ARENA_MW];   // RTT_*
    u8 grpId[ARENA_MH][ARENA_MW];     // breakable obstacle group of every cell (0: none)
    u8 grpHp[24];
    u16 grpHit[24];
    u16 floorMeta;
    u8 terrainShown;
    s16 wx0, wy0;                      // metatile origin of the streamed BG window
    u8 fieldW, fieldH;                 // battle field size in metatiles (0: default)
    u8 objective;                      // 1..8: guardian battle of a biome
    u8 objBuffShown;
    
    struct RtStruggle str;
};

// battle pace (OPTION menu, BATTLE SPEED): percentage of frames that advance the fight
u8 RtBattleSpeedPct(void);

extern struct RtBattle *gRt;

// ---------------------------------------------------------------------------
// Functions shared between modules
// ---------------------------------------------------------------------------
// rt_arena.c
void RtArena_Build(void);
void RtArena_ClampCamera(s32 *tx, s32 *ty);
bool8 RtArena_CircleBlocked(s32 x, s32 y, s32 r);
bool8 RtArena_PointSolid(s32 px, s32 py);
bool8 RtArena_PointBlocksShot(s32 px, s32 py);
bool8 RtArena_ShotClear(s32 x0, s32 y0, s32 x1, s32 y1);
bool8 RtArena_PathAngle(s32 fx, s32 fy, s32 tx, s32 ty, s32 r, u8 *ang);
void RtArena_ResolveActor(struct RtActor *a);
// rt_props.c
void RtProps_Place(void);
void RtProps_Update(void);
bool8 RtProps_CircleBlocked(s32 x, s32 y, s32 r);
void RtProps_PushActor(struct RtActor *a);
bool8 RtProps_HitAt(s32 fx, s32 fy, s32 r, u8 dmg);
bool8 RtProps_TryGrab(struct RtActor *a, u16 move);
void RtProps_Debris(u8 kind, s32 x, s32 y);
void RtProps_DrawOne(u8 i);
u8 RtProps_ShapeOf(u8 kind);
u8 RtProps_PalOf(u8 kind);
// rt_moves.c
void RtMove_ThrowProp(struct RtActor *a, u16 move, u8 kind, s32 x, s32 y, s16 z);
void RtArena_FindSpawn(u8 side, u8 index, s32 *x, s32 *y);

// rt_actor.c
void RtActor_InitFromMon(struct RtActor *a, struct Pokemon *mon, u8 side, u8 partyIdx, u8 slot);
void RtActor_SetAnim(struct RtActor *a, u8 anim, bool8 loop);
void RtActor_Update(struct RtActor *a);
void RtActor_UpdateAnim(struct RtActor *a);
void RtActor_LoadGfxIfNeeded(struct RtActor *a);
void RtActor_LoadPalette(struct RtActor *a);
void RtActor_SyncToMon(struct RtActor *a);
u16 RtActor_GetStat(struct RtActor *a, u8 stat);
u8 RtActor_SpeedPx(struct RtActor *a);
bool8 RtActor_CanAct(struct RtActor *a);
bool8 RtActor_IsAlive(struct RtActor *a);
bool8 RtActor_HasType(struct RtActor *a, u8 type);
void RtActor_ApplyStatus(struct RtActor *a, u8 status, u8 src);
void RtActor_TickStatus(struct RtActor *a);
void RtActor_Faint(struct RtActor *a);
void RtActor_Heal(struct RtActor *a, s32 amount);
void RtActor_Damage(struct RtActor *a, s32 amount, u8 src, bool8 direct);
bool8 RtActor_ChangeStage(struct RtActor *a, u8 stage, s8 delta, u8 src, bool8 showFx);
void RtActor_SetFacingFromAngle(struct RtActor *a, u8 angle);
u8 RtDirToAngle(u8 dir);
u8 RtAngleToDir(u8 angle);
u8 RtAtan2(s32 dx, s32 dy);
s32 RtDist(s32 dx, s32 dy);
s32 RtCos(u8 a);
s32 RtSin(u8 a);
void RtActor_Draw(struct RtActor *a);
void RtActor_Withdraw(struct RtActor *a);
void RtActor_SendOut(struct RtActor *a, u8 partyIdx, bool8 instant);
u8 RtActor_MoveSlotUsable(struct RtActor *a, u8 slot);
bool8 RtMove_ContactArmor(struct RtActor *a);
bool8 RtActor_StatusMove(struct RtActor *a, u16 move);

// rt_moves.c
bool8 RtMove_TryStart(struct RtActor *a, u8 slot, u8 angle);
void RtMove_StartCalled(struct RtActor *a, u16 move, u8 angle);
void RtMove_UpdateCast(struct RtActor *a);
void RtProj_UpdateAll(void);
struct RtProj *RtProj_Alloc(void);
bool8 RtMove_HasShotInFlight(u8 owner);
u8 RtMove_TypeOf(struct RtActor *a, u16 move);
u16 RtMove_Cooldown(u16 move, u8 ppBonuses, u8 slot);
s32 RtMove_CalcDamage(struct RtActor *atk, struct RtActor *def, u16 move, u16 power, u8 *outEff, u8 *outCrit);
void RtMove_OnActorHit(struct RtActor *atk, struct RtActor *def, u16 move, struct RtProj *p);
void RtMove_ApplySelfEffect(struct RtActor *a, u16 move);
u8 RtMove_GetRange(u16 move);
bool8 RtMove_IsOffensive(u16 move);
u16 RtMove_PickMetronome(void);
void RtMove_TickFuture(void);
void RtMove_DoFieldTick(void);
void RtMove_ThrowBall(struct RtActor *a, u16 ball, u8 angle);
u8 RtTypeEffectiveness(u8 atkType, struct RtActor *def);

// rt_fx.c
void RtFx_Init(void);
struct RtPart *RtFx_Spawn(u8 shape, u8 pal, s32 x, s32 y, u8 life);
void RtFx_Burst(u8 shape, u8 pal, s32 x, s32 y, u8 n, u8 speed, u8 life);
void RtFx_Ring(u8 shape, u8 pal, s32 x, s32 y, u8 n, u8 radius, u8 life);
void RtFx_Impact(u8 style, u8 pal, s32 x, s32 y);
void RtFx_Number(s32 x, s32 y, u16 value, u8 color);
void RtFx_Word(s32 x, s32 y, u8 word);
void RtFx_UpdateAll(void);
void RtFx_LineVBlank(void);
void RtFx_DynReset(void);
void RtFx_LineStop(void);
void RtFx_DrawEdges(void);
#define RFD_BLEND  1
#define RFD_HFLIP  2
#define RFD_AFFINE 4
bool8 RtFx_Draw(u8 shape, u8 frame, u8 pal, s32 sx, s32 sy, u8 bucket, u8 angle, u8 scale, u8 flags);
void RtFx_Dim(u8 level, u8 hold);
void RtFx_Wave(u8 amp, u8 frames);
struct RtPartX *RtFx_X(struct RtPart *p);
void RtActor_DrawGhost(struct RtActor *a, s32 dx, s32 dy);
void RtFx_DrawAll(void);
void RtLerp_ActorDelta(u8 actor, s32 *dx, s32 *dy); // drawn minus real position while drawing
u8 RtFx_GetPalSlot(u8 fxPal);
void RtFx_ReleaseUnused(void);
void RtFx_Shake(u8 mag, u8 frames);
void RtFx_Flash(u8 color, u8 frames);
void RtFx_StatArrows(struct RtActor *a, bool8 up);
void RtFx_DrawProjectiles(void);
void RtFx_WeatherUpdate(void);
u8 RtFx_AllocAffine(u8 angle, u8 scaleQ4, bool8 hflip);
void RtFx_ResetAffine(void);
void RtOam_Reset(void);
struct OamData *RtOam_Next(u8 prio);
void RtOam_Commit(void);
s16 RtWorldToScreenX(s32 fx);
s16 RtWorldToScreenY(s32 fx);

// rt_ext.c: ki, SUPER form, beam struggle helpers
void RtKi_InitActor(struct RtActor *a);
u16 RtKi_MoveCost(u16 move);
bool8 RtKi_Spend(struct RtActor *a, u16 cost);
void RtKi_Gain(struct RtActor *a, u16 amount);
void RtKi_OnDamage(struct RtActor *a, s32 amount, u8 src);
void RtKi_Tick(struct RtActor *a);
bool8 RtKi_CanCast(struct RtActor *a, u16 move);
u16 RtKi_DashCost(void);
struct RtSave { u16 magic; u8 ver; u8 pad; u8 meta[64]; };
struct RtSave *RtSave_Get(void);
bool8 RtExt_StartPoll(void);
bool8 RtExt_TryEvolve(struct RtActor *a, struct Pokemon *mon);
u16 RtKi_RuneGainPct(struct RtActor *a);
enum { RTT_NONE, RTT_WATER, RTT_SAND, RTT_GRASS, RTT_ICE };
u8 RtExt_TerrainAt(struct RtActor *a);
u16 RtExt_TerrainDmgPct(struct RtActor *atk, struct RtActor *def, u8 type);
u16 RtExt_TerrainSpeedPct(struct RtActor *a);
void RtExt_TerrainTick(struct RtActor *a);
void RtArena_StreamUpdate(void);
void RtExt_SetupObjective(void);
void RtExt_OnBattleEnd(u8 outcome);
void RtExt_GuardianInit(struct RtActor *a);
u8 RtExt_BuffId(void);
u8 RtExt_BuffLeft(void);
bool8 RtArena_HitCells(s32 fx, s32 fy, s32 r, u8 dmg);
void RtExt_OnHit(struct RtActor *victim, s32 *amount, u8 src);
u16 RtExt_StatMul(struct RtActor *a, u8 stat);
bool8 RtExt_TrySuper(struct RtActor *a);
void RtExt_PlayerInput(struct RtActor *a, u16 held, u16 pressed);
bool8 RtStruggle_Involves(u8 actorId);
void RtStruggle_Press(u8 actorId);

// rt_ai.c
void RtAi_Init(struct RtActor *a);
void RtAi_Update(struct RtActor *a);
void RtAi_TrainerItems(void);

// rt_ui.c
void RtUi_Init(void);
void RtUi_Update(void);
void Rt_StopLoopSE(void);
void Rt_UpdateLoopSE(void);
void RtUi_DrawObjs(void);
void RtUi_Toast(const u8 *str, u16 frames);
void RtUi_ToastFmt(const u8 *tmpl, u16 frames);
void RtUi_RefreshHud(void);
u16 RtFrameLinesUsed(void);
void RtUi_RefreshMoves(void);
void RtUi_OpenPauseMenu(void);
bool8 RtUi_UpdateMenu(void);   // returns TRUE when closed
void RtUi_OpenLearnPrompt(void);
bool8 RtUi_UpdatePrompt(void);
void RtUi_OpenPartyForced(void);
void RtUi_ShowMessageBox(const u8 *str);
bool8 RtUi_MessageBoxDone(void);
bool8 RtUi_ExpPanelStart(void);   // FALSE when nobody gained EXP
bool8 RtUi_ExpPanelUpdate(void);  // TRUE when closed
void RtUi_HideMessageBox(void);
void RtUi_HideToast(void);

// rt_main.c
struct RtActor *Rt_PlayerActor(void);
struct RtActor *Rt_GetActor(u8 id);
struct RtActor *Rt_NearestFoe(struct RtActor *a);
void Rt_OnActorFainted(struct RtActor *a);
void Rt_PlayerSwitchTo(u8 partyIdx);
bool8 Rt_TryRun(void);
void Rt_EndBattle(u8 outcome);
bool8 Rt_IsActorOnField(struct RtActor *a);
void Rt_GiveExpForFaint(struct RtActor *fainted);
void Rt_HandleCaught(void);
u8 Rt_CountUsableParty(u8 side);
void Rt_EnemySendNext(struct RtActor *slot);
u16 Rt_Random(u16 n);

// shared strings
extern const u8 gRtText_Empty[];

#endif // GUARD_RT_INTERNAL_H
