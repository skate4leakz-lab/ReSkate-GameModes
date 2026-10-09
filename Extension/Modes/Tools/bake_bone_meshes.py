"""Bakes the Bone Cam's 3D skeleton (Extension/Modes/bone_meshes.h) from BodyParts3D.

    python Extension/Modes/Tools/bake_bone_meshes.py <partof_BP3D_4.0_obj_99 folder> Extension/Modes/bone_meshes.h

BodyParts3D, (c) The Database Center for Life Science licensed under CC Attribution 4.0 International
(https://dbarchive.biosciencedbc.jp/en/bodyparts3d/lic.html): the "partof" OBJ set, 99% reduced.

The model stands in millimetres with +X its left, -Y its front and +Z up. Each bone is simplified
(vertex clustering) to a triangle budget and assigned to a frame that follows the skater:

  pelvis, chest, head        rigid frames on the hips, the chest and the head;
  vertebrae                  one frame each, placed along the skater's spine where the vertebra
                             sits along the model's;
  upper arm, forearm, hand,  limb frames from one of the skater's joints to the next: the bone
  thigh, shin, foot          stretches along the limb to fit and keeps its thickness.

A frame is three axes, each measured, never taken from a cross product (so it maps the anatomy the
right way round whatever the world's handedness): a primary axis, then a secondary and a tertiary
made square to it. Vertices are stored in their frame's coordinates: rigid frames in metres, limb
frames with the first coordinate as the fraction of the way from the limb's first joint to its
second. Every vertex also has its position along its own bone (0..255), for cracks and breaks.
"""
import math
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------- the bones
# (name, frame, region, budget, flags). Regions follow modes::BoneSprite: torso 0, skull 1,
# humerus_l 2, humerus_r 3, forearm_l 4, forearm_r 5, hand_l 6, hand_r 7, femur_l 8, femur_r 9,
# shin_l 10, shin_r 11, foot_l 12, foot_r 13. Flags: 1 snaps in two when its region breaks,
# 2 cartilage (drawn fainter), 4 cracks show as lines across it.
TORSO, SKULL = 0, 1
HUMERUS = {"left": 2, "right": 3}
FOREARM = {"left": 4, "right": 5}
HAND = {"left": 6, "right": 7}
FEMUR = {"left": 8, "right": 9}
SHIN = {"left": 10, "right": 11}
FOOT = {"left": 12, "right": 13}
SNAPS, CARTILAGE, CRACKS = 1, 2, 4

FRAMES = ["pelvis", "chest", "head",
          "upper arm right", "upper arm left", "forearm right", "forearm left", "hand right", "hand left",
          "thigh right", "thigh left", "shin right", "shin left", "foot right", "foot left"]
VERTEBRA_FRAME = len(FRAMES)  # vertebrae from here on, one frame each
ORDINALS = ["first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth", "tenth",
            "eleventh", "twelfth"]


def side_frame(name, side):
    return FRAMES.index(f"{name} {side}")


