#pragma once

#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <string_view>
#include <string>
#include <variant>
#include <vector>

#if defined __has_include
#   if __has_include (<boost/pfr.hpp>)
#       include <boost/pfr.hpp>
#       define SLATE_USE_PFR
#       define SLATE_PFR_NAMESPACE namespace pfr = boost::pfr
#   elif __has_include (<pfr.hpp>)
#       include <pfr.hpp>
#       define SLATE_USE_PFR
#       define SLATE_PFR_NAMESPACE
#   endif
#endif

#define SLATE_STATIC_FAIL(msg) []<bool _ = false>(){ static_assert(_, msg); }();

namespace slate {

SLATE_PFR_NAMESPACE;

using any = std::variant<int64_t, double, std::string_view, std::span<const std::byte>, std::nullopt_t>;
using any_return = std::variant<int64_t, double, std::string, std::vector<std::byte>, std::nullopt_t>;

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

enum class function
{
    non_pure = SQLITE_UTF8,
    pure = SQLITE_UTF8 | SQLITE_DETERMINISTIC
};

constexpr open operator|(const open& a, const open& b) {
    return open{static_cast<int>(a) | static_cast<int>(b)};
}

template <typename T>
struct serializer {};

namespace detail {

    template <auto Start, auto End, auto Inc, class F>
    constexpr void constexpr_for(F&& f) {
        if constexpr (Start < End) {
            f(std::integral_constant<decltype(Start), Start>());
            constexpr_for<Start + Inc, End, Inc>(f);
        }
    }

    template <class F, class Tuple>
    constexpr void constexpr_for_tuple(F&& f, Tuple&& tuple) {
        constexpr size_t size = std::tuple_size_v<std::decay_t<Tuple>>;
        constexpr_for<0UL, size, 1UL>([&](auto i) {
            f(std::get<i.value>(tuple));
        });
    }

    template <typename T>
    struct is_optional : public std::false_type {};

    template <typename T>
    struct is_optional<std::optional<T>> : public std::true_type { using type = T; };

    template <typename T>
    struct is_vector : public std::false_type {};

    template <typename T, typename A>
    struct is_vector<std::vector<T, A>> : public std::true_type { using type = T; };

    template <typename T>
    struct is_variant : public std::false_type {};

    template <typename... T>
    struct is_variant<std::variant<T...>> : public std::true_type {};

    template <typename T>
    concept is_trivial_vector = is_vector<T>::value && std::is_trivial_v<typename T::value_type>&& 
                                !std::is_same_v<typename T::value_type, bool>;

    template <typename T>
    concept is_span_trivial = std::is_trivial_v<typename T::value_type>;

    template <typename T>
    concept is_byte_span_convertible = requires(T a) {
        std::as_bytes(std::span(a));
        { std::span(a) } -> is_span_trivial;
    };

    template <typename T>
    struct function_traits {};

    template <typename R, typename... T>
    struct function_traits<std::function<R(T...)>*>
    {
        using return_t = R;
        using args_t = std::tuple<T...>;
    };

    template <typename T>
    struct deserializer_arg {};

    template <typename R, typename T>
    struct deserializer_arg<R(*)(T)> { using type = T; };

    template <typename T>
    concept is_serializable = requires(T a) {
        serializer<T>::to_sql(a);
    };

    template <typename T>
    concept is_deserializable = requires {
        serializer<T>::from_sql;
    };

#ifdef SLATE_USE_PFR
    template <std::size_t I>
    using size_constant = std::integral_constant<std::size_t, I>;

    template <typename T, typename I, typename M>
    constexpr int aggregate_size();

