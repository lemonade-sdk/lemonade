# C++ test targets. Shared registration policy lives in cmake/LemonadeTesting.cmake.
# Keep executable placement compatible with the former root declarations.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

set(VLLM_ARG_RESOLVER_TEST_SOURCE "${PROJECT_SOURCE_DIR}/test/cpp/test_vllm_arg_resolver.cpp")
if(BUILD_TESTING AND EXISTS "${VLLM_ARG_RESOLVER_TEST_SOURCE}")
    add_executable(test_vllm_arg_resolver
        ${VLLM_ARG_RESOLVER_TEST_SOURCE}
        ${PROJECT_SOURCE_DIR}/src/cpp/server/backends/vllm/vllm_arg_resolver.cpp
    )
    target_link_libraries(test_vllm_arg_resolver PRIVATE nlohmann_json::nlohmann_json)
    add_cpp_ci_test(vllm_arg_resolver CI OFF COMMAND test_vllm_arg_resolver)
endif()

set(LLAMACPP_REQUEST_TEST_SOURCE "${PROJECT_SOURCE_DIR}/test/cpp/test_llamacpp_request.cpp")
if(BUILD_TESTING AND EXISTS "${LLAMACPP_REQUEST_TEST_SOURCE}")
    add_executable(test_llamacpp_request
        ${LLAMACPP_REQUEST_TEST_SOURCE}
        ${PROJECT_SOURCE_DIR}/src/cpp/server/backends/llamacpp/llamacpp_request.cpp
    )
    target_link_libraries(test_llamacpp_request PRIVATE nlohmann_json::nlohmann_json)
    add_cpp_ci_test(llamacpp_request CI OFF COMMAND test_llamacpp_request)
endif()

set(MCP_CLIENT_CONFIG_TEST_SOURCE "${PROJECT_SOURCE_DIR}/test/cpp/test_mcp_client_config.cpp")
if(BUILD_TESTING AND EXISTS "${MCP_CLIENT_CONFIG_TEST_SOURCE}")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_package(Threads REQUIRED)

    add_executable(test_mcp_client_config
        ${MCP_CLIENT_CONFIG_TEST_SOURCE}
    )
    target_link_libraries(test_mcp_client_config PRIVATE lemonade-server-core)

    if(Python3_Interpreter_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/test/fixtures/mock_mcp_stdio_server.py")
        file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" LEMONADE_TEST_PYTHON_PATH)
        file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/test/fixtures/mock_mcp_stdio_server.py" LEMONADE_TEST_MCP_STDIO_SERVER_PATH)
        target_compile_definitions(test_mcp_client_config PRIVATE
            "LEMONADE_TEST_PYTHON=\"${LEMONADE_TEST_PYTHON_PATH}\""
            "LEMONADE_TEST_MCP_STDIO_SERVER=\"${LEMONADE_TEST_MCP_STDIO_SERVER_PATH}\"")
    endif()

    add_cpp_ci_test(mcp_client_config CI ON COMMAND test_mcp_client_config)
endif()

# ============================================================
# C++ Unit Tests
# ============================================================

set(_ARCHIVE_PLATFORM_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_archive_platform.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ARCHIVE_PLATFORM_TEST_SRC}")
    add_executable(test_archive_platform
        ${PROJECT_SOURCE_DIR}/test/cpp/test_archive_platform.cpp
    )
    target_include_directories(test_archive_platform PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
    )
    add_cpp_ci_test(ArchivePlatformTest CI ON COMMAND test_archive_platform)
endif()

# ProcessManager zombie detection and non-mutating reap contract.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_PROCESS_MANAGER_TEST_SRC
        "${PROJECT_SOURCE_DIR}/test/cpp/test_process_manager.cpp"
    )
    if(EXISTS "${_PROCESS_MANAGER_TEST_SRC}")
        add_executable(test_process_manager
            ${PROJECT_SOURCE_DIR}/test/cpp/test_process_manager.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/server/utils/process_manager.cpp
            ${PROJECT_SOURCE_DIR}/src/cpp/server/utils/platform/process_linux.cpp
        )
        target_include_directories(test_process_manager PRIVATE
            ${PROJECT_SOURCE_DIR}/src/cpp/include
            ${PROJECT_BINARY_DIR}/include
        )
        find_package(Threads REQUIRED)
        target_link_libraries(test_process_manager PRIVATE Threads::Threads)
        add_cpp_ci_test(ProcessManagerTest CI ON COMMAND test_process_manager)
    endif()
endif()

# FastFlowLM argument validation. Keep this test independent from the server
set(_FLM_ARG_RESOLVER_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_flm_arg_resolver.cpp"
)
set(_FLM_ARG_RESOLVER_SRC
    "${PROJECT_SOURCE_DIR}/src/cpp/server/backends/fastflowlm/flm_arg_resolver.cpp"
)
if(BUILD_TESTING AND EXISTS "${_FLM_ARG_RESOLVER_TEST_SRC}" AND EXISTS "${_FLM_ARG_RESOLVER_SRC}")
    add_executable(test_flm_arg_resolver
        ${PROJECT_SOURCE_DIR}/test/cpp/test_flm_arg_resolver.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/backends/fastflowlm/flm_arg_resolver.cpp
    )
    target_include_directories(test_flm_arg_resolver PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )

    add_cpp_ci_test(FlmArgResolverTest CI OFF COMMAND test_flm_arg_resolver)
endif()

# Pure SystemInfo mapping/matching helpers. The production server calls the same
# header-only implementation, so this replaces the old Python replica tests.
set(_SYSTEM_INFO_HELPERS_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_system_info_helpers.cpp"
)
if(BUILD_TESTING AND EXISTS "${_SYSTEM_INFO_HELPERS_TEST_SRC}")
    add_executable(test_system_info_helpers ${_SYSTEM_INFO_HELPERS_TEST_SRC})
    target_include_directories(test_system_info_helpers PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/server
    )
    add_cpp_ci_test(SystemInfoHelpersTest CI ON COMMAND test_system_info_helpers)
endif()

