#pragma once

#include <sqlite3.h>
#include <string_view>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <string>

#if defined __has_include
#  if __has_include ("pfr.hpp")
#    include "pfr.hpp"
#  endif
#endif

#define SLATE_STATIC_FAIL(msg) []<bool _ = false>(){ static_assert(_, msg); }();

namespace slate {

class exception : public std::exception
{
public:
    exception(int code) : m_code(code) {}
    int code() const noexcept { return m_code; }
    const char* what() const noexcept override {
        return sqlite3_errstr(m_code);
    }

private:
    int m_code;
};

enum class convert
{
    off,
    null,
    non_null,
    on
};

enum class transaction
{
    deferred,
    immediate,
    exclusive
};

enum class open
{
    read_only = SQLITE_OPEN_READONLY,
    read_write = SQLITE_OPEN_READWRITE,
    create = SQLITE_OPEN_CREATE,
    uri = SQLITE_OPEN_URI,
    memory = SQLITE_OPEN_MEMORY,
    no_mutex = SQLITE_OPEN_NOMUTEX,
    full_mutex = SQLITE_OPEN_FULLMUTEX,
    shared_cache = SQLITE_OPEN_SHAREDCACHE,
    private_cache = SQLITE_OPEN_PRIVATECACHE,
    no_follow = SQLITE_OPEN_NOFOLLOW
};

constexpr open operator|(const open& a, const open& b) {
    return {a | b};
}

namespace detail {

    template <typename T>
    struct is_optional : public std::false_type {};

    template <typename T>
    struct is_optional<std::optional<T>> : public std::true_type { using type = T; };

#ifdef PFR_HPP
    template <std::size_t I>
    using size_constant = std::integral_constant<std::size_t, I>;

    template <typename T, typename I, typename M>
    constexpr int aggregate_size();

    template <typename T>
    constexpr int field_size() {
        if constexpr (std::is_aggregate_v<T>) {
            return aggregate_size<T, size_constant<0>, size_constant<pfr::tuple_size_v<T>>>();
        } else {
            return 1;
        }
    }

    template <typename T, typename I, typename M>
    constexpr int aggregate_size() {
        if constexpr(I() < M()) {
            return field_size<pfr::tuple_element_t<I()+0, T>>() + aggregate_size<T, size_constant<I()+1>, M>();
        } else {
            return 0;
        }
    }

    template <typename T, typename... Ts>
    constexpr int row_size() {
        if constexpr (sizeof...(Ts) == 0) {
            return field_size<T>();
        } else {
            return field_size<T>() + row_size<Ts...>();
        }
    }
#else
    template <typename... Ts>
    constexpr int row_size() {
        return sizeof...(Ts);
    }
#endif

    void check_convert(sqlite3_stmt* stmt, int index, int desired, convert c) {
        if (c == convert::on) {
            return;
        }

        int actual = sqlite3_column_type(stmt, index);
        if (actual == desired) {
            return;
        }

        if (c == convert::off || ((c == convert::null) != (actual == SQLITE_NULL))) {
            throw std::runtime_error("Invalid type conversion requested");
        }
    }

    template <typename T, typename... Ts>
    auto build_tuple(sqlite3_stmt* stmt, int& index, convert conv) {
        if constexpr (sizeof...(Ts) == 0) {
            return std::tuple<T>{extract<T>(stmt, index, conv)};
        } else {
            auto t = std::tuple<T>{extract<T>(stmt, index, conv)};
            return std::tuple_cat(std::move(t), build_tuple<Ts...>(stmt, index, conv));
        }
    }

    template <typename T>
    T extract(sqlite3_stmt* stmt, int& index, convert conv) {
#ifdef PFR_HPP
        if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
            T val;
            pfr::for_each_field(val, [&](auto& v) {
                v = extract<std::remove_reference_t<decltype(v)>>(stmt, index, conv);
            });
            return val;
        } else
#endif

