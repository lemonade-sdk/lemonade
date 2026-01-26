project('lemonade', ['cpp', 'c'],
  version: '9.1.4',
  default_options: [
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=release',
  ],
  license: 'Apache-2.0',
  meson_version: '>=1.2.0'
)

# Project information
project_version = meson.project_version()
project_name = meson.project_name()

# Platform detection
is_windows = host_machine.system() == 'windows'
is_macos = host_machine.system() == 'darwin'
is_linux = host_machine.system() == 'linux'

# Import modules
fs = import('fs')

# Summary information
summary({
  'Version': project_version,
  'Platform': host_machine.system(),
  'CPU': host_machine.cpu(),
}, section: 'Project Info')

# ============================================================
# Node.js/Electron Component Configuration
# ============================================================

include_electron = get_option('electron')

if include_electron
  # Find Node.js and npm - required for Electron build
  node = find_program('node', required: true)
  npm = find_program('npm', required: true)

  electron_src_dir = meson.current_source_dir() / 'src' / 'app'
  electron_build_dir = electron_src_dir / 'dist-app'

  node_version = run_command(node, '--version', check: true).stdout().strip()
  npm_version = run_command(npm, '--version', check: true).stdout().strip()

  message('Node.js @0@ found'.format(node_version))
  message('npm @0@ found'.format(npm_version))

  # Platform-specific Electron build targets
  if is_windows
    electron_build_target = 'build:win'
    electron_output_dir = electron_build_dir / 'win-unpacked'
  elif is_macos
    electron_build_target = 'build:mac'
    electron_output_dir = electron_build_dir / 'mac'
  else
    electron_build_target = 'build:linux'
    electron_output_dir = electron_build_dir / 'linux-unpacked'
  endif

  # ============================================================
  # Build Targets
  # ============================================================

  # Electron app build
  # Install npm dependencies and build app
  electron_build = custom_target('electron-build',
    output: 'lemonade',
    command: ['sh', '-c', 'cd @0@ && @1@ install && @1@ run @2@ && cp @3@/lemonade @OUTPUT@'.format(electron_src_dir, npm.full_path(), electron_build_target, electron_output_dir)],
    console: true,
  )
endif

# ============================================================
# C++ Component Configuration
# ============================================================

cpp = meson.get_compiler('cpp')
cpp_src_dir = meson.current_source_dir() / 'src' / 'cpp'

# C++ Dependencies - try system first, fallback to subprojects
nlohmann_json_dep = dependency('nlohmann_json',
  version: '>=3.11.3',
  required: false,
  fallback: ['nlohmann_json', 'nlohmann_json_dep']
)

cli11_dep = dependency('CLI11',
  version: '>=2.4.2',
  required: false,
  fallback: ['cli11', 'CLI11_dep']
)

# On Windows, use system curl (installed via Chocolatey) with cmake method
# On other platforms, allow fallback to subproject
if is_windows
  libcurl_dep = dependency('libcurl',
    version: '>=8.5.0',
    method: 'cmake',
    required: true
  )
else
  libcurl_dep = dependency('libcurl',
    version: '>=8.5.0',
    required: false,
    fallback: ['curl', 'libcurl_dep']
  )
endif

zstd_dep = dependency('libzstd',
  version: '>=1.5.6',
  required: false,
  fallback: ['zstd', 'libzstd_dep']
)

httplib_dep = dependency('cpp-httplib',
  version: '>=0.26.0',
  required: false,
  fallback: ['cpp-httplib', 'httplib_dep']
)

# OpenSSL is only needed on Linux (curl uses it there)
# On Windows, curl uses Schannel; on macOS, curl uses SecureTransport
openssl_dep = dependency('openssl', required: is_linux)
threads_dep = dependency('threads', required: true)

cpp_deps = [
  nlohmann_json_dep,
  cli11_dep,
  libcurl_dep,
  zstd_dep,
  httplib_dep,
  openssl_dep,
  threads_dep,
]

# Compile arguments
cpp_compile_args = []
if is_windows
  cpp_compile_args += ['-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX']
endif

# Link arguments
cpp_link_args = []
if is_windows
  cpp_link_args += ['/DYNAMICBASE', '/NXCOMPAT', '/GUARD:CF']
endif

# Generate version header
version_conf = configuration_data()
version_conf.set('PROJECT_VERSION', project_version)
version_conf.set('PROJECT_VERSION_MAJOR', project_version.split('.')[0])
version_conf.set('PROJECT_VERSION_MINOR', project_version.split('.')[1])
version_conf.set('PROJECT_VERSION_PATCH', project_version.split('.')[2])

# Generate version header
version_h = configure_file(
  input: 'src/cpp/include/lemon/version.h.in',
  output: 'version.h',
  configuration: version_conf,
)

# Include directories - add build root for version.h
cpp_inc_dirs = [
  include_directories('src/cpp/include'),
  include_directories('.'),  # For generated headers (includes lemon/version.h)
]

# 3. C++ Components

