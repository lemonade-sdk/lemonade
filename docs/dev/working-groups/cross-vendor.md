# Working Group: Cross-Vendor Support

## Overview

**Lead:** This working group is led by Ken VanDine, whose handle is @kenvandine on both GitHub and Discord.

**Background:** Lemonade supports a range of hardware and operatings systems (collectively referred to as platforms). However, achieving full support across all mass-market platforms requires additional compilation targets, platform-specific backend support, etc.

**Why:** Developers are far more likely to build on Lemonade if it enables their applications to deploy across all relevant platforms.

**Goal:** Lemonade works on all mass-market platforms and delivers optimized performance on each.

## Contributing

Please see the general [contribution guidelines](../contribute.md), then contact @kenvandine on Discord to get started.

## Maintanance

Please see the [contribution guide](../contribute.md#maintainers) to see who is responsible for maintaining each backend. Working groups are focused areas of roadmap development, and are a subset of the end-to-end maintainance and support of this project.

## Roadmap

> Roadmap items may be high-level objectives that may span multiple issues and PRs.

### Vendor-Neutral Vulkan

- [ ] LLMs via llama.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 | x       | x     |
| ARM |         |       |

- [ ] Image generation and editing via stable-diffusion.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |    x    |   x   |
| ARM |         |       |

- [ ] Realtime transcription via whisper.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |         |   x   |
| ARM |         |       |


### AMD ROCm

- [x] LLMs via llama.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |     x   |   x   |

- [x] Image generation and editing via stable-diffusion.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |    x    |    x  |

- [ ] Realtime transcription via whisper.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |     x   |       |

### Nvidia CUDA

- [ ] LLMs via llama.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 | x       | x     |
| ARM |         |       |

- [ ] Image generation and editing via stable-diffusion.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |         |       |
| ARM |         |       |

- [ ] Realtime transcription via whisper.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |         |       |
| ARM |         |       |

### Intel OpenVino

- [ ] LLMs via llama.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |         |       |

- [ ] Image generation and editing via stable-diffusion.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |         |       |

- [ ] Realtime transcription via whisper.cpp

|     | Windows | Linux |
|-----|---------|-------|
| x86 |         |       |

### Qualcomm QNN

- [ ] LLMs via llama.cpp

|     | Windows | Linux |
|-----|---------|-------|
| ARM |         |       |

- [ ] Image generation and editing via stable-diffusion.cpp

|     | Windows | Linux |
|-----|---------|-------|
| ARM |         |       |

- [ ] Realtime transcription via whisper.cpp

|     | Windows | Linux |
|-----|---------|-------|
| ARM |         |       |

### Apple Silicon Metal

- [x] LLMs via llama.cpp
- [x] Image generation and editing via stable-diffusion.cpp
- [x] Realtime transcription via whisper.cpp
