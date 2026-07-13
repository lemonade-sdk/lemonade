#include "lemon/auto_opt_manager.h"

#include "lemon/auto_opt_engine.h"
#include "lemon/auto_tune.h"
#include "lemon/model_manager.h"
#include "lemon/recipe_options.h"
#include "lemon/router.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"
#include "lemon/version.h"

#include <lemon/utils/aixlog.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

namespace fs = std::filesystem;

namespace lemon {
namespace autoopt {

namespace {

std::string iso_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

std::string make_run_id() {
    static std::mt19937 rng{std::random_device{}()};
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char stamp[24];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_utc);
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "%04x", (unsigned)(rng() & 0xffff));
    return std::string("ao-") + stamp + "-" + suffix;
}

const std::set<std::string> kRecurrentArchs = {
    "mamba", "mamba2", "rwkv6", "rwkv6qwen2", "rwkv7", "arwkv7",
    "jamba", "falcon-h1", "granitehybrid", "nemotron-h", "lfm2", "plamo2",
};

constexpr size_t kMaxRuns = 20;

}  // namespace

// ── run (de)serialization ──────────────────────────────────────────────

json AutoOptRun::to_summary_json() const {
    json j = {{"id", id},           {"model", model},
              {"status", status},   {"budget", to_string(budget)},
              {"created_at", created_at}};
    if (!finished_at.empty()) j["finished_at"] = finished_at;
    if (!summary.empty()) j["summary"] = summary;
    if (!error.empty()) j["error"] = error;
    j["lemonade_version"] = LEMON_VERSION_STRING;
    if (status == "running" || status == "queued") {
        int completed = 0;
        std::string current;
        for (const auto& s : stages) {
            if (s.status == "completed" || s.status == "skipped") completed++;
            if (s.status == "running") current = s.name;
        }
        j["progress"] = {{"stage", current.empty() ? (stages.empty() ? "" : stages.back().name)
                                                   : current},
                         {"stage_index", completed},
                         {"stage_count", stages.size()},
                         {"detail", progress_detail}};
    }
    return j;
}

json AutoOptRun::to_json() const {
    json j = to_summary_json();
    j["checkpoint"] = checkpoint;
    j["answers"] = answers.to_json();
    j["allow_unload"] = allow_unload;
    json st = json::array();
    for (const auto& s : stages) {
        json sj = {{"name", s.name}, {"status", s.status}, {"duration_ms", s.duration_ms}};
        if (!s.error.empty()) sj["error"] = s.error;
        if (!s.data.empty()) sj["data"] = s.data;
        st.push_back(sj);
    }
    j["stages"] = st;
    json fit = json::array();
    for (const auto& f : fit_measurements) fit.push_back(f.to_json());
    json bench = json::array();
    for (const auto& b : bench_measurements) bench.push_back(b.to_json());
    j["measurements"] = {{"fit", fit}, {"bench", bench}};
    j["result"] = result ? result->to_json() : json(nullptr);
    return j;
}

