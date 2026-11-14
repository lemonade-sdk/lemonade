# C8 Halo Box stop: pre-install hf_load refusal

Source: `23e953915c96d2b2a039a0b3ffdfae46f97301d5`,
`src/cpp/server/backends/llamacpp/llamacpp_server.cpp:310-319`:

```cpp
if (container && model_info.extra<bool>("hf_load", false)) {
    throw std::runtime_error(
        "Model '" + model_name + "' has llama-server download its own weights, which llamacpp:" +
        llamacpp_backend + " cannot do from its container's private network. "
        "Load it on a llamacpp backend that runs on the host.");
}
```

Only after this refusal does the source call `install_backend`. At C7b base
`3af47274a`, the closed `launch.checks` record has only `id`, `require`, `error`;
`DefinitionRunner::prepare_load` evaluates those checks after the complete
preparation loop. The existing Prepare hook provides the `hf_load` fact but
has no error effect; the Backend hook does not receive that fact. No authored
check can run between backend selection and installation while retaining
installation before main-artifact admission and the original diagnostics.

The CPU probe combines `hf_load=true` with a failing fake installation service.
The source must return the quoted private-network refusal without installation.
The representable attempted profile instead calls installation and returns
`Fake install failure`. `C8EarlyHfRefusalUnavailable:halobox` and
`C8SourceRefusalErrorDiffers:halobox` require that concrete counterexample.
The complete attempted profile is `test/fixtures/backend-v2/c8.halobox.blocked.json`.
It is test data, not production authority or a migrated leaf.

This contradicts the handoff's no-blocker claim. C7b's narrower source statement
is: "All five engines' identified C7/C7a data-contract blockers are now
expressible." A sealed early-check capability and a separately disclosed
prerequisite stage are still needed before this production conversion.
No production definition, grant, pin, model update or identity stage is retained.
