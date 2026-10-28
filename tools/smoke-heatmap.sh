#!/usr/bin/env bash
# Load this plugin into an unmodified build of Nextpad++ Linux (upstream
# main) and check that a semantic heatmap paints under Xvfb.
#
# Two build aids live outside the tracked editor files:
#
# 1. scintilla/ and scintilla-patches/ are gitignored. A public clone does
#    not contain the patch that defines gNppUseBoostRegex, which prefs.c
#    references. This script clones bugaevc/scintilla (gtk4) into the
#    gitignored scintilla/ directory and adds one .cxx file, plus a makefile
#    object line, so the symbol exists. Default regex search stays
#    Scintilla's own (SCI_OWNREGEX is not defined).
#
# 2. src/main.c calls macrobatch_show_dialog without including macrobatch.h.
#    The function is defined in src/macrobatch.c. Clang treats the implicit
#    declaration as an error. The editor is configured with
#    -Wno-error=implicit-function-declaration. No tracked editor source is
#    edited.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${NPP_SMOKE_WORK:-/tmp/npp-semantic-smoke}
editor_src=${NPP_EDITOR_SRC:-"$work/nextpad-plus-plus-linux"}
editor_commit=${NPP_EDITOR_COMMIT:-8dec39c9f0f53aefc662520717fb7c0b6107fcdb}
scintilla_commit=${NPP_SCINTILLA_COMMIT:-3ba9b8f09851b60180ce57ee908c6eb57c751368}
art=${NPP_SMOKE_ARTIFACT_DIR:-"$work/artifacts"}
sample="$root/tests/fixtures/smoke/sample.txt"

mkdir -p "$work" "$art"
log="$art/editor.log"
status="$art/status.txt"
rm -f "$status" "$log"

if [[ ! -x "${NPP_EDITOR_BIN:-}" ]]; then
    if [[ ! -d "$editor_src/.git" ]]; then
        rm -rf "$editor_src"
        git clone --depth 1 https://github.com/nextpad-plus-plus/nextpad-plus-plus-linux.git "$editor_src"
        git -C "$editor_src" fetch --depth 1 origin "$editor_commit"
        git -C "$editor_src" checkout --detach "$editor_commit"
    fi
    if [[ ! -d "$editor_src/scintilla/.git" ]]; then
        rm -rf "$editor_src/scintilla"
        git clone --depth 1 --branch gtk4 https://github.com/bugaevc/scintilla.git "$editor_src/scintilla"
        git -C "$editor_src/scintilla" fetch --depth 1 origin "$scintilla_commit"
        git -C "$editor_src/scintilla" checkout --detach "$scintilla_commit"
    fi
    stub="$editor_src/scintilla/gtk4/npp_regex_toggle.cxx"
    if [[ ! -f "$stub" ]]; then
        cat > "$stub" << 'EOF'
/* Build aid. The editor references this symbol. The maintainer's
 * scintilla-patches compile it into libscintilla.so and are not in the
 * public tree. Default regex search stays Scintilla's own. */
extern "C" __attribute__((visibility("default"))) bool gNppUseBoostRegex = false;
EOF
    fi
    python3 - "$editor_src/scintilla/gtk4/makefile" << 'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
if "npp_regex_toggle.o" in text:
    raise SystemExit(0)
needle = "\tListBoxGTK.o\n"
if needle not in text:
    raise SystemExit("gtk4 makefile has no ListBoxGTK.o line to extend")
path.write_text(text.replace(needle, "\tListBoxGTK.o \\\n\tnpp_regex_toggle.o\n", 1))
PY
    cmake -S "$editor_src" -B "$editor_src/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-Wno-error=implicit-function-declaration"
    cmake --build "$editor_src/build" -j"$(nproc)"
    NPP_EDITOR_BIN="$editor_src/build/Nextpad++"
fi

cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build" -j"$(nproc)" --target SemanticSearch
so="$root/build/SemanticSearch.so"
[[ -f "$so" ]] || { echo "plugin .so was not produced" >&2; exit 1; }

ort_prefix="$root/.cache/onnxruntime"
if [[ ! -e "$ort_prefix/current/lib/libonnxruntime.so" && ! -e "$ort_prefix/current/lib/libonnxruntime.so.1" ]]; then
    "$root/tools/setup-semantic-runtime.sh" "$ort_prefix" cpu
fi
ort_root=$(readlink -f "$ort_prefix/current")

home="$work/home"
data="$home/.local/share"
rm -rf "$home"
mkdir -p "$data/nextpad++/plugins/SemanticSearch/resources/semantic" \
         "$data/nextpad++/semantic-models/all-MiniLM-L6-v2-fp32" \
         "$home/.config" "$home/.cache"
