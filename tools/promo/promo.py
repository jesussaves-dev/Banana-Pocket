"""Banana Pocket promo: 20 s, 1080p, 30 fps.  Run: blender -b -P tools/promo/promo.py -- [still N | anim]"""
import bpy, math, os, sys
from mathutils import Vector

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ART = os.path.join(ROOT, "docs", "readme")
OUT = os.environ.get("PROMO_OUT", "/tmp/promo")
FPS, FRAMES = 30, 600
argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc.render.fps = FPS
sc.frame_start, sc.frame_end = 1, FRAMES
sc.render.resolution_x, sc.render.resolution_y = 1920, 1080
sc.render.engine = "BLENDER_EEVEE"
sc.eevee.taa_render_samples = 48
sc.view_settings.view_transform = "Standard"

def rgb(h):
    return tuple(int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)) + (1,)

PINK, CYAN, PURPLE, GOLD, SKY = rgb("#ff00b6"), rgb("#00dbff"), rgb("#6d24db"), rgb("#ffdb00"), rgb("#24006d")

# world
w = bpy.data.worlds.new("w"); sc.world = w; w.use_nodes = True
bg = w.node_tree.nodes["Background"]
bg.inputs[0].default_value = SKY; bg.inputs[1].default_value = 0.6

def mat(name, color, rough=0.45, emit=0.0, metal=0.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = color
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metal
    if emit:
        b.inputs["Emission Color"].default_value = color
        b.inputs["Emission Strength"].default_value = emit
    return m

def put(obj, m):
    obj.data.materials.append(m)
    return obj

def bevel(obj, width=0.04, seg=4):
    mod = obj.modifiers.new("b", "BEVEL"); mod.width = width; mod.segments = seg
    bpy.ops.object.shade_smooth()

root = bpy.data.objects.new("root", None); sc.collection.objects.link(root)

def child(o):
    o.parent = root
    return o

# body
bpy.ops.mesh.primitive_cube_add(size=1); body = bpy.context.object
body.scale = (2.6, 0.22, 1.55); bevel(body, 0.06, 6)
put(body, mat("body", rgb("#15122a"), 0.35, metal=0.4)); child(body)
# side grips
for sx in (-1, 1):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=1, location=(sx * 1.2, 0, -0.1), segments=48, ring_count=24)
    g = bpy.context.object; g.scale = (0.5, 0.1, 0.85)
    put(g, mat("grip", rgb("#ffdb00"), 0.5)); bpy.ops.object.shade_smooth(); child(g)
# bezel and screens
bpy.ops.mesh.primitive_cube_add(size=1); bez = bpy.context.object
bez.scale = (1.36, 0.02, 1.2); bez.location = (0, -0.115, 0); bevel(bez, 0.02, 3)
put(bez, mat("bezel", rgb("#000000"), 0.1)); child(bez)

shots = ["home", "lib", "workshop", "phonebook", "ticket"]
SW = 1.2; screens = []
for n in shots:
    img = bpy.data.images.load(os.path.join(ART, n + ".png"))
    asp = img.size[0] / img.size[1]
    bpy.ops.mesh.primitive_plane_add(size=1, location=(0, -0.13, 0), rotation=(math.pi / 2, 0, 0))
    p = bpy.context.object
    p.scale = (SW, SW / asp, 1)
    m = bpy.data.materials.new(n); m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    t = nt.nodes.new("ShaderNodeTexImage"); t.image = img; t.interpolation = "Closest"
    e = nt.nodes.new("ShaderNodeEmission"); e.inputs[1].default_value = 1.0
    o = nt.nodes.new("ShaderNodeOutputMaterial")
    nt.links.new(t.outputs[0], e.inputs[0]); nt.links.new(e.outputs[0], o.inputs[0])
    p.data.materials.append(m); child(p); screens.append(p)

