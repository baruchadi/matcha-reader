add_executable(LibraryBookInputTest LibraryBookInputTest.cpp)

target_link_libraries(LibraryBookInputTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryBookInputTest)
