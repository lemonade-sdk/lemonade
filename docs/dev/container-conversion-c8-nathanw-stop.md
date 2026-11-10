# C8 Nathanw stop: pre-install hf_load refusal

Source: `d491c946fed2ab079f7387cf02a0eaabb51afce4`,
`src/cpp/server/backends/llamacpp/llamacpp_server.cpp:308-317`:

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
`Fake install failure`. `C8EarlyHfRefusalUnavailable:nathanw` and
`C8SourceRefusalErrorDiffers:nathanw` require that concrete counterexample.
The complete attempted profile is `test/fixtures/backend-v2/c8.nathanw.blocked.json`.
It is test data, not production authority or a migrated leaf.

This contradicts the handoff's no-blocker claim. C7b's narrower source statement
is: "All five engines' identified C7/C7a data-contract blockers are now
expressible." A sealed early-check capability and a separately disclosed
prerequisite stage are still needed before this production conversion.
No production definition, grant, pin, model update or identity stage is retained.
