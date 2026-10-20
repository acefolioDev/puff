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
inline void confirm(GtkWindow *parent, const std::string &title, const std::string &body,
                    const std::string &target, Action action) {
  auto *d = adw_message_dialog_new(parent, title.c_str(), body.c_str());
  adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(d), "cancel", "Cancel");
  adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(d), "apply", "Apply");
  adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(d), "apply",
                                             ADW_RESPONSE_DESTRUCTIVE);
  adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(d), "cancel");
  adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(d), "cancel");
  if (!target.empty()) {
    auto *e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), ("Type " + target + " to confirm").c_str());
    adw_message_dialog_set_extra_child(ADW_MESSAGE_DIALOG(d), e);
    adw_message_dialog_set_response_enabled(ADW_MESSAGE_DIALOG(d), "apply", FALSE);
    auto *ctx = new std::pair<GtkWidget *, std::string>(d, target);
    g_signal_connect_data(
        e, "changed", reinterpret_cast<GCallback>(+[](GtkEditable *e, gpointer p) {
          auto &[d, target] = *static_cast<std::pair<GtkWidget *, std::string> *>(p);
          adw_message_dialog_set_response_enabled(ADW_MESSAGE_DIALOG(d), "apply",
                                                  entry_text(GTK_WIDGET(e)) == target);
        }),
        ctx,
        +[](gpointer p, GClosure *) {
          delete static_cast<std::pair<GtkWidget *, std::string> *>(p);
        },
        G_CONNECT_DEFAULT);
  }
  auto *fn = new Action(std::move(action));
  g_signal_connect_data(
      d, "response",
      reinterpret_cast<GCallback>(+[](AdwMessageDialog *, const char *response, gpointer p) {
        if (std::string(response) == "apply")
          (*static_cast<Action *>(p))();
      }),
      fn, +[](gpointer p, GClosure *) { delete static_cast<Action *>(p); }, G_CONNECT_DEFAULT);
  gtk_window_present(GTK_WINDOW(d));
}
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
                                           try {
                                             action();
                                           } catch (const std::exception &e) {
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
} // namespace puff
