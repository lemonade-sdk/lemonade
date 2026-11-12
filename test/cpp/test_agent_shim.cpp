// The shim Lemonade writes for a managed agent, and the host's PATH walk.
#include "agent_test_util.h"

#include "lemon/agents/agent_shim.h"
#include "lemon/utils/path_utils.h"

using agent_test::check;
using agent_test::read_file;
using agent_test::write_file;
namespace fs = std::filesystem;
using namespace lemon::agents;

int main() {
    const fs::path dir = agent_test::temp_dir("agent-shim");

    const std::string posix = render_posix_shim("pi", "/home/o'neil/my pi/pi");
    check("posix shim has the marker", posix.find(kShimMarker) != std::string::npos);
    check("posix shim quotes spaces and quotes",
          posix.find("exec '/home/o'\\''neil/my pi/pi' \"$@\"") != std::string::npos);
    check("posix shim keeps a user's PI_SKIP_VERSION_CHECK",
          posix.find(": \"${PI_SKIP_VERSION_CHECK=1}\"") != std::string::npos);
    const std::string cmd = render_cmd_shim("pi", "C:\\Users\\u 100%\\pi.exe");
    check("cmd shim doubles %", cmd.find("\"C:\\Users\\u 100%%\\pi.exe\" %*") != std::string::npos);
    check("cmd shim has the marker", cmd.find(kShimMarker) != std::string::npos);
    // cmd.exe reads a batch file in the console code page, so a non-ASCII
    // profile path is written through %LOCALAPPDATA%, which it expands as Unicode.
    const std::string unicode = render_cmd_shim(
        "pi",
        lemon::utils::path_from_utf8(
            "C:\\Users\\Jos\xc3\xa9\\AppData\\Local\\lemonade\\agents\\pi\\1.5.0\\pi.exe"),
        {{"LOCALAPPDATA", "C:\\Users\\Jos\xc3\xa9\\AppData\\Local"}});
    check("cmd shim names the payload through %LOCALAPPDATA%",
          unicode.find("\"%LOCALAPPDATA%\\lemonade\\agents\\pi\\1.5.0\\pi.exe\" %*") !=
              std::string::npos);
    bool ascii = true;
    for (unsigned char c : unicode) ascii = ascii && c < 0x80;
    check("cmd shim is ASCII", ascii);

    const std::string plist =
        render_launch_agent_plist("/Apps/R&D <x>/lemonade-agent-host", "http://127.0.0.1:8000");
    check("LaunchAgent exports what it sources",
          plist.find("set -a;") != std::string::npos && plist.find("set +a;") != std::string::npos);
    check("LaunchAgent reads lemond.conf, then agent-host.env",
          plist.find("lemonade/lemond.conf") < plist.find("lemonade/agent-host.env") &&
              plist.find("lemonade/agent-host.env") != std::string::npos);
    check("LaunchAgent skips a missing env file instead of failing",
          plist.find("[ -f ") != std::string::npos);
    check("LaunchAgent escapes the command for XML",
          plist.find("R&amp;D &lt;x&gt;") != std::string::npos &&
              plist.find("R&D") == std::string::npos);
    check("LaunchAgent serves the lemond it was enabled for",
          plist.find("--persistent --lemond 'http://127.0.0.1:8000'") != std::string::npos);

    const std::string dropin =
        render_systemd_dropin("/opt/my lemonade/lemonade-agent-host", "http://[::1]:8000");
    check("systemd drop-in replaces ExecStart",
          dropin.find("[Service]\nExecStart=\nExecStart=") == 0);
    check("systemd drop-in serves the lemond it was enabled for",
          dropin.find("\"/opt/my lemonade/lemonade-agent-host\" --persistent --lemond "
                      "\"http://[::1]:8000\"") != std::string::npos);
    check("systemd drop-in escapes specifiers and variables",
          render_systemd_dropin("/x/100%$HOME", "u").find("\"/x/100%%$$HOME\"") != std::string::npos);

    const fs::path shim = dir / "bin" / "pi";
    check("missing shim is absent", shim_state(shim) == ShimState::Absent);
    check("writes a new shim", write_shim(shim, posix) && shim_state(shim) == ShimState::Ours);
    const auto first_write = fs::last_write_time(shim);
    check("an identical shim is left alone",
          write_shim(shim, posix) && fs::last_write_time(shim) == first_write);
    check("removes our shim", remove_shim(shim) && !fs::exists(shim));

    write_file(shim, "#!/bin/sh\nexec /usr/local/bin/pi \"$@\"\n");
    check("foreign file is reported", shim_state(shim) == ShimState::Foreign);
    check("foreign file is never overwritten",
          !write_shim(shim, posix) && read_file(shim).find("/usr/local/bin/pi") != std::string::npos);
    check("foreign file is never removed", !remove_shim(shim) && fs::exists(shim));
    const fs::path cyrillic =
        dir / lemon::utils::path_from_utf8("\xd0\x98\xd0\xb2\xd0\xb0\xd0\xbd") / "pi";
    check("writes a shim under a profile name outside the code page",
          write_shim(cyrillic, posix) && shim_state(cyrillic) == ShimState::Ours);

#ifndef _WIN32
    // PATH=[shim dir, other dir]: the external pi is the absolute path in the other dir.
    const fs::path shim_dir = dir / "shims";
    const fs::path other_dir = dir / "other";
    write_shim(shim_dir / "pi", posix);
    write_file(other_dir / "pi", "#!/bin/sh\n");
    fs::permissions(other_dir / "pi", fs::perms::owner_all);
    write_file(dir / "noexec" / "pi", "#!/bin/sh\n");
    const auto matches = path_matches(
        "pi", shim_dir.string() + "::relative:" + (dir / "noexec").string() + ":" + other_dir.string());
    check("PATH walk finds every executable in order",
          matches.size() == 2 && matches[0] == shim_dir / "pi" && matches[1] == other_dir / "pi");
    check("PATH dirs drop a trailing slash",
          path_dirs(shim_dir.string() + "/:/usr/bin").front() == shim_dir.lexically_normal());
    check("PATH walk results are absolute", !matches.empty() && matches[1].is_absolute());
    check("the first match is ours, the second foreign",
          matches.size() == 2 && is_lemonade_shim(matches[0]) && !is_lemonade_shim(matches[1]));
#endif

    std::error_code ec;
    fs::remove_all(dir, ec);
    return agent_test::finish();
}
