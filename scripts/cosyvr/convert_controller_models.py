#!/usr/bin/env python3
"""CosyVR: turns Meta's controller art into the controller models CosyVR draws in VR.

The models belong to Meta and are not part of this repository. Download "Meta Quest
Hardware Art" (Oculus Controller Art) from
https://developers.meta.com/vr/downloads/package/oculus-controller-art/ , unpack it,
and run:

    python convert_controller_models.py <unpacked folder> [<output folder>]

The output folder defaults to CosyVR's user settings folder
(%APPDATA%\\CosyVR_x64\\user_settings\\vr_controllers), where the viewer looks for
left.mesh/left.png and right.mesh/right.png. Without them it draws plain boxes.

Only the Meta Quest Touch Plus (Quest 3) controllers are converted. The package is ASCII
FBX, which Blender cannot import, so this reads it itself; no third-party modules.

Output mesh, text: "cosyvr_controller 1", "vertices N", N lines "x y z nx ny nz u v",
"triangles M", M lines "a b c". Metres, in the frame of the OpenXR aim pose (x right,
y up, -z where the laser points), triangles counter-clockwise seen from outside.
"""

import os
import re
import shutil
import sys

PACKAGE_DIR = "Meta Quest Touch Plus"
SIDES = {
    "left": ("models/MetaQuestTouchPlus_Left.fbx", "textures/MetaQuestTouchPlus_Left_BaseColor_AO.png"),
    "right": ("models/MetaQuestTouchPlus_Right.fbx", "textures/MetaQuestTouchPlus_right_BaseColor_AO.png"),
}
CM = 0.01  # the files are in centimetres


def number_array(block, name):
    match = re.search(name + r": \*\d+ \{\s*a: ([^}]*)\}", block)
    if not match:
        raise ValueError("no " + name)
    return [float(x) for x in match.group(1).replace("\n", "").split(",") if x.strip()]


def layer(block, element):
    start = block.find(element + ": 0 {")
    if start < 0:
        raise ValueError("no " + element)
    depth, end = 0, start
    for end in range(block.index("{", start), len(block)):
        depth += {"{": 1, "}": -1}.get(block[end], 0)
        if depth == 0:
            break
    part = block[start:end]
    mapping = re.search(r'MappingInformationType: "(\w+)"', part).group(1)
    reference = re.search(r'ReferenceInformationType: "(\w+)"', part).group(1)
    return part, mapping, reference


def geometries(text):
    """Each mesh geometry block of the file."""
    starts = [m.start() for m in re.finditer(r'\n\tGeometry: \d+, "Geometry::", "Mesh" \{', text)]
    ends = starts[1:] + [text.find("\n\tModel: ", starts[-1]) if starts else 0]
    return [text[s:e] for s, e in zip(starts, ends)]


def convert_mesh(fbx_path):
    text = open(fbx_path, encoding="latin-1").read()
    if not text.startswith("; FBX"):
        raise ValueError("not an ASCII FBX file")
    # The controller is the big mesh; the other one is the battery indicator quad.
    block = max(geometries(text), key=lambda b: len(number_array(b, "Vertices")))
    positions = number_array(block, "Vertices")
    polygon_indices = [int(i) for i in number_array(block, "PolygonVertexIndex")]
    normal_part, normal_mapping, normal_reference = layer(block, "LayerElementNormal")
    uv_part, uv_mapping, uv_reference = layer(block, "LayerElementUV")
    if (normal_mapping, normal_reference) != ("ByPolygonVertex", "Direct") or \
       (uv_mapping, uv_reference) != ("ByPolygonVertex", "IndexToDirect"):
        raise ValueError("unexpected normal/UV layout: %s %s %s %s" % (normal_mapping, normal_reference, uv_mapping, uv_reference))
    normals = number_array(normal_part, "Normals")
    uvs = number_array(uv_part, "UV")
    uv_indices = [int(i) for i in number_array(uv_part, "UVIndex")]
    if len(normals) != 3 * len(polygon_indices) or len(uv_indices) != len(polygon_indices):
        raise ValueError("normals or UVs do not match the polygons")

    vertices, lookup, triangles, polygon = [], {}, [], []
    for corner, index in enumerate(polygon_indices):
        last = index < 0
        index = ~index if last else index
        # Turned 180 degrees about +Y: the models face +Z, the aim pose looks down -Z.
        x, y, z = positions[3 * index:3 * index + 3]
        nx, ny, nz = normals[3 * corner:3 * corner + 3]
        u, v = uvs[2 * uv_indices[corner]:2 * uv_indices[corner] + 2]
        key = tuple(round(c, 6) for c in (-x * CM, y * CM, -z * CM, -nx, ny, -nz, u, v))
        if key not in lookup:
            lookup[key] = len(vertices)
            vertices.append(key)
        polygon.append(lookup[key])
        if last:
            for i in range(1, len(polygon) - 1):  # a fan: the polygons are quads and a few flat larger ones
                triangles.append((polygon[0], polygon[i], polygon[i + 1]))
            polygon = []
    return vertices, triangles


def check(vertices, triangles):
    """The controller is a hand-sized object; every triangle uses existing vertices."""
    assert vertices and triangles
    assert all(0 <= i < len(vertices) for t in triangles for i in t)
    extent = [max(v[a] for v in vertices) - min(v[a] for v in vertices) for a in range(3)]
    assert all(0.04 < e < 0.2 for e in extent), extent
    # The handle hangs down behind the front: the lowest points are at the back (+z).
    lowest = min(vertices, key=lambda v: v[1])
    assert lowest[2] > 0.0, lowest
    return extent


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    source = os.path.join(sys.argv[1], PACKAGE_DIR)
    target = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.environ.get("APPDATA", "."), "CosyVR_x64", "user_settings", "vr_controllers")
    os.makedirs(target, exist_ok=True)
    for side, (model, texture) in SIDES.items():
        vertices, triangles = convert_mesh(os.path.join(source, model))
        extent = check(vertices, triangles)
        with open(os.path.join(target, side + ".mesh"), "w", newline="\n") as out:
            out.write("cosyvr_controller 1\nvertices %d\n" % len(vertices))
            out.writelines("%g %g %g %g %g %g %g %g\n" % v for v in vertices)
            out.write("triangles %d\n" % len(triangles))
            out.writelines("%d %d %d\n" % t for t in triangles)
        shutil.copyfile(os.path.join(source, texture), os.path.join(target, side + ".png"))
        print("%s: %d vertices, %d triangles, %.1f x %.1f x %.1f cm" % (side, len(vertices), len(triangles), *(e * 100 for e in extent)))
    print("Written to " + target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