# A fresh data dir would otherwise ask about an update and cover the editor.
cat > "$data/nextpad++/config.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8" ?>
<NotepadPlus>
    <GUIConfigs>
        <GUIConfig name="UpdateAutoCheck" name2="hide" />
    </GUIConfigs>
</NotepadPlus>
EOF
cp "$so" "$data/nextpad++/plugins/SemanticSearch/SemanticSearch.so"
cp "$root/resources/semantic/models.json" \
   "$data/nextpad++/plugins/SemanticSearch/resources/semantic/models.json"
cp "$root/LICENSE" "$data/nextpad++/plugins/SemanticSearch/LICENSE"

model_dir="$data/nextpad++/semantic-models/all-MiniLM-L6-v2-fp32"
cache="$work/model-cache"
mkdir -p "$cache"
fetch() {
    local url=$1 name=$2 sum=$3
    local cached="$cache/$name"
    if [[ ! -f "$cached" ]] || ! echo "$sum  $cached" | sha256sum -c - >/dev/null 2>&1; then
        curl -fL --retry 3 --retry-delay 2 -o "$cached.partial" "$url"
        mv "$cached.partial" "$cached"
        echo "$sum  $cached" | sha256sum -c -
    fi
    cp -f "$cached" "$model_dir/$name"
}
fetch "https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/resolve/main/vocab.txt" \
      vocab.txt \
      "07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3"
fetch "https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/resolve/main/onnx/model.onnx" \
      model.onnx \
      "6fd5d72fe4589f189f8ebc006442dbb529bb7ce38f8082112682524616046452"

export HOME="$home"
export XDG_DATA_HOME="$data"
export XDG_CONFIG_HOME="$home/.config"
export XDG_CACHE_HOME="$home/.cache"
export ONNXRUNTIME_ROOT="$ort_root"
export GDK_BACKEND=x11
export NPP_SEMANTIC_SMOKE="cats and kittens"
export NPP_SEMANTIC_SMOKE_STATUS="$status"
unset WAYLAND_DISPLAY || true

display_num=99
while [[ -e "/tmp/.X11-unix/X${display_num}" ]]; do
    display_num=$((display_num + 1))
done
export DISPLAY=":${display_num}"
Xvfb "$DISPLAY" -screen 0 1400x900x24 >"$art/xvfb.log" 2>&1 &
xvfb_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [[ -e "/tmp/.X11-unix/X${display_num}" ]] && break
    sleep 0.2
done

dbus-run-session -- \
    "$NPP_EDITOR_BIN" -multiInst "$sample" \
    >"$log" 2>&1 &
app_pid=$!

cleanup() {
    if kill -0 "$app_pid" 2>/dev/null; then
        kill "$app_pid" 2>/dev/null || true
        sleep 1
        kill -9 "$app_pid" 2>/dev/null || true
    fi
    wait "$app_pid" 2>/dev/null || true
    if kill -0 "${xvfb_pid:-}" 2>/dev/null; then
        kill "$xvfb_pid" 2>/dev/null || true
        wait "$xvfb_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

deadline=$((SECONDS + 90))
while [[ ! -s "$status" ]]; do
    if ! kill -0 "$app_pid" 2>/dev/null; then
        echo "editor exited before the heatmap status was written" >&2
        tail -n 80 "$log" >&2 || true
        exit 1
    fi
    if (( SECONDS > deadline )); then
        echo "timed out waiting for $status" >&2
        tail -n 80 "$log" >&2 || true
        exit 1
    fi
    sleep 1
done

if ! grep -q "plugin: loaded 'Semantic Search'" "$log"; then
    echo "plugin did not load" >&2
    tail -n 80 "$log" >&2 || true
    exit 1
fi
head -n 1 "$status" | grep -qx "ok"
grep -E '^color 0x[0-9a-fA-F]{6}$' "$status" | grep -vq 'color 0x000000'
grep -E '^sentences [1-9]' "$status" >/dev/null
echo "heatmap status:"
cat "$status"

sleep 1
shot="$art/heatmap.png"
capture() {
    xdotool search --onlyvisible --name 'Nextpad' >"$art/windows.txt" || true
    local wid
    wid=$(head -n 1 "$art/windows.txt" || true)
    [[ -n "${wid:-}" ]] || return 1
    xdotool windowsize "$wid" 1280 800 || true
    xdotool windowmove "$wid" 40 40 || true
    sleep 0.6
    # ImageMagick's import reads the X window directly. xwd is not required.
    import -window "$wid" -silent "$shot"
}
if ! capture; then
    echo "could not capture the Nextpad++ window" >&2
    tail -n 40 "$log" >&2 || true
    exit 1
fi
[[ -s "$shot" ]] || { echo "screenshot is empty" >&2; exit 1; }
echo "screenshot: $shot"
