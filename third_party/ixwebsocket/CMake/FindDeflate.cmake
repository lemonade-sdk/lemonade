# Find package structure taken from libcurl

include(FindPackageHandleStandardArgs)

find_path(DEFLATE_INCLUDE_DIRS libdeflate.h)
find_library(DEFLATE_LIBRARY deflate)

find_package_handle_standard_args(Deflate
    FOUND_VAR
      DEFLATE_FOUND
    REQUIRED_VARS
      DEFLATE_LIBRARY
      DEFLATE_INCLUDE_DIRS
    FAIL_MESSAGE
      "Could NOT find deflate"
)

set(DEFLATE_INCLUDE_DIRS ${DEFLATE_INCLUDE_DIRS})
set(DEFLATE_LIBRARIES ${DEFLATE_LIBRARY})