# lemonade-router sources
router_sources = files(
  'src/cpp/server/main.cpp',
  'src/cpp/server/server.cpp',
  'src/cpp/server/router.cpp',
  'src/cpp/server/cli_parser.cpp',
  'src/cpp/server/model_manager.cpp',
  'src/cpp/server/wrapped_server.cpp',
  'src/cpp/server/streaming_proxy.cpp',
  'src/cpp/server/system_info.cpp',
  'src/cpp/server/recipe_options.cpp',
  'src/cpp/server/utils/http_client.cpp',
  'src/cpp/server/utils/json_utils.cpp',
  'src/cpp/server/utils/process_manager.cpp',
  'src/cpp/server/utils/path_utils.cpp',
  'src/cpp/server/utils/wmi_helper.cpp',
  'src/cpp/server/backends/llamacpp_server.cpp',
  'src/cpp/server/backends/fastflowlm_server.cpp',
  'src/cpp/server/backends/ryzenaiserver.cpp',
  'src/cpp/server/backends/whisper_server.cpp',
  'src/cpp/server/backends/sd_server.cpp',
  'src/cpp/server/backends/backend_utils.cpp',
)

# Windows resource file for router
if is_windows
  windows = import('windows')
  router_sources += windows.compile_resources('src/cpp/server/version.rc')
endif

# Build lemonade-router
lemonade_router = executable('lemonade-router',
  router_sources,
  version_h,  # Make sure version.h is built first
  include_directories: cpp_inc_dirs,
  dependencies: cpp_deps,
  cpp_args: cpp_compile_args,
  link_args: cpp_link_args,
  install: true,
)

# lemonade-server (tray) sources
tray_sources = files(
  'src/cpp/tray/main.cpp',
  'src/cpp/tray/tray_app.cpp',
  'src/cpp/tray/server_manager.cpp',
  'src/cpp/server/cli_parser.cpp',
  'src/cpp/server/recipe_options.cpp',
  'src/cpp/tray/platform/tray_factory.cpp',
)

# Platform-specific tray sources and dependencies
tray_platform_deps = []
if is_windows
  tray_sources += files('src/cpp/tray/platform/windows_tray.cpp')
  tray_platform_deps += [
    cpp.find_library('user32'),
    cpp.find_library('shell32'),
    cpp.find_library('ole32'),
    cpp.find_library('comctl32'),
  ]
  tray_sources += windows.compile_resources('src/cpp/tray/version.rc')
elif is_macos
  tray_sources += files('src/cpp/tray/platform/macos_tray.mm')
  tray_platform_deps += [
    dependency('Cocoa', required: true),
    dependency('Foundation', required: true),
  ]
else
  tray_sources += files('src/cpp/tray/platform/linux_tray.cpp')
endif

# Build lemonade-server (tray)
lemonade_server = executable('lemonade-server',
  tray_sources,
  version_h,
  include_directories: cpp_inc_dirs,
  dependencies: cpp_deps + tray_platform_deps,
  cpp_args: cpp_compile_args + ['-DLEMONADE_TRAY'],
  link_args: cpp_link_args,
  install: true,
)

# Copy resources
install_subdir('src/cpp/resources',
  install_dir: get_option('datadir') / 'lemonade-server',
  strip_directory: false,
)

# ============================================================
# Unified Build Targets
# ============================================================

all_deps = [lemonade_router, lemonade_server]
if include_electron
  all_deps += [electron_build]
endif

# Main build target that builds all components
build_all = custom_target('build-all',
  output: 'build-complete',
  command: ['echo', 'All components built successfully!'],
  depends: all_deps,
  console: true,
  build_by_default: true,
)

# ============================================================
# Testing and Development Targets
# ============================================================

# Electron development server
if include_electron
  electron_dev = run_target('electron-dev',
    command: ['sh', '-c', 'cd @0@ && @1@ install && @1@ run dev'.format(electron_src_dir, npm.full_path())]
  )
endif

# ============================================================
# Installation
# ============================================================

# Install C++ binaries (handled by cpp subproject)

# Python package installed directly via meson (no separate install script needed)

# Install Electron app (copy to system location)
if include_electron and fs.exists(electron_output_dir)
  install_subdir(electron_output_dir,
    install_dir: get_option('datadir') / 'lemonade' / 'electron',
    strip_directory: true
  )
endif

# ============================================================
# Summary
# ============================================================

if include_electron
  summary({
    'Node.js': node_version,
    'npm': npm_version,
    'Build target': electron_build_target,
    'Output directory': electron_output_dir,
  }, section: 'Electron')
endif

summary({
  'nlohmann_json': nlohmann_json_dep.type_name() == 'internal' ? 'subproject' : 'system',
  'CLI11': cli11_dep.type_name() == 'internal' ? 'subproject' : 'system',
  'libcurl': libcurl_dep.type_name() == 'internal' ? 'subproject' : 'system',
  'zstd': zstd_dep.type_name() == 'internal' ? 'subproject' : 'system',
  'cpp-httplib': httplib_dep.type_name() == 'internal' ? 'subproject' : 'system',
}, section: 'C++ Dependencies')
