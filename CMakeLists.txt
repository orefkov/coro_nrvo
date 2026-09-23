# CMakeList.txt : CMake project for core_as, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.20)

project(coro_nrvo)

include(FetchContent)

if(${CMAKE_CXX_COMPILER_ID} STREQUAL Clang)
    set(CLANG_COMPILER ON)
endif()

enable_testing()
# Load and build GTest
FetchContent_Declare(
	googletest
	# Specify the commit you depend on and update it regularly.
	URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
	FIND_PACKAGE_ARGS NAMES "GTest 1.17.0"
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt FALSE CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(test_jobs
    test_jobs.cpp
)

target_link_libraries(test_jobs gtest_main)
target_compile_features(test_jobs PUBLIC cxx_std_20)

add_test(NAME test_jobs COMMAND test_jobs)
