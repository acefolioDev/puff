#pragma once
#include "core.hpp"
#include <sqlite3.h>
namespace puff {
class Store {
  std::string path;

public:
  explicit Store(std::string root = "");
  std::vector<Profile> profiles() const;
  void save(const Profile &) const;
  void remove(const std::string &) const;
  std::vector<std::pair<std::string, std::string>> history(const std::string &search = "") const;
  void record(const std::string &, const std::string &) const;
  void clear_history() const;
  void snippet(const std::string &, const std::string &) const;
  std::vector<std::pair<std::string, std::string>> snippets() const;
  const std::string &filename() const {
    return path;
  }
};
std::string secret_lookup(const std::string &);
void secret_save(const Profile &);
void secret_remove(const std::string &);
} // namespace puff
