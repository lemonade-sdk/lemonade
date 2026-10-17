#include "lemon/backends/fastflowlm/fastflowlm_models.h"
#include "lemon/models/registry_id.h"

#include <stdexcept>
#include <string>
#include <vector>

using lemon::models::detail::add_descriptor;
using lemon::models::RegistryDescriptor;
using lemon::models::RegistryId;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

bool has(const std::vector<std::string>& args, const std::string& value) {
    for (const auto& arg : args) {
        if (arg == value) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> pull(const std::string& source, bool do_not_upgrade = true) {
    return lemon::backends::fastflowlm::flm_pull_arguments("org/repo", do_not_upgrade,
                                                           RegistryId(source));
}

bool throws(const std::string& source) {
    try {
        pull(source);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

static void test_the_flag_follows_the_protocol() {
    const std::vector<std::string> api_host = pull("modelscope");
    check("a host served through its own API gets flm's flag for that",
          has(api_host, "--modelscope") && has(api_host, "1"));

    const std::vector<std::string> hub = pull("huggingface");
    check("a plain hub host gets no flag", !has(hub, "--modelscope"));
    check("the pull still names the checkpoint",
          hub.size() == 2 && hub[0] == "pull" && hub[1] == "org/repo");
    check("a refresh still forces", has(pull("huggingface", false), "--force"));
}

static void test_an_unnamed_host_is_the_default_host() {
    check("an entry naming no host pulls from the default, with no flag",
          !has(pull(""), "--modelscope"));
}

static void test_an_unrecognized_host_stops_the_pull() {
    check("an unrecognized host is refused instead of pulling from the default",
          throws("modelplace"));
}

static void test_a_protocol_flm_cannot_express_stops_the_pull() {
    RegistryDescriptor pigeon;
    pigeon.id = RegistryId("carrier-pigeon");
    pigeon.aliases = {"carrier-pigeon"};
    pigeon.display_name = "Carrier Pigeon";
    pigeon.download_protocol = "carrier-pigeon-ftp";
    add_descriptor(pigeon);

    check("a protocol with no flag mapping stops the pull rather than guessing a host",
          throws("carrier-pigeon"));
}

}  // namespace

int main() {
    test_the_flag_follows_the_protocol();
    test_an_unnamed_host_is_the_default_host();
    test_an_unrecognized_host_stops_the_pull();
    test_a_protocol_flm_cannot_express_stops_the_pull();

    if (g_failures != 0) {
        std::printf("FAILED: %d\n", g_failures);
        return 1;
    }
    std::printf("All flm pull-argument checks passed\n");
    return 0;
}
