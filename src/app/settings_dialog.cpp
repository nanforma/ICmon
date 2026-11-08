// GtkFileChooserNative is deprecated since GTK 4.10 but is the system folder chooser available on GTK 4.6 (Mint 21).
#define GDK_DISABLE_DEPRECATION_WARNINGS
#include "settings_dialog.h"

#include "i18n.h"

#include <gst/gst.h>

#ifndef ICMON_WEBSITE
#define ICMON_WEBSITE "https://github.com/nanforma/ICmon"
#endif
#ifndef ICMON_VERSION
#define ICMON_VERSION "?"
#endif

namespace icmon {

static void set_margins(GtkWidget* w, int n) {
    gtk_widget_set_margin_top(w, n);
    gtk_widget_set_margin_bottom(w, n);
    gtk_widget_set_margin_start(w, n);
    gtk_widget_set_margin_end(w, n);
}

// Tabs: not focusable (no focus frame), as high and wide as the system's. The active tab is not a target of the pointer
// (a click on it does nothing anyway), so no theme can light it up under the cursor, whatever the theme is.
static void tabs_update_targets(GtkStack* stack, GtkWidget* sw) {
    GListModel* pages = G_LIST_MODEL(gtk_stack_get_pages(stack));
    GtkWidget* visible = gtk_stack_get_visible_child(stack);
    guint i = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(sw); c; c = gtk_widget_get_next_sibling(c), i++) {
        GtkStackPage* page = (GtkStackPage*)g_list_model_get_item(pages, i);
        bool active = page && gtk_stack_page_get_child(page) == visible;
        if (page) g_object_unref(page);
        gtk_widget_set_can_target(c, !active);
    }
    g_object_unref(pages);
}

static void style_tab_switcher(GtkWidget* sw, GtkWidget* stack) {
    gtk_widget_add_css_class(sw, "tab-switcher");
    gtk_widget_set_size_request(sw, 280, 34);
    for (GtkWidget* c = gtk_widget_get_first_child(sw); c; c = gtk_widget_get_next_sibling(c)) gtk_widget_set_focusable(c, FALSE);
    tabs_update_targets(GTK_STACK(stack), sw);
    g_signal_connect(stack, "notify::visible-child", G_CALLBACK(+[](GObject* st, GParamSpec*, gpointer swp) {
        tabs_update_targets(GTK_STACK(st), GTK_WIDGET(swp));
    }), sw);
}

// A freshly opened dialog must not show the text of its first field selected.
static void deselect_first_field(GtkWidget* win) {
    g_object_ref(win);
    g_idle_add(+[](gpointer p) -> gboolean {
        GtkWidget* w = (GtkWidget*)p;
        GtkWidget* f = gtk_window_get_focus(GTK_WINDOW(w));
        if (f) {
            GtkWidget* e = GTK_IS_EDITABLE(f) ? f : gtk_widget_get_ancestor(f, GTK_TYPE_EDITABLE);
            if (e && GTK_IS_EDITABLE(e)) gtk_editable_select_region(GTK_EDITABLE(e), 0, 0);
        }
        g_object_unref(w);
        return G_SOURCE_REMOVE;
    }, win);
}

namespace {
struct Dlg {
    GtkWidget *win = nullptr, *entry = nullptr, *msg = nullptr;
    GtkNativeDialog* native = nullptr;  // GTK 4.6 path: the chooser while it is open
    bool choosing = false;              // GTK 4.10+ path
    std::string fallback;
    std::function<void(const std::string&, const std::string&, bool, bool)> on_save;
    GtkWidget *lang = nullptr, *maximize = nullptr, *hwdec = nullptr;
};

const char* const kLangCodes[4] = {"", "en", "uk", "ru"};

// The system's folder chooser. GTK 4.10+: GtkFileDialog (asynchronous, always answers, also when closed with the X).
// GTK 4.6 (Mint 21): GtkFileChooserNative, which opens the desktop portal's own dialog, the same one the other GTK4
// programs show. Whatever the answer (accept, cancel, closed with the X, any other code) the native object is
// destroyed and released in the response handler. NOT verified on a real desktop by the author.
void apply_folder(Dlg* d, GFile* f) {
    if (!f) return;
    char* path = g_file_get_path(f);
    if (path) gtk_editable_set_text(GTK_EDITABLE(d->entry), path);
    g_free(path);
}

#if GTK_CHECK_VERSION(4, 10, 0)
void folder_done(GObject* src, GAsyncResult* res, gpointer p) {
    Dlg* d = (Dlg*)p;
    GError* err = nullptr;
    GFile* f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);  // an error here is "dismissed"
    g_clear_error(&err);
    if (f) { apply_folder(d, f); g_object_unref(f); }
    d->choosing = false;
}

