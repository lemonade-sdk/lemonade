## Multi Model Spec

This spec describes a major feature enhancement to the C++ Lemonade Server: enabling more than one model to be loaded at the same time.

This work builds on the existing WrappedServer design. The main difference is that, when loading a new WrappedServer, we will no longer uncoditionally evict the prior one. There will be a set of rules that determines when a WrappedServer should be evicted or left in memory and available.

## Eviction Logic

This section details the logic by which an existing WrappedServer will be evicted or kept in memory when we try to load a new WrappedServer.

### Max Loaded Models

Lemonade will have a new argument, `--max-loaded-models LLMS EMBEDDINGS RERANKINGS`, that determines how many WrappedServers can be in memory.

Additionally, we will consider `embedding` and `reranking` models as separate types from chat `llm`.

The default value for max loaded models is `1 1 1`, meaning:
 - 1x `llm` WrappedServer
 - 1x `embedding` WrappedServer
 - 1x `reranking` WrappedServer

If the user puts `--max-loaded-models 3`, that means:
 - 3x `llm` WrappedServer
 - 1x `embedding` WrappedServer (pad with the default value of 1)
 - 1x `reranking` WrappedServer (pad with the default value of 1)

If the user puts `--max-loaded-models 5 2 4`, that means:
 - 5x `llm` WrappedServer
 - 2x `embedding` WrappedServer
 - 4x `reranking` WrappedServer

At load time, these types are identified by the ModelInfo struct labels field:
- `embedding` label: `embedding` type
- `reranking` label: `reranking` type
- Neither of the above: `llm` type

ModelInfo and WrappedServer should add an explicit enum field for tracking the type. This field should also be used in places like llamacpp_server.cpp where the model's type (currently determined by label) is used to adjust settings.

### Least Recently Used Cache

When a new WrappedServer of TYPE is going to be loaded, Lemonade checks whether there is a slot available for that TYPE or not. If there is a slot available, attempt loading the new WrappedServer without evicting the existing ones.

If a slot of TYPE is not available, evict (unload) the least-recently-used WrappedServer of TYPE, then attempt to load the new WrappedServer. This means each WrappedServer has to keep a timestamp of when it was most recently accessed.

Types of access that should update the timestamp:
 - When the WrappedServer load starts and completes.
 - When an inference request (chat/completions, completions, responses, embedding, reranking) starts and completes.

### Error Handling

If we attempt to load a new WrappedServer of TYPE into an available slot, and the load fails for almost any reason (see exceptions below), evict all WrappedServers of every type, then re-attempt loading the new WrappedServer.

This policy is intentionally "nuclear". We chose it because more nuanced policies might be too difficult to implement correctly, and might not be necessary. 

Exceptions to the policy:
 - Do not evict models if the WrappedServer failed to load because a file was not found on disk.

### Additional NPU Rules

There can only be one model loaded on NPU at a time. The following `recipes` occupy the NPU:
- `oga-hybrid`
- `oga-npu`
- `flm`

Each WrappedServer will need to track which device(s) it is using. When a new WrappedServer is being loaded with an NPU recipe, it must check whether any other WrappedServer is using the NPU and evict it (regardless of type or --max-loaded-models).

> Note: There is no inherent limit to the amount of models that can be loaded on CPU or GPU, aside from available RAM. We are currently not tracking RAM usage, see Error Handling above for the alternative policy to RAM tracking.

The full mapping of recipes to devices is:
- `llamacpp`: `gpu`
- `oga-hybrid`: both `gpu` and `npu`
- `oga-npu`: `npu`
- `oga-cpu`: `cpu`
- `flm`: `npu`

ModelInfo and WrappedServer should add an explicit enum field for tracking the target devices. Use Bitmask/Flags Enum (e.g., `device = GPU | NPU)` to enable `if (model.device & Device::NPU)`.

## Additional Considerations

### Concurrency

1. If a WrappedServer is busy fulfilling an inference request, we will let it finish before evicting it. Details:
    - The load should queue up indefinitely. We will assume that the inference request will eventually finish.
2. WrappedServer loads shall be serialized (i.e., never attempt loading two or more WrappedServers at the same time).
    - If there are queued WrappedServer loads, make the eviction policy choices when starting the load for a specific WrappedServer (i.e., make the choices when the WrappedServer is exiting the queue, not when its entering the queue).
    - If a WrappedServer is being auto-loaded for an inference request, make sure that the inference request has a chance to start and finish before this WrappedServer can be evicted (see consideration #1)
        - Auto-load means: if an inference request comes in for a model that is not loaded, Lemonade will load a WrappedServer for that model, then send it the inference request.


### Health Endpoint

The current health endpoint reports `checkpoint_loaded` and `model_loaded`.

It will be updated in these ways:
1. `checkpoint_loaded` and `model_loaded` will refer to the most recently load WrappedServer.
2. Add a new field, `all_models_loaded`, which has a list of the WrappedServers that are loaded. Include:
    - `model_name`
    - `checkpoint`
    - `last_use` (timestamp)
    - `type`
    - `device`
    - `backend_url` (URL of the backend server process, useful for debugging)

### Stats Endpoint

Simiarly to the health endpoint, the stats endpoint should report stats for the most recent inference request on its existing fields.

> Note: we are not adding any more fields at this time.

### Unload Endpoint

Right now, the unload endpoint unloads the only loaded WrappedServer.

New behavior:
1. Add an optional argument, `model_name`. When this is provided, unload that specific WrappedServer.
    - Return a 404 if `model_name` is not loaded.
2. When no `model_name` is provided, unload all WrappedServers.

### Load Endpoint

The `load` endpoint will add the following optional arguments:
- ctx_size
- llamacpp_args
- llamacpp_backend (maps to --llamacpp)

These new optional arguments to `load` override the default values for WrappedServers.

The priority order for defaults is now:
1. Values explicity passed in `load` when loading a WrappedServer.
2. Values set by the user as `lemonade-server` CLI args or env vars.
3. Default hardcoded values in `lemonade-router`.
 

