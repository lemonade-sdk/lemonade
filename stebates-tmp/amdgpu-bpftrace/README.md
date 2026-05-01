# AMD GPU Kernel Tracer & Profiler

Two complementary tools for tracking GPU activity on
AMD GPUs:

1. **gpu-kernel-tracer** (bpftrace) -- lightweight
   kernel-level tracing of driver events, memory
   operations, and compute active/idle transitions
2. **gpu-kernel-profiler** (rocprofv3) -- per-kernel
   dispatch profiling with kernel names, durations,
   grid/workgroup sizes, and memory copy details

## Why two tools?

HIP/ROCm compute kernels use user-mode AQL queues,
bypassing the kernel driver for individual dispatches.
The kernel driver only sees coarse events (compute
active/idle transitions, memory operations, DMA copies).

| What you want | Use |
|---|---|
| Per-kernel names + timing | `gpu-kernel-profiler` |
| Memory ops / DMA / BO tracking | `gpu-kernel-tracer` |
| Compute active/idle boundaries | `gpu-kernel-tracer` |
| sysfs GPU utilization polling | `gpu-kernel-tracer` |
| Zero-overhead always-on tracing | `gpu-kernel-tracer` |

## Prerequisites

### gpu-kernel-tracer (bpftrace)

- Linux kernel with BTF support (5.x+)
- `bpftrace` >= 0.20 with kfunc support
- `amdgpu` kernel module loaded
- Python 3.10+
- Root access (sudo)

### gpu-kernel-profiler (rocprofv3)

