add_executable(LibraryPerformanceTest LibraryPerformanceTest.cpp)

target_link_libraries(LibraryPerformanceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryPerformanceTest)
