# Windows Setup and Build Instructions

## Preparing WSL Side

### mlir-aie tools: WSL Ubuntu 24.04
All steps in WSL Ubuntu terminal.
1. Open powershell as Administrator:
    - Install WSL2 and Ubuntu 22.04
        ```
        wsl --install
        wsl --install -d Ubuntu-22.0
        ```


2. Prepare WSL2 with Ubuntu 24.04:
    - Install packages (after apt-get update):
        ```
        sudo apt install \
        build-essential clang clang-14 lld lld-14 cmake \
        python3-venv python3-pip \
        libxrender1 libxtst6 libxi6 \
        mingw-w64-tools \
        gcc-13 g++-13
        ```
    - generate locales
        ```
        apt-get install locales
        locale-gen en_US.UTF-8
        ```
3. Clone [https://github.com/Xilinx/mlir-aie.git](https://github.com/Xilinx/mlir-aie.git) best under /home/username for speed (yourPathToBuildMLIR-AIE), with submodules:
    ```
    git clone --recurse-submodules https://github.com/Xilinx/mlir-aie.git
    ````
4. Install mlir-aie tools under WSL2:
    -  Use quick setup script to install from whls:
        ```
        cd mlir-air
        source utils/quick_setup.sh
        ```
5. Build XRT dll definition file to be used to create .lib needed for host code compilation with Visual Studio C/C++ compiler. After installing the updated Ryzen™ AI driver (see next subsection), use the gendef tool (from the mingw-w64-tools package) to create a .def file with the required link symbols. This step is needed to create an XRT dll def file that we can link against when we compile.
    ```
    mkdir /mnt/c/dev
    mkdir /mnt/c/dev/xrtNPUfromDLL
    cd /mnt/c/dev/xrtNPUfromDLL
    cp /mnt/c/Windows/System32/xrt_coreutil.dll .
    gendef xrt_coreutil.dll
    ```
6. Clone [XRT](https://github.com/Xilinx/XRT) under C:\dev\XRT.
    ```
    cd /mnt/c/dev
    git clone https://github.com/Xilinx/XRT.git
    ```


## Prepare Host Side: Natively on Win11

All steps in Win11 (powershell where needed).

1. Upgrade the NPU driver to the latest version. Navigate to [here](https://ryzenai.docs.amd.com/en/latest/inst.html#install-npu-drivers) under `NPU Driver` to download and install the driver.

   Note that we currently have two steps for setting up the driver for host compilation and linking. The driver installation provides the `xrt_coreutil.dll` under `C:\Windows\System32\` or `C:\Windows\System32\AMD` which is needed to generate the `xrt_coreutil.lib` that Visual Studio uses to compile against.

2. Install [Microsoft Visual Studio 17 2022 Community Edition](https://visualstudio.microsoft.com/vs/community/) with package for C++ development.

3. Install CMake on windows ([https://cmake.org/download/](https://cmake.org/download/)) which should include adding CMake to your PATH environment variable (e.g. `C:\Program Files\CMake\bin`)

4. Create visual studio XRT lib file for host code linking. This is done by creating a .lib file from the .dll shipped with the driver (along with generated .def file above).
    - In wsl, generate a .def file (see above)
    - Start a x86 Native Tools Command Prompt (installed as part of VS17), go to the folder `C:\dev\xrtNPUfromDLL` and run command:
      ```
      lib /def:xrt_coreutil.def /machine:x64 /out:xrt_coreutil.lib
      ```

5. Install Boost under `C:\Program Files\boost\boost_1_88_0`.
    - Create  the boost directory.
        ```
        mkdir 'C:\Program Files\boost'
        cd 'C:\Program Files\boost'
        ```
    -  Download the [Boost 1.88.0](https://www.boost.org/releases/1.88.0/) ZIP archive here. Then extract its contents to `C:\Program Files\boost\boost_1_88_0`.


6. Clone vcpkg under `C:\dev`.
    ```
    cd 'C:\dev'
    git clone https://github.com/microsoft/vcpkg.git
    ```

7. Install ffmpeg.
    ```
    # install Chocolatey (if you haven't already)
    Set-ExecutionPolicy Bypass -Scope Process -Force; `
    [System.Net.ServicePointManager]::SecurityProtocol = `
    [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; `
    iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))

    # Using Chocolatey
    choco install ffmpeg[zlib]
    ```


## Quick Start

1. Open a powershell

2. Go to the FastFlowLM dir

3. Type `make run`
