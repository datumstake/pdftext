# Only here for projects that already speak CMake. The library itself is one header;
# `add_subdirectory` or a bare `#include "pdftext.h"` are equally valid ways to use it.
cmake_minimum_required(VERSION 3.14)
project(pdftext VERSION 1.0.0 LANGUAGES CXX DESCRIPTION "Single-header PDF text extraction")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The library target carries no sources: consumers get the include path and nothing else.
add_library(pdftext INTERFACE)
target_include_directories(pdftext INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

add_executable(pdftext_cli cli/main.cpp)
target_link_libraries(pdftext_cli PRIVATE pdftext)
set_target_properties(pdftext_cli PROPERTIES OUTPUT_NAME pdftext)

if(MSVC)
    target_compile_options(pdftext_cli PRIVATE /W4 /utf-8)
else()
    target_compile_options(pdftext_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(PDFTEXT_BUILD_TESTS "Build the pdftext test suite" ON)
if(PDFTEXT_BUILD_TESTS)
    enable_testing()
    add_executable(pdftext_tests tests/test_pdftext.cpp)
    target_link_libraries(pdftext_tests PRIVATE pdftext)
    if(MSVC)
        target_compile_options(pdftext_tests PRIVATE /W4 /utf-8)
    else()
        target_compile_options(pdftext_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    # the corpus path is passed in, so the tests do not care about the build directory
    add_test(NAME pdftext_tests
             COMMAND pdftext_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
endif()

install(FILES pdftext.h DESTINATION include)
install(TARGETS pdftext_cli RUNTIME DESTINATION bin)
