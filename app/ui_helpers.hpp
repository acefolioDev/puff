#pragma once
#include "ui.hpp"
#include <map>
namespace puff {
using Action = std::function<void()>;
inline void click(GtkWidget *w, Action action) {
  auto fn = new Action(std::move(action));
  g_signal_connect_data(
      w, "clicked", reinterpret_cast<GCallback>(+[](GtkButton *, gpointer p) {
        try {
          (*static_cast<Action *>(p))();
        } catch (const std::exception &e) {
          auto *d = gtk_alert_dialog_new("%s", e.what());
          gtk_alert_dialog_show(d, nullptr);
          g_object_unref(d);
        }
      }),
      fn, +[](gpointer p, GClosure *) { delete static_cast<Action *>(p); }, G_CONNECT_DEFAULT);
}
inline GtkWidget *button(const std::string &label, Action action, const char *css = nullptr) {
  auto *w = gtk_button_new_with_label(label.c_str());
  if (css)
    gtk_widget_add_css_class(w, css);
  click(w, std::move(action));
  return w;
}
inline GtkWidget *box(bool vertical = true, int space = 8) {
  return gtk_box_new(vertical ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, space);
}
inline GtkWidget *padded(GtkWidget *w, int p = 12) {
  gtk_widget_set_margin_top(w, p);
  gtk_widget_set_margin_bottom(w, p);
  gtk_widget_set_margin_start(w, p);
  gtk_widget_set_margin_end(w, p);
  return w;
}
inline GtkWidget *label(const std::string &s, bool wrap = false) {
  auto *w = gtk_label_new(s.c_str());
  gtk_label_set_xalign(GTK_LABEL(w), 0);
  gtk_label_set_wrap(GTK_LABEL(w), wrap);
  return w;
}
inline std::string preview(const std::string &s, size_t maximum) {
  auto part = s.substr(0, maximum);
  gchar *valid = g_utf8_make_valid(part.data(), part.size());
  std::string out = valid;
  g_free(valid);
  return out;
}
inline std::string entry_text(GtkWidget *w) {
  return gtk_editable_get_text(GTK_EDITABLE(w));
}
inline void confirm(GtkWindow *, const std::string &, const std::string &, const std::string &,
                    Action);

struct Form {
  GtkWindow *window;
  GtkWidget *fields, *error, *actions;
  std::map<std::string, GtkWidget *> inputs;
  std::map<std::string, std::vector<std::string>> options;
  bool busy = false;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  Form(GtkWindow *parent, const std::string &title) {
    window = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(window, title.c_str());
    gtk_window_set_transient_for(window, parent);
    gtk_window_set_modal(window, TRUE);
    gtk_window_set_destroy_with_parent(window, TRUE);
    gtk_window_set_default_size(window, 680, 680);
    auto *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
    auto *content = padded(box(), 20);
    fields = box();
    gtk_box_append(GTK_BOX(content), fields);
    error = label("", true);
    gtk_label_set_selectable(GTK_LABEL(error), TRUE);
    gtk_widget_add_css_class(error, "error");
    gtk_widget_set_visible(error, FALSE);

    actions = box(false);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(actions), button("Cancel", [this] { close(); }));
    auto *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);
    auto *footer = padded(box(), 16);
    gtk_box_append(GTK_BOX(footer), error);
    gtk_box_append(GTK_BOX(footer), actions);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), footer);
    adw_window_set_content(ADW_WINDOW(window), view);
    g_signal_connect(window, "close-request",
                     reinterpret_cast<GCallback>(+[](GtkWindow *, gpointer p) -> gboolean {
                       return static_cast<Form *>(p)->busy;
                     }),
                     this);
    g_object_set_data_full(
        G_OBJECT(window), "puff-form", this, +[](gpointer p) { delete static_cast<Form *>(p); });
  }
  ~Form() {
    *alive = false;
  }
  void note(const std::string &s) {
    gtk_box_append(GTK_BOX(fields), label(s, true));
  }
  GtkWidget *entry(const std::string &key, const std::string &title, const std::string &value = "",
                   bool secret = false) {
    auto *b = box();
    auto *caption = label(title);
    gtk_box_append(GTK_BOX(b), caption);
    auto *e = secret ? gtk_password_entry_new() : gtk_entry_new();
    if (secret)
      gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(e), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(caption), e);
    gtk_editable_set_text(GTK_EDITABLE(e), value.c_str());
    gtk_box_append(GTK_BOX(b), e);
    gtk_box_append(GTK_BOX(fields), b);
    inputs[key] = e;
    return e;
  }
  GtkWidget *choice(const std::string &key, const std::string &title,
                    const std::vector<std::string> &values, const std::string &value = "") {
    auto *b = box();
    auto *caption = label(title);
    gtk_box_append(GTK_BOX(b), caption);
    std::vector<const char *> c;
    for (auto &v : values)
      c.push_back(v.c_str());
    c.push_back(nullptr);
    auto *w = gtk_drop_down_new_from_strings(c.data());
    gtk_label_set_mnemonic_widget(GTK_LABEL(caption), w);
    options[key] = values;
    for (size_t i = 0; i < values.size(); ++i)
      if (values[i] == value)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w), i);
    gtk_box_append(GTK_BOX(b), w);
    gtk_box_append(GTK_BOX(fields), b);
    inputs[key] = w;
    return w;
  }
  void check(const std::string &key, const std::string &title, bool value) {
    auto *w = gtk_check_button_new_with_label(title.c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(w), value);
    gtk_box_append(GTK_BOX(fields), w);
    inputs[key] = w;
  }
  std::string get(const std::string &key) {
    auto *w = inputs.at(key);
    if (GTK_IS_DROP_DOWN(w)) {
      auto i = gtk_drop_down_get_selected(GTK_DROP_DOWN(w));
      return i < options[key].size() ? options[key][i] : "";
    }
    return entry_text(w);
  }
  bool checked(const std::string &key) {
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(inputs.at(key)));
  }
  void set(const std::string &key, const std::string &value) {
    auto *w = inputs.at(key);
    if (GTK_IS_DROP_DOWN(w)) {
      for (size_t i = 0; i < options[key].size(); ++i)
        if (options[key][i] == value)
          gtk_drop_down_set_selected(GTK_DROP_DOWN(w), i);
    } else
      gtk_editable_set_text(GTK_EDITABLE(w), value.c_str());
  }
  void submit(const std::string &title, Action action) {
    gtk_box_append(GTK_BOX(actions), button(
                                         title,
                                         [this, action] {
                                           auto still_alive = alive;
                                           try {
                                             action();
                                           } catch (const std::exception &e) {
                                             if (*still_alive)
                                               report(e.what());
                                           }
                                         },
                                         "suggested-action"));
  }
  void working() {
    busy = true;
    gtk_widget_set_sensitive(actions, FALSE);
    gtk_widget_set_sensitive(fields, FALSE);
    gtk_widget_set_visible(error, TRUE);
    gtk_widget_remove_css_class(error, "error");
    gtk_widget_remove_css_class(error, "success");
    gtk_label_set_text(GTK_LABEL(error), "Working…");
  }
  void report(const std::string &s, bool success = false) {
    busy = false;
    gtk_widget_set_visible(error, !s.empty());
    gtk_widget_remove_css_class(error, success ? "error" : "success");
    gtk_widget_add_css_class(error, success ? "success" : "error");
    gtk_widget_set_sensitive(actions, TRUE);
    gtk_widget_set_sensitive(fields, TRUE);
    gtk_label_set_text(GTK_LABEL(error), s.c_str());
  }
  void close() {
    if (!busy)
      gtk_window_destroy(window);
  }
  void present() {
    gtk_window_present(window);
  }
};
// Keep unbounded SQL out of a message dialog's heading/body measurement.
// Scroll only the preview; confirmation and cancellation stay in the fixed footer.
inline void confirm(GtkWindow *parent, const std::string &title, const std::string &body,
                    const std::string &target, Action action) {
  auto *form = new Form(parent, title);
  gtk_window_set_default_size(form->window, 820, 600);
  form->note("Review the operation before applying it.");
  if (body.size() > 400 || body.find('\n') != std::string::npos) {
    auto *source = gtk_source_view_new();
    auto *buffer = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(source)));
    gtk_source_buffer_set_language(buffer, gtk_source_language_manager_get_language(
                                               gtk_source_language_manager_get_default(), "sql"));
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buffer), body.data(), body.size());
    gtk_text_view_set_editable(GTK_TEXT_VIEW(source), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(source), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(source), TRUE);
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(source), TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(source), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Operation preview", -1);
    auto *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 280);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), source);
    gtk_widget_add_css_class(scroll, "frame");
    gtk_box_append(GTK_BOX(form->fields), scroll);
    g_object_set_data(G_OBJECT(form->window), "review-source", source);
  } else {
    auto *description = label(body, true);
    gtk_label_set_selectable(GTK_LABEL(description), TRUE);
    gtk_box_append(GTK_BOX(form->fields), description);
  }
  GtkWidget *entry = nullptr;
  if (!target.empty())
    entry = form->entry("confirmation", "Type “" + target + "” to confirm");
  form->submit("Apply", [form, action, alive = form->alive] {
    auto run = action;
    auto still_alive = alive;
    run();
    if (*still_alive)
      form->close();
  });
  auto *apply = gtk_widget_get_last_child(form->actions);
  gtk_widget_remove_css_class(apply, "suggested-action");
  gtk_widget_add_css_class(apply, "destructive-action");
  if (entry) {
    gtk_widget_set_sensitive(apply, FALSE);
    auto *ctx = new std::pair<GtkWidget *, std::string>(apply, target);
    g_signal_connect_data(
        entry, "changed", reinterpret_cast<GCallback>(+[](GtkEditable *e, gpointer data) {
          const auto &[apply, target] = *static_cast<std::pair<GtkWidget *, std::string> *>(data);
          gtk_widget_set_sensitive(apply, entry_text(GTK_WIDGET(e)) == target);
        }),
        ctx,
        +[](gpointer data, GClosure *) {
          delete static_cast<std::pair<GtkWidget *, std::string> *>(data);
        },
        G_CONNECT_DEFAULT);
  }
  auto *cancel = gtk_widget_get_first_child(form->actions);
  gtk_window_set_default_widget(form->window, cancel);
  form->present();
  gtk_widget_grab_focus(cancel);
}
} // namespace puff
