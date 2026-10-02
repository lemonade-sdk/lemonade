#include <cstdlib>

#if defined(__GLIBC__)
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>

namespace {

bool process_has_exactly_one_thread() noexcept {
  const int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }

  char status[4096];
  const ssize_t length = read(fd, status, sizeof(status));
  close(fd);
  if (length <= 0) {
    return false;
  }

  constexpr char field[] = "Threads:";
  for (ssize_t offset = 0;
       offset + static_cast<ssize_t>(sizeof(field) - 1) < length; ++offset) {
    if (offset != 0 && status[offset - 1] != '\n') {
      continue;
    }

    bool matches = true;
    for (size_t index = 0; index < sizeof(field) - 1; ++index) {
      if (status[offset + static_cast<ssize_t>(index)] != field[index]) {
        matches = false;
        break;
      }
    }
    if (!matches) {
      continue;
    }

    ssize_t value = offset + static_cast<ssize_t>(sizeof(field) - 1);
    while (value < length && (status[value] == ' ' || status[value] == '\t')) {
      ++value;
    }
    return value < length && status[value] == '1' && value + 1 < length &&
           status[value + 1] == '\n';
  }
  return false;
}

} // namespace

extern "C" int mallopt(int param, int value) noexcept {
  if (param == M_MMAP_THRESHOLD) {
    const char *path = std::getenv("LEMONADE_MALLOPT_PROBE_PATH");
    if (path != nullptr) {
      const int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
      if (fd >= 0) {
        constexpr char expected[] = "1048576 threads=1\n";
        constexpr char threaded[] = "1048576 threads!=1\n";
        constexpr char unexpected[] = "unexpected\n";
        if (value == 1024 * 1024) {
          const bool single_threaded = process_has_exactly_one_thread();
          const char *message = single_threaded ? expected : threaded;
          const size_t message_size =
              single_threaded ? sizeof(expected) - 1 : sizeof(threaded) - 1;
          const ssize_t written = write(fd, message, message_size);
          (void)written;
        } else {
          const ssize_t written = write(fd, unexpected, sizeof(unexpected) - 1);
          (void)written;
        }
        close(fd);
      }
    }
  }
  return 1;
}
#endif
