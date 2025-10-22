# Building Ryzen AI LLM Server

This document describes how to build the Ryzen AI LLM Server from source.

## Prerequisites

### Required Software

1. **Windows 10/11** (64-bit)
2. **Visual Studio 2022** (or Visual Studio Build Tools 2022)
   - Install the "Desktop development with C++" workload
3. **CMake 3.20 or later**
   - Download from https://cmake.org/download/
4. **Ryzen AI Software 1.6.0** with LLM patch
   - Must be installed at `C:\Program Files\RyzenAI\1.6.0`
   - Download from: https://ryzenai.docs.amd.com

### Hardware Requirements

- AMD Ryzen AI 300-series processor (for NPU execution)
- Minimum 16GB RAM (32GB recommended for larger models)

## Build Instructions

### Using CMake (Command Line)

1. Open a **Developer Command Prompt for VS 2022**

2. Navigate to the project directory:
   ```cmd
   cd c:\work\lsdk\lemonade\src\ryzenai-serve
   ```

3. Create a build directory:
   ```cmd
   mkdir build
   cd build
   ```

4. Configure the project:
   ```cmd
   cmake .. -G "Visual Studio 17 2022" -A x64
   ```

   Or, if Ryzen AI is installed in a custom location:
   ```cmd
   cmake .. -G "Visual Studio 17 2022" -A x64 -DOGA_ROOT="C:\path\to\RyzenAI\1.6.0"
   ```

5. Build the project:
   ```cmd
   cmake --build . --config Release
   ```

6. The executable will be created at:
   ```
   build\bin\Release\ryzenai-serve.exe
   ```

### Using Visual Studio IDE

1. Open Visual Studio 2022

2. Select **Open a local folder**

3. Navigate to and select: `c:\work\lsdk\lemonade\src\ryzenai-serve`

4. Visual Studio will automatically detect CMakeLists.txt and configure the project

5. Select **Build > Build All** or press `Ctrl+Shift+B`

6. The executable will be in: `out\build\x64-Release\bin\ryzenai-serve.exe`

### Using CMake GUI

1. Open CMake GUI

2. Set **Where is the source code** to:
   ```
   C:\work\lsdk\lemonade\src\ryzenai-serve
   ```

3. Set **Where to build the binaries** to:
   ```
   C:\work\lsdk\lemonade\src\ryzenai-serve\build
   ```

4. Click **Configure**
   - Select "Visual Studio 17 2022" as the generator
   - Select "x64" as the platform

5. Click **Generate**

6. Click **Open Project** to open in Visual Studio, or build from command line

## Troubleshooting

### CMake cannot find Ryzen AI

If CMake reports:
```
Ryzen AI not found at C:/Program Files/RyzenAI/1.6.0
```

Solutions:
1. Verify Ryzen AI is installed correctly
2. Set the `OGA_ROOT` variable:
   ```cmd
   cmake .. -DOGA_ROOT="C:\your\custom\path\to\RyzenAI\1.6.0"
   ```

### Missing DLLs when running

If you get DLL errors when running the executable, ensure all required DLLs are copied:
- `onnxruntime_genai.dll`
- `onnxruntime.dll`
- `onnxruntime_vitis_ai_custom_ops.dll` (for NPU)
- `onnx_custom_ops.dll` (for Hybrid)

These should be automatically copied to the same directory as the executable during build.

### Build fails with C++ standard errors

Ensure you're using Visual Studio 2022 or later, which supports C++17.

### Header-only libraries not downloading

If the build fails to download cpp-httplib or nlohmann/json:
1. Check your internet connection
2. Manually download the headers:
   - cpp-httplib: https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.14.3/httplib.h
   - nlohmann/json: https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
3. Place them in:
   - `external/cpp-httplib/httplib.h`
   - `external/json/json.hpp`

## Running Tests

After building, test the server with a model:

```cmd
cd build\bin\Release
ryzenai-serve.exe -m "C:\path\to\onnx\model" --verbose
```

Example with a real model:
```cmd
ryzenai-serve.exe -m "C:\Users\YourName\.cache\huggingface\hub\models--microsoft--phi-3-mini-4k-instruct-onnx\snapshots\<hash>" --mode hybrid
```

## Development Build

For development with debug symbols:

```cmd
cmake --build . --config Debug
```

The debug executable will be at:
```
build\bin\Debug\ryzenai-serve.exe
```

## Clean Build

To start fresh:

```cmd
cd build
cmake --build . --target clean
```

Or delete the entire build directory:

```cmd
cd ..
rmdir /s /q build
```

## Next Steps

After building successfully, refer to [README.md](README.md) for usage instructions and API documentation.

