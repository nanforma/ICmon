#include <malloc.h>
#include <gtk/gtk.h>
#include <locale.h>

#include <libgen.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>

#include "camera_store.h"
#include "i18n.h"
#include "main_window.h"

static std::string g_exe_dir;  // folder of the program

static void on_activate(GtkApplication* app, gpointer) {
    // The program icon: <exe folder>/icons (build tree), <exe folder>/../share/icons (installed, AppImage), else the system's.
    GtkIconTheme* theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (!g_exe_dir.empty()) {
        gtk_icon_theme_add_search_path(theme, (g_exe_dir + "/icons").c_str());
        gtk_icon_theme_add_search_path(theme, (g_exe_dir + "/../share/icons").c_str());
    }
    gtk_window_set_default_icon_name("icmon");
    new icmon::MainWindow(app);  // owns itself: deleted when its window is destroyed
}

int main(int argc, char** argv) {
    // Video frames are megabytes each: keep big blocks out of the shared heap so freeing them returns memory to the system
    // (the heap otherwise grows with every camera restart), and use few arenas for the streaming threads.
    mallopt(M_MMAP_THRESHOLD, 1 << 20);
    mallopt(M_TRIM_THRESHOLD, 4 << 20);
    mallopt(M_ARENA_MAX, 2);
    // Language chosen in Settings (read before anything is translated): LANGUAGE overrides the system's.
    std::string lang = icmon::read_language(icmon::default_config_dir() + "/cameras.json");
    if (!lang.empty()) setenv("LANGUAGE", lang.c_str(), 1);
    setlocale(LC_ALL, "");
    // Catalogs: <folder of the program>/locale (build tree), else <folder>/../share/locale (installed), else the system's.
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string dir;
    if (n > 0) {
        exe[n] = 0;
        std::string base = dirname(exe);
        g_exe_dir = base;
        if (access((base + "/locale").c_str(), R_OK) == 0) dir = base + "/locale";
        else if (access((base + "/../share/locale").c_str(), R_OK) == 0) dir = base + "/../share/locale";
    }
    bindtextdomain(GETTEXT_DOMAIN, dir.empty() ? nullptr : dir.c_str());
    textdomain(GETTEXT_DOMAIN);
    bind_textdomain_codeset(GETTEXT_DOMAIN, "UTF-8");

#if GLIB_CHECK_VERSION(2, 74, 0)
    GtkApplication* app = gtk_application_new("org.icmon.client", G_APPLICATION_DEFAULT_FLAGS);
#else
    GtkApplication* app = gtk_application_new("org.icmon.client", G_APPLICATION_FLAGS_NONE);
#endif
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
