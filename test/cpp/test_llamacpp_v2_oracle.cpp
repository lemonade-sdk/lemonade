#include "llamacpp_v2_oracle.h"

using namespace runtime_fixture;

int main(int argc, char** argv) {
    try {
        const auto value = llama_corpus::sidecar(argc > 2 && std::string(argv[1]) == "--sidecar" ? argv[2] :
            std::string(LEMON_TEST_FIXTURES) + "/backend-v2/llamacpp.deliberate-diffs.json");
        llama_corpus::validate_sidecar(value);
#ifdef LEMON_V2_ORACLE_VIEW
        const std::vector<std::string> selected{LEMON_V2_ORACLE_VIEW};
#else
        const std::vector<std::string> selected(llama_corpus::views.begin(), llama_corpus::views.end());
#endif
        Json evidence = Json::array();
        for (const auto& view : selected) {
            const auto actual = llama_corpus::capture(view);
            evidence.push_back(llama_corpus::compare(view, actual, value));
            if (const auto* out = std::getenv("LEMON_CORPUS_OUT")) std::ofstream(std::filesystem::path(out) / (view + ".json")) << actual.dump(2) << '\n';
            check(true, "FullOracleWithEnumeratedBoundedProbeDelta:" + view);
        }
        if (const auto* out = std::getenv("LEMON_CORPUS_OUT")) std::ofstream(std::filesystem::path(out) / "deliberate-diff-usage.json") << evidence.dump(2) << '\n';
    } catch (const std::exception& error) {
        std::cout << "FAIL " << error.what() << '\n';
        check(false, "FullOracleGate");
    }
    return finish();
}
