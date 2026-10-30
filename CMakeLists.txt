cmake_minimum_required(VERSION 3.18)
project(MicroPulse LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)

# The core is header-only.
add_library(micropulse INTERFACE)
target_include_directories(micropulse INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(micropulse INTERFACE Threads::Threads)

add_executable(test_main tests/test_main.cpp)
target_link_libraries(test_main PRIVATE micropulse)
enable_testing()
add_test(NAME unit COMMAND test_main)

add_executable(bench_main bench/bench_main.cpp)
target_link_libraries(bench_main PRIVATE micropulse)

# Python bindings: cmake -DMICROPULSE_PYTHON=ON -Dpybind11_DIR=$(python3 -m pybind11 --cmakedir) ..
option(MICROPULSE_PYTHON "Build pybind11 module" OFF)
if(MICROPULSE_PYTHON)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(_micropulse bindings/pybind_module.cpp)
  target_link_libraries(_micropulse PRIVATE micropulse)
  set_target_properties(_micropulse PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/python/micropulse_research)
endif()
