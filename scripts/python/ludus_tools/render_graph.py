"""RG0 bounded graph documents and deterministic, contract-only serial plans.

This offline backend has no Qt, engine, shader compiler or GPU dependency. It is
shared by the installed CLI and future editor. Plans are diagnostics, not RHI
commands or shader packages; RG1/RG2 own those integrations.

Thanks to Donald Revie, "Designing a Data-Driven Renderer", GPU Pro 3, V.4,
sections 4.4-4.8, and Arseny Kapoulkine, "Writing an Efficient Vulkan Renderer",
GPU Zen 2, IV.4, sections 4.4-4.6: separate representation/context and enumerate
complete authored techniques. This original implementation uses explicit ports,
resource versions and immutable build inputs, not their object/context stacks.
See docs/architecture/render-graph-authoring.md for contracts and departures.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
import hashlib
import json
from pathlib import Path
import re
import sys

from .errors import ToolingError
from .render_graph_diagnostics import DiagnosticBatch, make_error
from . import render_graph_options as options
from . import render_graph_pipeline as pipeline
from . import render_graph_buffers as buffers
from . import render_graph_scene as scene_bundle
from . import render_graph_ray as ray
from . import render_graph_compute as compute
from . import render_graph_upscale as upscale
from . import render_graph_quality as quality
from . import render_graph_sizes as sizes
from . import render_graph_layout as layout_policy

PLAN_VERSION = 1
MAX_ASSET_BYTES = 128 * 1024
MAX_TOTAL_BYTES = 2 * 1024 * 1024
MAX_ASSETS = 32
GRAPH_SOURCE_REPAIR = ("Remove Subgraph nodes whose library graphs the package no longer needs (in the editor, "
                       "right-click each one on the canvas and choose Delete Subgraph and Library, which also "
                       "deletes the library and the shaders only it uses) and try again.")
MAX_DEPTH = 8
MAX_CONFIGURATIONS = 8
MAX_NODES = 128
MAX_EDGES = 512
MAX_EXPANDED_NODES = 1024
MAX_PASSES = 256
MAX_TARGET_EXTENT = 16384
MAX_ACCUMULATION_SAMPLES = 4096
MIN_TONE_MAPPING_EXPOSURE = 0.01
MAX_TONE_MAPPING_EXPOSURE = 64
# ToneMap display curves; mirrors render_graph_format::DisplayCurve. Absent selects Reinhard.
TONE_MAPPING_CURVES = ("Reinhard", "PbrNeutral")
TYPES = ("Scene", "View", "ColorTexture", "DepthTexture")
TEXTURES = TYPES[2:]
MODES = ("ColorAndDepth", "DepthOnly")
# Authored material families; each selects its own explicitly declared technique set.
MATERIALS = {"DefaultOpaque": "DefaultStaticOpaque", "DefaultMasked": "DefaultStaticMasked"}
FORMATS = {"ColorTexture": ("RGBA8Unorm",), "DepthTexture": ("Depth32Float",)}
# Kinds that may be authored with `"enabled": false`, and the (input, output) port a
# disabled node forwards unchanged. Both ports share one type, so rewiring preserves typing.
BYPASS_PORTS = {"Accumulate": ("color", "color"), "Denoise": ("color", "color"), "ToneMap": ("color", "color")}
# Sinks that may be disabled. A disabled sink keeps its structurally validated wire but is
# no root, so its input needs no connection and only enabled roots retain their routes.
DISABLE_KINDS = (*BYPASS_PORTS, "Export")
IDENTIFIER = re.compile(r"[A-Za-z][A-Za-z0-9_-]{0,62}\Z")


def _fail(code, where, message, **details):
    raise make_error(code, where, message, **details)


def _require(condition, code, where, message, **details):
    if not condition:
        _fail(code, where, message, **details)


def _keys(value, required, where, optional=()):
    _require(type(value) is dict, "Schema", where, "object required")
    _require(all(type(key) is str for key in value), "Schema", where, "field names must be strings")
    missing = set(required) - value.keys()
    unknown = value.keys() - set(required) - set(optional)
    _require(not missing and not unknown, "Schema", where,
             f"missing fields {sorted(missing)}; unknown fields {sorted(unknown)}")


def _identifier(value, where):
    _require(type(value) is str and IDENTIFIER.fullmatch(value) is not None,
             "Schema", where, "identifier must be 1..63 ASCII identifier characters")
    return value


def _enum(value, choices, where):
    _require(type(value) is str and value in choices, "Schema", where,
             f"expected one of {', '.join(choices)}")


def _boolean(value, where):
    _require(type(value) is bool, "Schema", where, "Boolean required")


def _integer(value, low, high, where):
    _require(type(value) is int and low <= value <= high, "Capacity", where,
             f"integer must be in [{low}, {high}]")


def _list(value, maximum, where):
    _require(type(value) is list and len(value) <= maximum, "Capacity", where,
             f"array with at most {maximum} entries required")


def _endpoint(value, where):
    _require(type(value) is str and value.count(".") == 1,
             "Schema", where, "endpoint must be node.port (input.port for an interface)")
    node, port = value.split(".")
    _identifier(node, where)
    # Derived members preserve every valid declared bundle ID, including 63-byte names.
    suffix = next(("_" + part for part in scene_bundle.PARTS if port.endswith("_" + part)), "")
    _identifier(port[:-len(suffix)] if suffix and len(port) > 63 else port, where)
    return node, port


def _ports(values, where, outputs=False, version=1):
    _list(values, 8, where)
    claimed = set()
    for port in values:
        _keys(port, ("id", "type", "source", "required") if outputs else ("id", "type"), where)
        _identifier(port["id"], where)
        _require(port["id"] not in claimed, "DuplicateIdentity", where, port["id"])
        claimed.add(port["id"])
        _enum(port["type"], ((*pipeline.RESOURCE_TYPES, 'RenderSize', 'Scene', *scene_bundle.TYPES.values()) if outputs else (*pipeline.INPUT_TYPES, 'RenderSize'))
              if version == 2 else (TEXTURES if outputs else TYPES), where)
        if outputs:
            _endpoint(port["source"], where)
            _boolean(port["required"], where)


def _validate_size(extent, where):
    if type(extent) is dict:
        _keys(extent, {'reference', 'scale'}, where)
        _enum(extent['reference'], ('View',), where)
        _validate_extent(extent['scale'], where)
    else:
        _validate_extent(extent, where)


def _validate_extent(extent, where):
    _require(type(extent) is list and len(extent) == 2,
             'Schema', where, 'extent must contain width and height in pixels')
    for size in extent:
        _integer(size, 1, MAX_TARGET_EXTENT, where)


def _validate_node(node, where):
    _require(type(node) is dict, "Schema", where, "node object required")
    kind = node.get("kind")
    _require("enabled" not in node or kind in DISABLE_KINDS, "Schema", where,
             f"only {', '.join(DISABLE_KINDS)} nodes can be disabled; remove the enabled field")
    if kind in sizes.KINDS:
        sizes.validate(node, where, sys.modules[__name__])
        return
    if kind == 'Upscale':
        upscale.validate(node, where, sys.modules[__name__])
        return
    if kind == 'StructuredBuffer':
        buffers.validate_node(node, where, sys.modules[__name__])
        return
    if kind in pipeline.KINDS:
        pipeline.validate(node, where, sys.modules[__name__])
        return
    _enum(kind, ("Scene", "Camera", "Target", "Buffer", "Raster", "Accumulate", "ToneMap", "Denoise", "Subgraph", "Export", "Present"), where)
    fields = {"id", "kind"}
    if kind == "Denoise":
        fields.add("quality")
    elif kind == "Accumulate":
        fields.add("samples")
    elif kind == "ToneMap":
        fields.add("exposure")
        if "curve" in node:
            fields.add("curve")
    elif kind == "Camera":
        fields |= {"active", "role", "target_type"}
    elif kind == "Buffer":
        fields |= ({"bytes_per_pixel", "extent"} if "bytes_per_pixel" in node else {"bytes"})
    elif kind == "Target":
        fields |= {"type", "format", "extent", "samples", "imported", "initialized", "presentation"}
    elif kind == "Raster":
        fields |= {"mode", "vertex_layout", "material", "depth_load", "depth_store"}
        if node.get("mode") == "ColorAndDepth":
            fields |= {"color_load", "color_store"}
    elif kind == "Subgraph":
        fields |= {"asset", "configuration"}
    elif kind == "Export":
        fields |= {"type"}
    _keys(node, fields, where, ("enabled",) if kind in DISABLE_KINDS else ("overrides",) if kind == "Subgraph" else ())
    _identifier(node["id"], where)
    _require(node["id"] != "input", "Schema", where, "input is reserved for interface ports")
    if "enabled" in node:
        _boolean(node["enabled"], where)
    if kind == "Denoise":
        _enum(node["quality"], tuple(quality.QUALITY_CODES), where + "/quality")
    elif kind == "Accumulate":
        _integer(node["samples"], 1, MAX_ACCUMULATION_SAMPLES, where)
    elif kind == "ToneMap":
        exposure = node["exposure"]
        _require(type(exposure) in (int, float) and MIN_TONE_MAPPING_EXPOSURE <= exposure <= MAX_TONE_MAPPING_EXPOSURE,
                 "Capacity", where, f"exposure must be a finite multiplier in [{MIN_TONE_MAPPING_EXPOSURE}, {MAX_TONE_MAPPING_EXPOSURE}]")
        if "curve" in node:
            _enum(node["curve"], TONE_MAPPING_CURVES, where + "/curve")
    elif kind == "Camera":
        _boolean(node["active"], where)
        _enum(node["role"], ("Presentation", "Offscreen"), where)
        _enum(node["target_type"], TEXTURES, where)
        _require(node["role"] != "Presentation" or node["target_type"] == "ColorTexture",
                 "TypeMismatch", where, "a presentation camera requires a color target")
    elif kind == "Buffer":
        if 'bytes_per_pixel' in node:
            _integer(node['bytes_per_pixel'], ray.BUFFER_WORD_BYTES, ray.MAX_BUFFER_PIXEL_BYTES, where)
            _require(node['bytes_per_pixel'] % ray.BUFFER_WORD_BYTES == 0 and node['extent'] == 'RenderSize',
                     'Capacity', where, 'pixel buffers require RenderSize and a stride multiple of 16')
        else:
            _integer(node['bytes'], ray.BUFFER_WORD_BYTES, ray.MAX_BUFFER_BYTES, where)
            _require(node['bytes'] % ray.BUFFER_WORD_BYTES == 0, 'Capacity', where,
                     'persistent buffer byte size must be a multiple of 16')
    elif kind == "Target":
        _enum(node["type"], TEXTURES, where)
        _enum(node["format"], FORMATS[node["type"]], where)
        if node['extent'] != 'RenderSize':
            _validate_extent(node['extent'], where)
        _integer(node["samples"], 1, 1, where)
        for flag in ("imported", "initialized", "presentation"):
            _boolean(node[flag], where)
        _require(not node["initialized"] or node["imported"], "Initialization", where,
                 "only an imported target may declare existing initialized contents")
        _require(not node["presentation"] or
                 (node["imported"] and node["type"] == "ColorTexture"),
                 "TypeMismatch", where, "presentation requires an imported color target")
    elif kind == "Raster":
        _enum(node["mode"], MODES, where)
        _enum(node["vertex_layout"], ("RendererStatic",), where)
        _enum(node["material"], tuple(MATERIALS), where)
        for aspect in ("depth", "color") if node["mode"] == "ColorAndDepth" else ("depth",):
            _enum(node[aspect + "_load"], ("Clear", "Load", "Discard"), where)
            _enum(node[aspect + "_store"], ("Store", "Discard"), where)
    elif kind == "Subgraph":
        _require(type(node["asset"]) is str and 0 < len(node["asset"]) <= 255,
                 "Schema", where, "package-relative asset path required")
        _identifier(node["configuration"], where)
    elif kind == "Export":
        _enum(node["type"], TEXTURES, where)


def _validate_document(value, where):
    _keys(value, ("version", "id", "default", "configurations"), where)
    _require(type(value["version"]) is int and value["version"] in (1, 2),
             "Version", where, "render graph document version 1 or 2 required")
    _identifier(value["id"], where)
    _identifier(value["default"], where)
    _list(value["configurations"], MAX_CONFIGURATIONS, where)
    claimed = set()
    for config in value["configurations"]:
        _keys(config, ("id", "inputs", "outputs", "nodes", "edges"), where, ("layout", "options"))
        _identifier(config["id"], where)
        _require(config["id"] not in claimed, "DuplicateIdentity", where, config["id"])
        claimed.add(config["id"])
        location = where + "#" + config["id"]
        options.validate(config.get("options", []), {}, location, sys.modules[__name__])
        _ports(config["inputs"], location, version=value["version"])
        _ports(config["outputs"], location, outputs=True, version=value["version"])
        _list(config["nodes"], MAX_NODES, location)
        _list(config["edges"], MAX_EDGES, location)
        nodes = set()
        for node in config["nodes"]:
            _require(type(node) is dict, "Schema", location, "node object required")
            _require(value["version"] == 2 or node.get("kind") not in (*pipeline.KINDS, "StructuredBuffer", "Upscale"),
                     "Version", location, "pipeline, structured buffer and upscaling nodes require document version 2")
            _require(value['version'] == 2 or (node.get('kind') not in sizes.KINDS and node.get('extent') != 'RenderSize'),
                     'Version', location, 'shared render sizes require document version 2')
            _validate_node(node, location + "/" + str(node.get("id", "node"))
                           if node.get("kind") in (*pipeline.KINDS, *sizes.KINDS, 'StructuredBuffer', 'Upscale') else location)
            _require(node["id"] not in nodes, "DuplicateIdentity", location, node["id"])
            nodes.add(node["id"])
        for edge in config["edges"]:
            _keys(edge, ("from", "to"), location)
            _endpoint(edge["from"], location)
            _endpoint(edge["to"], location)
        if "layout" in config:
            layout = config["layout"]
            _keys(layout, ("positions",), location, ("frames", "routes"))
            layout_policy.validate_frames(layout.get("frames", []), location, sys.modules[__name__])
            layout_policy.validate_routes(layout.get("routes", []), config, location, sys.modules[__name__])
            _list(layout["positions"], MAX_NODES, location)
            placed = set()
            for item in layout["positions"]:
                _keys(item, ("node", "x", "y"), location)
                _require(type(item["node"]) is str and item["node"] in nodes
                         and item["node"] not in placed, "Schema", location, "invalid layout node")
                placed.add(item["node"])
                for coordinate in ("x", "y"):
                    _integer(item[coordinate], -layout_policy.MAX_COORDINATE, layout_policy.MAX_COORDINATE, location)
    _require(value["default"] in claimed, "Configuration", where, "default configuration is missing")


def canonical_asset(value):
    """Serialize a locally schema-checked versioned asset; imports need closure validation.

    Returns UTF-8 bytes without mutating the caller's document. Rejects oversized
    sources and unknown fields. Does not write files or resolve resource bindings.
    """
    _validate_document(value, "document")
    data = (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + "\n").encode()
    _require(len(data) <= MAX_ASSET_BYTES, "Capacity", "document", "asset exceeds 128 KiB")
    return data


def _read(path):
    try:
        with path.open("rb") as stream:
            data = stream.read(MAX_ASSET_BYTES + 1)
    except (OSError, ValueError) as error:
        _fail("Source", str(path), str(error))
    _require(len(data) <= MAX_ASSET_BYTES, "Capacity", str(path), "asset exceeds 128 KiB")
    return data


def _resolve(path, where):
    try:
        return Path(path).resolve()
    except (OSError, RuntimeError, ValueError) as error:
        _fail("Source", where, str(error))


def _parse(data, where):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            _require(key not in result, "Schema", where, f"duplicate JSON field {key}")
            result[key] = value
        return result

    def constant(value):
        _fail("Schema", where, f"nonfinite JSON constant {value}")

    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=unique, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError) as error:
        _fail("Schema", where, str(error))
    _validate_document(value, where)
    return value


def _configuration(asset, identity, where):
    for config in asset["configurations"]:
        if config["id"] == identity:
            return config
    _fail("Configuration", where, f"configuration {identity} is not declared by {asset['id']}")


def _bypass_disabled(asset, where):
    """Return the execution view of a validated asset without its disabled nodes.

    Each consumer of a disabled node's output receives that node's input source, as
    though the node and its two wires were one wire; chains of disabled nodes collapse
    to the first enabled source. Wires into a disabled node are dropped. Disabled sinks
    stay; _is_effect excludes them from roots. The caller's document is not mutated, so
    source bytes, layout and canonical form are unchanged. A consumed disabled output
    whose input is unconnected raises UnboundInput.
    """
    configurations = []
    for config in asset["configurations"]:
        disabled = {node["id"]: BYPASS_PORTS[node["kind"]]
                    for node in config["nodes"] if node.get("enabled") is False and node["kind"] in BYPASS_PORTS}
        if not disabled:
            configurations.append(config)
            continue
        location = where + "#" + config["id"]
        sources = {}
        for edge in config["edges"]:
            node, port = edge["to"].split(".")
            if node in disabled and port == disabled[node][0]:
                sources[node] = edge["from"]

        def forward(endpoint, consumer):
            visited = set()
            node, port = endpoint.split(".")
            while node in disabled and port == disabled[node][1]:
                _require(node not in visited, "Cycle", location + "/" + node,
                         "Disabled nodes forward each other's outputs in a cycle. Remove a feedback wire.")
                visited.add(node)
                _require(node in sources, "UnboundInput", location + "/" + node,
                         f"Disabled node '{node}' would pass its {disabled[node][0]} input to {consumer}, but that "
                         "input is unconnected. Connect its input, enable the node, or disconnect its output.",
                         port=disabled[node][0], direction='input', expected='connected input of a disabled node',
                         actual='unconnected', repair='Connect the input, enable the node, or remove its output wires.')
                endpoint = sources[node]
                node, port = endpoint.split(".")
            return endpoint

        edges = [{"from": forward(edge["from"], edge["to"]), "to": edge["to"]}
                 for edge in config["edges"] if edge["to"].split(".")[0] not in disabled]
        outputs = [{**output, "source": forward(output["source"], "output " + output["id"])}
                   for output in config["outputs"]]
        configurations.append({**config, "nodes": [node for node in config["nodes"] if node["id"] not in disabled],
                               "edges": edges, "outputs": outputs})
    return {**asset, "configurations": configurations}


def _is_effect(node):
    """True for an observable sink: an enabled Export, a Present or an active camera obligation."""
    return ((node['kind'] == 'Export' and node.get('enabled') is not False) or node['kind'] == 'Present' or
            (node['kind'] == 'Camera' and node['active']))


def _locate_one(error, library, instances=None):
    """Map a validator location to its authored package-relative owner.

    Locations are package-relative source labels optionally followed by
    `#configuration` and `/node`, or expanded instance paths recorded by the
    compiler. The result names the source, configuration, node and nested
    instance path when known; absolute machine paths are never returned.
    """
    if getattr(error, "location", None) is not None:
        return error
    where = getattr(error, "where", None)
    if type(where) is not str:
        return error
    location = {}
    labels = sorted({library.label(p) for p in library.attempted}, key=len, reverse=True)
    if instances:
        prefix = max((key for key in instances if where == key or where.startswith(key + "/")),
                     key=len, default=None)
        if prefix is not None:
            source, configuration, nodes = instances[prefix]
            location = {"source": source, "configuration": configuration, "instance": where}
            remainder = where[len(prefix) + 1:].split("/")[0] if where != prefix else ""
            if remainder in nodes:
                location["node"] = remainder
    if not location:
        candidate = where
        try:
            path = Path(where)
            if path.is_absolute() and path.resolve().is_relative_to(library.root):
                candidate = library.label(path.resolve())
        except (OSError, RuntimeError, ValueError):
            pass
        for label in labels:
            if candidate == label or candidate.startswith(label + "#") or candidate.startswith(label + "/"):
                location["source"] = label
                remainder = candidate[len(label):]
                if remainder.startswith("#"):
                    configuration, _, node = remainder[1:].partition("/")
                    location["configuration"] = configuration
                else:
                    node = remainder[1:]
                if node:
                    location["node"] = node.split("/")[0]
                break
    if location:
        error.location = location
    return error


def _locate(error, library, instances=None):
    records = []
    for issue in getattr(error, 'issues', [error]):
        _locate_one(issue, library, instances)
        location = {**(issue.location or {}), **getattr(issue, 'target', {})}
        if location:
            issue.location = location
        records.append({'code': issue.code, 'severity': getattr(issue, 'severity', 'error'), 'message': issue.message,
                        'location': location, **getattr(issue, 'details', {})})
    error.diagnostics = records
    return error


def _lower_pipeline(node, where):
    if node['kind'] == 'ComputePipeline':
        if pipeline.is_scene_compute(node) and any(shader['stage'] in ray.STAGES for shader in node['shaders']):
            return compute.lower(node, where, sys.modules[__name__])
        _require(any(shader['stage'] == 'compute' for shader in node['shaders']),
                 'IncompletePipeline', where,
                 'ComputePipeline is missing its compute shader. Add that stage in Shaders and assign a build-manifest program ID.',
                 field='shaders', expected='compute shader', actual='no compute stage',
                 repair='Add a compute stage in Shaders and assign a build-manifest program ID.')
        if pipeline.is_scene_compute(node):
            return compute.lower(node, where, sys.modules[__name__])
        _fail('Capability', where, 'ComputePipeline execution without a Scene input is not supported by the current graph build profile. '
              'Add a Scene input for a scene compute pass, keep it as a saved draft, or use a GraphicsPipeline '
              'with supported vertex/fragment programs for preview.',
              expected='scene compute, supported graphics or triangle ray profile', actual='ComputePipeline without Scene input',
              repair='Add a Scene input for a scene compute pass, or keep this compute pipeline as an unused draft.')
    return (ray.lower(node, where, sys.modules[__name__]) if node['kind'] == 'RayTracingPipeline'
            else pipeline.lower_graphics(node, where, sys.modules[__name__]))


class _Library:
    def __init__(self, root):
        self.root = _resolve(root, str(root))
        self.assets = {}
        self.bytes = {}
        self.identities = {}
        self.active = []
        self.attempted = set()
        self.total = 0
        self.effects = {}

    def inside(self, relative, where):
        _require(type(relative) is str and relative and "\\" not in relative and ":" not in relative,
                 "Source", where, "imports must use package-relative POSIX paths")
        path = Path(relative)
        _require(not path.is_absolute() and ".." not in path.parts,
                 "Source", where, "import escapes the graph package")
        resolved = _resolve(self.root / path, where)
        _require(resolved.is_relative_to(self.root), "Source", where, "import symlink escapes the package")
        return resolved

    def label(self, path):
        return path.relative_to(self.root).as_posix()

    def load(self, path):
        _require(path.is_relative_to(self.root), "Source", str(path), "source must be inside the package")
        where = self.label(path)
        _require(path not in self.active, "Recursion", where,
                 "recursive graph import: " + " -> ".join(self.label(p) for p in [*self.active, path]))
        if path in self.assets:
            return self.assets[path]
        _require(len(self.active) < MAX_DEPTH, "Capacity", where,
                 f"subgraphs nest {len(self.active) + 1} levels deep, more than the {MAX_DEPTH} levels one graph may "
                 "import. Flatten a nested Subgraph by moving its nodes into its parent configuration.")
        _require(len(self.assets) < MAX_ASSETS, "Capacity", where,
                 f"the graph already imports {len(self.assets)} graph documents, the most one package may "
                 f"({MAX_ASSETS}). {GRAPH_SOURCE_REPAIR}")
        self.attempted.add(path)
        data = _read(path)
        self.total += len(data)
        _require(self.total <= MAX_TOTAL_BYTES, "Capacity", where,
                 f"the imported graph documents total {self.total} bytes, more than one package may "
                 f"({MAX_TOTAL_BYTES}). {GRAPH_SOURCE_REPAIR}")
        # Disabled nodes are validated as authored, then forwarded out of every later stage.
        asset = _bypass_disabled(_parse(data, where), where)
        _require(asset["id"] not in self.identities, "DuplicateIdentity", where,
                 f"asset identity {asset['id']} is already owned by another file")
        self.identities[asset["id"]] = path
        self.assets[path] = asset
        self.bytes[path] = data
        self.active.append(path)
        try:
            for config in asset["configurations"]:
                for node in config["nodes"]:
                    if node["kind"] == "Subgraph":
                        child = self.load(self.inside(node["asset"], where))
                        _configuration(child, node["configuration"], where + "/" + node["id"])
        finally:
            self.active.pop()
        return asset

    def reachable(self, path):
        """Return {file label: [configuration ids]} for the root configurations of the
        loaded asset at path and every configuration their Subgraph nodes import.

        load() validates every configuration of an imported file, but only these are
        planned; an imported file's unreferenced configurations are absent.
        """
        reached = {}
        pending = [(path, config["id"]) for config in self.assets[path]["configurations"]]
        while pending:
            source, identity = pending.pop()
            label = self.label(source)
            if identity in reached.setdefault(label, []):
                continue
            reached[label].append(identity)
            for node in _configuration(self.assets[source], identity, label)["nodes"]:
                if node["kind"] == "Subgraph":
                    pending.append((self.inside(node["asset"], label), node["configuration"]))
        return {label: sorted(identities) for label, identities in sorted(reached.items())}

    def node_ports(self, node):
        kind = node["kind"]
        if kind == "Upscale":
            return upscale.input_ports(node), {"color": "ColorTexture"}
        if kind in pipeline.KINDS:
            return (scene_bundle.ports({p["id"]: p["type"] for p in node["inputs"]}),
                    {p["id"]: p["type"] for p in node["outputs"]})
        if kind in ("Accumulate", "ToneMap", "Denoise"):
            return {"color": "ColorTexture"}, {"color": "ColorTexture"}
        if kind == "Scene":
            return {}, {"scene": "Scene", **scene_bundle.TYPES}
        if kind == "Camera":
            return {"target": node["target_type"]}, {"view": "View"}
        if kind in ("Buffer", "StructuredBuffer"):
            return ({'size': 'RenderSize'} if 'bytes_per_pixel' in node else {}), {'buffer': 'Buffer'}
        if kind == "Target":
            return ({'size': 'RenderSize'} if node['extent'] == 'RenderSize' else {}), {"texture": node["type"]}
        if kind in sizes.KINDS:
            return sizes.input_ports(node), {'size': 'RenderSize'}
        if kind == "Raster":
            inputs = {"scene": "Scene", "view": "View", "depth": "DepthTexture"}
            outputs = {"depth": "DepthTexture"}
            if node["mode"] == "ColorAndDepth":
                inputs["color"] = "ColorTexture"
                outputs["color"] = "ColorTexture"
            return inputs, outputs
        if kind == "Subgraph":
            path = self.inside(node["asset"], node["id"])
            config = _configuration(self.assets[path], node["configuration"], node["id"])
            return ({p["id"]: p["type"] for p in config["inputs"]},
                    {p["id"]: p["type"] for p in config["outputs"]})
        return {"texture" if kind == "Export" else "color":
                node["type"] if kind == "Export" else "ColorTexture"}, {}

    def order(self, path, config):
        where = self.label(path) + "#" + config["id"]
        nodes = {n["id"]: n for n in config["nodes"]}
        ports = {key: self.node_ports(node) for key, node in nodes.items()}
        sources = {"input": {p["id"]: p["type"] for p in config["inputs"]}}
        sources.update({key: value[1] for key, value in ports.items()})
        incoming = {key: {} for key in nodes}

        diagnostics = DiagnosticBatch()

        def source(endpoint):
            key, port = _endpoint(endpoint, where)
            _require(key in sources and port in sources[key], "Port", where + "/" + key,
                     f"Output '{endpoint}' does not exist. Reconnect from a declared output port or restore that port in Edit Node.",
                     port=port, direction='output', expected='declared output port', actual=endpoint,
                     repair='Reconnect from an existing output, or restore the output declaration.')
            return sources[key][port]

        for edge in config["edges"]:
            with diagnostics.capture():
                kind = source(edge["from"])
                key, port = _endpoint(edge["to"], where)
                target = where + '/' + key
                _require(key in ports and port in ports[key][0], "Port", target,
                         f"Input '{edge['to']}' does not exist. Reconnect to a declared input port or restore that port in Edit Node.",
                         port=port, direction='input', expected='declared input port', actual=edge['to'],
                         repair='Reconnect to an existing input, or restore the input declaration.')
                _require(port not in incoming[key], "MultipleWriters", target,
                         f"Input '{edge['to']}' has multiple connections, including '{edge['from']}'. Disconnect all but one incoming wire.",
                         port=port, direction='input', expected='one incoming connection', actual='multiple connections',
                         repair='Disconnect all but one incoming wire, or replace its current connection.')
                expected = ports[key][0][port]
                _require(kind == expected or (expected == 'Texture' and kind in TEXTURES), "TypeMismatch", target,
                         f"{edge['from']} ({kind}) cannot connect to {edge['to']} ({expected}). "
                         f"Connect a {expected} output to this input, or change the input type in Edit Node.",
                         port=port, direction='input', expected=expected, actual=kind,
                         repair=f'Connect a {expected} output, or change this input declaration.')
                incoming[key][port] = edge["from"]
        for key, node in nodes.items():
            if node['kind'] in pipeline.KINDS:
                with diagnostics.capture():
                    scene_bundle.check_connections(node, incoming[key], where + '/' + key, sys.modules[__name__])
        for output in config["outputs"]:
            with diagnostics.capture():
                actual = source(output["source"])
                _require(actual == output["type"], "TypeMismatch", where,
                         f"Configuration output '{output['id']}' expects {output['type']}, but source '{output['source']}' has type {actual}. "
                         'Choose a matching source in Edit Inputs / Outputs, or change the output type.',
                         port=output['id'], direction='output', field='outputs', expected=output['type'], actual=actual,
                         repair='Choose a matching source or change this output type in Edit Inputs / Outputs.')
        # Broken wiring invalidates dependency ordering; do not emit cycles or execution cascades.
        diagnostics.raise_if_any()
        dependencies = {key: {_endpoint(e, where)[0] for e in values.values()} - {"input"}
                        for key, values in incoming.items()}
        ordered, done = [], set()
        while len(done) < len(nodes):
            ready = next((key for key in nodes if key not in done and dependencies[key] <= done), None)
            _require(ready is not None, "Cycle", where, "Within-frame dependency cycle. Remove a feedback wire and connect passes in forward execution order.")
            ordered.append(nodes[ready])
            done.add(ready)
        return ordered, incoming

    def has_effects(self, path, config):
        """Explicit sinks and active-camera obligations survive unused output culling."""
        identity = (path, config['id'])
        if identity not in self.effects:
            self.effects[identity] = any(
                _is_effect(node) or (node['kind'] == 'Subgraph' and self.child_has_effects(node))
                for node in config['nodes'])
        return self.effects[identity]

    def child_has_effects(self, node):
        path = self.inside(node['asset'], node['id'])
        return self.has_effects(path, _configuration(self.assets[path], node['configuration'], node['id']))

    def execution_order(self, path, config, requested_outputs, diagnostics=None):
        """Select observable dependencies before checking execution-only requirements.

        A retained subgraph keeps its declared input contract. Only the requested
        output branches and explicit effects inside that subgraph are expanded.
        Structural validation still covers every saved node and wire in order().
        """
        own_diagnostics = diagnostics is None
        diagnostics = DiagnosticBatch() if own_diagnostics else diagnostics
        ordered, incoming = self.order(path, config)
        where = self.label(path) + '#' + config['id']
        used = {port['source'] for port in config['outputs'] if port['id'] in requested_outputs}
        selected = {node['id'] for node in ordered if _is_effect(node) or
                    (node['kind'] == 'Subgraph' and self.child_has_effects(node))}
        selected.update(_endpoint(endpoint, where)[0] for endpoint in used)
        # Reverse topological order visits each newly selected producer later.
        for node in reversed(ordered):
            if node['id'] in selected:
                used.update(incoming[node['id']].values())
                selected.update(_endpoint(endpoint, where)[0] for endpoint in incoming[node['id']].values())
        ordered = [node for node in ordered if node['id'] in selected]
        for node in ordered:
            key = node['id']
            ports = self.node_ports(node)
            required = ({p["id"] for p in node["inputs"] if p["required"]}
                        if node["kind"] in pipeline.KINDS else set(ports[0]))
            if node["kind"] == "Camera" and not node["active"]:
                required.clear()
            if node['kind'] in pipeline.KINDS:
                required.update(port['target'] for port in node['outputs'])
            if node["kind"] in pipeline.KINDS:
                required -= {p["id"] for p in node["inputs"] if p["type"] == "Scene" and
                             any(p["id"] + "_" + part in incoming[key] for part in scene_bundle.PARTS)}
            missing = sorted(required - incoming[key].keys())
            for port in missing:
                with diagnostics.capture():
                    _fail("UnboundInput", where + "/" + key,
                          f"Unconnected inputs: '{key}.{port}' ({ports[0][port]}). Connect a matching output; "
                          'writable resource inputs must be connected even when Load is Clear or Discard.',
                          port=port, direction='input', expected=ports[0][port], actual='unconnected',
                          repair=f'Connect a {ports[0][port]} output to {key}.{port}.')
            if node["kind"] == "Subgraph":
                child = self.assets[self.inside(node["asset"], where)]
                selected = _configuration(child, node["configuration"], where)
                for output in selected["outputs"]:
                    with diagnostics.capture():
                        _require(not output["required"] or key + "." + output["id"] in used,
                                 "UnusedRequiredOutput", where + "/" + key,
                                 f"output {output['id']} requires a consumer",
                                 port=output['id'], direction='output', expected='connected consumer', actual='unused',
                                 repair='Connect this required output, or make it optional in the subgraph interface.')
        if own_diagnostics:
            diagnostics.raise_if_any()
        return ordered, incoming, used

    def validate_execution(self, path, config, requested_outputs, diagnostics, depth=0, budget=None, prefix=None, overrides=None):
        config = options.resolve_configuration(config, overrides or {}, self.label(path) + "#" + config["id"], sys.modules[__name__], self)
        if budget is None:
            budget = [0]
        _require(depth < MAX_DEPTH, 'Capacity', self.label(path), 'expanded subgraph depth exceeded')
        prefix = self.assets[path]['id'] if prefix is None else prefix
        first_issue = len(diagnostics.errors)
        ordered, incoming, used = self.execution_order(path, config, requested_outputs, diagnostics)
        for issue in diagnostics.errors[first_issue:]:
            issue.target['instance'] = prefix + '/' + issue.where.rsplit('/', 1)[-1]
        for node in ordered:
            budget[0] += 1
            _require(budget[0] <= MAX_EXPANDED_NODES, 'Capacity', self.label(path), 'expanded node capacity exceeded')
            where = self.label(path) + '#' + config['id'] + '/' + node['id']
            if node['kind'] == 'Upscale':
                first_issue = len(diagnostics.errors)
                with diagnostics.capture():
                    upscale.reject_execution(node, where, sys.modules[__name__])
                for issue in diagnostics.errors[first_issue:]:
                    issue.target['instance'] = prefix + '/' + node['id']
            # Pipeline profile checks depend on declarations, not resource versions.
            # Check independent retained nodes before starting transactional lowering.
            if node['kind'] in pipeline.KINDS:
                required = {port['id'] for port in node['inputs'] if port['required']}
                required.update(port['target'] for port in node['outputs'])
                if required <= incoming[node['id']].keys():
                    first_issue = len(diagnostics.errors)
                    with diagnostics.capture():
                        _lower_pipeline(node, where)
                    for issue in diagnostics.errors[first_issue:]:
                        issue.target['instance'] = prefix + '/' + node['id']
            if node['kind'] == 'Subgraph':
                child_path = self.inside(node['asset'], node['id'])
                child = _configuration(self.assets[child_path], node['configuration'], node['id'])
                outputs = {port['id'] for port in child['outputs'] if node['id'] + '.' + port['id'] in used}
                self.validate_execution(child_path, child, outputs, diagnostics, depth + 1, budget, prefix + '/' + node['id'], node.get('overrides', {}))

    def verify_snapshot(self):
        for path, data in self.bytes.items():
            _require(_read(path) == data, "SourceChanged", self.label(path),
                     "graph source changed during validation; retry with a consistent snapshot")


@dataclass(frozen=True)
class _Value:
    type: str
    origin: str | None = None
    resource: str | None = None
    version: int = 0
    defined: bool = False
    producer: str | None = None
    cameras: tuple[str, ...] = ()
    extent: tuple[int, ...] = ()
    scale: tuple[int, ...] = ()
    scene_data: int = scene_bundle.ALL_DATA


class _Compiler:
    def __init__(self, library):
        self.library = library
        self.passes = []
        self.resources = {}
        self.writers = set()
        self.latest = {}
        self.roots = []
        self.cameras = []
        self.techniques = set()
        self.expanded = 0
        self.instances = {}
        self.activity = []

    def add_pass(self, entry):
        _require(len(self.passes) < MAX_PASSES, "Capacity", entry["id"], "serial pass capacity exceeded")
        self.passes.append(entry)

    def write(self, value, node, aspect, where, camera, *, port=None, field=None):
        load, store = node[aspect + "_load"], node[aspect + "_store"]
        _require(load != "Load" or value.defined, "Initialization", where,
                 f"{aspect} Load reads undefined {value.resource}@{value.version}. Choose Clear, or connect a preceding pass that stores initialized contents.",
                 port=port or aspect, direction='input', field=field,
                 expected='initialized resource version', actual=f'undefined {value.resource}@{value.version}',
                 repair='Choose Clear for the writable resource, or connect a preceding pass that stores initialized contents.')
        key = (value.resource, value.version)
        _require(key not in self.writers, "MultipleWriters", where,
                 f"multiple writes branch from {value.resource}@{value.version}; connect the preceding version")
        self.writers.add(key)
        defined = store == "Store" and (load == "Clear" or (load == "Load" and value.defined))
        cameras = tuple(sorted(set((*value.cameras, *camera)))) if load == "Load" else camera
        result = replace(value, version=value.version + 1, defined=defined, producer=where, cameras=cameras)
        self.latest[value.resource] = result.version
        return result

    def expand(self, path, config, prefix, bindings, depth=0, requested_outputs=None, overrides=None):
        config = options.resolve_configuration(config, overrides or {}, prefix, sys.modules[__name__], self.library)
        _require(depth < MAX_DEPTH, "Capacity", prefix, "expanded subgraph depth exceeded")
        self.instances[prefix] = (self.library.label(path), config["id"], {n["id"] for n in config["nodes"]})
        if requested_outputs is None:
            requested_outputs = {port['id'] for port in config['outputs']}
        order, incoming, used = self.library.execution_order(path, config, requested_outputs)
        # Authoritative activity for this exact caller/output/override context. Retained IDs
        # share the existing expanded-node budget; no second graph walk or runtime schedule.
        self.activity.append({'instance': prefix, 'source': self.library.label(path),
                              'configuration': config['id'], 'retained': [node['id'] for node in order]})
        values = {"input." + key: value for key, value in bindings.items()}
        for node in order:
            self.expanded += 1
            where = prefix + "/" + node["id"]
            _require(self.expanded <= MAX_EXPANDED_NODES, "Capacity", where, "expanded node capacity exceeded")
            inputs = {key: values[endpoint] for key, endpoint in incoming[node["id"]].items()}
            outputs = {}
            kind = node["kind"]
            authored_pipeline = None
            output_mapping = None
            if kind == 'Upscale':
                upscale.reject_execution(node, where, sys.modules[__name__])
            if kind in pipeline.KINDS:
                inputs = scene_bundle.resolve(node, inputs, where, sys.modules[__name__])
                authored_pipeline = node
                node, input_mapping, output_mapping = _lower_pipeline(node, where)
                _require(set(input_mapping.values()) <= inputs.keys(), 'UnboundInput', where,
                         'the selected graphics invocation requires all mapped inputs')
                inputs = {name: inputs[port] for name, port in input_mapping.items()}
                kind = node['kind']
            if kind == "Scene":
                outputs["scene"] = _Value("Scene", origin=where)
                outputs.update({part: _Value(kind, origin=where, scene_data=1 << scene_bundle.PARTS.index(part))
                                for part, kind in scene_bundle.TYPES.items()})
            elif kind == 'StructuredBuffer':
                self.resources[where] = {'type': 'Buffer', 'stride': node['stride'], 'count': node['count']}
                self.latest[where] = 0
                outputs['buffer'] = _Value('Buffer', origin=where, resource=where, defined=True)
            elif kind == "Buffer":
                capacity = node.get('bytes', 0)
                if 'bytes_per_pixel' in node:
                    capacity = inputs['size'].extent[0] * inputs['size'].extent[1] * node['bytes_per_pixel']
                    _require(capacity <= ray.MAX_BUFFER_BYTES, 'Capacity', where,
                             'pixel buffer exceeds storage capacity; reduce the connected RenderSize scale')
                self.resources[where] = {'type': 'Buffer', 'format': 'Bytes', 'extent': [capacity, 1],
                                         'samples': 1, 'imported': False, 'initialized': True, 'presentation': False}
                if 'bytes_per_pixel' in node:
                    self.resources[where]['bytes_per_pixel'] = node['bytes_per_pixel']
                    if inputs['size'].scale:
                        self.resources[where]['view_scale'] = list(inputs['size'].scale)
                self.latest[where] = 0
                outputs['buffer'] = _Value('Buffer', origin=where, resource=where, defined=True)
            elif kind == "Target":
                self.resources[where] = {key: node[key] for key in
                    ("type", "format", "extent", "samples", "imported", "initialized", "presentation")}
                if node['extent'] == 'RenderSize':
                    self.resources[where]['extent'] = list(inputs['size'].extent)
                    if inputs['size'].scale:
                        self.resources[where]['view_scale'] = list(inputs['size'].scale)
                self.latest[where] = 0
                outputs["texture"] = _Value(node["type"], origin=where, resource=where, defined=node["initialized"])
            elif kind in sizes.KINDS:
                extent, scale = sizes.resolve(node, inputs, self.resources, where, sys.modules[__name__])
                outputs['size'] = _Value('RenderSize', origin=where, extent=extent, scale=scale)
            elif kind == "Camera":
                camera = {"id": where, "active": node["active"], "role": node["role"],
                          "target": inputs["target"].resource if "target" in inputs else None}
                if node["active"]:
                    target = self.resources[camera["target"]]
                    _require(target["presentation"] == (node["role"] == "Presentation"),
                             "CameraTarget", where, "camera role does not match its designated target")
                self.cameras.append(camera)
                outputs["view"] = _Value("View", origin=where, cameras=(where,) if node["active"] else ())
            elif kind == "RayTracing":
                target = self.resources[inputs['color'].resource]
                is_compute = authored_pipeline['kind'] == compute.SOURCE_KIND
                label = 'scene compute pass' if is_compute else 'ray pipeline'
                _require(inputs['view'].cameras, 'CameraTarget', where, f'{label} requires an active camera',
                         port=input_mapping['view'], direction='input', expected='active camera View', actual='inactive camera View',
                         repair='Enable Active on the connected camera and give it a route to Export or Present.')
                _require(target['format'] == 'RGBA8Unorm' and target['samples'] == 1 and not target['presentation'],
                         'Capability', where, f'{label} requires an offscreen single-sample RGBA8Unorm target',
                         port=input_mapping['color'], direction='input', expected='offscreen RGBA8Unorm, samples=1',
                         actual=f"{target['format']}, samples={target['samples']}, presentation={target['presentation']}",
                         repair='Connect an offscreen single-sample RGBA8Unorm color target.')
                dimensions = authored_pipeline['invocation']['dimensions']
                expected_dimensions = [*target['extent'], 1]
                _require(dimensions == 'ColorTarget' or (not target.get('view_scale') and dimensions == expected_dimensions),
                         'AttachmentMismatch', where,
                         (f"Color target '{inputs['color'].resource}' is view-relative; explicit launch dimensions {dimensions} "
                          'cannot follow view changes. Choose Match color target in Invocation.' if target.get('view_scale') else
                          f"{'Scene compute' if is_compute else 'Ray'} launch dimensions are {dimensions}, but color target '{inputs['color'].resource}' "
                          f"requires {expected_dimensions}. Choose Match color target in Invocation, or set dimensions to {expected_dimensions}."),
                         field='invocation.dimensions', expected='ColorTarget' if target.get('view_scale') else expected_dimensions,
                         actual=dimensions, repair='Choose Match color target in Invocation → Launch size.')
                visibility_reads = {}
                if 'visibility' in inputs:
                    for aspect in ('visibility', 'visibility_depth'):
                        source_value = inputs[aspect]
                        resource = self.resources[source_value.resource]
                        _require(source_value.defined and resource['extent'] == target['extent'] and
                                 resource.get('view_scale') == target.get('view_scale') and resource['samples'] == 1,
                                 'AttachmentMismatch', where, 'visibility/depth must be initialized and match the ray extent')
                        visibility_reads[aspect] = {'resource': source_value.resource, 'input_version': source_value.version}
                structured = [port['id'] for port in authored_pipeline['inputs'] if port['type'] == 'Buffer' and
                              'stride' in self.resources[inputs['buffer:' + port['id']].resource]]
                _require(not is_compute or not structured, 'Capability', where,
                         f"Scene compute input '{structured[0] if structured else ''}' is connected to a StructuredBuffer. "
                         'Scene compute passes share render-graph Buffer nodes through GraphBuffer0/1; '
                         'replace the StructuredBuffer with a Buffer node (bytes or bytes_per_pixel).',
                         port=structured[0] if structured else None, direction='input', expected='Buffer', actual='StructuredBuffer',
                         repair='Connect a Buffer node instead of a StructuredBuffer.')
                buffer_uses = []
                buffer_outputs = {}
                for slot, port in enumerate(port for port in authored_pipeline['inputs'] if port['type'] == 'Buffer'):
                    identity = 'buffer:' + port['id']
                    if 'stride' in self.resources[inputs[identity].resource]:
                        continue
                    output = next((port for port in authored_pipeline['outputs'] if port['target'] == input_mapping[identity]), None)
                    _require(output is not None, 'Capability', where, 'each persistent Buffer input requires a paired Load/Store Buffer output')
                    written = self.write(inputs[identity], {identity + '_load': 'Load', identity + '_store': 'Store'},
                                         identity, where, ())
                    buffer_uses.append({'resource': written.resource, 'slot': slot,
                                        'input_version': inputs[identity].version, 'output_version': written.version})
                    buffer_outputs[output['id']] = written
                _require(len({use['resource'] for use in buffer_uses}) == len(buffer_uses), 'MultipleWriters', where,
                         'one physical buffer cannot occupy both buffer inputs of a pass')
                value = self.write(inputs['color'], node, 'color', where, inputs['view'].cameras)
                storage = {}
                storage_outputs = {}
                for name in sorted(set(inputs) - {'scene', 'view', 'color', 'visibility', 'visibility_depth'}):
                    buffer = inputs[name]
                    if 'stride' not in self.resources[buffer.resource]:
                        continue
                    _require(buffer.defined, 'Initialization', where, 'storage reads require defined input contents')
                    _require(buffer.resource not in {use['resource'] for use in storage.values()},
                             'MultipleWriters', where, 'one physical buffer cannot occupy two shader ports')
                    writable = name in output_mapping
                    updated = buffer
                    if writable:
                        _require(self.latest[buffer.resource] == buffer.version, 'ResourceVersion', where,
                                 'storage write must consume the latest buffer version')
                        self.latest[buffer.resource] += 1
                        updated = _Value('Buffer', origin=where, resource=buffer.resource,
                                         version=self.latest[buffer.resource], defined=True, producer=where)
                        storage_outputs[output_mapping[name]] = updated
                    storage[input_mapping[name]] = {'resource': buffer.resource, 'input_version': buffer.version,
                                     'output_version': updated.version, 'access': 'ReadWrite' if writable else 'Read'}
                _require(not buffer_uses or not storage, 'Capability', where, 'Buffer and StructuredBuffer storage profiles cannot share a pass')
                self.techniques.add(ray.TECHNIQUE)
                self.add_pass({'id': where, 'kind': 'RayTracing', 'configuration': 'RayTracing',
                    'technique': ray.TECHNIQUE, 'source': self.library.label(path), 'asset': self.library.assets[path]['id'],
                    'asset_configuration': config['id'], 'vertex_layout': 'RendererStatic', 'material': 'DefaultOpaque',
                    'scene': inputs['scene'].origin, 'scene_data': inputs['scene'].scene_data, 'view': inputs['view'].origin, 'cameras': list(inputs['view'].cameras),
                    'dependencies': sorted({v.producer for v in inputs.values() if v.producer}), 'pipeline': authored_pipeline,
                    'dimensions': expected_dimensions, 'persistent_buffers': buffer_uses, 'buffers': storage, **visibility_reads,
                    'attachments': {'color': {'resource': value.resource, 'input_version': inputs['color'].version,
                        'output_version': value.version, 'defined': value.defined, 'load': 'Clear', 'store': 'Store'}}})
                if is_compute:
                    # Only the offline plan names the source; the numeric pass record stays RayTracing (design D1).
                    self.passes[-1]['source_kind'] = compute.SOURCE_KIND
                outputs = {output_mapping['color']: value, **buffer_outputs, **storage_outputs}
            elif kind == "Upscale":
                source = inputs['color']
                destination = inputs['destination']
                label = upscale.ALGORITHMS[node['algorithm']][0]
                _require(source.defined, 'Initialization', where,
                         f'{label} color must be produced by a preceding pass that stores initialized contents.',
                         port='color', direction='input')
                _require(source.version == self.latest[source.resource], 'StaleVersion', where,
                         f'{label} color must consume the latest stored source version.', port='color', direction='input')
                _require(source.resource != destination.resource, 'AttachmentMismatch', where,
                         f'{label} color and destination must be distinct textures.', port='destination', direction='input')
                _require(len(source.cameras) == 1, 'Capability', where,
                         f'{label} requires one rendered camera route on its color input.')
                for port, value in inputs.items():
                    target = self.resources[value.resource]
                    _require(target['format'] == 'RGBA8Unorm' and target['samples'] == 1 and
                             (port != 'color' or not target['presentation']), 'Capability', where,
                             f'{label} supports single-sample RGBA8 color; its source must be offscreen.',
                             port=port, direction='input')
                if node['algorithm'] == 'metalfx-spatial':
                    source_extent = self.resources[source.resource]['extent']
                    output_extent = self.resources[destination.resource]['extent']
                    _require(all(output >= render for output, render in zip(output_extent, source_extent)),
                             'Capability', where, 'MetalFX Spatial output dimensions must be at least the render dimensions.')
                value = self.write(destination, {'color_load': 'Clear', 'color_store': 'Store'},
                                   'color', where, source.cameras)
                self.add_pass({'id': where, 'kind': 'Upscale', 'algorithm': node['algorithm'],
                    'color_processing': node.get('color_processing', 'Linear'),
                    'source': self.library.label(path), 'asset': self.library.assets[path]['id'],
                    'asset_configuration': config['id'], 'view': source.cameras[0], 'cameras': list(source.cameras),
                    'dependencies': sorted({v.producer for v in inputs.values() if v.producer}),
                    'source_color': {'resource': source.resource, 'input_version': source.version},
                    'attachments': {'color': {'resource': value.resource, 'input_version': destination.version,
                        'output_version': value.version, 'defined': value.defined, 'load': 'Clear', 'store': 'Store'}}})
                outputs['color'] = value
            elif kind == "Accumulate":
                source = inputs['color']
                producer = next((operation for operation in self.passes if operation['id'] == source.producer), None)
                _require(producer is not None and producer['kind'] == 'RayTracing', 'Capability', where,
                         'Accumulate requires a ray color output. Connect a RayTracingPipeline or scene compute color output; raster and already accumulated images are unsupported.')
                _require('accumulation' not in producer, 'MultipleWriters', where,
                         'A ray output can feed only one Accumulate node. Chain consumers after its color output.')
                value = self.write(source, {'color_load': 'Load', 'color_store': 'Store'}, 'color', where, source.cameras)
                producer['accumulation'] = {'id': where, 'samples': node['samples']}
                self.add_pass({'id': where, 'kind': 'Accumulate', 'samples': node['samples'],
                    'source': self.library.label(path), 'asset': self.library.assets[path]['id'],
                    'asset_configuration': config['id'], 'dependencies': [source.producer],
                    'attachments': {'color': {'resource': value.resource, 'input_version': source.version,
                        'output_version': value.version, 'defined': value.defined, 'load': 'Load', 'store': 'Store'}}})
                outputs['color'] = value
            elif kind == "Denoise":
                source = inputs['color']
                producer = next((operation for operation in self.passes if operation['id'] == source.producer), None)
                if producer is not None and producer['kind'] == 'Accumulate':
                    producer = next(operation for operation in self.passes if operation['id'] == producer['dependencies'][0])
                _require(producer is not None and producer['kind'] == 'RayTracing', 'Capability', where,
                         'Denoise requires linear ray color from a RayTracingPipeline or scene compute pass, optionally through Accumulate, before ToneMap. Raster, already denoised and display-encoded inputs are unsupported.')
                _require('denoising' not in producer, 'MultipleWriters', where,
                         'A ray output can feed only one Denoise node. Chain consumers after its color output.')
                value = self.write(source, {'color_load': 'Load', 'color_store': 'Store'}, 'color', where, source.cameras)
                producer['denoising'] = {'id': where, 'quality': node['quality']}
                self.add_pass({'id': where, 'kind': 'Denoise', 'quality': node['quality'],
                    'source': self.library.label(path), 'asset': self.library.assets[path]['id'],
                    'asset_configuration': config['id'], 'dependencies': [source.producer],
                    'attachments': {'color': {'resource': value.resource, 'input_version': source.version,
                        'output_version': value.version, 'defined': value.defined, 'load': 'Load', 'store': 'Store'}}})
                outputs['color'] = value
            elif kind == "ToneMap":
                source = inputs['color']
                producer = next((operation for operation in self.passes if operation['id'] == source.producer), None)
                while producer is not None and producer['kind'] in ('Accumulate', 'Denoise'):
                    producer = next(operation for operation in self.passes if operation['id'] == producer['dependencies'][0])
                curve = node.get('curve', 'Reinhard')
                # A raster producer is admitted when its color attachment is this exact version; the package build
                # then requires the clustered deferred profile, whose lighting resolve applies the conversion.
                is_raster = (producer is not None and producer['kind'] == 'Raster' and
                             producer['configuration'] == 'ColorAndDepth' and
                             producer['attachments'].get('color', {}).get('output_version') == source.version and
                             producer['attachments']['color']['resource'] == source.resource)
                _require(producer is not None and (producer['kind'] == 'RayTracing' or is_raster), 'Capability', where,
                         'ToneMap requires a ray color output from a RayTracingPipeline or scene compute pass, optionally through Accumulate, or the color output of a ColorAndDepth raster pass; already tone-mapped images are unsupported.')
                _require('tone_mapping' not in producer, 'MultipleWriters', where,
                         'A ray or raster output can feed only one ToneMap node. Chain consumers after its color output.')
                value = self.write(source, {'color_load': 'Load', 'color_store': 'Store'}, 'color', where, source.cameras)
                producer['tone_mapping'] = {'id': where, 'exposure': node['exposure'], 'curve': curve}
                self.add_pass({'id': where, 'kind': 'ToneMap', 'exposure': node['exposure'], 'curve': curve,
                    'source': self.library.label(path), 'asset': self.library.assets[path]['id'],
                    'asset_configuration': config['id'], 'dependencies': [source.producer],
                    'attachments': {'color': {'resource': value.resource, 'input_version': source.version,
                        'output_version': value.version, 'defined': value.defined, 'load': 'Load', 'store': 'Store'}}})
                outputs['color'] = value
            elif kind == "Raster":
                _require(inputs["view"].cameras, "CameraTarget", where, "raster requires an active camera",
                         port=input_mapping['view'] if authored_pipeline is not None else 'view', direction='input',
                         expected='active camera View', actual='inactive camera View',
                         repair='Enable Active on the connected camera and give it a route to Export or Present.')
                if node["mode"] == "ColorAndDepth":
                    color, depth_target = (self.resources[inputs[aspect].resource] for aspect in ("color", "depth"))
                    _require((color['extent'], color['samples'], color.get('view_scale')) == (depth_target['extent'], depth_target['samples'], depth_target.get('view_scale')),
                             "AttachmentMismatch", where,
                             f"Color target '{inputs['color'].resource}' has extent {color['extent']} and {color['samples']} samples; "
                             f"depth target '{inputs['depth'].resource}' has extent {depth_target['extent']} and {depth_target['samples']} samples. "
                             f"Size policies: color {color.get('view_scale', 'Fixed')}, depth {depth_target.get('view_scale', 'Fixed')}. "
                             'This raster operation requires equal attachment extents and samples. Connect both targets to the same RenderSize, or use matching fixed sizes.',
                             port=input_mapping['depth'] if authored_pipeline is not None else 'depth', direction='input',
                             expected=f"extent={color['extent']}, samples={color['samples']}, size policy={color.get('view_scale', 'Fixed')}",
                             actual=f"extent={depth_target['extent']}, samples={depth_target['samples']}, size policy={depth_target.get('view_scale', 'Fixed')}",
                             repair='Connect both attachments to the same RenderSize, or use matching fixed sizes and samples.')
                for aspect in ("depth", "color") if node["mode"] == "ColorAndDepth" else ("depth",):
                    outputs[aspect] = self.write(inputs[aspect], node, aspect, where, inputs["view"].cameras,
                        port=input_mapping[aspect] if authored_pipeline is not None else aspect,
                        field=None if authored_pipeline is not None else aspect + '_load')
                technique = MATERIALS[node["material"]] + "." + node["mode"]
                self.techniques.add(technique)
                entry = {"id": where, "kind": "Raster", "configuration": node["mode"],
                         "technique": technique, "source": self.library.label(path), "asset": self.library.assets[path]["id"],
                         "asset_configuration": config["id"], "vertex_layout": node["vertex_layout"],
                         "material": node["material"], "scene": inputs["scene"].origin,
                         "scene_data": inputs["scene"].scene_data,
                         "view": inputs["view"].origin, "cameras": list(inputs["view"].cameras),
                         "dependencies": sorted({v.producer for v in inputs.values() if v.producer}),
                         "attachments": {aspect: {"resource": value.resource, "input_version": inputs[aspect].version,
                            "output_version": value.version, "defined": value.defined,
                            "load": node[aspect + "_load"], "store": node[aspect + "_store"]}
                            for aspect, value in outputs.items()}}
                if authored_pipeline is not None:
                    for port in authored_pipeline['outputs']:
                        resource = self.resources[inputs[next(aspect for aspect, name in output_mapping.items() if name == port['id'])].resource]
                        _require(resource['format'] == port['format'] and
                                 resource['samples'] == authored_pipeline['state']['samples'],
                                 'AttachmentMismatch', where,
                                 f"Output '{port['id']}' declares {port['format']}/{authored_pipeline['state']['samples']} samples, "
                                 f"but input '{port['target']}' supplies {resource['format']}/{resource['samples']} samples. "
                                 'Make the writable resource Format and Pipeline State samples match the connected target, or connect a matching target.')
                    entry['pipeline'] = authored_pipeline
                    outputs = {output_mapping[aspect]: value for aspect, value in outputs.items()}
                self.add_pass(entry)
            elif kind == "Subgraph":
                child_path = self.library.inside(node["asset"], where)
                child = self.library.assets[child_path]
                selected = _configuration(child, node["configuration"], where)
                requested = {port['id'] for port in selected['outputs'] if node['id'] + '.' + port['id'] in used}
                outputs = self.expand(child_path, selected, where, inputs, depth + 1, requested, node.get("overrides", {}))
            else:
                value = inputs["texture" if kind == "Export" else "color"]
                self.root(where, kind, value)
                self.add_pass({"id": where, "kind": kind, "source": self.library.label(path),
                               "asset": self.library.assets[path]["id"], "asset_configuration": config["id"],
                               "dependencies": [value.producer] if value.producer else [],
                               "resource": value.resource, "version": value.version})
            values.update({node["id"] + "." + key: value for key, value in outputs.items()})
        return {p["id"]: values[p["source"]] for p in config["outputs"] if p['id'] in requested_outputs}

    def root(self, where, kind, value):
        _require(value.type in TEXTURES, 'TypeMismatch', where,
                 f'{kind} requires a texture; {value.type} is a graph value. Expose RenderSize through a subgraph interface and export a rendered texture.')
        _require(value.defined, "Initialization", where,
                 f"{kind} reads undefined {value.resource}@{value.version}")
        if kind == "Present":
            _require(self.resources[value.resource]["presentation"], "CameraTarget", where,
                     "Present requires the written version of an imported presentation target")
            _require(not any(r["kind"] == "Present" and r["resource"] == value.resource for r in self.roots),
                     "MultipleWriters", where, "presentation target has multiple Present operations")
        self.roots.append({"id": where, "kind": kind, "resource": value.resource,
                           "version": value.version, "cameras": list(value.cameras)})

    def finish(self):
        for average in (operation for operation in self.passes if operation['kind'] in ('Accumulate', 'ToneMap', 'Denoise')):
            source = average['attachments']['color']
            for operation in self.passes:
                if operation['id'] == average['id']:
                    continue
                reads = [*operation.get('attachments', {}).values(),
                         *(operation[key] for key in ('visibility', 'visibility_depth', 'source_color') if key in operation)]
                _require(not any(read['resource'] == source['resource'] and
                                 read['input_version'] == source['input_version'] for read in reads),
                         'Capability', average['id'],
                         f"{average['kind']} owns its producer's color version. Connect other readers after its output, or use a separate target for raw output.")
        for root in self.roots:
            _require(root["version"] == self.latest[root["resource"]], "StaleVersion", root["id"],
                     "export/presentation must consume the final physical resource version")
        for camera in self.cameras:
            if camera["active"]:
                kind = "Present" if camera["role"] == "Presentation" else "Export"
                _require(any(root["kind"] == kind and root["resource"] == camera["target"] and
                             camera["id"] in root["cameras"] for root in self.roots),
                         "CameraRouteMissing", camera["id"],
                         f"active camera has no rendered route to {kind} on {camera['target']}")
        _require(self.roots, "UnboundOutput", "root graph",
                 "an execution graph needs an enabled Export or Present root",
                 expected='enabled Export or Present root', actual='none',
                 repair='Enable a disabled Export node, or add and connect an Export node.')
        # A target consulted only for sizing supplies metadata, not a GPU allocation.
        used_resources = {root['resource'] for root in self.roots}
        used_resources.update(attachment['resource'] for operation in self.passes
                              for attachment in (*operation.get('attachments', {}).values(),
                                                 *(operation[key] for key in ('visibility', 'visibility_depth', 'source_color') if key in operation)))
        used_resources.update(use['resource'] for operation in self.passes for use in operation.get('persistent_buffers', []))
        used_resources.update(use['resource'] for operation in self.passes for use in operation.get('buffers', {}).values())
        self.resources = {identity: resource for identity, resource in self.resources.items() if identity in used_resources}


def _library(path, root):
    path = _resolve(path, str(path))
    library = _Library(Path(root) if root is not None else path.parent)
    try:
        library.load(path)
        diagnostics = DiagnosticBatch()
        for source, asset in library.assets.items():
            for config in asset['configurations']:
                with diagnostics.capture():
                    resolved = options.resolve_configuration(config, {}, library.label(source) + "#" + config["id"], sys.modules[__name__], library)
                    library.order(source, resolved)
        diagnostics.raise_if_any()
    except ToolingError as error:
        raise _locate(error, library)
    return library, path


def load_library(path, *, root=None):
    """Validate all declared interfaces/configurations and recursive imports.

    Reads only package-contained bounded UTF-8 JSON files and verifies the source
    snapshot before returning an interface report. Resource initialization and
    camera routes require concrete root bindings and are checked by compile_graph.
    Failure raises a stable ToolingError; no files or prior plans are modified.
    """
    library, path = _library(path, root)
    asset = library.assets[path]
    try:
        library.verify_snapshot()
    except ToolingError as error:
        raise _locate(error, library)
    return {"version": 1, "stage": "RG0", "asset": asset["id"], "default": asset["default"],
            "validation": "InterfacesOnly", "configurations": [
                {"id": c["id"], "inputs": c["inputs"], "outputs": c["outputs"]}
                for c in asset["configurations"]]}


def compile_graph(path, *, configuration=None, root=None):
    """Validate/lower one closed root graph into a deterministic RG0 plan.

    Root interface inputs must be bound by a containing graph, so a root execution
    configuration has no inputs. Imports select explicit authored configurations.
    The plan validates initialization, versions and active camera routes; it does
    not compile shaders, check scene coverage/device capabilities or submit GPU
    work. Returns copied diagnostic records only after all checks/source snapshots
    succeed. Failures leave sources and previously returned plans unchanged.
    """
    library, path = _library(path, root)
    asset = library.assets[path]
    selected = asset["default"] if configuration is None else configuration
    compiler = _Compiler(library)
    try:
        _identifier(selected, library.label(path))
        config = _configuration(asset, selected, library.label(path))
        _require(not config["inputs"], "UnboundInput", library.label(path),
                 "root interface inputs require a containing graph; use --library to inspect the interface")
        diagnostics = DiagnosticBatch()
        library.validate_execution(path, config, {port['id'] for port in config['outputs']}, diagnostics)
        diagnostics.raise_if_any()
        outputs = compiler.expand(path, config, asset["id"], {})
        for key, value in outputs.items():
            compiler.root(asset["id"] + "/output/" + key, "Export", value)
        compiler.finish()
        library.verify_snapshot()
    except ToolingError as error:
        raise _locate(error, library, compiler.instances)
    semantic = []
    for source in sorted(library.assets, key=library.label):
        document = library.assets[source]
        semantic.append({"source": library.label(source), "document": {
            **document, "configurations": [{k: v for k, v in c.items() if k != "layout"}
                                          for c in document["configurations"]]}})
    identity = {"documents": semantic, "configuration": selected}
    return {"version": PLAN_VERSION, "stage": "RG0", "execution": "ContractOnly", "asset": asset["id"],
            "configuration": selected, "selection": "Default" if configuration is None else "Explicit",
            "semantic_digest": hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
            "dependencies": [{"source": library.label(p), "sha256": hashlib.sha256(library.bytes[p]).hexdigest()}
                             for p in sorted(library.bytes, key=library.label)],
            "source": library.label(path), "activity": compiler.activity,
            "passes": compiler.passes, "resources": compiler.resources, "roots": compiler.roots,
            "cameras": compiler.cameras, "techniques": sorted(compiler.techniques),
            "checks_pending": ["ShaderBuild", "DeviceCapabilities", "SceneCoverage", "GpuExecution"]}


def dump_plan(plan):
    """Format the validated plan as readable text; includes source/instance IDs."""
    lines = [f"RG0 serial plan {plan['asset']}#{plan['configuration']} (contract only)"]
    for ordinal, entry in enumerate(plan["passes"]):
        detail = " " + entry["configuration"] if entry["kind"] == "Raster" else ""
        lines.append(f"{ordinal:03} {entry['id']} {entry['kind']}{detail} [{entry['source']}#{entry['asset_configuration']}]")
        if entry["kind"] == "Raster":
            lines.append(f"    scene: {entry['scene']}; view: {entry['view']}; technique: {entry['technique']}")
        if entry['kind'] == 'Upscale':
            source = entry['source_color']
            lines.append(f"    bilinear source: {source['resource']}@{source['input_version']}")
        if entry['kind'] == 'Accumulate':
            lines.append(f"    compensated linear RGB mean; samples: {entry['samples']}; reset: camera/scene/extent/shaders")
        if entry['kind'] == 'Denoise':
            lines.append(f"    OIDN RT HDR; quality: {entry['quality']}; before display conversion")
        if entry['kind'] == 'ToneMap':
            lines.append(f"    {entry.get('curve', 'Reinhard')} + sRGB; exposure multiplier: {entry['exposure']}")
        if entry['kind'] == 'RayTracing':
            lines.append(f"    launch: {entry['dimensions']}; color target: {entry['attachments']['color']['resource']}")
            if 'source_kind' in entry:
                lines.append(f"    source: {entry['source_kind']} (scene compute pass in the ray sequence)")
        for name, use in entry.get('buffers', {}).items():
            lines.append(f"    {name}: {use['resource']}@{use['input_version']} -> @{use['output_version']} ({use['access']})")
        for aspect, attachment in entry.get("attachments", {}).items():
            lines.append(f"    {aspect}: {attachment['resource']}@{attachment['input_version']} -> "
                         f"@{attachment['output_version']} {attachment['load']}/{attachment['store']} "
                         f"defined={str(attachment['defined']).lower()}")
        if "resource" in entry:
            lines.append(f"    {entry['resource']}@{entry['version']}")
        if entry["dependencies"]:
            lines.append("    after: " + ", ".join(entry["dependencies"]))
    for root in plan["roots"]:
        lines.append(f"Root {root['id']}: {root['kind']} {root['resource']}@{root['version']}")
    lines.append("Authored techniques: " + ", ".join(plan["techniques"]))
    lines.append("Pending: " + ", ".join(plan["checks_pending"]))
    return "\n".join(lines)


def _target(identity, kind, presentation=False):
    return {"id": identity, "kind": "Target", "type": kind, "format": FORMATS[kind][0],
            "extent": [640, 480], "samples": 1, "imported": presentation,
            "initialized": False, "presentation": presentation}


def _raster_asset():
    configs = []
    for mode in MODES:
        node = {"id": "draw", "kind": "Raster", "mode": mode, "vertex_layout": "RendererStatic",
                "material": "DefaultOpaque", "depth_load": "Clear", "depth_store": "Store"}
        inputs = [{"id": "scene", "type": "Scene"}, {"id": "view", "type": "View"},
                  {"id": "depth", "type": "DepthTexture"}]
        outputs = [{"id": "depth", "type": "DepthTexture", "source": "draw.depth", "required": mode == "DepthOnly"}]
        if mode == "ColorAndDepth":
            node.update(color_load="Clear", color_store="Store")
            inputs.append({"id": "color", "type": "ColorTexture"})
            outputs.append({"id": "color", "type": "ColorTexture", "source": "draw.color", "required": False})
        configs.append({"id": mode, "inputs": inputs, "outputs": outputs, "nodes": [node],
                        "edges": [{"from": "input." + p["id"], "to": "draw." + p["id"]} for p in inputs]})
    return {"version": 1, "id": "RasterizeGraph", "default": "ColorAndDepth", "configurations": configs}


def _root_asset(output):
    depth_only = output == "depth"
    presentation = output == "presentation"
    kind = "DepthTexture" if depth_only else "ColorTexture"
    nodes = [{"id": "scene", "kind": "Scene"},
             {"id": "camera", "kind": "Camera", "active": True,
              "role": "Presentation" if presentation else "Offscreen", "target_type": kind},
             _target("depth", "DepthTexture"),
             {"id": "raster", "kind": "Subgraph", "asset": "rasterize.lurender.json",
              "configuration": "DepthOnly" if depth_only else "ColorAndDepth"},
             {"id": "output", "kind": "Present"} if presentation else
             {"id": "output", "kind": "Export", "type": kind}]
    edges = [{"from": "scene.scene", "to": "raster.scene"},
             {"from": "camera.view", "to": "raster.view"},
             {"from": "depth.texture", "to": "raster.depth"}]
    if not depth_only:
        nodes.insert(2, _target("color", "ColorTexture", presentation))
        edges.append({"from": "color.texture", "to": "raster.color"})
    aspect = "depth" if depth_only else "color"
    edges += [{"from": aspect + ".texture", "to": "camera.target"},
              {"from": "raster." + aspect, "to": "output.color" if presentation else "output.texture"}]
    return {"version": 1, "id": "Main", "default": "Default", "configurations": [
        {"id": "Default", "inputs": [], "outputs": [], "nodes": nodes, "edges": edges}]}


def new_workspace(destination, *, output="presentation", renderer_static=False, profiles=None, template="raster"):
    """Publish an editable built-in render graph template in a new directory.

    template selects raster (the default), forward-lit, restir-di, visibility-buffer,
    visibility-secondary-rays, tiled-vbuffer-nee, compute-ray-tracing, simple-compute-ray-tracing,
    veach-guibas-mis, line-sampling, ris-projected-solid-angle, salesin-jarosz-lines,
    polygonal-light-sampling, slice-sampling-luminaire, steinberg-plt, steinberg-physical-optics or restir-gi. Offscreen
    Renderer raster creation copies the engine default package, including its shaders and shared
    view-relative RenderSize controls. Other Renderer templates also use offscreen output.
    output is presentation, offscreen or depth. Creation never overwrites an
    existing directory; staged files are validated before publication and cleaned
    up on failure. This is graph source creation, not game-project/SDK setup.
    renderer_static also stages the Renderer-compatible manifest and editable
    shader functions for explicitly selected profiles in the same transaction.
    Returns the saved root path; performs no downloads, shader builds or GPU work.
    """
    from .templates import TemplateFile, stage_project, publish_project, discard_staging

    _enum(output, ("presentation", "offscreen", "depth"), "new graph")
    _require(renderer_static == (profiles is not None), "Schema", "new graph",
             "Renderer sources require --renderer-static and explicit --profiles together")
    from .render_graph_templates import TEMPLATES, template_files
    _enum(template, TEMPLATES, "new graph template")
    destination = Path(destination).absolute()
    files = [TemplateFile("rasterize.lurender.json", canonical_asset(_raster_asset()).decode()),
             TemplateFile("main.lurender.json", canonical_asset(_root_asset(output)).decode())]
    should_copy_template = template != "raster" or (renderer_static and output == "offscreen")
    if should_copy_template:
        _require(renderer_static and output == "offscreen", "Capability", template,
                 "this template creates an offscreen Renderer graph; select --renderer-static --output offscreen and explicit --profiles")
        files = [TemplateFile(path, text) for path, text in template_files(template, profiles, sys.modules[__name__]).items()]
    staged = stage_project(files, destination)
    try:
        compile_graph(staged.staging / "main.lurender.json")
        if renderer_static and not should_copy_template:
            from .render_graph_build import new_build_sources
            new_build_sources(staged.staging / "main.lurender.json",
                              renderer_static=True, profiles=profiles)
        publish_project(staged)
    except BaseException:
        discard_staging(staged)
        raise
    return destination / "main.lurender.json"
