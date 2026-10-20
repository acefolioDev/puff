#include "store.hpp"
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <glib.h>
#ifdef __APPLE__
#include <Security/Security.h>
#else
#include <libsecret/secret.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
namespace puff {
namespace {
struct DB {
  sqlite3 *db = nullptr;
  explicit DB(const std::string &p) {
    if (sqlite3_open(p.c_str(), &db) != SQLITE_OK) {
      std::string e = sqlite3_errmsg(db);
      sqlite3_close(db);
      throw std::runtime_error(e);
    }
    sqlite3_busy_timeout(db, 1000);
  }
  ~DB() {
    sqlite3_close(db);
  }
};
struct Statement {
  sqlite3_stmt *s = nullptr;
  sqlite3 *db;
  Statement(sqlite3 *d, const std::string &q, const std::vector<Cell> &params = {}) : db(d) {
    if (sqlite3_prepare_v2(d, q.c_str(), -1, &s, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(d));
    for (size_t i = 0; i < params.size(); ++i) {
      if (params[i])
        sqlite3_bind_text(s, i + 1, params[i]->c_str(), params[i]->size(), SQLITE_TRANSIENT);
      else
        sqlite3_bind_null(s, i + 1);
    }
  }
  ~Statement() {
    sqlite3_finalize(s);
  }
  int step() {
    int c = sqlite3_step(s);
    if (c != SQLITE_ROW && c != SQLITE_DONE)
      throw std::runtime_error(sqlite3_errmsg(db));
    return c;
  }
  std::string value(int c) {
    auto *p = sqlite3_column_text(s, c);
    return p ? reinterpret_cast<const char *>(p) : "";
  }
};
void command(sqlite3 *d, const std::string &q, const std::vector<Cell> &p = {}) {
  Statement s(d, q, p);
  s.step();
}
#ifndef __APPLE__
const SecretSchema schema = {
    "io.github.puff.Connection",
    SECRET_SCHEMA_NONE,
    {{"profile", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    0,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr};
void check_secret(GError *e) {
  if (e) {
    std::string s = "Desktop secret service unavailable or locked: " + std::string(e->message);
    g_error_free(e);
    throw std::runtime_error(s);
  }
}
#endif
} // namespace
Store::Store(std::string root) {
  if (root.empty())
    root = std::string(g_get_user_config_dir()) + "/puff";
  std::filesystem::create_directories(root);
  chmod(root.c_str(), 0700);
  path = root + "/workspace.sqlite3";
  int fd = open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw std::runtime_error("Cannot open private configuration database");
  fchmod(fd, 0600);
  close(fd);
  DB db(path);
  command(db.db, "CREATE TABLE IF NOT EXISTS native_profiles(id TEXT PRIMARY KEY,name TEXT,engine "
                 "TEXT,host TEXT,port INTEGER,username TEXT,dbname TEXT,tls TEXT,environment "
                 "TEXT,project TEXT,readonly INTEGER,timeout INTEGER,remember INTEGER)");
  command(db.db, "CREATE TABLE IF NOT EXISTS native_history(id INTEGER PRIMARY KEY,profile "
                 "TEXT,sql TEXT,created TEXT DEFAULT CURRENT_TIMESTAMP)");
  command(db.db, "CREATE TABLE IF NOT EXISTS native_snippets(name TEXT PRIMARY KEY,sql TEXT)");
  command(db.db, "CREATE TABLE IF NOT EXISTS native_settings(key TEXT PRIMARY KEY,value TEXT)");
  Statement migrated(db.db, "SELECT 1 FROM native_settings WHERE key='legacy_imported'");
  bool already_imported = migrated.step() == SQLITE_ROW;
  // Preserve the original JSON file. Use SQLite's JSON parser without a new dependency.
  std::ifstream old(root + "/connections.json");
  if (old && !already_imported) {
    std::string json((std::istreambuf_iterator<char>(old)), {});
    if (json.size() > 1000000)
      throw std::runtime_error("Legacy profile file exceeds 1 MiB");
    Statement rows(db.db,
                   "SELECT "
                   "json_extract(value,'$.id'),json_extract(value,'$.name'),json_extract(value,'$."
                   "host'),json_extract(value,'$.port'),json_extract(value,'$.user'),json_extract("
                   "value,'$.database'),json_extract(value,'$.ssl') FROM json_each(?) LIMIT 200",
                   {json});
    while (rows.step() == SQLITE_ROW) {
      Profile p;
      p.id = rows.value(0);
      p.name = rows.value(1);
      p.host = rows.value(2);
      p.port = std::stoi(rows.value(3));
      p.user = rows.value(4);
      p.database = rows.value(5);
      p.tls = rows.value(6) == "1" ? "verify-full" : "disable";
      p.validate();
      command(db.db, "INSERT OR IGNORE INTO native_profiles VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
              {p.id, p.name, p.engine, p.host, std::to_string(p.port), p.user, p.database, p.tls,
               p.environment, p.group, "1", std::to_string(p.timeout), "0"});
    }
    command(db.db, "INSERT OR REPLACE INTO native_settings VALUES ('legacy_imported','1')");
  }
}
std::vector<Profile> Store::profiles() const {
  DB db(path);
  Statement s(db.db, "SELECT * FROM native_profiles ORDER BY project,name LIMIT 200");
  std::vector<Profile> out;
  while (s.step() == SQLITE_ROW) {
    Profile p;
    p.id = s.value(0);
    p.name = s.value(1);
    p.engine = s.value(2);
    p.host = s.value(3);
    p.port = std::stoi(s.value(4));
    p.user = s.value(5);
    p.database = s.value(6);
    p.tls = s.value(7);
    p.environment = s.value(8);
    p.group = s.value(9);
    p.readonly = s.value(10) == "1";
    p.timeout = std::stoi(s.value(11));
    p.remember = s.value(12) == "1";
    out.push_back(std::move(p));
  }
  return out;
}
void Store::save(const Profile &p) const {
  p.validate();
  DB db(path);
  command(db.db, "INSERT OR REPLACE INTO native_profiles VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
          {p.id, p.name, p.engine, p.host, std::to_string(p.port), p.user, p.database, p.tls,
           p.environment, p.group, p.readonly ? "1" : "0", std::to_string(p.timeout),
           p.remember ? "1" : "0"});
}
void Store::remove(const std::string &id) const {
  DB db(path);
  command(db.db, "DELETE FROM native_profiles WHERE id=?", {id});
  command(db.db, "DELETE FROM native_history WHERE profile=?", {id});
}
std::vector<std::pair<std::string, std::string>> Store::history(const std::string &search) const {
  DB db(path);
  Statement s(db.db,
              "SELECT created,sql FROM native_history WHERE sql LIKE ? ORDER BY id DESC LIMIT 200",
              {"%" + search + "%"});
  std::vector<std::pair<std::string, std::string>> out;
  while (s.step() == SQLITE_ROW)
    out.emplace_back(s.value(0), s.value(1));
  return out;
}
void Store::record(const std::string &id, const std::string &sql) const {
  DB db(path);
  command(db.db, "INSERT INTO native_history(profile,sql) VALUES (?,?)",
          {id, sql.substr(0, 100000)});
  command(db.db, "DELETE FROM native_history WHERE id NOT IN (SELECT id FROM native_history ORDER "
                 "BY id DESC LIMIT 200)");
}
void Store::clear_history() const {
  DB db(path);
  command(db.db, "DELETE FROM native_history");
}
void Store::snippet(const std::string &name, const std::string &sql) const {
  DB db(path);
  Statement n(db.db, "SELECT COUNT(*) FROM native_snippets");
  n.step();
  if (std::stoi(n.value(0)) >= 100)
    throw std::runtime_error("Snippet limit is 100");
  command(db.db, "INSERT OR REPLACE INTO native_snippets VALUES (?,?)",
          {name.substr(0, 100), sql.substr(0, 100000)});
}
std::vector<std::pair<std::string, std::string>> Store::snippets() const {
  DB db(path);
  Statement s(db.db, "SELECT name,sql FROM native_snippets ORDER BY name LIMIT 100");
  std::vector<std::pair<std::string, std::string>> out;
  while (s.step() == SQLITE_ROW)
    out.emplace_back(s.value(0), s.value(1));
  return out;
}
#ifndef __APPLE__
std::string secret_lookup(const std::string &id) {
  GError *err = nullptr;
  auto *s = secret_password_lookup_sync(&schema, nullptr, &err, "profile", id.c_str(), nullptr);
  check_secret(err);
  std::string value = s ? s : "";
  if (s)
    secret_password_free(s);
  return value;
}
void secret_save(const Profile &p) {
  GError *err = nullptr;
  bool ok = secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT,
                                       ("My Puff: " + p.name).c_str(), p.password.c_str(), nullptr,
                                       &err, "profile", p.id.c_str(), nullptr);
  check_secret(err);
  if (!ok)
    throw std::runtime_error("Desktop secret service did not save the credential");
}
void secret_remove(const std::string &id) {
  GError *err = nullptr;
  secret_password_clear_sync(&schema, nullptr, &err, "profile", id.c_str(), nullptr);
  check_secret(err);
}
#else
namespace {
struct KeychainQuery {
  CFMutableDictionaryRef value = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  explicit KeychainQuery(const std::string &id) {
    CFDictionarySetValue(value, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(value, kSecAttrService, CFSTR("io.github.puff.Connection"));
    auto account = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(id.data()),
                                           id.size(), kCFStringEncodingUTF8, false);
    if (!account) {
      CFRelease(value);
      throw std::runtime_error("Invalid credential profile identifier");
    }
    CFDictionarySetValue(value, kSecAttrAccount, account);
    CFRelease(account);
  }
  ~KeychainQuery() {
    CFRelease(value);
  }
};
void keychain_check(OSStatus status) {
  if (status != errSecSuccess)
    throw std::runtime_error("macOS Keychain operation failed (status " + std::to_string(status) +
                             "). Unlock your login keychain and allow My Puff access.");
}
} // namespace
std::string secret_lookup(const std::string &id) {
  KeychainQuery query(id);
  CFDictionarySetValue(query.value, kSecReturnData, kCFBooleanTrue);
  CFTypeRef result = nullptr;
  auto status = SecItemCopyMatching(query.value, &result);
  if (status == errSecItemNotFound)
    return "";
  keychain_check(status);
  auto bytes = static_cast<CFDataRef>(result);
  std::string password(reinterpret_cast<const char *>(CFDataGetBytePtr(bytes)),
                       CFDataGetLength(bytes));
  CFRelease(result);
  return password;
}
void secret_save(const Profile &p) {
  KeychainQuery query(p.id);
  auto bytes =
      CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(p.password.data()), p.password.size());
  auto attributes = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(attributes, kSecValueData, bytes);
  auto status = SecItemUpdate(query.value, attributes);
  if (status == errSecItemNotFound) {
    CFDictionarySetValue(query.value, kSecValueData, bytes);
    status = SecItemAdd(query.value, nullptr);
  }
  CFRelease(attributes);
  CFRelease(bytes);
  keychain_check(status);
}
void secret_remove(const std::string &id) {
  KeychainQuery query(id);
  auto status = SecItemDelete(query.value);
  if (status != errSecItemNotFound)
    keychain_check(status);
}
#endif
} // namespace puff