AutoOptRun AutoOptRun::from_json(const json& j) {
    AutoOptRun run;
    run.id = j.value("id", "");
    run.model = j.value("model", "");
    run.checkpoint = j.value("checkpoint", "");
    run.budget = budget_from_string(j.value("budget", "quick")).value_or(BudgetTier::Quick);
    if (j.contains("answers")) run.answers = WizardAnswers::from_json(j["answers"]);
    run.allow_unload = j.value("allow_unload", false);
    run.status = j.value("status", "failed");
    run.created_at = j.value("created_at", "");
    run.finished_at = j.value("finished_at", "");
    run.summary = j.value("summary", "");
    run.error = j.value("error", "");
    if (j.contains("stages"))
        for (const auto& sj : j["stages"]) {
            StageRecord s;
            s.name = sj.value("name", "");
            s.status = sj.value("status", "pending");
            s.duration_ms = sj.value("duration_ms", 0);
            s.error = sj.value("error", "");
            if (sj.contains("data")) s.data = sj["data"];
            run.stages.push_back(s);
        }
    // Measurements round-trip as raw json only for display; the engine never
    // re-reads persisted runs.
    if (j.contains("result") && j["result"].is_object()) {
        AutoOptResult r;
        const auto& pj = j["result"]["primary"];
        r.primary.label = pj.value("label", "Recommended");
        r.primary.llamacpp_backend = pj.value("llamacpp_backend", "");
        r.primary.ctx_size = pj.value("ctx_size", -1);
        r.primary.mmproj_enabled = pj.value("mmproj_enabled", true);
        r.primary.llamacpp_args = pj.value("llamacpp_args", "");
        if (pj.contains("rationale"))
            for (const auto& s : pj["rationale"]) r.primary.rationale.push_back(s);
        if (pj.contains("expected")) r.primary.expected = pj["expected"];
        if (j["result"].contains("alternatives"))
            for (const auto& aj : j["result"]["alternatives"]) {
                GeneratedPreset a;
                a.label = aj.value("label", "");
                a.tradeoff = aj.value("tradeoff", "");
                a.llamacpp_backend = aj.value("llamacpp_backend", "");
                a.ctx_size = aj.value("ctx_size", -1);
                a.mmproj_enabled = aj.value("mmproj_enabled", true);
                a.llamacpp_args = aj.value("llamacpp_args", "");
                if (aj.contains("rationale"))
                    for (const auto& s : aj["rationale"]) a.rationale.push_back(s);
                if (aj.contains("expected")) a.expected = aj["expected"];
                r.alternatives.push_back(a);
            }
        if (j["result"].contains("sampling_defaults")
            && j["result"]["sampling_defaults"].is_object()) {
            const auto& sd = j["result"]["sampling_defaults"];
            SamplingDefaults s;
            if (sd.contains("temperature")) s.temperature = sd["temperature"].get<double>();
            if (sd.contains("top_p")) s.top_p = sd["top_p"].get<double>();
            if (sd.contains("min_p")) s.min_p = sd["min_p"].get<double>();
            if (sd.contains("top_k")) s.top_k = sd["top_k"].get<int>();
            s.source = sd.value("source", "");
            r.sampling_defaults = s;
        }
        run.result = r;
    }
    return run;
}

// ── manager ────────────────────────────────────────────────────────────

AutoOptManager::AutoOptManager(Router* router, ModelManager* model_manager,
                               std::string cache_dir,
                               std::unique_ptr<MeasurementProvider> provider)
    : router_(router), model_manager_(model_manager),
      storage_path_((fs::path(cache_dir) / "autoopt_runs.json").string()),
      provider_(provider ? std::move(provider)
                         : std::make_unique<RealMeasurementProvider>()) {
    load_from_disk();
}

AutoOptManager::~AutoOptManager() {
    cancel_requested_.store(true);
    if (worker_.joinable()) worker_.join();
}

void AutoOptManager::load_from_disk() {
    std::ifstream in(lemon::utils::path_from_utf8(storage_path_));
    if (!in) return;
    try {
        json doc = json::parse(in);
        for (const auto& rj : doc.value("runs", json::array())) {
            AutoOptRun run = AutoOptRun::from_json(rj);
            if (run.id.empty()) continue;
            if (run.status == "running" || run.status == "queued") {
                run.status = "failed";
                run.error = "server restarted while the run was active";
                run.finished_at = iso_now();
            }
            order_.push_back(run.id);
            runs_.emplace(run.id, std::move(run));
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "AutoOpt") << "Could not load " << storage_path_ << ": " << e.what()
                                << std::endl;
    }
}

void AutoOptManager::persist_locked() {
    json runs = json::array();
    for (const auto& id : order_) {
        auto it = runs_.find(id);
        if (it != runs_.end()) runs.push_back(it->second.to_json());
    }
    json doc = {{"version", 1}, {"runs", runs}};
    const std::string tmp = storage_path_ + ".tmp";
    {
        std::ofstream out(lemon::utils::path_from_utf8(tmp), std::ios::trunc);
        out << doc.dump(2);
    }
    std::error_code ec;
    fs::rename(lemon::utils::path_from_utf8(tmp), lemon::utils::path_from_utf8(storage_path_),
               ec);
    if (ec)
        LOG(WARNING, "AutoOpt") << "Could not persist runs: " << ec.message() << std::endl;
}

