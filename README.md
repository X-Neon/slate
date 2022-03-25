# Slate

A single header C++20 SQLite wrapper.

## Examples

### Open

```cpp
slate::db database("file.db");
```

### Insert

```cpp
db.execute("INSERT INTO table (a, b, c) VALUES (?, ?, ?)", 10, 2.5, "a string");

// Or (if PFR is available)

struct S { int a; double b; std::string c; };
S val = {10, 2.5, "a string"};
db.execute("INSERT INTO table (a, b, c) VALUES (?, ?, ?)", val);
```

### Select

```cpp

for (auto [a, b, c] : db.execute("SELECT a, b, c FROM table").fetch<int, double, std::string>()) {
    std::cout << a << " " << b << " " << c << std::endl;
}

// Or (if PFR is available)

struct S { int a; double b; std::string c; };
for (auto s : db.execute("SELECT a, b, c FROM table").fetch_value<S>()) {
    std::cout << s.a << " " << s.b << " " << s.c << std::endl;
}
```

### Count

```cpp
int count = db.execute("SELECT COUNT(*) FROM table").fetch_single_value<int>();
```

### Transaction

```cpp
db.begin_transaction();
db.execute("INSERT INTO table (a, b, c) VALUES (?, ?, ?)", s);
db.rollback(); // No rows added
db.execute("INSERT INTO table (a, b, c) VALUES (?, ?, ?)", s);
db.commit(); // One row added

try {
    slate::scoped_transaction scoped(db);
    db.execute("INSERT INTO table (a, b, c) VALUES (?, ?, ?)", s);
    throw std::exception{};
} catch (std::exception& e) {} // No rows added

{
    slate::scoped_transaction scoped(db);
    db.execute("INSERT INTO table (a, b, c) VALUES (?, ?, ?)", s);
} // One row added
```

### NULL values

```cpp
db.execute("INSERT INTO table (a) VALUES (?)", std::optional<int>(10)); // Inserts INTEGER
db.execute("INSERT INTO table (a) VALUES (?)", std::optional<int>()); // Inserts NULL

for (auto opt : db.execute("SELECT a FROM table").fetch_value<std::optional<int>>()) {
    if (opt.has_value()) {
        // INTEGER
    } else {
        // NULL
    }
}
```

## Input/Output Types

| SQL  | Input                                               | Output                                            |
|------|-----------------------------------------------------|---------------------------------------------------|
| INT  | Any integral type                                   | Any integral type                                 |
| REAL | Any floating point type                             | Any floating point type                           |
| TEXT | Convertible to `std::string_view`                   | `std::string`                                     |
| BLOB | Convertible to `std::span<T>`, where `T` is trivial | `std::vector<T, Allocator>`, where `T` is trivial |
| NULL | `std::nullopt_t`                                    | N/A                                               |

## Type conversions

By default, Slate will never perform any type conversions between database values and requested output types, and will throw an exception if they don't match. To allow type conversion, choose a different conversion policy in the fetch methods.

```cpp
db.execute("INSERT INTO table (a) VALUES (?)", std::nullopt);
int no_convert = db.execute("SELECT a FROM table").fetch_single_value<int>(); // Throws
int converted = db.execute("SELECT a FROM table").fetch_single_value<int>(slate::convert::on); // 0
```

## Requirements

Slate requires a C++20 compliant compiler. Aggregate serialization/deserialization requires PFR (either [Boost](https://github.com/boostorg/pfr) or [non-Boost](https://github.com/apolukhin/pfr_non_boost)). Tested on GCC 11.2 / Ubuntu 21.10.