# Custom arg parsing helpers (header-only utility shared by all arg resolvers).
set(_CUSTOM_ARGS_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_custom_args.cpp"
)
if(BUILD_TESTING AND EXISTS "${_CUSTOM_ARGS_TEST_SRC}")
    add_executable(test_custom_args ${_CUSTOM_ARGS_TEST_SRC})
    target_include_directories(test_custom_args PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
    )
    add_cpp_ci_test(CustomArgsTest CI ON COMMAND test_custom_args)
endif()

# Recipe custom-argument scope resolution.
set(_RECIPE_ARG_RESOLVER_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_recipe_arg_resolution.cpp"
)
if(BUILD_TESTING AND EXISTS "${_RECIPE_ARG_RESOLVER_TEST_SRC}")
    add_executable(test_recipe_arg_resolution ${_RECIPE_ARG_RESOLVER_TEST_SRC})
    target_include_directories(test_recipe_arg_resolution PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
    )
    add_cpp_ci_test(RecipeArgResolutionTest CI ON COMMAND test_recipe_arg_resolution)
endif()

set(_DIR_WATCHER_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_directory_watcher.cpp"
)
set(_DIR_WATCHER_LIB_SRC
    "${PROJECT_SOURCE_DIR}/src/cpp/server/directory_watcher.cpp"
)
if(BUILD_TESTING AND EXISTS "${_DIR_WATCHER_TEST_SRC}" AND EXISTS "${_DIR_WATCHER_LIB_SRC}")
    add_executable(test_directory_watcher
        ${PROJECT_SOURCE_DIR}/test/cpp/test_directory_watcher.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/directory_watcher.cpp
    )
    target_include_directories(test_directory_watcher PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    if(UNIX)
        target_link_libraries(test_directory_watcher PRIVATE pthread)
        target_link_options(test_directory_watcher PRIVATE -pthread)
    endif()

    # Enable testing and register the test with CTest
    include(CTest)
    add_cpp_ci_test(DirectoryWatcherTest CI ON COMMAND test_directory_watcher)
endif()

# GGUF capability detection (MTP / vision / tool-calling label logic).
# Header-only unit under test, so no extra source files are required.
set(_GGUF_CAPS_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_gguf_capabilities.cpp"
)
if(BUILD_TESTING AND EXISTS "${_GGUF_CAPS_TEST_SRC}")
    add_executable(test_gguf_capabilities
        ${PROJECT_SOURCE_DIR}/test/cpp/test_gguf_capabilities.cpp
    )
    target_include_directories(test_gguf_capabilities PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )

    # Enable testing and register the test with CTest
    include(CTest)
    add_cpp_ci_test(GgufCapabilitiesTest CI OFF COMMAND test_gguf_capabilities)
endif()

# Transcription response_format interpretation (issue #1833): the requested
# format, not a parse attempt, decides how a successful body is read.
# Header-only unit under test, so no extra source files are required.
set(_TRANSCRIPTION_FORMAT_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_transcription_response_format.cpp"
)
if(BUILD_TESTING AND EXISTS "${_TRANSCRIPTION_FORMAT_TEST_SRC}")
    add_executable(test_transcription_response_format
        ${PROJECT_SOURCE_DIR}/test/cpp/test_transcription_response_format.cpp
    )
    target_include_directories(test_transcription_response_format PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_transcription_response_format PRIVATE nlohmann_json::nlohmann_json)

    include(CTest)
    add_cpp_ci_test(TranscriptionResponseFormatTest CI ON COMMAND test_transcription_response_format)
endif()

set(_GGUF_SHARD_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_gguf_shard_utils.cpp"
)
if(BUILD_TESTING AND EXISTS "${_GGUF_SHARD_TEST_SRC}")
    add_executable(test_gguf_shard_utils
        ${PROJECT_SOURCE_DIR}/test/cpp/test_gguf_shard_utils.cpp
    )
    target_include_directories(test_gguf_shard_utils PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_gguf_shard_utils PRIVATE lemonade-server-core)
    add_dependencies(test_gguf_shard_utils copy_resources)

    include(CTest)
    add_cpp_ci_test(GgufShardUtilsTest CI ON COMMAND test_gguf_shard_utils)
endif()

# Per-model artifact selection and snapshot baselines used to decide whether an
# upstream commit actually touches a downloaded model.
set(_HF_UPDATE_CHECK_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_hf_update_check.cpp"
)
if(BUILD_TESTING AND EXISTS "${_HF_UPDATE_CHECK_TEST_SRC}")
    add_executable(test_hf_update_check
        ${PROJECT_SOURCE_DIR}/test/cpp/test_hf_update_check.cpp
    )
    target_include_directories(test_hf_update_check PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_hf_update_check PRIVATE lemonade-server-core)
    add_dependencies(test_hf_update_check copy_resources)

    include(CTest)
    add_cpp_ci_test(HfUpdateCheckTest CI ON COMMAND test_hf_update_check)
endif()

# Model residency role/pool contract for router dependencies
set(_MODEL_RESIDENCY_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_model_residency.cpp"
)
if(BUILD_TESTING AND EXISTS "${_MODEL_RESIDENCY_TEST_SRC}")
    add_executable(test_model_residency
        ${PROJECT_SOURCE_DIR}/test/cpp/test_model_residency.cpp
    )
    target_include_directories(test_model_residency PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )

    include(CTest)
    add_cpp_ci_test(ModelResidencyTest CI ON COMMAND test_model_residency)
endif()

# Pure version-resolution policy test (lemon::resolve_latest_pin). Header-only
# logic, so the test links nothing beyond the standard library — regression
# coverage for #2265 (fall back to the installed binary when the GitHub
# "latest" lookup fails).
set(_LATEST_FALLBACK_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_latest_version_fallback.cpp"
)
if(BUILD_TESTING AND EXISTS "${_LATEST_FALLBACK_TEST_SRC}")
    add_executable(test_latest_version_fallback
        ${PROJECT_SOURCE_DIR}/test/cpp/test_latest_version_fallback.cpp
    )
    target_include_directories(test_latest_version_fallback PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )

    include(CTest)
    add_cpp_ci_test(LatestVersionFallbackTest CI ON COMMAND test_latest_version_fallback)
endif()

# Label-to-deployment-mode mapping (lemon::get_model_type_from_labels). Header-only,
# so the test links nothing beyond the standard library.
set(_MODEL_TYPE_CLASSIFIER_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_model_type_classifier.cpp"
)
if(BUILD_TESTING AND EXISTS "${_MODEL_TYPE_CLASSIFIER_TEST_SRC}")
    add_executable(test_model_type_classifier
        ${PROJECT_SOURCE_DIR}/test/cpp/test_model_type_classifier.cpp
    )
    target_include_directories(test_model_type_classifier PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )

    include(CTest)
    add_cpp_ci_test(ModelTypeClassifierTest CI ON COMMAND test_model_type_classifier)
endif()

# Descriptor contract: every mode a backend advertises in `supported_modes` is
# backed by a capability interface its server class implements. Compiles the
# server class definitions (for the compile-time capability mask) but calls
# nothing on them, so it links only the descriptor registry.
set(_BACKEND_MODE_CONTRACT_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_backend_mode_contract.cpp"
)
if(BUILD_TESTING AND EXISTS "${_BACKEND_MODE_CONTRACT_TEST_SRC}")
    add_executable(test_backend_mode_contract
        ${PROJECT_SOURCE_DIR}/test/cpp/test_backend_mode_contract.cpp
        ${LEMON_BACKEND_DESCRIPTOR_SOURCES}
    )
    target_include_directories(test_backend_mode_contract PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_backend_mode_contract PRIVATE
        nlohmann_json::nlohmann_json
        lemonade-httplib
    )

    include(CTest)
    add_cpp_ci_test(BackendModeContractTest CI ON COMMAND test_backend_mode_contract)
endif()

# Job engine pure core: the `when`/`branch` expression evaluator + reference
# resolution, and step-graph validation (forward-only ⇒ no loops). Header-only,
# so these link only nlohmann/json.
set(_JOB_EXPR_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_job_expr.cpp")
if(BUILD_TESTING AND EXISTS "${_JOB_EXPR_TEST_SRC}")
    add_executable(test_job_expr ${PROJECT_SOURCE_DIR}/test/cpp/test_job_expr.cpp)
    target_include_directories(test_job_expr PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_job_expr PRIVATE nlohmann_json::nlohmann_json)
    include(CTest)
    add_cpp_ci_test(JobExprTest CI ON COMMAND test_job_expr)
endif()

set(_JOB_GRAPH_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_job_graph.cpp")
if(BUILD_TESTING AND EXISTS "${_JOB_GRAPH_TEST_SRC}")
    add_executable(test_job_graph ${PROJECT_SOURCE_DIR}/test/cpp/test_job_graph.cpp)
    target_include_directories(test_job_graph PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_job_graph PRIVATE nlohmann_json::nlohmann_json)
    include(CTest)
    add_cpp_ci_test(JobGraphTest CI ON COMMAND test_job_graph)
endif()

# Backend install atomicity (staging + verified atomic swap).
# Header-only unit under test, so no extra source files are required.
set(_INSTALL_ATOMICITY_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_install_atomicity.cpp"
)
if(BUILD_TESTING AND EXISTS "${_INSTALL_ATOMICITY_TEST_SRC}")
    add_executable(test_install_atomicity
        ${PROJECT_SOURCE_DIR}/test/cpp/test_install_atomicity.cpp
    )
    target_include_directories(test_install_atomicity PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )

    # Enable testing and register the test with CTest
    include(CTest)
    add_cpp_ci_test(InstallAtomicityTest CI OFF COMMAND test_install_atomicity)
endif()

# Routing engine contract surface (issue #2407). Header-only foundation: the
# shared types/interfaces in routing_policy.h plus the fake ClassifierServices.
# The test loads the L0a-L3 fixtures from the source tree via ROUTING_FIXTURE_DIR.
set(_ROUTING_CONTRACT_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_contract.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_CONTRACT_TEST_SRC}")
    add_executable(test_routing_policy_contract
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_contract.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
    )
    target_include_directories(test_routing_policy_contract PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_SOURCE_DIR}/test/cpp
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_policy_contract PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(test_routing_policy_contract PRIVATE
        ROUTING_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/test/cpp/fixtures/routing"
    )

    include(CTest)
    add_cpp_ci_test(RoutingPolicyContractTest CI ON COMMAND test_routing_policy_contract)
endif()

# Routing match evaluator (issue #2378): composite Condition nodes,
# classifier-band leaf evaluation, MatchExpr compilation, trace and memoization.
set(_ROUTING_EVALUATOR_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_evaluator.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_EVALUATOR_TEST_SRC}")
    add_executable(test_routing_policy_evaluator
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_evaluator.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
    )
    target_include_directories(test_routing_policy_evaluator PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_policy_evaluator PRIVATE nlohmann_json::nlohmann_json)

    include(CTest)
    add_cpp_ci_test(RoutingPolicyEvaluatorTest CI ON COMMAND test_routing_policy_evaluator)
endif()

# Routing classifier/condition registry (issue #2379): classifier JSON
# instantiation, classifier leaf resolution, deterministic leaf dispatch seams.
set(_ROUTING_REGISTRY_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_registry.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_REGISTRY_TEST_SRC}")
    add_executable(test_routing_policy_registry
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_registry.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
    )
    target_include_directories(test_routing_policy_registry PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_SOURCE_DIR}/test/cpp
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_policy_registry PRIVATE nlohmann_json::nlohmann_json)

    include(CTest)
    add_cpp_ci_test(RoutingPolicyRegistryTest CI ON COMMAND test_routing_policy_registry)
endif()

# Routing semantic_similarity classifier (issue #2381): max-cosine scoring over
# reference phrases, reference-phrase embedding caching, inclusive band
# boundaries, and on_error handling, all against a fake embedder.
set(_ROUTING_SEMANTIC_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_semantic.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_SEMANTIC_TEST_SRC}")
    add_executable(test_routing_policy_semantic
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_semantic.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
    )
    target_include_directories(test_routing_policy_semantic PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_SOURCE_DIR}/test/cpp
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_policy_semantic PRIVATE nlohmann_json::nlohmann_json)

    include(CTest)
    add_cpp_ci_test(RoutingPolicySemanticTest CI ON COMMAND test_routing_policy_semantic)
endif()

# LLM router classifier + routing.router desugaring (issue #2405): the L0a
# on-ramp. Unit + parser + end-to-end tests against FakeClassifierServices.
set(_ROUTING_LLM_ROUTER_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_llm_router.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_LLM_ROUTER_TEST_SRC}")
    add_executable(test_routing_policy_llm_router
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_llm_router.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy_parser.cpp
    )
    target_include_directories(test_routing_policy_llm_router PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_SOURCE_DIR}/test/cpp
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_policy_llm_router PRIVATE nlohmann_json::nlohmann_json)

    include(CTest)
    add_cpp_ci_test(RoutingPolicyLlmRouterTest CI ON COMMAND test_routing_policy_llm_router)