std::string AutoOptManager::start(const std::string& model, BudgetTier budget,
                                  WizardAnswers answers, bool allow_unload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_run_id_.empty()) {
        throw AutoOptError(409, {{"error", "an AutoOpt run is already active"},
                                 {"active_run", active_run_id_}});
    }
    if (budget != BudgetTier::Quick && router_->is_model_loaded() && !allow_unload) {
        throw AutoOptError(409, {{"error", "models are loaded; benchmark budgets need "
                                           "allow_unload consent"},
                                 {"loaded_models", router_->get_all_loaded_models()}});
    }

    ModelInfo info = model_manager_->get_model_info(model);
    if (info.recipe != "llamacpp") {
        throw AutoOptError(400, {{"error", "AutoOpt currently supports llamacpp models only"},
                                 {"recipe", info.recipe}});
    }
    if (info.resolved_path().empty()) {
        throw AutoOptError(400, {{"error", "model is not downloaded"}});
    }

    if (worker_.joinable()) worker_.join();

    AutoOptRun run;
    run.id = make_run_id();
    run.model = model;
    run.checkpoint = info.checkpoint();
    run.budget = budget;
    run.answers = std::move(answers);
    run.allow_unload = allow_unload;
    run.created_at = iso_now();
    for (const char* name : {"snapshot", "model_facts", "hf_metadata", "fit_probes",
                             "bench_matrix", "load_validation", "synthesize"}) {
        StageRecord s;
        s.name = name;
        run.stages.push_back(s);
    }

    const std::string id = run.id;
    order_.insert(order_.begin(), id);
    while (order_.size() > kMaxRuns) {
        runs_.erase(order_.back());
        order_.pop_back();
    }
    runs_.emplace(id, std::move(run));
    active_run_id_ = id;
    cancel_requested_.store(false);
    persist_locked();
    worker_ = std::thread(&AutoOptManager::worker_main, this, id);
    return id;
}

json AutoOptManager::list_runs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    for (const auto& id : order_) {
        auto it = runs_.find(id);
        if (it != runs_.end()) out.push_back(it->second.to_summary_json());
    }
    return out;
}

std::optional<json> AutoOptManager::get_run(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runs_.find(id);
    if (it == runs_.end()) return std::nullopt;
    return it->second.to_json();
}

bool AutoOptManager::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runs_.find(id);
    if (it == runs_.end()) return false;
    if (id == active_run_id_) cancel_requested_.store(true);
    return true;
}

bool AutoOptManager::delete_run(const std::string& id, bool& active) {
    std::lock_guard<std::mutex> lock(mutex_);
    active = (id == active_run_id_);
    if (active) return false;
    auto it = runs_.find(id);
    if (it == runs_.end()) return false;
    runs_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    persist_locked();
    return true;
}

json AutoOptManager::apply(const std::string& id, size_t preset_index) {
    GeneratedPreset preset;
    std::string model;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runs_.find(id);
        if (it == runs_.end()) throw AutoOptError(404, {{"error", "unknown run"}});
        if (!it->second.result) throw AutoOptError(409, {{"error", "run has no result"}});
        const auto& result = *it->second.result;
        if (preset_index == 0) preset = result.primary;
        else if (preset_index - 1 < result.alternatives.size())
            preset = result.alternatives[preset_index - 1];
        else throw AutoOptError(400, {{"error", "preset_index out of range"}});
        model = it->second.model;
    }

    ModelInfo info = model_manager_->get_model_info(model);
    json opts = {{"ctx_size", preset.ctx_size},
                 {"llamacpp_args", preset.llamacpp_args},
                 {"mmproj_enabled", preset.mmproj_enabled}};
    if (!preset.llamacpp_backend.empty()) opts["llamacpp_backend"] = preset.llamacpp_backend;
    info.recipe_options = RecipeOptions(info.recipe, opts);
    model_manager_->save_model_options(info);
    return opts;
}

