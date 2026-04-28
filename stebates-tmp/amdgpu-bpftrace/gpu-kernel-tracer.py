#!/usr/bin/env python3
"""
AMD GPU Kernel Tracer -- Python wrapper for bpftrace.

Launches gpu-kernel-tracer.bt, parses its pipe-delimited
stdout, converts monotonic timestamps to UTC, optionally
polls sysfs for GPU utilization, and outputs structured
JSONL, CSV, or a live terminal table.

Usage:
    sudo python3 gpu-kernel-tracer.py [OPTIONS]

Requires: bpftrace, Python 3.10+, amdgpu module loaded.
"""

import argparse
import csv
import ctypes
import ctypes.util
import json
import os
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
BT_SCRIPT = SCRIPT_DIR / "gpu-kernel-tracer.bt"

TTM_DOMAINS = {
    0: "SYSTEM",
    1: "TT/GTT",
    2: "VRAM",
    3: "PRIV",
}

AMDGPU_GEM_DOMAINS = {
    0x1: "CPU",
    0x2: "GTT",
    0x4: "VRAM",
    0x8: "GDS",
    0x10: "GWS",
    0x20: "OA",
    0x40: "DOORBELL",
}


def domain_name(val, is_ttm=False):
    """Map numeric domain to human-readable name."""
    if is_ttm:
        return TTM_DOMAINS.get(val, f"UNKNOWN({val})")
    for bit, name in AMDGPU_GEM_DOMAINS.items():
        if val & bit:
            return name
    return f"UNKNOWN({val})"


# ── Monotonic-to-UTC offset ──────────────────────


def get_clock_offset_ns():
    """
    Compute offset between CLOCK_MONOTONIC and
    CLOCK_REALTIME so we can convert bpftrace nsecs
    (monotonic) to UTC.
    """
    try:
        libc_name = ctypes.util.find_library("c")
        if not libc_name:
            libc_name = "libc.so.6"
        libc = ctypes.CDLL(libc_name, use_errno=True)

        class Timespec(ctypes.Structure):
            _fields_ = [
                ("tv_sec", ctypes.c_long),
                ("tv_nsec", ctypes.c_long),
            ]

        clock_gettime = libc.clock_gettime
        clock_gettime.argtypes = [
            ctypes.c_int,
            ctypes.POINTER(Timespec),
        ]
        clock_gettime.restype = ctypes.c_int

        CLOCK_REALTIME = 0
        CLOCK_MONOTONIC = 1

        ts_mono = Timespec()
        ts_real = Timespec()
        clock_gettime(CLOCK_MONOTONIC, ctypes.byref(ts_mono))
        clock_gettime(CLOCK_REALTIME, ctypes.byref(ts_real))

        mono_ns = ts_mono.tv_sec * 10**9 + ts_mono.tv_nsec
        real_ns = ts_real.tv_sec * 10**9 + ts_real.tv_nsec
        return real_ns - mono_ns
    except Exception:
        mono = time.monotonic_ns()
        real = time.time_ns()
        return real - mono


