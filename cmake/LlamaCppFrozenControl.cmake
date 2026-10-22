set(_llama_control_root "${CMAKE_CURRENT_SOURCE_DIR}/.synagent/scratch/backend-v2-4e3a/frozen-control")
add_custom_target(llamacpp-frozen-control-build
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/llamacpp_frozen_control.py build
        --root ${_llama_control_root} --dependencies ${FETCHCONTENT_BASE_DIR}
    USES_TERMINAL)
foreach(_view linux_x64 linux_arm64 windows_x64 macos_arm64)
    add_custom_target(test_llamacpp_oracle_${_view})
    add_dependencies(test_llamacpp_oracle_${_view} llamacpp-frozen-control-build)
    add_cpp_ci_test(LlamaCppOracle_${_view}Test CI ON
        COMMAND ${_llama_control_root}/build/test_llamacpp_oracle_${_view}
        DEPENDS test_llamacpp_oracle_${_view})
endforeach()
foreach(_view linux windows macos)
    add_custom_target(test_llamacpp_oracle_descriptor_${_view})
    add_dependencies(test_llamacpp_oracle_descriptor_${_view} llamacpp-frozen-control-build)
    add_cpp_ci_test(LlamaCppOracleDescriptor_${_view}Test CI ON
        COMMAND ${_llama_control_root}/build/test_llamacpp_oracle_descriptor_${_view}
        DEPENDS test_llamacpp_oracle_descriptor_${_view})
endforeach()
