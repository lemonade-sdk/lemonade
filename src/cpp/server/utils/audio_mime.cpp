#include "lemon/utils/audio_mime.h"

namespace lemon::utils {

std::string audio_mime_for_extension(std::string_view extension) {
    if (extension == ".mp3") return "audio/mpeg";
    if (extension == ".m4a") return "audio/mp4";
    if (extension == ".ogg") return "audio/ogg";
    if (extension == ".flac") return "audio/flac";
    if (extension == ".webm") return "audio/webm";
    return "audio/wav";
}

}
