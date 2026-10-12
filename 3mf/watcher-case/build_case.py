# Sealed case for the SenseCAP Watcher W1-A. Run inside FreeCAD:
#
#   exec(open("3mf/watcher-case/build_case.py").read())
#
# It needs Seeed's shell model, which is too big to keep in the repo (48 MB):
#   https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher/blob/main/Hardware/SenseCAP_Watcher-3D-Shell_v1.stp
# Point STEP at it. Everything is built in that file's own frame: Y is depth
# (back of the device at -2.5, glass at +17), the round screen is centred on
# X = Z = 0, and +Z is the device's BOTTOM edge (where the data USB-C port is).
#
# Three parts:
#   CaseBody  rigid cup, the device drops in from the front
#   CaseLid   rigid front ring with the window; a clear film goes on its lip
#   UsbPlug   bung for the bottom USB-C port, print in TPU
import math
import os

import FreeCAD as App
import Part

STEP = os.path.expanduser("~/Downloads/SenseCAP_Watcher-3D-Shell_v1.stp")
OUT_DIR = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else None
V = App.Vector

# ---- parameters, mm ---------------------------------------------------------
CLR = 0.65        # cavity offset from the measured outline (which is 0.26 under the widest section)
WALL = 3.35       # body wall: wide enough to carry the O-ring groove
BACK_Y = -2.9     # cavity floor; the device's back is at -2.5
BACK_T = 2.4
SPLIT_Y = 15.0    # body / lid parting plane
LID_IN_Y = 17.8   # lid inner face; the bezel domes up to 17.53
LID_T = 2.0
KNOB_C = (16.34, -25.16)  # wheel axis (it runs along Y)
KNOB_R = 11.3
KNOB_CLR = 0.6
SCREW_OFF = 6.2   # screw centres, measured out from the device outline
EAR_R = 4.6
WIN_R = 20.5      # window; the lit part of the display is about 18.5 radius
LIP_R = 23.8      # the film lip drops into the bezel's glass opening (24.5 radius)
LIP_Y = 17.3      # underside of the lip; the glass is at 17.0, so a ~0.2 mm film rests on it
USB_Y = 8.3       # bottom USB-C port: centred X = 0 on the +Z flat
GROOVE_W, GROOVE_D = 1.6, 1.1  # for 1.5 mm O-ring cord
# M3 x 10 screws cut their own thread in the plastic: a 2.6 mm pilot, 8 mm deep.
# With 3.2 mm of lid under the head, 6.8 mm of the screw is in the body.
PILOT_D, PILOT_H = 2.6, 8.0

doc = App.ActiveDocument or App.newDocument("WatcherCase")
if not doc.getObject("WatcherRef"):
    ref = Part.Shape()
    ref.read(STEP)
    doc.addObject("Part::Feature", "WatcherRef").Shape = ref
    doc.WatcherRef.ViewObject.Visibility = False
subs = doc.WatcherRef.Shape.SubShapes
for name, i in (("RefFront", 1), ("RefRear", 2), ("RefScreen", 3)):
    if not doc.getObject(name):
        doc.addObject("Part::Feature", name).Shape = subs[i]
if not doc.getObject("RefWheel"):
    doc.addObject("Part::Feature", "RefWheel").Shape = subs[4].SubShapes[6]
for name in ("CaseBody", "CaseLid", "UsbPlug"):
    if doc.getObject(name):
        doc.removeObject(name)

# The device's outline, taken from the rear shell just above its back face.
P = max(doc.RefRear.Shape.slice(V(0, 1, 0), -1.5), key=lambda w: w.BoundBox.DiagonalLength)
R0 = 32.19  # radius of the round part of that outline


def cyl_y(r, y0, y1, x, z):
    return Part.makeCylinder(r, y1 - y0, V(x, y0, z), V(0, 1, 0))


def prism(d, y0, y1):
    """The outline grown by d, plus the wheel's bulge grown to match, from y0 to y1."""
    f = Part.Face(P.makeOffset2D(d))
    f.translate(V(0, y0 - f.BoundBox.YMin, 0))
    s = f.extrude(V(0, y1 - y0, 0))
    s = s.fuse(cyl_y(KNOB_R + KNOB_CLR + (d - CLR), y0, y1, *KNOB_C))
    return s.removeSplitter()


def stadium(lx, ly, z0, z1, grow=0.0):
    """A slot along X, centred on the USB port, from z0 to z1."""
    r, half = ly / 2 + grow, lx / 2 - ly / 2
    b = Part.makeBox(2 * half, 2 * r, z1 - z0, V(-half, USB_Y - r, z0))
    for sx in (-half, half):
        b = b.fuse(Part.makeCylinder(r, z1 - z0, V(sx, USB_Y, z0), V(0, 0, 1)))
    return b.removeSplitter()


# Screws: three on the round part (by angle), one on the straight side, one on
# the flat edge. None near the wheel or the USB plug.
ears = [((R0 + SCREW_OFF) * math.cos(math.radians(a)), (R0 + SCREW_OFF) * math.sin(math.radians(a)))
        for a in (135, 45, -8)]
ears += [(-R0 - SCREW_OFF, -22.0), (-12.0, -36.69 - SCREW_OFF)]

