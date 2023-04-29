cmake_minimum_required(VERSION 3.0)
project(Slate)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
find_package(Boost REQUIRED)

add_executable(
    slate_test 
    test/aggregate.cpp 
    test/convert.cpp 
    test/error.cpp 
    test/function.cpp 
    test/iterate.cpp
    test/optional.cpp
    test/serializer.cpp
    test/transaction.cpp
    test/tuple.cpp
)

target_include_directories(slate_test PRIVATE include)
target_link_libraries(slate_test PRIVATE unofficial::sqlite3::sqlite3 Catch2::Catch2WithMain Boost::boost)
target_compile_features(slate_test PRIVATE cxx_std_20)

enable_testing()
add_test(NAME slate_test COMMAND slate_test)