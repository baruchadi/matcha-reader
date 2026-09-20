add_executable(ReaderOpenPerformanceTest ReaderOpenPerformanceTest.cpp)

target_link_libraries(ReaderOpenPerformanceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderOpenPerformanceTest)
