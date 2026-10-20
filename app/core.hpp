#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace puff {
using Cell = std::optional<std::string>;
using Row = std::vector<Cell>;
constexpr size_t page_rows = 200, page_bytes = 4 * 1024 * 1024, cell_bytes = 256 * 1024;
struct Profile {
  std::string id, name = "Local PostgreSQL", engine = "postgres", host = "localhost",
                  user = "postgres", password, database = "postgres", tls = "prefer",
                  environment = "development", group = "Local";
  int port = 5432, timeout = 30;
  bool readonly = true, remember = false;
  void validate() const;
};
struct Column {
  std::string name, type;
  bool primary = false, generated = false, nullable = true;
};
struct Page {
  std::vector<std::string> names;
  std::vector<Row> rows;
  std::vector<Column> columns;
  bool more = false;
  double ms = 0;
  std::string message;
};
struct Change {
  std::string kind;
  std::map<std::string, Cell> values, original;
};
std::string quote(const std::string &);
std::string qualified(const std::string &, const std::string &);
std::string type(const std::string &);
std::string schema_sql(const std::string &, const std::string &, const std::string &,
                       const std::string &, const std::string &, const std::string &);
std::string create_table_sql(const std::string &, const std::string &, const std::vector<Column> &);
bool needs_review(const std::string &);
std::string redact(std::string, const std::string &);
std::string text(const Cell &);
class Driver {
public:
  virtual ~Driver() = default;
  virtual Page execute(const std::string &, const std::vector<Cell> & = {}) = 0;
  virtual Page fetch() = 0;
  virtual void release() = 0;
  virtual std::vector<Column> columns(const std::string &, const std::string &) = 0;
  virtual Page schemas() = 0;
  virtual Page tables(const std::string &) = 0;
  virtual Page inspect(const std::string &, const std::string &) = 0;
  virtual Page browse(const std::string &, const std::string &, size_t) = 0;
  virtual void mutate(const std::string &, const std::string &, const std::vector<Change> &) = 0;
  virtual bool in_transaction() const = 0;
  virtual void create_database(const std::string &);
};
std::unique_ptr<Driver> connect(const Profile &, std::atomic_bool &);
std::unique_ptr<Driver> postgres_driver(const Profile &, std::atomic_bool &);
std::unique_ptr<Driver> sqlite_driver(const Profile &, std::atomic_bool &);
void export_page(const std::string &, const Page &, bool);
void backup(const Profile &, const std::string &, bool, std::atomic_bool &);
} // namespace puff
