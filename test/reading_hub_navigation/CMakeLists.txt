add_executable(ReadingHubNavigationTest ReadingHubNavigationTest.cpp)

target_include_directories(ReadingHubNavigationTest PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/src/activities/home)
target_link_libraries(ReadingHubNavigationTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(ReadingHubNavigationTest)
