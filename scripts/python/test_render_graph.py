"""RG0 interface, import, resource-version and installed CLI regressions."""

import copy
from contextlib import redirect_stdout, redirect_stderr
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from ludus_tools import cli, render_graph as graph
from ludus_tools.errors import ToolingError


class RenderGraphTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name) / "graphs"
        self.path = graph.new_workspace(self.directory)

    def read(self, name="main.lurender.json"):
        return json.loads((self.directory / name).read_text())

    def write(self, value, name="main.lurender.json"):
        (self.directory / name).write_text(json.dumps(value))

    def config(self, value):
        return value["configurations"][0]

    def node(self, value, identity):
        return next(n for n in self.config(value)["nodes"] if n["id"] == identity)

    def fails(self, code):
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(self.path)
        self.assertEqual("RenderGraph" + code, failure.exception.code)
        return failure.exception.message

    def test_default_presentation_plan_and_read_only_validation(self):
        before = {p.name: p.read_bytes() for p in self.directory.iterdir()}
        plan = graph.compile_graph(self.path)
        self.assertEqual([("Main/raster/draw", "Raster"), ("Main/output", "Present")],
                         [(p["id"], p["kind"]) for p in plan["passes"]])
        self.assertEqual(["DefaultStaticOpaque.ColorAndDepth"], plan["techniques"])
        self.assertEqual("Main/color", plan["roots"][0]["resource"])
        self.assertEqual(["Main/camera"], plan["roots"][0]["cameras"])
        self.assertEqual(1, plan["roots"][0]["version"])
        self.assertEqual("ContractOnly", plan["execution"])
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.directory.iterdir()})
        text = graph.dump_plan(plan)
        self.assertIn("rasterize.lurender.json#ColorAndDepth", text)
        self.assertIn("Clear/Store defined=true", text)
        self.assertIn("SceneCoverage", text)

    def test_same_saved_raster_asset_for_offscreen_and_present(self):
        other = graph.new_workspace(self.directory.parent / "offscreen", output="offscreen")
        self.assertEqual((self.directory / "rasterize.lurender.json").read_bytes(),
                         (other.parent / "rasterize.lurender.json").read_bytes())
        present = graph.compile_graph(self.path)
        offscreen = graph.compile_graph(other)
        self.assertEqual(present["techniques"], offscreen["techniques"])
        self.assertEqual("Export", offscreen["roots"][0]["kind"])
        self.assertFalse(offscreen["resources"]["Main/color"]["presentation"])

    def test_explicit_depth_only_has_no_color_attachment(self):
        path = graph.new_workspace(self.directory.parent / "depth", output="depth")
        plan = graph.compile_graph(path)
        self.assertEqual(["DefaultStaticOpaque.DepthOnly"], plan["techniques"])
        self.assertEqual({"depth"}, set(plan["passes"][0]["attachments"]))
        self.assertEqual("Main/depth", plan["roots"][0]["resource"])

    def test_disconnected_color_does_not_synthesize_a_depth_shader(self):
        value = self.read()
        camera = self.node(value, "camera")
        camera.update(role="Offscreen", target_type="DepthTexture")
        self.node(value, "color").update(imported=False, presentation=False)
        self.node(value, "output").update(kind="Export", type="DepthTexture")
        for edge in self.config(value)["edges"]:
            if edge["to"] == "camera.target":
                edge["from"] = "depth.texture"
            if edge["to"] == "output.color":
                edge.update({"from": "raster.depth", "to": "output.texture"})
        self.write(value)
        plan = graph.compile_graph(self.path)
        self.assertEqual(["DefaultStaticOpaque.ColorAndDepth"], plan["techniques"])
        self.assertEqual({"color", "depth"}, set(plan["passes"][0]["attachments"]))
        self.assertEqual("ColorAndDepth", plan["passes"][0]["configuration"])

    def test_library_reports_interfaces_and_cannot_execute_with_unbound_inputs(self):
        path = self.directory / "rasterize.lurender.json"
        report = graph.load_library(path)
        self.assertEqual("InterfacesOnly", report["validation"])
        self.assertEqual(["ColorAndDepth", "DepthOnly"], [c["id"] for c in report["configurations"]])
        self.assertEqual(["depth"], [p["id"] for p in report["configurations"][1]["outputs"]])
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(path)
        self.assertEqual("RenderGraphUnboundInput", failure.exception.code)

    def test_root_configuration_selection_is_explicit_and_bounded(self):
        value = self.read()
        other = copy.deepcopy(self.config(value))
        other["id"] = "Alternate"
        value["configurations"].append(other)
        self.write(value)
        plan = graph.compile_graph(self.path, configuration="Alternate")
        self.assertEqual("Alternate", plan["configuration"])
        self.assertEqual("Explicit", plan["selection"])
        with self.assertRaises(ToolingError) as failure:
            graph.compile_graph(self.path, configuration="MadeUp")
        self.assertEqual("RenderGraphConfiguration", failure.exception.code)

    def test_missing_child_configuration_is_not_inferred(self):
        value = self.read()
        self.node(value, "raster")["configuration"] = "Auto"
        self.write(value)
        self.fails("Configuration")

    def test_required_output_consumer_policy(self):
        child = self.read("rasterize.lurender.json")
        self.config(child)["outputs"][0]["required"] = True
        self.write(child, "rasterize.lurender.json")
        self.fails("UnusedRequiredOutput")

    def test_invalid_unselected_configuration_is_rejected(self):
        child = self.read("rasterize.lurender.json")
        child["configurations"][1]["nodes"][0]["mode"] = "Inferred"
        self.write(child, "rasterize.lurender.json")
        self.fails("Schema")

    def test_type_mismatch_has_nested_source_context(self):
        child = self.read("rasterize.lurender.json")
        child["configurations"][1]["edges"][0]["to"] = "draw.view"
        self.write(child, "rasterize.lurender.json")
        message = self.fails("TypeMismatch")
        self.assertIn("rasterize.lurender.json#DepthOnly", message)
        self.assertIn("Scene", message)
        self.assertIn("View", message)

    def test_unknown_port_and_unavailable_color_output(self):
        value = self.read()
        self.node(value, "raster")["configuration"] = "DepthOnly"
        self.write(value)
        self.fails("Port")

    def test_missing_input(self):
        value = self.read()
        self.config(value)["edges"] = [e for e in self.config(value)["edges"] if e["to"] != "raster.scene"]
        self.write(value)
        self.fails("UnboundInput")

    def test_duplicate_connection(self):
        value = self.read()
        self.config(value)["edges"].append(self.config(value)["edges"][0])
        self.write(value)
        self.fails("MultipleWriters")

    def test_execution_cycle(self):
        value = self.read()
        for edge in self.config(value)["edges"]:
            if edge["to"] == "raster.color":
                edge["from"] = "raster.color"
        self.write(value)
        self.fails("Cycle")

    def test_recursive_import(self):
        child = self.read("rasterize.lurender.json")
        self.config(child)["nodes"] = [{"id": "again", "kind": "Subgraph",
            "asset": "rasterize.lurender.json", "configuration": "ColorAndDepth"}]
        self.write(child, "rasterize.lurender.json")
        self.fails("Recursion")

    def test_import_path_and_symlink_boundaries(self):
        value = self.read()
        for invalid in ("../outside.json", "/outside.json", "https://host/graph", "C:\\graph.json"):
            with self.subTest(invalid=invalid):
                self.node(value, "raster")["asset"] = invalid
                self.write(value)
                self.fails("Source")
        outside = self.directory.parent / "external.json"
        outside.write_bytes((self.directory / "rasterize.lurender.json").read_bytes())
        (self.directory / "link.json").symlink_to(outside)
        self.node(value, "raster")["asset"] = "link.json"
        self.write(value)
        self.fails("Source")

    def test_missing_source_and_symlink_loop_report_stable_errors(self):
        value = self.read()
        self.node(value, "raster")["asset"] = "missing.json"
        self.write(value)
        self.fails("Source")
        (self.directory / "loop.json").symlink_to("loop.json")
        self.node(value, "raster")["asset"] = "loop.json"
        self.write(value)
        self.fails("Source")

    def test_unknown_fields_duplicate_json_and_nonfinite_values(self):
        original = self.read()
        value = copy.deepcopy(original)
        value["specialize_from_connections"] = True
        self.write(value)
        self.fails("Schema")
        self.path.write_text('{"version":1,"version":1}')
        self.fails("Schema")
        self.path.write_text('{"version": NaN}')
        self.fails("Schema")

    def test_error_diagnostics_and_programmatic_field_names_are_bounded(self):
        value = self.read()
        value["x" * 10000] = 1
        self.write(value)
        message = self.fails("Schema")
        self.assertLessEqual(len(message.encode()), 4096)
        self.assertIn("truncated", message)
        value = self.read()
        del value["x" * 10000]
        value[7] = "invalid"
        with self.assertRaises(ToolingError) as failure:
            graph.canonical_asset(value)
        self.assertEqual("RenderGraphSchema", failure.exception.code)

    def test_version_bool_identity_and_encoding_failures(self):
        for version in (3, True):
            value = self.read()
            value["version"] = version
            self.write(value)
            self.fails("Version")
        self.path.write_bytes(b"\xff")
        self.fails("Schema")

    def test_duplicate_node_and_asset_identities(self):
        value = self.read()
        self.config(value)["nodes"].append(copy.deepcopy(self.node(value, "scene")))
        self.write(value)
        self.fails("DuplicateIdentity")
        self.path = graph.new_workspace(self.directory.parent / "other")
        self.directory = self.path.parent
        child = self.read("rasterize.lurender.json")
        child["id"] = "Main"
        self.write(child, "rasterize.lurender.json")
        self.fails("DuplicateIdentity")

    def test_load_undefined_and_partial_discard_do_not_initialize(self):
        child = self.read("rasterize.lurender.json")
        for load in ("Load", "Discard"):
            with self.subTest(load=load):
                self.config(child)["nodes"][0]["color_load"] = load
                self.write(child, "rasterize.lurender.json")
                self.fails("Initialization")

    def test_discard_store_invalidates_even_a_clear(self):
        child = self.read("rasterize.lurender.json")
        self.config(child)["nodes"][0]["color_store"] = "Discard"
        self.write(child, "rasterize.lurender.json")
        self.fails("Initialization")

    def test_only_imports_can_claim_existing_initialized_contents(self):
        value = self.read()
        self.node(value, "depth")["initialized"] = True
        self.write(value)
        self.fails("Initialization")

    def test_extent_and_sample_contracts(self):
        value = self.read()
        self.node(value, "depth")["extent"] = [100, 100]
        self.write(value)
        self.fails("AttachmentMismatch")
        self.node(value, "depth")["samples"] = 4
        self.write(value)
        self.fails("Capacity")

    def test_active_camera_needs_rendered_route_not_just_initialized_target(self):
        value = self.read()
        color = self.node(value, "color")
        color["initialized"] = True
        self.config(value)["nodes"] = [n for n in self.config(value)["nodes"] if n["id"] != "raster"]
        self.config(value)["edges"] = [{"from": "color.texture", "to": "camera.target"},
                                       {"from": "color.texture", "to": "output.color"}]
        self.write(value)
        self.fails("CameraRouteMissing")

    def test_inactive_camera_needs_no_route_but_cannot_drive_a_raster(self):
        value = self.read()
        self.node(value, "camera")["active"] = False
        self.write(value)
        self.fails("CameraTarget")
        value = self.read()
        self.config(value)["nodes"].append({"id": "inactive", "kind": "Camera", "active": False,
                                          "role": "Presentation", "target_type": "ColorTexture"})
        self.node(value, "camera")["active"] = True
        self.write(value)
        plan = graph.compile_graph(self.path)
        self.assertEqual(1, len(plan["cameras"]))

    def test_present_requires_designated_imported_surface(self):
        value = self.read()
        self.node(value, "color").update(imported=False, presentation=False)
        self.node(value, "camera")["role"] = "Offscreen"
        self.write(value)
        self.fails("CameraTarget")

    def test_camera_route_must_reach_its_designated_physical_target(self):
        value = self.read()
        other = copy.deepcopy(self.node(value, "color"))
        other["id"] = "other"
        self.config(value)["nodes"].append(other)
        for edge in self.config(value)["edges"]:
            if edge["to"] == "camera.target":
                edge["from"] = "other.texture"
        self.write(value)
        self.fails("CameraRouteMissing")

    def test_graph_root_needs_an_export_or_present(self):
        value = self.read()
        self.config(value)["nodes"] = [self.node(value, "scene")]
        self.config(value)["edges"] = []
        self.write(value)
        self.fails("UnboundOutput")

    def test_node_layout_does_not_change_semantic_identity(self):
        before = graph.compile_graph(self.path)
        value = self.read()
        self.config(value)["layout"] = {"positions": [{"node": "raster", "x": 55, "y": -25}]}
        self.write(value)
        after = graph.compile_graph(self.path)
        self.assertEqual(before["semantic_digest"], after["semantic_digest"])
        self.assertEqual(before["passes"], after["passes"])
        self.assertNotEqual(before["dependencies"], after["dependencies"])

    def test_wire_routes_preserve_semantics_and_canonical_roundtrip(self):
        before = graph.compile_graph(self.path)
        value = self.read()
        edge = self.config(value)['edges'][0]
        self.config(value)['layout'] = dict(positions=[], routes=[dict(
            **edge, points=[dict(x=-32768, y=32768), dict(x=100, y=200)])])
        self.assertEqual(json.loads(graph.canonical_asset(value)), value)
        self.write(value)
        after = graph.compile_graph(self.path)
        self.assertEqual(before['semantic_digest'], after['semantic_digest'])
        self.assertEqual(before['passes'], after['passes'])

    def test_wire_route_schema_rejects_invalid_geometry_and_connections(self):
        from ludus_tools import render_graph_layout as policy
        original = self.read()
        edge = self.config(original)['edges'][0]
        route = dict(**edge, points=[dict(x=0, y=0)])
        for change in (dict(points=[]), dict(points=None), dict(points=[dict(x=True, y=0)]),
                       dict(points=[dict(x=0.5, y=0)]), dict(points=[dict(x=policy.MAX_COORDINATE + 1, y=0)]),
                       dict(points=[dict(x=0, y=0, unknown=0)]),
                       dict(points=[dict(x=0, y=0)] * (policy.MAX_WIRE_REROUTE_POINTS + 1)),
                       dict(to='missing.color'), dict(to='#outputs.missing'), dict(unknown=0)):
            with self.subTest(change=change):
                value = copy.deepcopy(original)
                self.config(value)['layout'] = dict(positions=[], routes=[route | change])
                with self.assertRaises(ToolingError):
                    graph.canonical_asset(value)
        for routes in ([route, route], [route] * (policy.MAX_WIRE_ROUTES + 1), None):
            value = copy.deepcopy(original)
            self.config(value)['layout'] = dict(positions=[], routes=routes)
            with self.assertRaises(ToolingError):
                graph.canonical_asset(value)

    def test_comment_frames_preserve_semantics_and_canonical_roundtrip(self):
        before = graph.compile_graph(self.path)
        value = self.read()
        frame = dict(id="stage", title="Lighting 🌟", notes="<b>Plain text</b>\nSecond line",
                     colour="#547FA3", x=-100, y=-100, width=800, height=400)
        self.config(value)["layout"] = dict(positions=[], frames=[frame])
        self.assertEqual(json.loads(graph.canonical_asset(value)), value)
        self.write(value)
        after = graph.compile_graph(self.path)
        self.assertEqual(before["semantic_digest"], after["semantic_digest"])
        self.assertEqual(before["passes"], after["passes"])
        frame['collapsed'] = True
        self.assertEqual(json.loads(graph.canonical_asset(value)), value)
        self.write(value)
        collapsed = graph.compile_graph(self.path)
        self.assertEqual(before["semantic_digest"], collapsed["semantic_digest"])
        self.assertEqual(before["passes"], collapsed["passes"])

    def test_comment_frame_schema_is_bounded_and_rejects_malformed_metadata(self):
        from ludus_tools import render_graph_layout as policy
        original = dict(id="stage", title="Visibility", notes="", colour="#547FA3",
                        x=0, y=0, width=800, height=400)
        changes = [dict(title=" "), dict(title="first\nsecond"),
                   dict(title="🌟" * (policy.MAX_COMMENT_TITLE_CODEPOINTS + 1)),
                   dict(notes="a" * (policy.MAX_COMMENT_NOTES_CODEPOINTS + 1)),
                   dict(colour="red"), dict(colour="#FF000080"), dict(x=0.5), dict(x=True),
                   dict(width=policy.MIN_COMMENT_WIDTH-1), dict(height=policy.MIN_COMMENT_HEIGHT-1),
                   dict(x=policy.MAX_COORDINATE), dict(id="bad.id"), dict(extra=True),
                   dict(collapsed=1), dict(collapsed="true"), dict(collapsed=None)]
        for change in changes:
            with self.subTest(change=change):
                value = self.read()
                self.config(value)["layout"] = dict(positions=[], frames=[original | change])
                with self.assertRaises(ToolingError):
                    graph.canonical_asset(value)
        for frames in ([original, original], [original] * (policy.MAX_COMMENT_FRAMES + 1), None):
            value = self.read()
            self.config(value)["layout"] = dict(positions=[], frames=frames)
            with self.assertRaises(ToolingError):
                graph.canonical_asset(value)
        value = self.read()
        self.config(value)["layout"] = dict(positions=[], frames=[original | dict(title="🌟" * policy.MAX_COMMENT_TITLE_CODEPOINTS)])
        graph.canonical_asset(value)

    def test_activity_provenance_includes_retained_values_and_excludes_pruned_nodes(self):
        value = self.read()
        unused = copy.deepcopy(self.node(value, 'color'))
        unused['id'] = 'unused'
        self.config(value)['nodes'].append(unused)
        self.write(value)
        plan = graph.compile_graph(self.path)
        contexts = {item['instance']: item for item in plan['activity']}
        self.assertEqual(plan['source'], 'main.lurender.json')
        self.assertEqual(contexts['Main']['source'], 'main.lurender.json')
        self.assertIn('scene', contexts['Main']['retained'])
        self.assertIn('camera', contexts['Main']['retained'])
        self.assertIn('raster', contexts['Main']['retained'])
        self.assertNotIn('unused', contexts['Main']['retained'])
        self.assertEqual(contexts['Main/raster']['retained'], ['draw'])
        self.assertLessEqual(sum(len(item['retained']) for item in plan['activity']), graph.MAX_EXPANDED_NODES)

    def test_activity_provenance_respects_requested_outputs_per_shared_instance(self):
        target = self.node(self.read(), 'color')
        target.update(imported=True, presentation=False, initialized=True)
        targets = [dict(target, id=identity) for identity in ('a', 'b')]
        child = dict(version=2, id='Choice', default='Default', configurations=[dict(
            id='Default', inputs=[], outputs=[dict(id=identity, type='ColorTexture',
                source=identity+'.texture', required=False) for identity in ('a', 'b')], nodes=targets, edges=[])])
        self.write(child, 'choice.lurender.json')
        value = dict(version=2, id='Main', default='Default', configurations=[dict(
            id='Default', inputs=[], outputs=[], nodes=[
                dict(id=identity, kind='Subgraph', asset='choice.lurender.json', configuration='Default')
                for identity in ('first', 'second')] + [dict(id='out_'+identity, kind='Export', type='ColorTexture')
                for identity in ('a', 'b')], edges=[dict(**{'from': 'first.a', 'to': 'out_a.texture'}),
                                                 dict(**{'from': 'second.b', 'to': 'out_b.texture'})])])
        self.write(value)
        plan = graph.compile_graph(self.path)
        contexts = {item['instance']: item for item in plan['activity']}
        self.assertEqual(contexts['Main/first']['retained'], ['a'])
        self.assertEqual(contexts['Main/second']['retained'], ['b'])
        self.assertEqual(contexts['Main/first']['source'], contexts['Main/second']['source'])

    def test_plan_is_deterministic_after_relocation_and_repeat(self):
        before = graph.compile_graph(self.path)
        other = graph.new_workspace(self.directory.parent / "relocated")
        self.assertEqual(before, graph.compile_graph(other))
        self.assertEqual(before, graph.compile_graph(self.path))

    def test_nested_wrapper_preserves_instance_and_source_identity(self):
        child = self.read("rasterize.lurender.json")
        config = copy.deepcopy(child["configurations"][0])
        config["nodes"] = [{"id": "inner", "kind": "Subgraph", "asset": "rasterize.lurender.json",
                            "configuration": "ColorAndDepth"}]
        config["edges"] = [{"from": "input." + p["id"], "to": "inner." + p["id"]} for p in config["inputs"]]
        for output in config["outputs"]:
            output["source"] = "inner." + output["id"]
        wrapper = {"version": 1, "id": "Wrapper", "default": "ColorAndDepth", "configurations": [config]}
        self.write(wrapper, "wrapper.json")
        value = self.read()
        self.node(value, "raster")["asset"] = "wrapper.json"
        self.write(value)
        plan = graph.compile_graph(self.path)
        self.assertEqual("Main/raster/inner/draw", plan["passes"][0]["id"])
        self.assertEqual("rasterize.lurender.json", plan["passes"][0]["source"])
        self.assertEqual(3, len(plan["dependencies"]))

    def test_two_instances_share_a_technique_but_cannot_branch_physical_writes(self):
        value = self.read()
        second = copy.deepcopy(self.node(value, "raster"))
        second["id"] = "second"
        self.config(value)["nodes"].insert(-1, second)
        self.config(value)["edges"] += [{"from": e["from"], "to": "second." + e["to"].split(".")[1]}
                                      for e in list(self.config(value)["edges"]) if e["to"].startswith("raster.")]
        self.config(value)['nodes'].append({'id': 'depth_output', 'kind': 'Export', 'type': 'DepthTexture'})
        self.config(value)['edges'].append({'from': 'second.depth', 'to': 'depth_output.texture'})
        self.write(value)
        self.fails("MultipleWriters")
        self.config(value)['nodes'] = [node for node in self.config(value)['nodes'] if node['id'] != 'depth_output']
        self.config(value)['edges'] = [edge for edge in self.config(value)['edges'] if edge['to'] != 'depth_output.texture']
        for edge in self.config(value)["edges"]:
            if edge["to"] in ("second.color", "second.depth"):
                edge["from"] = "raster." + edge["to"].split(".")[1]
            if edge["to"] == "output.color":
                edge["from"] = "second.color"
        self.write(value)
        plan = graph.compile_graph(self.path)
        self.assertEqual(["Main/raster/draw", "Main/second/draw", "Main/output"],
                         [p["id"] for p in plan["passes"]])
        self.assertEqual(1, len(plan["techniques"]))
        self.assertEqual(2, plan["roots"][0]["version"])

    def test_stale_export_of_an_overwritten_resource(self):
        value = self.read()
        second = copy.deepcopy(self.node(value, "raster"))
        second["id"] = "second"
        self.config(value)["nodes"].append(second)
        self.config(value)["edges"] += [
            {"from": "scene.scene", "to": "second.scene"}, {"from": "camera.view", "to": "second.view"},
            {"from": "raster.color", "to": "second.color"}, {"from": "raster.depth", "to": "second.depth"}]
        self.config(value)['nodes'].append({'id': 'depth_output', 'kind': 'Export', 'type': 'DepthTexture'})
        self.config(value)['edges'].append({'from': 'second.depth', 'to': 'depth_output.texture'})
        self.write(value)
        self.fails("StaleVersion")

    def test_two_cameras_require_explicit_load_to_preserve_both_routes(self):
        value = self.read()
        self.config(value)["nodes"].insert(-1, {"id": "second_camera", "kind": "Camera", "active": True,
            "role": "Presentation", "target_type": "ColorTexture"})
        self.config(value)["nodes"].insert(-1, {"id": "second", "kind": "Subgraph",
            "asset": "rasterize.lurender.json", "configuration": "ColorAndDepth"})
        self.config(value)["edges"] += [
            {"from": "color.texture", "to": "second_camera.target"},
            {"from": "scene.scene", "to": "second.scene"},
            {"from": "second_camera.view", "to": "second.view"},
            {"from": "raster.color", "to": "second.color"},
            {"from": "raster.depth", "to": "second.depth"}]
        for edge in self.config(value)["edges"]:
            if edge["to"] == "output.color":
                edge["from"] = "second.color"
        self.write(value)
        self.fails("CameraRouteMissing")
        child = self.read("rasterize.lurender.json")
        child["id"] = "RasterizeLoad"
        self.config(child)["nodes"][0]["color_load"] = "Load"
        self.write(child, "load.json")
        self.node(value, "second")["asset"] = "load.json"
        self.write(value)
        plan = graph.compile_graph(self.path)
        self.assertEqual(["Main/camera", "Main/second_camera"], plan["roots"][0]["cameras"])
        self.assertEqual(1, len(plan["techniques"]))
        self.assertEqual("Load", plan["passes"][1]["attachments"]["color"]["load"])

    def test_root_interface_outputs_are_explicit_export_roots(self):
        path = graph.new_workspace(self.directory.parent / "export", output="offscreen")
        self.directory, self.path = path.parent, path
        value = self.read()
        self.config(value)["nodes"] = [n for n in self.config(value)["nodes"] if n["id"] != "output"]
        self.config(value)["edges"] = [e for e in self.config(value)["edges"] if e["to"] != "output.texture"]
        self.config(value)["outputs"] = [{"id": "image", "type": "ColorTexture", "source": "raster.color", "required": True}]
        self.write(value)
        plan = graph.compile_graph(self.path)
        self.assertEqual("Main/output/image", plan["roots"][0]["id"])
        self.assertIn("Root Main/output/image: Export Main/color@1", graph.dump_plan(plan))

    def test_untrusted_path_text_and_deep_json_fail_without_tracebacks(self):
        value = self.read()
        self.node(value, "raster")["asset"] = "bad\u0000path"
        self.write(value)
        self.fails("Source")
        self.path.write_text("[" * 2000 + "0" + "]" * 2000)
        self.fails("Schema")

    def test_source_change_rejects_candidate_and_preserves_previous_plan(self):
        before = graph.compile_graph(self.path)
        original = graph._read
        reads = 0

        def changed(path):
            nonlocal reads
            reads += 1
            data = original(path)
            return data + b" " if reads > 2 else data

        with patch.object(graph, "_read", changed):
            self.fails("SourceChanged")
        self.assertEqual(before, graph.compile_graph(self.path))

    def test_source_and_expansion_capacity(self):
        with patch.object(graph, "MAX_ASSET_BYTES", 16):
            self.fails("Capacity")
        with patch.object(graph, "MAX_TOTAL_BYTES", 128):
            self.fails("Capacity")
        with patch.object(graph, "MAX_EXPANDED_NODES", 1):
            self.fails("Capacity")
        with patch.object(graph, "MAX_PASSES", 1):
            self.fails("Capacity")

    def test_new_never_overwrites_and_failed_validation_cleans_staging(self):
        before = {p.name: p.read_bytes() for p in self.directory.iterdir()}
        with self.assertRaises(ToolingError):
            graph.new_workspace(self.directory)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.directory.iterdir()})
        destination = self.directory.parent / "bad"
        with patch.object(graph, "compile_graph", side_effect=ToolingError("Injected", "fail")):
            with self.assertRaises(ToolingError):
                graph.new_workspace(destination)
        self.assertFalse(destination.exists())
        self.assertFalse(list(self.directory.parent.glob(".bad.*.staging")))

    def test_renderer_creation_publishes_complete_sources_with_cli_equivalence(self):
        destination = self.directory.parent / "editor"
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(0, cli.main(["--json", "render-graph", "new", str(destination),
                             "--output", "offscreen", "--renderer-static", "--profiles", "metal", "wgsl"]))
        self.assertEqual(str(destination / "main.lurender.json"), json.loads(output.getvalue())["created"])
        manifest = json.loads((destination / "main.lushader.json").read_text())
        self.assertEqual(["metal", "wgsl"], manifest["profiles"])
        self.assertEqual("RendererStaticBuild", manifest["id"])
        for program in manifest["programs"]:
            self.assertTrue((destination / program["source"]).is_file())
        plan = graph.compile_graph(destination / "main.lurender.json")
        self.assertEqual("Export", plan["roots"][0]["kind"])
        separate = graph.new_workspace(self.directory.parent / "separate", output="offscreen")
        from ludus_tools.render_graph_build import new_build_sources
        new_build_sources(separate, renderer_static=True, profiles=["metal", "wgsl"])
        self.assertEqual({p.relative_to(destination): p.read_bytes() for p in destination.rglob("*") if p.is_file()},
                         {p.relative_to(separate.parent): p.read_bytes() for p in separate.parent.rglob("*") if p.is_file()})

    def test_renderer_creation_failure_never_publishes_partial_graph(self):
        from ludus_tools import render_graph_build
        destination = self.directory.parent / "failed-renderer"
        with patch.object(render_graph_build, "new_build_sources", side_effect=ToolingError("Injected", "fail")):
            with self.assertRaises(ToolingError):
                graph.new_workspace(destination, output="offscreen", renderer_static=True, profiles=["metal"])
        self.assertFalse(destination.exists())
        self.assertFalse(list(destination.parent.glob(".failed-renderer.*.staging")))
        for options in ({"renderer_static": True}, {"profiles": ["metal"]}):
            with self.assertRaises(ToolingError):
                graph.new_workspace(destination, **options)
        self.assertFalse(destination.exists())

    def test_cli_json_success_failure_and_library(self):
        for action, extra, field in (("validate", [], "plan"), ("dump", [], "plan"),
                                    ("validate", ["--library"], "library")):
            with self.subTest(action=action, extra=extra):
                output = io.StringIO()
                with redirect_stdout(output):
                    result = cli.main(["--json", "render-graph", action, str(self.path), *extra])
                self.assertEqual(0, result)
                self.assertIn(field, json.loads(output.getvalue()))
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(io.StringIO()):
            result = cli.main(["--json", "render-graph", "validate", str(self.path), "--configuration", "Undeclared"])
        self.assertEqual(1, result)
        self.assertEqual("RenderGraphConfiguration", json.loads(output.getvalue())["error"]["code"])

    def test_cli_runs_as_module_without_qt_or_engine(self):
        script = "from ludus_tools import render_graph; import sys; assert not any(n.startswith(('PyQt', 'PySide')) or n == 'engine' for n in sys.modules)"
        subprocess.run([sys.executable, "-c", script], check=True, capture_output=True)
        result = subprocess.run([sys.executable, "-m", "ludus_tools", "--json", "render-graph", "dump", str(self.path)],
                                check=True, text=True, capture_output=True)
        self.assertEqual("ContractOnly", json.loads(result.stdout)["plan"]["execution"])


if __name__ == "__main__":
    unittest.main()
