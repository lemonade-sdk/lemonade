# Note

Except *zlib* and *mbedtls* on Windows, all dependencies here are for the ws command line tools, not for the IXWebSockets library which is standalone.

## MbedTLS

A small CMakeLists.txt fix had to be done so that the library can be included with "include_directory" in the top level CMakeLists.txt file. See https://github.com/ARMmbed/mbedtls/issues/2609
