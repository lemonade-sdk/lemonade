"""One-property compiled mutations for bounded llama.cpp probes and lookup order."""

import backend_v2_llamacpp_runtime_mutations as driver


def mutations():
    services = driver.CONFIGURED + "backend_runtime_services.cpp"
    process = "src/cpp/server/utils/process_manager.cpp"
    rules = [
        (
            "probe-read-bound",
            process,
            "result.byte_limit = bytes > max_bytes - result.output.size();",
            "result.byte_limit = false;",
            "ByteCaptureBoundsNewlineFreeCombinedOutput",
        ),
        (
            "probe-wait-bound",
            process,
            "result.timed_out = std::chrono::steady_clock::now() >= deadline;",
            "result.timed_out = false;",
            "ByteCaptureWaitIsBounded",
        ),
        (
            "probe-shell",
            services,
            'capture_output(executable, {"--version"}, bytes, wait, cancel,',
            'capture_output("/bin/sh", {"-c", executable + " --version"}, bytes, wait, cancel,',
            "FixedProbeNeverUsesShellOrArbitraryExecutable",
        ),
        (
            "probe-arbitrary-executable",
            services,
            'const char* binary = "llama-server";',
            'const char* binary = "other-engine";',
            "FixedProbeNeverUsesShellOrArbitraryExecutable",
        ),
        (
            "stdout-only",
            process,
            'else posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);',
            "else posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);",
            "StdoutCaptureSuppressesStderr",
        ),
        (
            "combined-default",
            process,
            "if (output == CaptureOutput::Combined) posix_spawn_file_actions_adddup2",
            "if (false) posix_spawn_file_actions_adddup2",
            "DefaultCaptureStillCombinesBothStreams",
        ),
        (
            "cancellation-wait",
            process,
            "result.cancelled = is_cancelled();",
            "result.cancelled = false;",
            "CaptureCancellationDuringWait",
        ),
        (
            "probe-output-rejection",
            services,
            "result.byte_limit || result.timed_out || result.cancelled",
            "result.timed_out || result.cancelled",
            "TruncatedProbeCannotBecomeVersion",
        ),
        (
            "probe-timeout-rejection",
            services,
            "result.byte_limit || result.timed_out || result.cancelled",
            "result.byte_limit || result.cancelled",
            "FailedBoundedProbeCannotBecomeVersion:timeout",
        ),
        (
            "probe-cancel-rejection",
            services,
            "result.byte_limit || result.timed_out || result.cancelled",
            "result.byte_limit || result.timed_out",
            "FailedBoundedProbeCannotBecomeVersion:cancelled",
        ),
        (
            "posix-first-line",
            services,
            "if (newline != std::string::npos) output.resize(newline + 1);",
            "if (false) output.resize(newline + 1);",
            "FrozenSystemScopedParity:linux_x64:version-413",
        ),
        (
            "posix-255-byte",
            services,
            "output.resize(std::min(output.size(), std::size_t{255}));",
            "",
            "FrozenSystemScopedParity:linux_x64:version-line-limit",
        ),
        (
            "windows-full-output",
            services,
            "if (input_.platform != typed::Platform::Windows)",
            "if (true)",
            "FrozenSystemScopedParity:windows_x64:version-413",
        ),
        (
            "nonzero-status",
            services,
            "if (result.byte_limit || result.timed_out || result.cancelled)",
            "if (result.byte_limit || result.timed_out || result.cancelled || result.exit_code != 0)",
            "FrozenSystemScopedParity:linux_x64:version-nonzero-status",
        ),
        (
            "hip-override",
            services,
            "if (override && !override->empty())",
            "if (false)",
            "FrozenSystemScopedParity:linux_x64:hip-418",
        ),
        (
            "hip-override-name",
            services,
            'name.rfind("libggml-hip", 0) == 0 && name.find(".so") != std::string::npos &&',
            "",
            "FrozenSystemScopedParity:linux_x64:hip-419",
        ),
        (
            "hip-executable-check",
            services,
            "files.regular(executable) && files.executable(executable)",
            "files.regular(executable)",
            "FrozenSystemScopedParity:linux_x64:hip-420",
        ),
        (
            "hip-adjacent-first",
            services,
            'if (files.exists(utils::path_to_utf8(bin / "libggml-hip.so")) ||',
            "if (false ||",
            "FrozenSystemScopedParity:linux_x64:hip-420",
        ),
        (
            "hip-sibling",
            services,
            'files.exists(utils::path_to_utf8(bin.parent_path() / "lib" / "libggml-hip.so"))',
            "false",
            "FrozenSystemScopedParity:linux_x64:hip-421",
        ),
        (
            "hip-first-executable",
            services,
            "                    break;\n                }",
            "                    ;\n                }",
            "FrozenSystemScopedParity:linux_x64:hip-422",
        ),
    ]
    for name, source, before, after, assertion in rules:
        yield name, source, before, after, "runtime_services", assertion

    for name, change, assertion in (
        (
            "unused-actual-record",
            'actual["version-408"]["events"] = llama_corpus::read(view).at("cases").at("version-408").at("events");',
            "EveryProbeRecordUsed:linux_x64:version-408",
        ),
        (
            "undisclosed-scalar",
            'actual["version-408"]["managed"] = "changed";',
            "CompleteRawDiffEqualsSidecar:linux_x64",
        ),
        (
            "missing-actual-case",
            'actual.erase("hip-415");',
            "CompleteRawDiffEqualsSidecar:linux_x64",
        ),
        (
            "extra-actual-case",
            'actual["extra"] = nullptr;',
            "CompleteRawDiffEqualsSidecar:linux_x64",
        ),
    ):
        before = "const auto actual = llama_corpus::capture(view);"
        yield name, "test/cpp/test_llamacpp_v2_oracle.cpp", before, "auto actual = llama_corpus::capture(view); " + change, "test_llamacpp_v2_oracle_linux_x64", assertion


if __name__ == "__main__":
    driver.mutations = mutations
    driver.main()
