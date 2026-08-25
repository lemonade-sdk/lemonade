# ============================================================
# Self-describing backends registry
# ============================================================
# The authoritative backend list. Each entry is "<recipe>|<stem>":
#   recipe - the recipe string used in server_models.json (may contain dashes)
#   stem   - identifier-safe name and folder. Each backend lives in its own
#            folder, shipping (in namespace lemon::backends::<stem>):
#              include/lemon/backends/<stem>/<stem>.h         inline const descriptor (CLI-safe data)
#              include/lemon/backends/<stem>/<stem>_server.h  WrappedServer subclass + create() decl
#              server/backends/<stem>/<stem>_server.cpp       implementation + create() def
#
# Adding a backend is one line here plus that folder. cmake/BackendCodegen.cmake compiles the server
# source and regenerates the registry headers, which bind each descriptor to its
# create(). Because this list is a tracked input, editing it forces regeneration
# on the next build (a file(GLOB) would silently miss a newly added backend). The
# descriptor is a header-only inline const, so it links into both the lemonade
# CLI and lemond; only lemond links the server sources.
set(LEMON_BACKENDS
    # "<recipe>|<stem>"
    "llamacpp|llamacpp"
    "whispercpp|whispercpp"
    "moonshine|moonshine"
    "kokoro|kokoro"
    "sd-cpp|sdcpp"
    "flm|fastflowlm"
    "ryzenai-llm|ryzenai"
    "vllm|vllm"
    "thenoise|thenoise"
    "cloud|cloud"
    "thinksound|thinksound"
    "acestep|acestep"
    "onnxruntime|onnxruntime"
    "trellis|trellis"
    "openmoss|openmoss"
)
