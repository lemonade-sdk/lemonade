#pragma once

#include <string>
#include <string_view>

namespace lemon::utils {

std::string audio_mime_for_extension(std::string_view extension);

}
