# Plans the "every Pokemon of Gen 1-3 is obtainable" encounter changes.
# Usage: python3 dexplan.py [--apply]  (writes src/data/wild_encounters.json when --apply)
import sys, json, collections, random, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pokedata import *
random.seed(386)

nat = national_order(); info = species_info(); evo = evolutions()
DEXNUM = {s: i + 1 for i, s in enumerate(nat)}
WJ = REPO + '/src/data/wild_encounters.json'
data = json.load(open(WJ))
encs = data['wild_encounter_groups'][0]['encounters']
FIELDS = ['land_mons', 'water_mons', 'rock_smash_mons', 'fishing_mons']

def usable_table(e):
    l = e['base_label']
    if 'Unused' in l: return False
    if 'AlteringCave' in l and l != 'gAlteringCave1': return False
    return True

# --- evolution graph
parent = {}
for s, lst in evo.items():
    for (m, p, t) in lst:
        parent.setdefault(t, s)
def root(s):
    while s in parent: s = parent[s]
    return s
def descendants(s):
    out = [s]
    for (m, p, t) in evo.get(s, []): out += descendants(t)
    return out
BABIES = {'SPECIES_PICHU', 'SPECIES_CLEFFA', 'SPECIES_IGGLYBUFF', 'SPECIES_TOGEPI', 'SPECIES_TYROGUE',
          'SPECIES_SMOOCHUM', 'SPECIES_ELEKID', 'SPECIES_MAGBY', 'SPECIES_AZURILL', 'SPECIES_WYNAUT'}
# babies whose breeding needs an incense we can't count on: obtain directly
INCENSE_BABIES = {'SPECIES_AZURILL'}

# --- what the player can already get without trading
GIFTS = ['SPECIES_FEEBAS', 'SPECIES_CASTFORM', 'SPECIES_BELDUM', 'SPECIES_WYNAUT', 'SPECIES_VOLTORB', 'SPECIES_ELECTRODE',
         'SPECIES_KECLEON', 'SPECIES_SUDOWOODO', 'SPECIES_REGIROCK', 'SPECIES_REGICE', 'SPECIES_REGISTEEL',
         'SPECIES_GROUDON', 'SPECIES_KYOGRE', 'SPECIES_RAYQUAZA']
# handled by the special-encounter system in code (roaming legends, post-game rarities, ticket events)
SPECIAL = ['SPECIES_ARTICUNO', 'SPECIES_ZAPDOS', 'SPECIES_MOLTRES', 'SPECIES_RAIKOU', 'SPECIES_ENTEI',
           'SPECIES_SUICUNE', 'SPECIES_LUGIA', 'SPECIES_HO_OH', 'SPECIES_LATIAS', 'SPECIES_LATIOS',
           'SPECIES_MEWTWO', 'SPECIES_MEW', 'SPECIES_CELEBI', 'SPECIES_JIRACHI', 'SPECIES_DEOXYS']

def present_species():
    c = collections.Counter()
    for e in encs:
        if not usable_table(e): continue
        for f in FIELDS:
            if f in e:
                for m in e[f]['mons']: c[m['species']] += 1
    return c

def obtainable(wild):
    have = set(wild) | set(GIFTS) | set(SPECIAL)
    changed = True
    while changed:
        changed = False
        for s in list(have):
            for d in descendants(s):
                if d not in have: have.add(d); changed = True
            # breeding gives the family root (baby) unless it needs an incense
            r = root(s)
            if r not in have and r not in INCENSE_BABIES:
                have.add(r); changed = True
                for d in descendants(r):
                    if d not in have: have.add(d)
    return have

# --- table classification
CAVE_WORDS = ['Cave', 'Tunnel', 'Falls', 'VictoryRoad', 'MtPyre_', 'FieryPath', 'SkyPillar', 'Seafloor',
              'Hideout', 'Underpass', 'MirageTower', 'AbandonedShip', 'NewMauville', 'Underwater']
def is_cave(e):
    return any(w in e['base_label'] for w in CAVE_WORDS) and 'Exterior' not in e['base_label'] and 'Summit' not in e['base_label']

