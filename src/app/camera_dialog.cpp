#include "camera_dialog.h"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "discovery.h"
#include "i18n.h"
#include "rtsp_url.h"

namespace icmon {

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
    GtkWidget* win = nullptr;
    GtkWidget *name = nullptr, *host = nullptr, *dvrip_port = nullptr, *user = nullptr, *pass = nullptr;
    GtkWidget *no_pass = nullptr, *keyring = nullptr, *rtsp_port = nullptr, *channel = nullptr, *main_stream = nullptr;
    GtkWidget *sub_stream = nullptr, *tmpl = nullptr, *sub_grid = nullptr, *ptz_flip = nullptr, *ptz_flip_v = nullptr, *buttons = nullptr;
    GtkWidget *error = nullptr, *del_btn = nullptr;
    GtkWidget *found_list = nullptr, *search_btn = nullptr, *search_status = nullptr;
    std::vector<FoundCamera> found;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<xm::CancelToken> cancel = std::make_shared<xm::CancelToken>();
    bool editing = false, delete_armed = false;
    CameraConfig original;
    std::function<void(CameraDialogResult)> on_save;
    std::function<void(std::string)> on_delete;
};

GtkWidget* spin(int lo, int hi, int val) {
    GtkWidget* s = gtk_spin_button_new_with_range(lo, hi, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), val);
    return s;
}

void row(GtkWidget* grid, int r, const char* label, GtkWidget* w) {
    GtkWidget* l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w, 1, r, 1, 1);
}

void fail(Dlg* d, const std::string& m) { gtk_label_set_text(GTK_LABEL(d->error), m.c_str()); }

void save_clicked(GtkButton*, gpointer p) {
    Dlg* d = (Dlg*)p;
    CameraConfig c = d->original;
    c.name = gtk_editable_get_text(GTK_EDITABLE(d->name));
    c.host = gtk_editable_get_text(GTK_EDITABLE(d->host));
    c.user = gtk_editable_get_text(GTK_EDITABLE(d->user));
    c.dvrip_port = atoi(gtk_editable_get_text(GTK_EDITABLE(d->dvrip_port)));
    c.rtsp_port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->rtsp_port));
    c.channel = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->channel));
    c.main_stream = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->main_stream));
    c.sub_stream = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->sub_stream));
    c.use_sub_in_grid = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->sub_grid));
    c.no_password = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->no_pass));
    if (d->ptz_flip) c.ptz_flip_h = gtk_switch_get_active(GTK_SWITCH(d->ptz_flip));
    if (d->ptz_flip_v) c.ptz_flip_v = gtk_switch_get_active(GTK_SWITCH(d->ptz_flip_v));
    c.rtsp_template = gtk_editable_get_text(GTK_EDITABLE(d->tmpl));

    std::string pw = gtk_editable_get_text(GTK_EDITABLE(d->pass));
    for (unsigned char ch : pw)
        if (ch >= 0x80) {  // wrong keyboard layout is the usual cause; a failed login can lock the camera account
            fail(d, _("The password contains non-ASCII characters (wrong keyboard layout?). Nothing was saved."));
            return;
        }
    std::string why = validate_camera(c);
    if (!why.empty()) { fail(d, why); return; }
    if (!d->editing && !c.no_password && pw.empty()) {
        fail(d, _("Enter the password, or tick \"The camera has no password\"."));
        return;
    }
    CameraDialogResult r;
    r.cam = c;
    r.password_given = !c.no_password && !pw.empty();
    r.password = pw;
    r.save_to_keyring = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->keyring));
    auto cb = d->on_save;
    GtkWidget* win = d->win;
    gtk_window_destroy(GTK_WINDOW(win));  // frees d
    if (cb) cb(std::move(r));
}

void cancel_clicked(GtkButton*, gpointer p) { gtk_window_destroy(GTK_WINDOW(((Dlg*)p)->win)); }

void delete_clicked(GtkButton*, gpointer p) {
    Dlg* d = (Dlg*)p;
    if (!d->delete_armed) {  // a second click confirms
        d->delete_armed = true;
        gtk_button_set_label(GTK_BUTTON(d->del_btn), _("Click again to delete"));
        return;
    }
    auto cb = d->on_delete;
    std::string id = d->original.id;
    gtk_window_destroy(GTK_WINDOW(d->win));
    if (cb) cb(id);
}