        if constexpr (is_optional<T>()) {
            if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
                return {};
            } else {
                return extract<is_optional<T>::type>(stmt, index, conv);
            }
        } else if constexpr (std::is_integral_v<T>) {
            check_convert(stmt, index, SQLITE_INTEGER, conv);
            return sqlite3_column_int64(stmt, index++);
        } else if constexpr (std::is_floating_point_v<T>) {
            check_convert(stmt, index, SQLITE_FLOAT, conv);
            return sqlite3_column_double(stmt, index++);
        } else if constexpr (std::is_same_v<T, std::string>) {
            check_convert(stmt, index, SQLITE_TEXT, conv);
            auto ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index++));
            return std::string(ptr);
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
            check_convert(stmt, index, SQLITE_BLOB, conv);
            int size = sqlite3_column_bytes(stmt, index);
            const std::byte* ptr = sqlite3_column_blob(stmt, index++);
            return std::vector<std::byte>(ptr, ptr + size);
        } else {
            SLATE_STATIC_FAIL("Invalid output type");
        }
    }

    void check(int code) {
        if (code != SQLITE_OK) {
            throw exception(code);
        }
    }

    bool step(sqlite3_stmt* stmt) {
        auto result = sqlite3_step(stmt);
        if (result == SQLITE_DONE) {
            return true;
        } else if (result == SQLITE_ROW) {
            return false;
        } else {
            throw exception(result);
        }
    }

    constexpr std::string_view begin_deferred = "BEGIN DEFERRED;";
    constexpr std::string_view begin_immediate = "BEGIN IMMEDIATE;";
    constexpr std::string_view begin_exclusive = "BEGIN EXCLUSIVE;";

    std::string_view begin_transaction(transaction type) {
        if (type == transaction::deferred) {
            return begin_deferred;
        } else if (type == transaction::immediate) {
            return begin_immediate;
        } else {
            return begin_exclusive;
        }
    }

    std::string sql_command(std::string_view cmd, std::string_view arg) {
        std::string out(cmd.size() + arg.size() + 2, '\0');
        std::ranges::copy(cmd, out.data());
        out[cmd.size()] = ' ';
        std::ranges::copy(arg, out.data() + cmd.size() + 1);
        out.back() = ';';
        return out;
    }

} // namespace detail

template <typename... Types>
class cursor
{
    class sentinel {};

    class iterator
    {
    public:
        using value_type = std::tuple<Types...>;
        using difference_type = ptrdiff_t;

        iterator() = default;
        iterator(std::shared_ptr<sqlite3_stmt> stmt, convert conv) : m_stmt(std::move(stmt)), m_conv(conv) {
            if (sqlite3_column_count(m_stmt.get()) != detail::row_size<Types...>()) {
                throw std::out_of_range("Mismatch between query column count and extraction field count");
            }
        }

        iterator& operator++() {
            m_done = detail::step(m_stmt.get());
            return *this;
        }
        void operator++(int) { ++*this; }
        
        std::tuple<Types...> operator*() const {
            int i = 0;
            return detail::build_tuple<Types...>(m_stmt.get(), i, m_conv); 
        }
        bool operator==(const sentinel&) const noexcept { return m_done; }

    private:
        std::shared_ptr<sqlite3_stmt> m_stmt;
        convert m_conv = convert::off;
        bool m_done = false;
    };

public:
    cursor(std::shared_ptr<sqlite3_stmt> stmt, convert conv) : m_stmt(std::move(stmt)), m_conv(conv) {}
    iterator begin() { return iterator(m_stmt, m_conv); }
    sentinel end() { return sentinel{}; }

private:
    std::shared_ptr<sqlite3_stmt> m_stmt;
    convert m_conv = convert::off;
};

template <typename T>
class value_cursor
{
    class sentinel {};

    class iterator
    {
    public:
        using value_type = T;
        using difference_type = ptrdiff_t;

        iterator() = default;
        iterator(std::shared_ptr<sqlite3_stmt> stmt, convert conv) : m_stmt(std::move(stmt)), m_conv(conv) {
            if (sqlite3_column_count(m_stmt.get()) != detail::row_size<T>()) {
                throw std::out_of_range("Mismatch between query column count and extraction field count");
            }
        }

        iterator& operator++() {
            m_done = detail::step(m_stmt.get());
            return *this;
        }
        
        T operator*() const {
            int i = 0;
            return detail::extract<T>(m_stmt.get(), i, m_conv); 
        }
        bool operator==(const sentinel&) const noexcept { return m_done; }

    private:
        std::shared_ptr<sqlite3_stmt> m_stmt;
        convert m_conv = convert::off;
        bool m_done = false;
    };

public:
    value_cursor(std::shared_ptr<sqlite3_stmt> stmt, convert conv) : m_stmt(std::move(stmt)), m_conv(conv) {}
    iterator begin() { return iterator(m_stmt, m_conv); }
    sentinel end() { return sentinel{}; }

private:
    std::shared_ptr<sqlite3_stmt> m_stmt;
    convert m_conv = convert::off;
};

class statement
{
public:
    statement(sqlite3_stmt* s) : m_stmt(s, stmt_deleter) {}

    template <typename... Args>
    statement& execute(const Args&... args) {
        if (m_reset) {
            detail::check(sqlite3_reset(m_stmt.get()));
        }
        m_reset = true;

        int index = 1;
        (bind(args, index), ...);
        m_fetchable = !detail::step(m_stmt.get());

        return *this;
    }

    template <typename... Types>
    cursor<Types...> fetch(convert conversion = convert::off) {
        check_fetchable();
        return cursor<Types...>(m_stmt, conversion);
    }

