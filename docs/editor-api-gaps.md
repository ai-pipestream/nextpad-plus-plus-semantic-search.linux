# Editor API gaps

This note is for the Nextpad++ Linux plugin API. It records what the in-editor semantic search (pull request 28) did by calling editor functions directly, what this plugin does instead, and the host change that would make the workaround unnecessary.

Checked against nextpad-plus-plus-linux commit `8dec39c9f0f53aefc662520717fb7c0b6107fcdb`: `src/plugin.h`, `src/plugin.c`, `src/editor.c`, `src/main.c`, `src/docmap.c`, `src/findreplace.c`, `src/spell.h`, `src/sci_c.h`, `src/session.c`, and `scintilla/gtk4/ScintillaGTK.cxx` from bugaevc/scintilla `gtk4` at `3ba9b8f09851b60180ce57ee908c6eb57c751368`. The plugin compiles against the copy of `plugin.h` in `third_party/npp`.

## Search bar and dock edge

Pull request 28 packed a revealer directly above the status bar and registered an application action for it. There is no host message that inserts a widget into that slot. `NPPM_DMMREGASDCKDLG` ignores dock masks and forwards to `NPPM_DMM_REGISTERPANEL`. That path always docks on the right through `main_plugin_panel_attach`.

Linux argument order is fixed: `wParam` is `const char *` title, `lParam` is `GtkWidget *`. A Windows `tTbData` is rejected. `plugin.c` (the GAP-88j comment) explains why the host does not sniff the other order. `GTK_IS_WIDGET` on the title pointer treats the string bytes as a `GType` and crashes.

This plugin builds a GTK box (query, model, provider, sensitivity, six legend toggles, status) and registers it with `NPPM_DMM_REGISTERPANEL`, then `NPPM_DMM_SETPANELINFO` with `moduleName` equal to `getName()` (`"Semantic Search"`) and `cmdIndex` 0. A saved session can reopen the panel by running that menu command. The rows are stacked. A right-hand dock is too narrow for the query, the model list, and the provider list on one line.

Closing the frame hides the wrapper and does not notify the plugin. `PluginPanel.visible` stays set, so the plugin cannot trust it. The plugin watches `unmap` on its own widget and treats that as a close only when the toplevel window is still mapped. Minimizing the editor therefore does not clear the heatmap. A notification such as `NPPN_PANELCLOSED`, with the panel handle in `idFrom`, would replace the signal watch. A dock-edge argument on `NPPM_DMM_REGISTERPANEL`, honoured by the host, would allow the bar to sit above the status line the way pull request 28 did.

## Indicator numbers

`NPPM_ALLOCATEINDICATOR` starts at `INDIC_CONTAINER + 1` (9) and hands out consecutive numbers until a request would pass 31. It does not skip numbers the editor already paints:

| Number | Owner | Where |
| --- | --- | --- |
| 8 | Spell squiggle (`SPELL_INDICATOR`) | `src/spell.h`. Below the allocator start. |
| 9 | Incremental search (`INCR_INDICATOR`) | `src/editor.c` |
| 19 | Clickable links (`NPP_LINK_INDICATOR`) | `src/editor.c` |
| 21 to 25 | Mark colours (`NPP_MARK_INDIC_BASE`) | `src/main.c` |
| 31 | Find marks (`NPP_FIND_MARK_INDICATOR`) | `src/findreplace.c` |

The plugin asks for one slot at a time and discards 9, 19, and 21 to 25 without painting them. The first plugin in a session therefore receives indicator 10. It does not discard 31, because one allocation never reaches 31. A plugin that runs after others have consumed the range can still be handed 31, and it would paint over find marks.

`src/indicator_alloc.c` from pull request 28 starts at 9, skips 9 and 19, and stops at 20. The unit test still covers that helper. It is not the runtime allocator. A counter inside one plugin cannot see slots another plugin already took. The skip list belongs in `NPPM_ALLOCATEINDICATOR`, including 31.

## Which Scintilla widget is current

`setInfo` receives a copy of `NppData`. `scintillaMainHandle` and `scintillaSecondHandle` are `NULL` at load time, because `plugin_init` runs before the editors exist. `plugin_refresh_handles` writes the live widgets into the host's copy only. It does not call `setInfo` again. A plugin that keeps the pointers from `setInfo` is stuck with `NULL`.

`NPPM_GETCURRENTSCINTILLA` returns `editor_current_doc()->sci`. When `lParam` is an `int *`, the host writes view id 0. The comment in `plugin.c` says the host only has view 0, but split notebooks do exist. `NPPM_GETCURRENTVIEW` also returns 0. There is no message that lists the primary editor, the split clones, and the document map.

`SCI_*` is not sent through `hostMsg`. This plugin declares `scintilla_view_send_message` and links with `-Wl,--allow-shlib-undefined`. The editor's `libscintilla.so` exports that symbol (bugaevc/scintilla gtk4, GType name `ScintillaView`), and `dlopen` resolves it. Please keep the symbol exported.

Indicator ranges follow the document. `INDIC_TEXTFORE` with `SC_INDICFLAG_VALUEFORE` is stored per view, so every view that shows the document needs the style. Pull request 28 set it from `split.c`, `docmap.c`, and clone creation. This plugin walks every child of `nppHandle` whose GType name is `ScintillaView`, adds the widget from `NPPM_GETCURRENTSCINTILLA` if the walk missed it, and applies the style. `SCN_UPDATEUI` rearms a 400 ms timer so a document map opened later is included. A view that is not parented under `nppHandle` is missed.

