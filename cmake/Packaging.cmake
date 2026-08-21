# Platform packaging is kept in separate files so ownership can be scoped
# without changing the order in which the original root CMake logic executes.
include(${CMAKE_CURRENT_LIST_DIR}/PackagingMacOS.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/PackagingLinux.cmake)
