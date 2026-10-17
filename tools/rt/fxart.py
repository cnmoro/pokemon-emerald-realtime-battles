# New visual-effect art for the real-time battle engine (exec'd by fxgen.py: uses Canvas, add, mkpal, P...).
# Shapes here are streamed into VRAM on demand, so there is no hard limit on their number.
# Ramp indices: 1 = darkest ... 15 = brightest; 0 = transparent. Energy shapes have no dark outline:
# their rim is the dark end of the ramp, their core the bright end.
import math, random
R2 = random.Random(77)

def clamp(v, a, b): return a if v < a else b if v > b else v
def lv(t):  # 0..1 -> 2..15
    return 2 + int(round(clamp(t, 0, 1) * 13))

def soft_disc(c, cx, cy, r, gamma=1.0, lo=0.0, hi=1.0):
    for y in range(c.h):
        for x in range(c.w):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy) / r
            if d < 1:
                c.putmax(x, y, lv(lo + (hi - lo) * (1 - d) ** gamma))

def thick(c, x0, y0, x1, y1, w, vcore, vedge=None):
    # thick segment with a bright centre falling off to vedge at radius w/2
    if vedge is None: vedge = vcore
    L = math.hypot(x1 - x0, y1 - y0) + 1e-9
    for y in range(c.h):
        for x in range(c.w):
            px, py = x + 0.5, y + 0.5
            t = ((px - x0) * (x1 - x0) + (py - y0) * (y1 - y0)) / (L * L)
            t = clamp(t, 0, 1)
            qx, qy = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            d = math.hypot(px - qx, py - qy)
            if d <= w / 2:
                u = 1 - d / (w / 2)
                c.putmax(x, y, int(round(vedge + (vcore - vedge) * u)))

def polyfill(c, pts, f):
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    for y in range(max(0, int(min(ys))), min(c.h, int(max(ys)) + 1)):
        for x in range(max(0, int(min(xs))), min(c.w, int(max(xs)) + 1)):
            px, py = x + 0.5, y + 0.5; inside = False; j = len(pts) - 1
            for i in range(len(pts)):
                xi, yi = pts[i]; xj, yj = pts[j]
                if ((yi > py) != (yj > py)) and (px < (xj - xi) * (py - yi) / (yj - yi + 1e-9) + xi): inside = not inside
                j = i
            if inside:
                v = f(px, py)
                if v: c.putmax(x, y, v)

def dark_rim(c, v=2):
    # 1px rim of the darkest ramp tone around opaque pixels (for solid objects, not glows)
    q = [row[:] for row in c.p]
    for y in range(c.h):
        for x in range(c.w):
            if c.p[y][x] == 0:
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    if c.get(x + dx, y + dy) > v:
                        q[y][x] = 1; break
    c.p = q

# ---------------------------------------------------------------- arena edge dimming
def mk_edgedim():
    # 32x64: dither ramp over the first 8 px (from the field side), solid after that; colour index 15 of the UI
    # palette (near black). Drawn semi-transparent, so it darkens what is behind it like the scanline dimming.
    c = Canvas(32, 64)
    bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    for y in range(64):
        for x in range(32):
            t = min(1.0, (x + 0.5) / 10.0) * 0.5   # ramps up to a 50% checker (reads as dimmed scenery)
            if t * 16 > bayer[y & 3][x & 3]: c.p[y][x] = 15
    return [c]
add('EDGEDIM', 32, 64, mk_edgedim(), ui=True)

# ---------------------------------------------------------------- glows and flares
def mk_glow32():
    c = Canvas(32, 32); soft_disc(c, 16, 16, 15.5, gamma=2.2, lo=0.0, hi=0.85); return [c]
add('GLOW32', 32, 32, mk_glow32())

def mk_mote8():
    fr = []
    for f in range(2):
        c = Canvas(8, 8); soft_disc(c, 4, 4, 3.6 - f * 0.6, gamma=1.3)
        c.put(4, 4, 15); fr.append(c)
    return fr
add('MOTE8', 8, 8, mk_mote8())

def mk_flare32():
    fr = []
    for f, (L, l) in enumerate([(9, 4), (15, 7), (12, 5)]):
        c = Canvas(32, 32)
        soft_disc(c, 16, 16, 5 + f, gamma=1.2, lo=0.4)
        for k in range(8):
            a = k * math.pi / 4 + (0.2 if f == 2 else 0)
            ln = L if k % 2 == 0 else l
            thick(c, 16, 16, 16 + math.cos(a) * ln, 16 + math.sin(a) * ln, 2.6 if k % 2 == 0 else 1.8, 15, 6)
        c.put(16, 16, 15); c.put(15, 16, 15); c.put(16, 15, 15); c.put(15, 15, 15)
        fr.append(c)
    return fr
