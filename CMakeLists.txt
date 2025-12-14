cmake_minimum_required(VERSION 3.16)
project(superopt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# LLVM definitions and includes
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/include)

# Get LLVM libraries
llvm_map_components_to_libnames(LLVM_LIBS
    core
    support
    irreader
    bitreader
    bitwriter
    analysis
    ipo
    passes
    transformutils
    scalaropts
    instcombine
    aggressiveinstcombine
    vectorize
    target
    executionengine
    interpreter
    mcjit
    orcjit
    native
    nativecodegen
)

# Superoptimizer library
add_library(superopt_lib STATIC
    src/IRLoader.cpp
    src/CostModel.cpp
    src/Enumerator.cpp
    src/Verifier.cpp
    src/Superoptimizer.cpp
    src/Canonicalizer.cpp
    src/Pruning.cpp
)

target_include_directories(superopt_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(superopt_lib PUBLIC ${LLVM_LIBS})

# Main executable
add_executable(superopt src/main.cpp)
target_link_libraries(superopt PRIVATE superopt_lib)

# Test executable
add_executable(superopt_test test/test_main.cpp)
target_link_libraries(superopt_test PRIVATE superopt_lib)

# Install targets
install(TARGETS superopt DESTINATION bin)
install(DIRECTORY include/superopt DESTINATION include)

# Optional: Enable testing
enable_testing()
add_test(NAME SuperoptTest COMMAND superopt_test)
