#include "core.hpp"
#include "session.hpp"
#include "store.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <sqlite3.h>
#include <sys/stat.h>
#include <thread>
using namespace puff;
namespace {
int checks = 0;
void require(bool value, const std::string &message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void throws(F fn, const std::string &part = "") {
  ++checks;
  try {
    fn();
  } catch (const std::exception &e) {
    if (part.empty() || std::string(e.what()).find(part) != std::string::npos)
      return;
    throw std::runtime_error("Unexpected error: " + std::string(e.what()));
  }
  throw std::runtime_error("Expected failure did not occur");
}
struct Temp {
  std::string path;
  Temp() {
    gchar *value = g_dir_make_tmp("puff-native-test-XXXXXX", nullptr);
    path = value;
    g_free(value);
  }
  ~Temp() {
    std::filesystem::remove_all(path);
  }
};
std::map<std::string, Cell> original(const Page &p, size_t row) {
  std::map<std::string, Cell> o;
  for (size_t c = 0; c < p.names.size(); ++c)
    o[p.names[c]] = p.rows[row][c];
  return o;
}
void unit() {
  require(quote("a\";DROP TABLE x") == "\"a\"\";DROP TABLE x\"", "Identifier escaping");
  for (auto s : {"", "x\0bad"}) {
    if (!std::string(s).empty())
      continue;
    throws([&] { quote(s); });
  }
  throws([] { quote(std::string(64, 'x')); });
  throws([] { type("text; DROP TABLE x"); });
  require(!needs_review("SELECT 1;"), "Simple read");
  require(!needs_review(std::string(900000, ' ') + "SELECT 1"),
          "Large SQL input uses bounded-stack review");
  throws([] { quote(std::string("a\0b", 3)); });

  for (auto sql :
       {"WITH x AS (DELETE FROM a RETURNING *) SELECT * FROM x", "DELETE FROM a",
        "SELECT 1;DROP TABLE a", "EXPLAIN ANALYZE DELETE FROM a", "SELECT * INTO x FROM y"})
    require(needs_review(sql), "Risk review");
  auto ddl = create_table_sql("public", "t", {{"a", "integer", true}, {"b", "text", true}});
  require(ddl.find("PRIMARY KEY (\"a\",\"b\")") != std::string::npos, "Composite key DDL");
  throws([] { create_table_sql("s", "t", {{"a", "text"}, {"a", "text"}}); });
  auto redacted = redact("password=abc postgres://armi:hidden@host secret", "secret");
  require(redacted.find("abc") == std::string::npos &&
              redacted.find("hidden") == std::string::npos &&
              redacted.find("secret") == std::string::npos,
          "Credential redaction");
  auto quoted_secret = redact("Password = 'abc''def' host=localhost", "");
  require(quoted_secret.find("abc") == std::string::npos &&
              quoted_secret.find("def") == std::string::npos,
          "Quoted credential redaction");
  Temp temp;
  {
    std::ofstream legacy(temp.path + "/connections.json");
    legacy
        << R"([{"id":"legacy","name":"Old","host":"localhost","port":5432,"user":"postgres","database":"postgres","ssl":true,"password":"nevercopy"}])";
  }
  Store store(temp.path);
  require(store.profiles().size() == 1, "Legacy migration");
  require(store.profiles()[0].tls == "verify-full" && store.profiles()[0].readonly,
          "Migration safety defaults");
  Profile p;
  p.id = "unit";
  p.password = "topsecret";
  store.save(p);
  std::ifstream file(store.filename());
  std::string contents((std::istreambuf_iterator<char>(file)), {});
  require(contents.find("topsecret") == std::string::npos &&
              contents.find("nevercopy") == std::string::npos,
          "No plaintext credential storage");
  struct stat statbuf{};
  stat(store.filename().c_str(), &statbuf);
  require((statbuf.st_mode & 0777) == 0600, "Private metadata file");
  for (int i = 0; i < 205; ++i)
    store.record(p.id, "SELECT " + std::to_string(i));
  require(store.history().size() == 200, "Bounded history");
  require(store.history("SELECT 204").size() == 1, "History search");
  store.snippet("example", "SELECT 1");
  require(store.snippets().size() == 1, "Snippet persistence");
  store.clear_history();
  require(store.history().empty(), "History deletion");
  store.remove("legacy");
  require(store.profiles().size() == 1, "Profile deletion");
  Store reopened(temp.path);
  require(reopened.profiles().size() == 1, "Deleted imported profile stays deleted after restart");
  auto descriptor_count = [] {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
  };
  auto descriptors = descriptor_count();
  for (int i = 0; i < 50; ++i)
    reopened.profiles();
  require(descriptor_count() == descriptors, "Metadata opens release file descriptors");
  Page page;
  page.names = {"same", "same"};
  page.rows = {{Cell("0.1234567890123456789012345"), std::nullopt},
               {Cell("a,b\nquote\""), Cell("text")}};
  export_page(temp.path + "/page.json", page, true);
  std::ifstream data(temp.path + "/page.json");
  std::string exported((std::istreambuf_iterator<char>(data)), {});
  require(exported.find("0.1234567890123456789012345") != std::string::npos &&
              exported.find("null") != std::string::npos,
          "Exact text export and null");
  export_page(temp.path + "/page.csv", page, false);
  auto session = Session::make(p);
  std::atomic_bool entered = false, gate = false;
  require(session->submit(
              [&](Session &) {
                entered = true;
                while (!gate && !session->cancelled)
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return Page{};
              },
              [](Page, std::string) {}),
          "Bounded job accepted");
  require(!session->submit([](Session &) { return Page{}; }, [](Page, std::string) {}),
          "Second job rejected");
  session->close();
  gate = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  require(!session->busy, "Worker cleanup");
}
void driver_test(const Profile &p) {
  std::atomic_bool cancel = false;
  auto db = connect(p, cancel);
  std::string schema = p.engine == "postgres" ? "public" : "main";
  auto target = qualified(schema, "puff_test");
  db->execute("DROP TABLE IF EXISTS " + target);
  db->execute(create_table_sql(
      schema, "puff_test", {{"a", "integer", true}, {"b", "text", true}, {"value", "numeric"}}));
  db->mutate(schema, "puff_test",
             {{"insert", {{"a", "1"}, {"b", "key"}, {"value", "0.1234567890123456789012345"}}, {}},
              {"insert", {{"a", "2"}, {"b", "key"}, {"value", "2"}}, {}}});
  auto page = db->browse(schema, "puff_test", 0);
  require(page.rows.size() == 2 && page.columns[0].primary && page.columns[1].primary,
          "Composite keys and browse");
  if (p.engine == "postgres")
    require(page.rows[0][2] == Cell("0.1234567890123456789012345"),
            "Exact numeric text, no float conversion");
  auto before = original(page, 0);
  db->mutate(schema, "puff_test", {{"update", {{"value", "3.5"}}, before}});
  throws([&] { db->mutate(schema, "puff_test", {{"delete", {}, before}}); }, "Row changed");
  before["value"] = "3.5";
  throws([&] {
    db->mutate(
        schema, "puff_test",
        {{"update", {{"value", "999"}}, before}, {"insert", {{"a", "2"}, {"b", "key"}}, {}}});
  });
  require(db->browse(schema, "puff_test", 0).rows[0][2] == Cell("3.5"), "Atomic batch rollback");
  db->execute("BEGIN");
  db->mutate(schema, "puff_test", {{"delete", {}, before}});
  db->execute("ROLLBACK");
  require(db->browse(schema, "puff_test", 0).rows.size() == 2,
          "Nested mutation transaction rollback");
  db->execute(schema_sql(schema, "puff_test", "add", "extra", "", "integer"));
  db->execute(schema_sql(schema, "puff_test", "rename", "extra", "renamed", "text"));
  if (p.engine == "postgres")
    db->execute(schema_sql(schema, "puff_test", "type", "renamed", "", "text"));
  db->execute(schema_sql(schema, "puff_test", "drop", "renamed", "", "text"));
  auto large = p.engine == "postgres" ? "SELECT n FROM generate_series(1,1000000) n"
                                      : "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 "
                                        "FROM n WHERE x<1000000) SELECT x FROM n";
  page = db->execute(large);
  require(page.rows.size() == 200 && page.more, "Bounded million-row result");
  require(db->fetch().rows[0][0] == Cell("201"), "Incremental fetch");
  db->release();
  require(db->execute("SELECT 42").rows[0][0] == Cell("42"),
          "Connection usable after releasing stream");
  throws([&] {
    db->execute(p.engine == "postgres" ? "SELECT repeat('x',300000)"
                                       : "SELECT printf('%300000s','x')");
  });
  require(db->execute("SELECT 43").rows[0][0] == Cell("43"), "Recovery after cell budget");
  throws([&] { db->execute("SELECT 1; SELECT 2"); });
  require(db->execute("SELECT 1").rows.size() == 1, "Recovery after multiple statements");
  std::string cancellation_error;
  std::thread query([&] {
    try {
      db->execute(p.engine == "postgres" ? "SELECT pg_sleep(20)"
                                         : "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 "
                                           "FROM n WHERE x<1000000000) SELECT sum(x) FROM n");
    } catch (const std::exception &e) {
      cancellation_error = e.what();
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  cancel = true;
  query.join();
  require(!cancellation_error.empty(), "Query cancellation");
  cancel = false;
  require(db->execute("SELECT 1").rows.size() == 1, "Recovery after cancellation");
  auto ro = p;
  ro.readonly = true;
  auto readonly = connect(ro, cancel);
  throws([&] { readonly->execute("DELETE FROM " + target); });
  if (p.engine == "postgres")
    throws([&] { readonly->create_database("puff_readonly_must_not_exist"); }, "read-only");
  readonly.reset();
  if (p.engine == "postgres") {
    db->execute("BEGIN");
    db->execute("SELECT n FROM generate_series(1,10000) n");
    require(db->in_transaction(), "Open transaction remains visible during a stream");
    db->release();
    db->execute("ROLLBACK");
    auto name = "puff_native_\"quoted";
    db->create_database(name);
    auto created_profile = p;
    created_profile.database = name;
    auto created_connection = connect(created_profile, cancel);
    require(text(created_connection->execute("SELECT current_database()").rows.at(0).at(0)) == name,
            "Connect to the newly created database with a quoted identifier");
    created_connection.reset();
    auto listed = db->databases();
    require(std::any_of(listed.rows.begin(), listed.rows.end(),
                        [&](const auto &row) { return row.at(0) == Cell(name); }),
            "Database discovery includes newly created quoted names");
    require(std::none_of(listed.rows.begin(), listed.rows.end(),
                         [](const auto &row) {
                           return row.at(0) == Cell("template0") || row.at(0) == Cell("template1");
                         }),
            "Database discovery hides template databases");
    throws([&] { db->create_database(name); });
    db->execute("DROP DATABASE " + quote(name));
    listed = db->databases();
    require(std::none_of(listed.rows.begin(), listed.rows.end(),
                         [&](const auto &row) { return row.at(0) == Cell(name); }),
            "Database discovery refresh removes dropped databases");
    db->create_database("test");
    auto sample_profile = p;
    sample_profile.database = "test";
    auto sample = connect(sample_profile, cancel);
    std::ifstream seed_file("sql/test-database-sample.sql");
    require(seed_file.good(), "Sample seed file exists");
    std::string seed((std::istreambuf_iterator<char>(seed_file)), std::istreambuf_iterator<char>());
    sample->execute(seed);
    require(sample->execute("SELECT count(*) FROM puff_sample.customers").rows.at(0).at(0) ==
                Cell("1000"),
            "Entire sample DO block executes through the application's driver");
    throws([&] { sample->execute(seed); }, "already exists");
    throws([&] { db->execute(seed); }, "Connect to database test first");
    sample.reset();
    db->execute("DROP DATABASE test");
    db->execute("SET statement_timeout='100ms'");
    throws([&] { db->execute("SELECT pg_sleep(2)"); }, "57014");
    db->execute("SET statement_timeout='30s'");
    db->execute("CREATE ROLE puff_auth LOGIN PASSWORD 'correct'");
    auto bad = p;
    bad.host = "127.0.0.1";
    bad.user = "puff_auth";
    bad.password = "incorrect";
    throws([&] { connect(bad, cancel); }, "password authentication failed");
    auto restricted = p;
    restricted.user = "puff_auth";
    restricted.password = "correct";
    auto limited = connect(restricted, cancel);
    throws([&] { limited->create_database("puff_permission_must_not_exist"); }, "42501");
    require(limited->execute("SELECT 1").rows.size() == 1,
            "Connection recovers after denied creation");
    db->create_database("puff_hidden");
    db->execute("REVOKE CONNECT ON DATABASE puff_hidden FROM PUBLIC");
    auto restricted_databases = limited->databases();
    require(std::none_of(restricted_databases.rows.begin(), restricted_databases.rows.end(),
                         [](const auto &row) { return row.at(0) == Cell("puff_hidden"); }),
            "Database discovery respects CONNECT privileges");
    db->execute("DROP DATABASE puff_hidden");
    limited.reset();
    db->execute("DROP ROLE puff_auth");
    bad = p;
    bad.host = "127.0.0.1";
    bad.tls = "require";
    throws([&] { connect(bad, cancel); }, "SSL");
    bad = p;
    bad.host = "127.0.0.1";
    bad.port = 1;
    throws([&] { connect(bad, cancel); });
    Temp temp;
    backup(p, temp.path + "/archive.dump", false, cancel);
    require(std::filesystem::file_size(temp.path + "/archive.dump") > 0, "Backup archive output");
    db->create_database("puff_restore");
    auto restored = p;
    restored.database = "puff_restore";
    backup(restored, temp.path + "/archive.dump", true, cancel);
    auto other = connect(restored, cancel);
    require(other->browse("public", "puff_test", 0).rows.size() == 2, "Backup restore round trip");
    throws([&] { backup(restored, temp.path + "/archive.dump", true, cancel); });
    require(other->browse("public", "puff_test", 0).rows.size() == 2,
            "Restore collision rolls back");
    other.reset();
    db->execute("DROP DATABASE puff_restore");
  }
  db->execute("DROP TABLE " + target);
}
} // namespace
int main() {
  try {
    if (g_getenv("PUFF_TEST_SECRET_FAILURE")) {
      throws([] { secret_lookup("puff-unavailable-service-test"); },
             "Desktop secret service unavailable or locked");
      std::cout << "PASS unavailable Secret Service: " << checks << " check\n";
      return 0;
    }
    unit();
    Temp temp;
    std::string path = temp.path + "/test.sqlite";
    sqlite3 *db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_close(db);
    Profile p;
    p.engine = "sqlite";
    p.database = path;
    p.readonly = false;
    driver_test(p);
    std::cout << "PASS unit + SQLite: " << checks << " checks\n";
    if (auto *host = g_getenv("PUFF_TEST_PG")) {
      p = Profile{};
      p.host = host;
      p.port = 55439;
      p.user = g_get_user_name();
      p.tls = "disable";
      p.readonly = false;
      driver_test(p);
      std::cout << "PASS PostgreSQL integration: " << checks << " cumulative checks\n";
    } else
      std::cout << "SKIP PostgreSQL: set PUFF_TEST_PG using scripts/test-native-postgres.sh\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
    return 1;
  }
}