add('FLARE32', 32, 32, mk_flare32())

def mk_sparkle8():
    fr = []
    for f, s in enumerate([1, 2.5, 3.5, 2]):
        c = Canvas(8, 8)
        for k in range(-4, 5):
            t = 1 - abs(k) / (s + 0.5)
            if t > 0:
                c.putmax(4 + k, 4, lv(t)); c.putmax(4, 4 + k, lv(t))
        if s > 2:
            for d in (1, -1):
                c.putmax(4 + d, 4 + d, 8); c.putmax(4 + d, 4 - d, 8)
        c.put(4, 4, 15); fr.append(c)
    return fr
add('SPARKLE8', 8, 8, mk_sparkle8())

# ---------------------------------------------------------------- lightning
def mk_lbolt(w):
    # a straight bolt segment along +x: white core, coloured glow; slight jitter inside the piece
    fr = []
    for f in range(2):
        c = Canvas(w, 8)
        pts = [(0, 4)]
        for x in range(4, w, 4):
            pts.append((x, 4 + R2.choice([-1, 0, 0, 1])))
        pts.append((w, 4))
        for i in range(len(pts) - 1):
            thick(c, *pts[i], *pts[i + 1], 5.2, 9, 3)
        for i in range(len(pts) - 1):
            thick(c, *pts[i], *pts[i + 1], 1.6, 15, 15)
        fr.append(c)
    return fr
add('LBOLT32', 32, 8, mk_lbolt(32))
add('LBOLT16', 16, 8, mk_lbolt(16))

def mk_crackle16():
    # small electric crackle: a few jagged sparks from the centre
    fr = []
    for f in range(3):
        c = Canvas(16, 16)
        for k in range(3):
            a = R2.random() * 6.283; x, y = 8.0, 8.0
            for s in range(3):
                a += R2.uniform(-0.9, 0.9); nx, ny = x + math.cos(a) * 2.6, y + math.sin(a) * 2.6
                thick(c, x, y, nx, ny, 2.2, 15, 7); x, y = nx, ny
        c.put(8, 8, 15); fr.append(c)
    return fr
add('CRACKLE16', 16, 16, mk_crackle16())

# ---------------------------------------------------------------- beams
def mk_beam32():
    # beam body along +x, seamless when repeated: bright core band, soft falloff to the sides; 3 frames of
    # moving streaks
    fr = []
    for f in range(3):
        c = Canvas(32, 16)
        for y in range(16):
            dy = abs(y + 0.5 - 8) / 7.5
            for x in range(32):
                streak = 0.08 * math.sin((x + f * 11) * 2 * math.pi / 32 * 2 + y * 0.9)
                t = (1 - dy ** 1.6) + streak
                if dy < 0.25: t = 1
                if t > 0.08: c.put(x, y, lv(t))
        fr.append(c)
    return fr
add('BEAM32', 32, 16, mk_beam32())

def mk_beamthin32():
    fr = []
    for f in range(2):
        c = Canvas(32, 8)
        for y in range(8):
            dy = abs(y + 0.5 - 4) / 3.6
            for x in range(32):
                t = 1 - dy ** 1.4 + 0.1 * math.sin((x + f * 8) * 0.8)
                if dy < 0.3: t = 1
                if t > 0.1: c.put(x, y, lv(t))
        fr.append(c)
    return fr
add('BEAMTHIN32', 32, 8, mk_beamthin32())

def mk_crystal16():
    # ice beam crystal growing along the beam (directional, points +x)
    fr = []
    for f in range(2):
        c = Canvas(16, 16)
        s = 1.0 if f == 0 else 0.8
        pts = [(1, 8), (6, 3 * s + 5 * (1 - s)), (15, 8), (6, 13 - 3 * (1 - s))]
        polyfill(c, [(1, 8), (6, 8 - 5 * s), (15, 8), (6, 8 + 5 * s)],
                 lambda px, py: lv(0.95 - 0.5 * abs(py - 8) / 5 - 0.25 * (px / 16) + (0.2 if py < 8 else 0)))
        thick(c, 3, 8, 13, 8, 1.2, 15, 15)
        dark_rim(c); fr.append(c)
    return fr
