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
set(CPACK_PACKAGE_FILE_NAME "lemonade-server-${CPACK_PACKAGE_VERSION}.${CPACK_RPM_PACKAGE_ARCHITECTURE}")
set(CPACK_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)

# Provide RPM-native script hooks
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/postinst-rpm")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/prerm-rpm")
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/postrm-rpm")

# Strip tray from server-only RPM (tray source stays in repo, not packaged)
install(CODE "
  if(DEFINED ENV{DESTDIR} AND NOT \"\$ENV{DESTDIR}\" STREQUAL \"\")
    set(_tray_bin \"\$ENV{DESTDIR}/${CMAKE_INSTALL_PREFIX}/bin/lemonade-tray\")
    set(_tray_symlink \"\$ENV{DESTDIR}/usr/bin/lemonade-tray\")
    if(EXISTS \"\${_tray_bin}\")
      message(\"-- Removing lemonade-tray from RPM (server-only package)\")
      file(REMOVE \"\${_tray_bin}\")
    endif()
    if(EXISTS \"\${_tray_symlink}\")
      file(REMOVE \"\${_tray_symlink}\")
    endif()
  endif()
")

# End of RPM config.
