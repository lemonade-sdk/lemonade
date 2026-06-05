/// \file language_table.hpp
/// \brief language_table class
/// \author FastFlowLM Team
/// \date 2025-10-17
/// \version 0.9.24
/// \note This is a header file for the language_table class
#pragma once
#include <string>
#include <unordered_map>

namespace langmap {

static const std::unordered_map<std::string, std::string> TOKEN_TO_LANGUAGE = {
    {"<|en|>", "English"},
    {"<|zh|>", "Chinese"},
    {"<|de|>", "German"},
    {"<|es|>", "Spanish"},
    {"<|ru|>", "Russian"},
    {"<|ko|>", "Korean"},
    {"<|fr|>", "French"},
    {"<|ja|>", "Japanese"},
    {"<|pt|>", "Portuguese"},
    {"<|tr|>", "Turkish"},
    {"<|pl|>", "Polish"},
    {"<|ca|>", "Catalan"},
    {"<|nl|>", "Dutch"},
    {"<|ar|>", "Arabic"},
    {"<|sv|>", "Swedish"},
    {"<|it|>", "Italian"},
    {"<|id|>", "Indonesian"},
    {"<|hi|>", "Hindi"},
    {"<|fi|>", "Finnish"},
    {"<|vi|>", "Vietnamese"},
    {"<|he|>", "Hebrew"},
    {"<|uk|>", "Ukrainian"},
    {"<|el|>", "Greek"},
    {"<|ms|>", "Malay"},
    {"<|cs|>", "Czech"},
    {"<|ro|>", "Romanian"},
    {"<|da|>", "Danish"},
    {"<|hu|>", "Hungarian"},
    {"<|ta|>", "Tamil"},
    {"<|no|>", "Norwegian"},
    {"<|th|>", "Thai"},
    {"<|ur|>", "Urdu"},
    {"<|hr|>", "Croatian"},
    {"<|bg|>", "Bulgarian"},
    {"<|lt|>", "Lithuanian"},
    {"<|la|>", "Latin"},
    {"<|mi|>", "Maori"},
    {"<|ml|>", "Malayalam"},
    {"<|cy|>", "Welsh"},
    {"<|sk|>", "Slovak"},
    {"<|te|>", "Telugu"},
    {"<|fa|>", "Persian"},
    {"<|lv|>", "Latvian"},
    {"<|bn|>", "Bengali"},
    {"<|sr|>", "Serbian"},
    {"<|az|>", "Azerbaijani"},
    {"<|sl|>", "Slovenian"},
    {"<|kn|>", "Kannada"},
    {"<|et|>", "Estonian"},
    {"<|mk|>", "Macedonian"},
    {"<|br|>", "Breton"},
    {"<|eu|>", "Basque"},
    {"<|is|>", "Icelandic"},
    {"<|hy|>", "Armenian"},
    {"<|ne|>", "Nepali"},
    {"<|mn|>", "Mongolian"},
    {"<|bs|>", "Bosnian"},
    {"<|kk|>", "Kazakh"},
    {"<|sq|>", "Albanian"},
    {"<|sw|>", "Swahili"},
    {"<|gl|>", "Galician"},
    {"<|mr|>", "Marathi"},
    {"<|pa|>", "Punjabi"},
    {"<|si|>", "Sinhala"},
    {"<|km|>", "Khmer"},
    {"<|sn|>", "Shona"},
    {"<|yo|>", "Yoruba"},
    {"<|so|>", "Somali"},
    {"<|af|>", "Afrikaans"},
    {"<|oc|>", "Occitan"},
    {"<|ka|>", "Georgian"},
    {"<|be|>", "Belarusian"},
    {"<|tg|>", "Tajik"},
    {"<|sd|>", "Sindhi"},
    {"<|gu|>", "Gujarati"},
    {"<|am|>", "Amharic"},
    {"<|yi|>", "Yiddish"},
    {"<|lo|>", "Lao"},
    {"<|uz|>", "Uzbek"},
    {"<|fo|>", "Faroese"},
    {"<|ht|>", "Haitian Creole"},
    {"<|ps|>", "Pashto"},
    {"<|tk|>", "Turkmen"},
    {"<|nn|>", "Nynorsk"},
    {"<|mt|>", "Maltese"},
    {"<|sa|>", "Sanskrit"},
    {"<|lb|>", "Luxembourgish"},
    {"<|my|>", "Burmese"},
    {"<|bo|>", "Tibetan"},
    {"<|tl|>", "Tagalog"},
    {"<|mg|>", "Malagasy"},
    {"<|as|>", "Assamese"},
    {"<|tt|>", "Tatar"},
    {"<|haw|>", "Hawaiian"},
    {"<|ln|>", "Lingala"},
    {"<|ha|>", "Hausa"},
    {"<|ba|>", "Bashkir"},
    {"<|jw|>", "Javanese"},
    {"<|su|>", "Sundanese"},
    {"<|yue|>", "Cantonese"}
};

inline std::string to_language_name(const std::string& token) {
    auto it = TOKEN_TO_LANGUAGE.find(token);
    if (it != TOKEN_TO_LANGUAGE.end())
        return it->second;
    return "Unknown";
}

}  // namespace langmap