def bone_list():
    bones = []
    skull = ["frontal bone", "occipital bone", "sphenoid bone", "ethmoid", "vomer"]
    for side in ("left", "right"):
        skull += [f"{side} parietal bone", f"{side} temporal bone", f"{side} zygomatic bone", f"{side} maxilla",
                  f"{side} nasal bone", f"{side} lacrimal bone", f"{side} palatine bone", f"{side} inferior nasal concha"]
    for name in skull:
        budget = 700 if any(k in name for k in ("frontal", "parietal", "occipital", "temporal")) else 250
        bones.append((name, "head", SKULL, budget, CRACKS))
    bones.append(("mandible", "head", SKULL, 500, 0))
    bones.append(("hyoid bone", "head", SKULL, 60, 0))
    for name in ("manubrium", "body of sternum", "xiphoid process"):
        bones.append((name, "chest", TORSO, 160, 0))
    for side in ("left", "right"):
        for i, ordinal in enumerate(ORDINALS):
            bones.append((f"{side} {ordinal} rib", "chest", TORSO, 220, SNAPS if 3 <= i <= 5 else 0))
        for ordinal in ORDINALS[:7]:
            bones.append((f"{side} {ordinal} costal cartilage", "chest", TORSO, 60, CARTILAGE))
        bones.append((f"{side} clavicle", "chest", TORSO, 220, SNAPS))
        bones.append((f"{side} scapula", "chest", TORSO, 500, 0))
        bones.append((f"{side} hip bone", "pelvis", TORSO, 1100, 0))
        bones.append((f"{side} humerus", side_frame("upper arm", side), HUMERUS[side], 700, SNAPS))
        bones.append((f"{side} radius", side_frame("forearm", side), FOREARM[side], 380, SNAPS))
        bones.append((f"{side} ulna", side_frame("forearm", side), FOREARM[side], 380, SNAPS))
        bones.append((f"{side} femur", side_frame("thigh", side), FEMUR[side], 900, SNAPS))
        bones.append((f"{side} patella", side_frame("thigh", side), FEMUR[side], 90, 0))
        bones.append((f"{side} tibia", side_frame("shin", side), SHIN[side], 650, SNAPS))
        bones.append((f"{side} fibula", side_frame("shin", side), SHIN[side], 260, SNAPS))
        hand = [f"{side} {b}" for b in ("scaphoid", "lunate", "triquetral", "pisiform", "trapezium", "trapezoid", "capitate", "hamate")]
        hand += [f"{side} {o} metacarpal bone" for o in ORDINALS[:5]]
        for finger in ("thumb", "index finger", "middle finger", "ring finger", "little finger"):
            for joint in ("proximal", "middle", "distal"):
                hand.append(f"{joint} phalanx of {side} {finger}")
        for name in hand:
            bones.append((name, side_frame("hand", side), HAND[side], 45, 0))
        foot = [f"{side} talus", f"{side} calcaneus", f"navicular bone of {side} foot", f"{side} cuboid bone",
                f"{side} medial cuneiform bone", f"{side} intermediate cuneiform bone", f"{side} lateral cuneiform bone"]
        foot += [f"{side} {o} metatarsal bone" for o in ORDINALS[:5]]
        for toe in ("big toe", "second toe", "third toe", "fourth toe", "little toe"):
            for joint in ("proximal", "middle", "distal"):
                foot.append(f"{joint} phalanx of {side} {toe}")
        for name in foot:
            budget = 220 if ("talus" in name or "calcaneus" in name) else 50
            bones.append((name, side_frame("foot", side), FOOT[side], budget, 0))
    bones.append(("sacrum", "pelvis", TORSO, 500, 0))
    return bones


VERTEBRAE = (["atlas", "axis"] + [f"{o} cervical vertebra" for o in ORDINALS[2:7]] +
             [f"{o} thoracic vertebra" for o in ORDINALS] + [f"{o} lumbar vertebra" for o in ORDINALS[:5]])

# ---------------------------------------------------------------- vectors
def add(a, b): return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
def sub(a, b): return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
def mul(a, k): return [a[0] * k, a[1] * k, a[2] * k]
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def length(a): return math.sqrt(dot(a, a))
def unit(a):
    l = length(a)
    return mul(a, 1.0 / l) if l > 1e-9 else [0.0, 0.0, 1.0]
def mean(points):
    n = max(1, len(points))
    return [sum(p[i] for p in points) / n for i in range(3)]


def frame(primary, secondary, tertiary):
    """Three measured axes made square: the primary kept, the others projected off it."""
    p = unit(primary)
    s = unit(sub(secondary, mul(p, dot(secondary, p))))
    t = sub(tertiary, add(mul(p, dot(tertiary, p)), mul(s, dot(tertiary, s))))
    return p, s, unit(t)


# ---------------------------------------------------------------- meshes
def load_index(folder):
    index = {}
    for path in folder.glob("*.obj"):
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if not line.startswith("#"):
                    break
                m = re.match(r"# English name : (.*)", line)
                if m:
                    index[m.group(1).strip().lower()] = path
                    break
    return index


def load_obj(path):
    vertices, faces = [], []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("v "):
                vertices.append([float(v) for v in line.split()[1:4]])
            elif line.startswith("f "):
                ids = [int(token.split("/")[0]) - 1 for token in line.split()[1:]]
                for k in range(1, len(ids) - 1):
                    faces.append((ids[0], ids[k], ids[k + 1]))
    return vertices, faces


