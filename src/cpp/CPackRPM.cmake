# Optional CPack RPM configuration for lemonade-server
# Include this file before include(CPack) in CMakeLists.txt

# Only set these when RPM packaging is requested or when on RPM-friendly host.
# We do not force CPACK_GENERATOR here; the caller can run `cpack -G RPM`.
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_URL "https://github.com/lemonade-sdk/lemonade")

# RPM runtime requirements (package names on Fedora/RHEL)
# Adjust for target distro if needed.
set(CPACK_RPM_PACKAGE_REQUIRES "libcurl, openssl, zlib")

# Architecture and file name
set(CPACK_RPM_PACKAGE_ARCHITECTURE "x86_64")
set(CPACK_PACKAGE_FILE_NAME "lemonade-server-minimal-${CPACK_PACKAGE_VERSION}.${CPACK_RPM_PACKAGE_ARCHITECTURE}")

# Provide script hooks (optional)
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/postinst")

# End of RPM config.