add('CRYSTAL16', 16, 16, mk_crystal16())

# ---------------------------------------------------------------- water
def mk_jet16():
    # water jet segment along +x: blue body, a bright streak along the top, foamy head
    fr = []
    for f in range(2):
        c = Canvas(16, 16)
        for y in range(16):
            for x in range(16):
                u = (x + 0.5) / 16.0; v = (y + 0.5 - 8)
                w = 3.4 * (0.55 + 0.45 * math.sin(u * math.pi)) * (1 + 0.12 * math.sin(u * 9 + f * 3))
                if abs(v) < w:
                    t = 0.25 + 0.35 * (1 - abs(v) / w)
                    if -w * 0.55 < v < -w * 0.15: t = 0.8 + 0.15 * math.sin(u * 7 + f)
                    c.putmax(x, y, lv(t))
        for k in range(3):
            c.putmax(R2.randint(2, 13), R2.randint(6, 9), 15)
        fr.append(c)
    return fr
add('JET16', 16, 16, mk_jet16())

def mk_foam8():
    fr = []
    for f in range(3):
        c = Canvas(8, 8)
        for k in range(3 + f):
            soft_disc(c, 4 + R2.uniform(-1.6, 1.6), 4 + R2.uniform(-1.6, 1.6), 1.8 + R2.random(), gamma=0.6, lo=0.5, hi=1.0)
        fr.append(c)
    return fr
add('FOAM8', 8, 8, mk_foam8())

def mk_splash32():
    # crown splash, 4 frames rising and falling apart
    fr = []
    for f in range(4):
        c = Canvas(32, 32)
        h = [6, 11, 13, 10][f]; spread = [5, 8, 11, 13][f]
        for k in range(7):
            a = -math.pi / 2 + (k - 3) * 0.38
            x0, y0 = 16 + (k - 3) * 1.2, 26
            x1 = 16 + math.cos(a) * spread * 1.1 + (k - 3) * 1.5; y1 = 26 - h * (1 - abs(k - 3) / 5)
            if f < 3: thick(c, x0, y0, x1, y1, 2.4, 13, 6)
            soft_disc(c, x1, y1 - (f == 3) * -3, 1.9 if f < 3 else 1.4, gamma=0.7, lo=0.5)
        for x in range(32):  # base ripple
            dx = abs(x + 0.5 - 16) / (8 + f * 3)
            if dx < 1: c.putmax(x, 27, lv(0.3 + 0.5 * (1 - dx))); c.putmax(x, 28, lv(0.2 + 0.3 * (1 - dx)))
        fr.append(c)
    return fr
add('SPLASH32', 32, 32, mk_splash32())

def mk_crest32():
    # surf wave crest seen from above-front: curling lip with foam (32x16), 3 frames
    fr = []
    for f in range(3):
        c = Canvas(32, 16)
        for x in range(32):
            ph = (x + f * 4) * 2 * math.pi / 16
            top = 4 + 1.5 * math.sin(ph)
            for y in range(16):
                if y >= top:
                    t = 0.55 + 0.4 * (1 - (y - top) / (16 - top))
                    c.put(x, y, lv(t * 0.85))
            for y in range(int(top), int(top) + 3):
                c.put(x, y, lv(1.0 - (y - top) * 0.15))
        for k in range(6):
            soft_disc(c, R2.uniform(0, 32), R2.uniform(2, 6), 1.6, gamma=0.5, lo=0.7)
        fr.append(c)
    return fr
add('CREST32', 32, 16, mk_crest32())

# ---------------------------------------------------------------- fire
def mk_fire(sz, frames, seed):
    rr = random.Random(seed); fr = []
    for f in range(frames):
        c = Canvas(sz, sz); cx = sz / 2; base = sz * 0.9
        ph = f * 2 * math.pi / frames
        for y in range(sz):
            for x in range(sz):
                ny = (base - (y + 0.5)) / (sz * 0.85)
                if ny < -0.12 or ny > 1: continue
                wob = math.sin(ny * 6 + ph) * sz * 0.07 * ny
                w = (sz * 0.33) * (1 - max(ny, 0)) ** 0.7 * (1 + 0.18 * math.sin(ph + ny * 5))
                if ny < 0: w *= math.sqrt(max(0, 1 - (ny / 0.12) ** 2))
                dx = abs(x + 0.5 - cx - wob)
                if dx < w:
                    t = (1 - dx / w) * 0.6 + (1 - ny) * 0.55 - 0.15 + rr.uniform(-0.05, 0.05)
                    if t > 0.05: c.putmax(x, y, lv(t))
        # licking tongues
        for k in range(2):
            a = ph + k * 3.1
            x0 = cx + math.sin(a) * sz * 0.18; y0 = sz * 0.45
            thick(c, x0, y0, x0 + math.sin(a * 1.3) * sz * 0.08, sz * 0.12, sz / 10, 9, 4)
        fr.append(c)
    return fr
