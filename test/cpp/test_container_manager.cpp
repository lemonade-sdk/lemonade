#include "configured_fixture.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/container_process.h"
#include "lemon/utils/container_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace lemon;
using namespace lemon::utils;
using json = nlohmann::json;
int checks = 0;
int failures = 0;

void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok)
        ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
}

template <class F> void rejects(F action, const std::string& label) {
    bool threw = false;
    try {
        action();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, label);
}

bool contains(const std::vector<std::string>& argv, const std::string& value) {
    return std::find(argv.begin(), argv.end(), value) != argv.end();
}

std::vector<std::string> values(const std::vector<std::string>& argv, const std::string& option) {
    std::vector<std::string> result;
    for (size_t i = 0; i + 1 < argv.size(); ++i)
        if (argv[i] == option)
            result.push_back(argv[i + 1]);
    return result;
}

ContainerRunSpec spec() {
    ContainerRunSpec result;
    result.recipe = "fixture";
    result.backend = "rocm";
    result.model = "org/model";
    result.name = ContainerManager::container_name(result.recipe, result.backend, result.model);
    result.network = result.name;
    result.port = 32123;
    result.image = "docker.io/kyuz0/toolbox@sha256:" + std::string(64, 'a');
    return result;
}

struct FakeHost {
    bool podman = true;
    bool docker = true;
    bool root = false;
    bool gpu = true;
    bool subids = true;
    bool cancelled_stream = false;
    int info_status = 0;
    int inspect_status = 0;
    int stream_status = 0;
    int network_status = 0;
    std::string podman_info = "true netavark";
    std::string docker_info = "Linux|[]";
    std::string user = "fixture-user";
    std::string fail_command;
    std::string inspect;
    std::vector<std::vector<std::string>> commands;
    std::vector<int> timeouts;
    std::vector<std::string> groups;
    std::vector<std::string> stream_lines;
    std::map<std::string, std::string> env;
    std::map<std::string, std::string> files;
    std::map<std::string, std::string> links;
    std::map<std::string, ContainerFileKind> kinds;
    std::vector<std::string> inventory;
    int sleeps = 0;

    FakeHost() { inspection(); }
    void inspection() {
        auto s = spec();
        json container;
        container["Name"] = "/" + s.name;
        container["Config"]["Labels"]["ai.lemonade"] = "";
        container["NetworkSettings"]["Networks"][s.network]["IPAddress"] = "10.88.0.4";
        inspect = json::array({container}).dump();
    }
    ContainerHost host() {
        ContainerHost result;
        result.on_path = [this](const auto& name) {
            return (name == "podman" && podman) || (name == "docker" && docker) ||
                   name == "getsubids";
        };
        result.gpu_devices_accessible = [this] { return gpu; };
        result.group_id = [this](const auto& name) {
            groups.push_back(name);
            return name == "render" ? "109" : "44";
        };
        result.account = [this] { return user; };
        result.env = [this](const auto& name) { return env[name]; };
        result.is_root = [this] { return root; };
        result.read_file = [this](const auto& path) { return files[path]; };
        result.run = [this](const auto& argv, int timeout, bool) {
            commands.push_back(argv);
            timeouts.push_back(timeout);
            if (contains(argv, fail_command))
                throw std::runtime_error("injected host failure");
            if (argv.at(1) == "info")
                return CommandResult{info_status,
                                     argv.front() == "podman" ? podman_info : docker_info};
            if (argv.front() == "getsubids")
                return CommandResult{0, subids ? "0: lemonade 100000 65536" : ""};
            if (argv.at(1) == "inspect")
                return CommandResult{inspect_status, inspect};
            if (argv.at(1) == "network" && argv.at(2) == "create")
                return CommandResult{network_status, "fixture failure"};
            return CommandResult{0, ""};
        };
        result.stream = [this](const auto& argv, int timeout, const auto& on_line) {
            commands.push_back(argv);
            timeouts.push_back(timeout);
            for (const auto& line : stream_lines)
                if (!on_line(line)) {
                    cancelled_stream = true;
                    return -1;
                }
            return stream_status;
        };
        result.canonical = [this](const auto& path) {
            if (!links.count(path))
                throw std::runtime_error("Missing fake file");
            return links.at(path);
        };
        result.file_kind = [this](const auto& path) {
            return kinds.count(path) ? kinds.at(path) : ContainerFileKind::Other;
        };
        result.files = [this](const auto&) { return inventory; };
        result.sleep = [this](auto) { ++sleeps; };
        return result;
    }
    bool ran(const std::vector<std::string>& command) const {
        return std::find(commands.begin(), commands.end(), command) != commands.end();
    }
};

