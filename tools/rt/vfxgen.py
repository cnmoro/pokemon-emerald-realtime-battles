import os, re, json
REPO=os.environ.get('POKEEMERALD', os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))
WORK=os.environ.get('RT_WORK', os.path.join(REPO, 'build', 'rt_work'))
"""Generates src/data/rt/vfx_defs.h: the look of every move (style, colours, impact, screen effects)."""
PE=REPO
moves=json.load(open(os.path.join(WORK,'moves.json')))
defs=open(PE+'/src/data/rt/move_defs.h').read()
PAT={}; SHP={}
for m in re.finditer(r'\[MOVE_(\w+)\] = \{RTP_(\w+), FX_(\w+), FXP_(\w+)',defs):
    PAT[m.group(1)]=m.group(2); SHP[m.group(1)]=m.group(3)

# main / secondary colour per type
TP={'NORMAL':('E_WHITE','E_GOLD'),'FIGHTING':('E_FIGHT','E_HOT'),'FLYING':('E_SKY','E_WHITE'),'POISON':('E_POISON','E_PINK'),
    'GROUND':('E_MUD','E_DUST'),'ROCK':('E_ROCK','E_DUST'),'BUG':('E_BUG','E_HEAL'),'GHOST':('E_GHOST','E_DARK'),
    'STEEL':('E_STEEL','E_WHITE'),'MYSTERY':('E_GHOST','E_DARK'),'FIRE':('E_FIRE','E_HOT'),'WATER':('E_WATER','E_FOAM'),
    'GRASS':('E_GRASS','E_HEAL'),'ELECTRIC':('E_ELEC','E_BOLT'),'PSYCHIC':('E_PSY','E_PINK'),'ICE':('E_ICE','E_WHITE'),
    'DRAGON':('E_DRAGON','E_GHOST'),'DARK':('E_DARK','E_GHOST')}

def default(name, pat, shp, typ, power, eff):
    st='NONE'
    if pat=='BULLET':
        st={'FIRE':'FIREBALL','ELECTRIC':'ELECSHOT','GHOST':'DARKORB','POISON':'GOO','ICE':'SHARD','ROCK':'ROCK',
            'WATER':'WATERBALL','GROUND':'MUDBALL','DRAGON':'FIREBALL','FLYING':'BLADE','GRASS':'ORB','PSYCHIC':'ORB',
            'NORMAL':'ORB','STEEL':'ORB','FIGHTING':'ORB','DARK':'DARKORB','BUG':'NEEDLE'}.get(typ,'ORB')
        if shp in ('NEEDLE8',): st='NEEDLE'
        if shp=='CRESCENT32': st='BLADE'
        if shp=='STAR16': st='STAR'
        if shp=='TORNADO32': st='TWISTER'
        if shp=='BUBBLE16': st='BUBBLE'
        if shp=='LEAF16': st='LEAF'
        if shp in ('COIN8','HEART16','EYE16','SKULL16','EXCLAIM8','NOTE16','CROSS16','TARGET32','BONE16','TRI16','SEED8','GLOW16'): st='EMBLEM'
    elif pat=='STREAM':
        st={'FIRE':'FLAMES','WATER':'WATERJET','DRAGON':'DBREATH','POISON':'GAS','ICE':'SNOWS','GROUND':'SANDS','ROCK':'SANDS'}.get(typ,'POWDERS')
        if shp=='BUBBLE16': st='BUBBLES'
    elif pat=='BEAM':
        st={'ELECTRIC':'LIGHTNING','ICE':'ICEBEAM','PSYCHIC':'PSYBEAM'}.get(typ,'BEAM')
    elif pat=='TARGETED':
        st={'ELECTRIC':'THUNDER','ROCK':'ROCKFALL','ICE':'BLIZZARD','PSYCHIC':'PSYBLAST','FIRE':'PILLAR','WATER':'PILLAR','GRASS':'PILLAR','STEEL':'METEOR'}.get(typ,'NONE')
    elif pat=='MELEE':
        st={'SLASH32':'SLASH','CRESCENT32':'SLASH','FANG32':'BITE','FIST16':'PUNCH','HORN16':'HORN','DRILL16':'HORN','IMPACT32':'KICK','CROSS16':'SLASH','LEAF16':'SLASH','FLAME32':'KICK','STAR16':'PUNCH','FEATHER16':'SLASH'}.get(shp,'NONE')
    elif pat=='RUSH':
        st='DASH'
    elif pat=='BURST':
        st='NOVA'
    elif pat=='SELF':
        st='AURA'
    elif pat=='CONE':
        st='SPRAY'
        if shp in ('NOTE16','RING32','Z8'): st='SHOUT'
        if shp=='EYE16': st='GAZE'
        if shp in ('ARROWDN8','ARROWUP8','HEART16','DROP8','PSYRING16','FEATHER16'): st='NONE'
    elif pat=='ZONE':
        st='VORTEX' if name in ('FIRE_SPIN','WHIRLPOOL','SAND_TOMB') else ('CLOUD' if shp in ('SMOKE16','POWDER8') else 'NONE')
    elif pat in ('LEAP','BURROW'):
        st='LAND'
    elif pat=='FIELD':
        st='WEATHER'
    elif pat=='WAVE':
        st={'WATER':'SURF','FIRE':'HEAT'}.get(typ,'SOUND' if shp in ('NOTE16','PSYRING16','RING32') else 'NONE')
    return st