void destroyed(GtkWidget*, gpointer p) {
    Dlg* d = (Dlg*)p;
    *d->alive = false;  // a search still running must not touch the freed window
    d->cancel->cancel();
    delete d;
}

constexpr int kNameColumn = 190;  // width of the Name column of the search table

struct SearchResult {
    std::shared_ptr<std::atomic<bool>> alive;
    Dlg* d;
    std::vector<FoundCamera> found;
    std::string error;
};

gboolean search_done(gpointer p) {
    std::unique_ptr<SearchResult> r((SearchResult*)p);
    if (!*r->alive) return G_SOURCE_REMOVE;
    Dlg* d = r->d;
    gtk_widget_set_sensitive(d->search_btn, TRUE);
    GtkWidget* c;
    while ((c = gtk_widget_get_first_child(d->found_list))) gtk_list_box_remove(GTK_LIST_BOX(d->found_list), c);
    d->found = r->found;
    if (!r->error.empty()) {
        gtk_label_set_text(GTK_LABEL(d->search_status), (std::string(_("Search failed: ")) + r->error).c_str());
    } else if (d->found.empty()) {
        gtk_label_set_text(GTK_LABEL(d->search_status),
                           _("No cameras answered. Not every camera replies to the search, and a router may block it: enter the address by hand."));
    } else {
        gtk_label_set_text(GTK_LABEL(d->search_status), _("Click a camera in the list to fill in its address."));
    }
    for (const FoundCamera& f : d->found) {  // one row per camera: name | address
        GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(r, 4);
        gtk_widget_set_margin_bottom(r, 4);
        gtk_widget_set_margin_start(r, 8);
        GtkWidget* n = gtk_label_new((f.name.empty() ? std::string(_("(no name)")) : f.name).c_str());
        GtkWidget* a = gtk_label_new(f.host.c_str());
        for (GtkWidget* l : {n, a}) gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
        gtk_widget_set_size_request(n, kNameColumn, -1);
        gtk_box_append(GTK_BOX(r), n);
        gtk_box_append(GTK_BOX(r), gtk_separator_new(GTK_ORIENTATION_VERTICAL));  // column separator
        gtk_box_append(GTK_BOX(r), a);
        gtk_list_box_append(GTK_LIST_BOX(d->found_list), r);
    }
    return G_SOURCE_REMOVE;
}

void search_clicked(GtkButton*, gpointer p) {
    Dlg* d = (Dlg*)p;
    gtk_widget_set_sensitive(d->search_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(d->search_status), _("Searching…"));
    auto alive = d->alive;
    auto token = d->cancel;
    std::thread([alive, token, d]() {
        auto* r = new SearchResult{alive, d, {}, ""};
        try {
            r->found = discover(DiscoverOptions(), token.get());
        } catch (const xm::Error& e) {
            if (e.code() != xm::Errc::Cancelled) r->error = e.what();
        }
        g_idle_add(search_done, r);
    }).detach();
}

void found_activated(GtkListBox*, GtkListBoxRow* row, gpointer p) {
    Dlg* d = (Dlg*)p;
    int i = gtk_list_box_row_get_index(row);
    if (i < 0 || i >= (int)d->found.size()) return;
    const FoundCamera& f = d->found[i];
    gtk_editable_set_text(GTK_EDITABLE(d->host), f.host.c_str());
    gtk_editable_set_text(GTK_EDITABLE(d->dvrip_port), std::to_string(f.tcp_port).c_str());
    if (std::string(gtk_editable_get_text(GTK_EDITABLE(d->name))).empty())
        gtk_editable_set_text(GTK_EDITABLE(d->name), (f.name.empty() ? f.host : f.name).c_str());
}

}  // namespace

