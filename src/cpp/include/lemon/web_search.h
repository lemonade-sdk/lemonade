#pragma once

// Keyless, bounded search adapter for the built-in MCP tool. This fetches ONLY
// the fixed Bing RSS search endpoint; it never downloads result URLs.
#include <lemon/utils/http_client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <deque>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace lemon::web_search {

using json = nlohmann::json;

inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool ascii_space(unsigned char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f';
}

inline size_t find_ci(const std::string& text, std::string_view needle, size_t from = 0) {
    if (needle.empty()) return from;
    for (size_t pos = from; pos + needle.size() <= text.size(); ++pos) {
        size_t i = 0;
        for (; i < needle.size(); ++i) {
            if (ascii_lower(text[pos + i]) != ascii_lower(needle[i])) break;
        }
        if (i == needle.size()) return pos;
    }
    return std::string::npos;
}

inline std::string encode_query(const std::string& query) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char c : query) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

inline std::string decode_entities(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '&') { result += input[i]; continue; }
        // Search at most 12 bytes ahead: many unterminated '&' must stay O(n).
        size_t end = std::string::npos;
        for (size_t j = i + 1; j < input.size() && j - i <= 12; ++j) {
            if (input[j] == ';') { end = j; break; }
        }
        if (end == std::string::npos) { result += '&'; continue; }
        const std::string entity = input.substr(i + 1, end - i - 1);
        std::string replacement;
        if (entity == "amp") replacement = "&";
        else if (entity == "lt") replacement = "<";
        else if (entity == "gt") replacement = ">";
        else if (entity == "quot") replacement = "\"";
        else if (entity == "apos" || entity == "#39") replacement = "'";
        else if (entity == "nbsp") replacement = " ";
        else if (!entity.empty() && entity[0] == '#') {
            try {
                const bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
                const size_t offset = hex ? 2 : 1;
                size_t consumed = 0;
                const auto number = std::stoul(entity.substr(offset), &consumed, hex ? 16 : 10);
                if (consumed != entity.size() - offset || number == 0 || number > 0x10ffff ||
                    (number >= 0xd800 && number <= 0xdfff)) throw std::invalid_argument("codepoint");
                if (number < 0x80) replacement += static_cast<char>(number);
                else if (number < 0x800) {
                    replacement += static_cast<char>(0xc0 | (number >> 6));
                    replacement += static_cast<char>(0x80 | (number & 0x3f));
                } else if (number < 0x10000) {
                    replacement += static_cast<char>(0xe0 | (number >> 12));
                    replacement += static_cast<char>(0x80 | ((number >> 6) & 0x3f));
                    replacement += static_cast<char>(0x80 | (number & 0x3f));
                } else {
                    replacement += static_cast<char>(0xf0 | (number >> 18));
                    replacement += static_cast<char>(0x80 | ((number >> 12) & 0x3f));
                    replacement += static_cast<char>(0x80 | ((number >> 6) & 0x3f));
                    replacement += static_cast<char>(0x80 | (number & 0x3f));
                }
            } catch (const std::exception&) { /* Preserve unknown/invalid entities. */ }
        }
        if (replacement.empty()) { result += '&'; continue; }
        result += replacement;
        i = end;
    }
    return result;
}

inline std::string xml_field(const std::string& item, const char* name, bool* is_cdata = nullptr) {
    if (is_cdata) *is_cdata = false;
    const std::string open_tag = std::string("<") + name;
    const std::string close_tag = std::string("</") + name;
    size_t pos = 0;
    while ((pos = find_ci(item, open_tag, pos)) != std::string::npos) {
        const size_t after = pos + open_tag.size();
        if (after >= item.size() || (item[after] != '>' && !ascii_space(item[after]))) {
            pos = after;
            continue;
        }
        const size_t start = item.find('>', after);
        if (start == std::string::npos) return {};
        const size_t close = find_ci(item, close_tag, start + 1);
        if (close == std::string::npos) return {};
        const size_t close_end = item.find('>', close + close_tag.size());
        if (close_end == std::string::npos ||
            (close_end != close + close_tag.size() &&
             !ascii_space(static_cast<unsigned char>(item[close + close_tag.size()])))) return {};
        std::string value = item.substr(start + 1, close - start - 1);
        if (value.size() >= 12 && value.compare(0, 9, "<![CDATA[") == 0 &&
            value.compare(value.size() - 3, 3, "]]>") == 0) {
            value = value.substr(9, value.size() - 12);
            if (is_cdata) *is_cdata = true;
        }
        return value;
    }
    return {};
}

