"""Generate metre-scale Ludus scenes for matched World and installed-SDK captures."""

from dataclasses import dataclass, replace
import json
import math
from pathlib import Path

EXTENT = 160
LIGHT_Z = 2.0
SEAM = 1.98
CAMERA_Z = 0.5
HALF_VIEW = 0.78
SQUARE = (0.48, 0.48)
BEND = 0.24
SHIFT_PIXELS = 4
SHIFT_METRES = SHIFT_PIXELS * 2 * HALF_VIEW / EXTENT
CENTRE_PROBE_X = SEAM + HALF_VIEW / EXTENT


@dataclass(frozen=True)
class Plate:
    z: float
    left: float
    right: float
    bottom: float
    top: float


@dataclass(frozen=True)
class Case:
    name: str
    plates: tuple = ()
    emitter: tuple = SQUARE
    angle: float | None = None
    bent: bool = False
    camera_x: float = SEAM

    @property
    def control(self):
        return next(
            c
            for c in cases()
            if not c.plates
            and c.angle is None
            and (c.emitter, c.bent, c.camera_x)
            == (self.emitter, self.bent, self.camera_x)
        )


def edge(z):
    # The emitter centre projects the plate's upper-right corner near the cube seam.
    ratio = 1 - z / LIGHT_Z
    return Plate(z, -0.4, SEAM * ratio, -1.5, 0.22 * ratio)


def cases():
    result = []
    for name, size in [
        ("square", SQUARE),
        ("long-x", (0.96, 0.24)),
        ("long-y", (0.24, 0.96)),
    ]:
        result += [Case(name + "-clear", emitter=size), Case(name, (edge(1),), size)]
    result += [
        Case("near-receiver", (edge(0.65),)),
        Case("shift-clear", camera_x=SEAM + SHIFT_METRES),
        Case("shift", (edge(1),), camera_x=SEAM + SHIFT_METRES),
    ]
    # Keep the cube seam fixed while moving the point-shadow edge by 0.1 m on the receiver.
    for name, delta in (("edge-before-seam", -0.05), ("edge-after-seam", 0.05)):
        blocker = edge(1)
        result.append(Case(name, (replace(blocker, right=blocker.right + delta),)))
    # At the central probe each covers half the emitter width. Moving the second
    # through one half-width yields disjoint, partial, and coincident footprints.
    for i, shift in enumerate((0.24, 0.12, 0.0)):
        plates = []
        for z, offset in ((0.8, 0), (1.2, shift)):
            r = z / LIGHT_Z
            left = CENTRE_PROBE_X * (1 - r) + r * (-0.24 + offset)
            plates.append(Plate(z, left, left + r * 0.24, -1, 1))
        result += [
            Case(f"overlap-{i}", tuple(plates)),
            Case(f"overlap-{i}-second", (plates[1],)),
        ]
        if i == 0:
            result.append(Case("overlap-first", (plates[0],)))
    for z in (0.88, 1.0, 1.20, 1.45):
        # Both shadow edges meet at x=SEAM but each dominates on its own y band.
        far = edge(0.80)
        near = edge(z)
        pair = (
            Plate(far.z, far.left, far.right, -1, 0.055),
            Plate(z, near.left, near.right, -0.055, 1),
        )
        name = f"layers-{z:.2f}"
        result += [Case(name, pair), Case(name + "-upper", (pair[1],))]
        if z == 0.88:
            result.append(Case("layers-lower", (pair[0],)))
    bend_camera = (
        SEAM + 0.12
    )  # Include the far end of the rotating box's penumbra on both faces.
    result += [Case("bend-clear", bent=True, camera_x=bend_camera)]
    result += [
        Case(f"rotate-{a}", angle=a, bent=True, camera_x=bend_camera)
        for a in (41, 43, 45, 47, 49)
    ]
    result.append(Case("static-repeat", angle=45, bent=True, camera_x=bend_camera))
    return result


def rectangle(name, material, left, right, bottom, top, height, reverse=False):
    corners = [(left, bottom), (right, bottom), (right, top), (left, top)]
    return dict(
        name=name,
        material=material,
        positions=[[x, y, height(y)] for x, y in corners],
        polygons=[[3, 2, 1, 0] if reverse else [0, 1, 2, 3]],
    )


BOX_CENTER = (0.93, 0, 1.0)
BOX_HALF = (0.24, 0.9, 0.14)


def box(angle):
    c, s = math.cos(math.radians(angle)), math.sin(math.radians(angle))
    points = []
    for iz in (-1, 1):
        for ix, iy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            x, y, z = [n * r for n, r in zip((ix, iy, iz), BOX_HALF)]
            points.append(
                [BOX_CENTER[0] + x * c + z * s, y, BOX_CENTER[2] - x * s + z * c]
            )
    return dict(
        name="Rotating blocker",
        material="Black",
        positions=points,
        polygons=[
            [3, 2, 1, 0],
            [4, 5, 6, 7],
            [0, 1, 5, 4],
            [1, 2, 6, 5],
            [2, 3, 7, 6],
            [3, 0, 4, 7],
        ],
    )


def write_scene(case, root):
    source = root / case.name / "source"
    source.mkdir(parents=True, exist_ok=True)
    w, h = case.emitter
    objects = [
        rectangle(
            "Emitter", "Panel", -w / 2, w / 2, -h / 2, h / 2, lambda y: LIGHT_Z, True
        ),
        rectangle("Receiver", "Matte", -1, 5, -3, 0 if case.bent else 3, lambda y: 0),
    ]
    if case.bent:
        objects.append(rectangle("Fold", "Matte", -1, 5, 0, 3, lambda y: -BEND * y))
    for i, p in enumerate(case.plates):
        objects.append(
            rectangle(
                f"Blocker {i}", "Black", p.left, p.right, p.bottom, p.top, lambda y: p.z
            )
        )
    if case.angle is not None:
        objects.append(box(case.angle))
    materials = [
        dict(name=name, model="diffuse", base_color=[gray] * 3)
        for name, gray in [("Matte", 0.8), ("Black", 0), ("Panel", 0)]
    ]
    materials[-1]["emission"] = dict(radiance=[65] * 3, two_sided=False)
    camera = dict(
        eye=[case.camera_x, 0, CAMERA_Z],
        target=[case.camera_x, 0, 0],
        up=[0, 1, 0],
        vertical_fov_degrees=math.degrees(2 * math.atan(HALF_VIEW / CAMERA_Z)),
    )
    scene = dict(
        format="ludus.scene",
        version=1,
        name=case.name,
        camera=camera,
        materials=materials,
        objects=objects,
    )
    (source / "shadow.luscene.json").write_text(json.dumps(scene, indent=2) + "\n")


if __name__ == "__main__":
    import sys

    for case in cases():
        write_scene(case, Path(sys.argv[1]))
