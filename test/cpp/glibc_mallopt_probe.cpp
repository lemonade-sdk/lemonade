#include <cstdlib>

#if defined(__GLIBC__)
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>

extern "C" int mallopt(int param, int value) noexcept {
    if (param == M_MMAP_THRESHOLD) {
        const char* path = std::getenv("LEMONADE_MALLOPT_PROBE_PATH");
        if (path != nullptr) {
            const int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
            if (fd >= 0) {
                constexpr char expected[] = "1048576\n";
                constexpr char unexpected[] = "unexpected\n";
                if (value == 1024 * 1024) {
                    const ssize_t written =
                        write(fd, expected, sizeof(expected) - 1);
                    (void)written;
                } else {
                    const ssize_t written =
                        write(fd, unexpected, sizeof(unexpected) - 1);
                    (void)written;
                }
                close(fd);
            }
        }
    }
    return 1;
}
#endif
