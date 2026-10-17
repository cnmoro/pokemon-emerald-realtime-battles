#!/usr/bin/env python3
"""Shared data for the FireRed wild encounter plan: species info, evolutions, current FireRed tables."""
import json, re, os
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')

def species_list():
    names = []
    for l in open(f'{R}/include/constants/species.h'):
        m = re.match(r'#define (SPECIES_\w+)\s+(\d+)', l)
        if m and m.group(1) not in ('SPECIES_NONE',):
            names.append((m.group(1), int(m.group(2))))
    return names

def species_info():
    s = open(f'{R}/src/data/pokemon/species_info.h').read()
    info = {}
    for m in re.finditer(r'\[(SPECIES_\w+)\]\s*=\s*\{(.*?)\n    \}', s, re.S):
        body = m.group(2)
        g = lambda k: int(re.search(r'\.%s = (\d+)' % k, body).group(1)) if re.search(r'\.%s = (\d+)' % k, body) else 0
        t = re.search(r'\.types = \{(\w+), (\w+)\}', body)
        bst = sum(g(k) for k in ('baseHP', 'baseAttack', 'baseDefense', 'baseSpeed', 'baseSpAttack', 'baseSpDefense'))
        info[m.group(1)] = {'types': (t.group(1), t.group(2)) if t else ('', ''), 'bst': bst}
    return info

def evolutions():
    s = open(f'{R}/src/data/pokemon/evolution.h').read()
    evo = {}
    for m in re.finditer(r'\[(SPECIES_\w+)\]\s*=\s*\{(.*?)\}\},', s, re.S):
        for e in re.finditer(r"\{(EVO_\w+), (\w+), (SPECIES_\w+)\}", m.group(0)):
            evo.setdefault(m.group(1), []).append((e.group(1), e.group(2), e.group(3)))
    return evo

def load_wild():
    return json.load(open(f'{R}/src/data/wild_encounters.json'))

def fr_encounters(d):
    return [e for e in d['wild_encounter_groups'][0]['encounters'] if e['base_label'].endswith('_FireRed')]

LEGENDS = {'SPECIES_ARTICUNO', 'SPECIES_ZAPDOS', 'SPECIES_MOLTRES', 'SPECIES_MEWTWO', 'SPECIES_MEW', 'SPECIES_RAIKOU',
           'SPECIES_ENTEI', 'SPECIES_SUICUNE', 'SPECIES_LUGIA', 'SPECIES_HO_OH', 'SPECIES_CELEBI'}
BABIES = {'SPECIES_PICHU', 'SPECIES_CLEFFA', 'SPECIES_IGGLYBUFF', 'SPECIES_TOGEPI', 'SPECIES_TYROGUE',
          'SPECIES_SMOOCHUM', 'SPECIES_ELEKID', 'SPECIES_MAGBY'}