def cluster(vertices, faces, cell):
    """Vertex clustering: every vertex in a grid cell becomes their average."""
    cells, members = {}, []
    for v in vertices:
        key = (math.floor(v[0] / cell), math.floor(v[1] / cell), math.floor(v[2] / cell))
        if key not in cells:
            cells[key] = len(members)
            members.append([])
        members[cells[key]].append(v)
    remap = [cells[(math.floor(v[0] / cell), math.floor(v[1] / cell), math.floor(v[2] / cell))] for v in vertices]
    out_vertices = [mean(m) for m in members]
    seen, out_faces = set(), []
    for a, b, c in faces:
        a, b, c = remap[a], remap[b], remap[c]
        if a == b or b == c or a == c:
            continue
        key = tuple(sorted((a, b, c)))
        if key in seen:
            continue
        seen.add(key)
        out_faces.append((a, b, c))
    # Drop vertices no face uses.
    used = sorted({i for f in out_faces for i in f})
    renumber = {old: new for new, old in enumerate(used)}
    return [out_vertices[i] for i in used], [(renumber[a], renumber[b], renumber[c]) for a, b, c in out_faces]


def simplify(vertices, faces, budget):
    if len(faces) <= budget:
        return vertices, faces
    lo, hi = 0.05, 60.0
    best = cluster(vertices, faces, hi)
    for _ in range(18):
        mid = math.sqrt(lo * hi)
        v, f = cluster(vertices, faces, mid)
        if len(f) > budget:
            lo = mid
        else:
            hi, best = mid, (v, f)
    return best


# ---------------------------------------------------------------- landmarks
def end_region(points, axis, top, share=0.06):
    """The points within `share` of a bone's length from one end along `axis`."""
    values = [dot(p, axis) for p in points]
    lo, hi = min(values), max(values)
    cut = (hi - (hi - lo) * share) if top else (lo + (hi - lo) * share)
    return [p for p, v in zip(points, values) if (v >= cut if top else v <= cut)]


