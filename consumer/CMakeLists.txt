cmake_minimum_required(VERSION 3.20)
project(TrafficEngineeringFabricConsumer LANGUAGES CXX)

# An independent downstream consumer. It deliberately does NOT add the library
# source tree: it only locates an installed Traffic Engineering Fabric package.
find_package(TrafficEngineeringFabric 1.0 REQUIRED)

add_executable(tef_consumer main.cpp)
target_link_libraries(tef_consumer PRIVATE TrafficEngineeringFabric::tef)
target_compile_features(tef_consumer PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(tef_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
else()
  target_compile_options(tef_consumer PRIVATE -Wall -Wextra -Werror)
endif()
