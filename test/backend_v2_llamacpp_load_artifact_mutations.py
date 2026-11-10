"""Compiled semantic mutations for the llama.cpp candidate load/artifact corpus."""

import backend_v2_llamacpp_runtime_mutations as driver


def mutations():
    base = "src/cpp/server/backends/configured/"
    runner = base + "backend_definition_runner.cpp"
    services = base + "backend_artifact_services.cpp"
    typed = base + "typed/llamacpp_artifacts.cpp"
    yield (
        "device-binding",
        runner,
        "const auto device = binding == program_.bindings.end() ? std::string() : options.at(binding->public_name).template get<std::string>();",
        "const auto device = std::string();",
        "runtime_services",
        "FrozenLoadParity:linux_x64:device-rocm-Vulkan0",
    )
    yield (
        "failure-device-assignment",
        base + "configured_server.cpp",
        "[this](DeviceType device) { device_type_ = device; }",
        "[this](DeviceType) {}",
        "runtime_services",
        "ConfiguredServerAssignsDeviceBeforeInstallationFailure:cpu",
    )
    yield (
        "empty-backend-selection",
        base + "typed/llamacpp_hooks.cpp",
        'if (backend != "auto" && !services.supports(backend))',
        'if (!backend.empty() && backend != "auto" && !services.supports(backend))',
        "runtime_services",
        "FrozenLoadParity:linux_x64:backend-empty-stable",
    )
    yield (
        "program-error-order",
        runner,
        "for (const auto& phase : launch.prepare_order)",
        'for (const auto& phase : (spec.recipe == "llamacpp" ? std::vector<std::string>{"backend", "program", "install", "hook", "artifacts", "ports"} : launch.prepare_order))',
        "runtime_services",
        "FrozenLoadParity:linux_x64:backend-cpu-stable",
    )
    yield (
        "zero-port-policy",
        runner,
        "(number == 0 && port.reject_zero)",
        "number == 0",
        "runtime_services",
        "DeclaredZeroPortPolicyAllowsZero",
    )
    yield (
        "staging-effect-order",
        base + "typed/llamacpp_hooks.cpp",
        "services.stage({*input.staging, architecture});",
        "result.staging.push_back({*input.staging, architecture});",
        "runtime_services",
        "FrozenLoadParity:windows_x64:backend-rocm-stable",
    )
    yield (
        "missing-root-fallback",
        typed,
        "if (!root.exists) return root.path;",
        'if (!root.exists) return "";',
        "runtime_services",
        "FrozenArtifactParity:linux_x64:gguf-missing",
    )
    yield (
        "live-manifest",
        services,
        ' / ".download_manifest.json")) committed = false;',
        ' / ".download_manifest.json")) committed = true;',
        "runtime_services",
        "FrozenArtifactParity:linux_x64:gguf-snapshot-manifest",
    )
    yield (
        "ambiguous-variant",
        typed,
        "if (resolved.emplace(relative, entry->resolved_path).second) paths.push_back(relative);",
        "if (paths.empty() && resolved.emplace(relative, entry->resolved_path).second) paths.push_back(relative);",
        "runtime_services",
        "FrozenArtifactParity:linux_x64:gguf-ambiguous-token",
    )
    yield (
        "architecture-vision-inference",
        services,
        "else inspect_gguf_string(key, value, result.capabilities);",
        "inspect_gguf_string(key, value, result.capabilities);",
        "runtime_services",
        "FrozenArtifactParity:linux_x64:gguf-metadata-llm",
    )


if __name__ == "__main__":
    driver.mutations = mutations
    driver.main()
