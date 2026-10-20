#pragma once
#include "core.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
namespace puff {
class Session : public std::enable_shared_from_this<Session> {
  struct Job {
    std::function<Page(Session &)> work;
    std::function<void(Page, std::string)> done;
  };
  std::mutex mutex;
  std::condition_variable wake;
  std::optional<Job> pending;
  std::thread worker;
  void loop();

public:
  Profile profile;
  std::unique_ptr<Driver> driver;
  std::atomic_bool cancelled = false, closed = false, busy = false;
  std::atomic_bool transaction = false;
  explicit Session(Profile p) : profile(std::move(p)) {}
  ~Session();
  static std::shared_ptr<Session> make(Profile);
  bool submit(std::function<Page(Session &)>, std::function<void(Page, std::string)>);
  void cancel() {
    cancelled = true;
  }
  void close();
  void wait() {
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
      worker.join();
  }
};
} // namespace puff