- ROCm 6.x+ with `rocprofv3` installed
- Python 3.10+
- Root access (sudo, or profiling user's own process)

### Install bpftrace

```bash
# Ubuntu/Debian
sudo apt install bpftrace

# Arch
sudo pacman -S bpftrace

# Fedora
sudo dnf install bpftrace
```

---

## gpu-kernel-tracer (bpftrace)

### Features

- **Job lifecycle**: submit, schedule, fence start,
  fence completion -- with UTC timestamps
- **KFD compute boundaries**: detect when GPU enters
  and exits active compute state
- **Data transfers**: buffer object creation, VRAM/GTT
  migration, explicit DMA copies, KFD IB submissions
- **sysfs polling**: GPU utilization, VRAM usage, PCIe
  link speed/width
- **Structured output**: JSONL, CSV, or live terminal
- **PID filtering**: trace only a specific process
- **Near-zero overhead**: eBPF in kernel space

### Quick Start

```bash
# Live terminal table (default)
sudo python3 gpu-kernel-tracer.py

# JSONL output to file
sudo python3 gpu-kernel-tracer.py \
    --output json --outfile trace.jsonl

# CSV output, 10 seconds, specific PID
sudo python3 gpu-kernel-tracer.py \
    --output csv --duration 10 --pid 1234

# Standalone bpftrace (raw pipe-delimited)
sudo bpftrace gpu-kernel-tracer.bt
```

### CLI Reference

```
sudo python3 gpu-kernel-tracer.py [OPTIONS]

Options:
  --card N           DRM card number (default: auto)
  --output FORMAT    json | csv | live (default: live)
  --outfile PATH     Output file (default: stdout)
  --pid PID          Filter events to this PID
  --poll-interval S  sysfs poll interval (default: 1.0)
  --no-sysfs         Disable sysfs polling
  --no-bo            Suppress BO create/move/pin events
  --no-dma           Suppress DMA copy events
  --duration S       Run for S seconds then exit
```

### Probes Reference

#### Job Lifecycle (traditional GEM/CS path)

| Probe | What it captures |
|---|---|
| `amdgpu_job_submit` | Job submitted to scheduler |
| `amdgpu_ib_schedule` | IB scheduled to ring |
| `amdgpu_fence_update_start_timestamp` | GPU HW start |
| `amdgpu_fence_process` | Fence signaled |
| `drm_sched_job_done` | Scheduler job completion |
| `amdgpu_ring_commit` | Ring doorbell |

#### KFD Compute (HIP/ROCm path)

| Probe | What it captures |
|---|---|
| `kfd_inc_compute_active` | GPU enters compute |
| `kfd_dec_compute_active` | GPU exits compute |
| `amdgpu_amdkfd_submit_ib` | KFD indirect buffer submit |

#### Data Transfer / Memory

| Probe | What it captures |
|---|---|
| `amdgpu_bo_create` | Buffer object allocation |
| `amdgpu_bo_move` | Buffer migration (VRAM/GTT) |
| `amdgpu_lsdma_copy_mem` | Explicit DMA copy |
| `amdgpu_bo_pin` | Buffer pinning |

### Ring Name Prefixes

- `gfx_*` -- graphics/compute engine
- `comp_*` -- compute-only rings
- `sdma*` -- DMA/copy engine
- `vcn*` -- video codec engine
- `jpeg*` -- JPEG decoder

### sysfs Metrics

| sysfs file | Metric |
|---|---|
| `gpu_busy_percent` | GPU utilization % |
| `mem_info_vram_used` | Current VRAM usage |
| `mem_info_vram_total` | Total VRAM |
| `vcn_busy_percent` | Video codec utilization % |
| `current_link_speed` | PCIe link speed |
| `current_link_width` | PCIe lane count |

---

## gpu-kernel-profiler (rocprofv3)

### Features

- **Per-kernel dispatch tracking**: kernel name, UTC
  start/end, duration, grid/workgroup sizes
- **Memory copy tracking**: direction, bytes, duration
- **Summary statistics**: per-kernel aggregates (count,
  total time, avg/min/max duration)
- **Two modes**: wrap a command or attach to a running
  PID
- **Structured output**: JSONL, CSV, or summary table

### Quick Start

```bash
# Profile a command, show summary (default)
sudo python3 gpu-kernel-profiler.py \
    -- ./my_hip_app --arg1

# JSONL output to file
sudo python3 gpu-kernel-profiler.py \
    --output json --outfile kernels.jsonl \
    -- ./my_hip_app

# Attach to running process for 10 seconds
sudo python3 gpu-kernel-profiler.py \
    --attach 12345 --duration 10

# Keep raw rocprofv3 output for debugging
sudo python3 gpu-kernel-profiler.py \
    --keep-raw --raw-dir ./raw_profile \
    -- ./my_hip_app
```

### CLI Reference

```
sudo python3 gpu-kernel-profiler.py [OPTIONS] \
    [-- COMMAND ...]

Options:
  --attach PID       Attach to a running process
  --duration S       Profiling duration for attach
                     mode (default: 10)
  --output FORMAT    json | csv | summary
                     (default: summary)
  --outfile PATH     Output file (default: stdout)
  --keep-raw         Keep raw rocprofv3 CSV files
  --raw-dir PATH     Directory for raw output
  --stats            Include rocprofv3 --stats
```

### Output Formats

#### Summary (default)

```
========================================
  AMD GPU Kernel Profiler -- Summary
========================================

  Kernel Dispatches: 1024
  Total GPU time:    123.45 ms

  Kernel                 Count  Total    Avg
  ---------------------  -----  -------  -------
  void matmul_kernel...    512  98.2 ms  191 us
  void relu_kernel...      512  25.3 ms   49 us

  Memory Copies: 128
  Total bytes:   512.0 MiB
  Total time:    45.67 ms
========================================
```

#### JSONL (`--output json`)

```json
{"event": "kernel_dispatch",
 "utc_start": "2026-03-12T20:15:27.123Z",
 "utc_end": "2026-03-12T20:15:27.234Z",
 "duration_ns": 111000000,
 "kernel_name": "void matmul_kernel<float>(...)",
 "workgroup_size": [256, 1, 1],
 "grid_size": [65536, 1, 1]}
{"event": "memory_copy",
 "utc_start": "2026-03-12T20:15:27.000Z",
 "utc_end": "2026-03-12T20:15:27.012Z",
 "duration_ns": 12000000,
 "bytes": 67108864,
 "direction": "HOST_TO_DEVICE"}
```

---

## Architecture

```
┌──────────────────────────────────────┐
│  gpu-kernel-tracer.bt                │
│  (bpftrace kfunc probes)             │
│                                      │
│  Job lifecycle · KFD active/idle ·   │
│  BO create/move · DMA copies         │
│  → pipe-delimited stdout             │
└──────────┬───────────────────────────┘
           │ stdout
┌──────────▼───────────────────────────┐
│  gpu-kernel-tracer.py                │
│                                      │
│  Parse · mono→UTC · sysfs poll ·     │
│  PID filter · JSONL/CSV/live         │
└──────────────────────────────────────┘

┌──────────────────────────────────────┐
│  gpu-kernel-profiler.py              │
│                                      │
│  Wraps rocprofv3 --kernel-trace      │
│  --memory-copy-trace                 │
│                                      │
│  Per-kernel names + timing           │
│  Per-copy bytes + direction          │
│  Summary / JSONL / CSV               │
└──────────────────────────────────────┘
```

## Tested On

- AMD Radeon RX 9070 XT (gfx1201, RDNA4)
- Linux 6.17.0, ROCm 7.2.0
- bpftrace 0.20.2, rocprofv3 (ROCm 7.2.0)

## License

Apache 2.0 (same as parent project)