endif()

# Routing deterministic leaf conditions (issue #2380): keywords/regex/min_chars/
# max_chars/has_*/metadata conditions and their leaf-factory map.
set(_ROUTING_DETERMINISTIC_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_deterministic.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_DETERMINISTIC_TEST_SRC}")
    add_executable(test_routing_policy_deterministic
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_deterministic.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
    )
    target_include_directories(test_routing_policy_deterministic PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_SOURCE_DIR}/test/cpp
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_policy_deterministic PRIVATE nlohmann_json::nlohmann_json)

    include(CTest)
    add_cpp_ci_test(RoutingPolicyDeterministicTest CI ON COMMAND test_routing_policy_deterministic)
endif()

# Routing engine assembly (issue #2382): RoutingPolicyEngine constructor rule
# compilation, first-match-wins route(), default fail-open, trace opt-in,
# on_error error paths, and concurrent route() consistency against a fake.
set(_ROUTING_ENGINE_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_engine.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_ENGINE_TEST_SRC}")
    add_executable(test_routing_policy_engine
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_engine.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
    )
    target_include_directories(test_routing_policy_engine PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_SOURCE_DIR}/test/cpp
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_policy_engine PRIVATE nlohmann_json::nlohmann_json)
    find_package(Threads REQUIRED)
    target_link_libraries(test_routing_policy_engine PRIVATE Threads::Threads)

    include(CTest)
    add_cpp_ci_test(RoutingPolicyEngineTest CI ON COMMAND test_routing_policy_engine)