void choose_clicked(GtkButton*, gpointer p) {
    Dlg* d = (Dlg*)p;
    if (d->choosing) return;
    d->choosing = true;
    GtkFileDialog* fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, _("Folder for saved media"));
    gtk_file_dialog_select_folder(fd, GTK_WINDOW(d->win), nullptr, folder_done, d);
    g_object_unref(fd);
}
#else
void choose_response(GtkNativeDialog* nd, int response, gpointer p) {
    Dlg* d = (Dlg*)p;
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile* f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(nd));
        if (f) { apply_folder(d, f); g_object_unref(f); }
    }
    d->native = nullptr;
    gtk_native_dialog_destroy(nd);
    g_object_unref(nd);
}

void choose_clicked(GtkButton*, gpointer p) {
    Dlg* d = (Dlg*)p;
    if (d->native) return;  // already open
    d->native = gtk_file_chooser_native_new(_("Folder for saved media"), GTK_WINDOW(d->win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                            _("Select"), _("Cancel"));
    gtk_native_dialog_set_modal(d->native, TRUE);
    g_signal_connect(d->native, "response", G_CALLBACK(choose_response), d);
    gtk_native_dialog_show(d->native);
}
#endif

void default_clicked(GtkButton*, gpointer p) {
    Dlg* d = (Dlg*)p;
    gtk_editable_set_text(GTK_EDITABLE(d->entry), d->fallback.c_str());
}

void cancel_clicked(GtkButton*, gpointer p) { gtk_window_destroy(GTK_WINDOW(((Dlg*)p)->win)); }

void save_clicked(GtkButton*, gpointer p) {
    Dlg* d = (Dlg*)p;
    std::string path = gtk_editable_get_text(GTK_EDITABLE(d->entry));
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path.empty() || path[0] != '/') {
        gtk_label_set_text(GTK_LABEL(d->msg), _("Enter an absolute folder path (it starts with /)."));
        return;
    }
    guint li = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->lang));
    if (d->on_save) d->on_save(path, kLangCodes[li < 4 ? li : 0], gtk_switch_get_active(GTK_SWITCH(d->maximize)), gtk_switch_get_active(GTK_SWITCH(d->hwdec)));
    gtk_window_destroy(GTK_WINDOW(d->win));
}

void destroyed(GtkWidget*, gpointer p) {
    Dlg* d = (Dlg*)p;
    if (d->native) {
        g_signal_handlers_disconnect_by_data(d->native, d);
        gtk_native_dialog_destroy(d->native);
        g_object_unref(d->native);
    }
    delete d;
}
}  // namespace