def avg_level(e, f):
    ms = e[f]['mons']
    return sum((m['min_level'] + m['max_level']) / 2 for m in ms) / len(ms)

# typical habitat of a species by type
def habitats(s):
    t = set(info[s]['types'])
    h = set()
    if 'WATER' in t: h |= {'water_mons', 'fishing_mons'}
    if t & {'ROCK', 'GROUND'}: h |= {'rock_smash_mons'}
    if not ('WATER' in t and len(t) == 1) or s in LAND_WATER: h |= {'land_mons'}
    return h
LAND_WATER = {'SPECIES_SQUIRTLE', 'SPECIES_TOTODILE', 'SPECIES_PSYDUCK', 'SPECIES_POLIWAG', 'SPECIES_SLOWPOKE',
              'SPECIES_MUDKIP', 'SPECIES_MARILL', 'SPECIES_AZURILL'}
CAVE_TYPES = {'ROCK', 'GROUND', 'GHOST', 'DARK', 'ICE', 'STEEL', 'FIGHTING', 'DRAGON'}
OUTDOOR_TYPES = {'GRASS', 'BUG', 'NORMAL', 'FLYING', 'ELECTRIC', 'FIRE', 'FAIRY'}

# hand placements where type matching would be silly or impossible
OVERRIDES = {
    'SPECIES_PORYGON': ['gNewMauville_Inside', 'gNewMauville_Entrance'],
    'SPECIES_UNOWN': ['gDesertUnderpass', 'gSkyPillar_1F', 'gMirageTower_1F'],
    'SPECIES_SMEARGLE': ['gRoute114', 'gRoute121', 'gRoute123'],
    'SPECIES_DITTO': ['gDesertUnderpass', 'gRoute113', 'gArtisanCave_1F'],
    'SPECIES_CHANSEY': ['gSafariZone_North', 'gRoute121', 'gRoute123'],
    'SPECIES_KANGASKHAN': ['gSafariZone_Northwest', 'gSafariZone_South', 'gRoute120'],
    'SPECIES_TAUROS': ['gSafariZone_Southwest', 'gRoute121', 'gRoute111'],
    'SPECIES_LAPRAS': ['gRoute131', 'gRoute130', 'gRoute134'],
    'SPECIES_SURSKIT': ['gRoute102', 'gRoute114', 'gRoute117', 'gRoute120'],
    'SPECIES_ZANGOOSE': ['gRoute114', 'gRoute117', 'gRoute118'],
    'SPECIES_ROSELIA': ['gRoute117', 'gRoute120', 'gRoute123'],
    'SPECIES_LUNATONE': ['gMeteorFalls_1F_1R', 'gMeteorFalls_1F_2R', 'gMeteorFalls_B1F_1R'],
    'SPECIES_SNORLAX': ['gRoute120', 'gRoute123'],
    'SPECIES_DRATINI': ['gMeteorFalls_B1F_2R', 'gSafariZone_North', 'gRoute119'],
    'SPECIES_LARVITAR': ['gVictoryRoad_B1F', 'gMeteorFalls_B1F_1R', 'gJaggedPass'],
    'SPECIES_OMANYTE': ['gRoute126', 'gRoute129', 'gRoute131'],
    'SPECIES_KABUTO': ['gRoute126', 'gRoute129', 'gRoute131'],
    'SPECIES_AERODACTYL': ['gMeteorFalls_StevensCave', 'gVictoryRoad_B2F', 'gSkyPillar_3F'],
    'SPECIES_LILEEP': ['gRoute111', 'gMirageTower_2F'],
    'SPECIES_ANORITH': ['gRoute111', 'gMirageTower_3F'],
    'SPECIES_BULBASAUR': ['gPetalburgWoods', 'gRoute119'],
    'SPECIES_CHARMANDER': ['gFieryPath', 'gJaggedPass'],
    'SPECIES_SQUIRTLE': ['gRoute102', 'gRoute110'],
    'SPECIES_CHIKORITA': ['gRoute117', 'gRoute104'],
    'SPECIES_CYNDAQUIL': ['gRoute112', 'gMagmaHideout_1F'],
    'SPECIES_TOTODILE': ['gRoute103', 'gRoute118'],
    'SPECIES_TREECKO': ['gPetalburgWoods', 'gRoute119'],
    'SPECIES_TORCHIC': ['gRoute113', 'gFieryPath'],
    'SPECIES_MUDKIP': ['gRoute102', 'gRoute103'],
    'SPECIES_EEVEE': ['gRoute116', 'gRoute118', 'gSafariZone_Northeast'],
    'SPECIES_TOGEPI': ['gRoute121', 'gRoute104'],
    'SPECIES_AZURILL': ['gRoute104', 'gRoute110'],
    'SPECIES_SCYTHER': ['gSafariZone_Northeast', 'gRoute119', 'gPetalburgWoods'],
    'SPECIES_PINSIR': ['gSafariZone_Northwest', 'gRoute119'],
    'SPECIES_HERACROSS': ['gSafariZone_Northeast', 'gPetalburgWoods', 'gRoute119'],
    'SPECIES_MR_MIME': ['gRoute121', 'gRoute123'],
    'SPECIES_JYNX': ['gShoalCave_LowTideIceRoom', 'gShoalCave_LowTideLowerRoom'],
    'SPECIES_SNEASEL': ['gShoalCave_LowTideIceRoom', 'gShoalCave_LowTideInnerRoom'],
    'SPECIES_SWINUB': ['gShoalCave_LowTideIceRoom', 'gShoalCave_LowTideStairsRoom'],
    'SPECIES_DELIBIRD': ['gShoalCave_LowTideIceRoom', 'gShoalCave_LowTideLowerRoom'],
    'SPECIES_MAGMAR': ['gFieryPath', 'gMagmaHideout_2F_1R'],
    'SPECIES_ELECTABUZZ': ['gNewMauville_Inside', 'gRoute110'],
    'SPECIES_ONIX': ['gGraniteCave_B1F', 'gVictoryRoad_1F', 'gRusturfTunnel'],
    'SPECIES_FARFETCHD': ['gRoute118', 'gRoute121'],
}

