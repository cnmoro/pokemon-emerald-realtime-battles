#ifndef GUARD_RT_VFX_H
#define GUARD_RT_VFX_H

// Visual layer of the real-time battle engine: per-move "looks" on top of the gameplay hitboxes of rt_moves.c.
// Gameplay never depends on anything here.

enum
{
    VS_NONE,
    // projectiles
    VS_ORB, VS_FIREBALL, VS_ELECSHOT, VS_DARKORB, VS_GOO, VS_LEAF, VS_STAR, VS_SHARD, VS_ROCK, VS_BLADE,
    VS_WATERBALL, VS_NEEDLE, VS_TWISTER, VS_BUBBLE, VS_MUDBALL,
    // streams
    VS_FLAMES, VS_HELIX, VS_WATERJET, VS_BUBBLES, VS_DBREATH, VS_GAS, VS_POWDERS, VS_SNOWS, VS_SANDS,
    // beams
    VS_LIGHTNING, VS_BEAM, VS_ICEBEAM, VS_AURORA, VS_PSYBEAM, VS_SIGNAL, VS_SOLAR, VS_HYPER,
    // strikes from above
    VS_THUNDER, VS_ROCKFALL, VS_BLIZZARD, VS_METEOR, VS_PILLAR, VS_PSYBLAST,
    // melee
    VS_SLASH, VS_CLAW, VS_BITE, VS_PUNCH, VS_KICK, VS_HORN,
    // rushes
    VS_DASH,
    // bursts
    VS_QUAKE, VS_EXPLODE, VS_NOVA, VS_PETALS,
    // self
    VS_AURA, VS_SHIELD, VS_WALL, VS_HEAL,
    // waves
    VS_SURF, VS_SOUND, VS_HEAT,
    // cones, zones, landings, field moves, status projectiles
    VS_SPRAY, VS_SHOUT, VS_GAZE, VS_EMBLEM, VS_VORTEX, VS_CLOUD, VS_LAND, VS_WEATHER,
    VS_COUNT
};

enum
{
    VI_DEFAULT, VI_FIRE, VI_ELEC, VI_WATER, VI_GRASS, VI_ICE, VI_ROCK, VI_PSY, VI_GHOST, VI_DARK, VI_FIGHT,
    VI_NORMAL, VI_POISON, VI_DRAGON, VI_STEEL, VI_BUG, VI_FLYING, VI_GROUND, VI_NONE,
    VI_COUNT
};

#define VF_SHAKE1 (1 << 0)
#define VF_SHAKE2 (1 << 1)
#define VF_DIM    (1 << 2)  // the arena darkens while the move goes off
#define VF_WAVE   (1 << 3)  // heat-haze / psychic distortion of the arena
#define VF_FLASH  (1 << 4)
#define VF_BIG    (1 << 5)  // larger visuals
#define VF_HEAVY  (1 << 6)  // heavier impact (more particles, hit pause)

struct RtVfxDef
{
    u8 style;
    u8 pal;    // main colour (FXP_*)
    u8 pal2;   // secondary colour
    u8 impact; // VI_*
    u8 flags;  // VF_*
};

extern const struct RtVfxDef gRtVfxDefs[];

struct RtActor;
struct RtProj;

bool8 RtVfx_Charge(struct RtActor *a, u16 move, u16 t, u16 windup);
void RtVfx_Execute(struct RtActor *a, u16 move);
void RtVfx_StreamTick(struct RtActor *a, u16 move, u16 t);
bool8 RtVfx_ProjTrail(struct RtProj *p);
bool8 RtVfx_DrawProj(struct RtProj *p, s32 sx, s32 sy);
bool8 RtVfx_BeamTick(struct RtProj *p);
void RtVfx_Impact(u16 move, u8 style, u8 pal, s32 x, s32 y);
void RtVfx_SetHit(u8 atk, u8 tgt);
bool8 RtVfx_Strike(struct RtProj *p);
bool8 RtVfx_StrikeWarn(struct RtProj *p);
bool8 RtVfx_AreaTick(struct RtProj *p);
bool8 RtVfx_SelfCast(struct RtActor *a, u16 move);
bool8 RtVfx_RushTick(struct RtActor *a, u16 move);
bool8 RtVfx_WaveTick(struct RtProj *p);
void RtVfx_StatusTick(struct RtActor *a);
void RtVfx_BideTick(struct RtActor *a, u16 t);
bool8 RtVfx_Cone(struct RtActor *a, u16 move);
bool8 RtVfx_ZoneTick(struct RtProj *p);
void RtVfx_TossGrab(struct RtActor *a, struct RtActor *t);
void RtVfx_TossRise(struct RtActor *a, struct RtActor *t, s32 j);
void RtVfx_TossTrail(struct RtActor *t, u8 ang);
void RtVfx_TossSlam(struct RtActor *t);
void RtVfx_Ghosts(struct RtActor *a, u8 n, u8 life);
void RtVfx_DrawGhosts(void);
void RtVfx_ResetGhosts(void);

#endif // GUARD_RT_VFX_H