StageRecord& AutoOptManager::stage_locked(AutoOptRun& run, const std::string& name) {
    for (auto& s : run.stages)
        if (s.name == name) return s;
    run.stages.push_back(StageRecord{name});
    return run.stages.back();
}

void AutoOptManager::finish_stage(const std::string& run_id, const std::string& name,
                                  const std::string& status, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runs_.find(run_id);
    if (it == runs_.end()) return;
    StageRecord& s = stage_locked(it->second, name);
    s.status = status;
    s.error = error;
    persist_locked();
}

void AutoOptManager::set_progress(const std::string& run_id, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runs_.find(run_id);
    if (it != runs_.end()) it->second.progress_detail = detail;
}

void AutoOptManager::worker_main(std::string run_id) {
    using clock = std::chrono::steady_clock;

    WizardAnswers answers;
    BudgetTier budget = BudgetTier::Quick;
    std::string model;
    bool allow_unload = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runs_.find(run_id);
        if (it == runs_.end()) return;
        it->second.status = "running";
        answers = it->second.answers;
        budget = it->second.budget;
        model = it->second.model;
        allow_unload = it->second.allow_unload;
        persist_locked();
    }

    auto run_stage = [&](const std::string& name, auto&& body) -> bool {
        if (cancel_requested_.load()) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = runs_.find(run_id);
            if (it == runs_.end()) return false;
            stage_locked(it->second, name).status = "running";
            persist_locked();
        }
        const auto t0 = clock::now();
        std::string error;
        bool ok = true;
        try {
            body();
        } catch (const std::exception& e) {
            ok = false;
            error = e.what();
        }
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = runs_.find(run_id);
            if (it == runs_.end()) return false;
            StageRecord& s = stage_locked(it->second, name);
            s.duration_ms = ms;
            s.status = cancel_requested_.load() ? "failed" : (ok ? "completed" : "failed");
            s.error = error;
            persist_locked();
        }
        return ok && !cancel_requested_.load();
    };

    auto skip_stage = [&](const std::string& name) {
        finish_stage(run_id, name, "skipped");
    };

    auto fail_run = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runs_.find(run_id);
        if (it == runs_.end()) return;
        it->second.status = cancel_requested_.load() ? "cancelled" : "failed";
        it->second.error = cancel_requested_.load() ? "cancelled by user" : msg;
        it->second.finished_at = iso_now();
        active_run_id_.clear();
        persist_locked();
    };

    HardwareSnapshot hw;
    ModelFacts facts;
    std::vector<FitEstimate> fits;
    std::vector<BenchPoint> bench;
    std::optional<SamplingDefaults> sampling;

    // ── snapshot ───────────────────────────────────────────────────────
    if (!run_stage("snapshot", [&] {
            const json si = SystemInfoCache::get_system_info_with_cache();
            hw.os = si.value("OS Version", "");
            if (si.contains("Physical Memory") && si["Physical Memory"].is_string()) {
                try {
                    hw.host_ram_gb = std::stod(si["Physical Memory"].get<std::string>());
                } catch (const std::exception&) {}
            }
            const json devices = si.value("devices", json::object());
            if (devices.contains("amd_gpu"))
                for (const auto& g : devices["amd_gpu"]) {
                    if (!g.value("available", false)) continue;
                    hw.gpus.push_back({"amd", g.value("name", ""), g.value("family", ""),
                                       g.value("vram_gb", 0.0)});
                }
            if (devices.contains("nvidia_gpu"))
                for (const auto& g : devices["nvidia_gpu"]) {
                    if (!g.value("available", false)) continue;
                    hw.gpus.push_back({"nvidia", g.value("name", ""), g.value("family", ""),
                                       g.value("vram_gb", 0.0)});
                }
            hw.has_igpu = SystemInfo::get_has_igpu();
            hw.ram_is_vram = hw.has_igpu && hw.gpus.size() == 1;
            hw.gpu_available_gb = lemon::get_available_memory_gb(DEVICE_GPU);
            hw.host_ram_available_gb = lemon::get_available_memory_gb(DEVICE_CPU);
            const json recipes = si.value("recipes", json::object());
            if (recipes.contains("llamacpp") && recipes["llamacpp"].contains("backends"))
                for (const auto& [name, state] : recipes["llamacpp"]["backends"].items()) {
                    const std::string st = state.value("state", "");
                    if (st == "installed" || st == "update_available")
                        hw.installed_backends.push_back(name);
                }
        })) {
        fail_run("could not inspect system hardware");
        return;
    }

    // ── model_facts ────────────────────────────────────────────────────
    if (!run_stage("model_facts", [&] {
            ModelInfo info = model_manager_->get_model_info(model);
            facts.gguf_path = info.resolved_path();
            facts.mmproj_path = info.resolved_path("mmproj");
            facts.architecture = info.gguf.architecture;
            facts.block_count = info.gguf.block_count;
            facts.expert_count = info.gguf.expert_count;
            facts.full_attention_interval = info.gguf.full_attention_interval;
            facts.swa_layer_count = info.gguf.swa_layer_count;
            facts.n_ctx_train = info.gguf.context_length;
            facts.base_model_repo = info.gguf.base_model_repo;
            facts.kv_bytes_per_token = compute_weighted_kv_cache_bytes_per_token(info.gguf);
            facts.is_moe = info.gguf.expert_count > 1;
            facts.is_hybrid_or_recurrent = info.gguf.full_attention_interval > 0
                || kRecurrentArchs.count(info.gguf.architecture) > 0;
            facts.has_mtp = std::find(info.labels.begin(), info.labels.end(), "mtp")
                            != info.labels.end();
            facts.has_vision = std::find(info.labels.begin(), info.labels.end(), "vision")
                               != info.labels.end();
            std::error_code ec;
            const auto sz =
                fs::file_size(lemon::utils::path_from_utf8(facts.gguf_path), ec);
            if (!ec) facts.file_size_gb = (double)sz / (1024.0 * 1024.0 * 1024.0);
            if (facts.gguf_path.empty()) throw std::runtime_error("model has no local GGUF");
        })) {
        fail_run("could not read model metadata");
        return;
    }

    // ── hf_metadata (soft-fail) ────────────────────────────────────────
    std::string checkpoint;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runs_.find(run_id);
        if (it != runs_.end()) checkpoint = it->second.checkpoint;
    }
    if (answers.allow_network) {
        run_stage("hf_metadata", [&] {
            const std::string base = resolve_base_model_repo(facts.base_model_repo, checkpoint);
            if (base.empty()) throw std::runtime_error("base model not resolvable");
            auto gc = provider_->fetch_generation_config(base);
            if (!gc) throw std::runtime_error("no generation_config.json on " + base);
            SamplingDefaults sd;
            if (gc->contains("temperature")) sd.temperature = (*gc)["temperature"].get<double>();
            if (gc->contains("top_p")) sd.top_p = (*gc)["top_p"].get<double>();
            if (gc->contains("min_p")) sd.min_p = (*gc)["min_p"].get<double>();
            if (gc->contains("top_k")) sd.top_k = (*gc)["top_k"].get<int>();
            sd.source = "hf:" + base + "/generation_config.json";
            sampling = sd;
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = runs_.find(run_id);
            if (it != runs_.end())
                stage_locked(it->second, "hf_metadata").data = {{"base_model", base},
                                                                {"generation_config_found", true}};
        });
    } else {
        skip_stage("hf_metadata");
    }
    if (cancel_requested_.load()) {
        fail_run("");
        return;
    }

    // For standard/thorough tiers benchmarks own the GPU: unload everything
    // (consent enforced at start()) and never reload — apply/load does that.
    const bool with_bench = budget != BudgetTier::Quick;
    if (with_bench && allow_unload && router_->is_model_loaded()) {
        router_->unload_model("");
    }

    // ── fit_probes ─────────────────────────────────────────────────────
    std::vector<std::string> candidates;
    for (const auto& b : hw.installed_backends) {
        if (b == "cpu" || b == "system" || b == "metal") continue;
        if (!answers.backends_to_consider.empty()
            && std::find(answers.backends_to_consider.begin(),
                         answers.backends_to_consider.end(), b)
                   == answers.backends_to_consider.end())
            continue;
        candidates.push_back(b);
    }
    if (candidates.empty() && !hw.installed_backends.empty())
        candidates.push_back(hw.installed_backends.front());

    if (!run_stage("fit_probes", [&] {
            for (const auto& b : candidates) {
                set_progress(run_id, "llama-fit-params on " + b);
                FitEstimate f = provider_->fit_params(b, facts.gguf_path, {}, 1024,
                                                      cancel_requested_);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = runs_.find(run_id);
                    if (it != runs_.end()) it->second.fit_measurements.push_back(f);
                }
                fits.push_back(f);
                if (facts.has_vision && answers.use_vision && !*answers.use_vision) {
                    FitEstimate fnm = provider_->fit_params(b, facts.gguf_path, {"--no-mmproj"},
                                                            1024, cancel_requested_);
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = runs_.find(run_id);
                    if (it != runs_.end()) it->second.fit_measurements.push_back(fnm);
                    fits.push_back(fnm);
                }
                if (cancel_requested_.load()) return;
            }
            bool any_ok = false;
            for (const auto& f : fits) any_ok |= f.ok;
            if (!any_ok) throw std::runtime_error("all fit probes failed");
        })) {
        // Fit failures degrade to heuristics rather than failing the run —
        // unless we were cancelled.
        if (cancel_requested_.load()) {
            fail_run("");
            return;
        }
    }

    // ── bench_matrix ───────────────────────────────────────────────────
    if (with_bench) {
        run_stage("bench_matrix", [&] {
            const FitEstimate* primary_fit = nullptr;
            for (const auto& f : fits)
                if (f.ok && f.extra_args.empty()) {
                    primary_fit = &f;
                    break;
                }
            json depths = json::array({0});
            if (primary_fit && primary_fit->fitted_ctx >= 32768
                && budget == BudgetTier::Thorough)
                depths.push_back(30000);
            else if (primary_fit && primary_fit->fitted_ctx >= 8192
                     && budget == BudgetTier::Thorough)
                depths.push_back((int)(0.8 * primary_fit->fitted_ctx));

            // Backend duel
            if (candidates.size() > 1) {
                for (const auto& b : candidates) {
                    set_progress(run_id, "llama-bench duel on " + b);
                    auto pts = provider_->llama_bench(b, facts.gguf_path, {{"d", depths}},
                                                      cancel_requested_,
                                                      [&](const std::string& d) {
                                                          set_progress(run_id, d);
                                                      });
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = runs_.find(run_id);
                    for (const auto& p : pts) {
                        if (it != runs_.end()) it->second.bench_measurements.push_back(p);
                        bench.push_back(p);
                    }
                    if (cancel_requested_.load()) return;
                }
            } else if (!candidates.empty()) {
                set_progress(run_id, "llama-bench baseline on " + candidates.front());
                auto pts = provider_->llama_bench(candidates.front(), facts.gguf_path,
                                                  {{"d", depths}}, cancel_requested_,
                                                  [&](const std::string& d) {
                                                      set_progress(run_id, d);
                                                  });
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = runs_.find(run_id);
                for (const auto& p : pts) {
                    if (it != runs_.end()) it->second.bench_measurements.push_back(p);
                    bench.push_back(p);
                }
            }
            if (cancel_requested_.load()) return;

            // Batch ladder and MTP sweep run on the provisional duel winner
            std::string bb = candidates.empty() ? "vulkan" : candidates.front();
            if (candidates.size() > 1) {
                const std::string winner = engine_detail::pick_backend(bench, candidates);
                if (!winner.empty()) bb = winner;
            }
            if (hw.ram_is_vram && hw.host_ram_gb >= 32) {
                std::vector<int> rungs = budget == BudgetTier::Thorough
                                             ? std::vector<int>{512, 1024, 2048, 4096, 8192}
                                             : std::vector<int>{512, 2048, 8192};
                for (int r : rungs) {
                    set_progress(run_id, "batch ladder -b " + std::to_string(r));
                    auto pts = provider_->llama_bench(
                        bb, facts.gguf_path, {{"d", json::array({0})}, {"b", r}, {"ub", r}},
                        cancel_requested_, nullptr);
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = runs_.find(run_id);
                    for (const auto& p : pts) {
                        if (it != runs_.end()) it->second.bench_measurements.push_back(p);
                        bench.push_back(p);
                    }
                    if (cancel_requested_.load()) return;
                }
            }

            // MTP draft-length sweep (llama-bench has no spec flags)
            if (facts.has_mtp) {
                std::vector<int> ns = budget == BudgetTier::Thorough
                                          ? std::vector<int>{1, 2, 3, 4, 5, 6}
                                          : std::vector<int>{2, 3, 4};
                for (int n : ns) {
                    set_progress(run_id, "MTP draft sweep n=" + std::to_string(n));
                    auto ts = provider_->server_decode_probe(
                        bb, facts.gguf_path,
                        {"--spec-type", "draft-mtp", "--spec-draft-n-max", std::to_string(n),
                         "--spec-draft-p-min", "0.75"},
                        cancel_requested_);
                    if (ts) {
                        BenchPoint p;
                        p.backend = bb;
                        p.params = {{"spec_n", n}};
                        p.tg_avg_ts = *ts;
                        p.ok = true;
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto it = runs_.find(run_id);
                        if (it != runs_.end()) it->second.bench_measurements.push_back(p);
                        bench.push_back(p);
                    }
                    if (cancel_requested_.load()) return;
                }
            }
        });
    } else {
        skip_stage("bench_matrix");
    }
    if (cancel_requested_.load()) {
        fail_run("");
        return;
    }

    // ── synthesize (+ load_validation retry loop, lever 12) ────────────
    AutoOptResult result;
    if (!run_stage("synthesize", [&] {
            result = synthesize(hw, facts, answers, fits, bench, sampling);
        })) {
        fail_run("preset synthesis failed");
        return;
    }

    if (with_bench) {
        run_stage("load_validation", [&] {
            std::vector<std::string> tokens = {"-c", std::to_string(result.primary.ctx_size)};
            std::istringstream args(result.primary.llamacpp_args);
            std::string tok;
            while (args >> tok) tokens.push_back(tok);
            set_progress(run_id, "validating recommended flags with llama-fit-params");
            FitEstimate v = provider_->fit_params(result.primary.llamacpp_backend,
                                                  facts.gguf_path, tokens, 1024,
                                                  cancel_requested_);
            if (v.ok && v.fitted_ctx > 0 && v.fitted_ctx < result.primary.ctx_size) {
                result.primary.ctx_size = engine_detail::round_down_ctx(v.fitted_ctx);
                result.primary.rationale.push_back(
                    "Context reduced to " + std::to_string(result.primary.ctx_size)
                    + " after full-flag validation.");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = runs_.find(run_id);
            if (it != runs_.end()) it->second.fit_measurements.push_back(v);
        });
    } else {
        skip_stage("load_validation");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runs_.find(run_id);
        if (it == runs_.end()) return;
        AutoOptRun& run = it->second;
        run.result = result;
        run.status = cancel_requested_.load() ? "cancelled" : "completed";
        run.finished_at = iso_now();
        run.summary = result.primary.llamacpp_backend + " · ctx "
                      + std::to_string(result.primary.ctx_size)
                      + (result.primary.llamacpp_args.empty()
                             ? ""
                             : " · " + result.primary.llamacpp_args);
        active_run_id_.clear();
        persist_locked();
    }
}

}  // namespace autoopt
}  // namespace lemon
