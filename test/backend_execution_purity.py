"""Check the standalone compiler archive for effectful dependencies."""

import argparse
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--library", required=True)
    args = parser.parse_args()
    symbols = subprocess.check_output([args.nm, "-C", "-u", args.library], text=True)
    forbidden = re.compile(
        r"(?:\b(?:open|open64|fopen|fopen64|socket|connect|bind|listen|accept|"
        r"fork|vfork|system|popen|execve|posix_spawn|stat|lstat|access|getenv|"
        r"read|write|unlink|mkdir|chdir|dlopen|CreateProcess|CreateFile)\b|"
        r"installed_backend_hooks|lemon::backends::typed::(?:installed_targets|audit_targets|llamacpp::)|"
        r"WrappedServer|ServerProcess|NativeProcess|"
        r"hardware|httplib|curl_|std::filesystem::(?:exists|status|canonical|"
        r"directory_iterator|current_path|create_|remove|rename))"
    )
    rejected = [line for line in symbols.splitlines() if forbidden.search(line)]
    assert not rejected, "Effectful compiler dependencies: " + "\n".join(rejected)
    assert "compile_programs" in symbols and "schema_document" in symbols
    print(
        "Compiler symbol boundary: no filesystem IO, process, hook, hardware or network dependencies"
    )


if __name__ == "__main__":
    main()