add('FIRE16', 16, 16, mk_fire(16, 6, 3))
add('FIRE32', 32, 32, mk_fire(32, 4, 4))

def mk_ember8():
    fr = []
    for f in range(4):
        c = Canvas(8, 8); r = [2.6, 2.2, 1.7, 1.2][f]
        soft_disc(c, 4, 4, r + 0.6, gamma=0.8)
        thick(c, 4, 4, 4, 7.5, 1.2, 6, 3)
        fr.append(c)
    return fr
add('EMBER8', 8, 8, mk_ember8())

# ---------------------------------------------------------------- smoke / dust
def mk_puff(sz, frames, seed):
    rr = random.Random(seed); fr = []
    blobs = [(rr.uniform(-0.25, 0.25), rr.uniform(-0.25, 0.25), rr.uniform(0.18, 0.3)) for _ in range(6)]
    for f in range(frames):
        c = Canvas(sz, sz); g = 0.62 + f * 0.17
        for (bx, by, br) in blobs:
            r = br * sz * g
            cx, cy = sz / 2 + bx * sz * g, sz / 2 + by * sz * g
            for y in range(sz):
                for x in range(sz):
                    d = math.hypot(x + 0.5 - cx, y + 0.5 - cy) / r
                    if d < 1:
                        t = (1 - d) * 0.5 + 0.35 - 0.18 * f + (0.15 if y + 0.5 < cy else 0)
                        if t > 0.05: c.putmax(x, y, lv(t))
        fr.append(c)
    return fr
add('PUFF16', 16, 16, mk_puff(16, 3, 9))
add('PUFF32', 32, 32, mk_puff(32, 3, 10))

def mk_debris8():
    fr = []
    for f in range(4):
        c = Canvas(8, 8); a0 = f * math.pi / 4
        pts = [(4 + math.cos(a0 + k * 2.1) * (3.2 if k % 2 else 2.3), 4 + math.sin(a0 + k * 2.1) * (3.2 if k % 2 else 2.3)) for k in range(5)]
        polyfill(c, pts, lambda px, py: lv(0.8 - 0.5 * ((px + py) / 12)))
        dark_rim(c); fr.append(c)
    return fr
add('DEBRIS8', 8, 8, mk_debris8())

def mk_rock16b():
    fr = []
    for f in range(2):
        rr = random.Random(20 + f); c = Canvas(16, 16); pts = []
        for k in range(9):
            a = k * 2 * math.pi / 9 + rr.uniform(-0.15, 0.15); r = rr.uniform(5.2, 6.8)
            pts.append((8 + math.cos(a) * r, 8.5 + math.sin(a) * r * 0.85))
        polyfill(c, pts, lambda px, py: lv(0.85 - 0.55 * ((px - 2) + (py - 2)) / 24 + rr.uniform(-0.05, 0.05)))
        # facet highlight and crack
        thick(c, 5, 6, 9, 5, 1.2, 14, 14); thick(c, 9, 9, 11, 12, 1.0, 4, 4)
        dark_rim(c); fr.append(c)
    return fr
add('ROCK16B', 16, 16, mk_rock16b())

def mk_rock32b():
    rr = random.Random(31); c = Canvas(32, 32); pts = []
    for k in range(11):
        a = k * 2 * math.pi / 11 + rr.uniform(-0.12, 0.12); r = rr.uniform(11, 14)
        pts.append((16 + math.cos(a) * r, 17 + math.sin(a) * r * 0.82))
    polyfill(c, pts, lambda px, py: lv(0.88 - 0.6 * ((px - 3) + (py - 3)) / 52 + rr.uniform(-0.04, 0.04)))
    thick(c, 9, 11, 17, 8, 1.6, 14, 13); thick(c, 17, 18, 22, 24, 1.2, 4, 4); thick(c, 12, 20, 15, 23, 1.0, 5, 5)
    dark_rim(c); return [c]
