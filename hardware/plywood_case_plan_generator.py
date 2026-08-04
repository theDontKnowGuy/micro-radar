#!/usr/bin/env python3
"""LightBurn plan for a plywood radar-case, v5.

Terminology (matches user's labeled sketch):
    panel  = slanted upper-front face (with the display cutout)
    desk   = short horizontal ledge between the panel and the kick
    kik    = angled kick face at the bottom-front
    top    = short horizontal top (behind the panel)
    back   = vertical back wall
    floor  = horizontal bottom

Side profile is a HEXAGON. 8 pieces total (2 Sides + 6 mating).
The Side is rotated 180° for the SVG output so the shape reads in the
same orientation as the reference sketch (back on the right, floor at
the bottom).
"""
import math

# ---- Material & geometry parameters --------------------------------------
T          = 2.0        # plywood thickness (mm)
FW_TARGET  = 5.0        # target finger width (mm)
BURN       = 0.10       # LightBurn kerf-compensation note (mm)

# External dimensions (mm), coords: X = forward (from back), Y = up (from floor)
BACK_HEIGHT   = 70.0    # vertical back wall
TOP_DEPTH     = 32.0    # top horizontal length (from back)
MAIN_DEPTH    = 58.0    # back to base of panel (desk-back position)
DESK_DEPTH    = 16.0    # horizontal length of the desk ledge
KICK_H        = 18.0    # height of the desk above the floor
KICK_BACK     = 6.0     # how far the kik face recedes BACKWARD going down
                        # (desk-front overhangs the floor-front by this much)
WIDTH_EXT     = 75.0    # box external width
WIDTH_INT     = WIDTH_EXT - 2 * T   # 71 mm between the Sides

# Derived geometry
DESK_FRONT    = MAIN_DEPTH + DESK_DEPTH          # most-forward point (76 mm)
FLOOR_LEN     = DESK_FRONT - KICK_BACK           # floor stops short: 70 mm
PANEL_LEN     = math.hypot(MAIN_DEPTH - TOP_DEPTH, BACK_HEIGHT - KICK_H)
KICK_LEN      = math.hypot(KICK_BACK, KICK_H)

# GC9A01 1.28" 240x240 round display module cutout: the visible glass is a
# circle, but the module has a "chin" below it (folded ribbon area under the
# glass) that must also pass through the panel — so the hole is a circle
# merged with a trapezoid hanging off its bottom.
LENS_DIA      = 33.0    # glass circle diameter (mm)
CHIN_TOP_W    = 20.0    # trapezoid width where it meets the circle (mm)
CHIN_BOT_W    = 15.0    # trapezoid width at its bottom (mm)
CHIN_H        = 6.0     # trapezoid height below the circle chord (mm)
MNT_HOLE_DIA  = 4.0     # display mounting holes (mm), 2 at diagonal corners
MNT_OFFSET_X  = 14.37
MNT_OFFSET_Y  = 14.37

# SVG page
PAGE_MARGIN = 15.0
GAP         = 20.0

# ---- Finger-joint helpers ------------------------------------------------
def odd_segments(length, target=FW_TARGET):
    n = max(3, round(length / target))
    if n % 2 == 0:
        n = n - 1 if n > 3 else 3
    return n, length / n

def castellated_edge(x0, y0, x1, y1, start_with_tab, tab_out):
    L = math.hypot(x1 - x0, y1 - y0)
    n, seg = odd_segments(L)
    dx, dy = (x1 - x0) / L, (y1 - y0) / L
    nx, ny = dy, -dx  # outward normal (CCW polygon)
    pts = [(x0, y0)]
    for i in range(n):
        is_tab = (i % 2 == 0) if start_with_tab else (i % 2 == 1)
        s0 = i * seg; s1 = (i + 1) * seg
        p0 = (x0 + dx * s0, y0 + dy * s0)
        p1 = (x0 + dx * s1, y0 + dy * s1)
        if is_tab:
            pts.append((p0[0] + nx * tab_out, p0[1] + ny * tab_out))
            pts.append((p1[0] + nx * tab_out, p1[1] + ny * tab_out))
            pts.append(p1)
        else:
            pts.append(p1)
    return pts

def polyline_to_path(pts, closed=True):
    d = f"M {pts[0][0]:.3f},{pts[0][1]:.3f} " + \
        " ".join(f"L {x:.3f},{y:.3f}" for x, y in pts[1:])
    if closed:
        d += " Z"
    return d

