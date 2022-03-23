#include "catch.hpp"
#include "slate.hpp"

TEST_CASE("optional") {
    slate::db db(":memory:");
    db.execute("CREATE TABLE test (a INT)");

    SECTION("retrieving int as optional<int> preserves value") {
        db.execute("INSERT INTO test (a) VALUES (?)", 10);
        REQUIRE(db.execute("SELECT a from test").fetch_single_value<std::optional<int>>() == 10);
    }

    SECTION("retrieving null as optional<int> produces no value") {
        db.execute("INSERT INTO test (a) VALUES (?)", std::nullopt);
        REQUIRE(db.execute("SELECT a from test").fetch_single_value<std::optional<int>>() == std::nullopt);
    }

    SECTION("filled optional<int> round-trip preserves value") {
        db.execute("INSERT INTO test (a) VALUES (?)", std::optional<int>(10));
        REQUIRE(db.execute("SELECT a from test").fetch_single_value<std::optional<int>>() == 10);
    }

    SECTION("empty optional<int> round-trip preserves value") {
        db.execute("INSERT INTO test (a) VALUES (?)", std::optional<int>{});
        REQUIRE(db.execute("SELECT a from test").fetch_single_value<std::optional<int>>() == std::nullopt);
    }
}