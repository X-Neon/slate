#include "catch.hpp"
#include "slate.hpp"

struct nested
{
    double b;
    std::string c;

    bool operator==(const nested&) const = default;
};

struct base
{
    int a;
    nested n;
    std::vector<int> d;

    bool operator==(const base&) const = default;
};

TEST_CASE("aggregate") {
    slate::db db(":memory:");
    db.execute("CREATE TABLE test (a INT, b REAL, c TEXT, d BLOB)");

    base val = {5, {2.0, "test_string"}, {1, 2, 3, 4}};
    db.execute("INSERT INTO test (a, b, c, d) VALUES (?, ?, ?, ?)", val);

    auto result = db.execute("SELECT a, b, c, d from test").fetch_single_value<base>();
    REQUIRE(val == result);

    auto cursor = db.execute("SELECT a, b, c, d from test").fetch_value<base>();
    REQUIRE(val == *cursor.begin());
}