# ---- Side profile (HEXAGON: floor, kik, desk, panel, top, back) ----------
# CCW order in "world" coords (X=forward, Y=up, origin=back-bottom).
# Every edge carries finger joints. Edges touching the REFLEX desk-back
# corner (desk and panel-bottom) use an INSET: the castellation stops a few
# mm short of that corner, because a corner tab on a reflex corner overlaps
# the neighbouring edge and self-intersects.
# Edge (i -> i+1) meanings:
#   0->1 floor, 1->2 kik, 2->3 desk, 3->4 panel, 4->5 top, 5->0 back
INSET = 3.0
# (inset_at_edge_start, inset_at_edge_end) per edge, in CCW edge direction
EDGE_INSETS = [(0, 0),          # floor
               (0, INSET),      # kik (inset at desk-front so the desk's
                                #      corner stays a clean straight point —
                                #      a corner tooth on the slanted kik
                                #      reads as a diagonal off the desk)
               (INSET, INSET),  # desk (reflex at its end; symmetric for a
                                #       clean look and a symmetric piece)
               (INSET, 0),      # panel (reflex at its start = desk-back)
               (0, 0),          # top
               (0, 0)]          # back
def side_world_vertices(mirrored=False):
    P = [
        (0.0, 0.0),                                # 0 back-bottom
        (FLOOR_LEN, 0.0),                          # 1 floor-front (kik bottom)
        (DESK_FRONT, KICK_H),                      # 2 kik top = desk-front
                                                   #   overhangs floor-front
        (MAIN_DEPTH, KICK_H),                      # 3 desk-back = panel-bottom
        (TOP_DEPTH, BACK_HEIGHT),                  # 4 top of panel
        (0.0, BACK_HEIGHT),                        # 5 back-top
    ]
    if mirrored:
        maxx = DESK_FRONT
        P = [(maxx - x, y) for (x, y) in P]
        P.reverse()
    return P

def edge_insets(mirrored):
    ins = EDGE_INSETS[:]
    if mirrored:
        # Mirroring reverses every edge: new edge i = reverse of old edge
        # (N-2-i) for i < N-1, and the last edge reverses in place. A
        # reversed edge swaps its start/end insets.
        N = len(ins)
        ins = [tuple(reversed(ins[N - 2 - i])) for i in range(N - 1)] + \
              [tuple(reversed(ins[N - 1]))]
    return ins

def inset_castellated_edge(x0, y0, x1, y1, ins_s, ins_e, tab_out):
    """Straight for ins_s mm at the start and ins_e mm at the end, with a
    castellated middle that starts and ends with a tab (so the mating
    piece's tabs sit strictly inside the middle span)."""
    L = math.hypot(x1 - x0, y1 - y0)
    dx, dy = (x1 - x0) / L, (y1 - y0) / L
    a = (x0 + dx * ins_s, y0 + dy * ins_s)
    b = (x1 - dx * ins_e, y1 - dy * ins_e)
    pts = []
    if ins_s > 0:
        pts.append((x0, y0))
    pts.extend(castellated_edge(a[0], a[1], b[0], b[1],
                                start_with_tab=True, tab_out=tab_out))
    if ins_e > 0:
        pts.append((x1, y1))
    return pts

def build_side_path(mirrored=False):
    V = side_world_vertices(mirrored)
    ins = edge_insets(mirrored)
    N = len(V)
    pts = []
    for i in range(N):
        a = V[i]; b = V[(i + 1) % N]
        seg = inset_castellated_edge(a[0], a[1], b[0], b[1],
                                     ins[i][0], ins[i][1], T)
        pts.extend(seg[1:] if pts else seg)
    # SVG Y grows downward; flip vertically so the profile reads on screen
    # the way it stands in real life (floor at the bottom). A single-axis
    # flip reverses winding on screen only — the cut geometry is already
    # baked into the coordinates and the laser doesn't care about winding.
    ys = [p[1] for p in pts]
    my = (max(ys) + min(ys)) / 2.0
    pts = [(x, 2 * my - y) for (x, y) in pts]
    return pts

def piece_tab_spans(L, ins_s, ins_e):
    """Spans (distance-from-edge-start) where the MATING PIECE has tabs:
    the odd segments of the side edge's castellated middle."""
    middle = L - ins_s - ins_e
    n, seg = odd_segments(middle)
    return [(ins_s + i * seg, ins_s + (i + 1) * seg)
            for i in range(n) if i % 2 == 1]