endif()

# Routing policy parser (issue #2383): collection.router JSON -> RoutePolicy,
# schema/parser key parity, component resolution, and validation errors.
set(_ROUTING_PARSER_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_parser.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_PARSER_TEST_SRC}")
    add_executable(test_routing_policy_parser
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_parser.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy_parser.cpp
    )
    target_include_directories(test_routing_policy_parser PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_SOURCE_DIR}/test/cpp
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_policy_parser PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(test_routing_policy_parser PRIVATE
        ROUTING_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/test/cpp/fixtures/routing"
        ROUTING_SCHEMA_FILE="${PROJECT_SOURCE_DIR}/src/cpp/resources/schemas/route_policy.schema.json"
    )

    include(CTest)
    add_cpp_ci_test(RoutingPolicyParserTest CI ON COMMAND test_routing_policy_parser)
endif()


# Routing policy store (issue #2383): directory-backed policy loading and
# DirectoryWatcher-triggered shared_ptr snapshot swaps.
set(_ROUTING_STORE_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_store.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_STORE_TEST_SRC}")
    add_executable(test_routing_policy_store
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_policy_store.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy_parser.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy_store.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/directory_watcher.cpp
    )
    target_include_directories(test_routing_policy_store PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_SOURCE_DIR}/test/cpp
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_policy_store PRIVATE nlohmann_json::nlohmann_json)
    find_package(Threads REQUIRED)
    target_link_libraries(test_routing_policy_store PRIVATE Threads::Threads)
    target_compile_definitions(test_routing_policy_store PRIVATE
        ROUTING_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/test/cpp/fixtures/routing"
    )

    include(CTest)
    add_cpp_ci_test(RoutingPolicyStoreTest CI ON COMMAND test_routing_policy_store)
endif()

set(_ROUTE_DECISION_RESPONSE_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_route_decision_response.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTE_DECISION_RESPONSE_TEST_SRC}")
    add_executable(test_route_decision_response
        ${PROJECT_SOURCE_DIR}/test/cpp/test_route_decision_response.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/route_decision_response.cpp
    )
    target_include_directories(test_route_decision_response PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_route_decision_response PRIVATE nlohmann_json::nlohmann_json)
    if(USE_SYSTEM_HTTPLIB)
        target_link_libraries(test_route_decision_response PRIVATE lemonade-httplib)
    else()
        target_link_libraries(test_route_decision_response PRIVATE httplib::httplib)
    endif()

    include(CTest)
    add_cpp_ci_test(RouteDecisionResponseTest CI OFF COMMAND test_route_decision_response)
endif()

# Router back-compat conformance corpus (issue #2425): replays the golden
# policy -> Decision cases under ${PROJECT_SOURCE_DIR}/test/conformance/routing/ through the real
# engine and asserts each emitted Decision equals its recorded expectation.
set(_ROUTING_CONFORMANCE_CORPUS_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_conformance_corpus.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_CONFORMANCE_CORPUS_TEST_SRC}")
    add_executable(test_routing_conformance_corpus
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_conformance_corpus.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/route_decision_response.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_classifier_services.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/thinking_controls.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy_parser.cpp
    )
    target_include_directories(test_routing_conformance_corpus PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_conformance_corpus PRIVATE nlohmann_json::nlohmann_json)
    if(USE_SYSTEM_HTTPLIB)
        target_link_libraries(test_routing_conformance_corpus PRIVATE lemonade-httplib)
    else()
        target_link_libraries(test_routing_conformance_corpus PRIVATE httplib::httplib)
    endif()
    target_compile_definitions(test_routing_conformance_corpus PRIVATE
        CONFORMANCE_CORPUS_DIR="${PROJECT_SOURCE_DIR}/test/conformance/routing"
    )

    include(CTest)
    add_cpp_ci_test(RoutingConformanceCorpusTest CI ON COMMAND test_routing_conformance_corpus)
