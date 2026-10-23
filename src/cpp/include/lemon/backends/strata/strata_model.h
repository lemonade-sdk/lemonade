#pragma once

#include "lemon/model_manager.h"

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace lemon::backends::strata {

uint64_t model_expert_bytes(const std::vector<std::filesystem::path>& shards);

void prepare_pack(const std::vector<std::filesystem::path>& shards,
                  const std::filesystem::path& destination,
                  DownloadProgressCallback progress = nullptr,
                  bool compat_bf16 = false);
void prepare_vocabulary(const std::filesystem::path& source, const std::filesystem::path& destination);
void finish_preparation(const std::vector<std::filesystem::path>& sources,
                        const std::filesystem::path& destination, const std::string& mode);
std::pair<std::filesystem::path, std::filesystem::path> prepare_metal_native(
    const std::vector<std::filesystem::path>& shards, const std::filesystem::path& destination);
bool pack_present(const std::filesystem::path& directory);

}