    template <typename... Types>
    auto fetch_single(convert conversion = convert::off) {
        check_fetchable();
        return *cursor<Types...>(m_stmt, conversion).begin();
    }

    template <typename T>
    value_cursor<T> fetch_value(convert conversion = convert::off) {
        check_fetchable();
        return value_cursor<T>(m_stmt, conversion);
    }

    template <typename T>
    auto fetch_single_value(convert conversion = convert::off) {
        check_fetchable();
        return *value_cursor<T>(m_stmt, conversion).begin();
    }

private:
    static void stmt_deleter(sqlite3_stmt* ptr) {
        detail::check(sqlite3_finalize(ptr));
    }

    void check_fetchable() const {
        if (!m_fetchable) {
            throw std::runtime_error("No rows to fetch");
        }
    }

    template <typename T>
    void bind(const T& val, int& index) {
#ifdef PFR_HPP
        if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
            pfr::for_each_field(val, [&](const auto& v) {
                bind(v, index);
            });
            return;
        } else
#endif

        if constexpr (detail::is_optional<T>()) {
            if (val) {
                bind(*val, index);
                return;
            } else {
                detail::check(sqlite3_bind_null(m_stmt.get(), index));
            }
        } else if constexpr (std::is_integral_v<T>) {
            detail::check(sqlite3_bind_int64(m_stmt.get(), index, val));
        } else if constexpr (std::is_floating_point_v<T>) {
            detail::check(sqlite3_bind_double(m_stmt.get(), index, val));
        } else if constexpr (std::is_same_v<T, std::nullopt_t>) {
            detail::check(sqlite3_bind_null(m_stmt.get(), index));
        } else if constexpr (std::is_convertible_v<T, std::string_view>) {
            std::string_view view(val);
            detail::check(sqlite3_bind_text(m_stmt.get(), index, view.data(), view.size(), SQLITE_TRANSIENT));
        } else if constexpr (std::is_convertible_v<T, std::span<std::byte>>) {
            std::span<std::byte> span(val);
            detail::check(sqlite3_bind_blob(m_stmt.get(), index, span.data(), span.size(), SQLITE_TRANSIENT));
        } else {
            SLATE_STATIC_FAIL("Invalid input type");
        }

        index++;
    }

    std::shared_ptr<sqlite3_stmt> m_stmt;
    bool m_reset = false;
    bool m_fetchable = false;
};

class db
{
    using sqlite_ptr = std::unique_ptr<sqlite3, void(*)(sqlite3*)>;

public:
    db(std::string_view filename, open flags) : m_db(nullptr, db_deleter) {
        sqlite3* ptr;
        detail::check(sqlite3_open_v2(filename.data(), &ptr, static_cast<int>(flags), nullptr));
        m_db = sqlite_ptr(ptr, db_deleter);
    }
    db(std::string_view filename) : db(filename, open::read_write | open::create) {}

    statement prepare(std::string_view cmd) {
        sqlite3_stmt* stmt;
        detail::check(sqlite3_prepare_v2(m_db.get(), cmd.data(), cmd.size(), &stmt, nullptr));
        return statement(stmt);
    }

    template <typename... Types>
    statement execute(std::string_view cmd, const Types&... args) {
        return prepare(cmd).execute(args...);
    }

    void begin_transaction(transaction type = transaction::deferred) { execute(detail::begin_transaction(type)); }
    void commit() { execute("END;"); }
    void rollback() { execute("ROLLBACK;"); }
    void savepoint(std::string_view name) { execute(detail::sql_command("SAVEPOINT", name)); }
    void release(std::string_view name) { execute(detail::sql_command("RELEASE", name)); }
    void rollback_to(std::string_view name) { execute(detail::sql_command("ROLLBACK TO", name)); }

    sqlite3* get_raw() {
        return m_db.get();
    }

private:
    static void db_deleter(sqlite3* ptr) {
        detail::check(sqlite3_close_v2(ptr));
    }

    sqlite_ptr m_db;
};

class scoped_transaction
{
public:
    scoped_transaction(db& database, transaction type = transaction::deferred) : m_db(database.get_raw()) {
        execute(detail::begin_transaction(type));
    }
    scoped_transaction(scoped_transaction&&) = delete;
    scoped_transaction& operator=(scoped_transaction&&) = delete;
    ~scoped_transaction() {
        if (std::uncaught_exceptions()) {
            execute("ROLLBACK;");
        } else {
            execute("END;");
        }
    }

private:
    void execute(std::string_view cmd) {
        sqlite3_stmt* stmt;
        detail::check(sqlite3_prepare_v2(m_db, cmd.data(), cmd.size(), &stmt, nullptr));
        statement(stmt).execute();
    }

    sqlite3* m_db;
};

}