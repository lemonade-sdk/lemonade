# Find package structure taken from libcurl

include(FindPackageHandleStandardArgs)

find_path(SPDLOG_INCLUDE_DIRS spdlog/spdlog.h)
find_library(JSONCPP_LIBRARY spdlog)

find_package_handle_standard_args(SPDLOG
    FOUND_VAR
    SPDLOG_FOUND
    REQUIRED_VARS
      SPDLOG_LIBRARY
      SPDLOG_INCLUDE_DIRS
    FAIL_MESSAGE
      "Could NOT find spdlog"
)

set(SPDLOG_INCLUDE_DIRS ${SPDLOG_INCLUDE_DIRS})
set(SPDLOG_LIBRARIES ${SPDLOG_LIBRARY})
