#include "catch.hpp"
#include "slate.hpp"

TEST_CASE("iterate") {
    using data = std::tuple<std::optional<int>, double>;

    slate::db db(":memory:");
    db.execute("CREATE TABLE test (a INT, b REAL)");

    data v1 = {std::nullopt, 1.1};
    data v2 = {1, 2.2};
    data v3 = {3, 3.3};
    data v4 = {std::nullopt, 4.4};
    data v5 = {5, 5.5};
    data v6 = {6, 6.6};

    std::vector<data> in = {v1, v2, v3, v4, v5, v6};
    for (auto& [a, b] : in) {
        db.execute("INSERT INTO test (a, b) VALUES (?, ?)", a, b);
    }

    std::vector<data> out;
    std::ranges::copy(db.execute("SELECT a, b FROM test").fetch<std::optional<int>, double>(), std::back_inserter(out));
    REQUIRE(in == out);
}