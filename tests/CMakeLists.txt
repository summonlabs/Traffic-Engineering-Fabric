add_library(tef_test_support STATIC support/test_framework.cpp support/fixtures.cpp)
target_include_directories(tef_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tef_test_support PUBLIC TrafficEngineeringFabric::tef)
target_compile_options(tef_test_support PRIVATE ${TEF_PRIVATE_WARNINGS})

# Tests are proof obligations run plainly: no timeout is enforced, so a hang is
# diagnosed rather than hidden behind a watchdog termination.
function(tef_add_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE tef_test_support)
  target_compile_options(${target} PRIVATE ${TEF_PRIVATE_WARNINGS})
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES TIMEOUT 0)
endfunction()

tef_add_test(tef_core_tests test_core.cpp)
tef_add_test(tef_model_tests test_model.cpp)
tef_add_test(tef_solver_tests test_solver.cpp)
tef_add_test(tef_invariant_tests test_invariant_tests.cpp)
tef_add_test(tef_engine_tests test_engine_tests.cpp)
tef_add_test(tef_persistence_tests test_persistence_tests.cpp)
tef_add_test(tef_concurrency_tests test_concurrency_tests.cpp)
tef_add_test(tef_distributed_tests test_distributed_tests.cpp)
tef_add_test(tef_multiprocess_tests test_multiprocess_tests.cpp)

target_compile_definitions(tef_multiprocess_tests PRIVATE
  TEF_NODE_EXECUTABLE="$<TARGET_FILE:tef-node>"
)
add_dependencies(tef_multiprocess_tests tef-node)
