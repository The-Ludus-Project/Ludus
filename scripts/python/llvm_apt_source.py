"""Register the apt.llvm.org package source for the pinned LLVM major on Ubuntu.

Ubuntu 24.04's archive does not package the pinned LLVM, so CI, the devcontainer
and ``./init.sh`` install it from apt.llvm.org. The archive signing key is
committed as ``config/apt/apt.llvm.org.asc`` and checked against
``KEY_FINGERPRINT`` before apt trusts it; nothing downloads a key at setup time.
The source is limited to the pinned ``llvm-toolchain-<codename>-<major>`` suite.

Thanks to the LLVM project, "LLVM Debian/Ubuntu nightly packages"
(https://apt.llvm.org/), for the suite layout and the archive signing key.

Usage: ``python3 scripts/python/llvm_apt_source.py`` (uses sudo when not root).
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KEY_SOURCE = Path("config/apt/apt.llvm.org.asc")
# Published on https://apt.llvm.org/ for llvm-snapshot.gpg.key.
KEY_FINGERPRINT = "6084F3CF814B57C1CF12EFD515CF4D18AF4F7421"
KEYRING = Path("/etc/apt/keyrings/ludus-apt.llvm.org.asc")
SOURCE_LIST = Path("/etc/apt/sources.list.d/ludus-apt.llvm.org.sources")


class SourceError(RuntimeError):
    """The pinned apt.llvm.org source cannot be registered."""


def openpgp_fingerprint(armored: str) -> str:
    """Return the v4 fingerprint of the first public-key packet in an ASCII-armored key.

    Thanks to J. Callas et al., RFC 4880 "OpenPGP Message Format", sections
    4.2 (packet headers) and 12.2 (v4 fingerprint: SHA-1 of 0x99, a two-octet
    length and the public-key packet body).
    """
    lines = armored.splitlines()
    try:
        start = lines.index("-----BEGIN PGP PUBLIC KEY BLOCK-----") + 1
    except ValueError as error:
        raise SourceError("the key is not an ASCII-armored OpenPGP public key") from error
    body = []
    in_headers = True
    for line in lines[start:]:
        if in_headers:
            if line.strip() == "":
                in_headers = False
            elif ":" not in line:
                in_headers = False
                body.append(line.strip())
            continue
        if line.startswith("=") or line.startswith("-----END"):
            break
        body.append(line.strip())
    data = base64.b64decode("".join(body))
    if len(data) < 2 or data[0] & 0x80 == 0:
        raise SourceError("the key does not start with an OpenPGP packet")
    if data[0] & 0x40:
        tag = data[0] & 0x3F
        first = data[1]
        if first < 192:
            header, length = 2, first
        elif first < 224:
            header, length = 3, ((first - 192) << 8) + data[2] + 192
        else:
            header, length = 6, int.from_bytes(data[2:6], "big")
    else:
        tag = (data[0] >> 2) & 0x0F
        width = (1, 2, 4)[data[0] & 0x03]
        header, length = 1 + width, int.from_bytes(data[1:1 + width], "big")
    packet = data[header:header + length]
    if tag != 6 or len(packet) != length or not packet or packet[0] != 4:
        raise SourceError("the key's first packet is not a version 4 public key")
    return hashlib.sha1(b"\x99" + length.to_bytes(2, "big") + packet).hexdigest().upper()


def source_text(codename: str, llvm_major: int) -> str:
    """Return the deb822 source stanza for the pinned LLVM suite."""
    return (
        "Types: deb\n"
        f"URIs: https://apt.llvm.org/{codename}/\n"
        f"Suites: llvm-toolchain-{codename}-{llvm_major}\n"
        "Components: main\n"
        f"Signed-By: {KEYRING}\n"
    )


def pinned_llvm_major(root: Path) -> int:
    """Read the LLVM major from ``minimum.clang`` in ``config/tool_versions.json``."""
    versions = json.loads((root / "config/tool_versions.json").read_text(encoding="utf-8"))
    major = str(versions["minimum"]["clang"]).split(".", 1)[0]
    if not major.isdigit():
        raise SourceError("config/tool_versions.json must pin minimum.clang as a version")
    return int(major)


def os_release_codename(path: Path = Path("/etc/os-release")) -> str:
    """Return ``VERSION_CODENAME`` (or ``UBUNTU_CODENAME``) from os-release, or an empty string."""
    values = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            key, separator, value = line.partition("=")
            if separator:
                values[key] = value.strip().strip('"')
    return values.get("UBUNTU_CODENAME") or values.get("VERSION_CODENAME", "")


def privilege_prefix() -> list[str]:
    """Return an empty prefix as root, otherwise ``sudo``."""
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        return []
    sudo = shutil.which("sudo")
    if not sudo:
        raise SourceError("registering an apt source needs sudo or a root shell")
    return [sudo]


def register(root: Path, codename: str, llvm_major: int, prefix: list[str]) -> None:
    """Install the verified key and the pinned suite's source file; the caller runs ``apt-get update``."""
    key = (root / KEY_SOURCE).read_text(encoding="ascii")
    fingerprint = openpgp_fingerprint(key)
    if fingerprint != KEY_FINGERPRINT:
        raise SourceError(f"{KEY_SOURCE} has fingerprint {fingerprint}, expected {KEY_FINGERPRINT}")
    if not codename:
        raise SourceError("could not determine the Ubuntu codename from /etc/os-release")
    print(f"Registering third-party apt source apt.llvm.org (llvm-toolchain-{codename}-{llvm_major}, "
          f"key {KEY_FINGERPRINT}).", flush=True)
    with tempfile.TemporaryDirectory(prefix="ludus-llvm-apt-") as temporary:
        staged = Path(temporary) / SOURCE_LIST.name
        staged.write_text(source_text(codename, llvm_major), encoding="utf-8")
        for command in ([*prefix, "install", "-d", "-m", "0755", str(KEYRING.parent)],
                        [*prefix, "install", "-m", "0644", str(root / KEY_SOURCE), str(KEYRING)],
                        [*prefix, "install", "-m", "0644", str(staged), str(SOURCE_LIST)]):
            subprocess.run(command, check=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="Ludus checkout providing the pin and key")
    parser.add_argument("--codename", default="", help="Ubuntu codename (default: from /etc/os-release)")
    arguments = parser.parse_args(argv)
    try:
        register(arguments.root, arguments.codename or os_release_codename(),
                 pinned_llvm_major(arguments.root), privilege_prefix())
    except (SourceError, OSError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
