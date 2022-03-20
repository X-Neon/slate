#pragma once

#include <sqlite3.h>
#include <string_view>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <string>

namespace slate {

template<typename T>
struct is_optional : public std::false_type {};

template<typename T>
struct is_optional<std::optional<T>> : public std::true_type { using type = T; };

// template <typename T>
// struct optional_value {};

// template <typename T>
// struct optional_value<std::optional<T>> { using type = T; };

template <typename T, typename... Ts>
auto build(sqlite3_stmt* stmt, int index) {
    if constexpr (sizeof...(Ts) == 0) {
        return std::tuple<T>{extract<T>(stmt, index)};
    } else {
        return std::tuple_cat(std::tuple<T>{extract<T>(stmt, index)}, build<Ts...>(stmt, index + 1));
    }
}

template <typename T>
T extract(sqlite3_stmt* stmt, int index) {
    if constexpr (is_optional<T>()) {
        if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
            return {};
        } else {
            return extract<is_optional<T>::type>(stmt, index);
        }
    } else if constexpr (std::is_integral_v<T>) {
        return sqlite3_column_int64(stmt, index);
    } else if constexpr (std::is_floating_point_v<T>) {
        return sqlite3_column_double(stmt, index);
    } else if constexpr (std::is_same_v<T, std::string>) {
        auto ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
        return std::string(ptr);
    } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
        int size = sqlite3_column_bytes(stmt, index);
        const std::byte* ptr = sqlite3_column_blob(stmt, index);
        return std::vector<std::byte>(ptr, ptr + size);
    }
}

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
        iterator(std::shared_ptr<sqlite3_stmt> stmt) : m_stmt(std::move(stmt)) {}

        iterator& operator++() {
            auto result = sqlite3_step(m_stmt.get());
            if (result == SQLITE_DONE) {
                m_done = true;
            }

            return *this;
        }
        void operator++(int) { ++*this; }
        
        std::tuple<Types...> operator*() const { return build<Types...>(m_stmt.get(), 0); }
        bool operator==(const sentinel&) const noexcept { return m_done; }

    private:
        std::shared_ptr<sqlite3_stmt> m_stmt;
        bool m_done = false;
    };

public:
    cursor(std::shared_ptr<sqlite3_stmt> stmt) : m_stmt(std::move(stmt)) {}
    iterator begin() { return iterator(m_stmt); }
    sentinel end() { return sentinel{}; }

private:
    std::shared_ptr<sqlite3_stmt> m_stmt;
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
        iterator(std::shared_ptr<sqlite3_stmt> stmt) : m_stmt(std::move(stmt)) {}

        iterator& operator++() {
            auto result = sqlite3_step(m_stmt.get());
            if (result == SQLITE_DONE) {
                m_done = true;
            }

            return *this;
        }
        
        T operator*() const { return extract<T>(m_stmt.get(), 0); }
        bool operator==(const sentinel&) const noexcept { return m_done; }

    private:
        std::shared_ptr<sqlite3_stmt> m_stmt;
        bool m_done = false;
    };

public:
    value_cursor(std::shared_ptr<sqlite3_stmt> stmt) : m_stmt(std::move(stmt)) {}
    iterator begin() { return iterator(m_stmt); }
    sentinel end() { return sentinel{}; }

private:
    std::shared_ptr<sqlite3_stmt> m_stmt;
};

class statement
{
public:
    statement(sqlite3_stmt* s) : m_stmt(s, stmt_deleter) {}

    template <typename... Args>
    statement& execute(const Args&... args) {
        if (m_reset) {
            sqlite3_reset(m_stmt.get());
        }
        m_reset = true;

        int index = 1;
        (bind(args, index), ...);
        sqlite3_step(m_stmt.get());

        return *this;
    }

    template <typename... Types>
    cursor<Types...> fetch() {
        return cursor<Types...>(m_stmt);
    }

    template <typename... Types>
    auto fetch_single() {
        return *cursor<Types...>(m_stmt).begin();
    }

    template <typename T>
    value_cursor<T> fetch_value() {
        return value_cursor<T>(m_stmt);
    }

    template <typename T>
    auto fetch_single_value() {
        return *value_cursor<T>(m_stmt).begin();
    }

private:
    static void stmt_deleter(sqlite3_stmt* ptr) {
        sqlite3_finalize(ptr);
    }

    template <typename T>
    void bind(const T& val, int& index) {
        if constexpr (is_optional<T>()) {
            if (val) {
                bind(*val, index);
                return;
            } else {
                sqlite3_bind_null(m_stmt.get(), index);
            }
        } else if constexpr (std::is_integral_v<T>) {
            sqlite3_bind_int64(m_stmt.get(), index, val);
        } else if constexpr (std::is_floating_point_v<T>) {
            sqlite3_bind_double(m_stmt.get(), index, val);
        } else if constexpr (std::is_same_v<T, std::nullopt_t>) {
            sqlite3_bind_null(m_stmt.get(), index);
        } else if constexpr (std::is_convertible_v<T, std::string_view>) {
            std::string_view view(val);
            sqlite3_bind_text(m_stmt.get(), index, view.data(), view.size(), SQLITE_TRANSIENT);
        } else if constexpr (std::is_convertible_v<T, std::span<std::byte>>) {
            std::span<std::byte> span(val);
            sqlite3_bind_blob(m_stmt.get(), index, span.data(), span.size(), SQLITE_TRANSIENT);
        }

        index++;
    }

    std::shared_ptr<sqlite3_stmt> m_stmt;
    bool m_reset = false;
};

class db
{
    using sqlite_ptr = std::unique_ptr<sqlite3, void(*)(sqlite3*)>;

public:
    db(std::string_view filename) : m_db(nullptr, db_deleter) {
        sqlite3* ptr;
        if (sqlite3_open(filename.data(), &ptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(ptr));
        }

        m_db = sqlite_ptr(ptr, db_deleter);
    }

    statement prepare(std::string_view cmd) {
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(m_db.get(), cmd.data(), cmd.size(), &stmt, nullptr);
        return statement(stmt);
    }

    template <typename... Types>
    statement execute(std::string_view cmd, const Types&... args) {
        return prepare(cmd).execute(args...);
    }

private:
    static void db_deleter(sqlite3* ptr) {
        sqlite3_close(ptr);
    }

    sqlite_ptr m_db;
};

}