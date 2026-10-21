"""Check the standalone compiler archive for effectful dependencies."""

import argparse
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--binder")
    parser.add_argument("--registry")
    parser.add_argument("--authority")
    args = parser.parse_args()
    symbols = subprocess.check_output([args.nm, "-C", "-u", args.library], text=True)
    forbidden = re.compile(
        r"(?:\b(?:open|open64|fopen|fopen64|socket|connect|bind|listen|accept|"
        r"fork|vfork|system|popen|execve|posix_spawn|stat|lstat|access|getenv|"
        r"read|write|unlink|mkdir|chdir|dlopen|CreateProcess|CreateFile)\b|"
        r"BackendHookRegistry::|installed_backend_hooks|lemon::backends::typed::(?:installed_targets|audit_targets|llamacpp::)|"
        r"BackendOps|ModelManager|RecipeOptions|RuntimeConfig|SystemInfo|BackendUtils|"
        r"provider|factory|WrappedServer|ServerProcess|NativeProcess|"
        r"hardware|httplib|curl_|std::filesystem::(?:exists|status|canonical|"
        r"directory_iterator|current_path|create_|remove|rename))"
    )
    rejected = [line for line in symbols.splitlines() if forbidden.search(line)]
    assert not rejected, "Effectful compiler dependencies: " + "\n".join(rejected)
    assert "compile_programs" in symbols and "schema_document" in symbols
    for archive in [args.library, args.binder, args.registry, args.authority]:
        if archive:
            production = subprocess.check_output([args.nm, "-C", archive], text=True)
            assert not re.search(
                r"observe_entry|test_program|test_targets|test_without_grants|binding::counters|execution::counters",
                production,
            ), (
                "ProductionObjectsContainNoTestInjectionAPI: " + archive
            )
    if args.binder:
        binder = subprocess.check_output([args.nm, "-C", "-u", args.binder], text=True)
        allowed = re.compile(
            r"typed::(?:installed_targets|audit_targets)|CatalogAdmissionBuilder::contains_token"
        )
        rejected = [
            line
            for line in binder.splitlines()
            if forbidden.search(line) and not allowed.search(line)
        ]
        assert not rejected, "Effectful binder dependencies: " + "\n".join(rejected)
        assert (
            "contains_token" in binder
            and "installed_targets" in binder
            and "audit_targets" in binder
        )
    if args.registry:
        listing = subprocess.check_output(
            [args.nm, "-C", "-u", args.registry], text=True
        )
        registry = listing.split("backend_typed_hook_registry_internal.cpp.o:", 1)[
            1
        ].split("\n\n", 1)[0]
        allowed = re.compile(r"typed::llamacpp::")
        rejected = [
            line
            for line in registry.splitlines()
            if forbidden.search(line) and not allowed.search(line)
        ]
        assert not rejected, "Effectful registry dependencies: " + "\n".join(rejected)
        assert "typed::llamacpp::metadata" in registry
    print(
        "Compiler symbol boundary: no filesystem IO, process, hook, hardware or network dependencies"
    )


if __name__ == "__main__":
    main()