def landmarks(raw, side):
    """The model's joints on one side, matched to the skater's: hip, knee, ankle, toe, shoulder,
    elbow, wrist and the middle finger's knuckle."""
    up = [0.0, 0.0, 1.0]
    front = [0.0, -1.0, 0.0]
    femur = raw[f"{side} femur"][0]
    # The femoral head: the top of the femur, on its inner side (the greater trochanter is outer).
    top = end_region(femur, up, True, 0.08)
    inner = sorted(top, key=lambda p: abs(p[0]))[: max(1, len(top) // 3)]
    hip = mean(inner)
    tibia = raw[f"{side} tibia"][0]
    knee = mean([mean(end_region(femur, up, False, 0.04)), mean(end_region(tibia, up, True, 0.04))])
    ankle = mean(raw[f"{side} talus"][0])
    toe = mean([mean(end_region(raw[f"{side} {o} metatarsal bone"][0], front, True, 0.12)) for o in ("second", "third")])
    humerus = raw[f"{side} humerus"][0]
    shoulder = mean(end_region(humerus, up, True, 0.07))
    elbow = mean(end_region(humerus, up, False, 0.05))
    wrist = mean([mean(end_region(raw[f"{side} radius"][0], up, False, 0.05)),
                  mean(end_region(raw[f"{side} ulna"][0], up, False, 0.05))])
    knuckle = mean(end_region(raw[f"{side} third metacarpal bone"][0], up, False, 0.12))
    return {"hip": hip, "knee": knee, "ankle": ankle, "toe": toe, "shoulder": shoulder, "elbow": elbow,
            "wrist": wrist, "fingers": knuckle}


def polyline_fraction(line, point):
    """How far along a polyline (0..1) the point nearest `point` is."""
    total = sum(length(sub(line[i + 1], line[i])) for i in range(len(line) - 1))
    best, best_d, run = 0.0, float("inf"), 0.0
    for i in range(len(line) - 1):
        a, b = line[i], line[i + 1]
        seg = sub(b, a)
        l2 = max(1e-9, dot(seg, seg))
        t = max(0.0, min(1.0, dot(sub(point, a), seg) / l2))
        q = add(a, mul(seg, t))
        d = length(sub(point, q))
        if d < best_d:
            best_d, best = d, (run + t * math.sqrt(l2)) / total
        run += math.sqrt(l2)
    return best


def along(line, t):
    total = sum(length(sub(line[i + 1], line[i])) for i in range(len(line) - 1))
    want = max(0.0, min(1.0, t)) * total
    for i in range(len(line) - 1):
        piece = length(sub(line[i + 1], line[i]))
        if want <= piece or i + 2 == len(line):
            return add(line[i], mul(sub(line[i + 1], line[i]), min(1.0, want / piece) if piece > 0 else 0.0))
        want -= piece
    return line[-1]


def tangent(line, t, reach):
    return unit(sub(along(line, min(1.0, t + reach)), along(line, max(0.0, t - reach))))


# ---------------------------------------------------------------- bake
def main():
    folder, out_path = Path(sys.argv[1]), Path(sys.argv[2])
    index = load_index(folder)
    bones = bone_list()
    needed = {b[0] for b in bones} | set(VERTEBRAE) | {f"{s} {o} metatarsal bone" for s in ("left", "right") for o in ("second", "third")}
    missing = sorted(n for n in needed if n not in index)
    if missing:
        print("missing:", missing)
    raw = {n: load_obj(index[n]) for n in needed if n in index}

    marks = {side: landmarks(raw, side) for side in ("right", "left")}
    # The model's spine, from between the hip joints (a little up, where the skater's hips joint is)
    # through every vertebra to the skull's base: the line vertebrae are placed along.
    hips = add(mean([marks["right"]["hip"], marks["left"]["hip"]]), [0.0, 0.0, 60.0])
    head = add(mean(raw["atlas"][0]), [0.0, 0.0, 15.0])
    spine = [hips] + [mean(raw[n][0]) for n in reversed(VERTEBRAE) if n in raw] + [head]
    chest_t = polyline_fraction(spine, mean(raw["sixth thoracic vertebra"][0]))
    X, Y, Z = [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]  # left, back, up

    frames = []  # (origin, axes, limb length or 0, spine fraction or -1)
    hip_mid = mean([marks["right"]["hip"], marks["left"]["hip"]])
    frames.append((hip_mid, frame(tangent(spine, 0.08, 0.08), sub(marks["left"]["hip"], marks["right"]["hip"]), Y), 0.0, -1.0))
    chest_origin = along(spine, chest_t)
    frames.append((chest_origin, frame(tangent(spine, chest_t, 0.12), sub(marks["left"]["shoulder"], marks["right"]["shoulder"]), Y), 0.0, chest_t))
    frames.append((head, frame(Z, Y, X), 0.0, -1.0))
    limb_joints = [("shoulder", "elbow"), ("elbow", "wrist"), ("wrist", "fingers"), ("hip", "knee"), ("knee", "ankle"), ("ankle", "toe")]
    for a_name, b_name in limb_joints:
        for side in ("right", "left"):
            a, b = marks[side][a_name], marks[side][b_name]
            if a_name == "ankle":  # the foot: square to the shin, across the body
                axes = frame(sub(b, a), sub(marks[side]["knee"], a), X)
            else:
                axes = frame(sub(b, a), Y, X)
            frames.append((a, axes, length(sub(b, a)), -1.0))
    vertebra_frames = {}
    for name in VERTEBRAE:
        if name not in raw:
            continue
        c = mean(raw[name][0])
        t = polyline_fraction(spine, c)
        vertebra_frames[name] = len(frames)
        frames.append((along(spine, t), frame(tangent(spine, t, 0.05), Y, X), 0.0, t))

    meshes, vertices, params, indices = [], [], [], []
    bone_entries = [(n, f, r, b, fl) for (n, f, r, b, fl) in bones if n in raw]
    bone_entries += [(n, vertebra_frames[n], TORSO, 140 if "cervical" in n or n in ("atlas", "axis") else 200, 0) for n in VERTEBRAE if n in raw]
    total_tris = 0
    for name, frame_ref, region, budget, flags in bone_entries:
        frame_index = FRAMES.index(frame_ref) if isinstance(frame_ref, str) else frame_ref
        origin, (p, s, t), limb, _ = frames[frame_index]
        v, f = simplify(*raw[name], budget)
        if not f:
            continue
        # Position along the bone's own longest extent, for cracks and the snap.
        lo = [min(q[i] for q in v) for i in range(3)]
        hi = [max(q[i] for q in v) for i in range(3)]
        axis = max(range(3), key=lambda i: hi[i] - lo[i])
        span = max(1e-6, hi[axis] - lo[axis])
        first = len(vertices)
        for q in v:
            d = sub(q, origin)
            a, b, c = dot(d, p), dot(d, s), dot(d, t)
            if limb > 0:
                vertices.append((a / limb, b / 1000.0, c / 1000.0))
            else:
                vertices.append((a / 1000.0, b / 1000.0, c / 1000.0))
            params.append(int(round(255 * (q[axis] - lo[axis]) / span)))
        first_index = len(indices)
        for tri in f:
            indices.extend(tri)
        meshes.append((name, frame_index, region, flags, first, len(v), first_index, len(f) * 3))
        total_tris += len(f)

    # Rest data the runtime needs: each limb frame's length, each vertebra's place on the spine.
    lines = []
    w = lines.append
    w("#pragma once")
    w("// Generated by Extension/Modes/Tools/bake_bone_meshes.py from BodyParts3D: do not edit.")
    w("// BodyParts3D, (c) The Database Center for Life Science licensed under CC Attribution 4.0 International")
    w("// (https://dbarchive.biosciencedbc.jp/en/bodyparts3d/lic.html). Simplified and rearranged for the Bone Cam.")
    w("#include <array>")
    w("#include <cstdint>")
    w("")
    w("namespace dingosdk::modes::bone_mesh {")
    w("inline constexpr const char *credit = \"BodyParts3D, (c) The Database Center for Life Science licensed under CC Attribution 4.0 International\";")
    w("// Frames: " + ", ".join(f"{i} {n}" for i, n in enumerate(FRAMES)) + f"; {VERTEBRA_FRAME}+ vertebrae.")
    w(f"inline constexpr std::size_t frame_count = {len(frames)}, vertebra_frame = {VERTEBRA_FRAME};")
    w("inline constexpr std::uint8_t snaps = 1, cartilage = 2, cracks = 4;")
    w("struct Frame {")
    w("    float limb;  // a limb frame's rest length (metres; its vertices' first coordinate is a fraction of it), else 0")
    w("    float spine; // where along the spine (hips 0 .. head 1) a vertebra's or the chest's frame sits, else -1")
    w("};")
    w("inline constexpr std::array<Frame, frame_count> frames{{")
    for origin, _, limb, spine_t in frames:
        w(f"    {{{limb / 1000.0:.5f}f, {spine_t:.5f}f}},")
    w("}};")
    w("struct Mesh {")
    w("    std::uint8_t frame, region, flags; // region: modes::BoneSprite")
    w("    std::uint32_t first_vertex, vertex_count, first_index, index_count;")
    w("};")
    w(f"inline constexpr std::array<Mesh, {len(meshes)}> meshes{{{{")
    for name, frame_index, region, flags, first, count, first_index, index_count in meshes:
        w(f"    {{{frame_index}, {region}, {flags}, {first}, {count}, {first_index}, {index_count}}}, // {name}")
    w("}};")
    w(f"inline constexpr std::array<std::array<float, 3>, {len(vertices)}> vertices{{{{")
    for i in range(0, len(vertices), 4):
        w("    " + " ".join(f"{{{a:.5f}f, {b:.5f}f, {c:.5f}f}}," for a, b, c in vertices[i:i + 4]))
    w("}};")
    w(f"inline constexpr std::array<std::uint8_t, {len(params)}> along{{{{")
    for i in range(0, len(params), 32):
        w("    " + ", ".join(str(x) for x in params[i:i + 32]) + ",")
    w("}};")
    w(f"inline constexpr std::array<std::uint16_t, {len(indices)}> indices{{{{")
    for i in range(0, len(indices), 24):
        w("    " + ", ".join(str(x) for x in indices[i:i + 24]) + ",")
    w("}};")
    w("} // namespace dingosdk::modes::bone_mesh")
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(meshes)} bones, {len(vertices)} vertices, {total_tris} triangles, {len(frames)} frames -> {out_path}")


if __name__ == "__main__":
    main()
