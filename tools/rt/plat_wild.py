# Puts every Gen 1-3 POKeMON family in the wild of the Platinum demake's Sinnoh areas (by type / habitat),
# next to the Sinnoh species already there. With balanced encounters every species of a table is equally
# likely, so each species needs one slot only.
# usage: python3 tools/rt/plat_wild.py [--apply]   (types / evolutions read from the Emerald decomp: POKEEMERALD)
import json, os, re, sys, random, collections
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(os.environ.get('POKEEMERALD', '/home/claude/pokeemerald'), 'tools', 'rt'))
os.environ.setdefault('POKEEMERALD', '/home/claude/pokeemerald')
import pokedata as P
random.seed(493)

nat = P.national_order(); info = P.species_info(); evo = P.evolutions()
plat_ids = set(re.findall(r'#define (SPECIES_\w+)\s', open(REPO + '/include/constants/species.h').read()))
WJ = REPO + '/src/data/wild_encounters.json'
data = json.load(open(WJ))
encs = [e for g in data['wild_encounter_groups'] if g.get('for_maps') for e in g['encounters']]

LEGENDS = {'ARTICUNO', 'ZAPDOS', 'MOLTRES', 'MEWTWO', 'MEW', 'RAIKOU', 'ENTEI', 'SUICUNE', 'LUGIA', 'HO_OH',
           'CELEBI', 'REGIROCK', 'REGICE', 'REGISTEEL', 'LATIAS', 'LATIOS', 'KYOGRE', 'GROUDON', 'RAYQUAZA',
           'JIRACHI', 'DEOXYS'}
BABIES = {'PICHU': 'PIKACHU', 'CLEFFA': 'CLEFAIRY', 'IGGLYBUFF': 'JIGGLYPUFF', 'SMOOCHUM': 'JYNX',
          'ELEKID': 'ELECTABUZZ', 'MAGBY': 'MAGMAR', 'AZURILL': 'MARILL', 'WYNAUT': 'WOBBUFFET'}
parent = {}
for s, lst in evo.items():
    for (m, p, t) in lst:
        parent.setdefault(t, s)
def root(s):
    while s in parent:
        s = parent[s]
    return s

# the Sinnoh areas of the demake and what lives there (types), with their level band
AREAS = {
    'Route201':        {'NORMAL', 'FLYING', 'BUG', 'GRASS'},
    'LakeVerity':      {'WATER', 'NORMAL', 'FLYING', 'ICE'},
    'Route202':        {'NORMAL', 'ELECTRIC', 'GRASS', 'BUG'},
    'Route203':        {'NORMAL', 'PSYCHIC', 'FIGHTING', 'FLYING'},
    'Route204':        {'GRASS', 'BUG', 'POISON', 'NORMAL'},
    'Route204North':   {'GRASS', 'BUG', 'POISON', 'FLYING'},
    'RavagedPath':     {'WATER', 'GROUND', 'ROCK'},
    'OreburghGate':    {'ROCK', 'GROUND', 'WATER'},
    'OreburghMine_B1F': {'ROCK', 'GROUND', 'STEEL'},
    'OreburghMine_B2F': {'ROCK', 'GROUND', 'STEEL', 'FIGHTING'},
    'Route207':        {'ROCK', 'GROUND', 'FIRE', 'FIGHTING'},
    'ValleyWindworks': {'ELECTRIC', 'WATER', 'FLYING'},
    'Route205South1':  {'WATER', 'GRASS', 'NORMAL'},
    'Route205South2':  {'WATER', 'GRASS', 'NORMAL', 'ELECTRIC'},
    'Route205North':   {'BUG', 'GRASS', 'POISON'},
    'EternaForest1':   {'BUG', 'GRASS', 'GHOST', 'POISON', 'DARK'},
    'EternaForest2':   {'BUG', 'GRASS', 'GHOST', 'POISON', 'DARK'},
    'MtCoronet_N1F1':  {'ROCK', 'ICE', 'PSYCHIC', 'STEEL', 'DRAGON', 'GHOST', 'FIGHTING'},
    'Route211West':    {'FIGHTING', 'PSYCHIC', 'FIRE', 'DRAGON', 'GROUND', 'DARK'},
}
TIMES = ['Morning', 'Day', 'Night']
NIGHT = {'GHOST', 'DARK'}

