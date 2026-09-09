# cmake/DetectSystemHttplib.cmake
# Detect system cpp-httplib package and resolve version and TLS capabilities.

macro(detect_system_httplib min_version)
    set(HTTPLIB_FOUND FALSE)
    set(HTTPLIB_VERSION "")
    set(HTTPLIB_INCLUDE_DIRS "")
    set(HTTPLIB_LIBRARIES "")
    set(HTTPLIB_LINK_LIBRARIES "")
    set(HTTPLIB_LIBRARY_DIRS "")
    set(HTTPLIB_CFLAGS_OTHER "")

    set(HTTPLIB_IS_USING_OPENSSL FALSE)
    set(HTTPLIB_IS_USING_MBEDTLS FALSE)
    set(HTTPLIB_IS_USING_WOLFSSL FALSE)
    set(HTTPLIB_IS_COMPILED FALSE)

    # 1. Try PkgConfig first
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_search_module(HTTPLIB_PKG QUIET
            cpp-httplib>=${min_version}
            httplib>=${min_version})
        if(HTTPLIB_PKG_FOUND)
            set(HTTPLIB_FOUND TRUE)
            set(HTTPLIB_VERSION "${HTTPLIB_PKG_VERSION}")
            set(HTTPLIB_INCLUDE_DIRS "${HTTPLIB_PKG_INCLUDE_DIRS}")
            set(HTTPLIB_LIBRARIES "${HTTPLIB_PKG_LIBRARIES}")
            set(HTTPLIB_LINK_LIBRARIES "${HTTPLIB_PKG_LINK_LIBRARIES}")
            set(HTTPLIB_LIBRARY_DIRS "${HTTPLIB_PKG_LIBRARY_DIRS}")
            set(HTTPLIB_CFLAGS_OTHER "${HTTPLIB_PKG_CFLAGS_OTHER}")

            if(HTTPLIB_PKG_CFLAGS_OTHER MATCHES "CPPHTTPLIB_OPENSSL_SUPPORT")
                set(HTTPLIB_IS_USING_OPENSSL TRUE)
            endif()
            if(HTTPLIB_PKG_CFLAGS_OTHER MATCHES "CPPHTTPLIB_MBEDTLS_SUPPORT")
                set(HTTPLIB_IS_USING_MBEDTLS TRUE)
            endif()
            if(HTTPLIB_PKG_CFLAGS_OTHER MATCHES "CPPHTTPLIB_WOLFSSL_SUPPORT")
                set(HTTPLIB_IS_USING_WOLFSSL TRUE)
            endif()
            if(HTTPLIB_PKG_LIBRARIES OR HTTPLIB_PKG_LINK_LIBRARIES)
                set(HTTPLIB_IS_COMPILED TRUE)
            else()
                set(HTTPLIB_IS_COMPILED FALSE)
            endif()
        endif()
    endif()

    # 2. Try CMake Config fallback
    if(NOT HTTPLIB_FOUND)
        # Pre-check package configuration version if possible to avoid target pollution
        set(HTTPLIB_CONFIG_VERSION_OK TRUE)
        find_file(HTTPLIB_CONFIG_VERSION_FILE
            NAMES httplibConfigVersion.cmake httplib-config-version.cmake
            HINTS "${httplib_DIR}" "${httplib_ROOT}" "$ENV{httplib_DIR}" "$ENV{httplib_ROOT}"
            PATH_SUFFIXES
                .
                cmake/httplib
                lib/cmake/httplib
                share/cmake/httplib
                lib/${CMAKE_LIBRARY_ARCHITECTURE}/cmake/httplib
                lib64/cmake/httplib
                httplib
        )
        if(HTTPLIB_CONFIG_VERSION_FILE)
            file(STRINGS "${HTTPLIB_CONFIG_VERSION_FILE}" version_line REGEX "set\\(PACKAGE_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+[^\"]*\"\\)")
            if(version_line)
                string(REGEX REPLACE ".*set\\(PACKAGE_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+[^\"]*)\"\\).*" "\\1" detected_config_version "${version_line}")
                if(detected_config_version AND detected_config_version VERSION_LESS ${min_version})
                    message(STATUS "Pre-check: System httplib config version ${detected_config_version} is less than required ${min_version}")
                    set(HTTPLIB_CONFIG_VERSION_OK FALSE)
                endif()
            endif()
            unset(HTTPLIB_CONFIG_VERSION_FILE CACHE)
        endif()

        if(HTTPLIB_CONFIG_VERSION_OK)
            find_package(httplib CONFIG QUIET)
            if(httplib_FOUND OR HTTPLIB_FOUND)
                set(HTTPLIB_FOUND TRUE)
                if(httplib_VERSION)
                    set(HTTPLIB_VERSION "${httplib_VERSION}")
                elseif(HTTPLIB_VERSION)
                    set(HTTPLIB_VERSION "${HTTPLIB_VERSION}")
                endif()

                if(httplib_INCLUDE_DIRS)
                    set(HTTPLIB_INCLUDE_DIRS "${httplib_INCLUDE_DIRS}")
                elseif(httplib_INCLUDE_DIR)
                    set(HTTPLIB_INCLUDE_DIRS "${httplib_INCLUDE_DIR}")
                endif()

                # If it's a target but we have no include dirs, extract from target
                if(TARGET httplib::httplib AND NOT HTTPLIB_INCLUDE_DIRS)
                    get_target_property(target_includes httplib::httplib INTERFACE_INCLUDE_DIRECTORIES)
                    if(target_includes)
                        foreach(inc IN LISTS target_includes)
                            # Skip generator expressions
                            if(NOT inc MATCHES "\\$<")
                                list(APPEND HTTPLIB_INCLUDE_DIRS "${inc}")
                            endif()
                        endforeach()
                    endif()
                endif()
            endif()
        endif()
    endif()

    # 3. Try header-only path fallback
    if(NOT HTTPLIB_FOUND)
        find_path(HTTPLIB_INCLUDE_DIRS_TEMP "httplib.h")
        if(HTTPLIB_INCLUDE_DIRS_TEMP)
            set(HTTPLIB_FOUND TRUE)
            set(HTTPLIB_INCLUDE_DIRS "${HTTPLIB_INCLUDE_DIRS_TEMP}")
            unset(HTTPLIB_INCLUDE_DIRS_TEMP CACHE)
        endif()
    endif()

    # 4. Extract version from header if not already set
    if(HTTPLIB_FOUND AND NOT HTTPLIB_VERSION)
        if(NOT HTTPLIB_INCLUDE_DIRS)
            find_path(HTTPLIB_INCLUDE_DIRS_TEMP "httplib.h")
            if(HTTPLIB_INCLUDE_DIRS_TEMP)
                set(HTTPLIB_INCLUDE_DIRS "${HTTPLIB_INCLUDE_DIRS_TEMP}")
                unset(HTTPLIB_INCLUDE_DIRS_TEMP CACHE)
            endif()
        endif()
        foreach(dir IN LISTS HTTPLIB_INCLUDE_DIRS)
            if(EXISTS "${dir}/httplib.h")
                file(STRINGS "${dir}/httplib.h" version_line REGEX "^#define CPPHTTPLIB_VERSION ")
                if(version_line)
                    string(REGEX REPLACE "^#define CPPHTTPLIB_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+[^\"]*)\"" "\\1" detected_version "${version_line}")
                    if(detected_version)
                        set(HTTPLIB_VERSION "${detected_version}")
                        break()
                    endif()
                endif()
            endif()
        endforeach()
    endif()

    # 5. Reject if version is below minimum
    if(HTTPLIB_FOUND AND HTTPLIB_VERSION)
        if(HTTPLIB_VERSION VERSION_LESS ${min_version})
            if(TARGET httplib::httplib)
                message(FATAL_ERROR "System httplib target 'httplib::httplib' (version ${HTTPLIB_VERSION}) imported from configuration is below required minimum ${min_version}. Please provide a compatible package version or clear httplib_DIR to use the bundled dependency.")
            endif()
            message(STATUS "System httplib version ${HTTPLIB_VERSION} is less than required ${min_version}")
            set(HTTPLIB_FOUND FALSE)
            set(HTTPLIB_VERSION "")
            set(HTTPLIB_INCLUDE_DIRS "")
            set(HTTPLIB_LIBRARIES "")
            set(HTTPLIB_LINK_LIBRARIES "")
            set(HTTPLIB_LIBRARY_DIRS "")
            set(HTTPLIB_CFLAGS_OTHER "")
            set(HTTPLIB_IS_USING_OPENSSL FALSE)
            set(HTTPLIB_IS_USING_MBEDTLS FALSE)
            set(HTTPLIB_IS_USING_WOLFSSL FALSE)
            set(HTTPLIB_IS_COMPILED FALSE)
        endif()
    endif()
endmacro()
