#include "catch.hpp"
#include "slate.hpp"

class my_int
{
public:
    my_int(int val) : m_val(val) {}
    const int& get() const { return m_val; }
    bool operator==(const my_int&) const = default;

private:
    int m_val;
};

namespace slate {
    template <>
    struct serializer<my_int>
    {
        static int to_sql(const my_int& v) {
            return v.get();
        }

        static my_int from_sql(int v) {
            return v;
        }
    };
}

TEST_CASE("serializer") {
    slate::db db(":memory:");
    db.execute("CREATE TABLE test (a INT)");

    my_int in(5);
    db.execute("INSERT INTO test (a) VALUES (?)", in);
    auto out = db.execute("SELECT a FROM test").fetch_single_value<my_int>();
    REQUIRE(in == out);
}