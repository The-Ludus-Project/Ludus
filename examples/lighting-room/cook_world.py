"""Compile the Lighting Room World into the player's geometry and realtime light inputs."""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import sys


def vector(values):
    if not all(math.isfinite(value) for value in values):
        raise ValueError("World coordinates and colours must be finite")
    return "{" + ", ".join(f"{float(value):.9e}F" for value in values) + "}"


def rotate(value, transform):
    x, y, z = value
    for axis in "xyz":
        angle = math.radians(transform["r" + axis])
        c, s = math.cos(angle), math.sin(angle)
        if axis == "x":
            y, z = c*y-s*z, s*y+c*z
        elif axis == "y":
            x, z = c*x+s*z, -s*x+c*z
        else:
            x, y = c*x-s*y, s*x+c*y
    return x, y, z


def cook(source):
    document = json.loads(source)
    if document["schema"] != "ludus.world.default.v1" or document["version"] != 1:
        raise ValueError("Lighting Room expects a built-in version 1 World")
    planes, lights, roles = [], [], {}
    for entity in document["entities"]:
        label = entity["id"]
        components = entity["components"]
        if label in roles:
            raise ValueError(f"{label}: duplicate World role")
        t = components["transform"]
        position = tuple(t[axis] for axis in "xyz")
        if "renderable" in components:
            expected = {"transform", "renderable"}
            if label == "moving-receiver":
                expected.add("giProbeReceiver")
            if set(components) != expected:
                raise ValueError(f"{label}: unsupported renderable components, expected {sorted(expected)}")
            surface = components["renderable"]
            if surface["mesh"] != "mesh.plane" or surface["texture"]:
                raise ValueError(f"{label}: the sample player supports untextured planes")
            rgb = tuple(surface[channel] for channel in "rgb")
            # Columns of the World rotation times the authored non-uniform scale.
            columns = [rotate([t["s"+axis] if axis == other else 0 for other in "xyz"], t) for axis in "xyz"]
            roles[label] = len(planes)
            planes.append("{" + ", ".join([vector(position), *(vector(col) for col in columns),
                                           vector(rgb), str(surface["visible"]).lower()]) + "}")
        elif "light" in components:
            light = components["light"]
            kinds = {"always-on": "directional", "switchable": "point"}
            if (set(components) != {"transform", "light"} or kinds.get(label) != light["kind"]
                    or light["bakeAlwaysOn"] != (label == "always-on")
                    or (label == "always-on" and not light["staticTransform"])):
                raise ValueError(f"{label}: expected a selected static directional or unselected point light")
            kind = {"point": "Point", "directional": "Directional"}[light["kind"]]
            if not light["enabled"]:
                raise ValueError(f"{label}: enable both lights for the fixture baseline")
            roles[label] = len(lights)
            lights.append("{.SourceId=" + str(len(lights)+1) + ", .Kind=rr::LightKind::" + kind
                          + ", .Color=" + vector([light[c] for c in "rgb"]) + f", .Intensity={float(light['intensity'])}F"
                          + ", .Position=" + vector(position) + ", .Direction=" + vector(rotate((0, 0, -1), t))
                          + f", .Range={float(light['range'])}F}}")
        elif set(components) != {"transform", "giProbeVolume"}:
            raise ValueError(f"{label}: unsupported fixture component")
    lines = [f'constexpr char WORLD_SHA256[] = "{hashlib.sha256(source).hexdigest()}";',
             "constexpr Plane PLANES[] = {" + ",\n".join(planes) + "};",
             "constexpr rr::DirectLight LIGHTS[] = {" + ",\n".join(lights) + "};"]
    for label in ("bounce-panel", "bounce-blocker", "bounce-receiver", "moving-receiver",
                  "rough-dielectric", "switchable", "always-on"):
        lines.append(f"constexpr usize {label.upper().replace('-', '_')} = {roles[label]};")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    Path(sys.argv[2]).write_text(cook(Path(sys.argv[1]).read_bytes()))