endif()

# ModelManager collection validation for collection.router registration:
# /pull-style validation rejects malformed routing and preserves the routing
# block in ModelInfo::extras after user-model registration.
set(_MODEL_MANAGER_COLLECTION_VALIDATION_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_model_manager_collection_validation.cpp"
)
if(BUILD_TESTING AND EXISTS "${_MODEL_MANAGER_COLLECTION_VALIDATION_TEST_SRC}")
    add_executable(test_model_manager_collection_validation
        ${PROJECT_SOURCE_DIR}/test/cpp/test_model_manager_collection_validation.cpp
    )
    target_link_libraries(test_model_manager_collection_validation PRIVATE lemonade-server-core)
    add_dependencies(test_model_manager_collection_validation copy_resources)

    include(CTest)
    add_cpp_ci_test(ModelManagerCollectionValidationTest CI ON COMMAND test_model_manager_collection_validation)
endif()

# Recipe hiding: a backend with every built-in model dropped by the system-memory
# heuristic (and no user model left) is hidden from the recipe/backends listing.
set(_RECIPE_HIDING_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_recipe_hiding.cpp")
if(BUILD_TESTING AND EXISTS "${_RECIPE_HIDING_TEST_SRC}")
    add_executable(test_recipe_hiding ${PROJECT_SOURCE_DIR}/test/cpp/test_recipe_hiding.cpp)
    target_link_libraries(test_recipe_hiding PRIVATE lemonade-server-core)
    add_dependencies(test_recipe_hiding copy_resources)

    include(CTest)
    add_cpp_ci_test(RecipeHidingTest CI ON COMMAND test_recipe_hiding)
endif()

# Remote model registry source parsing, cache namespaces, snapshot fingerprints,
# and persistence of ModelScope provenance in ModelInfo.
set(_MODEL_REGISTRY_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_model_registry.cpp"
)
if(BUILD_TESTING AND EXISTS "${_MODEL_REGISTRY_TEST_SRC}")
    add_executable(test_model_registry ${PROJECT_SOURCE_DIR}/test/cpp/test_model_registry.cpp)
    target_link_libraries(test_model_registry PRIVATE lemonade-server-core)
    add_dependencies(test_model_registry copy_resources)

    include(CTest)
    add_cpp_ci_test(ModelRegistryTest CI OFF COMMAND test_model_registry)
endif()

set(_MODEL_ALIAS_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_model_alias.cpp"
)
if(BUILD_TESTING AND EXISTS "${_MODEL_ALIAS_TEST_SRC}")
    add_executable(test_model_alias ${PROJECT_SOURCE_DIR}/test/cpp/test_model_alias.cpp)
    target_link_libraries(test_model_alias PRIVATE lemonade-server-core)
    add_dependencies(test_model_alias copy_resources)

    include(CTest)
    add_cpp_ci_test(ModelAliasTest CI ON COMMAND test_model_alias)
endif()

# Router-backed ClassifierServices wiring (issue #2384): binds the pure engine's
# embed/run_classifier/chat lambdas to Router-style JSON calls.
set(_ROUTING_CLASSIFIER_SERVICES_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_classifier_services.cpp"
)
if(BUILD_TESTING AND EXISTS "${_ROUTING_CLASSIFIER_SERVICES_TEST_SRC}")
    add_executable(test_routing_classifier_services
        ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_classifier_services.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_classifier_services.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/thinking_controls.cpp
        ${PROJECT_SOURCE_DIR}/src/cpp/server/routing_policy.cpp
    )
    target_include_directories(test_routing_classifier_services PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )
    target_link_libraries(test_routing_classifier_services PRIVATE nlohmann_json::nlohmann_json)

    include(CTest)
    add_cpp_ci_test(RoutingClassifierServicesTest CI ON COMMAND test_routing_classifier_services)
endif()

# Auto-tune: GGUF array storage, scalar derivation, weighted KV cache computation.
# Covers head_count_kv_per_layer, sliding_window_pattern, SWA precise weighted sum,
# full_attention_interval exact count, and scalar fallback paths.
set(_AUTO_TUNE_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_auto_tune.cpp"
)
if(BUILD_TESTING AND EXISTS "${_AUTO_TUNE_TEST_SRC}")
    add_executable(test_auto_tune
        ${PROJECT_SOURCE_DIR}/test/cpp/test_auto_tune.cpp
    )
    target_include_directories(test_auto_tune PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cpp/include
        ${PROJECT_BINARY_DIR}/include
    )

    include(CTest)
    add_cpp_ci_test(AutoTuneTest CI OFF COMMAND test_auto_tune)
endif()

set(_TELEMETRY_HELPERS_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_telemetry_helpers.cpp")
if(BUILD_TESTING AND EXISTS "${_TELEMETRY_HELPERS_TEST_SRC}")
    add_executable(test_telemetry_helpers ${PROJECT_SOURCE_DIR}/test/cpp/test_telemetry_helpers.cpp)
    target_include_directories(test_telemetry_helpers PRIVATE "${PROJECT_SOURCE_DIR}/src/cpp/server")
    target_link_libraries(test_telemetry_helpers PRIVATE lemonade-server-core)
    add_cpp_ci_test(TelemetryHelpersTest CI ON COMMAND test_telemetry_helpers)
endif()

set(_SUSPEND_INHIBITOR_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_suspend_inhibitor.cpp")
if(BUILD_TESTING AND EXISTS "${_SUSPEND_INHIBITOR_TEST_SRC}")
    add_executable(test_suspend_inhibitor ${PROJECT_SOURCE_DIR}/test/cpp/test_suspend_inhibitor.cpp)
    target_link_libraries(test_suspend_inhibitor PRIVATE lemonade-server-core)
    add_cpp_ci_test(SuspendInhibitorTest CI OFF COMMAND test_suspend_inhibitor)
endif()

set(_CONFIG_MIGRATION_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_config_migration.cpp")
if(BUILD_TESTING AND EXISTS "${_CONFIG_MIGRATION_TEST_SRC}")
    add_executable(test_config_migration ${PROJECT_SOURCE_DIR}/test/cpp/test_config_migration.cpp)
    target_link_libraries(test_config_migration PRIVATE lemonade-server-core)
    add_cpp_ci_test(ConfigMigrationTest CI OFF COMMAND test_config_migration)
endif()

set(_CONFIG_DIR_MIGRATION_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_config_dir_migration.cpp")
if(BUILD_TESTING AND EXISTS "${_CONFIG_DIR_MIGRATION_TEST_SRC}")
    add_executable(test_config_dir_migration ${PROJECT_SOURCE_DIR}/test/cpp/test_config_dir_migration.cpp)
    target_link_libraries(test_config_dir_migration PRIVATE lemonade-server-core)
    add_cpp_ci_test(ConfigDirMigrationTest CI ON COMMAND test_config_dir_migration)
endif()

set(_CONFIG_MERGING_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_config_merging.cpp")
if(BUILD_TESTING AND EXISTS "${_CONFIG_MERGING_TEST_SRC}")
    add_executable(test_config_merging ${PROJECT_SOURCE_DIR}/test/cpp/test_config_merging.cpp)
    target_link_libraries(test_config_merging PRIVATE lemonade-server-core)
    add_cpp_ci_test(ConfigMergingTest CI ON COMMAND test_config_merging)
endif()

set(_CONFIG_DISCOVERY_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_config_discovery.cpp")
if(BUILD_TESTING AND EXISTS "${_CONFIG_DISCOVERY_TEST_SRC}")
    add_executable(test_config_discovery ${PROJECT_SOURCE_DIR}/test/cpp/test_config_discovery.cpp)
    target_link_libraries(test_config_discovery PRIVATE lemonade-server-core)
    add_cpp_ci_test(ConfigDiscoveryTest CI ON COMMAND test_config_discovery)
endif()

set(_CONFIG_TELEMETRY_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_config_telemetry.cpp")
if(BUILD_TESTING AND EXISTS "${_CONFIG_TELEMETRY_TEST_SRC}")
    add_executable(test_config_telemetry ${PROJECT_SOURCE_DIR}/test/cpp/test_config_telemetry.cpp)
    target_link_libraries(test_config_telemetry PRIVATE lemonade-server-core)
    add_cpp_ci_test(ConfigTelemetryTest CI ON COMMAND test_config_telemetry)
endif()

set(_RUNTIME_CONFIG_BACKEND_VALIDATION_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_runtime_config_backend_validation.cpp"
)
if(BUILD_TESTING AND EXISTS "${_RUNTIME_CONFIG_BACKEND_VALIDATION_TEST_SRC}")
    add_executable(test_runtime_config_backend_validation
        ${PROJECT_SOURCE_DIR}/test/cpp/test_runtime_config_backend_validation.cpp
    )
    target_link_libraries(test_runtime_config_backend_validation PRIVATE lemonade-server-core)
    add_cpp_ci_test(RuntimeConfigBackendValidationTest CI ON
        COMMAND test_runtime_config_backend_validation
    )
endif()

set(_RUNTIME_CONFIG_DIRECTORY_VALIDATION_TEST_SRC
    "${PROJECT_SOURCE_DIR}/test/cpp/test_runtime_config_directory_validation.cpp"
)
if(BUILD_TESTING AND EXISTS "${_RUNTIME_CONFIG_DIRECTORY_VALIDATION_TEST_SRC}")
    add_executable(test_runtime_config_directory_validation
        ${PROJECT_SOURCE_DIR}/test/cpp/test_runtime_config_directory_validation.cpp
    )
    target_link_libraries(test_runtime_config_directory_validation PRIVATE lemonade-server-core)
    add_cpp_ci_test(RuntimeConfigDirectoryValidationTest CI ON
        COMMAND test_runtime_config_directory_validation
    )
endif()

set(_CONFIG_DEFAULT_SOURCE_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_config_default_source.cpp")
if(BUILD_TESTING AND EXISTS "${_CONFIG_DEFAULT_SOURCE_TEST_SRC}")
    add_executable(test_config_default_source ${PROJECT_SOURCE_DIR}/test/cpp/test_config_default_source.cpp)
    target_link_libraries(test_config_default_source PRIVATE lemonade-server-core)
    add_cpp_ci_test(ConfigDefaultSourceTest CI ON COMMAND test_config_default_source)
endif()

set(_CLI_RUNTIME_OVERRIDE_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_cli_runtime_override.cpp")
if(BUILD_TESTING AND EXISTS "${_CLI_RUNTIME_OVERRIDE_TEST_SRC}")
    add_executable(test_cli_runtime_override ${PROJECT_SOURCE_DIR}/test/cpp/test_cli_runtime_override.cpp)
    target_link_libraries(test_cli_runtime_override PRIVATE lemonade-server-core)
    add_cpp_ci_test(CliRuntimeOverrideTest CI ON COMMAND test_cli_runtime_override)
endif()

# ROCm root resolution (ROCM_PATH / rocm-sdk / /opt/rocm priority): covers the
# external-ROCm detection that lets Lemonade skip the bundled TheRock download.
set(_ROCM_ROOT_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_rocm_root_resolution.cpp")
if(BUILD_TESTING AND EXISTS "${_ROCM_ROOT_TEST_SRC}")
    add_executable(test_rocm_root_resolution ${PROJECT_SOURCE_DIR}/test/cpp/test_rocm_root_resolution.cpp)
    target_link_libraries(test_rocm_root_resolution PRIVATE lemonade-server-core)
    add_cpp_ci_test(RocmRootResolutionTest CI OFF COMMAND test_rocm_root_resolution)
endif()

set(_GPU_ENV_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_backend_utils_gpu_env.cpp")
if(BUILD_TESTING AND EXISTS "${_GPU_ENV_TEST_SRC}")
    add_executable(test_backend_utils_gpu_env ${PROJECT_SOURCE_DIR}/test/cpp/test_backend_utils_gpu_env.cpp)
    target_link_libraries(test_backend_utils_gpu_env PRIVATE lemonade-server-core)
    add_cpp_ci_test(BackendUtilsGpuEnvTest CI ON COMMAND test_backend_utils_gpu_env)
endif()

set(_DLL_VERSION_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_backend_utils_dll_version.cpp")
if(BUILD_TESTING AND EXISTS "${_DLL_VERSION_TEST_SRC}")
    add_executable(test_backend_utils_dll_version ${PROJECT_SOURCE_DIR}/test/cpp/test_backend_utils_dll_version.cpp)
    target_link_libraries(test_backend_utils_dll_version PRIVATE lemonade-server-core)
    add_cpp_ci_test(BackendUtilsDllVersionTest CI ON COMMAND test_backend_utils_dll_version)
endif()

set(_PIP_DL_PARSER_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_pip_download_parser.cpp")
if(BUILD_TESTING AND EXISTS "${_PIP_DL_PARSER_TEST_SRC}")
    add_executable(test_pip_download_parser ${PROJECT_SOURCE_DIR}/test/cpp/test_pip_download_parser.cpp)
    target_link_libraries(test_pip_download_parser PRIVATE lemonade-server-core)
    add_cpp_ci_test(PipDownloadParserTest CI ON COMMAND test_pip_download_parser)
endif()

set(_GFX_ARCH_GATING_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_gfx_arch_gating.cpp")
if(BUILD_TESTING AND EXISTS "${_GFX_ARCH_GATING_TEST_SRC}")
    add_executable(test_gfx_arch_gating ${PROJECT_SOURCE_DIR}/test/cpp/test_gfx_arch_gating.cpp)
    target_link_libraries(test_gfx_arch_gating PRIVATE lemonade-server-core)
    add_cpp_ci_test(GfxArchGatingTest CI ON COMMAND test_gfx_arch_gating)
endif()

set(_NETWORK_UTILS_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_network_utils.cpp")
if(BUILD_TESTING AND EXISTS "${_NETWORK_UTILS_TEST_SRC}")
    add_executable(test_network_utils ${PROJECT_SOURCE_DIR}/test/cpp/test_network_utils.cpp)
    target_link_libraries(test_network_utils PRIVATE lemonade-server-core)
    add_cpp_ci_test(NetworkUtilsTest CI ON COMMAND test_network_utils)
endif()

set(_URL_UTILS_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_url_utils.cpp")
if(BUILD_TESTING AND EXISTS "${_URL_UTILS_TEST_SRC}")
    add_executable(test_url_utils ${PROJECT_SOURCE_DIR}/test/cpp/test_url_utils.cpp)
    target_link_libraries(test_url_utils PRIVATE lemonade-server-core)
    add_cpp_ci_test(UrlUtilsTest CI OFF COMMAND test_url_utils)
endif()

set(_GITHUB_API_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_github_api.cpp")
if(BUILD_TESTING AND EXISTS "${_GITHUB_API_TEST_SRC}")
    add_executable(test_github_api ${PROJECT_SOURCE_DIR}/test/cpp/test_github_api.cpp)
    target_link_libraries(test_github_api PRIVATE lemonade-server-core)
    add_cpp_ci_test(GithubApiTest CI ON COMMAND test_github_api)
endif()

# GGUF variant enumeration: grouping, name disambiguation, and quant ordering.
set(_HF_VARIANTS_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_hf_variants.cpp")
if(BUILD_TESTING AND EXISTS "${_HF_VARIANTS_TEST_SRC}")
    add_executable(test_hf_variants ${PROJECT_SOURCE_DIR}/test/cpp/test_hf_variants.cpp)
    target_link_libraries(test_hf_variants PRIVATE lemonade-server-core)
    add_cpp_ci_test(HfVariantsTest CI ON COMMAND test_hf_variants)
endif()

# vLLM CDNA gating: the vllm descriptor support row + asset-family resolution for
# AMD Instinct (gfx942), alongside the existing RDNA families.
set(_VLLM_CDNA_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_vllm_cdna_gating.cpp")
if(BUILD_TESTING AND EXISTS "${_VLLM_CDNA_TEST_SRC}")
    add_executable(test_vllm_cdna_gating ${PROJECT_SOURCE_DIR}/test/cpp/test_vllm_cdna_gating.cpp)
    target_link_libraries(test_vllm_cdna_gating PRIVATE lemonade-server-core)
    add_cpp_ci_test(VllmCdnaGatingTest CI OFF COMMAND test_vllm_cdna_gating)
endif()

# llamacpp gfx950 (MI350) install gate: only the Linux + stable-channel asset is
# published, so the shared Windows/Linux support row must not advertise Windows
# or nightly installs for gfx950.
set(_LLAMACPP_GFX950_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_llamacpp_gfx950_gating.cpp")
if(BUILD_TESTING AND EXISTS "${_LLAMACPP_GFX950_TEST_SRC}")
    add_executable(test_llamacpp_gfx950_gating ${PROJECT_SOURCE_DIR}/test/cpp/test_llamacpp_gfx950_gating.cpp)
    target_link_libraries(test_llamacpp_gfx950_gating PRIVATE lemonade-server-core)
    add_cpp_ci_test(LlamacppGfx950GatingTest CI OFF COMMAND test_llamacpp_gfx950_gating)
endif()

set(_HTTP_CLIENT_SECURITY_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_http_client_security.cpp")
if(BUILD_TESTING AND EXISTS "${_HTTP_CLIENT_SECURITY_TEST_SRC}")
    add_executable(test_http_client_security ${PROJECT_SOURCE_DIR}/test/cpp/test_http_client_security.cpp)
    target_link_libraries(test_http_client_security PRIVATE lemonade-server-core)
    add_cpp_ci_test(HttpClientSecurityTest CI OFF COMMAND test_http_client_security)
endif()

set(_STREAMING_PROXY_CANCEL_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_streaming_proxy_cancel.cpp")
if(BUILD_TESTING AND EXISTS "${_STREAMING_PROXY_CANCEL_TEST_SRC}")
    add_executable(test_streaming_proxy_cancel ${PROJECT_SOURCE_DIR}/test/cpp/test_streaming_proxy_cancel.cpp)
    target_link_libraries(test_streaming_proxy_cancel PRIVATE lemonade-server-core)
    add_cpp_ci_test(StreamingProxyCancelTest CI OFF COMMAND test_streaming_proxy_cancel)
endif()

set(_CLOUD_DISCOVERY_POLICY_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_cloud_discovery_policy.cpp")
if(BUILD_TESTING AND EXISTS "${_CLOUD_DISCOVERY_POLICY_TEST_SRC}")
    add_executable(test_cloud_discovery_policy ${PROJECT_SOURCE_DIR}/test/cpp/test_cloud_discovery_policy.cpp)
    target_link_libraries(test_cloud_discovery_policy PRIVATE lemonade-server-core)
    add_cpp_ci_test(CloudDiscoveryPolicyTest CI ON COMMAND test_cloud_discovery_policy)
endif()

set(_CLOUD_PROVIDER_REGISTRY_CONFIG_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_cloud_provider_registry_config.cpp")
if(BUILD_TESTING AND EXISTS "${_CLOUD_PROVIDER_REGISTRY_CONFIG_TEST_SRC}")
    add_executable(test_cloud_provider_registry_config ${PROJECT_SOURCE_DIR}/test/cpp/test_cloud_provider_registry_config.cpp)
    target_link_libraries(test_cloud_provider_registry_config PRIVATE lemonade-server-core)
    add_cpp_ci_test(CloudProviderRegistryConfigTest CI ON COMMAND test_cloud_provider_registry_config)
endif()

set(_ANTHROPIC_ERROR_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_anthropic_error.cpp")
if(BUILD_TESTING AND EXISTS "${_ANTHROPIC_ERROR_TEST_SRC}")
    add_executable(test_anthropic_error ${PROJECT_SOURCE_DIR}/test/cpp/test_anthropic_error.cpp)
    target_link_libraries(test_anthropic_error PRIVATE lemonade-server-core)
    add_cpp_ci_test(AnthropicErrorTest CI ON COMMAND test_anthropic_error)
endif()

set(_ORIGIN_UTILS_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_origin_utils.cpp")
if(BUILD_TESTING AND EXISTS "${_ORIGIN_UTILS_TEST_SRC}")
    add_executable(test_origin_utils ${PROJECT_SOURCE_DIR}/test/cpp/test_origin_utils.cpp)
    target_link_libraries(test_origin_utils PRIVATE lemonade-server-core)
    add_cpp_ci_test(OriginUtilsTest CI ON COMMAND test_origin_utils)
endif()

set(_STREAMING_MEM_FILTER_TEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/test/cpp/test_streaming_memory_filter.cpp")
if(BUILD_TESTING AND EXISTS "${_STREAMING_MEM_FILTER_TEST_SRC}")
    add_executable(test_streaming_memory_filter ${PROJECT_SOURCE_DIR}/test/cpp/test_streaming_memory_filter.cpp)
    target_link_libraries(test_streaming_memory_filter PRIVATE lemonade-server-core)
    add_cpp_ci_test(StreamingMemoryFilterTest CI ON COMMAND test_streaming_memory_filter)
endif()

if(BUILD_TESTING)
    add_cpp_ci_test(HttplibDetection_config_only CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_config_only -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=config_only -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_old_header CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_old_header -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=old_header -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_modern_header CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_modern_header -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=modern_header -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_openssl_target CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_openssl_target -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=openssl_target -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_disabled_generator_expression CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_disabled_generator_expression -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=disabled_generator_expression -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_too_old_config CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_too_old_config -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=too_old_config -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_compiled_no_tls CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_compiled_no_tls -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=compiled_no_tls -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_too_old_config_multiarch CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_too_old_config_multiarch -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=too_old_config_multiarch -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_header_only_pkgconfig CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_header_only_pkgconfig -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=header_only_pkgconfig -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_cli_composition_mbedtls_isolation CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_cli_composition_mbedtls_isolation -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=cli_composition_mbedtls_isolation -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_custom_httplib_dir_precheck CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_custom_httplib_dir_precheck -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=custom_httplib_dir_precheck -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    add_cpp_ci_test(HttplibDetection_polluted_target_fatal CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_polluted_target_fatal -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=polluted_target_fatal -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
    set_tests_properties(HttplibDetection_polluted_target_fatal PROPERTIES WILL_FAIL TRUE)
    add_cpp_ci_test(HttplibDetection_wolfssl_target CI OFF COMMAND ${CMAKE_COMMAND} -B ${PROJECT_BINARY_DIR}/test_httplib_wolfssl_target -S ${PROJECT_SOURCE_DIR}/test/cmake -DTEST_CASE=wolfssl_target -DDETECTION_MACRO_PATH=${CMAKE_SOURCE_DIR}/cmake/DetectSystemHttplib.cmake)
endif()

# Routing-helper reconciliation: durable needed-set + non-blocking prune that
# skips busy/pinned helpers, plus a policy-update-vs-busy concurrency race.
set(_ROUTING_HELPER_RECONCILE_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_routing_helper_reconcile.cpp")
if(BUILD_TESTING AND EXISTS "${_ROUTING_HELPER_RECONCILE_TEST_SRC}")
    add_executable(test_routing_helper_reconcile ${PROJECT_SOURCE_DIR}/test/cpp/test_routing_helper_reconcile.cpp)
    target_link_libraries(test_routing_helper_reconcile PRIVATE lemonade-server-core)
    add_cpp_ci_test(RoutingHelperReconcileTest CI ON COMMAND test_routing_helper_reconcile)
endif()

# Launch command bookkeeping: the command recorded at spawn must keep describing
# the process currently held, so a restart replaces it and cleanup erases it.
set(_LAUNCH_COMMAND_TEST_SRC "${PROJECT_SOURCE_DIR}/test/cpp/test_launch_command.cpp")
if(BUILD_TESTING AND EXISTS "${_LAUNCH_COMMAND_TEST_SRC}")
    add_executable(test_launch_command ${PROJECT_SOURCE_DIR}/test/cpp/test_launch_command.cpp)
    target_link_libraries(test_launch_command PRIVATE lemonade-server-core)
    add_cpp_ci_test(LaunchCommandTest CI ON COMMAND test_launch_command)
endif()