void show_settings_dialog(GtkWindow* parent, const std::string& current, const std::string& fallback, const std::string& language, bool maximized, bool hw_decoding,
                          std::function<void(const std::string&, const std::string&, bool, bool)> on_save) {
    Dlg* d = new Dlg();
    d->fallback = fallback;
    d->on_save = std::move(on_save);
    d->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(d->win), _("Settings"));
    gtk_window_set_transient_for(GTK_WINDOW(d->win), parent);
    gtk_window_set_modal(GTK_WINDOW(d->win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(d->win), 520, -1);  // the height is the content's; every label has a fixed size
    g_signal_connect(d->win, "destroy", G_CALLBACK(destroyed), d);

    // Two tabs: "General" (the settings) and "About"; Cancel / Save stay below them.
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    set_margins(outer, 16);
    gtk_window_set_child(GTK_WINDOW(d->win), outer);
    GtkWidget* tabs = gtk_stack_new();  // plain GtkStackSwitcher: the theme draws the tabs
    gtk_widget_set_vexpand(tabs, TRUE);
    GtkWidget* switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(tabs));
    gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(switcher, 10);
    gtk_box_append(GTK_BOX(outer), switcher);
    gtk_box_append(GTK_BOX(outer), tabs);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_stack_add_titled(GTK_STACK(tabs), box, "general", _("General"));

    GtkWidget* about = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_stack_add_titled(GTK_STACK(tabs), about, "about", _("About"));
    style_tab_switcher(switcher, tabs);
    gtk_widget_set_margin_top(about, 28);
    {
        GtkWidget* an = gtk_label_new("ICmon");
        gtk_widget_add_css_class(an, "title-2");
        gtk_widget_set_halign(an, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(about), an);
        std::string ver = std::string(_("Version")) + " " + ICMON_VERSION;
        GtkWidget* av = gtk_label_new(ver.c_str());
        gtk_widget_set_halign(av, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(about), av);
        GtkWidget* ad = gtk_label_new(_("A native Linux client for iCSee-compatible IP cameras: live view, playback and download of recordings, PTZ control and saved positions."));
        gtk_label_set_wrap(GTK_LABEL(ad), TRUE);
        gtk_label_set_xalign(GTK_LABEL(ad), 0.5);
        gtk_label_set_justify(GTK_LABEL(ad), GTK_JUSTIFY_CENTER);
        gtk_widget_set_size_request(ad, 488, 60);
        gtk_widget_set_margin_top(ad, 6);
        gtk_box_append(GTK_BOX(about), ad);
        std::string libs = std::string(_("Protocols")) + ": DVRIP (Sofia, TCP 34567), ONVIF, RTSP\n" +
                           "GTK " + std::to_string(gtk_get_major_version()) + "." + std::to_string(gtk_get_minor_version()) + "." +
                           std::to_string(gtk_get_micro_version());
        gchar* gv = gst_version_string();
        libs += std::string("\n") + gv;
        g_free(gv);
        GtkWidget* al = gtk_label_new(libs.c_str());
        gtk_label_set_xalign(GTK_LABEL(al), 0.5);
        gtk_label_set_justify(GTK_LABEL(al), GTK_JUSTIFY_CENTER);
        gtk_widget_add_css_class(al, "dim-label");
            gtk_box_append(GTK_BOX(about), al);
        // placeholder address of the project page: replace with the real repository
        GtkWidget* gh = gtk_link_button_new_with_label(ICMON_WEBSITE, "GitHub");
        gtk_widget_set_halign(gh, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(gh, 18);
        gtk_box_append(GTK_BOX(about), gh);
    }

    GtkWidget* title = gtk_label_new(_("Folder for saved pictures and video"));
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_add_css_class(title, "side-title");
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d->entry = gtk_entry_new();
    gtk_widget_set_hexpand(d->entry, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(d->entry), current.c_str());
    GtkWidget* choose = gtk_button_new_from_icon_name("folder-open-symbolic");  // the system's folder icon
    gtk_widget_set_tooltip_text(choose, _("Choose a folder"));
    GtkWidget* def = gtk_button_new_with_label(_("Default"));
    g_signal_connect(choose, "clicked", G_CALLBACK(choose_clicked), d);
    g_signal_connect(def, "clicked", G_CALLBACK(default_clicked), d);
    gtk_box_append(GTK_BOX(row), d->entry);
    gtk_box_append(GTK_BOX(row), choose);
    gtk_box_append(GTK_BOX(row), def);
    gtk_box_append(GTK_BOX(box), row);

    GtkWidget* hint = gtk_label_new(_("Saved frames go to the subfolder \"picture\", downloaded recordings to \"video\"."));
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_size_request(hint, 488, 40);  // fixed: a wrapping label made the window re-measure and jump
    gtk_widget_set_valign(hint, GTK_ALIGN_START);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(box), hint);

    GtkWidget* ltitle = gtk_label_new(_("Language"));
    gtk_widget_set_halign(ltitle, GTK_ALIGN_START);
    gtk_widget_add_css_class(ltitle, "side-title");
    gtk_widget_set_margin_top(ltitle, 8);
    gtk_box_append(GTK_BOX(box), ltitle);
    const char* names[] = {_("System default"), "English", "Українська", "Русский", nullptr};
    d->lang = gtk_drop_down_new_from_strings(names);
    for (guint i = 0; i < 4; i++)
        if (language == kLangCodes[i]) gtk_drop_down_set_selected(GTK_DROP_DOWN(d->lang), i);
    gtk_widget_set_halign(d->lang, GTK_ALIGN_START);  // half the width of the window, not stretched
    gtk_widget_set_size_request(d->lang, 220, -1);
    gtk_box_append(GTK_BOX(box), d->lang);
    GtkWidget* lhint = gtk_label_new(_("The language changes after the program is restarted."));
    gtk_label_set_xalign(GTK_LABEL(lhint), 0);
    gtk_label_set_wrap(GTK_LABEL(lhint), TRUE);
    gtk_widget_set_size_request(lhint, 488, 22);
    gtk_widget_add_css_class(lhint, "dim-label");
    gtk_box_append(GTK_BOX(box), lhint);

    GtkWidget* mrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(mrow, 8);
    GtkWidget* mlab = gtk_label_new(_("Start with the window maximized"));
    gtk_widget_set_hexpand(mlab, TRUE);
    gtk_label_set_xalign(GTK_LABEL(mlab), 0);
    d->maximize = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(d->maximize), maximized);
    gtk_box_append(GTK_BOX(mrow), mlab);
    gtk_box_append(GTK_BOX(mrow), d->maximize);
    gtk_box_append(GTK_BOX(box), mrow);

    GtkWidget* hrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(hrow, 8);
    GtkWidget* hlab = gtk_label_new(_("Hardware video decoding"));
    gtk_widget_set_hexpand(hlab, TRUE);
    gtk_label_set_xalign(GTK_LABEL(hlab), 0);
    d->hwdec = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(d->hwdec), hw_decoding);
    gtk_box_append(GTK_BOX(hrow), hlab);
    gtk_box_append(GTK_BOX(hrow), d->hwdec);
    gtk_box_append(GTK_BOX(box), hrow);
    GtkWidget* hhint = gtk_label_new(_("Off: the processor decodes the video (the video card stays cooler). Takes effect after the program is restarted."));
    gtk_label_set_xalign(GTK_LABEL(hhint), 0);
    gtk_label_set_wrap(GTK_LABEL(hhint), TRUE);
    gtk_widget_set_size_request(hhint, 488, 44);
    gtk_widget_add_css_class(hhint, "dim-label");
    gtk_box_append(GTK_BOX(box), hhint);

    d->msg = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->msg), 0);
    gtk_widget_set_vexpand(d->msg, TRUE);  // extra height goes here: the buttons follow the bottom edge
    gtk_widget_set_valign(d->msg, GTK_ALIGN_START);
    gtk_widget_set_size_request(d->msg, -1, 22);  // room for one line, so a message never changes the window size
    gtk_box_append(GTK_BOX(box), d->msg);

    GtkWidget* btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    GtkWidget* cancel = gtk_button_new_with_label(_("Cancel"));
    GtkWidget* save = gtk_button_new_with_label(_("Save"));
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_clicked), d);
    g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), d);
    gtk_box_append(GTK_BOX(btns), cancel);
    gtk_box_append(GTK_BOX(btns), save);
    gtk_widget_set_margin_top(btns, 10);
    gtk_box_append(GTK_BOX(outer), btns);
    gtk_window_present(GTK_WINDOW(d->win));
    deselect_first_field(d->win);
}

}  // namespace icmon
