cmake_minimum_required(VERSION 3.20)

project(nimblefix-hello-fix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NIMBLEFIX_HELLO_FIX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(NIMBLEFIX_HELLO_FIX_PUBLIC_INCLUDE "${NIMBLEFIX_HELLO_FIX_ROOT}/include/public")
set(NIMBLEFIX_HELLO_FIX_GENERATED_INCLUDE "${NIMBLEFIX_HELLO_FIX_ROOT}/build/generated")

if(NOT TARGET nimblefix)
  set(NIMBLEFIX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(NIMBLEFIX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(NIMBLEFIX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(NIMBLEFIX_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${NIMBLEFIX_HELLO_FIX_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/nimblefix")
endif()

function(nimblefix_add_hello_fix_example target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE nimblefix)
  target_include_directories(${target}
    PRIVATE
      "${NIMBLEFIX_HELLO_FIX_PUBLIC_INCLUDE}"
      "${NIMBLEFIX_HELLO_FIX_GENERATED_INCLUDE}")
  if(TARGET nimblefix-fix44-assets)
    add_dependencies(${target} nimblefix-fix44-assets)
  endif()
endfunction()

nimblefix_add_hello_fix_example(hello-fix-initiator initiator.cpp)
nimblefix_add_hello_fix_example(hello-fix-acceptor acceptor.cpp)
nimblefix_add_hello_fix_example(hello-fix-queue-handler queue_handler.cpp)
