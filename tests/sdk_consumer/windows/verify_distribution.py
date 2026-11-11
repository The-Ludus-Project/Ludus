"""Package the installed Release SDK with checked docs; never publish it."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/python"))
from ludus_tools.package_verify import file_digest
from ludus_tools.release_model import canonical, read_json
from ludus_tools.windows_distribution import create_distribution, verify_distribution


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--docs", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--redist", type=Path)
    parser.add_argument("--redist-version")
    args = parser.parse_args()
    manifest = args.sdk / "share/Ludus/LudusSdkManifest.json"
    identity = read_json(manifest)
    agility = "lib/cmake/Ludus/agility"
    dependencies = identity["dependencies"]
    agility_version = next(item["version"] for item in dependencies if item["name"] == "Microsoft.Direct3D.D3D12")
    spec = {"schemaVersion": 1, "kind": "sdk", "version": identity["version"],
            "revision": identity["source_revision"], "channel": "internal", "symbols": {},
            "license": "share/Ludus/licenses/LICENSE", "requiredFiles": [
                "lib/cmake/Ludus/LudusWindowsDistribution.cmake", "lib/cmake/Ludus/LudusConfig.cmake"],
            "dependencies": [{"name": "Microsoft.Direct3D.D3D12", "version": agility_version,
                              "license": agility + "/LICENSE.txt", "files": [
                                  agility + "/D3D12Core.dll", agility + "/d3d12SDKLayers.dll"]}]}
    if args.redist:
        if not args.redist_version:
            parser.error("--redist-version is required with --redist")
        spec["redistributable"] = {"path": str(args.redist.resolve()), "sha256": file_digest(args.redist),
                                   "version": args.redist_version,
                                   "license": "share/Ludus/licenses/THIRD_PARTY_NOTICES.md"}
    spec_path = args.output.parent / "sdk-distribution-spec.json"
    spec_path.parent.mkdir(parents=True, exist_ok=True)
    spec_path.write_bytes(canonical(spec))
    created = create_distribution(args.sdk, args.output, spec_path, sdk_manifest=manifest, docs=args.docs)
    result = verify_distribution(created / "windows.zip")
    if result["kind"] != "sdk" or result["sdk"]["source_revision"] != identity["source_revision"]:
        raise RuntimeError("packaged SDK identity differs from the candidate")
    # A stable report for CI artifact review, not a qualification assertion.
    (created / "acceptance.json").write_text(json.dumps({"checks": ["installed-sdk", "clean-extraction"],
        "archiveSha256": file_digest(created / "windows.zip"), "qualification": result["qualification"]}, indent=2) + "\n")


if __name__ == "__main__":
    main()
