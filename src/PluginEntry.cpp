/*
 * Semantic Search plugin entry points for Nextpad++ on Linux.
 *
 * The host loads plugins/<Name>/<Name>.so and calls setInfo, getName,
 * getFuncsArray, beNotified, messageProc, and isUnicode.
 */
#include "EditorBridge.h"
#include "scintilla_min.h"

#include <cstring>

void semantic_search_toggle(void);
void semantic_search_shutdown(void);
void semantic_search_note_modified(GtkWidget *sci);
void semantic_search_note_activated(GtkWidget *sci);
void semantic_search_note_closed(void *buffer_id);
void semantic_search_restyle(void);
void semantic_search_smoke_start(void);

namespace {

constexpr int kItems = 2;
FuncItem g_items[kItems];
bool g_items_ready = false;

void fill_items()
{
    if (g_items_ready) return;
    std::memset(g_items, 0, sizeof g_items);
    std::snprintf(g_items[0].itemName, sizeof g_items[0].itemName, "Semantic Heatmap Search");
    g_items[0].pFunc = [] {
        editor_set_menu_cmd_id(g_items[0].cmdID);
        semantic_search_toggle();
    };
    std::snprintf(g_items[1].itemName, sizeof g_items[1].itemName, "About");
    g_items[1].pFunc = [] {
        GtkWidget *parent = static_cast<GtkWidget *>(editor_main_window());
        GtkAlertDialog *dialog = gtk_alert_dialog_new("Semantic Search");
        gtk_alert_dialog_set_detail(
            dialog,
            "Describe what you are looking for. Each sentence is tinted from "
            "red through grey to green by similarity to that description.\n\n"
            "Embeddings run on this machine with ONNX Runtime. The default "
            "model is all-MiniLM-L6-v2 fp32. CUDA and OpenVINO are used when "
            "they load and produce a finite result, and otherwise the search "
            "falls back to CPU.\n\n"
            "Based on Nextpad++ Linux pull request 28. License: GPL-3.0.");
        gtk_alert_dialog_show(dialog, parent ? GTK_WINDOW(parent) : nullptr);
        g_object_unref(dialog);
    };
    g_items_ready = true;
}

} /* namespace */

extern "C" __attribute__((visibility("default"))) void setInfo(NppData data)
{
    editor_bridge_set(data);
    fill_items();
}

extern "C" __attribute__((visibility("default"))) const char *getName(void)
{
    return "Semantic Search";
}

extern "C" __attribute__((visibility("default"))) FuncItem *getFuncsArray(int *nbF)
{
    fill_items();
    if (nbF) *nbF = kItems;
    return g_items;
}

extern "C" __attribute__((visibility("default"))) void beNotified(SCNotification *n)
{
    if (!n) return;
    switch (n->nmhdr.code) {
    case NPPN_READY:
        editor_host(NPPM_SETPLUGINSUBSCRIPTIONS, NPPPLUGIN_WANTS_UPDATEUI,
                    reinterpret_cast<long>("Semantic Search"));
        semantic_search_smoke_start();
        break;
    case NPPN_BUFFERACTIVATED:
        semantic_search_note_activated(static_cast<GtkWidget *>(editor_current_sci()));
        break;
    case NPPN_FILEBEFORECLOSE:
        semantic_search_note_closed(reinterpret_cast<void *>(n->nmhdr.idFrom));
        break;
    case NPPN_BEFORESHUTDOWN:
    case NPPN_SHUTDOWN:
        semantic_search_shutdown();
        break;
    case SCN_MODIFIED:
        if (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
            void *from = n->nmhdr.hwndFrom;
            GtkWidget *sci = sci_is_view(from)
                                 ? static_cast<GtkWidget *>(from)
                                 : static_cast<GtkWidget *>(editor_current_sci());
            semantic_search_note_modified(sci);
        }
        break;
    case SCN_UPDATEUI:
        semantic_search_restyle();
        break;
    default:
        break;
    }
}

extern "C" __attribute__((visibility("default"))) long messageProc(unsigned int, unsigned long, long)
{
    return 1;
}

extern "C" __attribute__((visibility("default"))) int isUnicode(void)
{
    return 1;
}
