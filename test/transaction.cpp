#include "catch.hpp"
#include "slate.hpp"

TEST_CASE("transaction") {
    slate::db db(":memory:");
    db.execute("CREATE TABLE test (a INT)");

    SECTION("scoped_transaction destruction with active exceptions causes rollback") {
        try {
            slate::scoped_transaction transaction(db);
            db.execute("INSERT INTO test (a) VALUES (?)", 10);
            throw std::exception{};
        } catch (std::exception& e) {}
        REQUIRE(db.execute("SELECT COUNT(*) FROM test").fetch_single_value<int>() == 0);
    }

    SECTION("scoped_transaction destruction without active exceptions causes commit") {
        {
            slate::scoped_transaction transaction(db);
            db.execute("INSERT INTO test (a) VALUES (?)", 10);
        }
        REQUIRE(db.execute("SELECT COUNT(*) FROM test").fetch_single_value<int>() == 1);
    }
    
    SECTION("transaction rollback means no rows are inserted") {
        db.begin_transaction();
        db.execute("INSERT INTO test (a) VALUES (?)", 10);
        db.rollback();
        REQUIRE(db.execute("SELECT COUNT(*) FROM test").fetch_single_value<int>() == 0);
    }
    
    SECTION("transaction commit means rows are inserted") {
        db.begin_transaction();
        db.execute("INSERT INTO test (a) VALUES (?)", 10);
        db.commit();
        REQUIRE(db.execute("SELECT COUNT(*) FROM test").fetch_single_value<int>() == 1);
    }    
}