#include "catch.hpp"
#include "slate.hpp"

TEST_CASE("tuple") {
    slate::db db(":memory:");
    db.execute("CREATE TABLE test (a INT, b REAL, c TEXT, d BLOB)");

    int a = 5;
    double b = 2.0;
    std::string c = "test_string";
    std::vector<int> d = {1, 2, 3, 4};

    db.execute("INSERT INTO test (a, b, c, d) VALUES (?, ?, ?, ?)", a, b, c, d);

    auto result = db.execute("SELECT a, b, c, d from test").fetch_single<int, double, std::string, std::vector<int>>();
    REQUIRE(a == std::get<0>(result));
    REQUIRE(b == std::get<1>(result));
    REQUIRE(c == std::get<2>(result));
    REQUIRE(d == std::get<3>(result));

    auto cursor = db.execute("SELECT a, b, c, d from test").fetch<int, double, std::string, std::vector<int>>();
    REQUIRE(result == *cursor.begin());
}