    template <typename T>
    constexpr int field_size() {
        if constexpr (is_deserializable<T>) {
            using arg_t = deserializer_arg<decltype(&slate::serializer<T>::from_sql)>::type;
            return field_size<arg_t>();
        } else if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
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

    inline void check_convert(sqlite3_stmt* stmt, int index, int desired, convert c) {
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

    template <typename T>
    T extract_from_column(sqlite3_stmt* stmt, int& index, convert conv) {
        if constexpr (is_deserializable<T>) {
            using arg_t = detail::deserializer_arg<decltype(&slate::serializer<T>::from_sql)>::type;
            return serializer<T>::from_sql(extract_from_column<arg_t>(stmt, index, conv));
        } else
#ifdef SLATE_USE_PFR
        if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
            T val;
            pfr::for_each_field(val, [&](auto& v) {
                v = extract_from_column<std::remove_reference_t<decltype(v)>>(stmt, index, conv);
            });
            return val;
        } else
#endif
        if constexpr (is_optional<T>()) {
            if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
                index++;
                return {};
            } else {
                return extract_from_column<typename is_optional<T>::type>(stmt, index, conv);
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
        } else if constexpr (is_trivial_vector<T>) {
            check_convert(stmt, index, SQLITE_BLOB, conv);
            int size = sqlite3_column_bytes(stmt, index);
            if (size == 0) {
                return T{};
            }

            const void* ptr = sqlite3_column_blob(stmt, index++);
            T vec;
            vec.resize((size - 1) / sizeof(typename T::value_type) + 1);
            std::memcpy(vec.data(), ptr, size);
            return vec;
        } else {
            SLATE_STATIC_FAIL("Invalid output type");
        }
    }

    template <typename... T>
    auto fetch_values(sqlite3_stmt* stmt, convert conv) {
        std::tuple<T...> values;
        int index = 0;
        constexpr_for_tuple([&](auto&& val) {
            val = extract_from_column<std::remove_reference_t<decltype(val)>>(stmt, index, conv);
        }, values);

        return values;
    }

    template <typename T>
    T extract_from_values(sqlite3_value** values) {
        if constexpr (std::is_same_v<T, any>) {
            auto type = sqlite3_value_type(*values);
            if (type == SQLITE_INTEGER) {
                return extract_from_values<int64_t>(values);
            } else if (type == SQLITE_FLOAT) {
                return extract_from_values<double>(values);
            } else if (type == SQLITE_TEXT) {
                return extract_from_values<std::string_view>(values);
            } else if (type == SQLITE_BLOB) {
                return extract_from_values<std::span<const std::byte>>(values);
            } else {
                return std::nullopt;
            }
        } else if constexpr (is_optional<T>() && !std::is_same_v<typename is_optional<T>::type, any>) {
            if (sqlite3_value_type(*values) == SQLITE_NULL) {
                values++;
                return {};
            } else {
                return extract_from_values<typename is_optional<T>::type>(values);
            }
        } else if constexpr (std::is_integral_v<T>) {
            return sqlite3_value_int64(*values++);
        } else if constexpr (std::is_floating_point_v<T>) {
            return sqlite3_value_double(*values++);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            auto ptr = reinterpret_cast<const char*>(sqlite3_value_text(*values));
            return std::string_view(ptr);
        } else if constexpr (std::is_same_v<T, std::span<const std::byte>>) {
            int size = sqlite3_value_bytes(*values);
            auto ptr = reinterpret_cast<const std::byte*>(sqlite3_value_blob(*values));
            return std::span<const std::byte>(ptr, size);
        } else {
            SLATE_STATIC_FAIL("Invalid argument type");
        }
    }

    template <typename Tuple>
    auto create_function_args(sqlite3_value** values) {
        Tuple args;
        constexpr_for_tuple([&](auto&& arg) {
            arg = extract_from_values<std::remove_reference_t<decltype(arg)>>(values++);
        }, args);

        return args;
    }

    template <typename T>
    void set_function_return_value(sqlite3_context* context, T value) {
        if constexpr (is_variant<T>()) {
            std::visit([&](auto v) {
                set_function_return_value(context, v);
            }, value);
        } else if constexpr (detail::is_optional<T>()) {
            if (value) {
                set_function_return_value(context, *value);
            } else {
                sqlite3_result_null(context);
            }
        } else if constexpr (std::is_integral_v<T>) {
            sqlite3_result_int64(context, value);
        } else if constexpr (std::is_floating_point_v<T>) {
            sqlite3_result_double(context, value);
        } else if constexpr (std::is_same_v<T, std::nullopt_t>) {
            sqlite3_result_null(context);
        } else if constexpr (std::is_convertible_v<T, std::string_view>) {
            std::string_view view(value);
            sqlite3_result_text(context, view.data(), view.size(), SQLITE_TRANSIENT);
        } else if constexpr (detail::is_byte_span_convertible<T>) {
            auto span = std::as_bytes(std::span(value));
            sqlite3_result_blob(context, span.data(), span.size(), SQLITE_TRANSIENT);
        } else {
            SLATE_STATIC_FAIL("Invalid input type");
        }
    }

    inline void check(int code) {
        if (code != SQLITE_OK) {
            throw exception(code);
        }
    }

    inline bool step(sqlite3_stmt* stmt) {
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

    inline std::string_view begin_transaction(transaction type) {
        if (type == transaction::deferred) {
            return begin_deferred;
        } else if (type == transaction::immediate) {
            return begin_immediate;
        } else {
            return begin_exclusive;
        }
    }

    inline std::string sql_command(std::string_view cmd, std::string_view arg) {
        std::string out(cmd.size() + arg.size() + 2, '\0');
        std::ranges::copy(cmd, out.data());
        out[cmd.size()] = ' ';
        std::ranges::copy(arg, out.data() + cmd.size() + 1);
        out.back() = ';';
        return out;
    }

    template <typename T>
    constexpr bool is_input_t() {
#ifdef SLATE_USE_PFR
        if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
            bool valid = true;
            constexpr_for<0, pfr::tuple_size_v<T>, 1>([&](auto i) {
                valid = valid && is_input_t<pfr::tuple_element_t<i, T>>();
            });

            return valid;
        } else
#endif
        if constexpr (is_optional<T>()) {
            return is_input_t<typename is_optional<T>::type>();
        } else if constexpr (is_serializable<T>) {
            return is_input_t<decltype(serializer<T>::to_sql(std::declval<T>()))>();
        } else {
            return std::is_integral_v<T> || std::is_floating_point_v<T> || std::is_same_v<T, std::nullopt_t> ||
                   std::is_convertible_v<T, std::string_view> || is_byte_span_convertible<T>;
        }
    }

    template <typename T>
    constexpr bool is_output_t() {
#ifdef SLATE_USE_PFR
        if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
            bool valid = true;
            constexpr_for<0, pfr::tuple_size_v<T>, 1>([&](auto i) {
                valid = valid && is_output_t<pfr::tuple_element_t<i, T>>();
            });

            return valid;
        } else
#endif
        if constexpr (is_optional<T>()) {
            return is_input_t<typename is_optional<T>::type>();
        } else if constexpr (is_deserializable<T>) {
            return is_input_t<typename deserializer_arg<decltype(&slate::serializer<T>::from_sql)>::type>();
        } else {
            return std::is_integral_v<T> || std::is_floating_point_v<T> ||
                   std::is_same_v<T, std::string> || is_trivial_vector<T>;
        }      
    }

} // namespace detail

template <typename T>
concept input = detail::is_input_t<T>();

template <typename T>
concept output = detail::is_output_t<T>();

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
        iterator(std::shared_ptr<sqlite3_stmt> stmt, convert conv, bool done) : 
            m_stmt(std::move(stmt)), m_conv(conv), m_done(done) {
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
            return detail::fetch_values<Types...>(m_stmt.get(), m_conv);
        }
        bool operator==(const sentinel&) const noexcept { return m_done; }