def ns_to_utc(mono_ns, offset_ns):
    """Convert monotonic nanoseconds to UTC ISO string."""
    real_ns = mono_ns + offset_ns
    ts = real_ns / 1e9
    dt = datetime.fromtimestamp(ts, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.%fZ")


# ── sysfs polling ────────────────────────────────


class SysfsPoller:
    """Polls amdgpu sysfs entries for GPU utilization."""

    def __init__(self, card_num, interval=1.0):
        base = Path(f"/sys/class/drm/card{card_num}/device")
        self.base = base
        self.interval = interval
        self._stop = threading.Event()
        self._thread = None
        self._records = []
        self._lock = threading.Lock()

    def _read(self, name):
        try:
            return (self.base / name).read_text().strip()
        except (OSError, IOError):
            return None

    def _poll_once(self):
        rec = {
            "event": "sysfs_poll",
            "utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
        }
        gpu_busy = self._read("gpu_busy_percent")
        if gpu_busy is not None:
            try:
                rec["gpu_busy_pct"] = int(gpu_busy)
            except ValueError:
                pass

        vram_used = self._read("mem_info_vram_used")
        if vram_used is not None:
            try:
                rec["vram_used_bytes"] = int(vram_used)
            except ValueError:
                pass

        vram_total = self._read("mem_info_vram_total")
        if vram_total is not None:
            try:
                rec["vram_total_bytes"] = int(vram_total)
            except ValueError:
                pass

        vcn_busy = self._read("vcn_busy_percent")
        if vcn_busy is not None:
            try:
                rec["vcn_busy_pct"] = int(vcn_busy)
            except ValueError:
                pass

        link_speed = self._read("current_link_speed")
        if link_speed is not None:
            rec["link_speed"] = link_speed

        link_width = self._read("current_link_width")
        if link_width is not None:
            try:
                rec["link_width"] = int(link_width)
            except ValueError:
                rec["link_width"] = link_width

        with self._lock:
            self._records.append(rec)

    def _run(self):
        while not self._stop.is_set():
            self._poll_once()
            self._stop.wait(self.interval)

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)

    def drain(self):
        """Return and clear accumulated records."""
        with self._lock:
            recs = self._records[:]
            self._records.clear()
        return recs


# ── Output formatters ────────────────────────────


class JsonOutput:
    def __init__(self, fp):
        self.fp = fp

    def write(self, record):
        self.fp.write(json.dumps(record) + "\n")
        self.fp.flush()


class CsvOutput:
    def __init__(self, fp):
        self.fp = fp
        self.writer = None
        self._fields = None

    def write(self, record):
        if self.writer is None:
            self._fields = sorted(record.keys())
            self.writer = csv.DictWriter(
                self.fp,
                fieldnames=self._fields,
                extrasaction="ignore",
            )
            self.writer.writeheader()
        new_keys = set(record.keys()) - set(self._fields)
        if new_keys:
            self._fields = sorted(set(self._fields) | new_keys)
            self.writer = csv.DictWriter(
                self.fp,
                fieldnames=self._fields,
                extrasaction="ignore",
            )
        self.writer.writerow(record)
        self.fp.flush()


