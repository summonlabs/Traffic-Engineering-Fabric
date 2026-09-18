add_executable(tef-node tef_node.cpp)
target_link_libraries(tef-node PRIVATE TrafficEngineeringFabric::tef)
target_compile_options(tef-node PRIVATE ${TEF_PRIVATE_WARNINGS})

add_executable(tef-cli tef_cli.cpp)
target_link_libraries(tef-cli PRIVATE TrafficEngineeringFabric::tef)
target_compile_options(tef-cli PRIVATE ${TEF_PRIVATE_WARNINGS})

install(TARGETS tef-node tef-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