void pins_and_commands() {
    for (const auto& repo :
         {"docker.io/kyuz0/toolbox", "ghcr.io/peonist-ai/halogen-flash-server"}) {
        check(ContainerManager::allowed_image(repo),
              "Accepted image namespace " + std::string(repo));
        check(container_image(repo, "latest@sha256:" + std::string(64, 'a')) ==
                  std::string(repo) + "@sha256:" + std::string(64, 'a'),
              "Pin resolves to immutable identity");
    }
    for (const auto& repo :
         {"docker.io/other/toolbox", "docker.io/kyuz0/", "docker.io/kyuz0evil/toolbox",
          "ghcr.io/peonist-ai/../evil", "https://docker.io/kyuz0/toolbox",
          "docker.io/kyuz0/toolbox:latest", "docker.io/kyuz0/toolbox\n"}) {
        check(!ContainerManager::allowed_image(repo),
              "Rejected image repository " + std::string(repo));
    }
    check(parse_container_pin("latest@sha256:" + std::string(64, 'a')).has_value(),
          "Valid lowercase SHA256 pin");
    for (const auto& pin : std::vector<std::string>{
             "latest", "@sha256:" + std::string(64, 'a'), "latest@sha256:" + std::string(64, 'A'),
             "latest@sha256:" + std::string(63, 'a'), "latest@sha512:" + std::string(64, 'a'),
             "a/b@sha256:" + std::string(64, 'a')}) {
        check(!parse_container_pin(pin), "Malformed pin rejected");
    }
    check(short_container_pin("latest@sha256:" + std::string(64, 'a')) == "latest@aaaaaaaaaaaa",
          "Short pin retains channel");
    check(short_container_pin("invalid") == "invalid", "Invalid short pin unchanged");
    FakeHost host;
    ContainerManager manager(host.host());
    auto run = spec();
    run.devices = {"/dev/dri", "/dev/kfd"};
    run.cap_add = {"SYS_PTRACE"};
    run.mounts = {{"/cache/blob", "/mnt/models/main"}};
    run.env = {{"ENGINE", "a b"}};
    run.command = {"engine", "--pull=always", "a b", ";echo ignored"};
    auto argv = manager.run_command(run);
    check(argv.front() == "podman", "Podman preferred over Docker");
    check(contains(argv, "--rm") && contains(argv, "--init"),
          "Attached run retains init and automatic removal");
    check(contains(argv, "--pull=never"), "Run never pulls");
    check(contains(argv, "--cap-drop=all"), "All capabilities dropped");
    check(values(argv, "--cap-add") == run.cap_add, "Only granted capabilities added");
    check(std::find(argv.begin(), argv.end(), "--cap-drop=all") <
              std::find(argv.begin(), argv.end(), "--cap-add"),
          "Drop precedes capability grants");
    check(contains(argv, "--security-opt=no-new-privileges"), "No new privileges");
    check(!contains(argv, "--privileged") && !contains(argv, "--security-opt=seccomp=unconfined"),
          "Default seccomp and no privileged mode");
    check(values(argv, "--device") == run.devices, "Exact granted devices");
    check(values(argv, "--group-add") == std::vector<std::string>{"keep-groups"},
          "Rootless supplementary groups retained");
    check(values(argv, "-p") == std::vector<std::string>{"127.0.0.1:32123:32123"},
          "Rootless loopback publication only");
    check(values(argv, "-v") == std::vector<std::string>{"/cache/blob:/mnt/models/main:ro,z"},
          "Read only SELinux mount");
    check(values(argv, "--env") == std::vector<std::string>{"HOME=/tmp", "ENGINE=a b"},
          "Environment remains separate argv values");
    auto image = std::find(argv.begin(), argv.end(), run.image);
    check(image != argv.end() && std::vector<std::string>(image + 1, argv.end()) == run.command,
          "Engine flags occur after immutable image");
    check(contains(argv, "--network=" + run.network), "Owned network attached");
    for (const auto& bad :
         std::vector<std::string>{"docker.io/kyuz0/toolbox:latest", "docker.io/kyuz0/toolbox",
                                  "docker.io/other/toolbox@sha256:" + std::string(64, 'a'),
                                  "docker.io/kyuz0/toolbox@sha256:" + std::string(63, 'a')}) {
        auto invalid = spec();
        invalid.image = bad;
        rejects([&] { manager.run_command(invalid); },
                "Mutable or untrusted image cannot reach run");
    }
    auto invalid = run;
    invalid.cap_add = {"ALL"};
    rejects([&] { manager.run_command(invalid); }, "All-capability wildcard rejected");
    invalid = run;
    invalid.network = "host";
    rejects([&] { manager.run_command(invalid); }, "Unowned run network rejected");
    for (const auto& mount : std::vector<ContainerMount>{{"/cache/a:rw", "/mnt/models/a"},
                                                         {"/cache/a", "/mnt/models/../a"},
                                                         {"/cache/a", "/mnt/models/a:rw"}}) {
        invalid = run;
        invalid.mounts = {mount};
        rejects([&] { manager.run_command(invalid); }, "Mount syntax escape rejected");
    }
    for (bool docker : {false, true}) {
        FakeHost rootful;
        rootful.podman = !docker;
        rootful.podman_info = "false netavark";
        ContainerManager runtime(rootful.host());
        argv = runtime.run_command(run);
        check(values(argv, "--group-add") == std::vector<std::string>{"109"} &&
                  rootful.groups == std::vector<std::string>{"render"},
              "Render only explicit GPU grant");
        check(values(argv, "-p").empty(), "Rootful runtime never publishes host port");
        check(argv.front() == (docker ? "docker" : "podman"), "Rootful tool selection");
    }
    check(ContainerManager::container_name("r", "b", "A/_ snow") == "lemonade-r-b-A_2f___20snow",
          "Container name byte escaping");
}