class LiveOutput:
    """Live terminal table output."""

    def __init__(self):
        self._count = 0

    def write(self, record):
        ev = record.get("event", "?")
        if self._count % 40 == 0:
            self._print_header()
        self._count += 1

        if ev == "kernel_dispatch":
            utc = record.get("utc_end", "")
            ring = record.get("ring", "")
            dur = record.get("duration_us", "")
            nibs = record.get("num_ibs", "")
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  DISPATCH  {utc:>26s}"
                f"  {ring:<16s}"
                f"  {dur:>8s} us"
                f"  ibs={nibs:<3s}"
                f"  pid={p:<6s}  {c}"
            )
        elif ev == "fence_proc":
            utc = record.get("utc", "")
            ring = record.get("ring", "")
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  FENCE     {utc:>26s}"
                f"  {ring:<16s}"
                f"  {'':>8s}   "
                f"  {'':>8s}"
                f"  pid={p:<6s}  {c}"
            )
        elif ev == "bo_create":
            utc = record.get("utc", "")
            sz = record.get("size_bytes", 0)
            dom = record.get("domain", "")
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  BO_NEW    {utc:>26s}"
                f"  {dom:<16s}"
                f"  {self._fmt_size(sz):>11s}"
                f"  {'':>8s}"
                f"  pid={p:<6s}  {c}"
            )
        elif ev == "bo_move":
            utc = record.get("utc", "")
            sz = record.get("size_bytes", 0)
            to_d = record.get("to_domain", "")
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  BO_MOVE   {utc:>26s}"
                f"  -> {to_d:<12s}"
                f"  {self._fmt_size(sz):>11s}"
                f"  {'':>8s}"
                f"  pid={p:<6s}  {c}"
            )
        elif ev == "dma_copy":
            utc = record.get("utc", "")
            sz = record.get("size_bytes", 0)
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  DMA_COPY  {utc:>26s}"
                f"  {'':>16s}"
                f"  {self._fmt_size(sz):>11s}"
                f"  {'':>8s}"
                f"  pid={p:<6s}  {c}"
            )
        elif ev == "sysfs_poll":
            utc = record.get("utc", "")
            gpu = record.get("gpu_busy_pct", "?")
            vused = record.get("vram_used_bytes", 0)
            vtot = record.get("vram_total_bytes", 1)
            pct = f"{100 * vused / vtot:.0f}%" if vtot else "?"
            print(
                f"  SYSFS     {utc:>26s}"
                f"  gpu={gpu}%"
                f"  vram={self._fmt_size(vused)}"
                f" ({pct})"
            )
        elif ev == "ring_commit":
            utc = record.get("utc", "")
            ring = record.get("ring", "")
            p = record.get("pid", "")
            print(
                f"  COMMIT    {utc:>26s}"
                f"  {ring:<16s}"
                f"  {'':>11s}"
                f"  {'':>8s}"
                f"  pid={p:<6s}"
            )
        elif ev == "job_done":
            utc = record.get("utc", "")
            ctx = record.get("fence_ctx", "")
            seq = record.get("fence_seqno", "")
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  JOB_DONE  {utc:>26s}"
                f"  fence={ctx}:{seq}"
                f"  {'':>11s}"
                f"  {'':>8s}"
                f"  pid={p:<6s}  {c}"
            )
        elif ev == "bo_pin":
            utc = record.get("utc", "")
            sz = record.get("size_bytes", 0)
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  BO_PIN    {utc:>26s}"
                f"  {'':>16s}"
                f"  {self._fmt_size(sz):>11s}"
                f"  {'':>8s}"
                f"  pid={p:<6s}  {c}"
            )
        elif ev == "compute_active":
            utc = record.get("utc", "")
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  GPU_ON    {utc:>26s}"
                f"  {'compute':>16s}"
                f"  {'':>11s}"
                f"  {'':>8s}"
                f"  pid={p:<6s}  {c}"
            )
        elif ev == "compute_idle":
            utc = record.get("utc", "")
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  GPU_OFF   {utc:>26s}"
                f"  {'compute':>16s}"
                f"  {'':>11s}"
                f"  {'':>8s}"
                f"  pid={p:<6s}  {c}"
            )
        elif ev == "kfd_submit_ib":
            utc = record.get("utc", "")
            eng = record.get("engine", "")
            ib_len = record.get("ib_len", "")
            p = record.get("pid", "")
            c = record.get("comm", "")
            print(
                f"  KFD_IB    {utc:>26s}"
                f"  {eng:<16s}"
                f"  len={ib_len:<6s}"
                f"  {'':>8s}"
                f"  pid={p:<6s}  {c}"
            )
        else:
            print(f"  {ev:<10s}  {json.dumps(record)}")

    @staticmethod
    def _fmt_size(b):
        try:
            b = int(b)
        except (ValueError, TypeError):
            return str(b)
        if b < 1024:
            return f"{b} B"
        if b < 1024 * 1024:
            return f"{b / 1024:.1f} KiB"
        if b < 1024 * 1024 * 1024:
            return f"{b / (1024 * 1024):.1f} MiB"
        return f"{b / (1024 * 1024 * 1024):.2f} GiB"

    @staticmethod
    def _print_header():
        hdr = (
            f"  {'EVENT':<10s}"
            f"  {'UTC':>26s}"
            f"  {'RING/DOMAIN':<16s}"
            f"  {'SIZE/DUR':>11s}"
            f"  {'':>8s}"
            f"  {'PID/COMM'}"
        )
        print()
        print(hdr)
        print("  " + "-" * (len(hdr) - 2))


# ── bpftrace event parser ────────────────────────