void show_camera_dialog(GtkWindow* parent, const CameraConfig* existing, bool keyring_compiled_in,
                        std::function<void(CameraDialogResult)> on_save, std::function<void(std::string)> on_delete) {
    Dlg* d = new Dlg();
    d->editing = existing != nullptr;
    if (existing) d->original = *existing;
    else d->original.rtsp_template = kDefaultRtspTemplate;
    d->on_save = std::move(on_save);
    d->on_delete = std::move(on_delete);
    const CameraConfig& c = d->original;

    d->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(d->win), d->editing ? _("Camera settings") : _("Add camera"));
    gtk_window_set_modal(GTK_WINDOW(d->win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(d->win), parent);
    gtk_window_set_default_size(GTK_WINDOW(d->win), 520, -1);
    g_signal_connect(d->win, "destroy", G_CALLBACK(destroyed), d);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    // Two tabs: "Main" (what identifies the camera) and "Additional" (live view over RTSP); the buttons stay below them.
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // plain GtkStackSwitcher, no custom CSS: the theme draws the tabs in its own (system) style
    GtkWidget* tabs = gtk_stack_new();
    gtk_widget_set_vexpand(tabs, TRUE);
    gtk_stack_add_titled(GTK_STACK(tabs), box, "main", _("Main"));
    GtkWidget* switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(tabs));
    gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(switcher, 12);
    gtk_widget_set_margin_bottom(switcher, 6);
    gtk_box_append(GTK_BOX(outer), switcher);
    gtk_box_append(GTK_BOX(outer), tabs);
    gtk_widget_set_size_request(d->win, 460, -1);  // not narrower than this
    gtk_window_set_child(GTK_WINDOW(d->win), outer);

    if (!d->editing) {  // searching the local network (see discovery.h: not yet tried on a real network)
        GtkWidget* srow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        d->search_btn = gtk_button_new();
        {
            GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_append(GTK_BOX(inner), gtk_image_new_from_icon_name("camera-video-symbolic"));
            gtk_box_append(GTK_BOX(inner), gtk_label_new(_("Search")));
            gtk_button_set_child(GTK_BUTTON(d->search_btn), inner);
            gtk_widget_set_tooltip_text(d->search_btn, _("Search for cameras on the local network"));
            gtk_widget_set_valign(d->search_btn, GTK_ALIGN_CENTER);
        }
        g_signal_connect(d->search_btn, "clicked", G_CALLBACK(search_clicked), d);
        d->search_status = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(d->search_status), 0);
        gtk_label_set_wrap(GTK_LABEL(d->search_status), TRUE);
        gtk_widget_set_hexpand(d->search_status, TRUE);
        gtk_widget_add_css_class(d->search_status, "dim-label");
        gtk_box_append(GTK_BOX(srow), d->search_btn);
        gtk_box_append(GTK_BOX(srow), d->search_status);
        gtk_box_append(GTK_BOX(box), srow);
        d->found_list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(d->found_list), GTK_SELECTION_NONE);
        g_signal_connect(d->found_list, "row-activated", G_CALLBACK(found_activated), d);
        GtkWidget* fs = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(fs), d->found_list);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(fs), 120);
        gtk_list_box_set_show_separators(GTK_LIST_BOX(d->found_list), TRUE);  // lines between the rows
        // one bordered table: caption row (inside it), a line, then the rows; the columns have separators
        GtkWidget* table = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(table, "frame");
        GtkWidget* hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(hdr, 8);
        gtk_widget_set_margin_top(hdr, 4);
        gtk_widget_set_margin_bottom(hdr, 4);
        GtkWidget* hn = gtk_label_new(_("Name"));
        GtkWidget* ha = gtk_label_new(_("Address"));
        for (GtkWidget* l : {hn, ha}) { gtk_label_set_xalign(GTK_LABEL(l), 0); }
        gtk_widget_set_size_request(hn, kNameColumn, -1);
        gtk_box_append(GTK_BOX(hdr), hn);
        gtk_box_append(GTK_BOX(hdr), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        gtk_box_append(GTK_BOX(hdr), ha);
        gtk_box_append(GTK_BOX(table), hdr);
        gtk_box_append(GTK_BOX(table), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        gtk_box_append(GTK_BOX(table), fs);
        gtk_box_append(GTK_BOX(box), table);
    }

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(box), grid);

    d->name = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->name), c.name.c_str());
    d->host = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->host), c.host.c_str());
    d->dvrip_port = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->dvrip_port), std::to_string(c.dvrip_port).c_str());
    d->user = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->user), c.user.c_str());
    d->pass = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(d->pass), TRUE);
    row(grid, 0, _("Name"), d->name);
    row(grid, 1, _("Camera address"), d->host);
    row(grid, 2, _("Port (archive)"), d->dvrip_port);
    row(grid, 3, _("User"), d->user);
    row(grid, 4, _("Password"), d->pass);

    d->no_pass = gtk_check_button_new_with_label(_("The camera has no password"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->no_pass), c.no_password);
    gtk_box_append(GTK_BOX(box), d->no_pass);
    d->keyring = gtk_check_button_new_with_label(
        keyring_compiled_in ? _("Save the password in the system keyring")
                            : _("Save the password in the keyring (not available in this build: kept until the program closes)"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->keyring), keyring_compiled_in);
    gtk_widget_set_sensitive(d->keyring, keyring_compiled_in);
    gtk_box_append(GTK_BOX(box), d->keyring);
    if (d->editing) {
        GtkWidget* hint = gtk_label_new(_("Leave the password empty to keep the saved one."));
        gtk_widget_set_halign(hint, GTK_ALIGN_START);
        gtk_widget_add_css_class(hint, "dim-label");
        gtk_box_append(GTK_BOX(box), hint);
    }

    if (d->editing) {  // only the settings of an existing camera
        // PTZ: some cameras turn the other way round than the pad says
        GtkWidget* prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget* plab = gtk_label_new(_("Reverse PTZ left / right"));
        gtk_widget_set_hexpand(plab, TRUE);
        gtk_label_set_xalign(GTK_LABEL(plab), 0);
        d->ptz_flip = gtk_switch_new();
        gtk_widget_set_valign(d->ptz_flip, GTK_ALIGN_CENTER);
        gtk_switch_set_active(GTK_SWITCH(d->ptz_flip), c.ptz_flip_h);
        gtk_box_append(GTK_BOX(prow), plab);
        gtk_box_append(GTK_BOX(prow), d->ptz_flip);
        gtk_widget_set_margin_top(prow, 12);
        gtk_box_append(GTK_BOX(box), prow);
        GtkWidget* vrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget* vlab = gtk_label_new(_("Reverse PTZ up / down"));
        gtk_widget_set_hexpand(vlab, TRUE);
        gtk_label_set_xalign(GTK_LABEL(vlab), 0);
        d->ptz_flip_v = gtk_switch_new();
        gtk_widget_set_valign(d->ptz_flip_v, GTK_ALIGN_CENTER);
        gtk_switch_set_active(GTK_SWITCH(d->ptz_flip_v), c.ptz_flip_v);
        gtk_box_append(GTK_BOX(vrow), vlab);
        gtk_box_append(GTK_BOX(vrow), d->ptz_flip_v);
        gtk_box_append(GTK_BOX(box), vrow);

    }

    GtkWidget* g2 = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g2), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g2), 12);
    d->rtsp_port = spin(1, 65535, c.rtsp_port);
    d->channel = spin(0, 64, c.channel);
    d->main_stream = spin(0, 16, c.main_stream);
    d->sub_stream = spin(0, 16, c.sub_stream);
    // The mouse wheel changes a number field only after the field has been clicked.
    for (GtkWidget* sp : {d->rtsp_port, d->channel, d->main_stream, d->sub_stream}) {
        GtkEventController* wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
        gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
        g_signal_connect(wheel, "scroll", G_CALLBACK(+[](GtkEventControllerScroll* ec, double, double, gpointer) -> gboolean {
            GtkWidget* w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ec));
            return (gtk_widget_get_state_flags(w) & GTK_STATE_FLAG_FOCUS_WITHIN) ? FALSE : TRUE;
        }), nullptr);
        gtk_widget_add_controller(sp, wheel);
    }
    d->tmpl = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->tmpl), c.rtsp_template.c_str());
    d->sub_grid = gtk_check_button_new_with_label(_("Use the sub stream when several cameras are shown"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->sub_grid), c.use_sub_in_grid);
    row(g2, 0, _("RTSP port"), d->rtsp_port);
    row(g2, 1, _("Channel"), d->channel);
    row(g2, 2, _("Main stream number"), d->main_stream);
    row(g2, 3, _("Sub stream number"), d->sub_stream);
    row(g2, 4, _("RTSP address"), d->tmpl);
    gtk_grid_attach(GTK_GRID(g2), d->sub_grid, 0, 5, 2, 1);
    GtkWidget* note = gtk_label_new(
        _("Placeholders: {host} {port} {user} {password} {channel} {stream}. The default address is the form used by the "
          "working mpv script; the stream number is not part of it, so the sub stream option has no effect unless "
          "{stream} is put into the address."));
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_widget_add_css_class(note, "dim-label");
    gtk_grid_attach(GTK_GRID(g2), note, 0, 6, 2, 1);
    GtkWidget* extra = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(extra, 14);
    gtk_widget_set_margin_bottom(extra, 14);
    gtk_widget_set_margin_start(extra, 14);
    gtk_widget_set_margin_end(extra, 14);
    GtkWidget* extra_title = gtk_label_new(_("Live view (RTSP)"));
    gtk_label_set_xalign(GTK_LABEL(extra_title), 0);
    gtk_widget_add_css_class(extra_title, "dim-label");
    gtk_box_append(GTK_BOX(extra), extra_title);
    gtk_box_append(GTK_BOX(extra), g2);
    gtk_stack_add_titled(GTK_STACK(tabs), extra, "extra", _("Additional"));
    style_tab_switcher(switcher, tabs);

    d->error = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(d->error), TRUE);
    gtk_label_set_xalign(GTK_LABEL(d->error), 0);
    gtk_widget_add_css_class(d->error, "error");
    gtk_widget_set_margin_start(d->error, 14);
    gtk_widget_set_margin_end(d->error, 14);
    gtk_box_append(GTK_BOX(outer), d->error);

    GtkWidget* btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    if (d->editing) {
        d->del_btn = gtk_button_new_with_label(_("Delete camera"));
        g_signal_connect(d->del_btn, "clicked", G_CALLBACK(delete_clicked), d);
        gtk_box_append(GTK_BOX(btns), d->del_btn);
    }
    GtkWidget* spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(btns), spacer);
    GtkWidget* cancel = gtk_button_new_with_label(_("Cancel"));
    GtkWidget* save = gtk_button_new_with_label(_("Save"));
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_clicked), d);
    g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), d);
    gtk_box_append(GTK_BOX(btns), cancel);
    gtk_box_append(GTK_BOX(btns), save);
    gtk_widget_set_margin_start(btns, 14);
    gtk_widget_set_margin_end(btns, 14);
    gtk_widget_set_margin_bottom(btns, 14);
    d->buttons = btns;
    gtk_box_append(GTK_BOX(outer), btns);

    gtk_window_present(GTK_WINDOW(d->win));
    deselect_first_field(d->win);
}

