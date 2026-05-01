#!/usr/bin/env python3
"""
AMD GPU Kernel Profiler -- rocprofv3 companion script.

Wraps a HIP/ROCm application with rocprofv3 to capture
per-kernel dispatch and memory copy events with precise
timestamps and kernel names.

Modes:
  wrap   - Launch a command under rocprofv3 profiling
  attach - Attach to a running PID for a duration

Outputs structured JSONL, CSV, or a summary table with
UTC timestamps, kernel names, durations, grid/workgroup
sizes, and memory copy details.

Usage:
    sudo python3 gpu-kernel-profiler.py \\
        --output json --outfile kernels.jsonl \\
        -- ./my_hip_app --arg1

    sudo python3 gpu-kernel-profiler.py \\
        --attach 12345 --duration 10

Requires: rocprofv3 (ROCm 6.x+), Python 3.10+.
"""

import argparse
import csv
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

ROCPROFV3 = shutil.which("rocprofv3") or ("/opt/rocm/bin/rocprofv3")
ROCPROFV3_ATTACH = Path("/opt/rocm/bin/rocprofv3-attach")


def get_clock_offset_ns():
    """
    Compute offset between CLOCK_MONOTONIC and
    CLOCK_REALTIME.  rocprofv3 timestamps are monotonic
    nanoseconds (same basis as bpftrace nsecs).
    """
    import ctypes
    import ctypes.util

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

        ts_mono = Timespec()
        ts_real = Timespec()
        clock_gettime(1, ctypes.byref(ts_mono))
        clock_gettime(0, ctypes.byref(ts_real))

        mono_ns = ts_mono.tv_sec * 10**9 + ts_mono.tv_nsec
        real_ns = ts_real.tv_sec * 10**9 + ts_real.tv_nsec
        return real_ns - mono_ns
    except Exception:
        mono = time.monotonic_ns()
        real = time.time_ns()
        return real - mono


_CLOCK_OFFSET = get_clock_offset_ns()


