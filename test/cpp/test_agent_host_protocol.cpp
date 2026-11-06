// Frames lemond and the agent host exchange.
#include "agent_test_util.h"

#include "lemon/agents/host_protocol.h"

using agent_test::check;
namespace protocol = lemon::agents::protocol;

int main() {
    const std::string token(64, 'a');
    check("token format", protocol::is_token(token) && !protocol::is_token("ab") &&
                              !protocol::is_token(std::string(64, 'G')));
    check("token hash is sha256 hex",
          protocol::token_hash("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("constant-time compare", protocol::constant_time_equal("abc", "abc") &&
                                       !protocol::constant_time_equal("abc", "abd") &&
                                       !protocol::constant_time_equal("abc", "ab"));

    for (const auto& frame : {protocol::hello(token, "10.0", "linux-x64", false),
                              protocol::welcome(true),
                              protocol::request(7, "lemonade.agents_list", {}, token, {}),
                              protocol::cancel(7), protocol::event(7, "progress", {{"x", 1}}),
                              protocol::response(7, 200, {{"ok", true}})}) {
        check("round trip " + frame["type"].get<std::string>(),
              protocol::parse_frame(frame.dump()) == frame);
    }
    check("not json", !protocol::parse_frame("{"));
    check("unknown type", !protocol::parse_frame(R"({"type":"nope"})"));
    check("negative id", !protocol::parse_frame(R"({"type":"cancel","id":-1})"));
    check("response without status", !protocol::parse_frame(R"({"type":"response","id":1,"body":{}})"));
    check("hello without token",
          !protocol::parse_frame(R"({"type":"hello","protocol":1,"version":"x","platform":"y","persistent":false})"));
    check("loopback: empty, 0.0.0.0 and localhost map to 127.0.0.1",
          protocol::loopback_host("") == "127.0.0.1" && protocol::loopback_host("0.0.0.0") == "127.0.0.1" &&
              protocol::loopback_host("localhost") == "127.0.0.1");
    check("loopback: 127/8 and ::1 are kept",
          protocol::loopback_host("127.0.0.2") == "127.0.0.2" && protocol::loopback_host("::1") == "::1" &&
              protocol::loopback_host("[::1]") == "[::1]");
    for (const char* remote : {"127.0.0.1.evil.example", "127.attacker.example", "127.0.0.256",
                               "127.0.0", "1270.0.0.1", "10.0.0.1", "example.com", "127.0.0.1:80",
                               "::2"}) {
        check(std::string("not loopback: ") + remote, !protocol::loopback_host(remote));
    }

    const auto error = protocol::error_body("agent_busy", "busy", {{"version", "1.5.0"}});
    check("error body is lemond's plain shape",
          error["error"] == "busy" && error["code"] == "agent_busy" && error["version"] == "1.5.0");
    return agent_test::finish();
}
