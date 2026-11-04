#pragma once

#include <string>

namespace lemon {
namespace models {

// What stands between bytes on disk and calling them complete: the manifest an interrupted
// multi-file download leaves at its snapshot root, or a .partial beside a single file. A
// .partial elsewhere in a directory checkpoint belongs to no running download and is
// ignored. Empty when nothing is in flight.
//
// Presence asks this and nothing else does. A second reader of the markers is a second
// opinion about whether a download finished, and the two authorities built that way once.
std::string interrupted_download(const std::string& resolved_path);

}  // namespace models
}  // namespace lemon
