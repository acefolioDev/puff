#include "core.hpp"
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
namespace puff {
namespace {
struct TempFile {
  std::string path;
  explicit TempFile(const std::string &dir, const std::string &prefix) {
    std::string pattern = dir + "/" + prefix + "XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back(0);
    int fd = mkstemp(buffer.data());
    if (fd < 0)
      throw std::runtime_error("Cannot create private temporary file");
    fchmod(fd, 0600);
    close(fd);
    path = buffer.data();
  }
  ~TempFile() {
    if (!path.empty())
      unlink(path.c_str());
  }
};
std::string escaped(std::string s) {
  std::string out;
  for (char c : s) {
    if (c == '\n' || c == '\r')
      throw std::runtime_error("Newlines are not supported in pgpass fields");
    if (c == ':' || c == '\\')
      out += '\\';
    out += c;
  }
  return out;
}
std::string json_string(const Cell &v) {
  if (!v)
    return "null";
  std::string out = "\"";
  for (unsigned char c : *v) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 32) {
        const char *hex = "0123456789abcdef";
        out += "\\u00";
        out += hex[c >> 4];
        out += hex[c & 15];
      } else
        out += c;
    }
  }
  return out + '"';
}
void run(const std::vector<std::string> &args, const Profile &p, const std::string &passfile,
         std::atomic_bool &cancel) {
  GError *err = nullptr;
  auto launcher =
      g_subprocess_launcher_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE));
  gchar **env = g_get_environ();
  std::vector<std::string> remove;
  for (size_t i = 0; env[i]; ++i)
    if (g_str_has_prefix(env[i], "PG")) {
      std::string value = env[i];
      remove.push_back(value.substr(0, value.find('=')));
    }
  for (auto &key : remove)
    env = g_environ_unsetenv(env, key.c_str());
  g_subprocess_launcher_set_environ(launcher, env);
  g_strfreev(env);
  g_subprocess_launcher_setenv(launcher, "PGPASSFILE", passfile.c_str(), TRUE);
  g_subprocess_launcher_setenv(launcher, "PGSSLMODE", p.tls.c_str(), TRUE);
  g_subprocess_launcher_setenv(launcher, "PGCONNECT_TIMEOUT", "5", TRUE);
  // Route stderr to a private file to bound in-memory diagnostic output.
  TempFile errors(g_get_tmp_dir(), "puff-errors-");
  int fd = open(errors.path.c_str(), O_WRONLY | O_TRUNC);
  if (fd < 0) {
    g_object_unref(launcher);
    throw std::runtime_error("Cannot open diagnostic file");
  }
  g_subprocess_launcher_take_stderr_fd(launcher, fd);
  std::vector<const char *> argv;
  for (auto &arg : args)
    argv.push_back(arg.c_str());
  argv.push_back(nullptr);
  auto process = g_subprocess_launcher_spawnv(launcher, argv.data(), &err);
  g_object_unref(launcher);
  if (!process) {
    std::string e = err->message;
    g_error_free(err);
    throw std::runtime_error(e);
  }
  auto cancellable = g_cancellable_new();
  std::atomic_bool finished = false;
  std::thread monitor([&] {
    auto start = std::chrono::steady_clock::now();
    while (!finished) {
      if (cancel || std::chrono::steady_clock::now() - start > std::chrono::minutes(10)) {
        g_subprocess_force_exit(process);
        g_cancellable_cancel(cancellable);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  bool ok = g_subprocess_wait_check(process, cancellable, &err);
  finished = true;
  monitor.join();
  if (!ok) {
    g_subprocess_force_exit(process);
    g_subprocess_wait(process, nullptr, nullptr);
  }
  g_object_unref(cancellable);
  g_object_unref(process);
  if (!ok) {
    std::ifstream file(errors.path);
    std::string detail(8192, '\0');
    file.read(detail.data(), detail.size());
    detail.resize(file.gcount());
    std::string e = cancel ? "Operation cancelled; uncommitted restore transaction rolls back"
                    : detail.empty() ? (err ? err->message : "Backup utility failed")
                                     : detail;
    if (err)
      g_error_free(err);
    throw std::runtime_error(redact(e, p.password));
  }
}
std::string tool(const char *name) {
  gchar *path = g_find_program_in_path(name);
  if (!path)
    throw std::runtime_error(std::string("Install PostgreSQL client tools: ") + name + " missing");
  std::string out = path;
  g_free(path);
  return out;
}
} // namespace
void export_page(const std::string &destination, const Page &p, bool json) {
  auto parent = std::filesystem::path(destination).parent_path();
  TempFile temp(parent.empty() ? "." : parent.string(), ".puff-export-");
  {
    std::ofstream out(temp.path);
    if (!out)
      throw std::runtime_error("Cannot write export");
    if (json) {
      out << "{\"columns\":[";
      bool first = true;
      for (auto &name : p.names) {
        if (!first)
          out << ',';
        first = false;
        out << json_string(name);
      }
      out << "],\"rows\":[";
      first = true;
      for (auto &row : p.rows) {
        if (!first)
          out << ',';
        first = false;
        out << '[';
        bool fc = true;
        for (auto &value : row) {
          if (!fc)
            out << ',';
          fc = false;
          out << json_string(value);
        }
        out << ']';
      }
      out << "]}";
    } else {
      auto cell = [&](const Cell &v) {
        out << '"';
        if (v)
          for (char c : *v) {
            out << c;
            if (c == '"')
              out << '"';
          }
        out << '"';
      };
      for (size_t i = 0; i < p.names.size(); ++i) {
        if (i)
          out << ',';
        cell(p.names[i]);
      }
      out << '\n';
      for (auto &row : p.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
          if (i)
            out << ',';
          cell(row[i]);
        }
        out << '\n';
      }
    }
    out.flush();
    if (!out)
      throw std::runtime_error("Export failed while writing data");
  }
  if (rename(temp.path.c_str(), destination.c_str()) != 0)
    throw std::runtime_error("Cannot publish export file");
}
void backup(const Profile &p, const std::string &destination, bool restore,
            std::atomic_bool &cancel) {
  if (p.engine != "postgres")
    throw std::runtime_error("Backup/restore currently supports PostgreSQL custom archives");
  if (restore && p.readonly)
    throw std::runtime_error("Connection is read-only");
  TempFile auth(g_get_tmp_dir(), "puff-auth-");
  {
    std::ofstream out(auth.path);
    out << escaped(p.host) << ':' << p.port << ':' << escaped(p.database) << ':' << escaped(p.user)
        << ':' << escaped(p.password) << '\n';
    if (!out)
      throw std::runtime_error("Cannot write private authentication file");
  }
  std::vector<std::string> common = {"-h", p.host, "-p", std::to_string(p.port),
                                     "-U", p.user, "-d", p.database};
  if (restore) {
    run({tool("pg_restore"), "--list", destination}, p, auth.path, cancel);
    std::vector<std::string> args = {tool("pg_restore"), "--single-transaction", "--exit-on-error",
                                     "--no-owner"};
    args.insert(args.end(), common.begin(), common.end());
    args.push_back(destination);
    run(args, p, auth.path, cancel);
  } else {
    auto parent = std::filesystem::path(destination).parent_path();
    TempFile out(parent.empty() ? "." : parent.string(), ".puff-backup-");
    std::vector<std::string> args = {tool("pg_dump"), "--format=custom", "--compress=6", "--file",
                                     out.path};
    args.insert(args.end(), common.begin(), common.end());
    run(args, p, auth.path, cancel);
    run({tool("pg_restore"), "--list", out.path}, p, auth.path, cancel);
    if (std::filesystem::file_size(out.path) == 0)
      throw std::runtime_error("Empty backup archive");
    if (rename(out.path.c_str(), destination.c_str()) != 0)
      throw std::runtime_error("Cannot publish backup archive");
  }
}
} // namespace puff
