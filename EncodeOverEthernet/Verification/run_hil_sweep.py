#!/usr/bin/env python3
# Copyright (C) 2026 Vitor Mendes Camilo
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of OpenJLS-Demos. Available under GPLv3 or a
# commercial license. See LICENSE.md and README.md for details.
#
"""run_hil_sweep.py: byte-exact HIL sweep of the OpenJLS corpus vs CharLS

Per depth N (8..16, from each PGM's maxval, as the client derives it): reload
encode_eth_openjls_b<N>.bit.bin with board_reload.sh, encode every depth-N
image on the board, cmp each against its CharLS golden.

Environment (defaults suit the in-tree layout):
  BOARD            ssh target (default xilinx@192.168.2.99; key auth only;
                   empty = --dry-run only)
  BOARD_IP         address the client connects to (default: host of BOARD)
  SSH_KEY, SSH_OPTS
  SUDO_PASS        board sudo password (default xilinx; empty = passwordless)
  BOARD_HOME       default /home/<user of BOARD>
  BOARD_SERVER_DIR dir of the built ojls_server (default $BOARD_HOME/Software)
  BOARD_BS_DIR     bitstream staging dir (default $BOARD_HOME/bitstreams)
  BITSTREAM_DIR, IMAGES_DIR, CHARLS, CLIENT, OUT_DIR
  FULL_IMAGES_DIR  full-size originals (default Images_full beside IMAGES_DIR;
                   empty = skip)
  PORT             server port (default 19020)
  TX_BYTES         board tx buffer; larger images are skipped (default 128M)
  HW_MAX_W/H       max image size; larger are skipped (default 65535)

Usage:
  ./run_hil_sweep.py                 # full sweep
  ./run_hil_sweep.py --dry-run       # preflight + depth histogram
  ./run_hil_sweep.py --bitness 8 12  # only these depths
  ./run_hil_sweep.py --limit 5       # at most 5 images per depth
"""
import argparse
import csv
import filecmp
import os
import shlex
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.dirname(HERE)
OJLS = os.path.normpath(os.path.join(DEMO, "..", "ThirdParty", "OpenJLS"))


def env(name, default):
    return os.environ.get(name, default)


BITSTREAM_DIR = env("BITSTREAM_DIR", os.path.join(DEMO, "Hardware", "pynq-z2", "bitstreams"))
IMAGES_DIR = env("IMAGES_DIR", os.path.join(OJLS, "Verification", "Golden model", "Images"))
FULL_IMAGES_DIR = env("FULL_IMAGES_DIR", os.path.join(os.path.dirname(IMAGES_DIR), "Images_full"))
CHARLS = env("CHARLS", os.path.join(OJLS, "ThirdParty", "charls", "build", "cli", "charls-cli"))
CLIENT = env("CLIENT", os.path.join(DEMO, "Software", "ojls_client"))
OUT_DIR = env("OUT_DIR", os.path.join(HERE, "out"))
PORT = int(env("PORT", "19020"))
TX_BYTES = int(env("TX_BYTES", str(128 * 1024 * 1024)))
# The server enforces the real OJLS_REG_MAXDIM; this pre-skip only saves
# round trips on down-sized builds.
HW_MAX_W = int(env("HW_MAX_W", "65535"))
HW_MAX_H = int(env("HW_MAX_H", "65535"))

REF_DIR = os.path.join(OJLS, "Verification", "T87 conformance", "Reference Images")
GATE_PGM = os.path.join(REF_DIR, "TEST16.PGM")
GATE_JLS = os.path.join(REF_DIR, "T16E0.JLS")
BOARD_RELOAD = os.path.join(HERE, "board_reload.sh")


class Fail(Exception):
    pass


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


# --- PGM header -------------------------------------------------------------

def pgm_geom(path):
    """Return (width, height, maxval) for a binary PGM, tolerant of comments."""
    with open(path, "rb") as f:
        if f.read(2) != b"P5":
            raise Fail("not a binary PGM (P5)")
        toks = []
        tok = b""
        while len(toks) < 3:
            c = f.read(1)
            if not c:
                raise Fail("truncated header")
            if c == b"#":
                f.readline()
                continue
            if c.isspace():
                if tok:
                    toks.append(tok)
                    tok = b""
                continue
            tok += c
    return int(toks[0]), int(toks[1]), int(toks[2])


