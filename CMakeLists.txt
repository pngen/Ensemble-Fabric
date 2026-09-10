cmake_minimum_required(VERSION 3.20)

project(ensemble_fabric
  VERSION 1.0.0
  DESCRIPTION "Ensemble identity, participant authority, quorum, consensus, arbitration, and exactly-once logical result commit across multiple model executions"
  HOMEPAGE_URL "https://github.com/pngen/Ensemble-Fabric"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(ENSEMBLE_FABRIC_IS_TOP_LEVEL ON)
else()
  set(ENSEMBLE_FABRIC_IS_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(ENSEMBLE_FABRIC_BUILD_TESTS "Build the Ensemble Fabric test suite" ${ENSEMBLE_FABRIC_IS_TOP_LEVEL})
option(ENSEMBLE_FABRIC_BUILD_EXAMPLES "Build the Ensemble Fabric examples" ${ENSEMBLE_FABRIC_IS_TOP_LEVEL})
option(ENSEMBLE_FABRIC_BUILD_TOOLS "Build the Ensemble Fabric tools and reference deployment" ${ENSEMBLE_FABRIC_IS_TOP_LEVEL})
option(ENSEMBLE_FABRIC_BUILD_BENCHMARKS "Build the Ensemble Fabric benchmarks" ${ENSEMBLE_FABRIC_IS_TOP_LEVEL})
option(ENSEMBLE_FABRIC_BUILD_CUDA_PROOF "Build the optional real CUDA participant-authority proof" ON)
option(ENSEMBLE_FABRIC_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ENSEMBLE_FABRIC_ENABLE_SHARED "Build Ensemble Fabric as a shared library" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# The runtime is built from ordinary translation units and does not use C++20
# modules, so module scanning is disabled explicitly.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_CUDA_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(ENSEMBLE_FABRIC_ENABLE_SHARED)
  set(ENSEMBLE_FABRIC_LIBRARY_TYPE SHARED)
else()
  set(ENSEMBLE_FABRIC_LIBRARY_TYPE STATIC)
endif()

# ---------------------------------------------------------------------------
# Warnings: strict and never suppressed.  Restricted to C++ compiles so that
# nvcc never receives host-only flags.
# ---------------------------------------------------------------------------
add_library(ensemble_fabric_warnings INTERFACE)
if(MSVC)
  target_compile_options(ensemble_fabric_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/permissive-;/utf-8;/EHsc;/Zc:__cplusplus;/Zc:preprocessor>)
else()
  target_compile_options(ensemble_fabric_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror;-Wshadow;-Wconversion;-Wsign-conversion;-Wold-style-cast;-Wnon-virtual-dtor;-Woverloaded-virtual;-Wdouble-promotion;-Wcast-qual>)
endif()

if(ENSEMBLE_FABRIC_ENABLE_ASAN)
  if(MSVC)
    # Debug information is emitted so that sanitizer reports are symbolised.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/fsanitize=address;/Zi>")
    add_link_options(/fsanitize=address /DEBUG)
  else()
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fsanitize=address;-fno-omit-frame-pointer>)
    add_link_options(-fsanitize=address)
  endif()
endif()

if(NOT WIN32)
  find_package(Threads REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
set(ENSEMBLE_FABRIC_CORE_SOURCES
  src/arbitration.cpp
  src/authority.cpp
  src/candidate.cpp
  src/consensus.cpp
  src/evaluation.cpp
  src/fabric.cpp
  src/fabric_commit.cpp
  src/fabric_execution.cpp
  src/fabric_ingest.cpp
  src/fabric_inspect.cpp
  src/fabric_participant.cpp
  src/fabric_persist.cpp
  src/limits.cpp
  src/local_driver.cpp
  src/outcome.cpp
  src/participant.cpp
  src/persistence.cpp
  src/protocol.cpp
  src/quorum.cpp
  src/reference_backend.cpp
  src/result.cpp
  src/role.cpp
  src/spec.cpp
  src/version.cpp
  src/transport.cpp
  src/distributed/client.cpp
  src/distributed/coordinator.cpp
  src/distributed/worker.cpp
  src/detail/crc32c.cpp
  src/detail/sha256.cpp
  src/detail/text.cpp)

set(ENSEMBLE_FABRIC_PUBLIC_HEADERS
  include/ensemble_fabric/arbitration.hpp
  include/ensemble_fabric/authority.hpp
  include/ensemble_fabric/candidate.hpp
  include/ensemble_fabric/client.hpp
  include/ensemble_fabric/consensus.hpp
  include/ensemble_fabric/coordinator.hpp
  include/ensemble_fabric/ensemble_fabric.hpp
  include/ensemble_fabric/evaluation.hpp
  include/ensemble_fabric/explanation.hpp
  include/ensemble_fabric/fabric.hpp
  include/ensemble_fabric/ids.hpp
  include/ensemble_fabric/limits.hpp
  include/ensemble_fabric/local_driver.hpp
  include/ensemble_fabric/outcome.hpp
  include/ensemble_fabric/participant.hpp
  include/ensemble_fabric/persistence.hpp
  include/ensemble_fabric/protocol.hpp
  include/ensemble_fabric/quorum.hpp
  include/ensemble_fabric/reference_backend.hpp
  include/ensemble_fabric/result.hpp
  include/ensemble_fabric/role.hpp
  include/ensemble_fabric/spec.hpp
  include/ensemble_fabric/transport.hpp
  include/ensemble_fabric/version.hpp
  include/ensemble_fabric/worker.hpp)

add_library(ensemble_fabric ${ENSEMBLE_FABRIC_LIBRARY_TYPE} ${ENSEMBLE_FABRIC_CORE_SOURCES})
add_library(ensemble_fabric::ensemble_fabric ALIAS ensemble_fabric)
target_include_directories(ensemble_fabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(ensemble_fabric PUBLIC cxx_std_20)
target_link_libraries(ensemble_fabric PRIVATE $<BUILD_INTERFACE:ensemble_fabric_warnings>)
if(WIN32)
  target_link_libraries(ensemble_fabric PUBLIC ws2_32)
else()
  target_link_libraries(ensemble_fabric PUBLIC Threads::Threads)
endif()
set_target_properties(ensemble_fabric PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME ensemble_fabric)

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
install(TARGETS ensemble_fabric
  EXPORT ensemble_fabric_targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ensemble_fabric_targets
  FILE ensemble_fabric_targets.cmake
  NAMESPACE ensemble_fabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ensemble_fabric)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ensemble_fabric_config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ensemble_fabric-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ensemble_fabric)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ensemble_fabric-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ensemble_fabric-config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ensemble_fabric-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ensemble_fabric)

# ---------------------------------------------------------------------------
# Tools and the distributed reference deployment
# ---------------------------------------------------------------------------
if(ENSEMBLE_FABRIC_BUILD_TOOLS)
  add_executable(ensemble_coordinator tools/ensemble_coordinator_main.cpp)
  target_link_libraries(ensemble_coordinator PRIVATE ensemble_fabric)
  target_link_libraries(ensemble_coordinator PRIVATE ensemble_fabric_warnings)

  add_executable(ensemble_worker tools/ensemble_worker_main.cpp)
  target_link_libraries(ensemble_worker PRIVATE ensemble_fabric)
  target_link_libraries(ensemble_worker PRIVATE ensemble_fabric_warnings)

  add_executable(ensemble_client tools/ensemble_client_main.cpp)
  target_link_libraries(ensemble_client PRIVATE ensemble_fabric)
  target_link_libraries(ensemble_client PRIVATE ensemble_fabric_warnings)

  add_executable(ensemble_inspect tools/ensemble_inspect_main.cpp)
  target_link_libraries(ensemble_inspect PRIVATE ensemble_fabric)
  target_link_libraries(ensemble_inspect PRIVATE ensemble_fabric_warnings)

  install(TARGETS ensemble_coordinator ensemble_worker ensemble_client ensemble_inspect
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(ENSEMBLE_FABRIC_BUILD_EXAMPLES)
  set(ENSEMBLE_FABRIC_EXAMPLES
    example_parallel_best_of_n
    example_candidate_judge
    example_weighted_quorum
    example_specialist_judge
    example_verifier_gated
    example_fallback
    example_disagreement
    example_cancellation
    example_retry_replacement)
  foreach(example ${ENSEMBLE_FABRIC_EXAMPLES})
    add_executable(${example} examples/${example}.cpp)
    target_link_libraries(${example} PRIVATE ensemble_fabric ensemble_fabric_warnings)
    target_include_directories(${example} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
  endforeach()
  add_executable(example_multiprocess examples/example_multiprocess.cpp)
  target_link_libraries(example_multiprocess PRIVATE ensemble_fabric ensemble_fabric_warnings)
  target_include_directories(example_multiprocess PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(ENSEMBLE_FABRIC_BUILD_BENCHMARKS)
  add_executable(ensemble_benchmarks benchmarks/bench_ensemble.cpp)
  target_link_libraries(ensemble_benchmarks PRIVATE ensemble_fabric ensemble_fabric_warnings)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ENSEMBLE_FABRIC_BUILD_TESTS)
  enable_testing()
  set(ENSEMBLE_FABRIC_TESTS
    test_ids
    test_limits
    test_spec
    test_candidate_lifecycle
    test_participant_authority
    test_quorum
    test_consensus
    test_arbitration
    test_exactly_once_commit
    test_cancellation
    test_fallback
    test_retry
    test_persistence
    test_protocol
    test_transport
    test_property
    test_deterministic_races
    test_adversarial
    test_recovery_epoch
    test_reference_backend
    test_integration)
  foreach(test ${ENSEMBLE_FABRIC_TESTS})
    add_executable(${test} tests/${test}.cpp)
    target_link_libraries(${test} PRIVATE ensemble_fabric ensemble_fabric_warnings)
    target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME ${test} COMMAND ${test})
  endforeach()

  add_executable(test_multiprocess tests/test_multiprocess.cpp)
  target_link_libraries(test_multiprocess PRIVATE ensemble_fabric ensemble_fabric_warnings)
  add_test(NAME test_multiprocess COMMAND test_multiprocess
    $<TARGET_FILE:ensemble_coordinator>
    $<TARGET_FILE:ensemble_worker>
    $<TARGET_FILE:ensemble_client>)

endif()

# ---------------------------------------------------------------------------
# Optional CUDA authority-gating proof (real accelerator work, gated by the
# ensemble commit authority).  The core library never depends on CUDA.
# ---------------------------------------------------------------------------
# The CUDA proof is optional and additive: when no usable CUDA toolchain is
# available for this generator the rest of the project configures and builds
# exactly as before, and the accelerator proof is simply absent.
if(ENSEMBLE_FABRIC_BUILD_CUDA_PROOF)
  # The device translation unit is compiled by nvcc directly.  Depending on
  # CMake's CUDA language support would make the whole project fail to configure
  # on a generator whose CUDA integration is absent, and the core library must
  # never depend on CUDA being present at all.
  find_program(ENSEMBLE_FABRIC_NVCC NAMES nvcc nvcc.exe)
  if(ENSEMBLE_FABRIC_NVCC)
    get_filename_component(ENSEMBLE_FABRIC_CUDA_ROOT "${ENSEMBLE_FABRIC_NVCC}" DIRECTORY)
    get_filename_component(ENSEMBLE_FABRIC_CUDA_ROOT "${ENSEMBLE_FABRIC_CUDA_ROOT}" DIRECTORY)
  endif()
  find_library(ENSEMBLE_FABRIC_CUDART
    NAMES cudart
    HINTS
      "${ENSEMBLE_FABRIC_CUDA_ROOT}/lib/x64"
      "${ENSEMBLE_FABRIC_CUDA_ROOT}/lib64"
      "${ENSEMBLE_FABRIC_CUDA_ROOT}/lib")
  if(ENSEMBLE_FABRIC_NVCC AND ENSEMBLE_FABRIC_CUDART)
    add_executable(ensemble_cuda_proof cuda/cuda_proof_main.cpp)
    target_link_libraries(ensemble_cuda_proof PRIVATE
      ensemble_fabric
      ensemble_fabric_warnings
      "${ENSEMBLE_FABRIC_CUDART}")
    set(ENSEMBLE_FABRIC_CUDA_OBJECT
      "${CMAKE_CURRENT_BINARY_DIR}/cuda_proof${CMAKE_CXX_OUTPUT_EXTENSION}")
    # The device object must use the same C runtime as the rest of the build.
    if(MSVC)
      # Exactly one flag is produced, so nvcc never receives an empty argument.
      set(ENSEMBLE_FABRIC_CUDA_RUNTIME_FLAGS
        "$<$<CONFIG:Debug>:-Xcompiler=/MDd>$<$<NOT:$<CONFIG:Debug>>:-Xcompiler=/MD>")
    else()
      set(ENSEMBLE_FABRIC_CUDA_RUNTIME_FLAGS "")
    endif()
    add_custom_command(
      OUTPUT "${ENSEMBLE_FABRIC_CUDA_OBJECT}"
      COMMAND "${ENSEMBLE_FABRIC_NVCC}" -c "${CMAKE_CURRENT_SOURCE_DIR}/cuda/cuda_proof.cu"
              -o "${ENSEMBLE_FABRIC_CUDA_OBJECT}" -O2 -std=c++17
              -arch=native -Wno-deprecated-gpu-targets
              ${ENSEMBLE_FABRIC_CUDA_RUNTIME_FLAGS}
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cuda/cuda_proof.cu"
      COMMENT "Compiling the CUDA authority-gating proof"
      VERBATIM)
    add_custom_target(ensemble_cuda_proof_device DEPENDS "${ENSEMBLE_FABRIC_CUDA_OBJECT}")
    add_dependencies(ensemble_cuda_proof ensemble_cuda_proof_device)
    target_sources(ensemble_cuda_proof PRIVATE "${ENSEMBLE_FABRIC_CUDA_OBJECT}")
    if(ENSEMBLE_FABRIC_BUILD_TESTS)
      add_test(NAME test_cuda_proof COMMAND ensemble_cuda_proof)
    endif()
  else()
    message(STATUS "Ensemble Fabric: no usable CUDA toolchain found; the accelerator proof is not built.")
  endif()
endif()