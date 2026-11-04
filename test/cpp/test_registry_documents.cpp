#include <lemon/models/registry_documents.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using lemon::models::RegistryDocument;
using lemon::models::RegistryDocuments;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

std::filesystem::path test_file(const std::string& tag) {
    static int counter = 0;
    const auto path = std::filesystem::temp_directory_path() /
                      ("lemonade_registry_documents_" + std::to_string(counter++) + "_" + tag +
                       ".json");
    std::filesystem::remove(path);
    return path;
}

void write_file(const std::filesystem::path& path, const json& value) {
    std::ofstream out(path);
    out << value.dump();
}

json read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    return json::parse(in);
}

}  // namespace

static void test_a_key_lives_in_one_document(const std::filesystem::path& user_file) {
    write_file(user_file, json{{"Mine", {{"recipe", "llamacpp"}}}});
    const RegistryDocuments registries(json{{"Qwen", {{"recipe", "llamacpp"}}}},
                                       user_file.string());

    const auto rows = registries.pin();
    check("a built-in key reads from the built-in document",
          rows.contains(RegistryDocument::Builtin, "Qwen"));
    check("and not from the user document", !rows.contains(RegistryDocument::User, "Qwen"));
    check("a registered key reads from the user document",
          rows.contains(RegistryDocument::User, "Mine"));
    check("a row carries its own definition",
          rows.row(RegistryDocument::User, "Mine")->at("recipe") == "llamacpp");
    check("a key neither document has resolves to no row",
          rows.row(RegistryDocument::User, "Qwen") == nullptr &&
              rows.row(RegistryDocument::Builtin, "Nope") == nullptr);
}

static void test_a_first_run_has_an_empty_registry(const std::filesystem::path& user_file) {
    const RegistryDocuments registries(json::object(), user_file.string());
    check("a user file that does not exist yet is an empty registry, not an error",
          !registries.pin().contains(RegistryDocument::User, "Anything"));
}

static void test_a_reader_sees_one_frozen_pair(const std::filesystem::path& user_file) {
    write_file(user_file, json{{"First", {{"recipe", "llamacpp"}}}});
    RegistryDocuments registries(json::object(), user_file.string());

    const auto before = registries.pin();
    check("a pin reads the document as of the pin",
          before.contains(RegistryDocument::User, "First"));

    const bool wrote = registries.update_user([&](json& document) {
        document["Second"] = json{{"recipe", "llamacpp"}};
        return true;
    });
    check("a mutation that changes something reports that it wrote", wrote);

    check("a mutation cannot reach through a reader's pin",
          !before.contains(RegistryDocument::User, "Second"));
    const json* first = before.row(RegistryDocument::User, "First");
    check("and the row it handed out still reads as the document it came from",
          first != nullptr && first->size() == 1);

    const auto after = registries.pin();
    check("a later pin sees what was written", after.contains(RegistryDocument::User, "Second"));
    check("keeping what the pin already had", after.contains(RegistryDocument::User, "First"));
    check("the file holds the same set the pin does", read_file(user_file).size() == 2);
}

static void test_a_writer_starts_from_the_file_not_its_own_document(
    const std::filesystem::path& user_file) {
    write_file(user_file, json::object());
    RegistryDocuments registries(json::object(), user_file.string());

    registries.update_user([&](json& document) {
        document["One"] = json{{"recipe", "llamacpp"}};
        return true;
    });

    // Another writer's work, landed on disk outside this document.
    json on_disk = read_file(user_file);
    on_disk["FromElsewhere"] = json{{"recipe", "llamacpp"}};
    write_file(user_file, on_disk);

    std::vector<std::string> seen;
    registries.update_user([&](json& document) {
        for (const auto& [key, row] : document.items()) {
            seen.push_back(key);
        }
        document["Two"] = json{{"recipe", "llamacpp"}};
        return true;
    });

    check("a mutation is handed the copy on disk, not the one being published",
          std::find(seen.begin(), seen.end(), "FromElsewhere") != seen.end());

    const auto rows = registries.pin();
    check("so a registration that landed elsewhere survives",
          rows.contains(RegistryDocument::User, "FromElsewhere"));
    check("and so does the one before it", rows.contains(RegistryDocument::User, "One"));
    check("alongside the new one", rows.contains(RegistryDocument::User, "Two"));
}

static void test_no_mutation_writes_no_file() {
    const auto user_file = test_file("no-write");
    RegistryDocuments registries(json::object(), user_file.string());

    const bool wrote = registries.update_user([](json&) { return false; });
    check("a mutation that changes nothing says so", !wrote);
    check("and no file appears where the registry was absent",
          !std::filesystem::exists(user_file));

    write_file(user_file, json::array());
    const bool normalized = registries.update_user([&](json& document) {
        document["After"] = json{{"recipe", "llamacpp"}};
        return true;
    });
    check("a document that is not a set of rows becomes one", normalized);
    check("so the new row is there", registries.pin().contains(RegistryDocument::User, "After"));
    std::filesystem::remove(user_file);
}

static void test_a_document_that_is_not_a_set_of_rows_reads_as_empty() {
    const auto user_file = test_file("not-rows");
    write_file(user_file, json::array({1, 2, 3}));
    const RegistryDocuments registries(json::array({4, 5}), user_file.string());

    const auto rows = registries.pin();
    check("a built-in document that is not a set of rows has none",
          !rows.contains(RegistryDocument::Builtin, "0") &&
              rows.row(RegistryDocument::Builtin, "0") == nullptr);
    check("a user document that is not a set of rows has none",
          !rows.contains(RegistryDocument::User, "0") &&
              rows.row(RegistryDocument::User, "0") == nullptr);
    std::filesystem::remove(user_file);
}

static void test_registrations_do_not_overwrite_one_another() {
    const auto user_file = test_file("concurrent");
    write_file(user_file, json::object());
    RegistryDocuments registries(json::object(), user_file.string());

    constexpr int kWriters = 8;
    constexpr int kPerWriter = 4;
    const auto name = [](int writer, int index) {
        return "model-" + std::to_string(writer) + "-" + std::to_string(index);
    };

    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&registries, &name, w] {
            for (int i = 0; i < kPerWriter; ++i) {
                const std::string model = name(w, i);
                registries.update_user([&model](json& user_models) {
                    user_models[model] = json{{"recipe", "llamacpp"}};
                    return true;
                });
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }

    const auto rows = registries.pin();
    int present = 0;
    for (int w = 0; w < kWriters; ++w) {
        for (int i = 0; i < kPerWriter; ++i) {
            if (rows.contains(RegistryDocument::User, name(w, i))) ++present;
        }
    }
    check("no registration is lost to one in flight", present == kWriters * kPerWriter);
    check("and the file agrees with the published document",
          static_cast<int>(read_file(user_file).size()) == kWriters * kPerWriter);
    std::filesystem::remove(user_file);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    const auto user_file = test_file("main");
    test_a_key_lives_in_one_document(user_file);
    test_a_first_run_has_an_empty_registry(test_file("first-run"));
    test_a_reader_sees_one_frozen_pair(user_file);
    test_a_writer_starts_from_the_file_not_its_own_document(user_file);
    test_no_mutation_writes_no_file();
    test_a_document_that_is_not_a_set_of_rows_reads_as_empty();
    test_registrations_do_not_overwrite_one_another();

    std::filesystem::remove(user_file);
    if (g_failures == 0) {
        std::printf("All registry document tests passed.\n");
    } else {
        std::printf("%d registry document test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