void host_checks() {
    for (const auto& info : {"Docker Desktop|[]", "Linux|[\"name=rootless\"]"}) {
        FakeHost host;
        host.podman = false;
        host.docker_info = info;
        ContainerManager manager(host.host());
        check(manager.check_host().size() == 1, "Unsupported Docker host diagnosed");
        rejects([&] { manager.run_command(spec()); }, "Unsupported Docker rejected before run");
    }
    for (const auto& info : {"false cni", "garbage", "false unknown", "true netavark trailing"}) {
        FakeHost host;
        host.podman_info = info;
        ContainerManager manager(host.host());
        check(manager.check_podman().has_value(), "Unsafe or unknown Podman mode diagnosed");
        rejects([&] { manager.run_command(spec()); }, "Unsafe or unknown Podman mode rejected");
    }
    FakeHost host;
    host.podman = false;
    host.info_status = 1;
    ContainerManager docker(host.host());
    check(docker.check_host().front().id == "docker_access", "Docker daemon access checked");
    host.podman = true;
    ContainerManager podman(host.host());
    check(podman.check_podman()->id == "podman_info",
          "Unusable preferred Podman does not fall back");
    host.podman = host.docker = false;
    ContainerManager absent(host.host());
    check(!absent.tool(), "No runtime detected");
    rejects([&] { absent.run_command(spec()); }, "No runtime fails closed");
    host.podman = true;
    host.info_status = 0;
    host.gpu = false;
    host.user = "lemonade";
    ContainerManager inaccessible(host.host());
    auto failures = inaccessible.check_host();
    check(failures.front().steps == std::vector<std::string>{"sudo usermod -aG render lemonade"},
          "Setup requests render only");
    host.subids = false;
    check(inaccessible.check_podman()->id == "subordinate_ids", "Service subordinate IDs checked");
    host.subids = true;
    host.gpu = true;
    host.files["/sys/fs/selinux/enforce"] = "1";
    host.files["/sys/fs/selinux/booleans/container_use_devices"] = "0 0";
    check(inaccessible.check_selinux()->id == "selinux", "SELinux device permission checked");
    auto run = spec();
    run.devices = {"/dev/kfd"};
    rejects([&] { inaccessible.run_command(run); }, "KFD run enforces SELinux check");
    check(ContainerManager::podman_install_command("ID=ubuntu\nID_LIKE=debian") ==
              "sudo apt install podman",
          "Distro ancestor selects apt");
    check(ContainerManager::podman_install_command("ID=fedora") == "sudo dnf install podman",
          "Fedora selects dnf");
    check(ContainerManager::podman_install_command("ID=arch") == "sudo pacman -S podman",
          "Arch selects pacman");
    rejects([] { ContainerManager incomplete(ContainerHost{}); },
            "Incomplete effects never fall back to host");
}

