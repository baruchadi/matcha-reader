add_executable(ReadingQueueTest ReadingQueueTest.cpp)

target_include_directories(ReadingQueueTest PRIVATE ${REPO_ROOT}/src)
target_link_libraries(ReadingQueueTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(ReadingQueueTest)