# controls
bpy.ops.mesh.primitive_cylinder_add(radius=0.1, depth=0.05, location=(-1.45, -0.13, 0.25), rotation=(math.pi / 2, 0, 0))
dp = bpy.context.object; put(dp, mat("dpad", rgb("#0a0816"), 0.3)); child(dp)
for dx, dz in ((0, 1), (0, -1), (1, 0), (-1, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1, location=(-1.45 + dx * 0.13, -0.13, 0.25 + dz * 0.13))
    c = bpy.context.object; c.scale = (0.12 + 0.1 * (dz != 0) * 0 , 0.05, 0.12); put(c, mat("dp", rgb("#0a0816"), 0.3)); child(c)
for (x, z, col) in ((1.45, 0.42, PINK), (1.68, 0.2, CYAN), (1.45, -0.02, GOLD), (1.22, 0.2, PURPLE)):
    bpy.ops.mesh.primitive_cylinder_add(radius=0.1, depth=0.05, location=(x, -0.13, z), rotation=(math.pi / 2, 0, 0))
    b = bpy.context.object; put(b, mat("btn", col, 0.3, emit=0.6)); bpy.ops.object.shade_smooth(); child(b)
for sx in (-1, 1):
    bpy.ops.mesh.primitive_cube_add(size=1, location=(sx * 1.25, 0.0, -0.62))
    s = bpy.context.object; s.scale = (0.24, 0.05, 0.05); put(s, mat("sm", rgb("#0a0816"), 0.3)); child(s)

# stage: floor + neon ring
bpy.ops.mesh.primitive_plane_add(size=40, location=(0, 0, -1.35))
fl = bpy.context.object; put(fl, mat("floor", rgb("#0b0620"), 0.18, metal=0.2))
bpy.ops.mesh.primitive_torus_add(major_segments=128, major_radius=2.9, minor_radius=0.035, location=(0, 0.6, 0), rotation=(math.pi / 2, 0, 0))
ring = bpy.context.object; put(ring, mat("ring", PINK, 0.2, emit=6.0))
bpy.ops.mesh.primitive_torus_add(major_segments=128, major_radius=3.4, minor_radius=0.035, location=(0, 1.0, 0), rotation=(math.pi / 2, 0, 0))
ring2 = bpy.context.object; put(ring2, mat("ring2", CYAN, 0.2, emit=6.0))

def light(kind, loc, energy, col, size=3):
    d = bpy.data.lights.new("l", kind); d.energy = energy; d.color = col[:3]
    if kind == "AREA": d.size = size
    o = bpy.data.objects.new("l", d); o.location = loc; sc.collection.objects.link(o)
    direction = Vector((0, 0, 0)) - Vector(loc)
    o.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    return o
light("AREA", (-4, -5, 3), 500, PINK, 4)
light("AREA", (4, -5, 2), 500, CYAN, 4)
light("AREA", (0, -3, 5), 200, (1, 1, 1, 1), 5)

# camera
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam")); sc.collection.objects.link(cam); sc.camera = cam
cam.data.lens = 50
tgt = bpy.data.objects.new("tgt", None); sc.collection.objects.link(tgt)
tr = cam.constraints.new("TRACK_TO"); tr.target = tgt; tr.track_axis = "TRACK_NEGATIVE_Z"; tr.up_axis = "UP_Y"

def const(obj):
    ad = obj.animation_data
    if not ad or not ad.action: return
    for layer in ad.action.layers:
        for strip in layer.strips:
            for cb in strip.channelbags:
                for fc in cb.fcurves:
                    for kp in fc.keyframe_points: kp.interpolation = "CONSTANT"

def key(obj, path, frame, val, idx=-1, interp="BEZIER"):
    setattr_target = obj
    if idx >= 0:
        v = list(getattr(obj, path)); v[idx] = val; setattr(obj, path, v)
        obj.keyframe_insert(path, index=idx, frame=frame)
    else:
        setattr(obj, path, val); obj.keyframe_insert(path, frame=frame)

# camera path: (frame, loc, root rotation z, root rotation x)
path = [
    (1,   (0.0, -9.0, 0.0), 0.6, 0.0),
    (90,  (0.0, -6.0, 0.1), 0.0, 0.0),
    (180, (-1.2, -4.6, 0.3), -0.25, 0.1),
    (300, (1.4, -4.8, 0.2), 0.3, -0.05),
    (420, (0.0, -3.4, 0.0), 0.0, 0.0),
    (520, (0.0, -6.2, 0.4), 0.0, 0.1),
    (600, (0.0, -8.5, 0.6), math.tau * 0.5 * 0, 0.1),
]
for f, loc, rz, rx in path:
    cam.location = loc; cam.keyframe_insert("location", frame=f)
    root.rotation_euler = (rx, 0, rz); root.keyframe_insert("rotation_euler", frame=f)
# floating bob
for f in range(1, FRAMES + 1, 30):
    tgt.location = (0, 0, 0.04 * math.sin(f / 30 * 1.7)); tgt.keyframe_insert("location", frame=f)
for ring_o, sp in ((ring, 1), (ring2, -1)):
    ring_o.rotation_euler = (math.pi / 2, 0, 0); ring_o.keyframe_insert("rotation_euler", frame=1)
    ring_o.rotation_euler = (math.pi / 2, sp * math.tau, 0); ring_o.keyframe_insert("rotation_euler", frame=FRAMES)

# screen switching with scale pop (constant)
cuts = [1, 120, 220, 320, 420]
for i, p in enumerate(screens):
    on, off = cuts[i], (cuts[i + 1] if i + 1 < len(cuts) else FRAMES + 1)
    base = p.scale.copy()
    for f, s in ((1, 0), (max(on - 1, 1), 0), (on, 1), (off - 1, 1), (off, 0)):
        p.scale = (base.x * s, base.y * s, 1) if s else (0.0001, 0.0001, 1)
        p.keyframe_insert("scale", frame=f)
    const(p)

# title cards (text objects, fixed to camera)
def card(txt, size, frm, to, z, col, x=0.0):
    c = bpy.data.curves.new("t", "FONT"); c.body = txt; c.size = size * 0.17; c.align_x = "CENTER"; c.extrude = 0.01
    o = bpy.data.objects.new("t", c); sc.collection.objects.link(o)
    o.parent = cam; o.location = (x * 0.4, z * 0.2, -2.4); o.rotation_euler = (0, 0, 0)
    put(o, mat("tm", col, 0.3, emit=3.0))
    for f, s in ((1, 0), (frm - 1, 0), (frm, 1), (to - 1, 1), (to, 0)):
        o.scale = (s or 0.0001,) * 3; o.keyframe_insert("scale", frame=f)
    const(o)

card("BANANA POCKET", 0.9, 10, 100, 1.45, GOLD)
card("a street for your games", 0.4, 40, 100, 0.8, (1, 1, 1, 1))
card("Every system is a shop", 0.38, 125, 215, 1.9, CYAN)
card("Your games are on its shelves", 0.38, 225, 315, 1.9, PINK)
card("Settings are drawers", 0.38, 325, 415, 1.9, GOLD)
card("Built for the Retroid Pocket Classic", 0.34, 425, 515, 1.9, CYAN)
card("BANANA POCKET", 0.9, 520, 600, 1.55, GOLD)
card("github.com/jesussaves-dev/Banana-Pocket", 0.28, 540, 600, 0.95, (1, 1, 1, 1))

# compositor glow

if argv and argv[0] == "still":
    sc.frame_set(int(argv[1]))
    sc.render.filepath = os.path.join(OUT, "still_%04d.png" % int(argv[1]))
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.render.render(write_still=True)
else:
    os.makedirs(OUT, exist_ok=True)
    sc.render.image_settings.media_type = "VIDEO" if hasattr(sc.render.image_settings, "media_type") else sc.render.image_settings.media_type
    sc.render.image_settings.file_format = "FFMPEG"
    sc.render.ffmpeg.format = "MPEG4"; sc.render.ffmpeg.codec = "H264"
    sc.render.ffmpeg.constant_rate_factor = "HIGH"
    sc.render.filepath = os.path.join(OUT, "banana-pocket-promo.mp4")
    bpy.ops.render.render(animation=True)
