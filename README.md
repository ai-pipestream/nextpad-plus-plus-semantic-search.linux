# Semantic search for Nextpad++ on Linux

Semantic search tints each sentence by how close it is to a description you type. Red is a weak match, grey is in the middle, and green is a close match. Click a colour in the legend to bookmark those lines and hide the other bands. Several bands can be selected. The bookmarks are the same marker the View menu uses, so the usual bookmark keys still walk them, and bookmarks you set yourself stay put.

Embeddings run on this machine with ONNX Runtime. The default model is all-MiniLM-L6-v2 in fp32 (384 dimensions, mean pooling, 256 tokens). Two smaller int8 MiniLM builds are listed as well. CUDA and OpenVINO are used when they load and return a finite result. If a GPU provider fails, the search falls back to CPU and builds the index again. The plugin does not link ONNX Runtime. It loads `libonnxruntime.so` when a search runs.

The search UI is a panel docked on the right. Nextpad++ Linux has no plugin message that inserts a bar above the status line, which is where the same feature sat when it was built into the editor. The other limits of the plugin API are listed in [docs/editor-api-gaps.md](docs/editor-api-gaps.md).

The engine was proposed for the editor in [pull request 28](https://github.com/nextpad-plus-plus/nextpad-plus-plus-linux/pull/28), which tracks [issue 21](https://github.com/nextpad-plus-plus/nextpad-plus-plus-linux/issues/21).

## Building

You need CMake 3.20 or newer, a C++17 compiler, GTK 4, GLib, and Pango. The editor source tree is not required.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The shared library is `build/SemanticSearch.so`. Headers come from `third_party/npp/plugin.h`, a copy of the Linux plugin contract at nextpad-plus-plus-linux commit `8dec39c9f0f53aefc662520717fb7c0b6107fcdb`. Refresh that file with `tools/fetch-npp-headers.sh` (pass a checkout, or let it download the pinned commit). `third_party/npp/scintilla_min.h` is the small set of Scintilla constants this plugin sends. Scintilla itself is not linked. `scintilla_view_send_message` is left undefined and is resolved from the editor's `libscintilla.so` when the plugin is loaded.

`-DONNXRUNTIME_ROOT=/path/to/onnxruntime` bakes a search path into the loader. Leave it empty to use the `ONNXRUNTIME_ROOT` environment variable at run time. The tests do not need a model. Pass `-DNPP_SEMANTIC_TEST_MODEL_DIR` pointing at a directory that already contains the MiniLM files if you want the numeric embedding checks.

## Installing

The editor loads `<plugins>/<Name>/<Name>.so`. The folder name and the file name must match each other. The name returned by `getName()` may contain spaces. This plugin installs as `SemanticSearch/SemanticSearch.so` and shows up as Semantic Search.

```
cmake --install build
```

That copies the library, `resources/semantic/models.json`, and `LICENSE` into `~/.local/share/nextpad++/plugins/SemanticSearch/`. Set `-DPLUGIN_INSTALL_DIR` to install somewhere else. The editor also scans `/usr/lib/nextpad-plus-plus/plugins` and `/usr/local/lib/nextpad-plus-plus/plugins`, using the same `<Name>/<Name>.so` layout.

Open Plugins, then Semantic Search, then Semantic Heatmap Search. Preferences are written to `~/.local/share/nextpad++/plugins/Config/semantic-search.ini`. Downloaded models go to `~/.local/share/nextpad++/semantic-models/<model id>/`. Both paths follow `-settingsDir` and stay on this machine when settings are redirected to a cloud folder.

The first search downloads the model with curl and checks the SHA-256 sums in the manifest. The files come from huggingface.co.

## ONNX Runtime

```
tools/setup-semantic-runtime.sh
export ONNXRUNTIME_ROOT="$PWD/.cache/onnxruntime/current"
```

The first argument is the prefix. The default is `.cache/onnxruntime` inside this repository. The second argument is `auto`, `cpu`, `cuda`, or `openvino`. `auto` selects CUDA 12 or CUDA 13 when a device, the matching runtime, and cuDNN 9 are present, then OpenVINO on an Intel GPU on x86_64, and otherwise CPU. The script installs a wheel with `uv` or `pip`:

| Choice | Package | Version |
| --- | --- | --- |
| CPU | onnxruntime | 1.24.1 |
| CUDA 12 on x86_64 | onnxruntime-gpu | 1.24.1 |
| CUDA 13 | onnxruntime-gpu | 1.31.0 |
| OpenVINO on an Intel GPU, x86_64 | onnxruntime-openvino | 1.24.1 |

Wheels come from PyPI. A Jetson board (aarch64 with a Tegra userland) is not given a generic wheel. Those wheels target different GPUs. Point `ONNXRUNTIME_ROOT` at an ONNX Runtime built for that board, with `libonnxruntime.so` and `libonnxruntime_providers_cuda.so` in `lib/`.

The panel lists Automatic, CPU, CUDA, and OpenVINO. Automatic uses the same order at inference time. A provider that fails to create a session falls back to CPU.

## Limits

A document larger than 2 MiB is not indexed. A document that is not UTF-8 is not indexed. At most 4096 sentences are embedded. The query waits 300 ms after typing stops. An edit waits 600 ms before the index is rebuilt. Colours are relative to the scores in the current document. The document map and a split view pick up the heatmap style because the plugin walks Scintilla widgets under the main window. A view that is not a child of that window is missed. [docs/editor-api-gaps.md](docs/editor-api-gaps.md) lists each of these against the host source.

## Tests

`ctest` covers the tokenizer, the sentence splitter, heatmap colours, the model manifest, download cancellation, worker shutdown, the indicator helper from the editor proposal, a loader check of the `.so`, and the runtime-selection script. The loader check does not need a display. Embedding numbers run only when `NPP_SEMANTIC_TEST_MODEL_DIR` contains model files. When `libonnxruntime` is absent, the probe reports that it is disabled.

`tools/smoke-heatmap.sh` builds Nextpad++ Linux from upstream main without modifying that tree, installs this plugin into a temporary data directory, and checks that a heatmap paints under Xvfb. The public editor sources do not include the maintainer's Scintilla patches, and `src/main.c` calls `macrobatch_show_dialog` without a declaration. The script's comments describe the two build aids it applies outside the tracked editor files.

## License

GNU General Public License, version 3. See `LICENSE`. The ONNX Runtime C headers under `src/engine/ort/` are MIT licensed. See `src/engine/ort/LICENSE`.
