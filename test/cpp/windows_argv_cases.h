#pragma once

struct WindowsArgCase {
    const char* name;
    const char* input;
    const char* escaped;
};

inline const WindowsArgCase windows_arg_cases[] = {
    {"empty", "", R"("")"},
    {"plain", "plain", R"("plain")"},
    {"whitespace", "a b\tc", "\"a b\tc\""},
    {"quote", R"(")", R"("\"")"},
    {"embedded quote", R"(a"b)", R"("a\"b")"},
    {"adjacent quotes", R"(a""b)", R"("a\"\"b")"},
    {"single backslash", R"(\)", R"("\\")"},
    {"backslash run", R"(a\\\b)", R"("a\\\b")"},
    {"trailing backslash", R"(C:\dir\)", R"("C:\dir\\")"},
    {"trailing run", R"(C:\dir\\\)", R"("C:\dir\\\\\\")"},
    {"one backslash before quote", R"(a\"b)", R"("a\\\"b")"},
    {"two backslashes before quote", R"(a\\"b)", R"("a\\\\\"b")"},
    {"three backslashes before quote", R"(a\\\"b)", R"("a\\\\\\\"b")"},
    {"backslash-backslash-quote", R"(\\")", R"("\\\\\"")"},
    {"quote and trailing backslash", R"(a"b\)", R"("a\"b\\")"},
    {"serialized JSON", R"({"batch":4,"path":"C:\\models","text":"hi"})",
     R"("{\"batch\":4,\"path\":\"C:\\models\",\"text\":\"hi\"}")"},
    {"JSON escaped quotes", R"({"text":"say \"hi\""})",
     R"("{\"text\":\"say \\\"hi\\\"\"}")"},
    {"Unicode", u8"\u6a21\u578b/Za\u017c\u00f3\u0142\u0107/\U0001f600",
     u8"\"\u6a21\u578b/Za\u017c\u00f3\u0142\u0107/\U0001f600\""},
    {"Unicode and trailing backslash", u8"C:\\\u6a21\u578b " R"(\)",
     u8"\"C:\\\u6a21\u578b " R"(\\")"},
};