    private:
        std::shared_ptr<sqlite3_stmt> m_stmt;
        convert m_conv = convert::off;
        bool m_done = true;
    };

public:
    cursor(std::shared_ptr<sqlite3_stmt> stmt, convert conv, bool done) : 
        m_stmt(std::move(stmt)), m_conv(conv), m_done(done) {}
    iterator begin() { return iterator(m_stmt, m_conv, m_done); }
    sentinel end() { return sentinel{}; }

private:
    std::shared_ptr<sqlite3_stmt> m_stmt;
    convert m_conv;
    bool m_done;
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
        iterator(std::shared_ptr<sqlite3_stmt> stmt, convert conv, bool done) : 
            m_stmt(std::move(stmt)), m_conv(conv), m_done(done) {
            if (sqlite3_column_count(m_stmt.get()) != detail::row_size<T>()) {
                throw std::out_of_range("Mismatch between query column count and extraction field count");
            }
        }

        iterator& operator++() {
            m_done = detail::step(m_stmt.get());
            return *this;
        }
        void operator++(int) { ++*this; }
        
        T operator*() const {
            int i = 0;
            return detail::extract_from_column<T>(m_stmt.get(), i, m_conv); 
        }
        bool operator==(const sentinel&) const noexcept { return m_done; }

    private:
        std::shared_ptr<sqlite3_stmt> m_stmt;
        convert m_conv = convert::off;
        bool m_done = true;
    };

public:
    value_cursor(std::shared_ptr<sqlite3_stmt> stmt, convert conv, bool done) : 
        m_stmt(std::move(stmt)), m_conv(conv), m_done(done) {}
    iterator begin() { return iterator(m_stmt, m_conv, m_done); }
    sentinel end() { return sentinel{}; }

private:
    std::shared_ptr<sqlite3_stmt> m_stmt;
    convert m_conv;
    bool m_done;
};

class statement
{
    enum class state
    {
        clean,
        executed,
        fetched
    };

public:
    statement(sqlite3_stmt* s) : m_stmt(s, stmt_deleter) {}

    template <input... Args>
    statement& execute(const Args&... args) {
        if (m_state != state::clean) {
            detail::check(sqlite3_reset(m_stmt.get()));
        }
        m_state = state::executed;

        [[maybe_unused]] int index = 1;
        (bind(args, index), ...);
        m_done = detail::step(m_stmt.get());

        return *this;
    }

