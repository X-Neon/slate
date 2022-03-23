#include "catch.hpp"
#include "slate.hpp"

TEST_CASE("convert") {
    slate::db db(":memory:");
    db.execute("CREATE TABLE test (a INT)");
    constexpr auto select = "SELECT a from test";

    SECTION("conversion from int to string succeeds for on and non_null, throws for null and off") {
        db.execute("INSERT INTO test (a) VALUES (?)", 10);
        REQUIRE(db.execute(select).fetch_single_value<std::string>(slate::convert::on) == "10");
        REQUIRE(db.execute(select).fetch_single_value<std::string>(slate::convert::non_null) == "10");
        REQUIRE_THROWS_AS(db.execute(select).fetch_single_value<std::string>(slate::convert::null), std::runtime_error);
        REQUIRE_THROWS_AS(db.execute(select).fetch_single_value<std::string>(slate::convert::off), std::runtime_error);
    }

    SECTION("conversion from null to int succeeds for on and null, throws for non_null and off") {
        db.execute("INSERT INTO test (a) VALUES (?)", std::nullopt);
        REQUIRE(db.execute(select).fetch_single_value<int>(slate::convert::on) == 0);
        REQUIRE(db.execute(select).fetch_single_value<int>(slate::convert::null) == 0);
        REQUIRE_THROWS_AS(db.execute(select).fetch_single_value<int>(slate::convert::non_null), std::runtime_error);
        REQUIRE_THROWS_AS(db.execute(select).fetch_single_value<int>(slate::convert::off), std::runtime_error);
    }
}