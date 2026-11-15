#!/usr/bin/env python3
# Copyright (C) 2026 Vitor Mendes Camilo
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of OpenJLS-Demos. Available under GPLv3 or a
# commercial license. See LICENSE.md and README.md for details.
#
"""provenance.py: tie each bitstream to the RTL and build inputs it was made from

  provenance.py begin N          record the tree hashes when a build of depth N starts
  provenance.py stamp N [NOTE]   write bitstreams/encode_eth_openjls_b<N>.prov.json
  provenance.py check [N ...]    compare each bitstream against the current tree
"""
import glob
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.dirname(HERE)
OJLS = os.path.normpath(os.path.join(DEMO, "..", "ThirdParty", "OpenJLS"))
HW = os.path.join(DEMO, "Hardware", "pynq-z2")
BITSTREAM_DIR = os.environ.get("BITSTREAM_DIR", os.path.join(HW, "bitstreams"))
IP_SRC = os.path.join(OJLS, "Sources", "Xilinx", "ip_repo", "openjls_axis_regs")


def bitstream(n):
    return os.path.join(BITSTREAM_DIR, f"encode_eth_openjls_b{n}.bit.bin")


def sidecar(n):
    return os.path.join(BITSTREAM_DIR, f"encode_eth_openjls_b{n}.prov.json")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sources_hash():
    """Same value as SOURCES_HASH in the OpenJLS simulation status files."""
    h = hashlib.sha256()
    for p in sorted(glob.glob(os.path.join(OJLS, "Sources", "*.vhd"))):
        h.update(open(p, "rb").read())
    return h.hexdigest()[:12]


def build_inputs():
    """Every file a bitstream build reads from the two repos, minus Vivado itself."""
    files = sorted(glob.glob(os.path.join(IP_SRC, "**", "*"), recursive=True))
    files += sorted(glob.glob(os.path.join(HW, "rtl", "*")))
    files += [os.path.join(HW, f) for f in ("build.tcl", "design_encode_ethernet.tcl")]
    return [f for f in files if os.path.isfile(f)]


def build_hash():
    h = hashlib.sha256()
    for p in build_inputs():
        h.update(os.path.relpath(p, os.path.dirname(DEMO)).encode() + b"\0")
        h.update(open(p, "rb").read())
    return h.hexdigest()[:12]


def stale_ip_files():
    """IP repo copies that differ from Sources/: the IP was not repackaged."""
    stale = []
    for p in sorted(glob.glob(os.path.join(IP_SRC, "src", "*.vhd"))):
        name = os.path.basename(p)
        src = os.path.join(OJLS, "Sources", name)
        if not os.path.exists(src):
            src = os.path.join(OJLS, "Sources", "Xilinx", name)
        if os.path.exists(src) and open(src, "rb").read() != open(p, "rb").read():
            stale.append(name)
    return stale


def git_rev(path):
    try:
        rev = subprocess.run(["git", "-C", path, "rev-parse", "--short=12", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", path, "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "?"
    return rev + ("+dirty" if dirty else "")


def current():
    return {"sources_hash": sources_hash(), "build_hash": build_hash(),
            "openjls_commit": git_rev(OJLS),
            "demos_commit": git_rev(DEMO)}


def pending(n):
    return os.path.join(BITSTREAM_DIR, f".b{n}.prov.pending")


def begin(n):
    os.makedirs(BITSTREAM_DIR, exist_ok=True)
    with open(pending(n), "w") as f:
        json.dump(current(), f)


def stamp(n, note=""):
    """Hashes from begin() when present, so edits during a build do not leak in."""
    try:
        base = json.load(open(pending(n)))
        os.remove(pending(n))
    except OSError:
        base = current()
    rec = dict(base, bitness=n, bit_sha256=sha256_file(bitstream(n)))
    if note:
        rec["note"] = note
    with open(sidecar(n), "w") as f:
        json.dump(rec, f, indent=1)
        f.write("\n")
    return rec


def check(n):
    """(record or None, problems) for bitstream N against the current tree."""
    if not os.path.exists(sidecar(n)):
        return None, [f"b{n}: no {os.path.basename(sidecar(n))}; rebuild, or stamp it by hand"]
    rec = json.load(open(sidecar(n)))
    problems = []
    if rec.get("bit_sha256") != sha256_file(bitstream(n)):
        problems.append(f"b{n}: the bitstream is not the one its sidecar was stamped for")
    cur = current()
    for key in ("sources_hash", "build_hash"):
        if rec.get(key) != cur[key]:
            problems.append(f"b{n}: {key} {rec.get(key)} != current {cur[key]}")
    return rec, problems


def main(argv):
    if len(argv) == 2 and argv[0] == "begin":
        begin(int(argv[1]))
        return 0
    if len(argv) >= 2 and argv[0] == "stamp":
        print(json.dumps(stamp(int(argv[1]), " ".join(argv[2:]))))
        return 0
    if argv and argv[0] == "check":
        depths = [int(a) for a in argv[1:]] or [n for n in range(8, 17) if os.path.exists(bitstream(n))]
        bad = [f"IP repo is stale against Sources/: {', '.join(s)}" for s in [stale_ip_files()] if s]
        for n in depths:
            rec, p = check(n)
            bad += p
            if rec and not p:
                print(f"b{n}: ok, OpenJLS {rec['openjls_commit']}, sources {rec['sources_hash']}")
        for p in bad:
            print("  " + p)
        return 1 if bad else 0
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