// Number of bytes in a complete UTF-8 scalar at pos, or zero for invalid input.
inline size_t utf8_width(const std::string& text, size_t pos) {
    const auto lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80) return 1;
    const size_t width = (lead >= 0xc2 && lead <= 0xdf) ? 2 :
                         (lead >= 0xe0 && lead <= 0xef) ? 3 :
                         (lead >= 0xf0 && lead <= 0xf4) ? 4 : 0;
    if (!width || width > text.size() - pos) return 0;
    const auto second = static_cast<unsigned char>(text[pos + 1]);
    if ((second & 0xc0) != 0x80 ||
        (lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
        (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) return 0;
    for (size_t j = 2; j < width; ++j) {
        if ((static_cast<unsigned char>(text[pos + j]) & 0xc0) != 0x80) return 0;
    }
    return width;
}

inline std::string clean_text(const std::string& value, size_t limit, bool strip_markup = true,
                              bool decode_references = true) {
    // Decode XML references only outside CDATA. Strip markup and script/style
    // blocks before decoding any remaining HTML references in RSS descriptions.
    const std::string text = decode_references ? decode_entities(value) : value;
    std::string out;
    out.reserve(std::min(limit, text.size()));
    bool pending_space = false;
    for (size_t i = 0; i < text.size() && out.size() < limit;) {
        if (strip_markup && text[i] == '<') {
            const size_t tag_end = text.find('>', i + 1);
            if (tag_end == std::string::npos) break;
            size_t tag_start = i + 1;
            while (tag_start < tag_end && ascii_space(text[tag_start])) ++tag_start;
            bool skipped = false;
            for (const char* tag : {"script", "style"}) {
                const std::string opening = tag;
                const size_t tag_len = opening.size();
                if (tag_start + tag_len <= tag_end &&
                    std::equal(opening.begin(), opening.end(), text.begin() + tag_start,
                               [](char a, char b) { return ascii_lower(a) == ascii_lower(b); }) &&
                    (tag_start + tag_len == tag_end ||
                     ascii_space(static_cast<unsigned char>(text[tag_start + tag_len])))) {
                    const std::string closing = std::string("</") + tag;
                    size_t close = find_ci(text, closing, tag_end + 1);
                    while (close != std::string::npos) {
                        const size_t after = close + closing.size();
                        if (after < text.size() && (text[after] == '>' ||
                            ascii_space(static_cast<unsigned char>(text[after])))) break;
                        close = find_ci(text, closing, after);
                    }
                    if (close == std::string::npos) return out;
                    const size_t end = text.find('>', close);
                    if (end == std::string::npos) return out;
                    i = end + 1;
                    skipped = true;
                    break;
                }
            }
            if (!skipped) i = tag_end + 1;
            pending_space = !out.empty();
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (ascii_space(c) || c < 0x20 || c == 0x7f) {
            ++i;
            pending_space = !out.empty();
            continue;
        }
        const size_t width = utf8_width(text, i);
        if (!width) { ++i; continue; }  // Do not propagate invalid UTF-8 into MCP JSON.
        if (out.size() + width + (pending_space ? 1 : 0) > limit) break;
        if (pending_space) out += ' ';
        pending_space = false;
        out.append(text, i, width);
        i += width;
    }
    return out;
}

inline std::string percent_decode(const std::string& text) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = ascii_lower(c);
        return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
    };
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int a = hex(text[i + 1]), b = hex(text[i + 2]);
            if (a >= 0 && b >= 0) { out += static_cast<char>((a << 4) | b); i += 2; continue; }
        }
        out += text[i];
    }
    return out;
}