void mounts_images_networks() {
    FakeHost host;
    ContainerManager manager(host.host());
    host.links = {{"/snapshot", "/snapshot"},
                  {"/snapshot/a", "/blobs/a"},
                  {"/snapshot/sub/b", "/blobs/b"},
                  {"/alias", "/blobs/a"}};
    host.kinds = {{"/snapshot", ContainerFileKind::Directory},
                  {"/blobs/a", ContainerFileKind::File},
                  {"/blobs/b", ContainerFileKind::File}};
    host.inventory = {"/snapshot/sub/b", "/snapshot/a"};
    auto mounts = manager.model_mounts("/snapshot", "/mnt/models/bundle");
    check(mounts.size() == 2 && mounts[0].source == "/blobs/a" &&
              mounts[0].destination == "/mnt/models/bundle/a" && mounts[1].source == "/blobs/b" &&
              mounts[1].destination == "/mnt/models/bundle/sub/b",
          "HF links materialize as sorted individual files");
    check(manager.model_mounts("/alias", "/mnt/models/main")[0].source == "/blobs/a",
          "Single model symlink resolved");
    rejects([&] { manager.model_mounts("/missing", "/mnt/models/main"); },
            "Missing model rejected");
    host.inventory.push_back("/snapshot/a");
    rejects([&] { manager.model_mounts("/snapshot", "/mnt/models/bundle"); },
            "Duplicate mount target rejected");
    host.inventory = {"/elsewhere/a"};
    rejects([&] { manager.model_mounts("/snapshot", "/mnt/models/bundle"); },
            "Escaped inventory rejected");
    host.inventory = {"/snapshot/a"};
    host.kinds["/blobs/a"] = ContainerFileKind::Other;
    rejects([&] { manager.model_mounts("/snapshot", "/mnt/models/bundle"); },
            "Unsupported model type rejected");
    const auto s = spec();
    check(manager.has_image(s.image), "Image presence uses fake host");
    manager.remove_image(s.image);
    host.stream_lines = {"Copying blob aaa", "Copying blob aaa done", "bbb: Pulling fs layer",
                         "bbb: Pull complete"};
    std::vector<int> progress;
    manager.pull(s.image, [&](const DownloadProgress& p) {
        progress.push_back(p.percent);
        return true;
    });
    check(progress.front() == 0 && progress.back() == 100, "Pull progress ends at 100");
    check(host.ran({"podman", "pull", s.image}) && host.ran({"podman", "rmi", s.image}),
          "Pinned image pull/remove exact argv");
    rejects([&] { manager.pull(s.image, [](const auto&) { return false; }); },
            "Pull cancellation before host execution");
    rejects([&] { manager.pull(s.image, [](const auto& p) { return p.percent == 0; }); },
            "Pull cancellation during stream");
    check(host.cancelled_stream, "Host stream receives cancellation");
    host.stream_status = 2;
    rejects([&] { manager.pull(s.image, {}); }, "Failed pull rejected");
    manager.create_network(s.network);
    check(host.ran({"podman", "network", "create", "--internal", "--label", "ai.lemonade",
                    "--disable-dns", s.network}),
          "Labelled internal network created");
    manager.stop(s.name);
    check(host.ran({"podman", "stop", "--time", "10", s.name}) &&
              host.ran({"podman", "rm", "-f", s.name}) &&
              host.ran({"podman", "network", "rm", s.network}),
          "Stop removes container and owned network");
    host.fail_command = "stop";
    const auto before = host.commands.size();
    manager.stop(s.name);
    check(host.commands.size() == before + 3, "Cleanup continues after host stop exception");
    host.fail_command = "rm";
    manager.stop(s.name);
    check(host.commands.back() == std::vector<std::string>{"podman", "network", "rm", s.network},
          "Network cleanup survives container remove exception");
    FakeHost rootful;
    rootful.podman_info = "false netavark";
    ContainerManager root_manager(rootful.host());
    root_manager.create_network(s.network);
    check(rootful.ran(
              {"podman", "network", "create", "--internal", "--label", "ai.lemonade", s.network}),
          "Rootful network retains DNS gateway");
}

