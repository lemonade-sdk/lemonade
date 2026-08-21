include(CTest)

# Aggregate build target for the C++ tests selected by packaging CI.
# Platform-specific tests add themselves only inside their existing if() block.
if(NOT TARGET cpp-ci-tests)
    add_custom_target(cpp-ci-tests)
endif()

# Label a CTest test for the packaging CI and make the aggregate build target
# depend on every executable/helper target needed by that test. Internal helper
# used by add_cpp_ci_test(); call add_cpp_ci_test() instead of this directly.
#
# Usage:
#   register_cpp_ci_test(TestName test_target [additional_target...])
function(register_cpp_ci_test test_name)
    set_tests_properties(
        ${test_name}
        PROPERTIES
            LABELS "cpp-ci"
    )

    if(ARGN)
        add_dependencies(
            cpp-ci-tests
            ${ARGN}
        )
    endif()
endfunction()

# Declare a CTest test with an explicit CI decision. This is the ONLY supported
# way to add a test: direct add_test() is disabled (see the override installed
# before the test section) so that every test states, at its call site, whether
# it runs in the packaging CI (`ctest -L cpp-ci`).
#
#   * CI ON  -> the test is labeled cpp-ci and its build target(s) become a
#               dependency of the cpp-ci-tests aggregate target.
#   * CI OFF -> the test is created for local/other runs but excluded from CI.
#
# The enclosing if() MUST test BUILD_TESTING. Distro packaging configures with
# BUILD_TESTING=OFF so it does not build ~40 test binaries it then discards, and
# calling this helper in that configuration is a fatal error rather than a
# silent return to the slow build:
#
#   if(BUILD_TESTING AND EXISTS "${_FOO_TEST_SRC}")
#       add_executable(test_foo ...)
#       add_cpp_ci_test(FooTest CI ON COMMAND test_foo)
#   endif()
#
# Usage:
#   add_cpp_ci_test(<TestName>
#                   CI <ON|OFF>                 # required, explicit CI decision
#                   COMMAND <command> [args...] # required, what CTest runs
#                   [DEPENDS <target>...])      # CI build deps; defaults to the
#                                               # first COMMAND token (the test
#                                               # executable target)
function(add_cpp_ci_test test_name)
    cmake_parse_arguments(ACT "" "CI" "COMMAND;DEPENDS" ${ARGN})

    if(ACT_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "add_cpp_ci_test(${test_name}): unexpected argument(s) '${ACT_UNPARSED_ARGUMENTS}' "
            "(check for a misspelled CI/COMMAND/DEPENDS keyword)")
    endif()
    if(NOT DEFINED ACT_CI)
        message(FATAL_ERROR
            "add_cpp_ci_test(${test_name}): required 'CI <ON|OFF>' argument is missing")
    endif()
    string(TOUPPER "${ACT_CI}" _act_ci)
    if(NOT _act_ci STREQUAL "ON" AND NOT _act_ci STREQUAL "OFF")
        message(FATAL_ERROR
            "add_cpp_ci_test(${test_name}): CI must be exactly ON or OFF, got '${ACT_CI}'")
    endif()
    if(NOT ACT_COMMAND)
        message(FATAL_ERROR
            "add_cpp_ci_test(${test_name}): required 'COMMAND <command>' argument is missing")
    endif()
    # Distro packaging configures with BUILD_TESTING=OFF and never runs or ships
    # a test, so reaching here means the enclosing if() forgot to check it —
    # which silently puts the whole test suite back into every package build.
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR
            "add_cpp_ci_test(${test_name}): declared with BUILD_TESTING=OFF. Add "
            "BUILD_TESTING to the enclosing if() guard, as the other test blocks do.")
    endif()

    # _add_test is the built-in add_test preserved by the override below, so this
    # helper stays exempt from the direct-add_test() ban it enforces.
    _add_test(NAME ${test_name} COMMAND ${ACT_COMMAND})

    if(_act_ci STREQUAL "ON")
        if(ACT_DEPENDS)
            register_cpp_ci_test(${test_name} ${ACT_DEPENDS})
        else()
            list(GET ACT_COMMAND 0 _act_ci_target)
            register_cpp_ci_test(${test_name} ${_act_ci_target})
        endif()
    endif()
endfunction()

macro(lemonade_enable_test_registration_guard)
    # Disable direct add_test() from here on: every test below must be declared with
    # add_cpp_ci_test(... CI <ON|OFF> ...) so its CI status is explicit. Overriding
    # the built-in command preserves the original as _add_test (used by the helper
    # above). Installed after all third-party dependencies are configured so their
    # vendored test suites keep working.
    function(add_test)
        message(FATAL_ERROR
            "Direct add_test() is disabled in this project. Declare the test with "
            "add_cpp_ci_test(<Name> CI <ON|OFF> COMMAND <...>) so its CI status is explicit.")
    endfunction()
endmacro()