def bucket_of(maxval):
    """(bitness, bytes_per_pixel) exactly as the client derives them."""
    bits = max(8, maxval.bit_length())
    return bits, (2 if maxval > 255 else 1)


# --- ssh / board ------------------------------------------------------------

def ssh_prefix(board, key, opts):
    cmd = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
           "-o", "ConnectTimeout=10"]
    if key:
        cmd += ["-i", key]
    cmd += shlex.split(opts)
    return cmd + [board]


def scp_prefix(key, opts):
    cmd = ["scp", "-q", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
           "-o", "ConnectTimeout=10"]
    if key:
        cmd += ["-i", key]
    return cmd + shlex.split(opts)


def wait_for_port(ip, port, timeout=25.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((ip, port), timeout=2.0):
                return True
        except OSError:
            time.sleep(0.5)
    return False


# One ssh round trip; "kind:name:value" lines name the specific missing piece.
PROBE_SH = r"""set -u
for n in tx rx desc; do
  d=/sys/class/u-dma-buf/udmabuf-ojls-$n
  if [ -d "$d" ]; then echo "buf:$n:$(cat "$d/size" 2>/dev/null || echo 0)"
  else echo "buf:$n:missing"; fi
done
for n in 'ojls-tx@10000000' 'ojls-rx@18000000' 'ojls-desc@ffc0000'; do
  if [ -d "/proc/device-tree/reserved-memory/$n" ]; then echo "rmem:$n:ok"
  else echo "rmem:$n:missing"; fi
done
for n in 'openjls@40000000' 'dma@40400000'; do
  if [ -d "/proc/device-tree/$n" ]; then echo "dt:$n:ok"; else echo "dt:$n:missing"; fi
done
if [ -x @SERVER@/ojls_server ]; then echo "server:bin:ok"; else echo "server:bin:missing"; fi
if [ -d /sys/class/fpga_manager/fpga0 ]; then echo "fpga:mgr:ok"; else echo "fpga:mgr:missing"; fi
"""


def _mib(n):
    return f"{n // 1048576} MiB" if n >= 1048576 else f"{n // 1024} KiB"


def board_probe(args):
    """Check over ssh that the board can serve; returns (problems, notes).

    Tells apart a missing boot-time overlay (setup_bootargs.sh) from a missing
    u-dma-buf after a reboot (board_setup.sh); board_reload.sh loads neither.
    """
    problems, notes = [], []
    sh = PROBE_SH.replace("@SERVER@", shlex.quote(args.board_server_dir))
    cmd = ssh_prefix(args.board, args.ssh_key, args.ssh_opts) + [sh]
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=30)
    except subprocess.TimeoutExpired:
        return ([f"board {args.board} did not answer ssh within 30 s\n"
                 f"    check it is powered and on the network: ping {args.board_ip}"],
                notes)
    out = r.stdout.decode(errors="replace").strip()
    if r.returncode != 0:
        # ssh exits 255 for both unreachable and auth failure; the fixes differ.
        low = out.lower()
        unreachable = any(s in low for s in (
            "connection timed out", "connection refused", "no route to host",
            "network is unreachable", "name or service not known",
            "could not resolve hostname", "host is down"))
        hint = (f"check it is powered and on the network: ping {args.board_ip}"
                if unreachable else
                f"key-based auth is required: ssh-copy-id {args.board}")
        return ([f"cannot ssh to {args.board} (rc={r.returncode}): {out}\n"
                 f"    {hint}"], notes)

    got = {}
    for line in out.splitlines():
        parts = line.strip().split(":")
        if len(parts) == 3:
            got[(parts[0], parts[1])] = parts[2]
    if not got:
        return ([f"board probe returned nothing usable from {args.board}: {out}"], notes)

    missing_dt = sorted(n for (k, n), v in got.items()
                        if k in ("rmem", "dt") and v != "ok")
    missing_buf = sorted(n for (k, n), v in got.items()
                         if k == "buf" and not v.isdigit())

    if missing_dt:
        problems.append(
            "board device tree is missing: " + " ".join(missing_dt) + "\n"
            "    openjls.dtbo was not applied at boot (first-time setup never ran,\n"
            "    or its uenvcmd fell back to a stock boot). On the board, as root:\n"
            f"    sudo ./setup_bootargs.sh && sudo reboot   "
            f"(see {os.path.join(DEMO, 'Hardware', 'pynq-z2')}/INTERNALS.md)")
    elif missing_buf:
        problems.append(
            "u-dma-buf is not loaded on the board (no udmabuf-ojls-"
            + ", ".join(missing_buf) + ")\n"
            "    the DMA carveout is live, so this is just the one-shot bring-up:\n"
            "    it is REQUIRED AFTER EACH BOOT, and the sweep's per-depth\n"
            "    board_reload.sh does not do it. On the board, as root:\n"
            f"    sudo env HOME=$HOME ./board_setup.sh 8")
    else:
        sizes = {n: int(v) for (k, n), v in got.items() if k == "buf" and v.isdigit()}
        notes.append("board: u-dma-buf up — "
                     + ", ".join(f"{n} {_mib(sizes[n])}" for n in ("tx", "rx", "desc")
                                 if n in sizes))
        tx = sizes.get("tx", 0)
        if tx and tx < TX_BYTES:
            notes.append(f"  warn: board tx buffer is {_mib(tx)} but TX_BYTES is "
                         f"{_mib(TX_BYTES)}; images between the two will be sent and\n"
                         f"        fail rather than be skipped — set TX_BYTES={tx}")

    if got.get(("server", "bin")) != "ok":
        problems.append(
            f"ojls_server not executable at {args.board_server_dir}/ojls_server "
            f"on {args.board}\n"
            f"    build and copy it: make -C '{os.path.join(DEMO, 'Software')}' "
            f"CROSS_COMPILE=arm-linux-gnueabihf- ojls_server\n"
            f"    then: scp -r '{os.path.join(DEMO, 'Software')}' {args.board}:~/")
    if got.get(("fpga", "mgr")) != "ok":
        problems.append(f"no /sys/class/fpga_manager/fpga0 on {args.board} — "
                        "this kernel cannot load a bitstream")
    return problems, notes