The document map is a `ScintillaView` that shares the document pointer and has no `npp-doc` GObject data. `editor.c` sets that key only on real tabs. `on_sci_notify` returns before `plugin_notify_all` when the key is missing, so the map does not forward `SCN_*`. Close handling ignores views without `npp-doc`, otherwise a close would retarget bookmarks onto the minimap. The key is a private host detail. The same key is what CommentToggler.linux reads.

A host message that fills a caller-provided array of Scintilla widgets (editor, splits, document map), and a message that returns the `NppDoc *` for a widget, would remove the walk and the private key.

## Document lifetime and notifications

`NPPN_BUFFERACTIVATED` is delivered on idle after a tab switch. `idFrom` is the `NppDoc *`. The plugin rebinds through `NPPM_GETCURRENTSCINTILLA` rather than the id, because the focused view is not identified any other way.

`NPPN_FILEBEFORECLOSE` runs from `close_sci_full` while the Scintilla widget is still in the notebook. `idFrom` is the `NppDoc *`. The plugin matches it to `g_object_get_data(sci, "npp-doc")`. `NPPN_FILECLOSED` runs after the struct is freed. The comment in `editor.c` says the pointer is an identity key for that one call and must not be dereferenced. This plugin does not use `NPPN_FILECLOSED`.

`NPPN_BEFORESHUTDOWN` and `NPPN_SHUTDOWN` fire back to back, only after quit is committed, and before the notebooks are torn down (`editor.c`). Cancelled quit fires neither. The plugin sets its idle gate, cancels the model download, joins the worker, and releases the ONNX session on the worker thread. That order is what the engine needs. Please keep both notifications, in that order, while the widgets are still alive.

`SCN_MODIFIED` is forwarded only when `modificationType` intersects insert, delete, undo, redo, fold change, or indicator change, plus any bits added with `NPPM_ADDSCNMODIFIEDFLAGS`. A reentrancy guard drops a nested notification. Nothing with an `SCN_*` code is forwarded after `NPPN_SHUTDOWN`. Plugins receive a copy of `SCNotification`.

`hwndFrom` on those copies is the Scintilla widget. `ScintillaGTK.cxx` sets `scn.nmhdr.hwndFrom = widget()`. `plugin.h` does not say so. The plugin uses `hwndFrom` when its GType is `ScintillaView`, and otherwise the current editor. Stating that `hwndFrom` is the widget pointer would make the behaviour a contract.

On `NPPN_READY` the plugin calls `NPPM_SETPLUGINSUBSCRIPTIONS` with `NPPPLUGIN_WANTS_UPDATEUI` and its `getName()`, so `SCN_PAINTED` is not delivered. `SCN_UPDATEUI` only rearms the restyle timer.

## Bookmark marker

`NPPM_GETBOOKMARKID` returns `SC_MARKNUM_BOOKMARK`. `src/sci_c.h` defines that as 20, because 24 is the change-history marker. `main.c` toggles the same macro. The comment on `NPP_BOOKMARK_MARKER` that says the value is 24 is stale. `session.c` contains a local fallback of 24, and that fallback is inactive because the file includes `sci_c.h` first.

The plugin uses the host message. If the message returns a value outside 1 to 31 it uses 20. Legend selection adds and removes only that marker, so other markers on the line stay. Please keep `NPPM_GETBOOKMARKID` equal to the marker the View menu toggles.

## Preferences, models, and the manifest

`NPPM_GETPLUGINSCONFIGDIR` is `<local data>/plugins/Config`, created on the query. The plugin stores `semantic-search.ini` there.

`NPPM_GETPLUGINHOMEPATH` is the plugins directory, not the calling plugin's own folder. The parent of that directory is `npp_local_dir()`: `$XDG_DATA_HOME/nextpad++` unless `-settingsDir` overrides it. That directory is not the cloud settings folder. Models are stored at `<parent>/semantic-models/<model id>/`, which is the layout pull request 28 used.

`NPPM_GETNPPDIRECTORY` is `/usr/share/nextpad-plus-plus`. The model manifest is not installed there. The plugin finds it with `dladdr` on its own `.so`: `<directory of SemanticSearch.so>/resources/semantic/models.json`.

`NPPM_GETNPPSETTINGSDIRPATH` follows `npp_user_dir()`, which can be the cloud folder. The plugin does not use it for preferences or models.

## Heatmap drawing

This limit is in Scintilla, not in the plugin messages. `SurfaceGTK::AlphaRectangle` in the gtk4 backend logs that it is unimplemented, so `INDIC_ROUNDBOX` and `INDIC_FULLBOX` do not paint. The heatmap uses `INDIC_TEXTFORE` with `SC_INDICFLAG_VALUEFORE`, which does not need that call. A working `AlphaRectangle` would allow a box style.

## What already works

These host messages are used as documented and need no change: `NPPM_DMM_SHOWPANEL`, `NPPM_DMM_HIDEPANEL`, `NPPM_SETMENUITEMCHECK` (the checkmark on "Semantic Heatmap Search"), `NPPM_GETBOOKMARKID`, and the load path `<plugins>/<Name>/<Name>.so` with `setInfo` before `getFuncsArray`. The folder and file name are `SemanticSearch`. `getName()` is `Semantic Search`.