def ns_to_utc(ns_val):
    """
    Convert a monotonic nanosecond timestamp to UTC.

    rocprofv3 timestamps are monotonic nanoseconds
    (same basis as bpftrace/kernel nsecs).
    """
    real_ns = int(ns_val) + _CLOCK_OFFSET
    ts = real_ns / 1e9
    dt = datetime.fromtimestamp(ts, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def fmt_duration(ns_val):
    """Format nanosecond duration for display."""
    ns = int(ns_val)
    if ns < 1000:
        return f"{ns} ns"
    if ns < 1_000_000:
        return f"{ns / 1000:.1f} us"
    if ns < 1_000_000_000:
        return f"{ns / 1_000_000:.2f} ms"
    return f"{ns / 1_000_000_000:.3f} s"


def fmt_size(b):
    """Format byte count for display."""
    try:
        b = int(b)
    except (ValueError, TypeError):
        return str(b)
    if b < 1024:
        return f"{b} B"
    if b < 1024 * 1024:
        return f"{b / 1024:.1f} KiB"
    if b < 1024**3:
        return f"{b / (1024 * 1024):.1f} MiB"
    return f"{b / (1024**3):.2f} GiB"


def parse_kernel_row(row):
    """
    Parse a kernel trace CSV row into a structured
    record.  Field names are detected dynamically from
    the CSV header.
    """
    start_ns = int(
        row.get(
            "Start_Timestamp",
            row.get("start_timestamp", 0),
        )
    )
    end_ns = int(
        row.get(
            "End_Timestamp",
            row.get("end_timestamp", 0),
        )
    )
    dur_ns = end_ns - start_ns if end_ns > start_ns else 0

    kernel = row.get(
        "Kernel_Name",
        row.get("kernel_name", "unknown"),
    )

    rec = {
        "event": "kernel_dispatch",
        "utc_start": ns_to_utc(start_ns),
        "utc_end": ns_to_utc(end_ns),
        "duration_ns": dur_ns,
        "kernel_name": kernel,
    }

    for key in (
        "Agent_Id",
        "agent_id",
        "Queue_Id",
        "queue_id",
    ):
        if key in row:
            rec["agent_id"] = row[key]
            break

    for key in ("Queue_Id", "queue_id"):
        if key in row:
            rec["queue_id"] = row[key]
            break

    for key in ("Correlation_Id", "correlation_id"):
        if key in row:
            rec["correlation_id"] = row[key]
            break

    wg = []
    for dim in ("X", "Y", "Z"):
        for prefix in (
            f"Workgroup_Size_{dim}",
            f"workgroup_size_{dim}",
        ):
            if prefix in row:
                wg.append(int(row[prefix]))
                break
    if wg:
        rec["workgroup_size"] = wg

    grid = []
    for dim in ("X", "Y", "Z"):
        for prefix in (
            f"Grid_Size_{dim}",
            f"grid_size_{dim}",
        ):
            if prefix in row:
                grid.append(int(row[prefix]))
                break
    if grid:
        rec["grid_size"] = grid

    for key in ("VGPR_Count", "vgpr_count"):
        if key in row:
            try:
                rec["vgpr_count"] = int(row[key])
            except ValueError:
                pass
            break

    for key in ("SGPR_Count", "sgpr_count"):
        if key in row:
            try:
                rec["sgpr_count"] = int(row[key])
            except ValueError:
                pass
            break

    for key in ("LDS_Block_Size", "lds_block_size"):
        if key in row:
            try:
                rec["lds_bytes"] = int(row[key])
            except ValueError:
                pass
            break

    for key in ("Scratch_Size", "scratch_size"):
        if key in row:
            try:
                rec["scratch_bytes"] = int(row[key])
            except ValueError:
                pass
            break

    return rec


def parse_memcopy_row(row):
    """Parse a memory copy trace CSV row."""
    start_ns = int(
        row.get(
            "Start_Timestamp",
            row.get("start_timestamp", 0),
        )
    )
    end_ns = int(
        row.get(
            "End_Timestamp",
            row.get("end_timestamp", 0),
        )
    )
    dur_ns = end_ns - start_ns if end_ns > start_ns else 0

    raw_bytes = row.get("Bytes", row.get("bytes", None))
    nbytes = int(raw_bytes) if raw_bytes else 0

    direction = row.get(
        "Direction",
        row.get("direction", ""),
    )

    rec = {
        "event": "memory_copy",
        "utc_start": ns_to_utc(start_ns),
        "utc_end": ns_to_utc(end_ns),
        "duration_ns": dur_ns,
        "bytes": nbytes,
        "direction": direction,
    }

    for key in (
        "Source_Agent_Id",
        "source_agent_id",
    ):
        if key in row:
            rec["src_agent"] = row[key]
            break

    for key in (
        "Destination_Agent_Id",
        "destination_agent_id",
    ):
        if key in row:
            rec["dst_agent"] = row[key]
            break

    return rec


def parse_alloc_row(row):
    """Parse a memory allocation trace CSV row."""
    start_ns = int(
        row.get(
            "Start_Timestamp",
            row.get("start_timestamp", 0),
        )
    )
    end_ns = int(
        row.get(
            "End_Timestamp",
            row.get("end_timestamp", 0),
        )
    )
    dur_ns = end_ns - start_ns if end_ns > start_ns else 0

    size_val = row.get(
        "Allocation_Size",
        row.get("allocation_size", None),
    )
    alloc_size = int(size_val) if size_val else 0

    operation = row.get(
        "Operation",
        row.get("operation", ""),
    )

    rec = {
        "event": "memory_alloc",
        "utc_start": ns_to_utc(start_ns),
        "utc_end": ns_to_utc(end_ns),
        "duration_ns": dur_ns,
        "operation": operation,
        "size_bytes": alloc_size,
    }

    addr = row.get("Address", row.get("address", None))
    if addr:
        rec["address"] = addr

    for key in ("Agent_Id", "agent_id"):
        if key in row:
            rec["agent_id"] = row[key]
            break

    return rec


def find_output_files(output_dir):
    """
    Locate rocprofv3 CSV output files.  rocprofv3
    writes to <dir>/<hostname>/<pid>/ or directly
    into <dir>/ depending on version.
    """
    kernel_files = sorted(Path(output_dir).rglob("*kernel*trace*.csv"))
    memcopy_files = sorted(Path(output_dir).rglob("*memory*copy*trace*.csv"))
    alloc_files = sorted(Path(output_dir).rglob("*memory*allocation*trace*.csv"))
    return kernel_files, memcopy_files, alloc_files


def load_records(output_dir):
    """
    Load all kernel, memory copy, and allocation
    records from rocprofv3 CSV output, sorted by
    start time.
    """
    kfiles, mfiles, afiles = find_output_files(output_dir)
    records = []

    for kf in kfiles:
        print(
            f"  Reading kernel trace: {kf}",
            file=sys.stderr,
        )
        with open(kf) as f:
            reader = csv.DictReader(f)
            for row in reader:
                rec = parse_kernel_row(row)
                records.append(rec)

    for mf in mfiles:
        print(
            f"  Reading memcopy trace: {mf}",
            file=sys.stderr,
        )
        with open(mf) as f:
            reader = csv.DictReader(f)
            for row in reader:
                rec = parse_memcopy_row(row)
                records.append(rec)

    for af in afiles:
        print(
            f"  Reading alloc trace: {af}",
            file=sys.stderr,
        )
        with open(af) as f:
            reader = csv.DictReader(f)
            for row in reader:
                rec = parse_alloc_row(row)
                records.append(rec)

    records.sort(key=lambda r: r.get("utc_start", ""))
    return records


def output_json(records, fp):
    """Write records as JSONL."""
    for rec in records:
        fp.write(json.dumps(rec) + "\n")
    fp.flush()


def output_csv(records, fp):
    """Write records as CSV."""
    if not records:
        return
    all_keys = set()
    for rec in records:
        all_keys.update(rec.keys())
    fields = sorted(all_keys)
    writer = csv.DictWriter(fp, fieldnames=fields, extrasaction="ignore")
    writer.writeheader()
    for rec in records:
        flat = {}
        for k, v in rec.items():
            if isinstance(v, list):
                flat[k] = "x".join(str(x) for x in v)
            else:
                flat[k] = v
        writer.writerow(flat)
    fp.flush()


def output_summary(records):
    """Print a human-readable summary table."""
    kernels = [r for r in records if r["event"] == "kernel_dispatch"]
    copies = [r for r in records if r["event"] == "memory_copy"]

    print(f"\n{'=' * 72}" f"\n  AMD GPU Kernel Profiler -- Summary" f"\n{'=' * 72}")

    if kernels:
        print(f"\n  Kernel Dispatches: {len(kernels)}")
        total_ns = sum(k["duration_ns"] for k in kernels)
        print(f"  Total GPU time:    {fmt_duration(total_ns)}")

        by_name = {}
        for k in kernels:
            name = k["kernel_name"]
            if name not in by_name:
                by_name[name] = {
                    "count": 0,
                    "total_ns": 0,
                    "min_ns": float("inf"),
                    "max_ns": 0,
                }
            s = by_name[name]
            s["count"] += 1
            s["total_ns"] += k["duration_ns"]
            s["min_ns"] = min(s["min_ns"], k["duration_ns"])
            s["max_ns"] = max(s["max_ns"], k["duration_ns"])

        print(
            f"\n  {'Kernel':<40s}"
            f"  {'Count':>6s}"
            f"  {'Total':>10s}"
            f"  {'Avg':>10s}"
            f"  {'Min':>10s}"
            f"  {'Max':>10s}"
        )
        print(
            f"  {'-' * 40}"
            f"  {'-' * 6}"
            f"  {'-' * 10}"
            f"  {'-' * 10}"
            f"  {'-' * 10}"
            f"  {'-' * 10}"
        )

        for name, s in sorted(
            by_name.items(),
            key=lambda x: x[1]["total_ns"],
            reverse=True,
        ):
            avg_ns = s["total_ns"] // s["count"]
            short = name
            if len(short) > 40:
                short = short[:37] + "..."
            print(
                f"  {short:<40s}"
                f"  {s['count']:>6d}"
                f"  {fmt_duration(s['total_ns']):>10s}"
                f"  {fmt_duration(avg_ns):>10s}"
                f"  {fmt_duration(s['min_ns']):>10s}"
                f"  {fmt_duration(s['max_ns']):>10s}"
            )

    if copies:
        print(f"\n  Memory Copies: {len(copies)}")
        total_bytes = sum(c["bytes"] for c in copies)
        total_ns = sum(c["duration_ns"] for c in copies)
        print(f"  Total bytes:   {fmt_size(total_bytes)}")
        print(f"  Total time:    {fmt_duration(total_ns)}")

        by_dir = {}
        for c in copies:
            d = c.get("direction", "unknown")
            if d not in by_dir:
                by_dir[d] = {
                    "count": 0,
                    "bytes": 0,
                    "ns": 0,
                }
            by_dir[d]["count"] += 1
            by_dir[d]["bytes"] += c["bytes"]
            by_dir[d]["ns"] += c["duration_ns"]

        print(
            f"\n  {'Direction':<24s}"
            f"  {'Count':>6s}"
            f"  {'Bytes':>12s}"
            f"  {'Time':>10s}"
        )
        print(f"  {'-' * 24}" f"  {'-' * 6}" f"  {'-' * 12}" f"  {'-' * 10}")
        for d, s in sorted(by_dir.items()):
            print(
                f"  {d:<24s}"
                f"  {s['count']:>6d}"
                f"  {fmt_size(s['bytes']):>12s}"
                f"  {fmt_duration(s['ns']):>10s}"
            )

    allocs = [r for r in records if r["event"] == "memory_alloc"]

    if allocs:
        allocates = [a for a in allocs if "ALLOCATE" in a.get("operation", "")]
        frees = [a for a in allocs if "FREE" in a.get("operation", "")]
        print(f"\n  Memory Allocations: {len(allocates)}" f"  Frees: {len(frees)}")
        if allocates:
            total_alloc = sum(a.get("size_bytes", 0) for a in allocates)
            print(f"  Total allocated: " f"{fmt_size(total_alloc)}")

            by_agent = {}
            for a in allocates:
                agent = a.get("agent_id", "unknown")
                if agent not in by_agent:
                    by_agent[agent] = {
                        "count": 0,
                        "bytes": 0,
                    }
                by_agent[agent]["count"] += 1
                by_agent[agent]["bytes"] += a.get("size_bytes", 0)

            print(f"\n  {'Agent':<16s}" f"  {'Count':>6s}" f"  {'Total Size':>12s}")
            print(f"  {'-' * 16}" f"  {'-' * 6}" f"  {'-' * 12}")
            for agent, s in sorted(by_agent.items()):
                print(
                    f"  {agent:<16s}"
                    f"  {s['count']:>6d}"
                    f"  {fmt_size(s['bytes']):>12s}"
                )

    if not kernels and not copies and not allocs:
        print("\n  No kernel dispatches or memory " "operations recorded.")
        print("  Make sure the profiled application " "uses HIP/ROCm.")

    print()

    if kernels:
        print(f"  First 20 kernel dispatches:")
        print(f"  {'UTC Start':<28s}" f"  {'Duration':>10s}" f"  {'Kernel Name'}")
        print(f"  {'-' * 28}" f"  {'-' * 10}" f"  {'-' * 40}")
        for k in kernels[:20]:
            short = k["kernel_name"]
            if len(short) > 60:
                short = short[:57] + "..."
            print(
                f"  {k['utc_start']:<28s}"
                f"  {fmt_duration(k['duration_ns']):>10s}"
                f"  {short}"
            )
        if len(kernels) > 20:
            print(f"  ... and {len(kernels) - 20} more")

    print(f"\n{'=' * 72}\n")


def run_wrap_mode(app_cmd, output_dir, extra_flags):
    """
    Launch the application under rocprofv3 profiling.
    """
    cmd = (
        [
            ROCPROFV3,
            "--kernel-trace",
            "--memory-copy-trace",
            "--memory-allocation-trace",
            "-f",
            "csv",
            "-d",
            str(output_dir),
        ]
        + extra_flags
        + ["--"]
        + app_cmd
    )

    print(
        f"Launching: {' '.join(cmd)}",
        file=sys.stderr,
    )

    proc = subprocess.Popen(cmd)

    def handle_signal(signum, frame):
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    proc.wait()
    print(
        f"Application exited (rc={proc.returncode}).",
        file=sys.stderr,
    )
    return proc.returncode


def run_attach_mode(pid, duration_s, output_dir, extra_flags):
    """
    Attach rocprofv3 to a running process for a given
    duration.
    """
    if not ROCPROFV3_ATTACH.exists():
        print(
            "Error: rocprofv3-attach not found at " f"{ROCPROFV3_ATTACH}",
            file=sys.stderr,
        )
        return 1

    duration_ms = int(duration_s * 1000)
    env = os.environ.copy()
    env["ROCPROF_ATTACH_PID"] = str(pid)
    env["ROCPROF_ATTACH_DURATION"] = str(duration_ms)
    env["ROCPROFILER_OUTPUT_PATH"] = str(output_dir)

    cmd = [
        sys.executable,
        str(ROCPROFV3_ATTACH),
    ]

    print(
        f"Attaching to PID {pid} for {duration_s}s ...",
        file=sys.stderr,
    )

    proc = subprocess.Popen(cmd, env=env)

    def handle_signal(signum, frame):
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    proc.wait()
    print(
        f"Attach finished (rc={proc.returncode}).",
        file=sys.stderr,
    )
    return proc.returncode


def main():
    parser = argparse.ArgumentParser(
        description=(
            "AMD GPU Kernel Profiler -- rocprofv3 "
            "companion for per-kernel dispatch and "
            "memory copy tracking."
        ),
    )
    parser.add_argument(
        "--attach",
        type=int,
        default=None,
        metavar="PID",
        help="Attach to a running process PID",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=10.0,
        help=("Profiling duration in seconds " "(attach mode, default: 10)"),
    )
    parser.add_argument(
        "--output",
        choices=["json", "csv", "summary"],
        default="summary",
        help="Output format (default: summary)",
    )
    parser.add_argument(
        "--outfile",
        default=None,
        help="Output file path (default: stdout)",
    )
    parser.add_argument(
        "--keep-raw",
        action="store_true",
        help="Keep raw rocprofv3 output directory",
    )
    parser.add_argument(
        "--raw-dir",
        default=None,
        help=("Directory for rocprofv3 raw output " "(default: temp dir)"),
    )
    parser.add_argument(
        "--stats",
        action="store_true",
        help="Include rocprofv3 --stats flag",
    )
    parser.add_argument(
        "app_cmd",
        nargs="*",
        help=("Application command to profile " "(wrap mode)"),
    )

    args, unknown = parser.parse_known_args()

    if args.attach is None and not args.app_cmd:
        parser.print_help()
        print(
            "\nError: provide either --attach PID " "or a command to profile.",
            file=sys.stderr,
        )
        sys.exit(1)

    if args.attach is not None and args.app_cmd:
        print(
            "Error: cannot use both --attach and " "a command.",
            file=sys.stderr,
        )
        sys.exit(1)

    if args.raw_dir:
        output_dir = Path(args.raw_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        cleanup = False
    else:
        tmpdir = tempfile.mkdtemp(prefix="gpu-kernel-profiler-")
        output_dir = Path(tmpdir)
        cleanup = not args.keep_raw

    extra_flags = []
    if args.stats:
        extra_flags.append("--stats")

    print(
        f"rocprofv3 output dir: {output_dir}",
        file=sys.stderr,
    )

    try:
        if args.attach is not None:
            rc = run_attach_mode(
                args.attach,
                args.duration,
                output_dir,
                extra_flags,
            )
        else:
            rc = run_wrap_mode(
                args.app_cmd,
                output_dir,
                extra_flags,
            )

        print(
            "Parsing profiler output ...",
            file=sys.stderr,
        )
        records = load_records(output_dir)
        print(
            f"Found {len(records)} events.",
            file=sys.stderr,
        )

        out_fp = sys.stdout
        if args.outfile:
            out_fp = open(args.outfile, "w")

        if args.output == "json":
            output_json(records, out_fp)
        elif args.output == "csv":
            output_csv(records, out_fp)
        else:
            output_summary(records)

        if args.outfile and out_fp != sys.stdout:
            out_fp.close()
            print(
                f"Wrote {len(records)} records to " f"{args.outfile}",
                file=sys.stderr,
            )

    finally:
        if cleanup:
            import shutil as _shutil

            _shutil.rmtree(output_dir, ignore_errors=True)
        elif args.keep_raw or args.raw_dir:
            print(
                f"Raw output kept at: {output_dir}",
                file=sys.stderr,
            )

    sys.exit(rc if rc else 0)


if __name__ == "__main__":
    main()
