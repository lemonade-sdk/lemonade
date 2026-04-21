# llama.cpp Backend Options

Lemonade uses [llama.cpp](https://github.com/ggerganov/llama.cpp) as its primary LLM inference backend, supporting multiple hardware acceleration options. This document explains the available backends and how to choose between them.

## Available Backends

### CPU
- **Platform**: Windows, Linux, macOS
- **Hardware**: All x86_64 processors
- **Use Case**: Universal fallback, no GPU required
- **Performance**: Slowest option, suitable for small models or testing
- **Installation**: Automatically available via upstream llama.cpp releases

### Vulkan
- **Platform**: Windows, Linux
- **Hardware**: AMD GPUs (iGPU and dGPU), NVIDIA GPUs, Intel GPUs
- **Use Case**: Cross-vendor GPU acceleration
- **Performance**: Good performance across all GPU vendors
- **Installation**: Automatically available via upstream llama.cpp releases
- **Notes**: Recommended for most GPU users

### ROCm
- **Platform**: Windows, Linux
- **Hardware**: AMD Radeon RX 6000/7000 series (RDNA2/RDNA3/RDNA4), AMD Ryzen AI iGPUs (Strix Point/Halo)
- **Use Case**: AMD GPU-optimized inference
- **Performance**: Optimized for AMD hardware, may outperform Vulkan on supported GPUs
- **Channel Options**:
  - **Preview** (default): Custom builds with latest optimizations from lemonade-sdk
  - **Stable**: Upstream llama.cpp releases with AMD ROCm support
  - **Nightly**: Bleeding-edge builds from lemonade-sdk/llamacpp-rocm (experimental)
- **Installation**: Varies by channel (see below)

### Metal
- **Platform**: macOS only
- **Hardware**: Apple Silicon (M1/M2/M3/M4) and Intel Macs with Metal support
- **Use Case**: macOS GPU acceleration
- **Performance**: Optimized for Apple Silicon
- **Installation**: Automatically available via upstream llama.cpp releases

### System
- **Platform**: Linux only
- **Hardware**: Depends on system-installed llama-server binary
- **Use Case**: Advanced users with custom llama.cpp builds
- **Performance**: Depends on build configuration
- **Installation**: Requires manual installation of `llama-server` in system PATH
- **Notes**: Not enabled by default; set `LEMONADE_LLAMACPP_PREFER_SYSTEM=true` in config

## ROCm Channel Configuration

The ROCm backend supports three channels to balance stability, performance, and access to latest features:

### Preview Channel (Default)
```json
{
  "rocm_channel": "preview"
}
```
- **Source**: Custom builds from [lemonade-sdk/llama.cpp](https://github.com/lemonade-sdk/llama.cpp)
- **Binaries**: Common builds for supported architectures
- **Updates**: Frequent updates with latest optimizations and fixes
- **Platform**: Windows and Linux
- **Runtime**: Requires runtime for both Windows and Linux to be installed separately.
- **Best For**: Users who want the latest performance optimizations

### Stable Channel
```json
{
  "rocm_channel": "stable"
}
```
- **Source**: Upstream [llama.cpp](https://github.com/ggerganov/llama.cpp) releases
- **Binaries**:
  - **Windows**: Self-contained HIP binaries (no separate runtime needed)
  - **Linux**: Binaries built against ROCm 7.2 runtime
- **Updates**: Follows upstream llama.cpp release cycle
- **Platform**: Windows and Linux
- **Runtime**:
  - Windows: Self-contained, no runtime installation required
  - Linux: Downloads AMD ROCm 7.2.1 runtime if not present at `/opt/rocm`
- **Best For**: Users who prefer stable, tested releases aligned with upstream

### Nightly Channel
```json
{
  "rocm_channel": "nightly"
}
```
- **Source**: Nightly builds from [lemonade-sdk/llamacpp-rocm](https://github.com/lemonade-sdk/llamacpp-rocm)
- **Binaries**: Architecture-specific builds (gfx1150, gfx1151, gfx103X, gfx110X, gfx120X)
- **Updates**: Nightly builds with experimental features and latest upstream changes
- **Platform**: Windows and Linux
- **Runtime**: Bundled runtime on Linux, TheRock ROCm dependencies
- **Best For**: Developers and testers who want bleeding-edge features and are comfortable with potential instability

### Changing Channels

To switch between channels, update your `config.json`:

```json
{
  "rocm_channel": "stable"
}
```

Or use the Lemonade CLI:
```bash
# Switch to stable channel
lemonade config set rocm_channel=stable

# Switch to preview channel (default)
lemonade config set rocm_channel=preview

# Switch to nightly channel (experimental)
lemonade config set rocm_channel=nightly
```

After changing channels, you'll need to reinstall the ROCm backend:
```bash
lemonade backend install llamacpp:rocm
```

## Choosing the Right Backend

### Decision Tree

1. **Do you have an NVIDIA or Intel GPU?**
   - Use **Vulkan**

2. **Do you have an AMD GPU?**
   - **For Radeon RX 6000/7000 or Ryzen AI iGPU**:
     - Try **ROCm** first for best performance
     - Fall back to **Vulkan** if you encounter issues
   - **For older AMD GPUs (RX 5000 and earlier)**:
     - Use **Vulkan** (ROCm not supported)

3. **Do you have Apple Silicon?**
   - Use **Metal**

4. **No GPU or unsupported GPU?**
   - Use **CPU**

### ROCm Channel Selection

- **Use Preview** if you:
  - Want the best performance on AMD hardware
  - Are comfortable with frequent updates
  - Are testing new models or features

- **Use Stable** if you:
  - Prefer stability over latest features
  - Want upstream llama.cpp compatibility
  - Are deploying in production

- **Use Nightly** if you:
  - Want bleeding-edge experimental features
  - Are testing unreleased llama.cpp functionality
  - Are comfortable with potential bugs and instability
  - Are a developer contributing to lemonade or llama.cpp

## Platform Specifics

### Linux
- All backends supported (CPU, Vulkan, ROCm, System)
- ROCm requires compatible AMD GPU (see above)
- System backend requires manual llama-server installation

### Windows
- Supported: CPU, Vulkan, ROCm
- ROCm requires compatible AMD GPU
- No system backend support

### macOS
- Supported: CPU, Metal
- Metal recommended for all Macs with Metal support
