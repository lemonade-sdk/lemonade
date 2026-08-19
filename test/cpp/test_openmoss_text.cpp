#include "lemon/backends/openmoss/openmoss_text.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

}  // namespace

int main() {
    using lemon::backends::openmoss::detail::estimate_max_audio_frames;

    std::string cjk;
    for (int i = 0; i < 100; ++i) {
        cjk += "界";
    }

    check("short input keeps the minimum frame budget",
          estimate_max_audio_frames("short input") == 60);
    check("whitespace-free ASCII scales with characters",
          estimate_max_audio_frames(std::string(100, 'a')) == 150);
    check("whitespace-free CJK scales with codepoints",
          estimate_max_audio_frames(cjk) == 150);
    check("frame budget remains capped",
          estimate_max_audio_frames(std::string(2000, 'a')) == 1500);

    return failures == 0 ? 0 : 1;
}