# tables whose identity should not be diluted (Mirage Island's Wynaut, Mt. Pyre summit ghosts)
BLACKLIST = {('gRoute130', 'land_mons'), ('gMtPyre_Summit', 'land_mons'), ('gAlteringCave1', 'land_mons')}
STRONG_MIN_LEVEL = lambda s: 0 if info[s]['bst'] < 420 else (20 if info[s]['bst'] < 470 else 27)

def plan():
    wild = present_species()
    have = obtainable(wild)
    missing_species = [s for s in nat if s not in have]
    # one entry per family: its root (or the incense baby itself)
    fams = []
    for s in missing_species:
        r = root(s)
        if r in BABIES and r not in INCENSE_BABIES and r != 'SPECIES_TOGEPI':
            # add the first non-baby form instead
            r = evo[r][0][2]
        if r not in fams and r not in have: fams.append(r)
    # starters and the second fossil are single-choice gifts: make every one of them catchable
    for s in ['SPECIES_TREECKO', 'SPECIES_TORCHIC', 'SPECIES_MUDKIP', 'SPECIES_CHIKORITA', 'SPECIES_CYNDAQUIL',
              'SPECIES_TOTODILE', 'SPECIES_LILEEP', 'SPECIES_ANORITH']:
        if s not in fams: fams.append(s)
    return wild, have, missing_species, fams