def parse_line(line, offset_ns):
    """
    Parse a pipe-delimited bpftrace output line into
    a JSON-ready dict, or None if not a data line.
    """
    line = line.strip()
    if not line or line.startswith("Attaching"):
        return None

    parts = line.split("|")
    if len(parts) < 2:
        return None

    tag = parts[0]

    try:
        if tag == "TRACER_START":
            return {
                "event": "tracer_start",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
            }

        if tag == "TRACER_END":
            return {
                "event": "tracer_end",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
            }

        if tag == "HEARTBEAT":
            return {
                "event": "heartbeat",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
            }

        if tag == "FENCE_PROC":
            return {
                "event": "fence_proc",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "ring": parts[2],
                "pid": parts[3],
                "comm": parts[4] if len(parts) > 4 else "",
            }

        if tag == "JOB_DONE":
            return {
                "event": "job_done",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "fence_ctx": parts[2],
                "fence_seqno": parts[3],
                "pid": parts[4],
                "comm": parts[5] if len(parts) > 5 else "",
            }

        if tag == "RING_COMMIT":
            return {
                "event": "ring_commit",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "ring": parts[2],
                "pid": parts[3],
            }

        if tag == "BO_CREATE":
            dom_val = int(parts[4])
            return {
                "event": "bo_create",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "size_bytes": int(parts[2]),
                "type": int(parts[3]),
                "domain": domain_name(dom_val),
                "domain_raw": dom_val,
                "pid": parts[5],
                "comm": parts[6] if len(parts) > 6 else "",
            }

        if tag == "BO_MOVE":
            mem_type = int(parts[3])
            return {
                "event": "bo_move",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "size_bytes": int(parts[2]),
                "to_domain": domain_name(mem_type, is_ttm=True),
                "to_domain_raw": mem_type,
                "pid": parts[4],
                "comm": parts[5] if len(parts) > 5 else "",
            }

        if tag == "DMA_COPY":
            return {
                "event": "dma_copy",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "src_addr": f"0x{parts[2]}",
                "dst_addr": f"0x{parts[3]}",
                "size_bytes": int(parts[4]),
                "pid": parts[5],
                "comm": parts[6] if len(parts) > 6 else "",
            }

        if tag == "BO_PIN":
            return {
                "event": "bo_pin",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "size_bytes": int(parts[2]),
                "pid": parts[3],
                "comm": parts[4] if len(parts) > 4 else "",
            }

        if tag == "COMPUTE_ACTIVE":
            return {
                "event": "compute_active",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "pid": parts[2],
                "comm": parts[3] if len(parts) > 3 else "",
            }

        if tag == "COMPUTE_IDLE":
            return {
                "event": "compute_idle",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "pid": parts[2],
                "comm": parts[3] if len(parts) > 3 else "",
            }

        KFD_ENGINES = {
            0: "PM4_COMPUTE",
            1: "SDMA",
            2: "PM4_GFX",
        }

        if tag == "KFD_SUBMIT_IB":
            eng_val = int(parts[2])
            return {
                "event": "kfd_submit_ib",
                "utc": ns_to_utc(int(parts[1]), offset_ns),
                "engine": KFD_ENGINES.get(eng_val, f"UNKNOWN({eng_val})"),
                "engine_raw": eng_val,
                "vmid": int(parts[3]),
                "gpu_addr": f"0x{int(parts[4]):x}",
                "ib_len": int(parts[5]),
                "pid": parts[6],
                "comm": parts[7] if len(parts) > 7 else "",
            }

    except (ValueError, IndexError):
        return None

    return None


# ── Auto-detect DRM card ─────────────────────────


def find_amdgpu_card():
    """Find the DRM card number for the first amdgpu device."""
    drm = Path("/sys/class/drm")
    for card in sorted(drm.glob("card[0-9]*")):
        if not card.is_dir():
            continue
        driver_link = card / "device" / "driver"
        try:
            driver = driver_link.resolve().name
        except OSError:
            continue
        if driver == "amdgpu":
            name = card.name
            num = "".join(c for c in name if c.isdigit())
            return int(num)
    return None


