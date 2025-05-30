# define the test executable
add_executable(
  logging_tests ${CMAKE_SOURCE_DIR}/src/logging/log_puppy.cpp
                ${CMAKE_SOURCE_DIR}/src/logging/log.cpp tests.cpp utils.cpp
  )
target_compile_features(logging_tests PRIVATE cxx_std_17)
target_link_libraries(logging_tests freertos_tests)

# define required search paths
target_compile_definitions(logging_tests PUBLIC LOG_LOWEST_SEVERITY=1)
target_include_directories(
  logging_tests PUBLIC . ${CMAKE_SOURCE_DIR}/src/logging ${CMAKE_SOURCE_DIR}/include
                       ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/logging/include
  )

# tell build system about the test case
add_catch_test(logging_tests)

# define the test executable
add_executable(
  logging_tests_compile_time_filter
  ${CMAKE_SOURCE_DIR}/src/logging/log_puppy.cpp ${CMAKE_SOURCE_DIR}/src/logging/log.cpp
  tests_compile_time_filter.cpp utils.cpp
  )

target_compile_definitions(logging_tests_compile_time_filter PUBLIC LOG_LOWEST_SEVERITY=3)
target_compile_features(logging_tests_compile_time_filter PRIVATE cxx_std_17)
target_link_libraries(logging_tests_compile_time_filter freertos_tests)

# define required search paths
target_include_directories(
  logging_tests_compile_time_filter
  PUBLIC . ${CMAKE_SOURCE_DIR}/src/logging ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
         ${CMAKE_SOURCE_DIR}/src/logging/include
  )

# tell build system about the test case
add_catch_test(logging_tests_compile_time_filter)