    template <output... Types>
    cursor<Types...> fetch(convert conversion = convert::off) {
        transition_to_fetched();
        return cursor<Types...>(m_stmt, conversion, m_done);
    }

    template <output... Types>
    auto fetch_single(convert conversion = convert::off) {
        transition_to_fetched();
        return *cursor<Types...>(m_stmt, conversion, m_done).begin();
    }

    template <output T>
    value_cursor<T> fetch_value(convert conversion = convert::off) {
        transition_to_fetched();
        return value_cursor<T>(m_stmt, conversion, m_done);
    }

    template <output T>
    auto fetch_single_value(convert conversion = convert::off) {
        transition_to_fetched();
        return *value_cursor<T>(m_stmt, conversion, m_done).begin();
    }

private:
    static void stmt_deleter(sqlite3_stmt* ptr) {
        detail::check(sqlite3_finalize(ptr));
    }

    void transition_to_fetched() {
        if (m_state == state::fetched) {
            throw std::runtime_error("Rows can only be fetched once");
        }

        m_state = state::fetched;
    }

    template <typename T>
    void bind(const T& val, int& index) {
        if constexpr (detail::is_serializable<T>) {
            bind(serializer<T>::to_sql(val), index);
            return;
        } else
#ifdef SLATE_USE_PFR
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
        } else if constexpr (detail::is_byte_span_convertible<T>) {
            auto span = std::as_bytes(std::span(val));
            detail::check(sqlite3_bind_blob(m_stmt.get(), index, span.data(), span.size(), SQLITE_TRANSIENT));
        } else {
            SLATE_STATIC_FAIL("Invalid input type");
        }

        index++;
    }

    std::shared_ptr<sqlite3_stmt> m_stmt;
    state m_state = state::clean;
    bool m_done = false;
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
    explicit db(std::string_view filename) : db(filename, open::read_write | open::create) {}

    statement prepare(std::string_view cmd) {
        sqlite3_stmt* stmt;
        detail::check(sqlite3_prepare_v2(m_db.get(), cmd.data(), cmd.size(), &stmt, nullptr));
        return statement(stmt);
    }

    template <input... Types>
    statement execute(std::string_view cmd, const Types&... args) {
        return prepare(cmd).execute(args...);
    }

    int64_t last_insert_rowid() const {
        return sqlite3_last_insert_rowid(m_db.get());
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

    template <typename F>
    void declare(std::string_view name, function type, F&& func) {
        auto func_ptr = new std::function(std::move(func));

        using args_t = detail::function_traits<decltype(func_ptr)>::args_t;
        constexpr bool var_args = std::tuple_size_v<args_t> == 1 && 
                                  detail::is_vector<std::tuple_element_t<0, args_t>>();
        constexpr int n_args = var_args ? -1 : int(std::tuple_size_v<args_t>);

        detail::check(sqlite3_create_function_v2(
            m_db.get(), 
            name.data(), 
            n_args, 
            static_cast<int>(type),
            reinterpret_cast<void*>(func_ptr),
            &invoker<decltype(func_ptr)>,
            nullptr,
            nullptr,
            &function_deleter<decltype(func_ptr)>
        ));
    }

private:
    static void db_deleter(sqlite3* ptr) {
        detail::check(sqlite3_close_v2(ptr));
    }

    template <typename F>
    static void function_deleter(void* ptr) {
        delete static_cast<F>(ptr);
    }

    template <typename F>
    static void invoker(sqlite3_context* c, int n, sqlite3_value** values) {
        using args_t = detail::function_traits<F>::args_t;
        
        auto func = reinterpret_cast<F>(sqlite3_user_data(c));
        if constexpr (std::tuple_size_v<args_t> == 1 && detail::is_vector<std::tuple_element_t<0, args_t>>()) {
            using T = detail::is_vector<std::tuple_element_t<0, args_t>>::type;
            
            std::vector<T> args;
            for (int i = 0; i < n; ++i) {
                args.push_back(detail::extract_from_values<T>(&values[i]));
            }
            auto return_value = (*func)(std::move(args));
            detail::set_function_return_value(c, return_value);
        } else {
            auto args = detail::create_function_args<args_t>(values);
            auto return_value = std::apply(*func, args);
            detail::set_function_return_value(c, return_value);
        }
    }

    sqlite_ptr m_db;
};

class scoped_transaction
{
public:
    explicit scoped_transaction(db& database, transaction type = transaction::deferred) : m_db(database.get_raw()) {
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