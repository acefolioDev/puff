#include "core.hpp"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include <sqlite3.h>
namespace puff {
namespace {
class SQLite final : public Driver {
  sqlite3 *db = nullptr;
  sqlite3_stmt *statement = nullptr;
  Profile profile;
  std::atomic_bool &cancelled;
  std::chrono::steady_clock::time_point deadline;
  bool timed = false;
  sqlite3_int64 initial_changes = 0;
  static int progress(void *p) {
    auto *self = static_cast<SQLite *>(p);
    return self->cancelled.load() ||
           (self->timed && std::chrono::steady_clock::now() > self->deadline);
  }
  void check(int code) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
      throw std::runtime_error(sqlite3_errmsg(db));
  }
  Page all(const std::string &sql, const std::vector<Cell> &params = {}) {
    auto out = execute(sql, params);
    while (out.more) {
      auto p = fetch();
      if (out.rows.size() + p.rows.size() > 1000)
        throw std::runtime_error("Metadata exceeds 1,000 rows");
      out.rows.insert(out.rows.end(), std::make_move_iterator(p.rows.begin()),
                      std::make_move_iterator(p.rows.end()));
      out.more = p.more;
    }
    return out;
  }

public:
  SQLite(const Profile &p, std::atomic_bool &c) : profile(p), cancelled(c) {
    if (!std::filesystem::is_regular_file(p.database))
      throw std::runtime_error("Select an existing SQLite database file");
    int code = sqlite3_open_v2(
        p.database.c_str(), &db,
        (p.readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE) | SQLITE_OPEN_NOMUTEX, nullptr);
    if (code != SQLITE_OK) {
      std::string e = db ? sqlite3_errmsg(db) : "SQLite allocation failed";
      if (db)
        sqlite3_close(db);
      db = nullptr;
      throw std::runtime_error(e);
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, static_cast<int>(page_bytes));
    sqlite3_progress_handler(db, 1000, progress, this);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    if (p.readonly)
      sqlite3_exec(db, "PRAGMA query_only=ON", nullptr, nullptr, nullptr);
  }
  ~SQLite() override {
    release();
    if (db)
      sqlite3_close(db);
  }
  void release() override {
    if (statement)
      sqlite3_finalize(statement);
    statement = nullptr;
    timed = false;
  }
  bool in_transaction() const override {
    return !sqlite3_get_autocommit(db);
  }
  Page execute(const std::string &sql, const std::vector<Cell> &params = {}) override {
    if (sql.empty() || sql.size() > 1000000 || sql.find('\0') != std::string::npos)
      throw std::runtime_error("Enter one statement, maximum 1 MiB, without NUL");
    release();
    initial_changes = sqlite3_total_changes64(db);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(profile.timeout);
    timed = true;
    const char *tail = nullptr;
    check(sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &statement, &tail));
    if (!statement)
      throw std::runtime_error("No SQL statement");
    // Reject trailing statements through SQLite's own parser, not a regex.
    sqlite3_stmt *extra = nullptr;
    int extra_code = sqlite3_prepare_v2(db, tail, -1, &extra, nullptr);
    if (extra)
      sqlite3_finalize(extra);
    if (extra || extra_code != SQLITE_OK) {
      release();
      throw std::runtime_error("Execute one statement at a time");
    }
    if (static_cast<int>(params.size()) != sqlite3_bind_parameter_count(statement)) {
      release();
      throw std::runtime_error("Incorrect query parameter count");
    }
    for (size_t i = 0; i < params.size(); ++i) {
      if (params[i])
        check(sqlite3_bind_text(statement, static_cast<int>(i + 1), params[i]->c_str(),
                                static_cast<int>(params[i]->size()), SQLITE_TRANSIENT));
      else
        check(sqlite3_bind_null(statement, static_cast<int>(i + 1)));
    }
    return fetch();
  }
  Page fetch() override {
    if (!statement)
      throw std::runtime_error("No active result");
    auto start = std::chrono::steady_clock::now();
    Page out;
    size_t size = 0;
    for (int c = 0; c < sqlite3_column_count(statement); ++c)
      out.names.emplace_back(sqlite3_column_name(statement, c));
    if (out.names.size() > 500)
      throw std::runtime_error("Select no more than 500 columns");
    try {
      while (out.rows.size() < page_rows) {
        int code = sqlite3_step(statement);
        if (code == SQLITE_DONE) {
          out.message = out.names.empty()
                            ? "Command completed (" +
                                  std::to_string(sqlite3_total_changes64(db) - initial_changes) +
                                  " total changes)"
                            : "Complete";
          release();
          break;
        }
        check(code);
        Row row;
        for (int c = 0; c < sqlite3_column_count(statement); ++c) {
          if (sqlite3_column_type(statement, c) == SQLITE_NULL)
            row.emplace_back(std::nullopt);
          else {
            auto bytes = sqlite3_column_bytes(statement, c);
            if (bytes > static_cast<int>(cell_bytes))
              throw std::runtime_error("A cell exceeds 256 KiB; select a substring");
            if (sqlite3_column_type(statement, c) == SQLITE_BLOB)
              throw std::runtime_error("Use hex(blob_column) in SQL to view SQLite binary data");
            if (sqlite3_column_type(statement, c) == SQLITE_FLOAT) {
              char buffer[64];
              auto converted = std::to_chars(
                  buffer, buffer + sizeof(buffer), sqlite3_column_double(statement, c),
                  std::chars_format::general, std::numeric_limits<double>::max_digits10);
              row.emplace_back(std::string(buffer, converted.ptr));
            } else {
              row.emplace_back(
                  std::string(reinterpret_cast<const char *>(sqlite3_column_text(statement, c)),
                              sqlite3_column_bytes(statement, c)));
            }
            size += row.back()->size();
          }
        }
        if (size > page_bytes)
          throw std::runtime_error("Page exceeds 4 MiB; select fewer columns");
        out.rows.push_back(std::move(row));
      }
      out.more = statement != nullptr;
      out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                   .count();
      return out;
    } catch (...) {
      release();
      throw;
    }
  }
  Page schemas() override {
    return {{"schema"}, {{Cell("main")}}};
  }
  Page tables(const std::string &s) override {
    if (s != "main")
      throw std::runtime_error("Unknown SQLite schema");
    return all("SELECT name,type FROM sqlite_schema WHERE type IN ('table','view') AND name NOT "
               "LIKE 'sqlite_%' ORDER BY name LIMIT 1000");
  }
  std::vector<Column> columns(const std::string &s, const std::string &t) override {
    if (s != "main")
      throw std::runtime_error("Unknown SQLite schema");
    auto p = all("PRAGMA table_xinfo(" + quote(t) + ")");
    std::vector<Column> out;
    for (auto &r : p.rows)
      out.push_back(
          {*r[1], r[2].value_or("text"), r[5] != Cell("0"), r[6] != Cell("0"), r[3] == Cell("0")});
    return out;
  }
  Page browse(const std::string &s, const std::string &t, size_t offset) override {
    if (offset > 100000000)
      throw std::runtime_error("Invalid offset");
    auto cols = columns(s, t);
    std::string order;
    for (auto &c : cols)
      if (c.primary) {
        if (!order.empty())
          order += ",";
        order += quote(c.name);
      }
    auto p =
        execute("SELECT * FROM " + qualified(s, t) + (order.empty() ? "" : " ORDER BY " + order) +
                " LIMIT 201 OFFSET " + std::to_string(offset));
    p.columns = std::move(cols);
    return p;
  }
  Page inspect(const std::string &, const std::string &t) override {
    return execute(
        "SELECT type,name,sql FROM sqlite_schema WHERE tbl_name=? ORDER BY type,name LIMIT 500",
        {t});
  }
  void mutate(const std::string &s, const std::string &t,
              const std::vector<Change> &changes) override {
    if (profile.readonly)
      throw std::runtime_error("Connection is read-only");
    if (changes.empty() || changes.size() > page_rows)
      throw std::runtime_error("Use 1–200 changes");
    auto cols = columns(s, t);
    all("SAVEPOINT puff_edit");
    try {
      for (auto &change : changes) {
        std::vector<Cell> params;
        std::string sql;
        auto entries = change.values;
        for (auto &[k, v] : entries) {
          auto c = std::find_if(cols.begin(), cols.end(),
                                [&](auto &c) { return c.name == k && !c.generated; });
          if (c == cols.end())
            throw std::runtime_error("Invalid writable column");
          params.push_back(v);
        }
        if (change.kind == "insert") {
          std::string names, values;
          for (auto &[k, v] : entries) {
            if (!names.empty()) {
              names += ",";
              values += ",";
            }
            names += quote(k);
            values += "?";
          }
          sql = "INSERT INTO " + qualified(s, t) +
                (names.empty() ? " DEFAULT VALUES" : " (" + names + ") VALUES (" + values + ")");
        } else {
          bool has_key = false;
          for (auto &c : cols)
            if (c.primary) {
              has_key = true;
              if (!change.original.contains(c.name) || !change.original.at(c.name))
                throw std::runtime_error("Complete primary key required");
            }
          if (!has_key)
            throw std::runtime_error("Primary key required");
          if (change.kind == "update" && !entries.empty()) {
            sql = "UPDATE " + qualified(s, t) + " SET ";
            bool first = true;
            for (auto &[k, v] : entries) {
              if (!first)
                sql += ",";
              first = false;
              sql += quote(k) + "=?";
            }
          } else if (change.kind == "delete") {
            sql = "DELETE FROM " + qualified(s, t);
            params.clear();
          } else
            throw std::runtime_error("Invalid row operation");
          sql += " WHERE ";
          bool first = true;
          for (auto &c : cols) {
            if (!change.original.contains(c.name))
              throw std::runtime_error("Original row incomplete");
            if (!first)
              sql += " AND ";
            first = false;
            sql += quote(c.name) + " IS ?";
            params.push_back(change.original.at(c.name));
          }
        }
        all(sql, params);
        if (sqlite3_changes(db) != 1)
          throw std::runtime_error("Row changed or disappeared; reload before saving");
      }
      all("RELEASE puff_edit");
    } catch (...) {
      try {
        all("ROLLBACK TO puff_edit");
        all("RELEASE puff_edit");
      } catch (...) {
      }
      throw;
    }
  }
};
} // namespace
std::unique_ptr<Driver> sqlite_driver(const Profile &p, std::atomic_bool &c) {
  return std::make_unique<SQLite>(p, c);
}
} // namespace puff
