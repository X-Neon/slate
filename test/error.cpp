#include "catch.hpp"
#include "slate.hpp"

struct too_few { int a; };
struct too_many { int a, b, c; };

TEST_CASE("error") {
    slate::db db(":memory:");
    db.execute("CREATE TABLE test (a INT, b INT)");
    db.execute("INSERT INTO test (a, b) VALUES (?, ?)", 10, 20);
    auto result = db.execute("SELECT a, b FROM test");

    SECTION("extracting fewer fields than in the row throws an exception") {
        REQUIRE_THROWS_AS(result.fetch_single<int>(), std::out_of_range);
    }

    SECTION("extracting more fields than in the row throws an exception") {
        REQUIRE_THROWS_AS((result.fetch_single<int, int, int>()), std::out_of_range);
    }

    SECTION("extracting fewer fields than in the row (via an aggregate) throws an exception") {
        REQUIRE_THROWS_AS(result.fetch_single<too_few>(), std::out_of_range);
    }

    SECTION("extracting more fields than in the row (via an aggregate) throws an exception") {
        REQUIRE_THROWS_AS(result.fetch_single<too_many>(), std::out_of_range);
    }

    SECTION("attempting to fetch rows multiple times throws an exception") {
        result.fetch<int, int>();
        REQUIRE_THROWS_AS((result.fetch<int, int>()), std::runtime_error);
    }
}