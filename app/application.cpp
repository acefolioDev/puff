#include "ui_helpers.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
namespace puff {
namespace {
std::string self_test_path;
bool self_test_postgres = false;
bool self_test_failed = false;
} // namespace
Application::Application() : started(std::chrono::steady_clock::now()) {
  application = adw_application_new("io.github.puff.Puff", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(application, "activate",
                   reinterpret_cast<GCallback>(+[](GApplication *, gpointer data) {
                     auto *self = static_cast<Application *>(data);
                     try {
                       self->activate();
                     } catch (const std::exception &e) {
                       std::cerr << "Cannot start My Puff: " << e.what() << '\n';
                       g_application_quit(G_APPLICATION(self->application));
                       self_test_failed = true;
                     }
                   }),
                   this);
}
Application::~Application() {
  for (auto &w : workspaces)
    w->dispose();
  for (auto &s : pending)
    s->close();
  for (auto &w : workspaces)
    w->session->wait();
  for (auto &s : pending)
    s->wait();
  workspaces.clear();
  pending.clear();
  g_object_unref(application);
}
int Application::run(int argc, char **argv) {
  umask(0077);
  if (argc == 3 &&
      (std::string(argv[1]) == "--self-test" || std::string(argv[1]) == "--self-test-postgres")) {
    self_test_path = argv[2];
    self_test_postgres = std::string(argv[1]) == "--self-test-postgres";
  }
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "My Puff 0.2.0 (C++20 / GTK4 / libadwaita)\n";
    return 0;
  }
  int count = self_test_path.empty() ? argc : 1;
  int code = g_application_run(G_APPLICATION(application), count, argv);
  return self_test_failed ? 1 : code;
}
Workspace *Application::current() {
  auto *page = tabs ? adw_tab_view_get_selected_page(tabs) : nullptr;
  if (!page)
    return nullptr;
  auto *widget = adw_tab_page_get_child(page);
  for (auto &w : workspaces)
    if (w->widget() == widget)
      return w.get();
  return nullptr;
}
void Application::activate() {
  if (window) {
    gtk_window_present(window);
    return;
  }
  store = std::make_unique<Store>();
  window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(application)));
  gtk_window_set_title(window, "My Puff");
  gtk_window_set_icon_name(window, "puff");
  gtk_window_set_default_size(window, 1200, 780);
  auto *root = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  auto *title = adw_window_title_new("My Puff", "Native database workspace");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                            button("Connect existing", [this] { connection(); }));
  adw_header_bar_pack_start(
      ADW_HEADER_BAR(header),
      button("Create Database", [this] { create_database(); }, "suggested-action"));
  adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                            button("Disconnect tab", [this] { close_current(); }));
  auto *menu = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
  auto *popover = gtk_popover_new();
  auto *appearance = padded(box(), 6);
  for (auto [label, scheme] : {std::pair{"System appearance", ADW_COLOR_SCHEME_DEFAULT},
                               std::pair{"Light appearance", ADW_COLOR_SCHEME_FORCE_LIGHT},
                               std::pair{"Dark appearance", ADW_COLOR_SCHEME_FORCE_DARK}})
    gtk_box_append(GTK_BOX(appearance), button(label, [scheme, popover] {
                     adw_style_manager_set_color_scheme(adw_style_manager_get_default(), scheme);
                     gtk_popover_popdown(GTK_POPOVER(popover));
                   }));
  gtk_popover_set_child(GTK_POPOVER(popover), appearance);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(root), header);
  tabs = adw_tab_view_new();
  auto *bar = adw_tab_bar_new();
  adw_tab_bar_set_view(ADW_TAB_BAR(bar), tabs);
  adw_tab_bar_set_autohide(ADW_TAB_BAR(bar), TRUE);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(root), GTK_WIDGET(bar));
  stack = GTK_STACK(gtk_stack_new());
  auto *empty = adw_status_page_new();
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Your databases, a little lighter.");
  adw_status_page_set_description(ADW_STATUS_PAGE(empty),
                                  "Connect to PostgreSQL or an existing SQLite file. No database "
                                  "connections open automatically.");
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "puff");
  adw_status_page_set_child(
      ADW_STATUS_PAGE(empty),
      button("Connect to a database", [this] { connection(); }, "suggested-action"));
  gtk_stack_add_named(stack, empty, "empty");
  gtk_stack_add_named(stack, GTK_WIDGET(tabs), "tabs");
  gtk_stack_set_visible_child_name(stack, "empty");
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(root), GTK_WIDGET(stack));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), root);
  g_signal_connect(
      tabs, "close-page",
      reinterpret_cast<GCallback>(+[](AdwTabView *, AdwTabPage *page, gpointer p) -> gboolean {
        static_cast<Application *>(p)->close_page(page);
        return TRUE;
      }),
      this);
  g_signal_connect(window, "close-request",
                   reinterpret_cast<GCallback>(+[](GtkWindow *, gpointer data) -> gboolean {
                     auto *self = static_cast<Application *>(data);
                     if (self->quitting)
                       return FALSE;
                     bool busy =
                         std::any_of(self->workspaces.begin(), self->workspaces.end(), [](auto &w) {
                           return w->dirty() || w->busy() || w->transaction();
                         });
                     if (busy) {
                       confirm(self->window, "Quit My Puff?",
                               "Discard staged edits, cancel work and disconnect? Open "
                               "transactions roll back; committed changes remain.",
                               "", [self] { self->quit(); });
                       return TRUE;
                     }
                     self->quit();
                     return TRUE;
                   }),
                   this);
  auto *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(
      keys, "key-pressed",
      reinterpret_cast<GCallback>(+[](GtkEventControllerKey *, guint key, guint,
                                      GdkModifierType state, gpointer data) -> gboolean {
        auto *self = static_cast<Application *>(data);
        if (!(state & GDK_CONTROL_MASK))
          return FALSE;
        auto *w = self->current();
        if (key == GDK_KEY_Return && w) {
          w->run();
          return TRUE;
        }
        if (key == GDK_KEY_period && w) {
          w->cancel();
          return TRUE;
        }
        if (key == GDK_KEY_n || key == GDK_KEY_t) {
          std::optional<Profile> profile;
          if (w) {
            profile = w->profile();
            profile->password.clear();
          }
          self->connection(profile);
          return TRUE;
        }
        if (key == GDK_KEY_w) {
          self->close_current();
          return TRUE;
        }
        return FALSE;
      }),
      this);
  gtk_widget_add_controller(GTK_WIDGET(window), keys);
  if (g_getenv("PUFF_MEASURE"))
    g_signal_connect(window, "map", reinterpret_cast<GCallback>(+[](GtkWidget *, gpointer p) {
                       auto *self = static_cast<Application *>(p);
                       std::cout << "MEASURE ready_ms "
                                 << std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - self->started)
                                        .count()
                                 << std::endl;
                     }),
                     this);
  gtk_window_present(window);
  if (!self_test_path.empty()) {
    Profile p;
    p.engine = "sqlite";
    p.name = "Native UI integration";
    p.database = self_test_path;
    if (self_test_postgres) {
      p.engine = "postgres";
      p.host = self_test_path;
      p.database = "postgres";
      p.user = g_get_user_name();
      p.port = 55439;
      p.tls = "disable";
    }
    p.readonly = false;
    auto session = Session::make(p);
    pending.push_back(session);
    session->submit(
        [](Session &s) {
          s.driver = connect(s.profile, s.cancelled);
          return Page{};
        },
        [this, session](Page, std::string error) {
          pending.erase(std::remove(pending.begin(), pending.end(), session), pending.end());
          if (!error.empty()) {
            self_test_failed = true;
            std::cerr << error << '\n';
            session->close();
            quit();
            return;
          }
          auto w = std::make_shared<Workspace>(*this, session);
          workspaces.push_back(w);
          w->build();
          auto *page = adw_tab_view_append(tabs, w->widget());
          adw_tab_page_set_title(page, "Native integration");
          gtk_stack_set_visible_child_name(stack, "tabs");
          if (auto *count = g_getenv("PUFF_MEASURE_TABS"); count && std::string(count) == "4") {
            for (int i = 0; i < 3; ++i) {
              auto extra = Session::make(session->profile);
              pending.push_back(extra);
              extra->submit(
                  [](Session &s) {
                    s.driver = connect(s.profile, s.cancelled);
                    return Page{};
                  },
                  [this, extra](Page, std::string error) {
                    pending.erase(std::remove(pending.begin(), pending.end(), extra),
                                  pending.end());
                    if (!error.empty()) {
                      extra->close();
                      return;
                    }
                    auto tab = std::make_shared<Workspace>(*this, extra);
                    workspaces.push_back(tab);
                    tab->build();
                    auto *page = adw_tab_view_append(tabs, tab->widget());
                    adw_tab_page_set_title(page, "Benchmark connection");
                  });
            }
          }
          w->exercise([this](bool success, std::string message) {
            std::cout << message << std::endl;
            self_test_failed = !success;
            if (success && g_getenv("PUFF_MEASURE")) {
              while (adw_tab_view_get_n_pages(tabs))
                close_current();
              std::cout << "MEASURE disconnected" << std::endl;
              g_timeout_add(
                  2000,
                  +[](gpointer p) -> gboolean {
                    static_cast<Application *>(p)->quit();
                    return G_SOURCE_REMOVE;
                  },
                  this);
            } else
              quit();
          });
        });
  }
}
void Application::create_database() {
  if (auto *w = current(); w && w->profile().engine == "postgres" && !w->profile().readonly) {
    w->create_database();
    return;
  }
  Profile p;
  if (auto *w = current(); w && w->profile().engine == "postgres") {
    p = w->profile();
    p.id.clear();
    p.password.clear();
    p.remember = false;
    p.name += " · administration";
  }
  p.readonly = false;
  connection(p, true);
}
void Application::connection(std::optional<Profile> initial, bool create_after) {
  auto *form = new Form(window, create_after ? "Create Database · Connect to server"
                                             : "Connect to an existing database");
  form->note(create_after ? "Step 1 of 2 · Connect to an existing PostgreSQL maintenance database "
                            "(usually postgres) with a role that has CREATEDB permission. Next, "
                            "choose the new database name and review its SQL."
                          : "Open an existing PostgreSQL database or SQLite file. Saved profiles "
                            "never include plaintext passwords.");
  auto profiles = store->profiles();
  auto p = std::make_shared<Profile>(initial.value_or(Profile{}));
  if (p->id.empty()) {
    gchar *id = g_uuid_string_random();
    p->id = id;
    g_free(id);
  }
  std::vector<std::string> saved = {"New connection"};
  for (auto &v : profiles)
    saved.push_back(v.group + " / " + v.name + " · " + v.database + " [" + v.environment + "]");
  auto *selector = form->choice("saved", "Saved profiles", saved);
  form->entry("name", "Profile name", p->name);
  auto *engine = form->choice("engine", "Database engine", {"postgres", "sqlite"}, p->engine);
  form->entry("group", "Project / group", p->group);
  form->choice("environment", "Environment", {"development", "staging", "production"},
               p->environment);
  form->entry("host", "Host / socket directory", p->host);
  form->entry("port", "Port", std::to_string(p->port));
  form->entry("user", "User", p->user);
  auto *password = form->entry("password", "Password", "", true);
  gtk_widget_set_tooltip_text(password,
                              "Leave blank to use this saved profile's keyring credential");
  form->entry("database", "Database name / existing SQLite file", p->database);
  form->choice("tls", "TLS: prefer allows fallback; verify-full validates server identity",
               {"disable", "prefer", "require", "verify-full"}, p->tls);
  form->entry("timeout", "Statement timeout (seconds)", std::to_string(p->timeout));
  form->check("readonly", "Read-only connection default", p->readonly);
  form->check("remember", "Store password in system keyring", p->remember);
  form->note("No passwords go into configuration. Use a restricted database role for production. "
             "No automatic connections or destructive retries.");
  auto move_row = [form](const char *key, GtkWidget *destination) {
    auto *row = gtk_widget_get_parent(form->inputs.at(key));
    g_object_ref(row);
    gtk_box_remove(GTK_BOX(form->fields), row);
    gtk_box_append(GTK_BOX(destination), row);
    g_object_unref(row);
    gtk_widget_set_hexpand(row, TRUE);
  };
  auto *advanced = gtk_expander_new("Advanced · connection profile and security");
  auto *advanced_fields = padded(box(), 8);
  for (const char *key : {"name", "group", "environment", "tls", "timeout"})
    move_row(key, advanced_fields);
  auto *security_note = gtk_widget_get_last_child(form->fields);
  g_object_ref(security_note);
  gtk_box_remove(GTK_BOX(form->fields), security_note);
  gtk_box_append(GTK_BOX(advanced_fields), security_note);
  g_object_unref(security_note);
  gtk_expander_set_child(GTK_EXPANDER(advanced), advanced_fields);
  gtk_box_append(GTK_BOX(form->fields), advanced);
  auto pair_rows = [form, move_row](const char *first, const char *second, const char *tag) {
    auto *before = gtk_widget_get_prev_sibling(gtk_widget_get_parent(form->inputs.at(first)));
    auto *pair = box(false, 12);
    move_row(first, pair);
    move_row(second, pair);
    gtk_box_insert_child_after(GTK_BOX(form->fields), pair, before);
    g_object_set_data(G_OBJECT(form->fields), tag, pair);
  };
  pair_rows("host", "port", "pg-network");
  pair_rows("user", "password", "pg-auth");
  auto update_engine = +[](GObject *, GParamSpec *, gpointer data) {
    auto *f = static_cast<Form *>(data);
    bool postgres = f->get("engine") == "postgres";
    for (const char *key : {"host", "port", "user", "password", "tls"})
      gtk_widget_set_visible(gtk_widget_get_parent(f->inputs.at(key)), postgres);
    gtk_widget_set_visible(f->inputs.at("remember"), postgres);
    for (const char *tag : {"pg-network", "pg-auth"})
      gtk_widget_set_visible(GTK_WIDGET(g_object_get_data(G_OBJECT(f->fields), tag)), postgres);
  };
  g_signal_connect(engine, "notify::selected", reinterpret_cast<GCallback>(update_engine), form);
  update_engine(nullptr, nullptr, form);
  struct C {
    Form *form;
    std::shared_ptr<Profile> p;
    std::vector<Profile> profiles;
  };
  auto *ctx = new C{form, p, std::move(profiles)};
  g_signal_connect_data(
      selector, "notify::selected",
      reinterpret_cast<GCallback>(+[](GObject *object, GParamSpec *, gpointer data) {
        auto *c = static_cast<C *>(data);
        auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
        if (selected && selected <= c->profiles.size())
          *c->p = c->profiles[selected - 1];
        else {
          *c->p = Profile{};
          gchar *id = g_uuid_string_random();
          c->p->id = id;
          g_free(id);
        }
        auto &p = *c->p;
        auto *f = c->form;
        if (auto *remove = GTK_WIDGET(g_object_get_data(G_OBJECT(f->fields), "remove-profile")))
          gtk_widget_set_visible(remove, selected > 0);
        for (auto [name, value] :
             {std::pair{"name", p.name}, std::pair{"engine", p.engine}, std::pair{"group", p.group},
              std::pair{"environment", p.environment}, std::pair{"host", p.host},
              std::pair{"port", std::to_string(p.port)}, std::pair{"user", p.user},
              std::pair{"database", p.database}, std::pair{"tls", p.tls},
              std::pair{"timeout", std::to_string(p.timeout)}})
          f->set(name, value);
        f->set("password", "");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(f->inputs["readonly"]), p.readonly);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(f->inputs["remember"]), p.remember);
      }),
      ctx, +[](gpointer p, GClosure *) { delete static_cast<C *>(p); }, G_CONNECT_DEFAULT);
  auto read = [form, p] {
    Profile v = *p;
    v.name = form->get("name");
    v.engine = form->get("engine");
    v.group = form->get("group");
    v.environment = form->get("environment");
    v.host = form->get("host");
    auto number = [form](const std::string &key, const std::string &caption) {
      const auto value = form->get(key);
      if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(caption + " must be a whole number.");
      try {
        return std::stoi(value);
      } catch (const std::exception &) {
        throw std::runtime_error(caption + " is too large.");
      }
    };
    v.port = v.engine == "postgres" ? number("port", "Port") : 5432;
    v.user = form->get("user");
    v.password = form->get("password");
    v.database = form->get("database");
    v.tls = form->get("tls");
    v.timeout = number("timeout", "Statement timeout");
    v.readonly = form->checked("readonly");
    v.remember = v.engine == "postgres" && form->checked("remember");
    v.validate();
    return v;
  };
  for (const std::string action : {"Test", "Save", "Connect"}) {
    form->submit(action == "Connect" && create_after ? "Connect & continue" : action,
                 [this, form, read, action, create_after] {
                   auto p = read();
                   if (create_after && (p.engine != "postgres" || p.readonly))
                     throw std::runtime_error(
                         "Database creation requires a writable PostgreSQL connection.");
                   connection_action(form, p, action, create_after);
                 });
    if (action != "Connect")
      gtk_widget_remove_css_class(gtk_widget_get_last_child(form->actions), "suggested-action");
  }
  auto *remove = button("Remove saved profile and credential", [this, form, p] {
    auto saved = store->profiles();
    auto found =
        std::find_if(saved.begin(), saved.end(), [&](const auto &v) { return v.id == p->id; });
    if (found == saved.end()) {
      form->report("This profile has not been saved.");
      return;
    }
    Profile target = *found;
    confirm(window, "Remove profile?", target.name, "", [this, form, target, alive = form->alive] {
      if (*alive)
        connection_action(form, target, "Delete");
    });
  });
  gtk_box_append(GTK_BOX(form->fields), remove);
  g_object_set_data(G_OBJECT(form->fields), "remove-profile", remove);
  gtk_widget_set_visible(remove, std::any_of(ctx->profiles.begin(), ctx->profiles.end(),
                                             [&](const auto &v) { return v.id == p->id; }));
  form->present();
}
void Application::connection_action(void *raw, Profile p, std::string action, bool create_after) {
  auto *form = static_cast<Form *>(raw);
  if (pending.size() >= 4 ||
      ((action == "Test" || action == "Connect") && workspaces.size() + pending.size() >= 4)) {
    form->report("At most four connection operations/workspaces. Disconnect a tab first.");
    return;
  }
  form->working();
  auto s = Session::make(std::move(p));
  pending.push_back(s);
  auto alive = form->alive;
  auto metadata = store.get();
  s->submit(
      [action, metadata](Session &s) {
        auto &profile = s.profile;
        auto saved = metadata->profiles();
        auto old =
            std::find_if(saved.begin(), saved.end(), [&](auto &p) { return p.id == profile.id; });
        if (action == "Delete") {
          if (old != saved.end() && old->remember)
            secret_remove(profile.id);
          metadata->remove(profile.id);
          return Page{};
        }
        if (profile.remember && profile.password.empty())
          profile.password = secret_lookup(profile.id);
        if (action == "Test" || action == "Connect")
          s.driver = connect(profile, s.cancelled);
        if (action == "Save" || action == "Connect") {
          if (profile.remember)
            secret_save(profile);
          else if (old != saved.end() && old->remember)
            secret_remove(profile.id);
          metadata->save(profile);
        }
        return Page{};
      },
      [this, s, form, alive, action, create_after](Page, std::string error) {
        pending.erase(std::remove(pending.begin(), pending.end(), s), pending.end());
        if (!*alive) {
          s->close();
          return;
        }
        form->report(error.empty() ? (action == "Test"     ? "Connection successful"
                                      : action == "Delete" ? "Profile removed"
                                                           : "Profile saved")
                                   : error,
                     error.empty());
        if (error.empty() && action == "Save")
          if (auto *remove =
                  GTK_WIDGET(g_object_get_data(G_OBJECT(form->fields), "remove-profile")))
            gtk_widget_set_visible(remove, TRUE);
        if (!error.empty() || action != "Connect") {
          s->close();
          if (action == "Delete" && error.empty())
            form->close();
          return;
        }
        form->close();
        auto w = std::make_shared<Workspace>(*this, s);
        workspaces.push_back(w);
        w->build();
        auto *page = adw_tab_view_append(tabs, w->widget());
        adw_tab_page_set_title(page, (s->profile.name + " · " + s->profile.database + " [" +
                                      s->profile.environment + "]")
                                         .c_str());
        adw_tab_view_set_selected_page(tabs, page);
        gtk_stack_set_visible_child_name(stack, "tabs");
        if (create_after)
          w->create_database();
      });
}
void Application::close_page(AdwTabPage *page) {
  auto *widget = adw_tab_page_get_child(page);
  auto it = std::find_if(workspaces.begin(), workspaces.end(),
                         [&](auto &w) { return w->widget() == widget; });
  if (it == workspaces.end()) {
    adw_tab_view_close_page_finish(tabs, page, TRUE);
    return;
  }
  auto workspace = *it;
  auto finish = [this, workspace, page] {
    workspace->dispose();
    adw_tab_view_close_page_finish(tabs, page, TRUE);
    workspaces.erase(std::remove(workspaces.begin(), workspaces.end(), workspace),
                     workspaces.end());
    if (workspaces.empty())
      gtk_stack_set_visible_child_name(stack, "empty");
  };
  if (workspace->dirty() || workspace->busy() || workspace->transaction()) {
    auto *d = adw_message_dialog_new(window, "Disconnect tab?",
                                     "Discard staged edits and cancel work? Open transactions roll "
                                     "back; committed changes remain.");
    adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(d), "cancel", "Keep tab");
    adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(d), "close", "Disconnect");
    adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(d), "cancel");
    struct C {
      Application *a;
      AdwTabPage *p;
      Action finish;
    };
    auto *c = new C{this, page, finish};
    g_signal_connect_data(
        d, "response",
        reinterpret_cast<GCallback>(+[](AdwMessageDialog *, const char *response, gpointer p) {
          auto *c = static_cast<C *>(p);
          if (std::string(response) == "close")
            c->finish();
          else
            adw_tab_view_close_page_finish(c->a->tabs, c->p, FALSE);
        }),
        c, +[](gpointer p, GClosure *) { delete static_cast<C *>(p); }, G_CONNECT_DEFAULT);
    gtk_window_present(GTK_WINDOW(d));
  } else
    finish();
}
void Application::close_current() {
  if (auto *page = adw_tab_view_get_selected_page(tabs))
    adw_tab_view_close_page(tabs, page);
}
void Application::quit() {
  if (quitting)
    return;
  quitting = true;
  for (auto &w : workspaces)
    w->dispose();
  for (auto &s : pending)
    s->close();
  for (auto &w : workspaces)
    w->session->wait();
  for (auto &s : pending)
    s->wait();
  gtk_window_destroy(window);
  g_application_quit(G_APPLICATION(application));
}
} // namespace puff
