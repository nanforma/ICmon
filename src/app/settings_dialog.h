// settings_dialog.h - the program settings window. Only what exists is offered: the folder for saved media.
#pragma once
#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace icmon {

// `current` is the folder in use, `fallback` the default shown by "Default". `on_save` runs on the GTK main thread with
// an absolute path. The window owns itself and is freed when closed.
// `language` is "", "en", "uk" or "ru"; `on_save` gets the folder and the language.
void show_settings_dialog(GtkWindow* parent, const std::string& current, const std::string& fallback, const std::string& language, bool maximized, bool hw_decoding,
                          std::function<void(const std::string&, const std::string&, bool, bool)> on_save);

}  // namespace icmon
