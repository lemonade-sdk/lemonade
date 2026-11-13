---
license: other
tags:
  - lemonade
  - llama.cpp
  - backends
---

# Lemonade backends mirror / Lemonade 推理后端镜像

This repository is the official mirror of the backend files that [Lemonade](https://github.com/lemonade-sdk/lemonade) downloads, for users who cannot reach GitHub reliably. Every file is copied unchanged from the project's GitHub release and checked against the SHA-256 that GitHub publishes.

本仓库是 [Lemonade](https://github.com/lemonade-sdk/lemonade) 所需推理后端的官方镜像，供无法稳定访问 GitHub 的用户使用。所有文件均与对应项目的 GitHub Release 完全一致，并已按 GitHub 公布的 SHA-256 校验。

## Layout / 目录结构

```
github/<owner>/<repo>/<version>/<file>   GitHub release files / GitHub Release 文件
therock/<version>/<file>                 ROCm runtime / ROCm 运行时
<each version folder>/assets.json        name, size and SHA-256 of every file / 每个文件的名称、大小和 SHA-256
```

Only the versions that a Lemonade release pins are mirrored. A sync job updates this repository when Lemonade changes a pin.

仅镜像 Lemonade 发布版本所固定的后端版本，Lemonade 更新版本时自动同步。

## Use it / 使用方法

Point Lemonade at this mirror once, and it downloads every backend from here instead of GitHub:

只需设置一次，Lemonade 即会从本镜像而非 GitHub 下载所有推理后端：

```
lemonade config set backend_source=modelscope
lemonade config set default_model_source=modelscope
```

You can also download a file from the Files tab, unzip it, and point the backend at the folder, for example `lemonade config set llamacpp.vulkan_bin=C:\path\to\folder`.

也可以在“数据集文件”页下载文件，解压后在 Lemonade 中指定路径。

## Licenses / 许可证

Each folder keeps the license of its upstream project, for example llama.cpp, whisper.cpp and stable-diffusion.cpp (MIT), FastFlowLM, Ryzen AI and ROCm. See each project's repository.

各目录沿用其上游项目的许可证，详见各项目仓库。