struct Conf {
    GtkWidget* win = nullptr;
    std::function<void()> yes;
};

static void conf_yes(GtkButton*, gpointer p) {
    Conf* c = (Conf*)p;
    auto fn = c->yes;
    gtk_window_destroy(GTK_WINDOW(c->win));
    if (fn) fn();
}
static void conf_no(GtkButton*, gpointer p) { gtk_window_destroy(GTK_WINDOW(((Conf*)p)->win)); }
static void conf_destroyed(GtkWidget*, gpointer p) { delete (Conf*)p; }

void show_confirm(GtkWindow* parent, const std::string& title, const std::string& text, const std::string& yes_label,
                  std::function<void()> on_yes) {
    Conf* c = new Conf();
    c->yes = std::move(on_yes);
    c->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(c->win), title.c_str());
    gtk_window_set_modal(GTK_WINDOW(c->win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(c->win), parent);
    g_signal_connect(c->win, "destroy", G_CALLBACK(conf_destroyed), c);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 48);
    gtk_box_append(GTK_BOX(box), l);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget* no = gtk_button_new_with_label(_("Cancel"));
    GtkWidget* yes = gtk_button_new_with_label(yes_label.c_str());
    g_signal_connect(no, "clicked", G_CALLBACK(conf_no), c);
    g_signal_connect(yes, "clicked", G_CALLBACK(conf_yes), c);
    gtk_box_append(GTK_BOX(row), no);
    gtk_box_append(GTK_BOX(row), yes);
    gtk_box_append(GTK_BOX(box), row);
    gtk_window_set_child(GTK_WINDOW(c->win), box);
    gtk_window_present(GTK_WINDOW(c->win));
}

}  // namespace icmon
