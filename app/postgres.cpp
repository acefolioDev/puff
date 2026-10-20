#include "core.hpp"
#include <algorithm>
#include <libpq-fe.h>
#include <mutex>
#include <poll.h>
namespace puff {
namespace {
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
class Postgres final : public Driver {
  PGconn *conn = nullptr;
  Profile profile;
  std::atomic_bool &cancelled;
  bool active = false;
  bool started_in_transaction = false;
  std::optional<Row> pending;
  std::vector<std::string> names;
  using Clock = std::chrono::steady_clock;
  Clock::time_point deadline;
  void socket_wait(int events) {
    auto now = Clock::now();
    if (now > deadline)
      throw std::runtime_error("Database network/statement timeout; cancel or reconnect if needed");
    pollfd fd{PQsocket(conn), static_cast<short>(events), 0};
    if (fd.fd < 0 || poll(&fd, 1, 100) < 0)
      throw std::runtime_error("Database socket unavailable");
  }
  void cancel_request() {
    auto request = PQcancelCreate(conn);
    if (!request)
      return;
    auto end = Clock::now() + std::chrono::seconds(2);
    bool sent = false;
    if (PQcancelStart(request))
      while (Clock::now() < end) {
        auto status = PQcancelPoll(request);
        if (status == PGRES_POLLING_OK) {
          sent = true;
          break;
        }
        if (status == PGRES_POLLING_FAILED)
          break;
        pollfd fd{PQcancelSocket(request),
                  static_cast<short>(status == PGRES_POLLING_WRITING ? POLLOUT : POLLIN), 0};
        poll(&fd, 1, 50);
      }
    PQcancelFinish(request);
    if (!sent) {
      PQfinish(conn);
      conn = nullptr;
      active = false;
      throw std::runtime_error(
          "Cancellation could not reach server; connection closed. Reconnect.");
    }
  }
  void flush() {
    while (PQflush(conn) == 1) {
      if (cancelled.exchange(false))
        cancel_request();
      socket_wait(POLLOUT);
    }
  }
  PGresult *result() {
    while (PQisBusy(conn)) {
      if (cancelled.exchange(false))
        cancel_request();
      socket_wait(POLLIN);
      if (!PQconsumeInput(conn))
        throw std::runtime_error(PQerrorMessage(conn));
    }
    return PQgetResult(conn);
  }
  std::string error(PGresult *r) {
    std::string out = PQresultErrorMessage(r);
    for (auto [field, label] : {std::pair{PG_DIAG_SQLSTATE, "SQLSTATE "},
                                std::pair{PG_DIAG_STATEMENT_POSITION, "Position "},
                                std::pair{PG_DIAG_MESSAGE_DETAIL, "Detail: "},
                                std::pair{PG_DIAG_MESSAGE_HINT, "Hint: "}}) {
      const char *v = PQresultErrorField(r, field);
      if (v)
        out += '\n' + std::string(label) + v;
    }
    return out;
  }
  std::optional<Row> next_row(std::string &message) {
    while (active) {
      Result r(result(), PQclear);
      if (!r) {
        active = false;
        return {};
      }
      auto status = PQresultStatus(r.get());
      if (status == PGRES_SINGLE_TUPLE) {
        if (names.empty())
          for (int c = 0; c < PQnfields(r.get()); ++c)
            names.emplace_back(PQfname(r.get(), c));
        if (names.size() > 500)
          throw std::runtime_error("Result exceeds 500 columns; select fewer columns");
        Row row;
        row.reserve(names.size());
        for (int c = 0; c < PQnfields(r.get()); ++c) {
          if (PQgetisnull(r.get(), 0, c))
            row.emplace_back(std::nullopt);
          else {
            int len = PQgetlength(r.get(), 0, c);
            if (len > static_cast<int>(cell_bytes))
              throw std::runtime_error(
                  "A cell exceeds 256 KiB. Select a substring or use native backup tools.");
            row.emplace_back(std::string(PQgetvalue(r.get(), 0, c), len));
          }
        }
        return row;
      }
      if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
        if (names.empty())
          for (int c = 0; c < PQnfields(r.get()); ++c)
            names.emplace_back(PQfname(r.get(), c));
        message = PQcmdStatus(r.get());
        continue;
      }
      if (status == PGRES_COPY_IN || status == PGRES_COPY_OUT || status == PGRES_COPY_BOTH) {
        PQfinish(conn);
        conn = nullptr;
        active = false;
        throw std::runtime_error("COPY protocol requires a dedicated tool. Connection closed; "
                                 "reconnect. Use pg_dump/psql for COPY.");
      }
      auto message = error(r.get());
      while (auto rest = PQgetResult(conn))
        PQclear(rest);
      active = false;
      throw std::runtime_error(message);
    }
    return {};
  }
  Page all(const std::string &sql, const std::vector<Cell> &params = {}) {
    Page out = execute(sql, params);
    while (out.more) {
      auto next = fetch();
      if (out.rows.size() + next.rows.size() > 1000)
        throw std::runtime_error("Metadata exceeds 1,000 rows");
      out.rows.insert(out.rows.end(), std::make_move_iterator(next.rows.begin()),
                      std::make_move_iterator(next.rows.end()));
      out.more = next.more;
    }
    return out;
  }

public:
  Postgres(const Profile &p, std::atomic_bool &c) : profile(p), cancelled(c) {
    const char *keys[] = {"host",
                          "port",
                          "user",
                          "password",
                          "dbname",
                          "sslmode",
                          "connect_timeout",
                          "application_name",
                          "options",
                          nullptr};
    std::string port = std::to_string(p.port),
                options = "-c client_encoding=UTF8 -c statement_timeout=" +
                          std::to_string(p.timeout * 1000) +
                          " -c lock_timeout=5000 -c default_transaction_read_only=" +
                          (p.readonly ? "on" : "off");
    const char *vals[] = {
        p.host.c_str(), port.c_str(), p.user.c_str(), p.password.c_str(), p.database.c_str(),
        p.tls.c_str(),  "5",          "My Puff",      options.c_str(),    nullptr};
    conn = PQconnectStartParams(keys, vals, 0);
    if (!conn)
      throw std::runtime_error("Could not allocate PostgreSQL connection");
    try {
      deadline = Clock::now() + std::chrono::seconds(5);
      while (true) {
        if (cancelled.exchange(false))
          throw std::runtime_error("Connection cancelled");
        auto status = PQconnectPoll(conn);
        if (status == PGRES_POLLING_OK)
          break;
        if (status == PGRES_POLLING_FAILED)
          throw std::runtime_error(PQerrorMessage(conn));
        socket_wait(status == PGRES_POLLING_WRITING ? POLLOUT : POLLIN);
      }
      if (PQsetnonblocking(conn, 1) != 0)
        throw std::runtime_error(PQerrorMessage(conn));
      PQsetNoticeProcessor(conn, [](void *, const char *) {}, nullptr);
    } catch (...) {
      PQfinish(conn);
      conn = nullptr;
      throw;
    }
  }
  ~Postgres() override {
    if (conn)
      PQfinish(conn);
  }
  Page execute(const std::string &sql, const std::vector<Cell> &params = {}) override {
    if (sql.empty() || sql.size() > 1000000 || sql.find('\0') != std::string::npos)
      throw std::runtime_error("Enter one SQL statement, maximum 1 MiB, without NUL");
    release();
    if (!conn || PQstatus(conn) != CONNECTION_OK)
      throw std::runtime_error("Connection closed; reconnect");
    auto start = Clock::now();
    deadline = start + std::chrono::seconds(profile.timeout + 5);
    auto prior_status = PQtransactionStatus(conn);
    started_in_transaction = prior_status == PQTRANS_INTRANS || prior_status == PQTRANS_INERROR;
    names.clear();
    pending.reset();
    std::vector<const char *> values;
    for (auto &v : params)
      values.push_back(v ? v->c_str() : nullptr);
    if (!PQsendQueryParams(conn, sql.c_str(), static_cast<int>(values.size()), nullptr,
                           values.data(), nullptr, nullptr, 0))
      throw std::runtime_error(PQerrorMessage(conn));
    if (!PQsetSingleRowMode(conn))
      throw std::runtime_error("Could not enable incremental result mode");
    active = true;
    flush();
    auto out = fetch();
    out.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return out;
  }
  Page fetch() override {
    auto start = Clock::now();
    deadline = start + std::chrono::seconds(profile.timeout + 5);
    Page out;
    size_t bytes = 0;
    try {
      while (out.rows.size() < page_rows) {
        auto row = pending ? std::move(pending) : next_row(out.message);
        pending.reset();
        if (!row)
          break;
        size_t size = 0;
        for (auto &v : *row)
          if (v)
            size += v->size();
        if (size > page_bytes)
          throw std::runtime_error("Row exceeds 4 MiB; select fewer columns");
        if (bytes + size > page_bytes && !out.rows.empty()) {
          pending = std::move(row);
          break;
        }
        bytes += size;
        out.rows.push_back(std::move(*row));
      }
      out.names = names;
      out.more = active || pending.has_value();
      out.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      return out;
    } catch (...) {
      if (conn) {
        cancel_request();
        try {
          while (auto r = result())
            PQclear(r);
        } catch (...) {
          PQfinish(conn);
          conn = nullptr;
        }
      }
      active = false;
      pending.reset();
      throw;
    }
  }
  void release() override {
    if (conn && active) {
      cancel_request();
      deadline = Clock::now() + std::chrono::seconds(3);
      try {
        while (auto r = result())
          PQclear(r);
      } catch (...) {
        PQfinish(conn);
        conn = nullptr;
      }
    }
    active = false;
    pending.reset();
  }
  bool in_transaction() const override {
    if (!conn)
      return false;
    auto status = PQtransactionStatus(conn);
    return status == PQTRANS_INTRANS || status == PQTRANS_INERROR ||
           (status == PQTRANS_ACTIVE && started_in_transaction);
  }
  Page schemas() override {
    return all("SELECT nspname FROM pg_namespace WHERE nspname NOT LIKE 'pg\\_%' ESCAPE '\\' AND "
               "nspname <> 'information_schema' ORDER BY 1 LIMIT 500");
  }
  Page tables(const std::string &s) override {
    return all("SELECT c.relname,c.relkind::text FROM pg_class c JOIN pg_namespace n ON "
               "n.oid=c.relnamespace WHERE n.nspname=$1 AND c.relkind IN ('r','p','v','m') ORDER "
               "BY 1 LIMIT 1000",
               {s});
  }
  std::vector<Column> columns(const std::string &s, const std::string &t) override {
    auto p = all(
        "SELECT a.attname,format_type(a.atttypid,a.atttypmod),EXISTS(SELECT 1 FROM pg_index i "
        "WHERE i.indrelid=c.oid AND i.indisprimary AND a.attnum=ANY(i.indkey)),(a.attgenerated<>'' "
        "OR a.attidentity<>''),NOT a.attnotnull FROM pg_attribute a JOIN pg_class c ON "
        "c.oid=a.attrelid JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname=$1 AND "
        "c.relname=$2 AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum LIMIT 500",
        {s, t});
    std::vector<Column> out;
    for (auto &r : p.rows)
      out.push_back({*r[0], *r[1], r[2] == Cell("t"), r[3] == Cell("t"), r[4] == Cell("t")});
    return out;
  }
  Page browse(const std::string &s, const std::string &t, size_t offset) override {
    if (offset > 100000000)
      throw std::runtime_error("Invalid table offset");
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
    if (p.rows.size() < page_rows)
      p.more = false;
    return p;
  }
  void mutate(const std::string &s, const std::string &t,
              const std::vector<Change> &changes) override {
    if (profile.readonly)
      throw std::runtime_error("Connection is read-only");
    if (changes.empty() || changes.size() > page_rows)
      throw std::runtime_error("Use a batch of 1–200 changes");
    auto cols = columns(s, t);
    bool existing = in_transaction();
    all(existing ? "SAVEPOINT puff_edit" : "BEGIN");
    try {
      for (auto &change : changes) {
        std::vector<Cell> params;
        auto bind = [&](const Cell &v) {
          params.push_back(v);
          return "$" + std::to_string(params.size());
        };
        std::string sql;
        if (change.kind == "insert") {
          std::string names, values;
          for (auto &[k, v] : change.values) {
            auto c = std::find_if(cols.begin(), cols.end(),
                                  [&](auto &c) { return c.name == k && !c.generated; });
            if (c == cols.end())
              throw std::runtime_error("Invalid writable column");
            if (!names.empty()) {
              names += ",";
              values += ",";
            }
            names += quote(k);
            values += bind(v);
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
          if (change.kind == "update" && !change.values.empty()) {
            sql = "UPDATE " + qualified(s, t) + " SET ";
            bool first = true;
            for (auto &[k, v] : change.values) {
              auto c = std::find_if(cols.begin(), cols.end(),
                                    [&](auto &c) { return c.name == k && !c.generated; });
              if (c == cols.end())
                throw std::runtime_error("Invalid writable column");
              if (!first)
                sql += ",";
              first = false;
              sql += quote(k) + "=" + bind(v);
            }
          } else if (change.kind == "delete")
            sql = "DELETE FROM " + qualified(s, t);
          else
            throw std::runtime_error("Invalid row operation");
          sql += " WHERE ";
          bool first = true;
          for (auto &c : cols) {
            if (!change.original.contains(c.name))
              throw std::runtime_error("Original row incomplete");
            if (!first)
              sql += " AND ";
            first = false;
            sql += "to_jsonb(" + quote(c.name) + ") IS NOT DISTINCT FROM to_jsonb(" +
                   bind(change.original.at(c.name)) + "::" + c.type + ")";
          }
        }
        sql += " RETURNING 1";
        auto result = all(sql, params);
        if (result.rows.size() != 1)
          throw std::runtime_error("Row changed or disappeared; reload before saving");
      }
      all(existing ? "RELEASE SAVEPOINT puff_edit" : "COMMIT");
    } catch (...) {
      try {
        all(existing ? "ROLLBACK TO SAVEPOINT puff_edit" : "ROLLBACK");
        if (existing)
          all("RELEASE SAVEPOINT puff_edit");
      } catch (...) {
      }
      throw;
    }
  }
  void create_database(const std::string &name) override {
    if (profile.readonly)
      throw std::runtime_error("Connection is read-only");
    all("CREATE DATABASE " + quote(name));
  }
  Page inspect(const std::string &s, const std::string &t) override {
    return execute(
        "SELECT 'constraint' AS kind,conname AS name,pg_get_constraintdef(oid) AS definition FROM "
        "pg_constraint WHERE conrelid=$1::regclass UNION ALL SELECT 'index',indexname,indexdef "
        "FROM pg_indexes WHERE schemaname=$2 AND tablename=$3",
        {qualified(s, t), s, t});
  }
};
} // namespace
std::unique_ptr<Driver> postgres_driver(const Profile &p, std::atomic_bool &c) {
  return std::make_unique<Postgres>(p, c);
}
} // namespace puff