add('ROCK32B', 32, 32, mk_rock32b())

def mk_crack32():
    # ground crack decal (32x16), 2 variants
    fr = []
    for f in range(2):
        rr = random.Random(40 + f); c = Canvas(32, 16)
        for k in range(4):
            a = k * math.pi / 2 + rr.uniform(-0.5, 0.5); x, y = 16.0, 8.0
            for s in range(4):
                a += rr.uniform(-0.6, 0.6); nx = x + math.cos(a) * 3.6; ny = y + math.sin(a) * 1.8
                thick(c, x, y, nx, ny, 1.8 - s * 0.25, 3, 2); x, y = nx, ny
        fr.append(c)
    return fr
add('CRACK32', 32, 16, mk_crack32())

# ---------------------------------------------------------------- ice
def mk_snow8():
    fr = []
    for f in range(2):
        c = Canvas(8, 8)
        for k in range(3):
            a = k * math.pi / 3 + f * math.pi / 6
            thick(c, 4 - math.cos(a) * 3.4, 4 - math.sin(a) * 3.4, 4 + math.cos(a) * 3.4, 4 + math.sin(a) * 3.4, 1.1, 13, 13)
        c.put(4, 4, 15); fr.append(c)
    return fr
add('SNOW8', 8, 8, mk_snow8())

def mk_shard16b():
    # long ice spike pointing +x, faceted
    c = Canvas(16, 16)
    polyfill(c, [(0, 8), (5, 4.5), (16, 8), (5, 11.5)], lambda px, py: lv((0.95 if py < 8 else 0.6) - 0.3 * abs(py - 8) / 4 + 0.1 * px / 16))
    thick(c, 2, 8, 14, 8, 0.9, 15, 15)
    dark_rim(c); return [c]
add('SHARD16B', 16, 16, mk_shard16b())

# ---------------------------------------------------------------- slashes / strikes
def mk_arc32():
    # crescent slash with a bright leading edge, sweeping (3 frames), oriented along +x (cut direction up)
    fr = []
    for f in range(3):
        c = Canvas(32, 32)
        a0, a1 = -1.9 + f * 0.25, 0.5 + f * 0.35
        for i in range(60):
            t = i / 59; a = a0 + (a1 - a0) * t
            for rr_ in range(-5, 6):
                r = 12 + rr_ * 0.6
                x = 14 + math.cos(a) * r; y = 16 + math.sin(a) * r
                body = math.sin(t * math.pi) ** 0.8
                edge = 1 - abs(rr_ + 2.0) / (6.5 * body + 0.5)
                v = body * edge * (0.6 + 0.4 * t)
                if v > 0.12: c.putmax(x, y, lv(v))
        fr.append(c)
    return fr
add('ARC32', 32, 32, mk_arc32())

def mk_claw32():
    # three parallel claw streaks (diagonal), 3 frames: drawing in, full, fading thin
    fr = []
    for f in range(3):
        c = Canvas(32, 32)
        for k in range(3):
            off = (k - 1) * 7
            L = [0.55, 1.0, 1.0][f]; w = [2.6, 3.0, 1.6][f]
            x0, y0 = 7 + off, 5 - off * 0.2; x1, y1 = x0 + 18 * L, y0 + 22 * L
            thick(c, x0, y0, x1, y1, w, 15, 7)
        fr.append(c)
    return fr
add('CLAW32', 32, 32, mk_claw32())

def mk_jaws32():
    # bite: two rows of fangs closing on the centre (3 frames)
    fr = []
    for f in range(3):
        c = Canvas(32, 32); gap = [9, 4, 0][f]
        for side in (-1, 1):
            gum = 16 + side * (gap + 9)   # gum line
            tip = 16 + side * gap         # fang tips
            for k in range(5):
                tx = 4.5 + k * 5.75
                h = 1.0 if k in (0, 4) else 0.8
                polyfill(c, [(tx - 2.6, gum), (tx + 2.6, gum), (tx, gum + (tip - gum) * h)],
                         lambda px, py, tx=tx: lv(0.98 - 0.45 * abs(px - tx) / 2.6))
            polyfill(c, [(2, gum - side * 2.5), (30, gum - side * 2.5), (30, gum), (2, gum)], lambda px, py: lv(0.45))
        dark_rim(c); fr.append(c)
    return fr
add('JAWS32', 32, 32, mk_jaws32())

