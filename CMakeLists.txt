cmake_minimum_required(VERSION 3.16)
project(superopt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Threading support
find_package(Threads REQUIRED)

# Find LLVM using llvm-config
find_program(LLVM_CONFIG
    NAMES llvm-config llvm-config-18 llvm-config-17 llvm-config-16 llvm-config-15
    PATHS /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin /usr/bin
    DOC "Path to llvm-config"
)

if(NOT LLVM_CONFIG)
    message(FATAL_ERROR "llvm-config not found! Please install LLVM development package.")
endif()

message(STATUS "Found llvm-config: ${LLVM_CONFIG}")

# Get LLVM version
execute_process(
    COMMAND ${LLVM_CONFIG} --version
    OUTPUT_VARIABLE LLVM_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "LLVM version: ${LLVM_VERSION}")

# Get LLVM include directories
execute_process(
    COMMAND ${LLVM_CONFIG} --includedir
    OUTPUT_VARIABLE LLVM_INCLUDE_DIRS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get LLVM library directories
execute_process(
    COMMAND ${LLVM_CONFIG} --libdir
    OUTPUT_VARIABLE LLVM_LIBRARY_DIRS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get LLVM C++ flags
execute_process(
    COMMAND ${LLVM_CONFIG} --cxxflags
    OUTPUT_VARIABLE LLVM_CXXFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get LLVM LD flags
execute_process(
    COMMAND ${LLVM_CONFIG} --ldflags
    OUTPUT_VARIABLE LLVM_LDFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get LLVM libraries - try shared first, fallback to component libs
execute_process(
    COMMAND ${LLVM_CONFIG} --link-shared --libs
    OUTPUT_VARIABLE LLVM_LIBS_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE LLVM_SHARED_RESULT
)

# If shared linking failed, try component libs
if(NOT LLVM_SHARED_RESULT EQUAL 0)
    execute_process(
        COMMAND ${LLVM_CONFIG} --libs core support irreader bitreader bitwriter
                analysis ipo passes transformutils scalaropts instcombine
                aggressiveinstcombine vectorize executionengine interpreter
                mcjit orcjit native
        OUTPUT_VARIABLE LLVM_LIBS_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

# Get system libs
execute_process(
    COMMAND ${LLVM_CONFIG} --system-libs
    OUTPUT_VARIABLE LLVM_SYSTEM_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Parse the libraries into a list
string(REPLACE " " ";" LLVM_LIBS "${LLVM_LIBS_OUTPUT}")
string(REPLACE " " ";" LLVM_SYSTEM_LIBS_LIST "${LLVM_SYSTEM_LIBS}")

message(STATUS "LLVM include dirs: ${LLVM_INCLUDE_DIRS}")
message(STATUS "LLVM library dirs: ${LLVM_LIBRARY_DIRS}")

# Optional Z3 support
option(SUPEROPT_USE_Z3 "Enable Z3 SMT solver for formal verification" OFF)
if(SUPEROPT_USE_Z3)
    find_package(Z3 REQUIRED)
    message(STATUS "Found Z3: ${Z3_LIBRARIES}")
endif()

# Add LLVM flags to compile options
separate_arguments(LLVM_CXXFLAGS_LIST NATIVE_COMMAND "${LLVM_CXXFLAGS}")
# Remove -std flags from LLVM as we set our own
list(FILTER LLVM_CXXFLAGS_LIST EXCLUDE REGEX "-std=")
# Remove -W flags to avoid conflicts
list(FILTER LLVM_CXXFLAGS_LIST EXCLUDE REGEX "^-W")

# Superoptimizer library
add_library(superopt_lib STATIC
    src/IRLoader.cpp
    src/CostModel.cpp
    src/Enumerator.cpp
    src/Verifier.cpp
    src/Superoptimizer.cpp
    src/Canonicalizer.cpp
    src/Pruning.cpp
    src/Cache.cpp
)

target_include_directories(superopt_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
)

target_compile_options(superopt_lib PUBLIC ${LLVM_CXXFLAGS_LIST})

target_link_directories(superopt_lib PUBLIC ${LLVM_LIBRARY_DIRS})

target_link_libraries(superopt_lib PUBLIC
    ${LLVM_LIBS}
    ${LLVM_SYSTEM_LIBS_LIST}
    Threads::Threads
)

if(SUPEROPT_USE_Z3)
    target_compile_definitions(superopt_lib PUBLIC SUPEROPT_USE_Z3)
    target_link_libraries(superopt_lib PUBLIC ${Z3_LIBRARIES})
    target_include_directories(superopt_lib PUBLIC ${Z3_INCLUDE_DIRS})
endif()

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