# --- CharLS -----------------------------------------------------------------

def charls_encode(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    r = subprocess.run([CHARLS, "encode", src, dst],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise Fail(f"charls encode failed: {r.stderr.decode(errors='replace').strip()}")


def charls_gate():
    """Refuse to trust CharLS unless it reproduces the official T16E0.JLS."""
    if not (os.path.exists(GATE_PGM) and os.path.exists(GATE_JLS)):
        die("CharLS trust-gate vectors missing. Fetch them first:\n"
            f"    '{OJLS}/Verification/T87 conformance/fetch_reference_images.sh'")
    out = os.path.join(OUT_DIR, "gate", "TEST16_charls.jls")
    charls_encode(GATE_PGM, out)
    if not filecmp.cmp(out, GATE_JLS, shallow=False):
        die("CharLS does NOT reproduce the official T16E0.JLS byte-exact — "
            "the golden generator is untrustworthy, aborting.")
    print("CharLS gate: reproduces official T16E0.JLS byte-exact OK")


# --- main -------------------------------------------------------------------

def available_bitstreams():
    found = {}
    for n in range(8, 17):
        p = os.path.join(BITSTREAM_DIR, f"encode_eth_openjls_b{n}.bit.bin")
        if os.path.exists(p):
            found[n] = p
    return found


def corpus_dirs():
    """IMAGES_DIR, plus FULL_IMAGES_DIR when set and present."""
    dirs = [IMAGES_DIR]
    if FULL_IMAGES_DIR and os.path.isdir(FULL_IMAGES_DIR):
        dirs.append(FULL_IMAGES_DIR)
    return dirs


def corpus_pgms():
    for d in corpus_dirs():
        for name in sorted(os.listdir(d)):
            if name.lower().endswith(".pgm"):
                yield name, os.path.join(d, name)


def enumerate_corpus():
    """Map bitness -> list of (path, w, h, bytes_per_pixel, in_bytes)."""
    buckets = {}
    sub8 = []
    for name, path in corpus_pgms():
        try:
            w, h, mx = pgm_geom(path)
        except Fail as e:
            print(f"  warn: skipping {name}: {e}", file=sys.stderr)
            continue
        if mx.bit_length() < 8:
            sub8.append(name)
            continue
        bits, bpp = bucket_of(mx)
        buckets.setdefault(bits, []).append((path, w, h, bpp, w * h * bpp))
    return buckets, sub8


def preflight(args):
    problems = []
    has_pgm = os.path.isdir(IMAGES_DIR) and any(
        n.lower().endswith(".pgm") for n in os.listdir(IMAGES_DIR))
    if not has_pgm:
        problems.append(
            f"no corpus PGMs in {IMAGES_DIR}\n"
            f"    prepare it: '{OJLS}/Verification/Golden model/prepare_images.sh'")
    if not os.path.exists(CHARLS):
        problems.append(
            f"charls-cli not built at {CHARLS}\n"
            f"    build it: '{OJLS}/ThirdParty/fetch_third_party.sh' charls")
    if not (os.path.exists(GATE_PGM) and os.path.exists(GATE_JLS)):
        problems.append(
            f"CharLS trust-gate vectors missing (T16E0.JLS and its source PGM)\n"
            f"    fetch them: '{OJLS}/Verification/T87 conformance/fetch_reference_images.sh'")
    if not (os.path.exists(CLIENT) and os.access(CLIENT, os.X_OK)):
        problems.append(
            f"ojls_client not built at {CLIENT}\n"
            f"    build it: make -C '{os.path.join(DEMO, 'Software')}' ojls_client")
    if not available_bitstreams():
        problems.append(
            f"no bitstreams in {BITSTREAM_DIR}\n"
            f"    build them: '{os.path.join(DEMO, 'Hardware', 'pynq-z2', 'build_all_bitness.sh')}'")
    if not args.dry_run and not args.board:
        problems.append("BOARD is required for a live sweep (or pass --dry-run)")

    # On --dry-run, board findings are warnings.
    if args.board:
        board_problems, notes = board_probe(args)
        for n in notes:
            print(n)
        if args.dry_run:
            for p in board_problems:
                print("warn: " + p, file=sys.stderr)
        else:
            problems += board_problems

    if problems:
        die("preflight failed:\n  - " + "\n  - ".join(problems))


def reload_board(args, bitness):
    """Copy the bitstream + reload script to the board and reload as root."""
    board_home = args.board_home
    bs_dir = args.board_bs_dir
    server_dir = args.board_server_dir
    fw = f"encode_eth_openjls_b{bitness}.bit.bin"
    local_bs = os.path.join(BITSTREAM_DIR, fw)

    scp = scp_prefix(args.ssh_key, args.ssh_opts)
    run(scp + [BOARD_RELOAD, f"{args.board}:{board_home}/board_reload.sh"], "scp reload script")
    run(ssh_prefix(args.board, args.ssh_key, args.ssh_opts) + [f"mkdir -p {shlex.quote(bs_dir)}"],
        "mkdir board bitstream dir")
    run(scp + [local_bs, f"{args.board}:{bs_dir}/{fw}"], "scp bitstream")

    sudo = f"echo {shlex.quote(args.sudo_pass)} | sudo -S" if args.sudo_pass else "sudo"
    remote = (f"{sudo} bash {board_home}/board_reload.sh {bitness} "
              f"{shlex.quote(server_dir)} /lib/firmware {shlex.quote(bs_dir)}")
    run(ssh_prefix(args.board, args.ssh_key, args.ssh_opts) + [remote],
        f"reload board to BITNESS {bitness}", timeout=60)

    if not wait_for_port(args.board_ip, PORT):
        raise Fail(f"server did not open {args.board_ip}:{PORT} after reload")


def run(cmd, what, timeout=30):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    if r.returncode != 0:
        raise Fail(f"{what} failed (rc={r.returncode}): {r.stdout.decode(errors='replace').strip()}")
    return r.stdout.decode(errors="replace")


def encode_one(args, path):
    stem = os.path.splitext(os.path.basename(path))[0]
    out_jls = os.path.join(OUT_DIR, "encoded", stem + ".jls")
    os.makedirs(os.path.dirname(out_jls), exist_ok=True)
    if os.path.exists(out_jls):
        os.remove(out_jls)
    cmd = [CLIENT, "-p", str(PORT), args.board_ip, path, out_jls]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    if r.returncode != 0:
        lines = r.stdout.decode(errors="replace").strip().splitlines()
        raise Fail("client: " + (" | ".join(lines) if lines else "failed"))
    golden = os.path.join(OUT_DIR, "golden", stem + ".jls")
    if not os.path.exists(golden) or os.path.getmtime(golden) < os.path.getmtime(path):
        charls_encode(path, golden)
    return out_jls, golden


def write_report(results):
    path = os.path.join(OUT_DIR, "report.md")
    rows = {}
    for n, _, status, in_b, jls_b in results:
        r = rows.setdefault(n, [0, 0, 0, 0, 0, 0])
        r[0] += 1
        r[1 if status == "pass" else 2 if status.startswith("skip") else 3] += 1
        r[4] += in_b
        r[5] += jls_b
    rows = dict(sorted(rows.items()))
    rows["**Total**"] = [sum(c) for c in zip(*rows.values())]
    with open(path, "w") as f:
        f.write("# Hardware-in-the-loop results\n\n"
                "Each image encoded on the PYNQ-Z2 and byte-compared against CharLS; "
                "per-image results are in results.csv.\n\n"
                "| Depth | Images | Pass | Skip | Fail | Input (B) | JLS checked (B) | Ratio |\n"
                "|--:|--:|--:|--:|--:|--:|--:|--:|\n")
        for n, (imgs, ok, skip, fail, in_b, jls_b) in rows.items():
            ratio = f"{in_b / jls_b:.2f}:1" if jls_b else "-"
            f.write(f"| {n} | {imgs} | {ok} | {skip} | {fail} | {in_b} | {jls_b} | {ratio} |\n")
    return path


def main():
    ap = argparse.ArgumentParser(description="HIL bitness sweep for EncodeOverEthernet")
    ap.add_argument("--dry-run", action="store_true",
                    help="preflight + bucket histogram only; encodes nothing. Probes "
                         "the board over ssh and reports what it finds as warnings; "
                         "set BOARD= empty to skip the board entirely")
    ap.add_argument("--bitness", nargs="+", type=int, metavar="N",
                    help="restrict to these depths (default: all available)")
    ap.add_argument("--limit", type=int, default=0,
                    help="at most N images per bucket (0 = all)")
    ap.add_argument("--no-gate", action="store_true",
                    help="skip the CharLS T16E0.JLS trust gate")
    args = ap.parse_args()

    args.board = env("BOARD", "xilinx@192.168.2.99")
    args.board_ip = env("BOARD_IP", args.board.split("@")[-1] if args.board else "")
    args.ssh_key = env("SSH_KEY", "")
    args.ssh_opts = env("SSH_OPTS", "")
    args.sudo_pass = env("SUDO_PASS", "xilinx")
    user = args.board.split("@")[0] if "@" in args.board else "xilinx"
    args.board_home = env("BOARD_HOME", f"/home/{user}")
    args.board_server_dir = env("BOARD_SERVER_DIR", f"{args.board_home}/Software")
    args.board_bs_dir = env("BOARD_BS_DIR", f"{args.board_home}/bitstreams")

    preflight(args)

    bitstreams = available_bitstreams()
    buckets, sub8 = enumerate_corpus()

    depths = sorted(set(bitstreams) & set(buckets))
    if args.bitness:
        depths = [d for d in depths if d in args.bitness]

    # -- report the plan
    print(f"\ncorpus: {', '.join(corpus_dirs())}")
    print(f"bitstreams: {sorted(bitstreams)}")
    print("bucket histogram (precision: image count):")
    for n in range(8, 17):
        imgs = buckets.get(n, [])
        if not imgs:
            continue
        have = "bitstream" if n in bitstreams else "NO BITSTREAM"
        run_it = "run" if n in depths else "skip"
        print(f"  b{n:<2}: {len(imgs):4d} images   [{have}, {run_it}]")
    if sub8:
        print(f"  sub-8-bit (no bitstream, skipped): {len(sub8)}")
    missing = sorted(set(buckets) - set(bitstreams))
    if missing:
        print(f"  note: precisions with images but no bitstream: {missing}")
    if not depths:
        die("nothing to run: no precision has both a bitstream and images")

    if not args.no_gate:
        charls_gate()

    if args.dry_run:
        print("\n--dry-run: probed the board, encoded nothing. Plan above.")
        return

    # -- live sweep
    os.makedirs(OUT_DIR, exist_ok=True)
    results = []           # (bitness, image, status, in_bytes, jls_bytes)
    totals = {}            # bitness -> [passed, failed, skipped]

    for n in depths:
        imgs = sorted(buckets[n], key=lambda t: t[4])   # small first
        if args.limit:
            imgs = imgs[:args.limit]
        print(f"\n===== BITNESS {n}: {len(imgs)} images =====")
        try:
            reload_board(args, n)
        except Fail as e:
            die(f"BITNESS {n}: board reload failed: {e}")
        tally = [0, 0, 0]
        for path, w, h, bpp, in_bytes in imgs:
            name = os.path.basename(path)
            if in_bytes > TX_BYTES:
                print(f"  skip {name}: {in_bytes} B > tx buffer {TX_BYTES} B")
                results.append((n, name, "skip-too-large", in_bytes, 0))
                tally[2] += 1
                continue
            if w > HW_MAX_W or h > HW_MAX_H:
                print(f"  skip {name}: {w}x{h} exceeds hardware max {HW_MAX_W}x{HW_MAX_H}")
                results.append((n, name, "skip-hw-dims", in_bytes, 0))
                tally[2] += 1
                continue
            try:
                out_jls, golden = encode_one(args, path)
            except Fail as e:
                print(f"  FAIL {name}: {e}")
                results.append((n, name, f"error:{e}", in_bytes, 0))
                tally[1] += 1
                continue
            jls_bytes = os.path.getsize(out_jls)
            if filecmp.cmp(out_jls, golden, shallow=False):
                results.append((n, name, "pass", in_bytes, jls_bytes))
                tally[0] += 1
            else:
                print(f"  MISMATCH {name}: hw {jls_bytes} B != golden {os.path.getsize(golden)} B")
                results.append((n, name, "mismatch", in_bytes, jls_bytes))
                tally[1] += 1
        totals[n] = tally
        print(f"  b{n}: {tally[0]} pass, {tally[1]} fail, {tally[2]} skip")

    # -- results.csv
    csv_path = os.path.join(OUT_DIR, "results.csv")
    with open(csv_path, "w", newline="") as f:
        wtr = csv.writer(f)
        wtr.writerow(["bitness", "image", "status", "in_bytes", "jls_bytes"])
        wtr.writerows(results)
    report_path = write_report(results)

    # -- summary
    print("\n==================== summary ====================")
    tot_pass = tot_fail = tot_skip = 0
    for n in depths:
        p, fl, sk = totals[n]
        tot_pass += p; tot_fail += fl; tot_skip += sk
        flag = "OK" if fl == 0 else "FAIL"
        print(f"  b{n:<2}: {p:4d} pass  {fl:4d} fail  {sk:4d} skip   [{flag}]")
    print(f"  all: {tot_pass} pass, {tot_fail} fail, {tot_skip} skip")
    print(f"  results: {csv_path}, {report_path}")
    sys.exit(1 if tot_fail else 0)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