def mk_boom32():
    # comic impact burst: jagged star with white core (3 frames: pop, full, breaking)
    fr = []
    for f in range(3):
        c = Canvas(32, 32); n = 9; R = [9, 14, 15][f]; r = [4, 7, 9][f]
        rr = random.Random(60 + f); pts = []
        for k in range(2 * n):
            a = k * math.pi / n + rr.uniform(-0.12, 0.12)
            pts.append((16 + math.cos(a) * (R * rr.uniform(0.85, 1.1) if k % 2 == 0 else r), 16 + math.sin(a) * (R * rr.uniform(0.85, 1.1) if k % 2 == 0 else r)))
        polyfill(c, pts, lambda px, py: lv(1.15 - math.hypot(px - 16, py - 16) / R * (0.9 if f < 2 else 1.3)))
        if f == 2:
            soft_disc(c, 16, 16, 4, lo=0, hi=0)  # hollow centre feel
            for y in range(32):
                for x in range(32):
                    if math.hypot(x + 0.5 - 16, y + 0.5 - 16) < 5: c.p[y][x] = 0
        fr.append(c)
    return fr
add('BOOM32', 32, 32, mk_boom32())

def mk_speed16():
    # motion streak along +x (16x8), 2 frames
    fr = []
    for f in range(2):
        c = Canvas(16, 8)
        for k, (y, L) in enumerate([(2, 10), (4, 15), (6, 8)]):
            x0 = 1 + f * 2 + (k * 3) % 5
            thick(c, x0, y + 0.5, min(15.5, x0 + L), y + 0.5, 1.2, 13, 5)
        fr.append(c)
    return fr
add('SPEED16', 16, 8, mk_speed16())

def mk_fist32():
    c = Canvas(32, 32)
    # knuckles facing +x
    polyfill(c, [(6, 9), (22, 7), (28, 11), (28, 21), (22, 25), (6, 23)], lambda px, py: lv(0.8 - 0.35 * (py - 7) / 18 + 0.1 * (px - 6) / 22))
    for k in range(4):
        y = 9.5 + k * 3.6
        thick(c, 22, y, 28, y + 0.5, 1.0, 4, 4)
    thick(c, 12, 23, 20, 23, 1.2, 4, 4)
    dark_rim(c)
    for k, y in enumerate((8, 16, 24)):
        thick(c, 0, y, 5 - k % 2, y, 1.1, 10, 5)
    return [c]
add('FIST32', 32, 32, mk_fist32())

# ---------------------------------------------------------------- nature
def mk_leaf16b():
    fr = []
    for f in range(4):
        c = Canvas(16, 16); a = f * math.pi / 4
        for y in range(16):
            for x in range(16):
                dx = x + 0.5 - 8; dy = y + 0.5 - 8
                u = dx * math.cos(a) + dy * math.sin(a); v = -dx * math.sin(a) + dy * math.cos(a)
                L = 6.8
                if abs(u) < L:
                    W = 3.2 * (1 - (u / L) ** 2) * (1.0 if u < 0 else 0.85)
                    if abs(v) < W:
                        t = 0.45 + 0.4 * (1 - abs(v) / W) + (0.18 if v < 0 else -0.05)
                        if abs(v) < 0.55: t = 0.98
                        c.putmax(x, y, lv(t))
        dark_rim(c); fr.append(c)
    return fr
add('LEAF16B', 16, 16, mk_leaf16b())

def mk_star16b():
    fr = []
    for f in range(2):
        c = Canvas(16, 16); pts = []
        for k in range(10):
            a = -math.pi / 2 + k * math.pi / 5 + f * 0.2; r = 7.2 if k % 2 == 0 else 3.0
            pts.append((8 + math.cos(a) * r, 8 + math.sin(a) * r))
        polyfill(c, pts, lambda px, py: lv(1.05 - math.hypot(px - 7, py - 7) / 9))
        dark_rim(c); c.put(7, 6, 15); fr.append(c)
    return fr
add('STAR16B', 16, 16, mk_star16b())

def mk_goo16():
    fr = []
    for f in range(3):
        c = Canvas(16, 16); sq = [1.0, 1.15, 0.9][f]
        for y in range(16):
            for x in range(16):
                dx = (x + 0.5 - 8) / (5.8 * sq); dy = (y + 0.5 - 8.5) / (5.8 / sq)
                d = math.hypot(dx, dy) + 0.12 * math.sin(math.atan2(dy, dx) * 5 + f)
                if d < 1:
                    t = 0.35 + 0.5 * (1 - d) + (0.25 if (dx < -0.1 and dy < -0.1 and d < 0.6) else 0)
                    c.putmax(x, y, lv(t))
        c.put(6, 5, 15); c.put(5, 6, 14)
        dark_rim(c); fr.append(c)
    return fr
