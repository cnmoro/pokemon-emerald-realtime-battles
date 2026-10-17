#!/usr/bin/env python3
"""Real-time mod (FireRed): add every Gen 1/2 family that FireRed doesn't have in the wild
(starters, version exclusives, trade/gift/fossil-only species, Johto bases and babies) to fitting
FireRed wild tables. Each new species takes over a duplicate slot of a species that has more than
one, so table sizes stay valid. Writes src/data/wild_encounters.json and docs/POKEMON_LOCAIS.md."""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wildplan import R, load_wild, fr_encounters

# species, map, area (land/water/rock/fish_super/fish_good), min, max, note
PLAN = [
    ('PICHU',      'ROUTE2',                     'land', 3, 5,   'perto do Pikachu da floresta'),
    ('PICHU',      'VIRIDIAN_FOREST',            'land', 3, 6,   'bebê do Pikachu'),
    ('CLEFFA',     'MT_MOON_1F',                 'land', 7, 10,  'bebê do Clefairy, em Mt. Moon'),
    ('IGGLYBUFF',  'ROUTE3',                     'land', 4, 7,   'bebê do Jigglypuff, onde ele aparece'),
    ('HOOTHOOT',   'ROUTE1',                     'land', 3, 5,   'coruja das primeiras rotas'),
    ('SUNKERN',    'ROUTE2',                     'land', 3, 5,   'semente de girassol'),
    ('BULBASAUR',  'ROUTE24',                    'land', 8, 12,  'inicial de Kanto'),
    ('CHARMANDER', 'ROUTE4',                     'land', 8, 12,  'inicial de Kanto'),
    ('SQUIRTLE',   'ROUTE25',                    'land', 8, 12,  'inicial de Kanto, perto do mar'),
    ('SANDSHREW',  'ROUTE4',                     'land', 7, 12,  'exclusivo do LeafGreen'),
    ('VULPIX',     'ROUTE7',                     'land', 17, 21, 'exclusivo do LeafGreen'),
    ('BELLSPROUT', 'ROUTE5',                     'land', 11, 15, 'exclusivo do LeafGreen'),
    ('SLOWPOKE',   'SEAFOAM_ISLANDS_B1F',        'land', 24, 30, 'exclusivo do LeafGreen'),
    ('STARYU',     'ROUTE19',                    'water', 20, 35,'exclusivo do LeafGreen (surf)'),
    ('STARYU',     'VERMILION_CITY',             'fish_super', 15, 30, 'exclusivo do LeafGreen (Super Rod)'),
    ('PINSIR',     'SAFARI_ZONE_CENTER',         'land', 23, 30, 'exclusivo do LeafGreen'),
    ('FARFETCHD',  'ROUTE12',                    'land', 22, 28, 'antes só por troca'),
    ('LICKITUNG',  'ROUTE18',                    'land', 22, 28, 'antes só por troca'),
    ('MR_MIME',    'ROUTE8',                     'land', 16, 20, 'antes só por troca (perto de Saffron)'),
    ('EEVEE',      'ROUTE16',                    'land', 18, 24, 'antes só presente em Celadon'),
    ('PORYGON',    'POWER_PLANT',                'land', 24, 32, 'antes só no Game Corner'),
    ('OMANYTE',    'MT_MOON_B2F',                'land', 10, 12, 'antes só pelo fóssil'),
    ('KABUTO',     'MT_MOON_B2F',                'land', 10, 12, 'antes só pelo fóssil'),
    ('AERODACTYL', 'VICTORY_ROAD_2F',            'land', 36, 42, 'antes só pelo Old Amber'),
    ('SNORLAX',    'ROUTE15',                    'land', 26, 30, 'antes só os dois estáticos'),
    ('CHIKORITA',  'ROUTE3',                     'land', 5, 8,   'inicial de Johto'),
    ('CYNDAQUIL',  'ROUTE9',                     'land', 12, 16, 'inicial de Johto'),
    ('TOTODILE',   'ROUTE11',                    'land', 12, 16, 'inicial de Johto, perto da água'),
    ('CHINCHOU',   'ROUTE20',                    'fish_super', 20, 30, 'mar aberto (Super Rod)'),
    ('TOGEPI',     'ROUTE24',                    'land', 8, 12,  'antes só o ovo de Sevii'),
    ('MARILL',     'ROUTE6',                     'land', 11, 15, 'perto do lago de Vermilion'),
    ('SUDOWOODO',  'ROUTE10',                    'land', 13, 17, 'a "árvore" da rota'),
    ('MISDREAVUS', 'POKEMON_TOWER_5F',           'land', 15, 20, 'fantasma da torre'),
    ('GIRAFARIG',  'SAFARI_ZONE_EAST',           'land', 23, 30, 'savana do Safari'),
    ('GLIGAR',     'ROUTE23',                    'land', 34, 40, 'penhascos antes da Victory Road'),
    ('SNUBBULL',   'ROUTE7',                     'land', 17, 21, 'perto de Celadon'),
    ('SNEASEL',    'SEAFOAM_ISLANDS_B2F',        'land', 26, 32, 'caverna gelada'),
    ('CORSOLA',    'ONE_ISLAND_TREASURE_BEACH',  'water', 25, 35,'coral da praia (surf)'),
    ('REMORAID',   'ROUTE21_NORTH',              'fish_super', 20, 30, 'mar (Super Rod)'),
    ('MANTINE',    'ROUTE19',                    'water', 25, 35,'mar (surf)'),
    ('TYROGUE',    'ROUTE17',                    'land', 20, 26, 'Cycling Road, lutadores'),
    ('SMOOCHUM',   'SEAFOAM_ISLANDS_1F',         'land', 22, 28, 'bebê da Jynx, caverna gelada'),
    ('ELEKID',     'POWER_PLANT',                'land', 22, 28, 'bebê do Electabuzz'),
    ('MAGBY',      'POKEMON_MANSION_1F',         'land', 24, 30, 'bebê do Magmar, mansão queimada'),
    ('MILTANK',    'ROUTE13',                    'land', 22, 28, 'pasto da rota'),
]