OUT = CLR + WALL
y_bot = BACK_Y - BACK_T
pad_z = 31.9 + OUT + 0.7  # outer face of the flat pad the plug's flange sits on

# ---- body -------------------------------------------------------------------
body = prism(OUT, y_bot, SPLIT_Y)
for x, z in ears:
    body = body.fuse(cyl_y(EAR_R, y_bot, SPLIT_Y, x, z))
body = body.fuse(Part.makeBox(22, SPLIT_Y - y_bot, 6, V(-11, y_bot, pad_z - 6)))
body = body.removeSplitter().cut(prism(CLR, BACK_Y, SPLIT_Y + 1))
g0 = CLR + 0.8
body = body.cut(prism(g0 + GROOVE_W, SPLIT_Y - GROOVE_D, SPLIT_Y + 1).cut(prism(g0, SPLIT_Y - 2, SPLIT_Y + 2)))
for x, z in ears:
    body = body.cut(cyl_y(PILOT_D / 2, SPLIT_Y - PILOT_H, SPLIT_Y + 1, x, z))
body = body.cut(stadium(13.0, 7.0, 28.0, pad_z + 1))

# The wheel's opening: the wall is taken away around the wheel over a 100
# degree sector, with the front and back edges chamfered for a fingertip. It
# stops short of the parting plane, so the O-ring still runs all the way round.
kx, kz = KNOB_C
rk = KNOB_R + KNOB_CLR
pts = [(0, -1.0), (rk, -1.0), (rk + 3.5, -3.0), (24, -3.0), (24, 12.5), (rk + 3.5, 12.5), (rk, 9.5), (0, 9.5), (0, -1.0)]
scoop = Part.Face(Part.makePolygon([V(kx + r, y, kz) for r, y in pts])).revolve(V(kx, 0, kz), V(0, 1, 0), 360)
a0, half = math.radians(-55.0), math.radians(50)
wedge = Part.Face(Part.makePolygon(
    [V(kx, -10, kz)] + [V(kx + 40 * math.cos(a), -10, kz + 40 * math.sin(a)) for a in (a0 - half, a0, a0 + half)] +
    [V(kx, -10, kz)])).extrude(V(0, 30, 0))
body = body.cut(scoop.common(wedge)).removeSplitter()

# ---- lid --------------------------------------------------------------------
y_top = LID_IN_Y + LID_T
lid = prism(OUT, SPLIT_Y, y_top)
for x, z in ears:
    lid = lid.fuse(cyl_y(EAR_R, SPLIT_Y, y_top, x, z))
lid = lid.removeSplitter().cut(prism(CLR, SPLIT_Y - 1, LID_IN_Y))
lid = lid.fuse(cyl_y(LIP_R, LIP_Y, LID_IN_Y + 0.1, 0, 0))            # the film lip
lid = lid.cut(cyl_y(WIN_R, LIP_Y - 1, y_top + 1, 0, 0))
lid = lid.cut(Part.makeCone(WIN_R, WIN_R + 1.6, y_top - LID_IN_Y, V(0, LID_IN_Y, 0), V(0, 1, 0)))
lid = lid.cut(cyl_y(6.3, LID_IN_Y - 1, 18.5, -24.0, -28.5))          # the camera ring stands proud
for x, z in ears:
    lid = lid.cut(cyl_y(1.7, SPLIT_Y - 1, y_top + 1, x, z))
    lid = lid.cut(cyl_y(3.1, y_top - 1.6, y_top + 1, x, z))
lid = lid.removeSplitter()

# ---- USB plug (TPU): flange, stem with two sealing ribs, pull tab ------------
wall_t = pad_z - (31.9 + CLR)
plug = stadium(18.0, 11.5, pad_z, pad_z + 1.6)
plug = plug.fuse(stadium(13.0, 7.0, pad_z - wall_t + 0.3, pad_z, grow=0.1))
for zz in (pad_z - 1.6, pad_z - 3.0):
    plug = plug.fuse(stadium(13.0, 7.0, zz - 0.4, zz + 0.4, grow=0.35))
plug = plug.fuse(Part.makeBox(6, 1.6, 5.0, V(-3, USB_Y - 0.8, pad_z + 1.6))).removeSplitter()

for name, shape, colour in (("CaseBody", body, (0.25, 0.55, 0.85)), ("CaseLid", lid, (0.95, 0.6, 0.2)),
                            ("UsbPlug", plug, (0.2, 0.8, 0.4))):
    o = doc.addObject("Part::Feature", name)
    o.Shape = shape
    if App.GuiUp:
        o.ViewObject.ShapeColor = colour
doc.recompute()

# ---- checks -----------------------------------------------------------------
device = [doc.getObject(n).Shape for n in ("RefFront", "RefRear", "RefScreen", "RefWheel")]
for name in ("CaseBody", "CaseLid", "UsbPlug"):
    s = doc.getObject(name).Shape
    clash = sum(s.common(so).Volume for d in device for so in d.Solids if so.BoundBox.intersect(s.BoundBox))
    b = s.BoundBox
    print("%-9s valid=%s solids=%d  %.1f x %.1f x %.1f mm  clash with device %.2f mm3"
          % (name, s.isValid(), len(s.Solids), b.XLength, b.YLength, b.ZLength, clash))