add('GOO16', 16, 16, mk_goo16())

# ---------------------------------------------------------------- barriers / auras
def mk_hex32():
    fr = []
    for f in range(2):
        c = Canvas(32, 32); R = 14.5
        pts = [(16 + math.cos(k * math.pi / 3 + math.pi / 6) * R, 16 + math.sin(k * math.pi / 3 + math.pi / 6) * R) for k in range(6)]
        for k in range(6):
            thick(c, *pts[k], *pts[(k + 1) % 6], 2.2, 15 if f == 0 else 12, 8)
        for k in range(6):
            a = k * math.pi / 3 + math.pi / 6
            thick(c, 16, 16, 16 + math.cos(a) * R * 0.95, 16 + math.sin(a) * R * 0.95, 1.0, 7, 7)
        soft_disc(c, 16, 16, 12, gamma=0.5, lo=0.05, hi=0.25)
        fr.append(c)
    return fr
add('HEX32', 32, 32, mk_hex32())

def mk_pane16():
    # tall glassy wall pane (16x32)
    c = Canvas(16, 32)
    for y in range(32):
        for x in range(16):
            t = 0.35 + 0.2 * math.sin((x + y * 0.5) * 0.7)
            if x in (0, 15) or y in (0, 31): t = 0.95
            c.put(x, y, lv(t))
    thick(c, 3, 26, 11, 4, 1.4, 15, 12)
    return [c]
add('PANE16', 16, 32, mk_pane16())

def mk_dorb32():
    # shadow ball: dark core, coloured swirling rim (3 frames)
    fr = []
    for f in range(3):
        c = Canvas(32, 32)
        for y in range(32):
            for x in range(32):
                dx = x + 0.5 - 16; dy = y + 0.5 - 16; d = math.hypot(dx, dy) / 12.5
                if d < 1:
                    a = math.atan2(dy, dx)
                    sw = 0.5 + 0.5 * math.sin(a * 3 + d * 6 + f * 2.1)
                    t = 0.12 + 0.75 * (d ** 2.2) * (0.55 + 0.45 * sw)
                    c.put(x, y, lv(t))
        soft_disc(c, 12, 11, 3, gamma=1, lo=0.3, hi=0.7)
        fr.append(c)
    return fr
add('DORB32', 32, 32, mk_dorb32())

def mk_twister32():
    fr = []
    for f in range(3):
        c = Canvas(32, 32)
        for k in range(6):
            y = 3 + k * 5; w = 3 + k * 2.2
            for i in range(48):
                a = i / 48 * 2 * math.pi + f * 0.8 + k * 1.1
                x = 16 + math.cos(a) * w + math.sin(k + f) * 1.2; yy = y + math.sin(a) * 1.8
                front = math.sin(a)
                c.putmax(x, yy, lv(0.35 + 0.6 * (front + 1) / 2))
                c.putmax(x, yy + 1, lv(0.25 + 0.4 * (front + 1) / 2))
        fr.append(c)
    return fr
add('TWISTER32', 32, 32, mk_twister32())

def mk_psywave32():
    fr = []
    for f in range(2):
        c = Canvas(32, 32)
        for y in range(32):
            for x in range(32):
                d = math.hypot((x + 0.5 - 16) / 1.0, (y + 0.5 - 16) / 0.55)
                for R in (13.5 - f * 1.5, 8 - f):
                    if abs(d - R) < 1.4: c.putmax(x, y, lv(0.95 - abs(d - R) / 2.2))
        fr.append(c)
    return fr
add('PSYWAVE32', 32, 32, mk_psywave32())

