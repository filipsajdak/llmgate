cmake_minimum_required(VERSION 3.30)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD f35a9ac6-8463-4d38-8eec-5d6008153e7d)
project(llmgate VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(stdexec
  GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
  GIT_TAG nvhpc-26.05
  GIT_SHALLOW ON
  EXCLUDE_FROM_ALL)
set(STDEXEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(stdexec)

find_package(OpenSSL REQUIRED)
find_package(simdjson REQUIRED)
find_package(llhttp REQUIRED)

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

add_library(llmgate_core STATIC src/core/contract_violation.cpp src/core/kqueue_reactor.cpp)
target_include_directories(llmgate_core PUBLIC src)
target_link_libraries(llmgate_core PUBLIC llmgate_flags STDEXEC::stdexec)

add_executable(s0_toolchain spike/s0_toolchain.cpp)
target_link_libraries(s0_toolchain PRIVATE llmgate_core)

enable_testing()
add_test(NAME s0_toolchain COMMAND s0_toolchain)

add_library(llmgate_reflect)
target_sources(llmgate_reflect PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES src/reflect/enum.cppm)
target_link_libraries(llmgate_reflect PUBLIC llmgate_flags)
set_target_properties(llmgate_reflect PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_library(llmgate_tls)
target_sources(llmgate_tls PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES src/tls/tls.cppm)
target_link_libraries(llmgate_tls PUBLIC llmgate_flags llmgate_reflect OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(llmgate_tls PROPERTIES CXX_SCAN_FOR_MODULES ON)

function(llmgate_unit_test name)
  cmake_parse_arguments(ARG "MODULES" "" "LIBS" ${ARGN})
  add_executable(${name} tests/unit/${name}.cpp)
  target_link_libraries(${name} PRIVATE Boost::ut ${ARG_LIBS})
  if(ARG_MODULES)
    set_target_properties(${name} PROPERTIES CXX_SCAN_FOR_MODULES ON)
  endif()
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS unit)
endfunction()

llmgate_unit_test(kqueue_reactor_test LIBS llmgate_core)
llmgate_unit_test(reflect_enum_test MODULES LIBS llmgate_reflect llmgate_core)
llmgate_unit_test(tls_test MODULES LIBS llmgate_tls llmgate_core)
