add_executable(tef_bench tef_bench.cpp)
target_link_libraries(tef_bench PRIVATE TrafficEngineeringFabric::tef)
target_compile_options(tef_bench PRIVATE ${TEF_PRIVATE_WARNINGS})