def build_mating_piece(L, ins_s=0.0, ins_e=0.0, flip=False, width_int=WIDTH_INT):
    """Rectangle (width_int x L) whose left/right end tabs complement the
    Side edge that has insets (ins_s, ins_e). flip=True maps the edge start
    to local y=L instead of y=0 (use when the piece is drawn 'upside down'
    relative to the side edge's CCW direction)."""
    spans = piece_tab_spans(L, ins_s, ins_e)
    if flip:
        spans = sorted((L - e, L - s) for (s, e) in spans)
    pts = [(0.0, 0.0), (width_int, 0.0)]
    for y0, y1 in spans:                            # right end, walking +y
        pts.extend([(width_int, y0), (width_int + T, y0),
                    (width_int + T, y1), (width_int, y1)])
    pts.extend([(width_int, L), (0.0, L)])
    for y0, y1 in reversed(spans):                  # left end, walking -y
        pts.extend([(0.0, y1), (-T, y1), (-T, y0), (0.0, y0)])
    return pts

def display_cutout_path(cx, cy, r, chin_top_w, chin_bot_w, chin_h):
    """One closed outline: circle merged with a trapezoid chin below it.
    'Below' = toward +y (which is toward the desk once the panel is placed
    with its lower edge at y=0 flipped... see assembly note in summary).
    Returned as a point list ready for polyline_to_path."""
    half_t = chin_top_w / 2.0
    drop   = math.sqrt(r * r - half_t * half_t)  # chord depth below center
    # trapezoid corners (y grows downward on the page)
    A = (cx - half_t, cy + drop)                 # left end of chord
    B = (cx + half_t, cy + drop)                 # right end of chord
    C = (cx + chin_bot_w / 2.0, cy + drop + chin_h)
    D = (cx - chin_bot_w / 2.0, cy + drop + chin_h)
    # arc from A to B the long way around (over the top of the circle)
    a_start = math.atan2(drop, -half_t)          # angle of A
    a_end   = math.atan2(drop,  half_t) + 2 * math.pi  # angle of B, unwrapped
    pts = []
    N = 96
    for i in range(N + 1):
        th = a_start + (a_end - a_start) * i / N
        pts.append((cx + r * math.cos(th), cy + r * math.sin(th)))
    pts.extend([C, D])
    return pts

# ---- Piece list ----------------------------------------------------------
pieces = []
pieces.append(("Left Side",   build_side_path(False), []))
pieces.append(("Right Side",  build_side_path(True),  []))
pieces.append(("Floor",       build_mating_piece(FLOOR_LEN), []))
pieces.append(("Back Wall",   build_mating_piece(BACK_HEIGHT), []))
pieces.append(("Top",         build_mating_piece(TOP_DEPTH), []))
# Kik local y=0 is the floor end; the desk-front end (y=KICK_LEN) matches
# the side edge's inset, so its tab pattern shifts toward the floor end.
pieces.append(("Kik",         build_mating_piece(KICK_LEN, 0.0, INSET), []))
pieces.append(("Desk",        build_mating_piece(DESK_DEPTH, INSET, INSET), []))

# Panel with display cutout (circle + chin, one hole) + mount holes.
# Panel local y=0 edge meets the TOP; y=PANEL_LEN meets the DESK, so the
# chin (toward +y on the page) points at the desk — matching how the
# module mounts with its connector chin at the bottom. The side's panel
# edge runs desk->top with an inset at the desk end, hence flip=True.
sp_pts = build_mating_piece(PANEL_LEN, INSET, 0.0, flip=True)
cx = WIDTH_INT / 2.0
cy = PANEL_LEN / 2.0 - CHIN_H / 2.0   # shift up so circle+chin sit centered
sp_extras = [
    ("path", display_cutout_path(cx, cy, LENS_DIA / 2.0,
                                 CHIN_TOP_W, CHIN_BOT_W, CHIN_H)),
    ("circle", cx - MNT_OFFSET_X, cy + MNT_OFFSET_Y, MNT_HOLE_DIA / 2.0),
    ("circle", cx + MNT_OFFSET_X, cy - MNT_OFFSET_Y, MNT_HOLE_DIA / 2.0),
]
pieces.append(("Panel", sp_pts, sp_extras))

