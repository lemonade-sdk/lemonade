#pragma once
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <locale>
#include <random>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "AutoModel/automodel.hpp"

using json = nlohmann::ordered_json;

class PromptCache {
private:
    uint64_t checksum_;
    std::vector<uint64_t> tool_checksums_;

    uint64_t _calculate_message_checksum(const json& messages, size_t end) {
        uint64_t checksum = 0;
        const size_t message_count = std::min(end, messages.size());
        for (size_t i = 0; i < message_count; ++i) {
            const std::string message_string = messages[i].dump();
            checksum = _calculate_checksum(message_string.data(), message_string.size(), checksum);
        }
        return checksum;
    }

    std::vector<uint64_t> _calculate_tool_checksums(const json& tools) {
        std::vector<uint64_t> tool_checksums;
        tool_checksums.reserve(tools.size());
        for (const auto& tool : tools) {
            const std::string tool_string = tool.dump();
            tool_checksums.push_back(_calculate_checksum(tool_string.data(), tool_string.size()));
        }
        return tool_checksums;
    }

    uint64_t _calculate_checksum(const void* p, size_t len, uint64_t sum = 0) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(p);
        uint64_t _sum = sum;

        const uint64_t* p64 = reinterpret_cast<const uint64_t*>(data);
        size_t blocks = len / sizeof(uint64_t);
        for (size_t i = 0; i < blocks; ++i) {
            _sum += p64[i];
        }

        const uint8_t* p8 = data + blocks * sizeof(uint64_t);
        size_t remain = len % sizeof(uint64_t);
        for (size_t i = 0; i < remain; ++i) {
            _sum += p8[i];
        }

        return _sum;
    }

public:
    PromptCache() : checksum_(0), tool_checksums_() {}

    bool can_use_tool_cache(json& tools) {
        std::vector<uint64_t> new_tool_checksums = _calculate_tool_checksums(tools);

        if (tool_checksums_.size() == new_tool_checksums.size()){
            for (size_t i = 0; i < tool_checksums_.size(); ++i) {
                if (tool_checksums_[i] != new_tool_checksums[i]) {
                    tool_checksums_ = std::move(new_tool_checksums);
                    return false;
                }
            }
            return true;
        }
        else {
            tool_checksums_ = std::move(new_tool_checksums);
            return false;
        }
    }

    void reset_tool_checksum() {
        tool_checksums_.clear();
    }

    void update_tool_checksum(json& tools) {
        if (!tools.is_array() || tools.empty()) {
            reset_tool_checksum();
            return;
        }

        tool_checksums_ = _calculate_tool_checksums(tools);
    }


    bool can_use_message_cache(json& messages, chat_template_type_t template_type) {
        (void)template_type;
        if (messages.size() > 2) {
            const uint64_t check_sum_to_compare = _calculate_message_checksum(messages, messages.size() - 2);
            const uint64_t new_checksum = _calculate_message_checksum(messages, messages.size());

            if (checksum_ == check_sum_to_compare) {
                checksum_ = new_checksum;
                return true;
            }
            else {
                checksum_ = new_checksum;
                return false;
            }
        }
        else {
            return false;
        }

    }

    void update_message_checksum(json& messages) {
        checksum_ = _calculate_message_checksum(messages, messages.size());
    }


    bool can_use_cache(json& messages, chat_template_type_t template_type, json& tools) {
        (void)template_type;
        if (messages.size() <= 2) {
            update_message_checksum(messages);
            update_tool_checksum(tools);
            return false;
        }

        const uint64_t check_sum_to_compare = _calculate_message_checksum(messages, messages.size() - 2);
        const uint64_t new_checksum = _calculate_message_checksum(messages, messages.size());
        std::vector<uint64_t> new_tool_checksums = _calculate_tool_checksums(tools);

        const bool can_use_message = checksum_ == check_sum_to_compare;
        const bool can_use_tools = tool_checksums_ == new_tool_checksums;

        checksum_ = new_checksum;
        tool_checksums_ = std::move(new_tool_checksums);

        return can_use_message && can_use_tools;
    }

    /// @brief Reset the checksum to force cache miss
    /// @note This function increments the checksum value by 1 to ensure that
    ///       the next call to can_use_cache will result in a cache miss.
    void reset() {
        checksum_ = checksum_ + 1;
        reset_tool_checksum();
    }
};