void addresses() {
    FakeHost host;
    ContainerManager manager(host.host());
    const auto s = spec();
    check(manager.address(s.name, s.network) == "10.88.0.4", "Owned network address returned");
    check(host.ran({"podman", "inspect", "--type", "container", s.name}) &&
              host.timeouts.back() == 5,
          "Inspector targets owned container with bound");
    auto original = json::parse(host.inspect);
    auto wrong = original;
    wrong[0]["NetworkSettings"]["Networks"]["aaa-other"] = {{"IPAddress", "10.99.0.8"}};
    host.inspect = wrong.dump();
    check(manager.address(s.name, s.network) == "10.88.0.4", "Wrong network address ignored");
    wrong[0]["NetworkSettings"]["Networks"].erase(s.network);
    host.inspect = wrong.dump();
    rejects([&] { manager.address(s.name, s.network); }, "Missing expected network fails closed");
    wrong = original;
    wrong[0]["Name"] = "/another-container";
    host.inspect = wrong.dump();
    rejects([&] { manager.address(s.name, s.network); }, "Wrong container rejected");
    wrong = original;
    wrong[0]["Config"]["Labels"] = json::object();
    host.inspect = wrong.dump();
    rejects([&] { manager.address(s.name, s.network); }, "Unlabelled container rejected");
    wrong = original;
    wrong.push_back(original[0]);
    host.inspect = wrong.dump();
    rejects([&] { manager.address(s.name, s.network); }, "Ambiguous inspection array rejected");
    wrong = original;
    wrong[0]["NetworkSettings"]["Networks"][s.network]["GlobalIPv6Address"] = "fd00::1";
    host.inspect = wrong.dump();
    rejects([&] { manager.address(s.name, s.network); }, "Ambiguous dual address rejected");
    host.inspect = original.dump();
    auto pos = host.inspect.find("\"IPAddress\":\"10.88.0.4\"");
    host.inspect.replace(pos, std::string("\"IPAddress\":\"10.88.0.4\"").size(),
                         "\"IPAddress\":\"10.88.0.4\",\"IPAddress\":\"10.88.0.5\"");
    rejects([&] { manager.address(s.name, s.network); }, "Duplicate inspection keys rejected");
    for (const auto& address :
         {"", "127.0.0.1", "0.1.2.3", "224.1.2.3", "169.254.2.3", "10.1.2.999", "10.1.2.3.4",
          "10.1.2.3.", "10.01.2.3", "10.1.2.3\n10.1.2.4", "http://10.1.2.3", "10.1.2.3:8000"}) {
        wrong = original;
        wrong[0]["NetworkSettings"]["Networks"][s.network]["IPAddress"] = address;
        host.inspect = wrong.dump();
        rejects([&] { manager.address(s.name, s.network); },
                "Invalid or missing address rejected " + std::string(address));
    }
    host.inspect = "garbage";
    rejects([&] { manager.address(s.name, s.network); }, "Malformed inspection rejected");
    rejects([&] { manager.address(s.name, "unowned-network"); },
            "Unowned address request rejected");
}

struct ClientState {
    ServerCommand invocation;
    bool alive = true;
    bool throw_start = false;
    int starts = 0;
    int stops = 0;
};
class FakeClient : public ServerProcess {
public:
    explicit FakeClient(std::shared_ptr<ClientState> state)
        : ServerProcess({}), state_(std::move(state)) {}
    ~FakeClient() override { handle_ = {nullptr, 0}; }
    std::string start(const ServerCommand& invocation) override {
        ++state_->starts;
        state_->invocation = invocation;
        if (state_->throw_start)
            throw std::runtime_error("injected spawn failure");
        handle_.pid = 4242;
        return "unused-client-host";
    }
    void stop() override {
        ++state_->stops;
        state_->alive = false;
        handle_ = {nullptr, 0};
    }
    bool running() const override { return handle_.pid && state_->alive; }

private:
    std::shared_ptr<ClientState> state_;
};

