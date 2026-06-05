# FastFlowLM Release

Public version.

## Installation & Deployment

### Prerequisites
This application requires the **Microsoft Visual C++ Redistributable for Visual Studio 2015-2022** to run on target computers.

### Quick Installation
1. **Download Visual C++ Redistributable:**
   - x64: https://aka.ms/vs/17/release/vc_redist.x64.exe
   - x86: https://aka.ms/vs/17/release/vc_redist.x86.exe

2. **Install as Administrator:**
   - Run the downloaded installer as Administrator
   - Restart computer if prompted

3. **Alternative installation methods:**
   ```powershell
   # Using Windows Package Manager
   winget install Microsoft.VCRedist.2015+.x64

   # Using Chocolatey
   choco install vcredist140
   ```


### Building with Static Linking (Recommended)
To avoid the MSVCP140.dll dependency entirely, build with static linking:

```bash
# Download submodule (tokenizer)
git submodule update --init --recursive

# Clean previous build
rm -rf build/

# Configure with static linking
cmake -B build -S . -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded

# Build
cmake --build build --config Release
```

**Comprehensive Static Linking (New):**
The CMakeLists.txt now includes comprehensive static linking that attempts to statically link:
- Visual C++ Runtime libraries
- Windows system libraries (kernel32, user32, etc.)
- Network libraries (ws2_32, wininet, etc.)
- Cryptography libraries (crypt32, bcrypt, etc.)

```bash
# Build with comprehensive static linking
cmake -B build -S .
cmake --build build --config Release

# Check remaining DLL dependencies
cmake --build build --target check_dependencies
```

**Note:** Some custom libraries (XRT, NPU libraries) may still require DLLs if static versions aren't available.

### Creating Deployment Package
Use the provided deployment script:

```powershell
# Create deployment package
.\deploy.ps1

# Or specify custom directories
.\deploy.ps1 -BuildDir "build" -OutputDir "deploy"
```

The deployment package will include:
- `flm.exe` - Main executable
- All required DLLs from `lib/` directory
- `model_list.json` - Model configuration
- `INSTALLATION.md` - Installation instructions
- `run_flm.bat` - Easy execution script

### Troubleshooting

**"MSVCP140.dll not found" error:**
1. Install the Visual C++ Redistributable (see Prerequisites above)
2. Ensure you're using the correct architecture (x64/x86)
3. Try running as Administrator
4. Check if antivirus is blocking DLL files

**Other common issues:**
- If you get "VCRUNTIME140.dll not found", install the same Visual C++ Redistributable
- For "libcurl.dll not found", ensure all DLLs from the `lib/` directory are present
- For AMD XDNA/GPU related errors, ensure proper drivers are installed

**Finding Static Library Alternatives:**
Use the provided script to identify static library alternatives:
```powershell
.\find_static_libs.ps1
```

This will help you find static versions of your custom libraries to further reduce DLL dependencies.

### Development
```bash
# Build for development
cmake -B build -S .
cmake --build build --config Debug

# Run tests (if available)
ctest --test-dir build
```
