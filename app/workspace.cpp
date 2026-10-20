#include "ui_helpers.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
namespace puff {
namespace {
struct PuffRow {
  GObject parent;
  size_t index;
};
struct PuffRowClass {
  GObjectClass parent;
};
G_DEFINE_TYPE(PuffRow, puff_row, G_TYPE_OBJECT)
void puff_row_class_init(PuffRowClass *) {}
void puff_row_init(PuffRow *r) {
  r->index = 0;
}
PuffRow *row_item(size_t index) {
  auto *r = static_cast<PuffRow *>(g_object_new(puff_row_get_type(), nullptr));
  r->index = index;
  return r;
}
// Optional snapshots of disposable test UI only; never capture a user's workspace.
void capture_test_widget(GtkWidget *widget, const std::string &name) {
  const char *directory = g_getenv("PUFF_CAPTURE_DIR");
  if (!directory)
    return;
  auto *paintable = gtk_widget_paintable_new(widget);
  auto *snapshot = gtk_snapshot_new();
  int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
  gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
  auto *node = gtk_snapshot_free_to_node(snapshot);
  g_object_unref(paintable);
  if (!node)
    throw std::runtime_error("Test UI snapshot is empty");
  graphene_rect_t viewport;
  graphene_rect_init(&viewport, 0, 0, static_cast<float>(width), static_cast<float>(height));
  auto *texture = gsk_renderer_render_texture(
      gtk_native_get_renderer(gtk_widget_get_native(widget)), node, &viewport);
  bool saved =
      gdk_texture_save_to_png(texture, (std::string(directory) + "/" + name + ".png").c_str());
  g_object_unref(texture);
  gsk_render_node_unref(node);
  if (!saved)
    throw std::runtime_error("Cannot save test UI snapshot");
}
std::vector<std::string> types = {"text",      "varchar(255)", "integer", "bigint",
                                  "numeric",   "boolean",      "uuid",    "date",
                                  "timestamp", "timestamptz",  "jsonb",   "bytea"};
} // namespace
Workspace::Workspace(Application &a, std::shared_ptr<Session> s) : app(a), session(std::move(s)) {}
Workspace::~Workspace() {
  dispose();
  if (rows)
    g_object_unref(rows);
  if (words)
    g_object_unref(words);
  if (word_buffer)
    g_object_unref(word_buffer);
}
void Workspace::build() {
  root = box();
  auto *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(paned), 225);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_box_append(GTK_BOX(root), paned);
  auto *sidebar = box();
  gtk_widget_set_size_request(sidebar, 210, -1);
  gtk_box_append(GTK_BOX(sidebar), padded(label(profile().group + " · " + profile().environment)));
  gtk_box_append(GTK_BOX(sidebar), button("Reload schemas", [this] { schemas_load(); }));
  schema_box = box();
  auto *side_scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(side_scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side_scroll), schema_box);
  gtk_box_append(GTK_BOX(sidebar), side_scroll);
  gtk_paned_set_start_child(GTK_PANED(paned), sidebar);
  auto *body = box(true, 0);
  gtk_paned_set_end_child(GTK_PANED(paned), body);
  auto *tools = padded(box(false), 8);
  run_button = button("Run  Ctrl+Enter", [this] { run(); }, "suggested-action");
  cancel_button = button("Cancel", [this] { cancel(); });
  gtk_box_append(GTK_BOX(tools), run_button);
  gtk_box_append(GTK_BOX(tools), cancel_button);
  auto *mode = label(profile().readonly ? "Read-only" : "Writable");
  gtk_widget_set_hexpand(mode, TRUE);
  gtk_label_set_xalign(GTK_LABEL(mode), 1);
  gtk_box_append(GTK_BOX(tools), mode);
  history_toggle = gtk_check_button_new_with_label("Save history");
  gtk_widget_set_tooltip_text(
      history_toggle,
      "Opt in: SQL may contain sensitive values; last 200 statements stored locally");
  gtk_box_append(GTK_BOX(tools), history_toggle);
  auto *menu = gtk_menu_button_new();
  gtk_menu_button_set_label(GTK_MENU_BUTTON(menu), "Database tools");
  g_object_set_data(G_OBJECT(root), "tools-menu", menu);
  auto *popover = gtk_popover_new();
  auto *menu_box = padded(box(), 6);
  std::vector<std::pair<std::string, Action>> actions = {
      {"Create table", [this] { schema_dialog(false); }},
      {"Manage columns", [this] { schema_dialog(true); }},
      {"Inspect constraints / indexes", [this] { inspect(); }},
      {"BEGIN transaction", [this] { execute("BEGIN"); }},
      {"Commit transaction", [this] { execute("COMMIT"); }},
      {"Rollback transaction", [this] { execute("ROLLBACK"); }},
      {"EXPLAIN current SQL",
       [this] {
         execute(
             (profile().engine == "postgres" ? "EXPLAIN (FORMAT JSON) " : "EXPLAIN QUERY PLAN ") +
             sql_text());
       }},
      {"Open SQL file", [this] { file(false, false); }},
      {"History and snippets", [this] { history(); }},
      {"Save snippet", [this] { snippet(); }},
      {"Export page CSV", [this] { file(true, false); }},
      {"Export page JSON", [this] { file(true, true); }},
      {"Backup PostgreSQL", [this] { backup_file(false); }},
      {"Restore PostgreSQL archive", [this] { backup_file(true); }},
      {"Sessions (on demand)",
       [this] {
         execute("SELECT pid,usename,state,wait_event_type,wait_event FROM pg_stat_activity WHERE "
                 "datname=current_database() LIMIT 200");
       }},
      {"Locks (on demand)",
       [this] { execute("SELECT pid,locktype,mode,granted FROM pg_locks LIMIT 200"); }}};
  for (auto &[title, action] : actions) {
    if (profile().engine != "postgres" &&
        (title == "Backup PostgreSQL" || title == "Restore PostgreSQL archive" ||
         title == "Sessions (on demand)" || title == "Locks (on demand)"))
      continue;
    auto *w = button(title, [popover, action] {
      gtk_popover_popdown(GTK_POPOVER(popover));
      action();
    });
    gtk_button_set_has_frame(GTK_BUTTON(w), FALSE);
    gtk_box_append(GTK_BOX(menu_box), w);
  }
  auto *menu_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(menu_scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(menu_scroll), TRUE);
  gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(menu_scroll), 280);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(menu_scroll), 550);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(menu_scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(menu_scroll), menu_box);
  gtk_popover_set_child(GTK_POPOVER(popover), menu_scroll);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
  gtk_box_append(GTK_BOX(tools), menu);
  gtk_box_append(GTK_BOX(body), tools);
  auto *vertical = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_position(GTK_PANED(vertical), 240);
  gtk_widget_set_vexpand(vertical, TRUE);
  gtk_box_append(GTK_BOX(body), vertical);
  buffer = gtk_source_buffer_new(nullptr);
  gtk_source_buffer_set_language(buffer, gtk_source_language_manager_get_language(
                                             gtk_source_language_manager_get_default(), "sql"));
  gtk_source_buffer_set_highlight_syntax(buffer, TRUE);
  auto update_scheme = +[](GObject *, GParamSpec *, gpointer data) {
    auto *manager = adw_style_manager_get_default();
    auto *scheme = gtk_source_style_scheme_manager_get_scheme(
        gtk_source_style_scheme_manager_get_default(),
        adw_style_manager_get_dark(manager) ? "Adwaita-dark" : "Adwaita");
    if (scheme)
      gtk_source_buffer_set_style_scheme(GTK_SOURCE_BUFFER(data), scheme);
  };
  update_scheme(nullptr, nullptr, buffer);
  g_signal_connect_object(adw_style_manager_get_default(), "notify::dark",
                          reinterpret_cast<GCallback>(update_scheme), buffer, G_CONNECT_DEFAULT);

  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buffer),
                           profile().engine == "postgres"
                               ? "SELECT current_database(), current_user;"
                               : "SELECT sqlite_version();",
                           -1);
  editor = gtk_source_view_new_with_buffer(buffer);
  gtk_accessible_update_property(GTK_ACCESSIBLE(editor), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "SQL editor", -1);
  g_object_unref(buffer);
  gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(editor), TRUE);
  gtk_source_view_set_auto_indent(GTK_SOURCE_VIEW(editor), TRUE);
  gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(editor), 4);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(editor), TRUE);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(editor), 12);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(editor), 12);
  word_buffer = gtk_source_buffer_new(nullptr);
  words = gtk_source_completion_words_new("SQL and loaded metadata");
  gtk_source_completion_words_register(words, GTK_TEXT_BUFFER(word_buffer));
  gtk_source_completion_add_provider(gtk_source_view_get_completion(GTK_SOURCE_VIEW(editor)),
                                     GTK_SOURCE_COMPLETION_PROVIDER(words));
  add_words({});
  auto *editor_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(editor_scroll), editor);
  gtk_paned_set_start_child(GTK_PANED(vertical), editor_scroll);
  auto *lower = box(true, 0);
  gtk_paned_set_end_child(GTK_PANED(vertical), lower);
  auto *row_tools = padded(box(false), 6);
  table_label = label("Query results");
  gtk_widget_set_hexpand(table_label, TRUE);
  gtk_box_append(GTK_BOX(row_tools), table_label);
  auto *add_button = button("Add row", [this] { insert(); });
  g_object_set_data(G_OBJECT(root), "add-button", add_button);
  gtk_box_append(GTK_BOX(row_tools), add_button);
  save_button = button("Save edits", [this] { save(); }, "suggested-action");
  gtk_box_append(GTK_BOX(row_tools), save_button);
  auto *delete_button = button("Delete selected", [this] { erase(); });
  g_object_set_data(G_OBJECT(root), "delete-button", delete_button);
  gtk_box_append(GTK_BOX(row_tools), delete_button);
  auto *discard_button = button("Discard", [this] { discard(); });
  g_object_set_data(G_OBJECT(root), "discard-button", discard_button);
  gtk_box_append(GTK_BOX(row_tools), discard_button);
  gtk_box_append(GTK_BOX(lower), row_tools);
  auto *search_tools = padded(box(false), 6);
  filter_entry = gtk_search_entry_new();
  gtk_accessible_update_property(GTK_ACCESSIBLE(filter_entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Filter current page", -1);
  gtk_widget_set_hexpand(filter_entry, TRUE);
  gtk_widget_set_tooltip_text(filter_entry, "Filter current page only");
  gtk_box_append(GTK_BOX(search_tools), filter_entry);
  gtk_box_append(GTK_BOX(search_tools), button("Copy rows", [this] { copy(); }));
  gtk_box_append(GTK_BOX(lower), search_tools);
  rows = g_list_store_new(puff_row_get_type());
  filter = gtk_custom_filter_new(
      +[](gpointer item, gpointer data) -> gboolean {
        auto *w = static_cast<Workspace *>(data);
        auto *row = static_cast<PuffRow *>(item);
        std::string needle = entry_text(w->filter_entry);
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (needle.empty())
          return TRUE;
        for (auto &cell : w->page.rows.at(row->index)) {
          std::string hay = text(cell);
          std::transform(hay.begin(), hay.end(), hay.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (hay.find(needle) != std::string::npos)
            return TRUE;
        }
        return FALSE;
      },
      this, nullptr);
  auto *filtered = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(rows)), GTK_FILTER(filter));
  sorter = gtk_sort_list_model_new(G_LIST_MODEL(filtered), nullptr);
  selection = gtk_multi_selection_new(G_LIST_MODEL(sorter));
  view = GTK_COLUMN_VIEW(gtk_column_view_new(GTK_SELECTION_MODEL(selection)));
  gtk_accessible_update_property(GTK_ACCESSIBLE(view), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Query results", -1);
  gtk_column_view_set_show_column_separators(view, TRUE);
  gtk_column_view_set_show_row_separators(view, TRUE);
  gtk_sort_list_model_set_sorter(sorter, gtk_column_view_get_sorter(view));
  auto *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(view));
  gtk_box_append(GTK_BOX(lower), scroll);
  auto refresh_scroll = +[](GtkAdjustment *, gpointer data) {
    auto *w = static_cast<Workspace *>(data);
    if (w->disposed)
      return;
    auto *refresh = static_cast<Action *>(g_object_get_data(G_OBJECT(w->view), "refresh-all"));
    if (refresh)
      (*refresh)();
  };
  g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)),
                   "value-changed", reinterpret_cast<GCallback>(refresh_scroll), this);
  g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll)),
                   "value-changed", reinterpret_cast<GCallback>(refresh_scroll), this);

  g_signal_connect(filter_entry, "search-changed",
                   reinterpret_cast<GCallback>(+[](GtkSearchEntry *, gpointer data) {
                     auto *w = static_cast<Workspace *>(data);
                     if (w->filter_source)
                       g_source_remove(w->filter_source);
                     w->filter_source = g_timeout_add(
                         150,
                         +[](gpointer data) -> gboolean {
                           auto *w = static_cast<Workspace *>(data);
                           w->filter_source = 0;
                           gtk_filter_changed(GTK_FILTER(w->filter), GTK_FILTER_CHANGE_DIFFERENT);
                           return G_SOURCE_REMOVE;
                         },
                         w);
                   }),
                   this);
  auto *pages = padded(box(false), 6);
  prev_button = button("Previous", [this] { previous(); });
  next_button = button("Next page", [this] { next(); });
  gtk_box_append(GTK_BOX(pages), prev_button);
  gtk_box_append(GTK_BOX(pages), next_button);
  gtk_box_append(GTK_BOX(pages), button("Close result", [this] { close_result(); }));
  gtk_box_append(GTK_BOX(pages), label("200 rows/page · sort/filter current page"));
  gtk_box_append(GTK_BOX(lower), pages);
  status = padded(label("Connected", true), 8);
  gtk_label_set_selectable(GTK_LABEL(status), TRUE);
  gtk_box_append(GTK_BOX(body), status);
  buttons();
  schemas_load();
}
void Workspace::buttons() {
  bool busy = session->busy;
  gtk_widget_set_sensitive(run_button, !busy);
  gtk_widget_set_sensitive(cancel_button, busy);
  gtk_widget_set_sensitive(next_button, !busy && page.more && edits.empty());
  gtk_widget_set_sensitive(prev_button, !busy && table.has_value() && offset > 0 && edits.empty());
  gtk_widget_set_sensitive(save_button, !busy && !edits.empty() && !profile().readonly);
  gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(root), "add-button")),
                           !busy && table.has_value() && !profile().readonly && edits.empty());
  gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(root), "delete-button")),
                           !busy && table.has_value() && !profile().readonly &&
                               std::any_of(page.columns.begin(), page.columns.end(),
                                           [](const auto &c) { return c.primary; }));
  gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(root), "discard-button")),
                           !busy && !edits.empty());
}
void Workspace::job(std::function<Page(Session &)> work, std::function<void(Page)> done) {
  auto weak = weak_from_this();
  bool accepted =
      session->submit(std::move(work), [weak, done = std::move(done)](Page p, std::string error) {
        if (auto w = weak.lock(); w && !w->disposed) {
          gtk_widget_remove_css_class(w->status, "error");
          if (!error.empty()) {
            gtk_label_set_text(GTK_LABEL(w->status), error.c_str());
            gtk_widget_add_css_class(w->status, "error");
          } else {
            gtk_label_set_text(GTK_LABEL(w->status),
                               w->session->transaction
                                   ? "Completed · transaction open (commit or rollback)"
                                   : "Completed");
            if (done)
              try {
                done(std::move(p));
              } catch (const std::exception &e) {
                gtk_label_set_text(GTK_LABEL(w->status),
                                   redact(e.what(), w->profile().password).c_str());
              }
          }
          w->buttons();
        }
      });
  gtk_label_set_text(
      GTK_LABEL(status),
      accepted ? "Working…" : "Operation already running in this tab. Cancel or use another tab.");
  buttons();
}
void Workspace::safe(Action action) {
  if (edits.empty())
    action();
  else {
    auto weak = weak_from_this();
    confirm(app.parent(), "Discard unsaved edits?", "The staged changes have not been saved.", "",
            [weak, action] {
              if (auto w = weak.lock(); w && !w->disposed) {
                w->discard();
                action();
              }
            });
  }
}
void Workspace::write_review(const std::string &title, const std::string &sql, Action action) {
  if (profile().readonly) {
    gtk_label_set_text(GTK_LABEL(status), "Reconnect with read-only disabled to modify data.");
    return;
  }
  confirm(app.parent(), title, sql, profile().environment == "production" ? profile().database : "",
          std::move(action));
}
std::string Workspace::sql_text() {
  GtkTextIter start, end;
  if (!gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(buffer), &start, &end))
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(buffer), &start, &end);
  gchar *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(buffer), &start, &end, FALSE);
  std::string out = text;
  g_free(text);
  return out;
}
void Workspace::run() {
  execute(sql_text());
}
void Workspace::execute(const std::string &sql) {
  auto weak = weak_from_this();
  safe([weak, sql] {
    if (auto w = weak.lock()) {
      auto go = [weak, sql] {
        if (auto w = weak.lock()) {
          w->table.reset();
          w->page.more = false;
          w->job([sql](Session &s) { return s.driver->execute(sql); },
                 [weak, sql](Page p) {
                   if (auto w = weak.lock()) {
                     w->display(std::move(p));
                     if (gtk_check_button_get_active(GTK_CHECK_BUTTON(w->history_toggle)))
                       w->app.metadata().record(w->profile().id, sql);
                   }
                 });
        }
      };
      if (needs_review(sql) || w->profile().environment == "production")
        confirm(w->app.parent(), "Review SQL", sql.substr(0, 12000),
                w->profile().environment == "production" ? w->profile().database : "", go);
      else
        go();
    }
  });
}
void Workspace::cancel() {
  session->cancel();
  gtk_label_set_text(GTK_LABEL(status), "Cancellation requested…");
}
void Workspace::clear() {
  g_list_store_remove_all(rows);
  auto *columns = gtk_column_view_get_columns(view);
  while (g_list_model_get_n_items(columns)) {
    auto *c = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(columns, 0));
    gtk_column_view_remove_column(view, c);
    g_object_unref(c);
  }
  page = {};
  edits.clear();
}
void Workspace::display(Page p) {
  auto model_start = std::chrono::steady_clock::now();
  auto revision = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(view), "page-revision")) + 1;
  g_object_set_data(G_OBJECT(view), "page-revision", GUINT_TO_POINTER(revision));
  bool same_columns =
      page.names == p.names && g_list_model_get_n_items(gtk_column_view_get_columns(view)) ==
                                   std::min<size_t>(p.names.size(), 100);
  bool reuse_rows = same_columns && g_list_model_get_n_items(G_LIST_MODEL(rows)) == p.rows.size();
  struct Context {
    Workspace *w;
    size_t column;
    std::vector<GtkListItem *> bound;
    std::function<void(GtkListItem *)> update;
  };
  if (same_columns) {
    if (!reuse_rows)
      g_list_store_remove_all(rows);
    edits.clear();
  } else
    clear();
  page = std::move(p);
  gtk_label_set_text(GTK_LABEL(table_label),
                     table ? (table->first + "." + table->second).c_str() : "Query results");
  for (size_t index = 0; !same_columns && index < std::min<size_t>(page.names.size(), 100);
       ++index) {
    auto *ctx = new Context{this, index};
    auto *factory = gtk_signal_list_item_factory_new();
    g_object_set_data_full(
        G_OBJECT(factory), "context", ctx, +[](gpointer p) { delete static_cast<Context *>(p); });
    g_signal_connect(factory, "setup",
                     reinterpret_cast<GCallback>(+[](GtkSignalListItemFactory *, GtkListItem *item,
                                                     gpointer data) {
                       auto *ctx = static_cast<Context *>(data);
                       auto *label = gtk_label_new("");
                       gtk_label_set_xalign(GTK_LABEL(label), 0);
                       gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
                       gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
                       padded(label, 4);
                       auto *gesture = gtk_gesture_click_new();
                       struct Edit {
                         Context *ctx;
                         GtkListItem *item;
                       };
                       auto *e = new Edit{ctx, item};
                       g_signal_connect_data(
                           gesture, "released",
                           reinterpret_cast<GCallback>(+[](GtkGestureClick *, int count, double,
                                                           double, gpointer p) {
                             auto *e = static_cast<Edit *>(p);
                             if (count == 2) {
                               auto *r = static_cast<PuffRow *>(gtk_list_item_get_item(e->item));
                               if (r)
                                 e->ctx->w->edit(r->index, e->ctx->column);
                             }
                           }),
                           e, +[](gpointer p, GClosure *) { delete static_cast<Edit *>(p); },
                           G_CONNECT_DEFAULT);
                       gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(gesture));
                       gtk_list_item_set_child(item, label);
                     }),
                     ctx);
    ctx->update = [ctx](GtkListItem *item) {
      auto *w = ctx->w;
      auto *r = static_cast<PuffRow *>(gtk_list_item_get_item(item));
      if (!r)
        return;
      auto *label = gtk_list_item_get_child(item);
      auto revision = g_object_get_data(G_OBJECT(w->view), "page-revision");
      auto row_tag = GUINT_TO_POINTER(static_cast<guint>(r->index + 1));
      if (g_object_get_data(G_OBJECT(label), "cell-revision") == revision &&
          g_object_get_data(G_OBJECT(label), "cell-row") == row_tag)
        return;
      g_object_set_data(G_OBJECT(label), "cell-revision", revision);
      g_object_set_data(G_OBJECT(label), "cell-row", row_tag);
      auto &name = w->page.names.at(ctx->column);
      const Cell *value = &w->page.rows.at(r->index).at(ctx->column);
      auto edited = w->edits.find(r->index);
      bool changed = edited != w->edits.end() && edited->second.contains(name);
      if (changed)
        value = &edited->second.at(name);
      static const std::string null_text = "NULL";
      const std::string &str = *value ? **value : null_text;
      auto visible = preview(str, 1000), tip = preview(str, 4000);
      if (visible != gtk_label_get_text(GTK_LABEL(label)))
        gtk_label_set_text(GTK_LABEL(label), visible.c_str());
      auto *previous_tip = gtk_widget_get_tooltip_text(label);
      if (tip != (previous_tip ? previous_tip : ""))
        gtk_widget_set_tooltip_text(label, tip.c_str());
      if (changed != static_cast<bool>(gtk_widget_has_css_class(label, "accent"))) {
        if (changed)
          gtk_widget_add_css_class(label, "accent");
        else
          gtk_widget_remove_css_class(label, "accent");
      }
      bool null_style = !changed && !*value;
      if (null_style != static_cast<bool>(gtk_widget_has_css_class(label, "dim-label"))) {
        if (null_style)
          gtk_widget_add_css_class(label, "dim-label");
        else
          gtk_widget_remove_css_class(label, "dim-label");
      }
    };
    g_signal_connect(factory, "bind",
                     reinterpret_cast<GCallback>(
                         +[](GtkSignalListItemFactory *, GtkListItem *item, gpointer data) {
                           auto *ctx = static_cast<Context *>(data);
                           if (std::find(ctx->bound.begin(), ctx->bound.end(), item) ==
                               ctx->bound.end())
                             ctx->bound.push_back(item);
                           ctx->update(item);
                         }),
                     ctx);
    g_signal_connect(factory, "unbind",
                     reinterpret_cast<GCallback>(
                         +[](GtkSignalListItemFactory *, GtkListItem *item, gpointer data) {
                           auto *ctx = static_cast<Context *>(data);
                           ctx->bound.erase(std::remove(ctx->bound.begin(), ctx->bound.end(), item),
                                            ctx->bound.end());
                         }),
                     ctx);
    auto *column =
        gtk_column_view_column_new(page.names[index].c_str(), GTK_LIST_ITEM_FACTORY(factory));
    gtk_column_view_column_set_resizable(column, TRUE);
    gtk_column_view_column_set_fixed_width(column, index == 0 ? 110 : 180);
    gtk_column_view_column_set_expand(column,
                                      index + 1 == std::min<size_t>(page.names.size(), 100));
    auto *sort_context = new Context{this, index};
    auto *sort = gtk_custom_sorter_new(
        +[](gconstpointer a, gconstpointer b, gpointer p) -> int {
          auto *c = static_cast<Context *>(p);
          auto &x = c->w->page.rows.at(static_cast<const PuffRow *>(a)->index).at(c->column);
          auto &y = c->w->page.rows.at(static_cast<const PuffRow *>(b)->index).at(c->column);
          return text(x).compare(text(y));
        },
        sort_context, +[](gpointer p) { delete static_cast<Context *>(p); });
    gtk_column_view_column_set_sorter(column, GTK_SORTER(sort));
    g_object_unref(sort);
    gtk_column_view_append_column(view, column);
    g_object_unref(column);
  }
  auto *refresh_all = new Action([this] {
    auto *columns = gtk_column_view_get_columns(view);
    for (guint i = 0; i < g_list_model_get_n_items(columns); ++i) {
      auto *column = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(columns, i));
      auto *factory = gtk_column_view_column_get_factory(column);
      auto *ctx = static_cast<Context *>(g_object_get_data(G_OBJECT(factory), "context"));
      for (auto *item : ctx->bound)
        ctx->update(item);
      g_object_unref(column);
    }
  });
  g_object_set_data_full(
      G_OBJECT(view), "refresh-all", refresh_all,
      +[](gpointer p) { delete static_cast<Action *>(p); });
  if (reuse_rows) {
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    if (!entry_text(filter_entry).empty())
      gtk_filter_changed(GTK_FILTER(filter), GTK_FILTER_CHANGE_DIFFERENT);
    if (gtk_sorter_get_order(gtk_column_view_get_sorter(view)) != GTK_SORTER_ORDER_NONE)
      gtk_sorter_changed(gtk_column_view_get_sorter(view), GTK_SORTER_CHANGE_DIFFERENT);
    auto *refresh = static_cast<Action *>(g_object_get_data(G_OBJECT(view), "refresh-all"));
    if (refresh)
      (*refresh)();
  } else {
    std::vector<gpointer> additions;
    additions.reserve(page.rows.size());
    for (size_t i = 0; i < page.rows.size(); ++i)
      additions.push_back(row_item(i));
    if (!additions.empty())
      g_list_store_splice(rows, 0, 0, additions.data(), additions.size());
    for (auto *row : additions)
      g_object_unref(row);
  }
  std::string summary = std::to_string(page.rows.size()) + " rows · " +
                        std::to_string(static_cast<int>(page.ms)) + " ms · " +
                        (page.more ? "more available" : "complete") +
                        (session->transaction ? " · transaction open" : "");
  if (!page.message.empty())
    summary += " · " + page.message;
  gtk_label_set_text(GTK_LABEL(status), summary.c_str());
  buttons();
  if (g_getenv("PUFF_MEASURE"))
    std::cout << "MEASURE model_ms "
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           model_start)
                     .count()
              << std::endl;
}
void Workspace::schemas_load() {
  auto weak = weak_from_this();
  job([](Session &s) { return s.driver->schemas(); },
      [weak](Page p) {
        if (auto w = weak.lock()) {
          w->page.more = false;
          w->schemas.clear();
          while (auto *child = gtk_widget_get_first_child(w->schema_box))
            gtk_box_remove(GTK_BOX(w->schema_box), child);
          for (auto &row : p.rows) {
            std::string name = text(row[0]);
            w->schemas.push_back(name);
            auto *expander = gtk_expander_new(name.c_str());
            struct C {
              std::weak_ptr<Workspace> w;
              std::string schema;
            };
            auto *ctx = new C{weak, name};
            g_signal_connect_data(
                expander, "notify::expanded",
                reinterpret_cast<GCallback>(+[](GObject *object, GParamSpec *, gpointer data) {
                  auto *c = static_cast<C *>(data);
                  if (auto w = c->w.lock())
                    w->schema_expand(GTK_EXPANDER(object), c->schema);
                }),
                ctx, +[](gpointer p, GClosure *) { delete static_cast<C *>(p); },
                G_CONNECT_DEFAULT);
            gtk_box_append(GTK_BOX(w->schema_box), expander);
          }
        }
      });
}
void Workspace::schema_expand(GtkExpander *expander, const std::string &schema) {
  if (!gtk_expander_get_expanded(expander) || gtk_expander_get_child(expander))
    return;
  auto weak = weak_from_this();
  job([schema](Session &s) { return s.driver->tables(schema); },
      [weak, expander, schema](Page p) {
        if (auto w = weak.lock()) {
          w->page.more = false;
          auto *model = gtk_string_list_new(nullptr);
          std::vector<std::string> names;
          for (auto &r : p.rows) {
            names.push_back(text(r[0]));
            gtk_string_list_append(model, names.back().c_str());
          }
          w->add_words(names);
          auto *factory = gtk_signal_list_item_factory_new();
          g_signal_connect(factory, "setup",
                           reinterpret_cast<GCallback>(
                               +[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
                                 gtk_list_item_set_child(item, padded(label(""), 6));
                               }),
                           nullptr);
          g_signal_connect(factory, "bind",
                           reinterpret_cast<GCallback>(
                               +[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
                                 auto *str = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
                                 gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                                                    gtk_string_object_get_string(str));
                               }),
                           nullptr);
          auto *list =
              gtk_list_view_new(GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(model))),
                                GTK_LIST_ITEM_FACTORY(factory));
          struct C {
            std::weak_ptr<Workspace> w;
            std::string schema;
            std::vector<std::string> names;
          };
          auto *ctx = new C{weak, schema, std::move(names)};
          g_signal_connect_data(
              list, "activate",
              reinterpret_cast<GCallback>(+[](GtkListView *, guint index, gpointer p) {
                auto *c = static_cast<C *>(p);
                if (auto w = c->w.lock(); w && index < c->names.size())
                  w->browse(c->schema, c->names[index]);
              }),
              ctx, +[](gpointer p, GClosure *) { delete static_cast<C *>(p); }, G_CONNECT_DEFAULT);
          gtk_expander_set_child(expander, list);
        }
      });
}
void Workspace::add_words(const std::vector<std::string> &names) {
  completions.insert(names.begin(), names.end());
  while (completions.size() > 1500)
    completions.erase(std::prev(completions.end()));
  std::string out = "SELECT FROM WHERE JOIN ORDER BY GROUP LIMIT OFFSET INSERT INTO VALUES UPDATE "
                    "SET DELETE CREATE ALTER TABLE INDEX BEGIN COMMIT ROLLBACK NULL TRUE FALSE ";
  for (auto &name : completions)
    out += name + ' ';
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(word_buffer), out.c_str(), out.size());
}
void Workspace::browse(const std::string &s, const std::string &t, size_t start) {
  auto weak = weak_from_this();
  safe([weak, s, t, start] {
    if (auto w = weak.lock())
      w->job([s, t, start](Session &sesh) { return sesh.driver->browse(s, t, start); },
             [weak, s, t, start](Page p) {
               if (auto w = weak.lock()) {
                 w->table = {{s, t}};
                 w->offset = start;
                 std::vector<std::string> names;
                 for (auto &c : p.columns)
                   names.push_back(c.name);
                 w->add_words(names);
                 w->display(std::move(p));
               }
             });
  });
}
void Workspace::next() {
  if (table)
    browse(table->first, table->second, offset + page_rows);
  else {
    auto weak = weak_from_this();
    job([](Session &s) { return s.driver->fetch(); },
        [weak](Page p) {
          if (auto w = weak.lock())
            w->display(std::move(p));
        });
  }
}
void Workspace::previous() {
  if (table)
    browse(table->first, table->second, offset >= page_rows ? offset - page_rows : 0);
}
void Workspace::close_result() {
  auto weak = weak_from_this();
  safe([weak] {
    if (auto w = weak.lock())
      w->job(
          [](Session &s) {
            s.driver->release();
            return Page{};
          },
          [weak](Page) {
            if (auto w = weak.lock()) {
              w->clear();
              w->table.reset();
            }
          });
  });
}
void Workspace::discard() {
  g_object_set_data(
      G_OBJECT(view), "page-revision",
      GUINT_TO_POINTER(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(view), "page-revision")) + 1));
  edits.clear();
  g_list_model_items_changed(G_LIST_MODEL(rows), 0, g_list_model_get_n_items(G_LIST_MODEL(rows)),
                             g_list_model_get_n_items(G_LIST_MODEL(rows)));
  buttons();
}
void Workspace::edit(size_t row, size_t column) {
  if (!table || profile().readonly || session->busy || column >= page.columns.size())
    return;
  if (!std::any_of(page.columns.begin(), page.columns.end(), [](auto &c) { return c.primary; })) {
    gtk_label_set_text(GTK_LABEL(status), "Primary key required for safe editing");
    return;
  }
  auto c = page.columns[column];
  if (c.generated)
    return;
  auto *form = new Form(app.parent(), "Edit " + c.name);
  auto changed = edits.find(row);
  Cell value = changed != edits.end() && changed->second.contains(c.name)
                   ? changed->second.at(c.name)
                   : page.rows[row][column];
  form->entry("value", "Value (" + c.type + ")", value.value_or(""));
  form->check("null", "SQL NULL", !value);
  form->note(
      "Values are bound as database text: ordinary text needs no quotes. PostgreSQL validates "
      "numbers, JSON, arrays and types. Bytea uses PostgreSQL's \\x hex notation.");
  auto weak = weak_from_this();
  form->submit("Stage edit", [weak, form, row, c] {
    if (auto w = weak.lock()) {
      w->edits[row][c.name] = form->checked("null") ? Cell{} : Cell(form->get("value"));
      g_object_set_data(
          G_OBJECT(w->view), "page-revision",
          GUINT_TO_POINTER(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w->view), "page-revision")) +
                           1));
      auto *r = row_item(row);
      gpointer items[] = {r};
      g_list_store_splice(w->rows, row, 1, items, 1);
      g_object_unref(r);
      w->buttons();
    }
    form->close();
  });
  form->present();
}
std::vector<size_t> Workspace::selected() {
  std::vector<size_t> out;
  auto *model = G_LIST_MODEL(selection);
  for (guint i = 0; i < g_list_model_get_n_items(model); ++i)
    if (gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection), i)) {
      auto *r = static_cast<PuffRow *>(g_list_model_get_item(model, i));
      out.push_back(r->index);
      g_object_unref(r);
    }
  return out;
}
void Workspace::copy() {
  std::string out;
  for (size_t i = 0; i < page.names.size(); ++i) {
    if (i)
      out += '\t';
    out += page.names[i];
  }
  out += '\n';
  for (auto index : selected()) {
    for (size_t i = 0; i < page.rows[index].size(); ++i) {
      if (i)
        out += '\t';
      out += text(page.rows[index][i]);
    }
    out += '\n';
  }
  gdk_clipboard_set_text(gtk_widget_get_clipboard(root), out.c_str());
}
void Workspace::save() {
  if (!table || edits.empty())
    return;
  std::vector<Change> changes;
  for (auto &[row, values] : edits) {
    Change c;
    c.kind = "update";
    c.values = values;
    for (size_t i = 0; i < page.names.size(); ++i)
      c.original[page.names[i]] = page.rows[row][i];
    changes.push_back(std::move(c));
  }
  auto target = *table;
  auto weak = weak_from_this();
  write_review("Save staged changes?",
               std::to_string(changes.size()) + " rows in " + target.first + '.' + target.second +
                   ". Atomic batch; conflicts cause rollback.",
               [weak, target, changes] {
                 if (auto w = weak.lock())
                   w->job(
                       [target, changes](Session &s) {
                         s.driver->mutate(target.first, target.second, changes);
                         return Page{};
                       },
                       [weak, target](Page) {
                         if (auto w = weak.lock()) {
                           w->edits.clear();
                           w->browse(target.first, target.second, w->offset);
                         }
                       });
               });
}
void Workspace::erase() {
  if (!table || profile().readonly)
    return;
  if (!edits.empty()) {
    gtk_label_set_text(GTK_LABEL(status), "Save/discard cell edits before deletion");
    return;
  }
  auto indices = selected();
  if (indices.empty())
    return;
  std::vector<Change> changes;
  for (auto row : indices) {
    Change c;
    c.kind = "delete";
    for (size_t i = 0; i < page.names.size(); ++i)
      c.original[page.names[i]] = page.rows[row][i];
    changes.push_back(std::move(c));
  }
  auto target = *table;
  auto weak = weak_from_this();
  write_review("Delete selected rows?",
               "Permanently delete " + std::to_string(changes.size()) + " selected rows from " +
                   target.first + '.' + target.second,
               [weak, target, changes] {
                 if (auto w = weak.lock())
                   w->job(
                       [target, changes](Session &s) {
                         s.driver->mutate(target.first, target.second, changes);
                         return Page{};
                       },
                       [weak, target](Page) {
                         if (auto w = weak.lock())
                           w->browse(target.first, target.second, w->offset);
                       });
               });
}
void Workspace::insert() {
  if (!table || profile().readonly)
    return;
  auto *form = new Form(app.parent(), "Insert row");
  std::vector<Column> cols;
  for (auto &c : page.columns)
    if (!c.generated) {
      cols.push_back(c);
      form->check("use:" + c.name, "Supply " + c.name, false);
      form->entry(c.name, c.name + " (" + c.type + ")");
      form->check("null:" + c.name, "SQL NULL for " + c.name, false);
    }
  form->note("Unchecked fields use database defaults. This inserts one row immediately unless an "
             "explicit transaction is open.");
  auto weak = weak_from_this();
  auto target = *table;
  form->submit("Review insertion", [weak, target, form, cols] {
    Change c;
    c.kind = "insert";
    for (auto &column : cols)
      if (form->checked("use:" + column.name))
        c.values[column.name] =
            form->checked("null:" + column.name) ? Cell{} : Cell(form->get(column.name));
    if (auto w = weak.lock())
      w->write_review("Insert one row?", target.first + '.' + target.second,
                      [weak, target, c, form, alive = form->alive] {
                        if (auto w = weak.lock()) {
                          form->working();
                          w->session->submit(
                              [target, c](Session &s) {
                                s.driver->mutate(target.first, target.second, {c});
                                return Page{};
                              },
                              [weak, target, form, alive](Page, std::string error) {
                                if (!*alive)
                                  return;
                                form->report(error);
                                if (error.empty()) {
                                  form->close();
                                  if (auto w = weak.lock())
                                    w->browse(target.first, target.second, w->offset);
                                }
                              });
                        }
                      });
  });
  form->present();
}
void Workspace::create_database() {
  if (session->busy || session->transaction) {
    gtk_label_set_text(GTK_LABEL(status), "Finish the current operation and commit or roll back "
                                          "any transaction before creating a database.");
    return;
  }
  if (profile().readonly || profile().engine != "postgres") {
    gtk_label_set_text(GTK_LABEL(status), "Requires a writable PostgreSQL connection");
    return;
  }
  auto *form = new Form(app.parent(), "Create database");
  form->note("Create a PostgreSQL database on " + profile().host + " as " + profile().user +
             ". The current connection stays open.");
  form->entry("name", "New database name", "my_database");
  form->note("Your role needs CREATEDB privileges. Connect to the new database in a new tab after "
             "creation.");
  auto weak = weak_from_this();
  form->submit("Review SQL", [weak, form] {
    std::string name = form->get("name");
    if (name.empty() || name.size() > 63 || name.find_first_of("\r\n") != std::string::npos)
      throw std::runtime_error("Use a database name between 1 and 63 bytes, without line breaks.");
    std::string sql = "CREATE DATABASE " + quote(name);
    if (auto w = weak.lock())
      w->write_review("Create database?", sql, [weak, form, name, alive = form->alive] {
        if (!*alive)
          return;
        if (auto w = weak.lock()) {
          form->working();
          bool accepted = w->session->submit(
              [name](Session &s) {
                s.driver->create_database(name);
                return Page{};
              },
              [weak, form, alive, name](Page, std::string error) {
                if (auto w = weak.lock()) {
                  // CREATE DATABASE replaces an unfinished raw-query stream. The visible
                  // page remains useful, but fetching from that old stream is no longer valid.
                  if (!w->table)
                    w->page.more = false;
                  w->buttons();
                }
                if (!*alive)
                  return;
                form->report(error);
                if (error.empty()) {
                  form->close();
                  if (auto w = weak.lock()) {
                    gtk_label_set_text(GTK_LABEL(w->status),
                                       ("Created database “" + name + "”.").c_str());
                    Profile next = w->profile();
                    next.database = name;
                    next.password.clear();
                    next.id.clear();
                    next.remember = false;
                    next.name = name;
                    w->app.connection(next);
                  }
                }
              });
          if (!accepted)
            form->report("Operation already running");
        }
      });
  });
  form->present();
}
void Workspace::inspect() {
  if (!table)
    return;
  auto target = *table;
  auto weak = weak_from_this();
  safe([weak, target] {
    if (auto w = weak.lock())
      w->job([target](Session &s) { return s.driver->inspect(target.first, target.second); },
             [weak](Page p) {
               if (auto w = weak.lock()) {
                 w->table.reset();
                 w->display(std::move(p));
               }
             });
  });
}
void Workspace::schema_dialog(bool existing) {
  if (profile().readonly)
    return;
  if (existing && !table)
    return;
  auto weak = weak_from_this();
  safe([weak, existing] {
    if (auto w = weak.lock()) {
      auto *form = new Form(w->app.parent(), existing ? "Manage columns" : "Create table");
      form->choice("schema", "Schema", w->schemas, existing ? w->table->first : "");
      form->entry("table", "Table name", existing ? w->table->second : "");
      if (existing) {
        gtk_editable_set_editable(GTK_EDITABLE(form->inputs["table"]), FALSE);
        std::vector<std::string> names;
        for (auto &c : w->page.columns)
          names.push_back(c.name);
        form->choice("column", "Existing column", names);
        std::vector<std::string> actions = {"add", "rename", "drop"};
        if (w->profile().engine == "postgres")
          actions.push_back("type");
        form->choice("operation", "Operation", actions);
        form->entry("new", "New name");
        form->choice("type", "Data type", types);
      } else {
        form->note("Define columns below. Press Add column for additional fields; primary keys can "
                   "span several columns.");
      }
      auto columns = std::make_shared<std::vector<std::string>>();
      auto add = [form, columns] {
        if (columns->size() >= 100)
          throw std::runtime_error("Maximum 100 columns");
        std::string id = std::to_string(columns->size());
        columns->push_back(id);
        form->entry("name:" + id, "Column name", id == "0" ? "id" : "");
        form->choice("type:" + id, "Column type", types, id == "0" ? "bigint" : "text");
        form->check("pk:" + id, "Primary key", id == "0");
        form->check("nullable:" + id, "Nullable", id != "0");
      };
      if (!existing) {
        add();
        gtk_box_append(GTK_BOX(form->fields), button("Add column", add));
      }
      form->submit("Review SQL", [weak, form, columns, existing] {
        std::string sql;
        if (existing) {
          auto op = form->get("operation");
          sql = schema_sql(form->get("schema"), form->get("table"), op,
                           op == "add" ? form->get("new") : form->get("column"), form->get("new"),
                           form->get("type"));
        } else {
          std::vector<Column> cols;
          for (auto &id : *columns)
            cols.push_back({form->get("name:" + id), form->get("type:" + id),
                            form->checked("pk:" + id), false, form->checked("nullable:" + id)});
          sql = create_table_sql(form->get("schema"), form->get("table"), cols);
        }
        if (auto w = weak.lock())
          w->write_review(
              "Apply schema change?", sql + "\n\nThis can lock tables and permanently change data.",
              [weak, form, sql, alive = form->alive] {
                if (auto w = weak.lock()) {
                  form->working();
                  if (!w->session->submit([sql](Session &s) { return s.driver->execute(sql); },
                                          [weak, form, alive](Page, std::string error) {
                                            if (!*alive)
                                              return;
                                            form->report(error);
                                            if (error.empty()) {
                                              form->close();
                                              if (auto w = weak.lock()) {
                                                w->clear();
                                                w->table.reset();
                                                w->schemas_load();
                                              }
                                            }
                                          }))
                    form->report("Operation already running");
                }
              });
      });
      form->present();
    }
  });
}
void Workspace::file(bool save, bool json) {
  auto *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, save ? "Export current page only" : "Open SQL file");
  if (save)
    gtk_file_dialog_set_initial_name(dialog, json ? "puff-page.json" : "puff-page.csv");
  struct C {
    std::weak_ptr<Workspace> w;
    bool save, json;
    Page page;
  };
  auto *ctx = new C{weak_from_this(), save, json, save ? page : Page{}};
  auto done = +[](GObject *object, GAsyncResult *result, gpointer data) {
    std::unique_ptr<C> ctx(static_cast<C *>(data));
    GError *error = nullptr;
    auto *file = ctx->save ? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(object), result, &error)
                           : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(object), result, &error);
    if (!file) {
      if (error)
        g_error_free(error);
      return;
    }
    gchar *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path)
      return;
    std::string filename = path;
    g_free(path);
    if (auto w = ctx->w.lock(); w && !w->disposed) {
      if (ctx->save) {
        auto p = std::move(ctx->page);
        bool json = ctx->json;
        w->job([filename, p = std::move(p), json](Session &) {
          export_page(filename, p, json);
          return Page{};
        });
      } else {
        auto weak = ctx->w;
        w->job(
            [filename](Session &) {
              if (std::filesystem::file_size(filename) > 1000000)
                throw std::runtime_error("SQL files are limited to 1 MiB");
              std::ifstream in(filename);
              if (!in)
                throw std::runtime_error("Cannot read SQL file");
              Page p;
              p.message = std::string(std::istreambuf_iterator<char>(in), {});
              if (!g_utf8_validate(p.message.data(), p.message.size(), nullptr))
                throw std::runtime_error("SQL file must use UTF-8 encoding");
              return p;
            },
            [weak](Page p) {
              if (auto w = weak.lock())
                gtk_text_buffer_set_text(GTK_TEXT_BUFFER(w->buffer), p.message.c_str(),
                                         p.message.size());
            });
      }
    }
  };
  if (save)
    gtk_file_dialog_save(dialog, app.parent(), nullptr, done, ctx);
  else
    gtk_file_dialog_open(dialog, app.parent(), nullptr, done, ctx);
  g_object_unref(dialog);
}
void Workspace::backup_file(bool restore) {
  if (profile().engine != "postgres" || (restore && profile().readonly)) {
    gtk_label_set_text(GTK_LABEL(status),
                       "This workflow requires PostgreSQL; restore needs a writable connection.");
    return;
  }
  auto *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, restore ? "Choose PostgreSQL custom archive"
                                            : "Create PostgreSQL custom archive");
  if (!restore)
    gtk_file_dialog_set_initial_name(dialog, "puff-backup.dump");
  struct C {
    std::weak_ptr<Workspace> w;
    bool restore;
  };
  auto *ctx = new C{weak_from_this(), restore};
  auto done = +[](GObject *object, GAsyncResult *result, gpointer data) {
    std::unique_ptr<C> ctx(static_cast<C *>(data));
    GError *error = nullptr;
    auto *f = ctx->restore ? gtk_file_dialog_open_finish(GTK_FILE_DIALOG(object), result, &error)
                           : gtk_file_dialog_save_finish(GTK_FILE_DIALOG(object), result, &error);
    if (!f) {
      if (error)
        g_error_free(error);
      return;
    }
    gchar *path = g_file_get_path(f);
    g_object_unref(f);
    if (!path)
      return;
    std::string filename = path;
    g_free(path);
    auto weak = ctx->w;
    bool restore = ctx->restore;
    auto go = [weak, restore, filename] {
      if (auto w = weak.lock())
        w->job(
            [restore, filename](Session &s) {
              backup(s.profile, filename, restore, s.cancelled);
              Page p;
              p.message =
                  restore ? "Restore completed"
                          : "Backup completed; archive structure verified, not a full restore test";
              return p;
            },
            [weak](Page p) {
              if (auto w = weak.lock())
                gtk_label_set_text(GTK_LABEL(w->status), p.message.c_str());
            });
    };
    if (auto w = weak.lock(); w && !w->disposed) {
      if (restore)
        confirm(w->app.parent(), "Restore into " + w->profile().database + "?",
                "Destination: " + w->profile().host + ":" + std::to_string(w->profile().port) +
                    "/" + w->profile().database +
                    "\nCreates objects and imports data in one transaction. Conflicting objects "
                    "cause failure. Prefer an empty test database first.",
                w->profile().database, go);
      else
        go();
    }
  };
  if (restore)
    gtk_file_dialog_open(dialog, app.parent(), nullptr, done, ctx);
  else
    gtk_file_dialog_save(dialog, app.parent(), nullptr, done, ctx);
  g_object_unref(dialog);
}
void Workspace::history() {
  auto *form = new Form(app.parent(), "History and snippets");
  form->note("History is opt-in per tab. SQL can include sensitive values. Stored locally: at most "
             "200 queries and 100 snippets.");
  form->entry("search", "Search history");
  auto *items = box();
  gtk_box_append(GTK_BOX(form->fields), items);
  auto weak = weak_from_this();
  auto reload = [weak, form, items] {
    if (auto w = weak.lock()) {
      while (auto *child = gtk_widget_get_first_child(items))
        gtk_box_remove(GTK_BOX(items), child);
      auto entries = w->app.metadata().history(form->get("search"));
      auto snippets = w->app.metadata().snippets();
      entries.insert(entries.end(), snippets.begin(), snippets.end());
      for (auto &[name, sql] : entries)
        gtk_box_append(GTK_BOX(items), button(name + " · " + sql.substr(0, 55), [weak, form, sql] {
                         if (auto w = weak.lock())
                           gtk_text_buffer_set_text(GTK_TEXT_BUFFER(w->buffer), sql.c_str(),
                                                    sql.size());
                         form->close();
                       }));
    }
  };
  form->submit("Search", reload);
  form->submit("Clear history", [this, reload] {
    app.metadata().clear_history();
    reload();
  });
  reload();
  form->present();
}
void Workspace::snippet() {
  auto *form = new Form(app.parent(), "Save SQL snippet");
  form->entry("name", "Name");
  auto weak = weak_from_this();
  auto sql = sql_text();
  form->submit("Save", [weak, form, sql] {
    if (auto w = weak.lock()) {
      if (form->get("name").empty())
        throw std::runtime_error("Enter a snippet name");
      w->app.metadata().snippet(form->get("name"), sql);
    }
    form->close();
  });
  form->present();
}
void Workspace::dispose() {
  if (disposed)
    return;
  disposed = true;
  if (filter_source) {
    g_source_remove(filter_source);
    filter_source = 0;
  }
  session->close();
  if (rows)
    g_list_store_remove_all(rows);
  page = {};
  edits.clear();
}
} // namespace puff
namespace puff {
void Workspace::exercise(std::function<void(bool, std::string)> done) {
  struct Test {
    std::weak_ptr<Workspace> w;
    std::function<void(bool, std::string)> done;
    int phase = 0;
    bool scroll_checked = false;
    std::chrono::steady_clock::time_point resume{};
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
  };
  auto *test = new Test{weak_from_this(), std::move(done)};
  g_timeout_add_full(
      G_PRIORITY_DEFAULT, 25,
      +[](gpointer raw) -> gboolean {
        auto *t = static_cast<Test *>(raw);
        auto w = t->w.lock();
        if (!w)
          return G_SOURCE_REMOVE;
        try {
          if (std::chrono::steady_clock::now() > t->deadline)
            throw std::runtime_error("Native UI integration timed out: " +
                                     std::string(gtk_label_get_text(GTK_LABEL(w->status))));
          if (w->session->busy || std::chrono::steady_clock::now() < t->resume)
            return G_SOURCE_CONTINUE;
          auto require = [](bool value, const char *message) {
            if (!value)
              throw std::runtime_error(message);
          };
          switch (t->phase++) {
          case 0:
            require(w->schemas.size() == 1, "Schemas did not load");
            w->browse("main", "t");
            break;
          case 1:
            require(w->page.rows.size() == 200, "Table page is not bounded to 200 rows");
            require(w->page.columns.size() == 2, "Column metadata missing");
            if (g_getenv("PUFF_MEASURE")) {
              std::cout << "MEASURE large_page" << std::endl;
              t->resume = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
            w->next();
            break;
          case 2: {
            require(w->page.rows.at(0).at(0) == Cell("200"), "Pagination incorrect");
            std::function<bool(GtkWidget *, const std::string &)> has_label;
            has_label = [&has_label](GtkWidget *widget, const std::string &value) {
              if (GTK_IS_LABEL(widget) && value == gtk_label_get_text(GTK_LABEL(widget)))
                return true;
              for (auto *child = gtk_widget_get_first_child(widget); child;
                   child = gtk_widget_get_next_sibling(child))
                if (has_label(child, value))
                  return true;
              return false;
            };
            if (!t->scroll_checked) {
              require(has_label(GTK_WIDGET(w->view), "200"),
                      "Visible row label did not refresh after paging");
              auto *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(w->view));
              gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment) -
                                                       gtk_adjustment_get_page_size(adjustment));
              t->scroll_checked = true;
              --t->phase;
              t->resume = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
              break;
            }
            require(has_label(GTK_WIDGET(w->view), "399"),
                    "Cached offscreen labels were stale after scrolling");

            w->edit(0, 1);
            GList *windows = gtk_window_list_toplevels();
            Form *form = nullptr;
            for (auto *node = windows; node; node = node->next)
              if (auto *f =
                      static_cast<Form *>(g_object_get_data(G_OBJECT(node->data), "puff-form")))
                form = f;
            g_list_free(windows);
            require(form, "Cell edit dialog did not open");
            form->set("value", "native edit");
            gtk_check_button_set_active(GTK_CHECK_BUTTON(form->inputs["null"]), FALSE);
            auto *apply = gtk_widget_get_last_child(form->actions);
            g_signal_emit_by_name(apply, "clicked");
            require(!w->edits.empty(), "Cell edit did not stage");
            w->discard();
            require(w->edits.empty(), "Discard failed");
            w->execute("SELECT * FROM t");
            break;
          }
          case 3:
            require(w->page.rows.size() == 200 && w->page.more, "Raw SQL paging failed");
            w->next();
            break;
          case 4:
            require(w->page.rows.at(0).at(0) == Cell("200"), "Raw SQL next page failed");
            w->close_result();
            break;
          case 5: {
            require(w->page.rows.empty(), "Closing result retained rows");
            require(g_list_model_get_n_items(G_LIST_MODEL(w->rows)) == 0,
                    "Closing result retained model rows");
            w->schema_dialog(false);
            GList *windows = gtk_window_list_toplevels();
            for (auto *node = windows; node; node = node->next)
              if (auto *f =
                      static_cast<Form *>(g_object_get_data(G_OBJECT(node->data), "puff-form")))
                f->close();
            g_list_free(windows);
            if (g_getenv("PUFF_MEASURE")) {
              std::cout << "MEASURE released" << std::endl;
              t->resume = std::chrono::steady_clock::now() + std::chrono::seconds(2);
              break;
            }
            [[fallthrough]];
          }
          case 6: {
            w->app.create_database();
            GList *windows = gtk_window_list_toplevels();
            for (auto *node = windows; node; node = node->next)
              if (auto *f =
                      static_cast<Form *>(g_object_get_data(G_OBJECT(node->data), "puff-form")))
                gtk_window_set_default_size(f->window, 680,
                                            g_getenv("PUFF_CAPTURE_TALL") ? 680 : 480);
            g_list_free(windows);
            t->resume = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            break;
          }
          case 7: {
            Form *form = nullptr;
            GList *windows = gtk_window_list_toplevels();
            for (auto *node = windows; node; node = node->next)
              if (auto *f =
                      static_cast<Form *>(g_object_get_data(G_OBJECT(node->data), "puff-form")))
                form = f;
            g_list_free(windows);
            require(form, "Connection form did not open");
            graphene_rect_t bounds;
            require(gtk_widget_compute_bounds(form->actions, GTK_WIDGET(form->window), &bounds),
                    "Connection actions have no layout");
            require(bounds.size.height > 0 && bounds.origin.y >= 0 &&
                        bounds.origin.y + bounds.size.height <=
                            gtk_widget_get_height(GTK_WIDGET(form->window)),
                    "Connection actions are clipped in a short window");
            capture_test_widget(GTK_WIDGET(form->window), "connection");
            form->set("engine", "sqlite");
            require(!gtk_widget_get_visible(gtk_widget_get_parent(form->inputs.at("password"))),
                    "SQLite form exposes PostgreSQL credentials");
            form->set("engine", "postgres");
            form->set("port", "5432invalid");
            for (auto *action = gtk_widget_get_first_child(form->actions); action;
                 action = gtk_widget_get_next_sibling(action))
              if (std::string(gtk_button_get_label(GTK_BUTTON(action))) == "Test")
                g_signal_emit_by_name(action, "clicked");
            require(std::string(gtk_label_get_text(GTK_LABEL(form->error))).find("whole number") !=
                        std::string::npos,
                    "Invalid port was not rejected inline");
            form->close();
            auto *menu = GTK_MENU_BUTTON(g_object_get_data(G_OBJECT(w->root), "tools-menu"));
            gtk_menu_button_popup(menu);
            t->resume = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            break;
          }
          case 8: {
            auto *menu = GTK_MENU_BUTTON(g_object_get_data(G_OBJECT(w->root), "tools-menu"));
            auto *popover = gtk_menu_button_get_popover(menu);
            require(gtk_widget_get_width(GTK_WIDGET(popover)) >= 280,
                    "Database tools menu collapsed horizontally");
            capture_test_widget(GTK_WIDGET(popover), "database-tools");
            gtk_menu_button_popdown(menu);
            t->done(true, "PASS: compiled GTK UI connection, schema loading, virtual grid, "
                          "table/raw paging, cell staging/discard, schema dialog, result cleanup, "
                          "connection form layout/validation, tools menu width");
            return G_SOURCE_REMOVE;
          }
          }
        } catch (const std::exception &e) {
          t->done(false, e.what());
          return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
      },
      test, +[](gpointer p) { delete static_cast<Test *>(p); });
}
} // namespace puff
