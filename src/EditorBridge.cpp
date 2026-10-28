#include "EditorBridge.h"
#include "scintilla_min.h"

#include <dlfcn.h>
#include <glib.h>

#include <cstring>

/* Resolved when the editor dlopens this plugin. Do not link libscintilla. */
extern "C" intptr_t scintilla_view_send_message(void *view, unsigned int msg,
                                                uintptr_t wParam, intptr_t lParam);

namespace {

NppData g_host{};
int g_cmd_id = 0;
long g_panel = 0;

/* Editor-owned indicator numbers on nextpad-plus-plus-linux main
 * (commit 8dec39c). NPPM_ALLOCATEINDICATOR starts at 9 and does not skip
 * these, so a slot it hands back is discarded when it collides. */
bool indicator_reserved(int slot)
{
    if (slot == 9 || slot == 19) return true;
    if (slot >= 21 && slot <= 25) return true;
    return false;
}

void collect_into(GtkWidget *w, std::vector<void *> &out)
{
    if (!w) return;
    if (sci_is_view(w)) out.push_back(w);
    for (GtkWidget *child = gtk_widget_get_first_child(w); child;
         child = gtk_widget_get_next_sibling(child))
        collect_into(child, out);
}

std::string host_path(unsigned int msg)
{
    char buf[2048];
    buf[0] = '\0';
    if (!g_host.hostMsg) return {};
    if (!g_host.hostMsg(msg, 0, reinterpret_cast<long>(buf))) return {};
    return buf;
}

} /* namespace */

void editor_bridge_set(const NppData &data)
{
    g_host = data;
}

void *editor_main_window()
{
    return g_host.nppHandle;
}

int editor_menu_cmd_id()
{
    return g_cmd_id;
}

void editor_set_menu_cmd_id(int cmd_id)
{
    g_cmd_id = cmd_id;
}

long editor_host(unsigned int msg, unsigned long wParam, long lParam)
{
    if (!g_host.hostMsg) return 0;
    return g_host.hostMsg(msg, wParam, lParam);
}

intptr_t sci_send(void *sci, unsigned int msg, uintptr_t wParam, intptr_t lParam)
{
    if (!sci) return 0;
    return scintilla_view_send_message(sci, msg, wParam, lParam);
}

void *editor_current_sci()
{
    if (!g_host.hostMsg) return nullptr;
    return reinterpret_cast<void *>(editor_host(NPPM_GETCURRENTSCINTILLA, 0, 0));
}

bool sci_is_view(void *widget)
{
    if (!widget || !G_IS_OBJECT(widget)) return false;
    return g_strcmp0(G_OBJECT_TYPE_NAME(widget), "ScintillaView") == 0;
}

void editor_collect_views(std::vector<void *> &out)
{
    out.clear();
    if (g_host.nppHandle)
        collect_into(g_host.nppHandle, out);
    void *current = editor_current_sci();
    if (current) {
        bool have = false;
        for (void *view : out)
            if (view == current) have = true;
        if (!have) out.push_back(current);
    }
}

uintptr_t sci_doc_pointer(void *sci)
{
    if (!sci) return 0;
    return static_cast<uintptr_t>(sci_send(sci, SCI_GETDOCPOINTER, 0, 0));
}

void *sci_buffer_id(void *sci)
{
    if (!sci || !G_IS_OBJECT(sci)) return nullptr;
    return g_object_get_data(G_OBJECT(sci), "npp-doc");
}

int editor_bookmark_marker()
{
    long id = editor_host(NPPM_GETBOOKMARKID, 0, 0);
    if (id <= 0 || id > 31) return 20;
    return static_cast<int>(id);
}

int editor_allocate_indicator()
{
    /* Ask for one slot at a time. Reserved numbers are not painted, which
     * leaves them for the editor feature that already owns them. */
    for (int attempt = 0; attempt < 24; ++attempt) {
        int slot = 0;
        long ok = editor_host(NPPM_ALLOCATEINDICATOR, 1, reinterpret_cast<long>(&slot));
        if (!ok || slot <= 0) return -1;
        if (!indicator_reserved(slot)) return slot;
    }
    return -1;
}

std::string editor_plugins_config_file()
{
    std::string dir = host_path(NPPM_GETPLUGINSCONFIGDIR);
    if (dir.empty()) {
        const char *home = g_get_user_data_dir();
        dir = std::string(home ? home : ".") + "/nextpad++/plugins/Config";
        g_mkdir_with_parents(dir.c_str(), 0755);
    }
    return dir + "/semantic-search.ini";
}

std::string editor_model_dir(const std::string &model_id)
{
    std::string plugins = host_path(NPPM_GETPLUGINHOMEPATH);
    std::string base;
    if (!plugins.empty()) {
        gchar *parent = g_path_get_dirname(plugins.c_str());
        base = parent ? parent : plugins;
        g_free(parent);
    } else {
        const char *home = g_get_user_data_dir();
        base = std::string(home ? home : ".") + "/nextpad++";
    }
    return base + "/semantic-models/" + model_id;
}

std::string editor_manifest_path()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&editor_manifest_path), &info) &&
        info.dli_fname && info.dli_fname[0]) {
        gchar *dir = g_path_get_dirname(info.dli_fname);
        gchar *path = g_build_filename(dir, "resources", "semantic", "models.json", nullptr);
        std::string out = path ? path : "";
        g_free(path);
        g_free(dir);
        if (!out.empty()) return out;
    }
    return "resources/semantic/models.json";
}

long editor_register_panel(void *widget)
{
    if (g_panel > 0 || !widget) return g_panel;
    /* Linux order: wParam is the title, lParam is the GtkWidget. */
    g_panel = editor_host(NPPM_DMM_REGISTERPANEL,
                          reinterpret_cast<unsigned long>("Semantic Search"),
                          reinterpret_cast<long>(widget));
    if (g_panel > 0) {
        NppPanelInfo info;
        info.moduleName = "Semantic Search";
        info.cmdIndex = 0;
        editor_host(NPPM_DMM_SETPANELINFO, static_cast<unsigned long>(g_panel),
                    reinterpret_cast<long>(&info));
    }
    return g_panel;
}

void editor_show_panel(long handle)
{
    if (handle > 0) editor_host(NPPM_DMM_SHOWPANEL, static_cast<unsigned long>(handle), 0);
}

void editor_hide_panel(long handle)
{
    if (handle > 0) editor_host(NPPM_DMM_HIDEPANEL, static_cast<unsigned long>(handle), 0);
}

void editor_set_menu_check(bool on)
{
    if (g_cmd_id == 0) return;
    editor_host(NPPM_SETMENUITEMCHECK, static_cast<unsigned long>(g_cmd_id), on ? 1 : 0);
}
