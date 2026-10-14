#include "lemon/model_manager.h"
#include "lemon/models/change_notifier.h"
#include "lemon/models/store_snapshot.h"
#include "lemon/routing_policy.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "show_all_models.h"

using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::RoutePolicy;
using lemon::json;
using lemon::models::collect_helper_models;
using lemon::models::ModelChangeNotifier;
using lemon::utils::path_to_utf8;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static ModelInfo model(const std::string& name) {
    ModelInfo info;
    info.model_name = name;
    return info;
}

static ModelInfo policy_model(const std::string& name, const std::vector<std::string>& helpers) {
    ModelInfo info = model(name);
    auto policy = std::make_shared<RoutePolicy>();
    policy->helper_models = helpers;
    info.route_policy = policy;
    return info;
}

static void test_each_signal_has_one_subscriber() {
    ModelChangeNotifier notifier;
    std::vector<std::string> delivered;
    notifier.on_model_updated([&](const std::string& id) { delivered.push_back("first:" + id); });
    notifier.on_model_updated([&](const std::string& id) { delivered.push_back("second:" + id); });

    notifier.model_updated("user.Renewed");

    check("a second registration replaces the subscriber rather than joining it",
          delivered.size() == 1 && delivered[0] == "second:user.Renewed");
}

static void test_a_throwing_subscriber_does_not_reach_the_change_that_reported_it() {
    ModelChangeNotifier notifier;
    notifier.on_models_changed([](uint64_t) { throw std::runtime_error("subscriber failed"); });
    notifier.on_model_updated([](const std::string&) { throw 1; });

    bool survived = true;
    try {
        notifier.models_changed(1);
        notifier.model_updated("user.Any");
    } catch (...) {
        survived = false;
    }
    check("a subscriber failure never aborts the registry mutation", survived);
}

static void test_an_unsubscribed_signal_is_quiet() {
    ModelChangeNotifier notifier;
    notifier.models_changed(7);
    notifier.model_updated("user.Nobody");
    check("firing with no subscriber does nothing", true);
}

static void test_helper_projection() {
    std::map<std::string, ModelInfo> models;
    models["plain"] = model("plain");
    models["kit"] = policy_model("kit", {"helper.A", "helper.B"});
    models["other_kit"] = policy_model("other_kit", {"helper.B"});

    const auto needed = collect_helper_models(models);
    check("every policy's helpers are needed",
          needed.count("helper.A") == 1 && needed.count("helper.B") == 1);
    check("a helper named twice is still needed once", needed.size() == 2);

    models["kit"] = model("kit");
    const auto after = collect_helper_models(models);
    check("dropping the policy drops what only it needed", after.count("helper.A") == 0);
    check("a helper another policy still names stays needed", after.count("helper.B") == 1);
}

static void register_model(ModelManager& manager, const std::string& name) {
    manager.register_user_model(name, json{{"model_name", name},
                                           {"recipe", "llamacpp"},
                                           {"checkpoint", "zzorg/zznotify:main.gguf"}});
}

static void test_notification_carries_the_published_generation(ModelManager& manager) {
    std::vector<uint64_t> delivered;
    manager.on_models_changed([&](uint64_t generation) { delivered.push_back(generation); });

    manager.notify_models_changed();

    check("a change notifies once", delivered.size() == 1);
    check("the notification names the current snapshot",
          !delivered.empty() && delivered.back() == manager.generation());

    const uint64_t first = delivered.empty() ? 0 : delivered.back();
    manager.notify_models_changed();

    check("repeating without a change repeats one generation, so the consumer coalesces",
          delivered.size() == 2 && delivered[1] == first);

    register_model(manager, "user.Notified");
    manager.notify_models_changed();

    check("after a change the generation has moved", delivered.size() == 3 && delivered[2] > first);
    check("and matches the snapshot again", delivered.back() == manager.generation());
}

static void test_only_a_policy_change_fires_the_signal(ModelManager& manager) {
    int fired = 0;
    manager.on_models_changed([&](uint64_t) { ++fired; });

    register_model(manager, "user.Ordinary");

    check("registering a model with no policy notifies nobody", fired == 0);
    check("but the model is there for readers", manager.model_exists("user.Ordinary"));
}

static void test_generation_survives_a_rebuild(ModelManager& manager) {
    const uint64_t before = manager.generation();
    manager.invalidate_models_cache();
    const uint64_t after_invalidate = manager.generation();
    manager.get_supported_models();

    check("invalidating publishes, so a lock-free reader sees it",
          after_invalidate > before);
    check("rebuilding moves the generation on again", manager.generation() > after_invalidate);
}

int main() {
    ShowAllModels show_all_models;
    std::filesystem::path temp = std::filesystem::temp_directory_path();
    temp /= "change_notifier_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(temp);
    lemon::utils::set_cache_dir(path_to_utf8(temp));
    lemon::utils::set_config_dir(path_to_utf8(temp));
    std::filesystem::path models_dir = temp / "hf";
    std::filesystem::create_directories(models_dir);
    lemon::utils::set_models_dir(path_to_utf8(models_dir));

    test_each_signal_has_one_subscriber();
    test_a_throwing_subscriber_does_not_reach_the_change_that_reported_it();
    test_an_unsubscribed_signal_is_quiet();
    test_helper_projection();

    ModelManager manager;
    test_notification_carries_the_published_generation(manager);
    test_only_a_policy_change_fires_the_signal(manager);
    test_generation_survives_a_rebuild(manager);

    std::filesystem::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All change notifier tests passed.\n");
    } else {
        std::printf("%d change notifier test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
