#pragma once

// Compile-time libc detection.
//
// glibc defines __GLIBC__ (exposed via <features.h>); musl defines no libc
// identification macro, so on Linux its absence means musl. A lemond built
// against musl only ever runs on musl, so this compile-time signal is
// sufficient to select musl-specific backend release assets.
#ifdef __linux__
#include <features.h>
#if !defined(__GLIBC__)
#define LEMON_LINUX_MUSL 1
#endif
#endif