KEY = {'land': 'land_mons', 'water': 'water_mons', 'rock': 'rock_smash_mons', 'fish_super': 'fishing_mons', 'fish_good': 'fishing_mons'}
RANGE = {'land': (0, 12), 'water': (0, 5), 'rock': (0, 5), 'fish_super': (5, 10), 'fish_good': (2, 5)}

def apply():
    d = load_wild()
    encs = {e['map']: e for e in fr_encounters(d)}
    for sp, mp, area, lo, hi, note in PLAN:
        e = encs['MAP_' + mp]
        k = KEY[area]
        mons = e[k]['mons']
        a, b = RANGE[area]
        if any(m['species'] == 'SPECIES_' + sp for m in mons[a:b]):
            continue
        counts = {}
        for i in range(a, b):
            counts.setdefault(mons[i]['species'], []).append(i)
        donor = max(counts.values(), key=len)
        if len(donor) < 2:
            raise SystemExit('no free slot for %s in %s %s' % (sp, mp, area))
        mons[donor[-1]] = {'min_level': lo, 'max_level': hi, 'species': 'SPECIES_' + sp}
    json.dump(d, open(f'{R}/src/data/wild_encounters.json', 'w'), indent=2, ensure_ascii=False)
    open(f'{R}/src/data/wild_encounters.json', 'a').write('\n')

def doc():
    rows = {}
    for sp, mp, area, lo, hi, note in PLAN:
        rows.setdefault(sp, []).append((mp, area, lo, hi, note))
    nice = lambda s: s.replace('_', ' ').title().replace('Mr Mime', 'Mr. Mime').replace('Farfetchd', "Farfetch'd")
    where = {'land': 'grama/chão', 'water': 'surf', 'rock': 'Rock Smash', 'fish_super': 'Super Rod', 'fish_good': 'Good Rod'}
    out = ['# Pokémon adicionados à natureza (FireRed)', '',
           'No FireRed original muitos Pokémon da 1ª e 2ª geração só vinham de troca, presente, fóssil, escolha única',
           '(iniciais) ou da versão LeafGreen. Neste mod, a forma base de **toda família** da 1ª/2ª geração (e os',
           'bebês: Pichu, Cleffa, Igglybuff, Togepi, Tyrogue, Smoochum, Elekid, Magby) pode ser capturada na natureza;',
           'o resto vem por evolução (todas possíveis sem troca: Kadabra, Machoke, Graveler e Haunter no nível 37;',
           "Onix/Scyther com Metal Coat, Seadra com Dragon Scale, Porygon com Up-Grade, Poliwhirl/Slowpoke com King's",
           'Rock; Espeon com Sun Stone e Umbreon com Moon Stone, já que o FireRed não tem relógio). Os lendários vêm do',
           'sistema de lendários (cães de Johto andando livres, Celebi e Mew no pós-jogo) ou dos encontros originais.',
           '', 'Em cada mapa, toda espécie tem a mesma chance de aparecer (encontros equilibrados).', '',
           '| Pokémon | Onde | Tipo de encontro | Nível | Observação |', '|---|---|---|---|---|']
    for sp in rows:
        for mp, area, lo, hi, note in rows[sp]:
            out.append('| %s | %s | %s | %d–%d | %s |' % (nice(sp), nice(mp), where[area], lo, hi, note))
    os.makedirs(f'{R}/docs', exist_ok=True)
    open(f'{R}/docs/POKEMON_LOCAIS.md', 'w').write('\n'.join(out) + '\n')

if __name__ == '__main__':
    apply()
    doc()
