"""Inspect the production artifact adapter and private typed-hook archive."""

import argparse
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--object", required=True)
    parser.add_argument("--production", required=True)
    args = parser.parse_args()
    symbols = subprocess.check_output([args.nm, "-C", "-u", args.object], text=True)
    forbidden = re.compile(
        r"BackendOps|ModelManager|RecipeOptions|RuntimeConfig|SystemInfo|BackendUtils|"
        r"typed::llamacpp::(?:runtime_options|prepare|environment|select_backend|"
        r"validate_device|install_context|resolve_version|check_install)|"
        r"std::filesystem|basic_(?:i|o|f)fstream|(?:^|\W)(?:open|open64|fopen|"
        r"stat|lstat|access|getenv|system|popen|execve|fork|socket|connect|read|write|"
        r"CreateFile|CreateProcess)(?:\W|$)|curl_|httplib"
    )
    rejected = [line for line in symbols.splitlines() if forbidden.search(line)]
    rejected.extend(
        line
        for line in symbols.splitlines()
        if " U lemon::" in line and " U lemon::enumerate_gguf_variants(" not in line
    )
    assert not rejected, "ArtifactAdaptersHaveNoAmbientRuntimeCapability: " + "\n".join(
        rejected
    )
    production = subprocess.check_output([args.nm, "-C", args.production], text=True)
    assert "observe_entry" not in production, "test entry seam in production"
    print(
        "ArtifactAdaptersHaveNoAmbientRuntimeCapability: production object has no "
        "ambient runtime/system, filesystem, process or network dependency; "
        "production archive has no test seam"
    )


if __name__ == "__main__":
    main()
