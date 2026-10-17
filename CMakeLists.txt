cmake_minimum_required(VERSION 3.30)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD f35a9ac6-8463-4d38-8eec-5d6008153e7d)
project(llmgate VERSION 0.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

find_package(OpenSSL 3.5 REQUIRED)

add_library(llmgate_flags INTERFACE)
target_compile_options(llmgate_flags INTERFACE
  -std=c++26 -freflection -fcontracts -Wall -Wextra -Werror)

FetchContent_Declare(ut
  GIT_REPOSITORY https://github.com/boost-ext/ut.git
  GIT_TAG v2.3.1
  GIT_SHALLOW ON
  SYSTEM
  EXCLUDE_FROM_ALL
  PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/cmake/patch_ut.cmake)
set(BOOST_UT_DISABLE_MODULE ON CACHE BOOL "" FORCE)
set(BOOST_UT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_UT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ut)

FetchContent_Declare(llhttp
  URL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.4.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  SYSTEM
  EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(llhttp)

function(llmgate_module name)
  cmake_parse_arguments(ARG "" "" "INTERFACE;SOURCES;LIBS" ${ARGN})
  add_library(${name})
  target_sources(${name} PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES ${ARG_INTERFACE})
  if(ARG_SOURCES)
    target_sources(${name} PRIVATE ${ARG_SOURCES})
  endif()
  target_link_libraries(${name} PUBLIC llmgate_flags llmgate_contracts ${ARG_LIBS})
endfunction()

add_library(llmgate_contracts STATIC src/core/contract_violation.cpp)
target_link_libraries(llmgate_contracts PUBLIC llmgate_flags)

llmgate_module(llmgate_reflect INTERFACE src/reflect/enum.cppm)
if(APPLE)
  set(LLMGATE_REACTOR src/async/reactor_kqueue.cpp)
else()
  set(LLMGATE_REACTOR src/async/reactor_epoll.cpp)
endif()
llmgate_module(llmgate_async INTERFACE src/async/async.cppm SOURCES ${LLMGATE_REACTOR})
llmgate_module(llmgate_tls INTERFACE src/tls/tls.cppm LIBS llmgate_reflect OpenSSL::SSL OpenSSL::Crypto)
llmgate_module(llmgate_http INTERFACE src/http/http.cppm LIBS llmgate_reflect llhttp::llhttp)
llmgate_module(llmgate_sse INTERFACE src/sse/sse.cppm)
llmgate_module(llmgate_jsonscan INTERFACE src/json/scan.cppm LIBS llmgate_reflect)
llmgate_module(llmgate_anthropic INTERFACE src/anthropic/guard.cppm LIBS llmgate_jsonscan llmgate_sse)
llmgate_module(llmgate_net INTERFACE src/net/net.cppm LIBS llmgate_async llmgate_tls)
llmgate_module(llmgate_httpio INTERFACE src/http/io.cppm LIBS llmgate_async llmgate_http llmgate_net)

enable_testing()

function(llmgate_test name label)
  add_executable(${name} tests/${label}/${name}.cpp)
  target_link_libraries(${name} PRIVATE Boost::ut llmgate_contracts ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS ${label} TIMEOUT 60)
endfunction()

llmgate_test(reflect_enum_test unit llmgate_reflect)
llmgate_test(async_test unit llmgate_async)
llmgate_test(tls_test unit llmgate_tls)
llmgate_test(http_test unit llmgate_http)
llmgate_test(sse_test unit llmgate_sse)
llmgate_test(jsonscan_test unit llmgate_jsonscan)
llmgate_test(anthropic_guard_test unit llmgate_anthropic)
llmgate_test(net_test unit llmgate_net)
llmgate_test(tls_stream_test unit llmgate_net)
target_include_directories(tls_stream_test PRIVATE tests)
target_link_libraries(tls_stream_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
llmgate_test(httpio_test unit llmgate_httpio)
llmgate_test(tls_network_test network llmgate_net)

add_library(llmgate_chaos)
target_sources(llmgate_chaos PUBLIC FILE_SET CXX_MODULES BASE_DIRS tests/support FILES tests/support/chaos.cppm)
target_link_libraries(llmgate_chaos PUBLIC llmgate_flags llmgate_contracts llmgate_net)

add_library(llmgate_scenarios)
target_sources(llmgate_scenarios PUBLIC FILE_SET CXX_MODULES BASE_DIRS tests/support FILES tests/support/scenarios.cppm)
target_include_directories(llmgate_scenarios PRIVATE tests)
target_link_libraries(llmgate_scenarios PUBLIC llmgate_flags llmgate_contracts llmgate_chaos llmgate_httpio llmgate_net llmgate_tls
  OpenSSL::SSL OpenSSL::Crypto)

llmgate_test(schedule_test unit llmgate_scenarios)

add_library(llmgate_fuzz_driver)
target_sources(llmgate_fuzz_driver PUBLIC FILE_SET CXX_MODULES BASE_DIRS tests/fuzz FILES tests/fuzz/driver.cppm)
target_link_libraries(llmgate_fuzz_driver PUBLIC llmgate_flags)

function(llmgate_fuzz name)
  add_executable(fuzz_${name} tests/fuzz/${name}/harness.cpp)
  target_link_libraries(fuzz_${name} PRIVATE llmgate_fuzz_driver llmgate_contracts ${ARGN})
  add_test(NAME fuzz_${name} COMMAND fuzz_${name} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/${name}/corpus)
  set_tests_properties(fuzz_${name} PROPERTIES LABELS fuzz-regression TIMEOUT 60)
endfunction()

llmgate_fuzz(http_parser llmgate_http)
llmgate_fuzz(sse_parser llmgate_sse)
llmgate_fuzz(schedule llmgate_scenarios)
llmgate_fuzz(jsonscan llmgate_jsonscan)
llmgate_fuzz(guard llmgate_anthropic)