# ---- Layout & SVG emission -----------------------------------------------
def bbox(pts):
    xs=[p[0] for p in pts]; ys=[p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)

MAX_ROW_W = 420.0   # wrap pieces into rows (fits a 600x400-class bed)

placed = []
cursor_x = PAGE_MARGIN
row_top  = PAGE_MARGIN
row_max_h = 0
max_row_right = 0
for name, pts, extras in pieces:
    mnx, mny, mxx, mxy = bbox(pts)
    w = mxx - mnx; h = mxy - mny
    if cursor_x > PAGE_MARGIN and cursor_x + w > MAX_ROW_W:
        row_top += row_max_h + GAP
        cursor_x = PAGE_MARGIN
        row_max_h = 0
    dx = cursor_x - mnx
    dy = row_top - mny
    tpts = [(x + dx, y + dy) for (x, y) in pts]
    textras = []
    for e in extras:
        if e[0] == "circle":
            _, cx_, cy_, r = e
            textras.append(("circle", cx_ + dx, cy_ + dy, r))
        else:  # ("path", pts)
            textras.append(("path", [(x + dx, y + dy) for (x, y) in e[1]]))
    placed.append((name, tpts, textras, cursor_x, row_top, w, h))
    cursor_x += w + GAP
    row_max_h = max(row_max_h, h)
    max_row_right = max(max_row_right, cursor_x - GAP)

page_w = max_row_right + PAGE_MARGIN
page_h = row_top + row_max_h + PAGE_MARGIN + 25

CUT_STROKE = 'stroke="#000000" stroke-width="0.1" fill="none"'
TEXT_STYLE = 'fill="#c00000" font-family="Helvetica, Arial, sans-serif" font-size="4"'

out = [f'<svg xmlns="http://www.w3.org/2000/svg" '
       f'width="{page_w:.2f}mm" height="{page_h:.2f}mm" '
       f'viewBox="0 0 {page_w:.2f} {page_h:.2f}" '
       f'stroke-linejoin="miter" stroke-linecap="butt">']

for name, pts, extras, ox, oy, w, h in placed:
    out.append(f'<path d="{polyline_to_path(pts)}" {CUT_STROKE}/>')
    for e in extras:
        if e[0] == "circle":
            _, cx_, cy_, r = e
            out.append(f'<circle cx="{cx_:.3f}" cy="{cy_:.3f}" r="{r:.3f}" {CUT_STROKE}/>')
        else:
            out.append(f'<path d="{polyline_to_path(e[1])}" {CUT_STROKE}/>')
    lx = ox + w / 2.0; ly = oy + h / 2.0
    out.append(f'<text x="{lx:.2f}" y="{ly:.2f}" text-anchor="middle" '
               f'dominant-baseline="middle" {TEXT_STYLE}>{name}</text>')

bar_x = PAGE_MARGIN
bar_y = page_h - PAGE_MARGIN - 8
out.append(f'<rect x="{bar_x:.2f}" y="{bar_y:.2f}" width="100" height="8" {CUT_STROKE}/>')
out.append(f'<text x="{bar_x + 50:.2f}" y="{bar_y + 5.5:.2f}" '
           f'text-anchor="middle" {TEXT_STYLE}>100.0mm, burn:{BURN:.2f}mm  '
           f'(2mm ply, W{WIDTH_EXT:.0f} D{FLOOR_LEN:.0f} H{BACK_HEIGHT:.0f} ext)</text>')
out.append('</svg>')

svg_path = "/Users/avner/Documents/Projects/ESP32Projects/micro-radar/hardware/plywood_case_plan.svg"
with open(svg_path, "w") as f:
    f.write("\n".join(out))

print("Wrote:", svg_path)
print(f"Page: {page_w:.1f} x {page_h:.1f} mm")
print(f"Floor {FLOOR_LEN:.0f}  Kik {KICK_LEN:.2f}  Desk {DESK_DEPTH:.0f}  Panel {PANEL_LEN:.2f}")
print(f"External: W{WIDTH_EXT:.0f} x D{FLOOR_LEN:.0f} x H{BACK_HEIGHT:.0f} mm")
print(f"Internal (usable): W{WIDTH_INT:.0f} x D{MAIN_DEPTH-T:.0f} x H{BACK_HEIGHT-2*T:.0f} mm")
for name, pts, extras, ox, oy, w, h in placed:
    print(f"  {name:14s} bbox {w:6.2f} x {h:6.2f} mm")
