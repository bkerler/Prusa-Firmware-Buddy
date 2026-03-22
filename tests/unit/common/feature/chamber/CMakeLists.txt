add_executable(chamber_marlin_compat_tests chamber_marlin_compat_tests.cpp)
target_include_directories(
  chamber_marlin_compat_tests
  PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/common
          ${CMAKE_SOURCE_DIR}/src/freertos/include ${CMAKE_SOURCE_DIR}/src
  )
add_catch_test(chamber_marlin_compat_tests)
