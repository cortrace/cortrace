# Cortrace — code coverage via gcovr with a minimum line-coverage gate.
#
# Usage (from top-level CMakeLists.txt):
#   include(cmake/CodeCoverage.cmake)
#   enable_coverage_if_requested()                 # adds flags when -DENABLE_COVERAGE=ON
#   add_coverage_target(<test-target> <min-percent>)
#
# Produces:
#   - `coverage` target: runs tests, writes coverage.xml + HTML, prints summary,
#     and FAILS if line coverage < the minimum threshold.

option(ENABLE_COVERAGE "Build with coverage instrumentation (gcov)" OFF)

function(enable_coverage_if_requested)
    if(ENABLE_COVERAGE)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "Coverage requires GCC or Clang")
        endif()
        message(STATUS "Coverage instrumentation: ENABLED")
        # atomic: the engine tests run decoders on several threads, and plain
        # counter updates race (gcov then reports negative hits, GCC bug 68080).
        add_compile_options(--coverage -O0 -g -fprofile-arcs -ftest-coverage -fprofile-update=atomic)
        add_link_options(--coverage)
    endif()
endfunction()

# add_coverage_target(<test_target> <min_line_percent>)
function(add_coverage_target test_target min_percent)
    if(NOT ENABLE_COVERAGE)
        return()
    endif()
    find_program(GCOVR_EXECUTABLE gcovr)
    if(NOT GCOVR_EXECUTABLE)
        message(WARNING "gcovr not found; `coverage` target unavailable")
        return()
    endif()
    set(_html_dir "${CMAKE_BINARY_DIR}/coverage-html")
    add_custom_target(coverage
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_html_dir}
        COMMAND ${GCOVR_EXECUTABLE}
                --root ${CMAKE_SOURCE_DIR}
                --filter "${CMAKE_SOURCE_DIR}/src/.*"
                --exclude ".*/tests/.*"
                --exclude ".*/tools/.*"
                # OpenCSD adapter is an external-library integration boundary,
                # exercised by the CLI / integration tests (needs a live
                # decoder), not by the synthetic-element unit tests.
                --exclude ".*/src/opencsd_decoder.cpp"
                # gcovr >= 8 asserts when one function shows up on multiple
                # lines in the gcov data (inlining / cxx-abi tags); "separate"
                # keeps each record instead of failing. Harmless on older gcovr.
                --merge-mode-functions=separate
                --xml ${CMAKE_BINARY_DIR}/coverage.xml
                --html-details ${_html_dir}/index.html
                --print-summary
                --fail-under-line ${min_percent}
                # Read only THIS build's data. Without a search path gcovr scans
                # the whole --root, picking up stale .gcda files from any other
                # build directory in the source tree and skewing the number.
                ${CMAKE_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running tests + gcovr (fail under ${min_percent}% line coverage)"
        VERBATIM)
    add_dependencies(coverage ${test_target})
endfunction()