def apply(fams, wild):
    by_label = {e['base_label']: e for e in encs}
    slot_count = collections.Counter(wild)
    edits_per_table = collections.Counter()
    log = []
    PREF_SLOTS = {'land_mons': [4, 5, 6, 7, 8, 9, 3, 2], 'water_mons': [2, 3, 1], 'rock_smash_mons': [2, 3, 1],
                  'fishing_mons': [3, 4, 7, 8, 6]}
    MAX_EDITS = {'land_mons': 5, 'water_mons': 2, 'rock_smash_mons': 2, 'fishing_mons': 3}

    def table_presence(sp):
        return sum(1 for e in encs if usable_table(e) and any(m['species'] == sp for f in FIELDS if f in e for m in e[f]['mons']))

    def can_replace(e, f, i):
        old = e[f]['mons'][i]['species']
        if (e['base_label'], f) in BLACKLIST: return False
        if old in fams_set: return False
        if slot_count[old] <= 2: return False
        # keep each table's identity: never remove the last slot of a species that lives in few tables
        if sum(1 for m in e[f]['mons'] if m['species'] == old) == 1 and table_presence(old) <= 3: return False
        if edits_per_table[(e['base_label'], f)] >= MAX_EDITS[f]: return False
        return True

    def do_replace(e, f, i, s):
        old = e[f]['mons'][i]['species']
        slot = e[f]['mons'][i]
        lv = STRONG_MIN_LEVEL(s)
        slot['species'] = s
        if slot['max_level'] < lv:
            slot['min_level'] = max(slot['min_level'], lv - 2); slot['max_level'] = max(slot['max_level'], lv)
        slot_count[old] -= 1; slot_count[s] += 1
        edits_per_table[(e['base_label'], f)] += 1
        log.append('%-22s %-28s %-16s slot %d  (was %s) lv %d-%d' % (s[8:], e['base_label'][1:], f, i, old[8:], slot['min_level'], slot['max_level']))

    fams_set = set(fams)
    # hand-placed species first, so their tables still have room
    for s in sorted(fams, key=lambda x: 0 if x in OVERRIDES else 1):
        t = set(info[s]['types'])
        hab = habitats(s)
        want = 2
        done = 0
        if s in OVERRIDES:
            for lab in OVERRIDES[s]:
                if done >= want + 1: break
                e = by_label.get(lab)
                if not e: continue
                for f in [x for x in ['land_mons', 'water_mons', 'fishing_mons', 'rock_smash_mons'] if x in e and x in hab]:
                    placed = False
                    for i in PREF_SLOTS[f]:
                        if i < len(e[f]['mons']) and can_replace(e, f, i):
                            do_replace(e, f, i, s); placed = True; break
                    if placed: done += 1; break
            if done: continue
        # score every candidate slot
        cands = []
        for e in encs:
            if not usable_table(e): continue
            for f in FIELDS:
                if f not in e or f not in hab: continue
                lvl = avg_level(e, f)
                if lvl + 6 < STRONG_MIN_LEVEL(s): continue
                for i in PREF_SLOTS[f]:
                    if i >= len(e[f]['mons']) or not can_replace(e, f, i): continue
                    old = e[f]['mons'][i]['species']
                    ot = set(info[old]['types'])
                    sc = 4 * len(t & ot)
                    if is_cave(e) and not t & CAVE_TYPES: continue
                    if is_cave(e): sc += 3 if t & CAVE_TYPES else 0
                    elif f == 'land_mons' and t & CAVE_TYPES and not t & OUTDOOR_TYPES: sc -= 2
                    elif f == 'land_mons': sc += 1 if t & OUTDOOR_TYPES else 0
                    sc -= abs(info[s]['bst'] - info[old]['bst']) / 60
                    sc -= PREF_SLOTS[f].index(i) * 0.3
                    sc += random.random() * 0.8
                    cands.append((sc, e['base_label'], f, i))
        cands.sort(reverse=True)
        used = set()
        for sc, lab, f, i in cands:
            if done >= want: break
            if lab in used: continue
            e = by_label[lab]
            if not can_replace(e, f, i): continue
            do_replace(e, f, i, s); used.add(lab); done += 1
        if done == 0: log.append('!! could not place %s' % s)
    return log

if __name__ == '__main__':
    wild, have, missing, fams = plan()
    print('obtainable now:', len(have), '/ 386; missing species:', len(missing))
    print('families to add:', len(fams), ' '.join(s[8:] for s in fams))
    log = apply(fams, wild)
    print('\n'.join(log))
    wild2 = present_species()
    have2 = obtainable(wild2)
    print('obtainable after:', len(have2), [s for s in nat if s not in have2])
    if '--apply' in sys.argv:
        json.dump(data, open(WJ, 'w'), indent=2)
        open(WJ, 'a').write('\n')
        print('written', WJ)