# ── Main ─────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(
        description=(
            "AMD GPU kernel dispatch and data transfer "
            "tracer using bpftrace kprobes."
        ),
    )
    parser.add_argument(
        "--card",
        type=int,
        default=None,
        help=("DRM card number " "(default: auto-detect amdgpu)"),
    )
    parser.add_argument(
        "--output",
        choices=["json", "csv", "live"],
        default="live",
        help="Output format (default: live)",
    )
    parser.add_argument(
        "--outfile",
        default=None,
        help="Output file path (default: stdout)",
    )
    parser.add_argument(
        "--pid",
        type=int,
        default=None,
        help="Filter events to this PID only",
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=1.0,
        help="sysfs poll interval in seconds " "(default: 1.0)",
    )
    parser.add_argument(
        "--no-sysfs",
        action="store_true",
        help="Disable sysfs polling",
    )
    parser.add_argument(
        "--no-bo",
        action="store_true",
        help="Suppress BO create/move/pin events",
    )
    parser.add_argument(
        "--no-dma",
        action="store_true",
        help="Suppress DMA copy events",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=None,
        help="Run for N seconds then exit",
    )
    args = parser.parse_args()

    if os.geteuid() != 0:
        print(
            "Error: must run as root (sudo).",
            file=sys.stderr,
        )
        sys.exit(1)

    if not BT_SCRIPT.exists():
        print(
            f"Error: bpftrace script not found: " f"{BT_SCRIPT}",
            file=sys.stderr,
        )
        sys.exit(1)

    card = args.card
    if card is None:
        card = find_amdgpu_card()
        if card is None:
            print(
                "Error: no amdgpu device found. " "Use --card N.",
                file=sys.stderr,
            )
            sys.exit(1)
    print(
        f"Using DRM card{card} " f"(/sys/class/drm/card{card}/device/)",
        file=sys.stderr,
    )

    offset_ns = get_clock_offset_ns()
    print(
        f"Clock offset: {offset_ns} ns " f"(mono -> UTC)",
        file=sys.stderr,
    )

    out_fp = sys.stdout
    if args.outfile:
        out_fp = open(args.outfile, "w")

    if args.output == "json":
        formatter = JsonOutput(out_fp)
    elif args.output == "csv":
        formatter = CsvOutput(out_fp)
    else:
        formatter = LiveOutput()

    suppress = set()
    if args.no_bo:
        suppress.update({"bo_create", "bo_move", "bo_pin"})
    if args.no_dma:
        suppress.add("dma_copy")

    sysfs = None
    if not args.no_sysfs:
        sysfs = SysfsPoller(card, args.poll_interval)

    cmd = ["bpftrace", str(BT_SCRIPT)]
    print(
        f"Launching: {' '.join(cmd)}",
        file=sys.stderr,
    )

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    shutdown = threading.Event()

    def handle_signal(signum, frame):
        shutdown.set()
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    def stderr_reader():
        for line in proc.stderr:
            line = line.strip()
            if line:
                print(
                    f"[bpftrace] {line}",
                    file=sys.stderr,
                )

    stderr_t = threading.Thread(target=stderr_reader, daemon=True)
    stderr_t.start()

    if sysfs:
        sysfs.start()

    start_time = time.monotonic()

    try:
        for line in proc.stdout:
            if shutdown.is_set():
                break

            if args.duration is not None:
                elapsed = time.monotonic() - start_time
                if elapsed >= args.duration:
                    break

            record = parse_line(line, offset_ns)
            if record is None:
                continue

            ev = record.get("event", "")

            if ev in suppress:
                continue

            if ev in (
                "tracer_start",
                "tracer_end",
                "heartbeat",
            ):
                if args.output != "live":
                    formatter.write(record)
                if sysfs:
                    for srec in sysfs.drain():
                        formatter.write(srec)
                continue

            if args.pid is not None:
                rec_pid = record.get("pid", "")
                try:
                    if int(rec_pid) != args.pid:
                        continue
                except (ValueError, TypeError):
                    pass

            formatter.write(record)

            if sysfs:
                for srec in sysfs.drain():
                    formatter.write(srec)

    except BrokenPipeError:
        pass
    finally:
        if sysfs:
            sysfs.stop()

        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

        if args.outfile and out_fp != sys.stdout:
            out_fp.close()

    ret = proc.returncode or 0
    print(
        f"\nTracer exited (rc={ret}).",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
