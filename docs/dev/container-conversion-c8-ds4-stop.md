# C8 OCI DS4 stop: absolute-checkpoint fallback

Source: `8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78`,
`src/cpp/server/backends/ds4/ds4_server.cpp:88-96`:

```cpp
std::string gguf_path = model_info.resolved_path("main");
if (gguf_path.empty() || !fs::exists(gguf_path)) {
    const std::string checkpoint = model_info.checkpoint();
    if (!checkpoint.empty() && fs::path(checkpoint).is_absolute() && fs::exists(checkpoint)) {
        gguf_path = checkpoint;
    }
}
```

The schema recognizes `artifacts.*.absolute_checkpoint_fallback`, but the v2
`DefinitionRunner` constructor explicitly rejects any selected instance with
`Configured v2 artifact consumer is unavailable`. The v1 `LaunchPlanner`
implements it; that implementation does not make it available to v2. The
`ds4_rocm_v1` model contract supplies memory/context behavior, not this fallback.

`test/fixtures/backend-v2/c8.ds4.blocked.json` selects the source-required field
on the C7a DS4 contract fixture. The CPU probe admits, compiles and binds that
definition, then verifies that creating the configured entry fails with the
exact error above (`C8Ds4AbsoluteFallbackUnavailable`). This is an executable
binding gap, not a schema-only rejection or a model/device failure.

This contradicts the handoff's no-blocker claim. C7b states: "All five engines'
identified C7/C7a data-contract blockers are now expressible." A sealed v2
absolute-path artifact consumer and separately disclosed prerequisite stage
remain necessary. No production OCI DS4 definition, grant, pin, model update or
identity stage is retained. The existing native v1 `ds4.json` is unchanged.