tables = {}  # (area, time) -> encounter
for e in encs:
    m = re.match(r'g(\w+)_(Morning|Day|Night)$', e['base_label'])
    if m and m.group(1) in AREAS and 'land_mons' in e:
        tables[(m.group(1), m.group(2))] = e

def species_in(e):
    out = []
    for mm in e['land_mons']['mons']:
        if mm['species'] not in out:
            out.append(mm['species'])
    return out
present = set(s for e in tables.values() for s in species_in(e))
present_roots = set(root(s) for s in present if s in info)
def avg_level(e):
    ms = e['land_mons']['mons']
    return sum((m['min_level'] + m['max_level']) / 2 for m in ms) / len(ms)

fams = []
for s in nat:
    name = s[8:]
    if name in LEGENDS:
        continue
    r = root(s)
    r = 'SPECIES_' + BABIES.get(r[8:], r[8:])
    if r in fams or r in present_roots or r in present or r not in plat_ids:
        continue
    fams.append(r)

# per table: the species it will hold (old ones first)
plan = {k: species_in(e) for k, e in tables.items()}
def room(k):
    return 12 - len(plan[k])
STRONG = lambda s: info[s]['bst'] >= 450

log = []
order = sorted(fams, key=lambda s: (STRONG(s), random.random()))
area_load = collections.Counter()
for s in order:
    t = set(info[s]['types'])
    best = None
    for (area, time), e in tables.items():
        if room((area, time)) <= 0:
            continue
        lvl = avg_level(e)
        if STRONG(s) and lvl < 10:
            continue
        sc = 3 * len(t & AREAS[area])
        if sc == 0:
            continue
        if t & NIGHT and time == 'Night':
            sc += 2
        sc -= area_load[area] * 0.35
        sc += room((area, time)) * 0.1 + random.random() * 0.5
        if best is None or sc > best[0]:
            best = (sc, area, time)
    if best is None:
        # no matching type: the least crowded table that is high enough
        cands = [(room(k), k) for k, e in tables.items() if room(k) > 0 and (not STRONG(s) or avg_level(e) >= 10)]
        if not cands:
            log.append('!! no room for %s' % s); continue
        k = max(cands)[1]
        best = (0, k[0], k[1])
    _, area, time = best
    plan[(area, time)].append(s)
    area_load[area] += 1
    log.append('%-14s -> %-18s %s' % (s[8:], area, time))

# spare room: species of the area also at the other times of day (not the night-only ones by day)
for area in AREAS:
    for time in TIMES:
        k = (area, time)
        if k not in tables:
            continue
        others = [s for t2 in TIMES if (area, t2) in tables for s in plan[(area, t2)] if s not in plan[k]]
        random.shuffle(others)
        for s in others:
            if room(k) <= 0:
                break
            if set(info.get(s, {'types': []})['types']) & NIGHT and time != 'Night':
                continue
            plan[k].append(s)

# write the tables: one slot per species, the slots' levels from the table's own range
for k, e in tables.items():
    old = e['land_mons']['mons']
    lo = min(m['min_level'] for m in old); hi = max(m['max_level'] for m in old)
    first = {}
    for m in old:
        first.setdefault(m['species'], m)
    mons = []
    for s in plan[k]:
        if s in first:
            mons.append(dict(first[s]))
        else:
            mons.append({'min_level': lo, 'max_level': hi, 'species': s})
    i = 0
    while len(mons) < 12:  # tables keep their 12 slots
        mons.append(dict(mons[i % len(plan[k])])); i += 1
    e['land_mons']['mons'] = mons[:12]

print('families added:', len(fams))
print('\n'.join(log))
print({'%s_%s' % k: len(v) for k, v in plan.items()})
if '--apply' in sys.argv:
    json.dump(data, open(WJ, 'w'), indent=2)
    open(WJ, 'a').write('\n')
    print('written', WJ)
