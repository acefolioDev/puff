#include "session.hpp"
#include <glib.h>
namespace puff {
std::shared_ptr<Session> Session::make(Profile p) {
  auto s = std::make_shared<Session>(std::move(p));
  s->worker = std::thread([s] { s->loop(); });
  return s;
}
Session::~Session() {
  if (worker.joinable()) {
    if (worker.get_id() == std::this_thread::get_id())
      worker.detach();
    else
      worker.join();
  }
  std::fill(profile.password.begin(), profile.password.end(), '\0');
}
bool Session::submit(std::function<Page(Session &)> work,
                     std::function<void(Page, std::string)> done) {
  std::lock_guard lock(mutex);
  if (closed || busy)
    return false;
  busy = true;
  cancelled = false;
  pending = Job{std::move(work), std::move(done)};
  wake.notify_one();
  return true;
}
void Session::close() {
  closed = true;
  cancelled = true;
  wake.notify_one();
}
void Session::loop() {
  while (true) {
    Job job;
    {
      std::unique_lock lock(mutex);
      wake.wait(lock, [&] { return closed || pending.has_value(); });
      if (closed)
        break;
      job = std::move(*pending);
      pending.reset();
    }
    Page result;
    std::string error;
    try {
      result = job.work(*this);
      transaction = driver && driver->in_transaction();
    } catch (const std::exception &e) {
      error = redact(e.what(), profile.password);
      transaction = driver && driver->in_transaction();
    }
    if (closed)
      break;
    auto callback =
        new std::function<void()>([self = shared_from_this(), done = std::move(job.done),
                                   result = std::move(result), error = std::move(error)]() mutable {
          self->busy = false;
          if (!self->closed)
            done(std::move(result), std::move(error));
        });
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer p) -> gboolean {
          (*static_cast<std::function<void()> *>(p))();
          return G_SOURCE_REMOVE;
        },
        callback, [](gpointer p) { delete static_cast<std::function<void()> *>(p); });
  }
  driver.reset();
  std::fill(profile.password.begin(), profile.password.end(), '\0');
  profile.password.clear();
  busy = false;
}
} // namespace puff
