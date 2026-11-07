// camera_dialog.h - modal window to add a camera or edit / delete an existing one (manual entry only).
// Searching for cameras on the local network is NOT implemented yet.
#pragma once
#include <gtk/gtk.h>

#include <functional>
#include <string>

#include "camera_store.h"

namespace icmon {

struct CameraDialogResult {
    CameraConfig cam;          // id is empty for a new camera: the caller assigns it
    bool password_given = false;
    std::string password;      // only meaningful when password_given
    bool save_to_keyring = true;
};

// The window owns itself and is freed when closed. on_save / on_delete run on the GTK main thread.
void show_camera_dialog(GtkWindow* parent, const CameraConfig* existing, bool keyring_compiled_in,
                        std::function<void(CameraDialogResult)> on_save, std::function<void(std::string)> on_delete);

// A small modal question with a cancel button and a confirm button; `on_yes` runs only on confirmation.
void show_confirm(GtkWindow* parent, const std::string& title, const std::string& text, const std::string& yes_label,
                  std::function<void()> on_yes);

}  // namespace icmon
