cmake_minimum_required(VERSION 3.14)
project(CoreDB)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(CoreDB src/main.cpp)

# ---- Testing: fetch GTest from source, build it ourselves ----
# (Avoids relying on any pre-installed GTest, which can be the wrong
#  platform/compiler version, as we discovered when CMake found a
#  Windows-built copy via an Anaconda install mounted into WSL.)
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(CoreDBTests tests/test_main.cpp)
target_link_libraries(CoreDBTests GTest::gtest_main pthread)

include(GoogleTest)
gtest_discover_tests(CoreDBTests)