# ---------------------------------------------------------------- energy palettes (no dark outline needed)
def EP(name, keys): P[name] = mkpal(keys)
EP('E_FIRE',   [(0, (110, 10, 0)), (0.25, (210, 40, 10)), (0.55, (255, 120, 20)), (0.8, (255, 210, 70)), (1, (255, 255, 230))])
EP('E_HOT',    [(0, (200, 60, 0)), (0.3, (255, 150, 30)), (0.7, (255, 235, 120)), (1, (255, 255, 255))])
EP('E_ELEC',   [(0, (120, 90, 0)), (0.3, (230, 190, 20)), (0.65, (255, 245, 110)), (1, (255, 255, 255))])
EP('E_BOLT',   [(0, (40, 60, 160)), (0.35, (110, 170, 255)), (0.7, (210, 235, 255)), (1, (255, 255, 255))])
EP('E_WATER',  [(0, (10, 40, 120)), (0.3, (30, 110, 220)), (0.65, (100, 190, 255)), (0.88, (200, 240, 255)), (1, (255, 255, 255))])
EP('E_FOAM',   [(0, (90, 150, 210)), (0.4, (170, 215, 245)), (1, (255, 255, 255))])
EP('E_ICE',    [(0, (40, 90, 150)), (0.35, (110, 190, 240)), (0.7, (200, 240, 255)), (1, (255, 255, 255))])
EP('E_DRAGON', [(0, (40, 10, 90)), (0.3, (90, 40, 210)), (0.6, (60, 160, 255)), (0.85, (150, 240, 255)), (1, (240, 255, 255))])
EP('E_PSY',    [(0, (90, 10, 70)), (0.35, (220, 60, 170)), (0.7, (255, 150, 220)), (1, (255, 240, 255))])
EP('E_GHOST',  [(0, (10, 0, 25)), (0.35, (60, 20, 110)), (0.7, (150, 80, 220)), (1, (230, 190, 255))])
EP('E_GRASS',  [(0, (10, 60, 10)), (0.35, (50, 170, 40)), (0.7, (160, 240, 90)), (1, (245, 255, 220))])
EP('E_SOLAR',  [(0, (130, 110, 0)), (0.35, (240, 220, 60)), (0.7, (255, 250, 170)), (1, (255, 255, 255))])
EP('E_POISON', [(0, (40, 0, 50)), (0.35, (140, 40, 170)), (0.7, (210, 120, 230)), (1, (250, 220, 255))])
EP('E_STEEL',  [(0, (50, 55, 70)), (0.4, (150, 160, 185)), (0.75, (215, 225, 240)), (1, (255, 255, 255))])
EP('E_WHITE',  [(0, (140, 140, 170)), (0.5, (220, 220, 240)), (1, (255, 255, 255))])
EP('E_DARK',   [(0, (5, 0, 10)), (0.4, (45, 30, 50)), (0.75, (110, 70, 110)), (1, (200, 150, 200))])
EP('E_ROCK',   [(0, (50, 35, 20)), (0.35, (125, 95, 60)), (0.7, (190, 160, 110)), (1, (240, 225, 190))])
EP('E_DUST',   [(0, (110, 90, 60)), (0.5, (190, 170, 130)), (1, (240, 230, 205))])
EP('E_SMOKE',  [(0, (50, 50, 55)), (0.5, (120, 120, 128)), (1, (200, 200, 205))])
EP('E_AURORA', [(0, (40, 80, 160)), (0.3, (90, 200, 220)), (0.55, (150, 255, 170)), (0.8, (255, 180, 230)), (1, (255, 255, 255))])
EP('E_GOLD',   [(0, (110, 60, 0)), (0.35, (240, 170, 30)), (0.7, (255, 235, 120)), (1, (255, 255, 240))])
EP('E_HEAL',   [(0, (20, 80, 40)), (0.4, (90, 220, 130)), (0.75, (190, 255, 200)), (1, (255, 255, 255))])
EP('E_PINK',   [(0, (110, 20, 60)), (0.4, (240, 100, 160)), (0.75, (255, 190, 220)), (1, (255, 250, 255))])
EP('E_BUG',    [(0, (40, 70, 10)), (0.4, (150, 200, 40)), (0.75, (220, 245, 130)), (1, (255, 255, 230))])
EP('E_FIGHT',  [(0, (90, 20, 10)), (0.4, (220, 90, 50)), (0.75, (255, 190, 130)), (1, (255, 250, 230))])
EP('E_SKY',    [(0, (60, 80, 140)), (0.4, (150, 180, 240)), (0.75, (215, 230, 255)), (1, (255, 255, 255))])
EP('E_MUD',    [(0, (40, 25, 10)), (0.4, (110, 75, 40)), (0.75, (170, 130, 80)), (1, (220, 190, 140))])
EP('E_SAND',   [(0, (120, 90, 40)), (0.5, (210, 180, 110)), (1, (250, 240, 200))])