inline std::string canonical_link(std::string url) {
    auto trim = [](std::string& value) {
        size_t first = 0, last = value.size();
        while (first < last && ascii_space(static_cast<unsigned char>(value[first]))) ++first;
        while (last > first && ascii_space(static_cast<unsigned char>(value[last - 1]))) --last;
        value = value.substr(first, last - first);
    };
    trim(url);
    // Bing News may return a tracking URL with the real HTTPS URL encoded in
    // the `url=` argument. Only unwrap this *known* wrapper; never fetch it.
    constexpr std::string_view secure_prefix = "https://www.bing.com/news/apiclick.aspx?";
    // Some Bing News RSS feeds use an HTTP apiclick wrapper. We do not fetch
    // that wrapper: only extract its `url=` argument and still require HTTPS.
    constexpr std::string_view legacy_prefix = "http://www.bing.com/news/apiclick.aspx?";
    const size_t prefix_size = url.compare(0, secure_prefix.size(), secure_prefix.data(), secure_prefix.size()) == 0
        ? secure_prefix.size()
        : (url.compare(0, legacy_prefix.size(), legacy_prefix.data(), legacy_prefix.size()) == 0
           ? legacy_prefix.size() : 0);
    if (prefix_size) {
        const std::string params = url.substr(prefix_size);
        url.clear();
        size_t begin = 0;
        while (begin < params.size()) {
            const size_t end = params.find('&', begin);
            const size_t length = (end == std::string::npos ? params.size() : end) - begin;
            if (length > 4 && params.compare(begin, 4, "url=") == 0) {
                url = percent_decode(params.substr(begin + 4, length - 4));
                break;
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    trim(url);  // Also trim destinations decoded from a Bing apiclick wrapper.
    // Tracking-only redirects such as /ck/a are not real source URLs.
    if (url.rfind("https://www.bing.com/ck/", 0) == 0) return {};
    if (url.rfind("https://", 0) != 0 || url.size() > 2048 ||
        url.find_first_of(" \t\r\n<>\"'\\") != std::string::npos) return {};
    for (size_t i = 0; i < url.size();) {
        const size_t width = utf8_width(url, i);
        if (!width) return {};  // Invalid source URLs must not break MCP JSON serialization.
        i += width;
    }
    const size_t host_end = url.find_first_of("/?#", 8);
    const std::string host = url.substr(8, host_end == std::string::npos ? host_end : host_end - 8);
    if (host.empty() || host.find('@') != std::string::npos ||
        host[0] == ':' || host.back() == ':') return {};
    return url;
}

// The provider supports web and news RSS under the same fixed HTTPS host.
// Keep this explicit rather than accepting URLs or hostnames from the model.
inline std::string search_url(const std::string& query, const std::string& search_type) {
    const char* path = search_type == "news" ? "news/search" : "search";
    return std::string("https://www.bing.com/") + path + "?format=rss&q=" + encode_query(query);
}

inline json parse_rss(const std::string& xml, size_t max_results) {
    if (find_ci(xml, "<rss") == std::string::npos) {
        throw std::runtime_error("Search provider returned an unexpected response (possibly a block or CAPTCHA).");
    }
    json results = json::array();
    std::set<std::string> seen;
    bool truncated = false;
    size_t cursor = 0;
    while ((cursor = find_ci(xml, "<item", cursor)) != std::string::npos) {
        const size_t after = cursor + 5;
        if (after >= xml.size() || (xml[after] != '>' && !ascii_space(xml[after]))) {
            cursor = after;
            continue;
        }
        const size_t open_end = xml.find('>', after);
        if (open_end == std::string::npos) break;
        const size_t close = find_ci(xml, "</item", open_end + 1);
        if (close == std::string::npos) break;
        const size_t close_end = xml.find('>', close + 6);
        if (close_end == std::string::npos) break;
        cursor = close_end + 1;
        const std::string item = xml.substr(open_end + 1, close - open_end - 1);
        bool title_cdata = false;
        const std::string raw_title = xml_field(item, "title", &title_cdata);
        const std::string title = clean_text(raw_title, 180, false, !title_cdata);
        bool description_cdata = false;
        const std::string raw_description = xml_field(item, "description", &description_cdata);
        const std::string description = description_cdata ? raw_description : decode_entities(raw_description);
        std::string snippet = clean_text(description, 420, true, false);
        // Nested HTML references are decoded only after tags have been removed.
        // Plain CDATA remains literal (including spellings such as &amp;).
        if (description.find('<') != std::string::npos && description.find('>') != std::string::npos) {
            snippet = clean_text(snippet, 420, false);
        }
        bool link_cdata = false;
        const std::string raw_link = xml_field(item, "link", &link_cdata);
        const std::string url = canonical_link(link_cdata ? raw_link : decode_entities(raw_link));
        if (title.empty() || url.empty() || !seen.insert(url).second) continue;
        if (results.size() == max_results) { truncated = true; break; }
        json result = {{"title", title}, {"url", url}, {"snippet", snippet}};
        // RSS dates are provider-supplied strings (usually RFC 822), not proof
        // that the item was published today. Missing dates remain absent.
        bool published_cdata = false;
        std::string published = xml_field(item, "pubDate", &published_cdata);
        if (published.empty()) published = xml_field(item, "News:Date", &published_cdata);
        published = clean_text(published, 80, true, !published_cdata);
        if (!published.empty()) result["published_at"] = published;
        bool source_cdata = false;
        const std::string raw_source = xml_field(item, "News:Source", &source_cdata);
        const std::string source = clean_text(raw_source, 100, true, !source_cdata);
        if (!source.empty()) result["source"] = source;
        results.push_back(std::move(result));
    }
    return {{"results", std::move(results)}, {"truncated", truncated}};
}

inline json search(const json& args) {
    if (!args.contains("query") || !args["query"].is_string()) {
        throw std::invalid_argument("query must be a nonempty string");
    }
    const std::string query = args["query"].get<std::string>();
    if (query.empty() || query.size() > 256 ||
        std::all_of(query.begin(), query.end(), [](unsigned char c) { return c <= 0x20; })) {
        throw std::invalid_argument("query must contain 1-256 UTF-8 bytes, not just whitespace");
    }
    std::string search_type = "web";
    if (args.contains("search_type")) {
        if (!args["search_type"].is_string()) {
            throw std::invalid_argument("search_type must be 'web' or 'news'");
        }
        search_type = args["search_type"].get<std::string>();
        if (search_type != "web" && search_type != "news") {
            throw std::invalid_argument("search_type must be 'web' or 'news'");
        }
    }
    int max_results = 5;
    if (args.contains("max_results") && !args["max_results"].is_null()) {
        const auto& requested = args["max_results"];
        if (!requested.is_number_integer() || requested < 1 || requested > 5) {
            throw std::invalid_argument("max_results must be an integer from 1 to 5");
        }
        max_results = requested.get<int>();
    }

    static std::atomic<int> active{0};
    const int previous = active.fetch_add(1);
    if (previous >= 2) {
        active.fetch_sub(1);
        throw std::runtime_error("Too many concurrent web searches; try again shortly.");
    }
    struct Release { std::atomic<int>& count; ~Release() { count.fetch_sub(1); } } release{active};
    static std::mutex rate_mutex;
    static std::deque<std::chrono::steady_clock::time_point> recent;
    {
        std::lock_guard<std::mutex> lock(rate_mutex);
        const auto now = std::chrono::steady_clock::now();
        while (!recent.empty() && now - recent.front() >= std::chrono::minutes(1)) recent.pop_front();
        if (recent.size() >= 20) throw std::runtime_error("Web search rate limit reached; try again shortly.");
        recent.push_back(now);
    }

    // Fixed host, fixed path, no redirects. No arbitrary target, cookies,
    // authentication headers or browsing of result URLs.
    const std::string url = search_url(query, search_type);
    const auto response = utils::HttpClient::get(
        url, {}, 8, utils::HttpSecurityPolicy::ExternalHttpsOnly, 256 * 1024, {}, false);
    if (response.status_code != 200) {
        throw std::runtime_error("Search provider returned HTTP " + std::to_string(response.status_code));
    }
    json payload = parse_rss(response.body, static_cast<size_t>(max_results));
    payload["query"] = query;
    payload["search_type"] = search_type;
    return payload;
}

}  // namespace lemon::web_search
