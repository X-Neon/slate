#include "catch.hpp"
#include "slate.hpp"

#include <numeric>

slate::any_return double_val(slate::any v) {
    if (v.index() == 0) {
        return 2 * std::get<0>(v);
    } else if (v.index() == 1) {
        return 2 * std::get<1>(v);
    } else if (v.index() == 2) {
        std::string s(std::get<2>(v));
        return s + s;
    } else if (v.index() == 3) {
        std::vector<std::byte> vec;
        std::ranges::copy(std::get<3>(v), std::back_inserter(vec));
        std::ranges::copy(std::get<3>(v), std::back_inserter(vec));
        return vec;
    } else {
        return std::nullopt;
    }
}

double new_sum(std::vector<double> v) {
    return std::accumulate(v.begin(), v.end(), 0.0);
}

TEST_CASE("function") {
    slate::db db(":memory:");

    SECTION("test double_val") {
        constexpr auto query = "SELECT double_val(?)";
        db.declare("double_val", slate::function::pure, double_val);
        REQUIRE(db.execute(query, 2).fetch_single_value<int>() == 4);
        REQUIRE(db.execute(query, 0.5).fetch_single_value<double>() == 1.0);
        REQUIRE(db.execute(query, "str").fetch_single_value<std::string>() == "strstr");
        REQUIRE(db.execute(query, std::vector<int>{1,2}).fetch_single_value<std::vector<int>>() == std::vector<int>{1,2,1,2});
        REQUIRE(db.execute(query, std::nullopt).fetch_single_value<std::optional<int>>() == std::nullopt);
    }

    SECTION("test new_sum") {
        db.declare("new_sum", slate::function::pure, new_sum);
        REQUIRE(db.execute("SELECT new_sum(?, ?, ?, ?)", 1, 2, 3, 4).fetch_single_value<double>() == 10);
    }
}