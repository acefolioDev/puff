#pragma once
#include "session.hpp"
#include "store.hpp"
#include <adwaita.h>
#include <gtksourceview/gtksource.h>
#include <set>
namespace puff {
class Application;
class Workspace : public std::enable_shared_from_this<Workspace> {
  Application &app;
  std::shared_ptr<Session> session, catalog;
  GtkWidget *root = nullptr, *schema_box = nullptr, *status = nullptr, *editor = nullptr,
            *run_button = nullptr, *cancel_button = nullptr, *next_button = nullptr,
            *prev_button = nullptr, *save_button = nullptr, *table_label = nullptr,
            *history_toggle = nullptr, *filter_entry = nullptr, *database_selector = nullptr,
            *database_status = nullptr, *database_refresh = nullptr;
  GtkSourceBuffer *buffer = nullptr;
  GtkSourceCompletionWords *words = nullptr;
  GtkSourceBuffer *word_buffer = nullptr;
  GListStore *rows = nullptr;
  GtkMultiSelection *selection;
  GtkColumnView *view;
  GtkCustomFilter *filter;
  GtkSortListModel *sorter;
  guint filter_source = 0;
  Page page;
  std::map<size_t, std::map<std::string, Cell>> edits;
  std::optional<std::pair<std::string, std::string>> table;
  size_t offset = 0;
  std::vector<std::string> schemas;
  std::set<std::string> completions;
  bool disposed = false, loading_databases = false;
  std::vector<std::string> database_names;
  void databases_load();
  void buttons();
  void display(Page);
  void browse(const std::string &, const std::string &, size_t = 0);
  void edit(size_t, size_t);
  void job(std::function<Page(Session &)>, std::function<void(Page)> = {});
  void safe(std::function<void()>);
  void write_review(const std::string &, const std::string &, std::function<void()>);
  void schemas_load();
  void schema_expand(GtkExpander *, const std::string &);
  void schema_dialog(bool);
  void insert();
  void save();
  void erase();
  void discard();
  void create_database();
  void inspect();
  void file(bool, bool);
  void backup_file(bool);
  void history();
  void snippet();
  void copy();
  void close_result();
  void add_words(const std::vector<std::string> &);
  std::vector<size_t> selected();
  std::string sql_text();
  void clear();

public:
  Workspace(Application &, std::shared_ptr<Session>);
  ~Workspace();
  void build();
  GtkWidget *widget() {
    return root;
  }
  const Profile &profile() {
    return session->profile;
  }
  void run();
  void execute(const std::string &);
  void next();
  void previous();
  void cancel();
  void dispose();
  bool dirty() const {
    return !edits.empty();
  }
  bool busy() const {
    return session->busy;
  }
  bool transaction() const {
    return session->transaction;
  }
  void exercise(std::function<void(bool, std::string)>); // Real-database UI integration scenario,
                                                         // only invoked by --self-test.
  friend class Application;
};
class Application {
  AdwApplication *application;
  GtkWindow *window = nullptr;
  AdwTabView *tabs = nullptr;
  GtkStack *stack = nullptr;
  std::unique_ptr<Store> store;
  std::vector<std::shared_ptr<Workspace>> workspaces;
  std::vector<std::shared_ptr<Session>> pending;
  bool quitting = false;
  std::chrono::steady_clock::time_point started;
  void activate();
  void connection_action(void *, Profile, std::string, bool = false);
  void close_page(AdwTabPage *);
  void quit();

public:
  Application();
  ~Application();
  int run(int, char **);
  void connection(std::optional<Profile> = {}, bool = false);
  void create_database();
  Workspace *current();
  GtkWindow *parent() {
    return window;
  }
  Store &metadata() {
    return *store;
  }
  void close_current();
  friend class Workspace;
};
} // namespace puff
