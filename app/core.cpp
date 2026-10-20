#include "core.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
namespace puff {
void Profile::validate() const {
  if (name.empty() || database.empty() || timeout < 1 || timeout > 3600)
    throw std::runtime_error("Name/database required; timeout must be 1–3600 seconds");
  if (engine != "postgres" && engine != "sqlite")
    throw std::runtime_error("Unsupported driver");
  if (engine == "postgres" && (host.empty() || user.empty() || port < 1 || port > 65535))
    throw std::runtime_error("Check host, user and port");
  if (tls != "disable" && tls != "prefer" && tls != "require" && tls != "verify-full")
    throw std::runtime_error("Invalid TLS mode");
  if (environment != "development" && environment != "staging" && environment != "production")
    throw std::runtime_error("Invalid environment");
}
std::string quote(const std::string &value) {
  if (value.empty() || value.size() > 63 || value.find('\0') != std::string::npos)
    throw std::runtime_error("Identifier must contain 1–63 bytes without NUL");
  std::string out = "\"";
  for (char c : value) {
    out += c;
    if (c == '"')
      out += '"';
  }
  return out + '"';
}
std::string qualified(const std::string &s, const std::string &t) {
  return quote(s) + '.' + quote(t);
}
std::string type(const std::string &value) {
  static const std::vector<std::string> allowed = {"text",      "varchar(255)", "integer", "bigint",
                                                   "numeric",   "boolean",      "uuid",    "date",
                                                   "timestamp", "timestamptz",  "jsonb",   "bytea"};
  if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
    throw std::runtime_error("Unsupported data type");
  return value;
}
std::string schema_sql(const std::string &s, const std::string &t, const std::string &a,
                       const std::string &c, const std::string &n, const std::string &ty) {
  std::string base = "ALTER TABLE " + qualified(s, t);
  if (a == "add")
    return base + " ADD COLUMN " + quote(c) + ' ' + type(ty);
  if (a == "rename")
    return base + " RENAME COLUMN " + quote(c) + " TO " + quote(n);
  if (a == "drop")
    return base + " DROP COLUMN " + quote(c);
  if (a == "type")
    return base + " ALTER COLUMN " + quote(c) + " TYPE " + type(ty) + " USING " + quote(c) +
           "::" + type(ty);
  throw std::runtime_error("Unsupported column operation");
}
std::string create_table_sql(const std::string &s, const std::string &t,
                             const std::vector<Column> &cols) {
  if (cols.empty() || cols.size() > 100)
    throw std::runtime_error("Specify 1–100 columns");
  std::map<std::string, bool> seen;
  std::string out = "CREATE TABLE " + qualified(s, t) + " (", keys;
  for (const auto &c : cols) {
    if (seen[c.name])
      throw std::runtime_error("Duplicate column name");
    seen[c.name] = true;
    if (out.back() != '(')
      out += ", ";
    out += quote(c.name) + ' ' + type(c.type) + (c.nullable ? "" : " NOT NULL");
    if (c.primary) {
      if (!keys.empty())
        keys += ",";
      keys += quote(c.name);
    }
  }
  if (!keys.empty())
    out += ", PRIMARY KEY (" + keys + ")";
  return out + ")";
}
bool needs_review(const std::string &sql) {
  // Conservative linear scan, not an authorization boundary or full SQL parser.
  // Avoid recursive std::regex evaluation on large editor buffers.
  std::string_view input(sql);
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
    input.remove_suffix(1);
  if (!input.empty() && input.back() == ';')
    input.remove_suffix(1);
  size_t i = 0;
  auto word = [&] {
    std::string value;
    while (i < input.size() &&
           (std::isalnum(static_cast<unsigned char>(input[i])) || input[i] == '_')) {
      if (value.size() < 64)
        value += static_cast<char>(std::toupper(static_cast<unsigned char>(input[i])));
      ++i;
    }
    return value;
  };
  while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i])))
    ++i;
  auto first = word();
  if (first != "SELECT" && first != "SHOW" && first != "VALUES" && first != "EXPLAIN")
    return true;
  static constexpr std::string_view blocked[] = {"DROP",   "TRUNCATE", "DELETE",  "UPDATE",
                                                 "INSERT", "ALTER",    "CREATE",  "COPY",
                                                 "CALL",   "DO",       "ANALYZE", "INTO"};
  while (i < input.size()) {
    char c = input[i];
    if (c == ';' || c == '$' || (c == '-' && i + 1 < input.size() && input[i + 1] == '-') ||
        (c == '/' && i + 1 < input.size() && input[i + 1] == '*'))
      return true;
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      auto token = word();
      for (auto danger : blocked)
        if (token == danger)
          return true;
    } else
      ++i;
  }
  return false;
}
std::string redact(std::string message, const std::string &secret) {
  if (!secret.empty()) {
    size_t p = 0;
    while ((p = message.find(secret, p)) != std::string::npos) {
      message.replace(p, secret.size(), "[redacted]");
      p += 10;
    }
  }
  auto lower = [](const std::string &text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
  };
  std::string normalized = lower(message);
  size_t position = 0;
  while ((position = normalized.find("password", position)) != std::string::npos) {
    size_t start = position + 8;
    while (start < message.size() && std::isspace(static_cast<unsigned char>(message[start])))
      ++start;
    if (start == message.size() || (message[start] != '=' && message[start] != ':')) {
      position += 8;
      continue;
    }
    ++start;
    while (start < message.size() && std::isspace(static_cast<unsigned char>(message[start])))
      ++start;
    size_t end = start;
    if (end < message.size() && (message[end] == '\'' || message[end] == '"')) {
      char delimiter = message[end++];
      while (end < message.size()) {
        if (message[end] == '\\' && end + 1 < message.size()) {
          end += 2;
          continue;
        }
        if (message[end] == delimiter) {
          ++end;
          if (end < message.size() && message[end] == delimiter) {
            ++end;
            continue;
          }
          break;
        }
        ++end;
      }
    } else
      while (end < message.size() && !std::isspace(static_cast<unsigned char>(message[end])))
        ++end;
    message.replace(start, end - start, "[redacted]");
    normalized = lower(message);
    position = start + 10;
  }
  for (const std::string prefix : {"postgres://", "postgresql://"}) {
    position = 0;
    while ((position = normalized.find(prefix, position)) != std::string::npos) {
      size_t start = position + prefix.size(), end = start;
      while (end < message.size() && !std::isspace(static_cast<unsigned char>(message[end])) &&
             message[end] != '@')
        ++end;
      size_t colon = message.find(':', start);
      if (end < message.size() && message[end] == '@' && colon < end) {
        message.replace(colon + 1, end - colon - 1, "[redacted]");
        normalized = lower(message);
        position = colon + 11;
      } else
        position = start;
    }
  }
  return message;
}
std::string text(const Cell &value) {
  return value.value_or("NULL");
}
void Driver::create_database(const std::string &) {
  throw std::runtime_error("Database creation is PostgreSQL-only");
}
std::unique_ptr<Driver> connect(const Profile &p, std::atomic_bool &cancel) {
  p.validate();
  return p.engine == "postgres" ? postgres_driver(p, cancel) : sqlite_driver(p, cancel);
}
} // namespace puff