void processes() {
#ifdef __linux__
    const auto s = spec();
    ServerCommand command;
    command.program = "engine";
    command.args = {"--port", "32123"};
    command.port = s.port;
    command.env = {{"ENGINE", "1"}};
    for (bool rootless : {true, false}) {
        FakeHost host;
        host.podman_info = rootless ? "true netavark" : "false netavark";
        auto manager = std::make_shared<ContainerManager>(host.host());
        auto client = std::make_shared<ClientState>();
        {
            ContainerProcess process({}, s, manager, std::make_unique<FakeClient>(client));
            check(host.commands.empty(), "Container process construction is inert");
            check(process.start(command) == (rootless ? "127.0.0.1" : "10.88.0.4"),
                  "Process returns mode specific host");
            check(process.running() && process.handle().pid == 4242,
                  "Process seam reports client liveness and handle");
            check(client->invocation.program == "podman" && client->invocation.env.empty(),
                  "Only container client spawned without engine host environment");
            check(contains(process.command_line(), "ENGINE=1") &&
                      process.command_line().back() == "32123",
                  "Start command bound to image invocation");
            rejects([&] { process.start(command); }, "Repeated start rejected");
        }
        check(!client->alive && host.ran({"podman", "stop", "--time", "10", s.name}),
              "Destructor stops owned container and client");
    }
    for (const auto& failure : {"spawn", "network", "address", "exited", "timeout", "cancelled"}) {
        FakeHost host;
        host.podman_info = "false netavark";
        auto client = std::make_shared<ClientState>();
        auto now = std::chrono::steady_clock::time_point{};
        bool cancelled = false;
        ContainerProcessServices services{[&] { return cancelled; }, [&] { return now; },
                                          [&](auto duration) {
                                              now += duration;
                                              if (std::string(failure) == "cancelled")
                                                  cancelled = true;
                                          }};
        if (std::string(failure) == "spawn")
            client->throw_start = true;
        if (std::string(failure) == "network")
            host.network_status = 1;
        if (std::string(failure) == "address")
            host.inspect = "[]";
        if (std::string(failure) == "exited")
            client->alive = false;
        if (std::string(failure) == "timeout" || std::string(failure) == "cancelled")
            host.inspect_status = 1;
        ContainerProcess process({}, s, std::make_shared<ContainerManager>(host.host()),
                                 std::make_unique<FakeClient>(client), services);
        rejects([&] { process.start(command); }, "Failed process start " + std::string(failure));
        check(!process.running() && process.handle().pid == 0 && !client->alive &&
                  std::count(host.commands.begin(), host.commands.end(),
                             std::vector<std::string>{"podman", "rm", "-f", s.name}) == 2 &&
                  std::count(host.commands.begin(), host.commands.end(),
                             std::vector<std::string>{"podman", "network", "rm", s.name}) == 2,
              "Partial start cleans up " + std::string(failure));
    }
    {
        thinksound_oracle::HttpFixture http;
        thinksound_oracle::EffectsFixture effects(http.port);
        FakeHost host;
        auto manager = std::make_shared<ContainerManager>(host.host());
        auto client = std::make_shared<ClientState>();
        int factories = 0;
        auto run = s;
        run.port = 0;
        backends::ConfiguredServer server(
            thinksound_oracle::entry(), backends::BackendContext{"error"},
            {std::make_shared<thinksound_oracle::FixtureLaunchServices>(effects),
             [&](ProcessOutput output, const std::string&) {
                 ++factories;
                 return std::make_unique<ContainerProcess>(output, run, manager,
                                                           std::make_unique<FakeClient>(client));
             }});
        auto options = thinksound_oracle::empty_options();
        server.load("fixture", thinksound_oracle::model(), options);
        check(factories == 1 && client->starts == 1 && client->invocation.program == "podman",
              "ConfiguredServer make_process seam launches container adapter");
        server.unload();
        check(!client->alive && host.ran({"podman", "network", "rm", s.name}),
              "Configured unload owns container teardown");
    }
#endif
}

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
#ifdef __linux__
        pins_and_commands();
        host_checks();
        mounts_images_networks();
        addresses();
        processes();
#else
        FakeHost host;
        auto client = std::make_shared<ClientState>();
        ContainerProcess process({}, spec(), std::make_shared<ContainerManager>(host.host()),
                                 std::make_unique<FakeClient>(client));
        rejects([&] { process.start({}); }, "Non-Linux container start rejected");
        check(host.commands.empty() && client->starts == 0,
              "Non-Linux rejection precedes host effects");
#endif
    } catch (const std::exception& error) {
        check(false, std::string("Unexpected exception: ") + error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
