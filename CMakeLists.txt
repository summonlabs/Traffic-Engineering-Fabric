cmake_minimum_required(VERSION 3.20)

project(TrafficEngineeringFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral global traffic-engineering intent runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Traffic-Engineering-Fabric"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TEF_BUILD_TESTS "Build the Traffic Engineering Fabric test suite" ON)
option(TEF_BUILD_BENCHMARKS "Build the synthetic benchmark harness" ON)
option(TEF_BUILD_TOOLS "Build the coordinator, publisher and inspection tools" ON)
option(TEF_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(TEF_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers when the toolchain supports them" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(Threads REQUIRED)

add_library(tef
  src/diagnostic.cpp
  src/identity.cpp
  src/digest.cpp
  src/model.cpp
  src/authority.cpp
  src/feasibility.cpp
  src/derived.cpp
  src/allocation.cpp
  src/solver.cpp
  src/plan.cpp
  src/explain.cpp
  src/engine.cpp
  src/persist.cpp
  src/net.cpp
  src/protocol.cpp
  src/coordinator.cpp
  src/publisher.cpp
  src/inspect.cpp
)
add_library(TrafficEngineeringFabric::tef ALIAS tef)

target_include_directories(tef
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(tef PUBLIC cxx_std_20)
target_link_libraries(tef PUBLIC Threads::Threads)

if(WIN32)
  target_compile_definitions(tef PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601
  )
  target_link_libraries(tef PUBLIC ws2_32)
endif()

set(TEF_PRIVATE_WARNINGS "")
if(MSVC)
  list(APPEND TEF_PRIVATE_WARNINGS /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)
  if(TEF_WARNINGS_AS_ERRORS)
    list(APPEND TEF_PRIVATE_WARNINGS /WX)
  endif()
else()
  list(APPEND TEF_PRIVATE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(TEF_WARNINGS_AS_ERRORS)
    list(APPEND TEF_PRIVATE_WARNINGS -Werror)
  endif()
endif()

target_compile_options(tef PRIVATE ${TEF_PRIVATE_WARNINGS})

if(TEF_ENABLE_SANITIZERS AND NOT MSVC)
  target_compile_options(tef PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(tef PUBLIC -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS tef
  EXPORT TrafficEngineeringFabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT TrafficEngineeringFabricTargets
  FILE TrafficEngineeringFabricTargets.cmake
  NAMESPACE TrafficEngineeringFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TrafficEngineeringFabric
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TrafficEngineeringFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/TrafficEngineeringFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TrafficEngineeringFabric
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/TrafficEngineeringFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/TrafficEngineeringFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/TrafficEngineeringFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TrafficEngineeringFabric
)

install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------
# Tests, benchmarks, tools, examples
# ---------------------------------------------------------------------------
if(TEF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TEF_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(TEF_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(TEF_BUILD_TESTS)
  add_subdirectory(examples)
endif()
