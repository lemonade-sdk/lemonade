# Optional CPack RPM configuration for lemonade-server
# Include this file before include(CPack) in CMakeLists.txt

# Only set these when RPM packaging is requested or when on RPM-friendly host.
# We do not force CPACK_GENERATOR here; the caller can run `cpack -G RPM`.
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_URL "https://github.com/lemonade-sdk/lemonade")

# RPM runtime requirements. Package names differ between RPM distros, so each
# entry is a rich dependency listing the Fedora/RHEL name first and the
# openSUSE name second (the library packages are named libcurl4, libz1, ...).
# Only the name that exists on the installing distro has to be available.
# libgomp is required at runtime by OpenMP-linked backends (e.g. llama-server).
set(CPACK_RPM_PACKAGE_REQUIRES
    "(libcurl or libcurl4), (openssl or libopenssl3), (zlib or libz1), (libgomp or libgomp1)")
# CPack before CMake 4.1 drops CPACK_RPM_PACKAGE_RECOMMENDS, so the tag goes
# into the spec preamble directly.
set(CPACK_RPM_SPEC_MORE_DEFINE "Recommends: (podman or docker or docker-ce)\nRecommends: crun")

# Architecture and file name
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_RPM_PACKAGE_ARCHITECTURE "aarch64")
else()
    set(CPACK_RPM_PACKAGE_ARCHITECTURE "x86_64")
endif()
set(CPACK_PACKAGE_FILE_NAME "lemonade-server-${CPACK_PACKAGE_VERSION}.${CPACK_RPM_PACKAGE_ARCHITECTURE}")
set(CPACK_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)

# Provide RPM-native script hooks
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/postinst-rpm")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/prerm-rpm")
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/postrm-rpm")
# End of RPM config.