OV={  # name: dict(style=, pal=, pal2=, imp=, flags=)
 'HYDRO_PUMP':dict(style='HELIX',flags='BIG HEAVY SHAKE1'),
 'WATER_GUN':dict(style='WATERJET'),
 'FLAMETHROWER':dict(style='FLAMES'),
 'SACRED_FIRE':dict(style='FLAMES',pal='E_GOLD',flags='BIG HEAVY'),
 'DRAGON_BREATH':dict(style='DBREATH',flags='HEAVY'),
 'BUBBLE_BEAM':dict(style='BUBBLES'),
 'STRING_SHOT':dict(style='POWDERS',pal='E_WHITE',pal2='WHITE',imp='NONE'),
 'THUNDERBOLT':dict(style='LIGHTNING',flags='HEAVY'),
 'THUNDER':dict(style='THUNDER',flags='DIM HEAVY'),
 'THUNDER_SHOCK':dict(style='ELECSHOT'),
 'SHOCK_WAVE':dict(style='ELECSHOT'),
 'THUNDER_WAVE':dict(style='ELECSHOT',imp='ELEC'),
 'ZAP_CANNON':dict(style='ELECSHOT',flags='BIG HEAVY SHAKE1'),
 'HYPER_BEAM':dict(style='HYPER',pal='E_HOT',pal2='E_GOLD',flags='BIG HEAVY DIM SHAKE2',imp='NORMAL'),
 'SOLAR_BEAM':dict(style='SOLAR',pal='E_SOLAR',pal2='E_GOLD',flags='BIG HEAVY DIM SHAKE1',imp='GRASS'),
 'ICE_BEAM':dict(style='ICEBEAM'),
 'AURORA_BEAM':dict(style='AURORA',pal='E_AURORA',pal2='E_PINK',imp='ICE'),
 'PSYBEAM':dict(style='PSYBEAM'),
 'SIGNAL_BEAM':dict(style='SIGNAL',pal='E_BUG',pal2='E_PINK'),
 'LUSTER_PURGE':dict(style='BEAM',pal='E_WHITE',pal2='E_PSY',flags='BIG HEAVY'),
 'AEROBLAST':dict(style='BEAM',pal='E_SKY',pal2='E_WHITE',flags='BIG HEAVY SHAKE1',imp='FLYING'),
 'HYDRO_CANNON':dict(style='BEAM',pal='E_WATER',pal2='E_FOAM',flags='BIG HEAVY DIM SHAKE2',imp='WATER'),
 'OVERHEAT':dict(style='BEAM',pal='E_FIRE',pal2='E_HOT',flags='BIG HEAVY SHAKE1 WAVE',imp='FIRE'),
 'FIRE_BLAST':dict(style='FIREBALL',flags='BIG HEAVY SHAKE1'),
 'EMBER':dict(style='FIREBALL'),
 'DRAGON_RAGE':dict(style='FIREBALL',pal='E_DRAGON',imp='DRAGON'),
 'SHADOW_BALL':dict(style='DARKORB',flags='HEAVY'),
 'NIGHT_SHADE':dict(style='DARKORB',pal='E_GHOST'),'FISSURE':dict(style='QUAKE',flags='SHAKE1 HEAVY',imp='GROUND'),
 'CONFUSE_RAY':dict(style='ORB',pal='E_GHOST',pal2='E_GOLD',imp='GHOST'),
 'WILL_O_WISP':dict(style='FIREBALL',pal='E_GHOST',imp='GHOST'),
 'SLUDGE':dict(style='GOO'), 'SLUDGE_BOMB':dict(style='GOO',flags='BIG HEAVY'),
 'ACID':dict(style='GOO'),'TOXIC':dict(style='GOO'),
 'MUD_SLAP':dict(style='MUDBALL'),'MUD_SHOT':dict(style='MUDBALL'),'OCTAZOOKA':dict(style='GOO',pal='E_DARK',imp='DARK'),
 'MIST_BALL':dict(style='ORB',pal='E_PINK',pal2='E_WHITE',imp='PSY'),
 'PSYCHO_BOOST':dict(style='ORB',pal='E_PSY',pal2='E_PINK',flags='BIG HEAVY DIM WAVE',imp='PSY'),
 'WEATHER_BALL':dict(style='ORB',pal='E_SKY',pal2='E_WHITE'),
 'GIGA_DRAIN':dict(style='ORB',pal='E_HEAL',pal2='E_GRASS',imp='GRASS'),
 'MEGA_DRAIN':dict(style='ORB',pal='E_HEAL',pal2='E_GRASS',imp='GRASS'),
 'ABSORB':dict(style='ORB',pal='E_HEAL',pal2='E_GRASS',imp='GRASS'),
 'DREAM_EATER':dict(style='ORB',pal='E_PSY',pal2='E_GHOST'),
 'WATER_PULSE':dict(style='WATERBALL'),
 'ICICLE_SPEAR':dict(style='SHARD'),
 'ROCK_THROW':dict(style='ROCK'),'ROCK_BLAST':dict(style='ROCK'),'ANCIENT_POWER':dict(style='ROCK',flags='HEAVY'),
 'RAZOR_LEAF':dict(style='LEAF'),'MAGICAL_LEAF':dict(style='LEAF',pal2='E_PINK'),
 'SWIFT':dict(style='STAR'),
 'AIR_CUTTER':dict(style='BLADE',pal='E_SKY',pal2='E_WHITE'),'SONIC_BOOM':dict(style='BLADE',pal='E_WHITE',pal2='E_SKY'),
 'RAZOR_WIND':dict(style='BLADE',pal='E_WHITE',pal2='E_SKY'),
 'GUST':dict(style='TWISTER',pal='E_SKY'),'TWISTER':dict(style='TWISTER',pal='E_DRAGON'),
 'HIDDEN_POWER':dict(style='ORB',pal='E_WHITE',pal2='E_PINK'),'SECRET_POWER':dict(style='ORB'),
 'EGG_BOMB':dict(style='ORB',pal='E_WHITE',pal2='E_GOLD',imp='NORMAL',flags='HEAVY'),'BARRAGE':dict(style='ORB'),
 'SPIT_UP':dict(style='ORB',pal='E_GOLD',pal2='E_HOT',flags='BIG'),
 'SHADOW_PUNCH':dict(style='DARKORB'),
 'EXTRASENSORY':dict(style='ORB',pal='E_PSY',pal2='E_PINK'),
 'CONFUSION':dict(style='ORB',pal='E_PSY',pal2='E_PINK'),
 'SKILL_SWAP':dict(style='ORB',pal='E_PSY',pal2='E_PINK'),
 'BLIZZARD':dict(style='BLIZZARD',flags='HEAVY'),
 'ROCK_SLIDE':dict(style='ROCKFALL'),'ROCK_TOMB':dict(style='ROCKFALL',flags='BIG HEAVY'),
 'ERUPTION':dict(style='PILLAR',pal='E_FIRE',flags='BIG HEAVY SHAKE2 WAVE',imp='FIRE'),
 'WATER_SPOUT':dict(style='PILLAR',pal='E_WATER',flags='BIG HEAVY SHAKE1',imp='WATER'),
 'FRENZY_PLANT':dict(style='PILLAR',pal='E_GRASS',flags='BIG HEAVY SHAKE2',imp='GRASS'),
 'FUTURE_SIGHT':dict(style='METEOR',pal='E_PSY',pal2='E_PINK',imp='PSY'),
 'DOOM_DESIRE':dict(style='METEOR',pal='E_GOLD',pal2='E_WHITE',imp='STEEL',flags='HEAVY'),
 'PSYCHIC':dict(style='PSYBLAST',flags='WAVE HEAVY'),
 'EARTHQUAKE':dict(style='QUAKE',flags='SHAKE2 HEAVY',imp='GROUND'),'MAGNITUDE':dict(style='QUAKE',flags='SHAKE2',imp='GROUND'),
 'EXPLOSION':dict(style='EXPLODE',pal='E_HOT',flags='BIG HEAVY'),'SELF_DESTRUCT':dict(style='EXPLODE',pal='E_HOT',flags='HEAVY'),
 'BLAST_BURN':dict(style='EXPLODE',pal='E_FIRE',flags='BIG HEAVY WAVE'),
 'PETAL_DANCE':dict(style='PETALS',pal='E_PINK',imp='GRASS'),
 'SHEER_COLD':dict(style='NOVA',pal='E_ICE',pal2='E_WHITE',flags='DIM'),
 'FLASH':dict(style='NOVA',pal='E_WHITE',pal2='E_GOLD',flags='FLASH'),
 'MEMENTO':dict(style='NOVA',pal='E_DARK',pal2='E_GHOST',flags='DIM'),
 'TEETER_DANCE':dict(style='NOVA',pal='E_PINK',pal2='E_GOLD'),
 'SURF':dict(style='SURF',flags='SHAKE1'),'MUDDY_WATER':dict(style='SURF',pal='E_MUD'),
 'HEAT_WAVE':dict(style='HEAT',flags='WAVE'),
 'PROTECT':dict(style='SHIELD',pal='E_HEAL',pal2='E_WHITE'),'DETECT':dict(style='SHIELD',pal='E_FIGHT',pal2='E_GOLD'),
 'ENDURE':dict(style='SHIELD',pal='E_HOT',pal2='E_GOLD'),
 'REFLECT':dict(style='WALL',pal='E_SKY'),'LIGHT_SCREEN':dict(style='WALL',pal='E_GOLD'),'SAFEGUARD':dict(style='WALL',pal='E_PSY'),
 'BARRIER':dict(style='WALL',pal='E_PSY'),'MAGIC_COAT':dict(style='SHIELD',pal='E_PSY',pal2='E_GOLD'),
 'MIRROR_COAT':dict(style='SHIELD',pal='E_PINK',pal2='E_WHITE'),'COUNTER':dict(style='SHIELD',pal='E_FIGHT',pal2='E_HOT'),
 'RECOVER':dict(style='HEAL'),'SOFT_BOILED':dict(style='HEAL'),'MILK_DRINK':dict(style='HEAL'),'SLACK_OFF':dict(style='HEAL'),
 'SYNTHESIS':dict(style='HEAL'),'MORNING_SUN':dict(style='HEAL'),'MOONLIGHT':dict(style='HEAL'),'REST':dict(style='HEAL'),
 'SWALLOW':dict(style='HEAL'),'WISH':dict(style='HEAL'),'REFRESH':dict(style='HEAL'),'INGRAIN':dict(style='HEAL'),
 'DRAGON_DANCE':dict(style='AURA',pal='E_DRAGON',pal2='E_FIRE'),'BULK_UP':dict(style='AURA',pal='E_FIGHT',pal2='E_HOT'),
 'CALM_MIND':dict(style='AURA',pal='E_PSY',pal2='E_PINK'),'SWORDS_DANCE':dict(style='AURA',pal='E_STEEL',pal2='E_WHITE'),
 'FOCUS_PUNCH':dict(style='PUNCH',flags='HEAVY SHAKE1'),
 'FIRE_PUNCH':dict(style='PUNCH',pal='E_FIRE',pal2='E_HOT',imp='FIRE'),'ICE_PUNCH':dict(style='PUNCH',pal='E_ICE',pal2='E_WHITE',imp='ICE'),
 'THUNDER_PUNCH':dict(style='PUNCH',pal='E_ELEC',pal2='E_BOLT',imp='ELEC'),
 'METEOR_MASH':dict(style='PUNCH',pal='E_STEEL',pal2='E_GOLD',flags='HEAVY'),
 'DYNAMIC_PUNCH':dict(style='PUNCH',flags='HEAVY SHAKE1'),'MEGA_PUNCH':dict(style='PUNCH',flags='HEAVY'),
 'SKY_UPPERCUT':dict(style='PUNCH',pal='E_SKY',pal2='E_WHITE'),
 'METAL_CLAW':dict(style='CLAW',pal='E_STEEL'),'DRAGON_CLAW':dict(style='CLAW',pal='E_DRAGON',pal2='E_GHOST'),
 'CRUSH_CLAW':dict(style='CLAW'),'FURY_SWIPES':dict(style='CLAW'),'SCRATCH':dict(style='CLAW'),
 'SLASH':dict(style='SLASH',pal='E_WHITE',pal2='E_SKY'),'LEAF_BLADE':dict(style='SLASH',pal='E_GRASS',pal2='E_HEAL',flags='HEAVY'),
 'CROSS_CHOP':dict(style='SLASH',pal='E_FIGHT',pal2='E_HOT',flags='HEAVY'),
 'CRUNCH':dict(style='BITE',pal='E_WHITE',flags='HEAVY'),'POISON_FANG':dict(style='BITE',pal='E_POISON'),
 'BLAZE_KICK':dict(style='KICK',pal='E_FIRE',pal2='E_HOT',imp='FIRE'),
 'FLAME_WHEEL':dict(style='DASH',pal='E_FIRE',imp='FIRE'),'VOLT_TACKLE':dict(style='DASH',pal='E_ELEC',imp='ELEC',flags='HEAVY SHAKE1'),
 'SPARK':dict(style='DASH',pal='E_ELEC',imp='ELEC'),'OUTRAGE':dict(style='DASH',pal='E_DRAGON',imp='DRAGON'),
 'WATERFALL':dict(style='DASH',pal='E_WATER',imp='WATER'),'EXTREME_SPEED':dict(style='DASH',flags='HEAVY'),
 'SKY_ATTACK':dict(style='DASH',pal='E_SKY',imp='FLYING',flags='HEAVY SHAKE1 DIM'),
 'MEGAHORN':dict(style='DASH',pal='E_BUG',imp='BUG',flags='HEAVY'),
 'DOUBLE_EDGE':dict(style='DASH',flags='HEAVY SHAKE1'),'TAKE_DOWN':dict(style='DASH',flags='HEAVY'),
 'HYPER_VOICE':dict(style='SOUND',pal='E_WHITE',flags='SHAKE1'),'SUPERSONIC':dict(style='SOUND',pal='E_SKY'),
 'SING':dict(style='SOUND',pal='E_PINK'),'GRASS_WHISTLE':dict(style='SOUND',pal='E_GRASS'),'HYPNOSIS':dict(style='SOUND',pal='E_PSY'),
 'PSYWAVE':dict(style='SOUND',pal='E_PSY',flags='WAVE'),
 'POWDER_SNOW':dict(style='SPRAY',pal='E_ICE',pal2='E_WHITE'),'ICY_WIND':dict(style='SPRAY',pal='E_ICE',pal2='E_SKY'),
 'SAND_ATTACK':dict(style='SPRAY',pal='E_SAND',pal2='E_SAND',imp='NONE'),'WHIRLWIND':dict(style='SPRAY',pal='E_SKY',pal2='E_WHITE',imp='NONE'),
 'GROWL':dict(style='SHOUT',pal='E_WHITE'),'ROAR':dict(style='SHOUT',pal='E_HOT',flags='SHAKE1'),'SCREECH':dict(style='SHOUT',pal='E_PINK'),
 'UPROAR':dict(style='SHOUT',pal='E_HOT',flags='SHAKE1'),'METAL_SOUND':dict(style='SHOUT',pal='E_STEEL'),'SNORE':dict(style='SHOUT',pal='E_SKY'),
 'LEER':dict(style='GAZE',pal='E_FIGHT'),'GLARE':dict(style='GAZE',pal='E_GOLD'),'SCARY_FACE':dict(style='GAZE',pal='E_DARK'),
 'FIRE_SPIN':dict(style='VORTEX',pal='E_FIRE'),'WHIRLPOOL':dict(style='VORTEX',pal='E_WATER'),'SAND_TOMB':dict(style='VORTEX',pal='E_SAND'),
 'POISON_POWDER':dict(style='CLOUD',pal='E_POISON',pal2='POISON'),'STUN_SPORE':dict(style='CLOUD',pal='E_ELEC',pal2='ELECTRIC'),
 'SLEEP_POWDER':dict(style='CLOUD',pal='E_SKY',pal2='SLEEP'),'SPORE':dict(style='CLOUD',pal='E_GRASS',pal2='GRASS'),
 'COTTON_SPORE':dict(style='CLOUD',pal='E_WHITE',pal2='WHITE'),'SMOKESCREEN':dict(style='CLOUD',pal='E_SMOKE',pal2='SMOKE'),
 'SMOG':dict(style='CLOUD',pal='E_POISON',pal2='POISON'),'POISON_GAS':dict(style='CLOUD',pal='E_POISON',pal2='POISON'),
 'SWEET_SCENT':dict(style='CLOUD',pal='E_PINK',pal2='HEART'),
 'FLY':dict(style='LAND',pal='E_SKY',pal2='E_WHITE',imp='FLYING'),'BOUNCE':dict(style='LAND',pal='E_SKY',pal2='E_DUST'),
 'HI_JUMP_KICK':dict(style='LAND',pal='E_FIGHT',pal2='E_DUST',flags='HEAVY'),'DIG':dict(style='LAND',pal='E_MUD',pal2='E_DUST',imp='GROUND'),
 'DIVE':dict(style='LAND',pal='E_WATER',pal2='E_FOAM',imp='WATER'),
 'HEAL_BELL':dict(style='WEATHER',pal='E_GOLD',pal2='E_HEAL'),'AROMATHERAPY':dict(style='WEATHER',pal='E_PINK',pal2='E_HEAL'),
 'PERISH_SONG':dict(style='WEATHER',pal='E_DARK',pal2='E_GHOST'),'MUD_SPORT':dict(style='WEATHER',pal='E_MUD',pal2='E_DUST'),
 'WATER_SPORT':dict(style='WEATHER',pal='E_WATER',pal2='E_FOAM'),'SANDSTORM':dict(style='WEATHER',pal='E_SAND',pal2='E_DUST'),
 'HAIL':dict(style='WEATHER',pal='E_ICE',pal2='E_WHITE'),'RAIN_DANCE':dict(style='WEATHER',pal='E_WATER',pal2='E_SKY'),
 'SUNNY_DAY':dict(style='WEATHER',pal='E_SOLAR',pal2='E_GOLD'),'HAZE':dict(style='WEATHER',pal='E_FOAM',pal2='E_WHITE'),
 'ATTRACT':dict(style='EMBLEM',pal='E_PINK',pal2='E_WHITE'),'CHARM':dict(style='EMBLEM',pal='E_PINK',pal2='E_WHITE'),
 'SWEET_KISS':dict(style='EMBLEM',pal='E_PINK',pal2='E_WHITE'),'LOVELY_KISS':dict(style='EMBLEM',pal='E_PINK',pal2='E_PSY'),
 'FLATTER':dict(style='EMBLEM',pal='E_PINK',pal2='E_GOLD'),'TRI_ATTACK':dict(style='EMBLEM',pal='E_GOLD',pal2='E_ICE'),
 'PAY_DAY':dict(style='EMBLEM',pal='E_GOLD',pal2='E_WHITE'),
}
FLAGS={'SHAKE1':1,'SHAKE2':2,'DIM':4,'WAVE':8,'FLASH':16,'BIG':32,'HEAVY':64}
VI={'FIRE','ELEC','WATER','GRASS','ICE','ROCK','PSY','GHOST','DARK','FIGHT','NORMAL','POISON','DRAGON','STEEL','BUG','FLYING','GROUND','NONE'}
out=[]
for m in moves:
    name=m['name'][5:]; typ=m['type'][5:]; power=int(m['power']) if m['power'].isdigit() else 0
    pat=PAT.get(name,'NONE'); shp=SHP.get(name,'NONE')
    st=default(name,pat,shp,typ,power,m['effect'])
    pal,pal2=TP.get(typ,('E_WHITE','E_GOLD'))
    imp='DEFAULT'; fl=set()
    if power>=100: fl|={'BIG','HEAVY'}
    elif power>=80: fl|={'HEAVY'}
    o=OV.get(name,{})
    st=o.get('style',st); pal=o.get('pal',pal); pal2=o.get('pal2',pal2); imp=o.get('imp',imp)
    if 'flags' in o: fl|=set(o['flags'].split())
    if imp!='DEFAULT': assert imp in VI, imp
    out.append((m['id'],name,st,pal,pal2,imp,fl))
with open(PE+'/src/data/rt/vfx_defs.h','w') as f:
    f.write('// generated by vfxgen.py - the look of every move\nconst struct RtVfxDef gRtVfxDefs[MOVES_COUNT] = {\n')
    for (i,name,st,pal,pal2,imp,fl) in out:
        flags='|'.join('VF_'+x for x in sorted(fl)) or '0'
        f.write('    [MOVE_%s] = {VS_%s, FXP_%s, FXP_%s, VI_%s, %s},\n'%(name,st,pal,pal2,imp,flags))
    f.write('};\n')
from collections import Counter
print(Counter(o[2] for o in out).most_common())
