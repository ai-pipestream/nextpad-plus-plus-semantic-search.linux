/*
 * EditorBridge
 *
 * The only place the plugin talks to Nextpad++ and to Scintilla.
 * Host messages go through NppData.hostMsg. Scintilla messages go through
 * scintilla_view_send_message, which the editor's libscintilla.so provides
 * when the plugin is loaded. The symbol is left undefined at link time.
 */
#ifndef SEMANTIC_EDITOR_BRIDGE_H
#define SEMANTIC_EDITOR_BRIDGE_H

#include "plugin.h"

#include <string>
#include <vector>

void editor_bridge_set(const NppData &data);
void *editor_main_window();
int editor_menu_cmd_id();
void editor_set_menu_cmd_id(int cmd_id);

long editor_host(unsigned int msg, unsigned long wParam, long lParam);
intptr_t sci_send(void *sci, unsigned int msg, uintptr_t wParam, intptr_t lParam);

void *editor_current_sci();
void editor_collect_views(std::vector<void *> &out);
bool sci_is_view(void *widget);
uintptr_t sci_doc_pointer(void *sci);
void *sci_buffer_id(void *sci);

int editor_bookmark_marker();
int editor_allocate_indicator();

std::string editor_plugins_config_file();
std::string editor_model_dir(const std::string &model_id);
std::string editor_manifest_path();

long editor_register_panel(void *widget);
void editor_show_panel(long handle);
void editor_hide_panel(long handle);
void editor_set_menu_check(bool on);

#endif
