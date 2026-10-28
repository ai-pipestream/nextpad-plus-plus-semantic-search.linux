/*
 * Semantic search panel for the Linux plugin.
 *
 * The indexing, download, and paint decisions follow the in-editor bar from
 * nextpad-plus-plus-linux plugin/semantic (d23c099). Editor access goes
 * through EditorBridge instead of editor-private hooks.
 */
#include "EditorBridge.h"
#include "scintilla_min.h"

#include "BackgroundWorker.h"
#include "BackendConsistency.h"
#include "CancellableSpawn.h"
#include "DocumentTrack.h"
#include "ExecutionProviders.h"
#include "HeatmapApply.h"
#include "ModelZoo.h"
#include "OrtRuntime.h"
#include "SemanticHeatmapColors.h"
#include "SentenceSplitter.h"

#include <gdk/gdkkeysyms.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr long kMaxBytes = 2L * 1024L * 1024L;
constexpr size_t kMaxSentences = 4096;
constexpr unsigned kQueryDebounceMs = 300;
constexpr unsigned kRebuildDebounceMs = 600;
constexpr size_t kCacheLimit = 12288;

struct Span {
    int64_t byte_start = 0;
    int64_t byte_length = 0;
};

/* Idle callbacks and the worker share this gate. shutdown() sets dead
 * before it joins the worker, and every callback checks it before
 * touching a widget. post_main() holds the mutex across that check and
 * the g_idle_add, so a callback cannot be queued once shutdown has
 * started, including after the worker has been joined. */
struct IdleGate {
    std::mutex mu;
    std::atomic<bool> dead{false};
};

IdleGate &idle_gate()
{
    static IdleGate gate;
    return gate;
}

struct MainCall {
    std::function<void()> fn;
};

gboolean dispatch_main(gpointer data)
{
    auto *call = static_cast<MainCall *>(data);
    if (!idle_gate().dead.load()) call->fn();
    delete call;
    return G_SOURCE_REMOVE;
}

void post_main(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(idle_gate().mu);
    if (idle_gate().dead.load()) return;
    g_idle_add(dispatch_main, new MainCall{[fn = std::move(fn)] {
        if (idle_gate().dead.load()) return;
        fn();
    }});
}

struct Controller {
    BackgroundWorker worker;
    /* One spawner for model downloads. shutdown() retires it so a curl
     * that is still running is killed, and a download that has not
     * started does not start. */
    CancellableSpawn downloads;
    bool shut_down = false;

    GtkWidget *panel = nullptr;
    long panel_handle = 0;
    GtkWidget *entry = nullptr;
    GtkWidget *status = nullptr;
    GtkWidget *spinner = nullptr;
    GtkWidget *model_dd = nullptr;
    GtkWidget *sens_dd = nullptr;
    GtkWidget *ep_dd = nullptr;
    GtkWidget *swatch[6] = {};
    GtkCssProvider *legend_css = nullptr;

    bool ui_ready = false;
    bool open = false;
    int sensitivity = 0;
    unsigned band_mask = 0;
    std::string model_id;
    std::string ep_id = "auto";
    std::vector<std::string> model_ids;
    std::vector<std::string> ep_ids;

    GtkWidget *sci = nullptr;
    void *buffer = nullptr;
    std::vector<Span> spans;
    int64_t first_id = 1;
    int64_t next_id = 1;
    bool index_ready = false;
    std::string query;
    std::string index_status;
    std::vector<SemanticHit> last_hits;
    DocumentTrack track;
    /* Slot from NPPM_ALLOCATEINDICATOR, skipping editor-owned numbers. */
    int indicator = -1;
    int bookmark_marker = 20;
    guint restyle_timer = 0;

    std::atomic<uint64_t> build_gen{0};
    std::atomic<uint64_t> query_gen{0};
    guint query_timer = 0;
    guint rebuild_timer = 0;

    MarkerSyncState bookmark_state;
    ModelZoo zoo;
    std::unique_ptr<EmbeddingProvider> provider;
    std::unique_ptr<SimilarityEngine> engine;
    std::unique_ptr<VectorIndex> index;
    std::unordered_map<std::string, std::vector<float>> cache;
    std::string loaded_model;
    std::string loaded_ep;
    std::string backend_name = "ONNX Runtime CPU";

    // UI-thread state: document messages must not hide an active download.
    std::string requested_status;
    bool requested_busy = false;
    std::optional<CancellableSpawn::Ticket> downloading_ticket;
    std::string downloading_status;

    sptr_t send(GtkWidget *w, unsigned msg, uptr_t wp, sptr_t lp)
    {
        return sci_send(w, msg, wp, lp);
    }

    void set_status(const std::string &text, bool busy)
    {
        requested_status = text;
        requested_busy = busy;
        render_status();
    }

    void render_status()
    {
        bool downloading = downloading_ticket && !downloads.cancelled(*downloading_ticket);
        const std::string &text = downloading ? downloading_status : requested_status;
        bool busy = downloading || requested_busy;
        if (status) {
            gtk_label_set_text(GTK_LABEL(status), text.c_str());
            gtk_widget_set_tooltip_text(status, text.c_str());
        }
        if (spinner) {
            gtk_widget_set_visible(spinner, busy);
            if (busy) gtk_spinner_start(GTK_SPINNER(spinner));
            else gtk_spinner_stop(GTK_SPINNER(spinner));
        }
    }

    void save_prefs()
    {
        std::string path = editor_plugins_config_file();
        GKeyFile *kf = g_key_file_new();
        g_key_file_set_integer(kf, "semantic", "sensitivity", sensitivity);
        g_key_file_set_string(kf, "semantic", "model", model_id.c_str());
        g_key_file_set_string(kf, "semantic", "provider", ep_id.c_str());
        g_key_file_save_to_file(kf, path.c_str(), nullptr);
        g_key_file_free(kf);
    }

    void load_prefs()
    {
        std::string path = editor_plugins_config_file();
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
            GError *err = nullptr;
            int sens = g_key_file_get_integer(kf, "semantic", "sensitivity", &err);
            if (!err) sensitivity = std::clamp(sens, -1, 1);
            g_clear_error(&err);
            gchar *model = g_key_file_get_string(kf, "semantic", "model", nullptr);
            gchar *ep = g_key_file_get_string(kf, "semantic", "provider", nullptr);
            if (model && *model) model_id = model;
            if (ep && *ep) ep_id = ep;
            g_free(model);
            g_free(ep);
        }
        g_key_file_free(kf);
    }

    void configure_indicator_on(GtkWidget *w)
    {
        if (!w || indicator < 0) return;
        /* Full-box indicators go through Surface::AlphaRectangle. The GTK4
         * Scintilla surface still stubs that call, so a wash would not
         * paint. Text foreground uses the same per-sentence colour and
         * does draw. */
        send(w, SCI_INDICSETSTYLE, static_cast<uptr_t>(indicator), INDIC_TEXTFORE);
        send(w, SCI_INDICSETFLAGS, static_cast<uptr_t>(indicator), SC_INDICFLAG_VALUEFORE);
    }

    void configure_indicator()
    {
        if (indicator < 0) return;
        if (sci) configure_indicator_on(sci);
        /* Indicator style is per view. The host does not list split clones
         * or the document map, so every ScintillaView under the main window
         * gets the same text-foreground style. Ranges stay on the document. */
        std::vector<void *> views;
        editor_collect_views(views);
        for (void *view : views)
            configure_indicator_on(static_cast<GtkWidget *>(view));
    }

    void schedule_restyle()
    {
        if (shut_down || !open || restyle_timer) return;
        restyle_timer = g_timeout_add(400, [](gpointer data) -> gboolean {
            if (idle_gate().dead.load()) return G_SOURCE_REMOVE;
            auto *self = static_cast<Controller *>(data);
            self->restyle_timer = 0;
            if (self->shut_down || idle_gate().dead.load() || !self->open)
                return G_SOURCE_REMOVE;
            self->configure_indicator();
            return G_SOURCE_REMOVE;
        }, this);
    }

    std::uintptr_t doc_pointer(GtkWidget *w)
    {
        if (!w) return 0;
        return static_cast<std::uintptr_t>(send(w, SCI_GETDOCPOINTER, 0, 0));
    }

    void sync_bookmarks(const std::set<int> &want)
    {
        if (!sci) return;
        const int marker = bookmark_marker;
        const unsigned mask = 1u << marker;
        GtkWidget *w = sci;
        MarkerFns fns;
        fns.line_from_handle = [this, w](int handle) {
            return static_cast<int>(send(w, SCI_MARKERLINEFROMHANDLE,
                                         static_cast<uptr_t>(handle), 0));
        };
        fns.delete_handle = [this, w](int handle) {
            send(w, SCI_MARKERDELETEHANDLE, static_cast<uptr_t>(handle), 0);
        };
        fns.mask_on_line = [this, w](int line) {
            return static_cast<unsigned>(send(w, SCI_MARKERGET, static_cast<uptr_t>(line), 0));
        };
        fns.add_on_line = [this, w, marker](int line) {
            return static_cast<int>(send(w, SCI_MARKERADD, static_cast<uptr_t>(line), marker));
        };
        sync_marker_handles(track.handles(), want, mask, fns, &bookmark_state);
    }

    SemanticHeatmap::Profile heatmap_profile() const
    {
        const ModelSpec *spec = zoo.find(model_id);
        return spec ? spec->heatmap_profile : SemanticHeatmap::Profile::MiniLM;
    }

    void refresh_legend()
    {
        if (!legend_css) return;
        GString *css = g_string_new("");
        for (int i = 0; i < 6; ++i) {
            double sample = SemanticHeatmap::legendScore(heatmap_profile(), i);
            uint32_t bgr = SemanticHeatmap::colorBGR(sample, 0, heatmap_profile());
            g_string_append_printf(css,
                "button.sem-band-%d { background: #%02x%02x%02x;"
                " min-width: 16px; min-height: 16px; padding: 0; border-radius: 3px; }"
                "button.sem-band-%d:checked { box-shadow: inset 0 0 0 2px currentColor; }",
                i, bgr & 255, (bgr >> 8) & 255, (bgr >> 16) & 255, i);
        }
        gtk_css_provider_load_from_string(legend_css, css->str);
        g_string_free(css, TRUE);
    }

    void paint(const std::vector<SemanticHit> &hits)
    {
        if (!sci) return;
        configure_indicator();
        sptr_t doc_len = send(sci, SCI_GETLENGTH, 0, 0);
        if (indicator < 0) return;
        send(sci, SCI_SETINDICATORCURRENT, static_cast<uptr_t>(indicator), 0);
        send(sci, SCI_INDICATORCLEARRANGE, 0, doc_len);
        std::set<int> want_lines;
        for (const SemanticHit &hit : hits) {
            int band = SemanticHeatmap::bandForScore(hit.score, heatmap_profile());
            if (band_mask && (band < 0 || !(band_mask & (1u << band)))) continue;
            if (hit.sentence_id < first_id) continue;
            size_t idx = static_cast<size_t>(hit.sentence_id - first_id);
            if (idx >= spans.size()) continue;
            const Span &span = spans[idx];
            if (span.byte_start >= doc_len || span.byte_length <= 0) continue;
            long len = std::min(span.byte_length, static_cast<int64_t>(doc_len - span.byte_start));
            uint32_t color = SemanticHeatmap::colorBGR(hit.score, sensitivity, heatmap_profile());
            send(sci, SCI_SETINDICATORVALUE,
                 static_cast<uptr_t>(color | SC_INDICVALUEBIT), 0);
            send(sci, SCI_INDICATORFILLRANGE, static_cast<uptr_t>(span.byte_start), len);
            if (band_mask) {
                int line = static_cast<int>(send(sci, SCI_LINEFROMPOSITION,
                                                 static_cast<uptr_t>(span.byte_start), 0));
                want_lines.insert(line);
            }
        }
        sync_bookmarks(want_lines);
        note_smoke(hits);
    }

    void note_smoke(const std::vector<SemanticHit> &hits)
    {
        const char *flag = std::getenv("NPP_SEMANTIC_SMOKE");
        if (!flag || !*flag || !sci || hits.empty() || spans.empty()) return;
        const char *path = std::getenv("NPP_SEMANTIC_SMOKE_STATUS");
        if (!path || !*path) path = "/tmp/semantic-smoke-status.txt";
        uint32_t color = SemanticHeatmap::colorBGR(hits.front().score, sensitivity, heatmap_profile());
        FILE *out = std::fopen(path, "w");
        if (!out) return;
        std::fprintf(out, "ok\nindicator %d\ncolor 0x%06x\nsentences %zu\nbackend %s\n",
                     indicator, color, hits.size(), backend_name.c_str());
        std::fclose(out);
    }

    void clear_heatmap()
    {
        last_hits.clear();
        if (!sci || indicator < 0) {
            if (sci) sync_bookmarks({});
            return;
        }
        send(sci, SCI_SETINDICATORCURRENT, static_cast<uptr_t>(indicator), 0);
        send(sci, SCI_INDICATORCLEARRANGE, 0, send(sci, SCI_GETLENGTH, 0, 0));
        sync_bookmarks({});
    }

    std::string storage_dir(const std::string &id) const
    {
        return editor_model_dir(id);
    }

    void schedule_query()
    {
        if (shut_down || idle_gate().dead.load()) return;
        if (query_timer) g_source_remove(query_timer);
        const char *text = entry ? gtk_editable_get_text(GTK_EDITABLE(entry)) : "";
        if (!text || !*text) {
            query_timer = 0;
            apply_query("");
            return;
        }
        query_timer = g_timeout_add(kQueryDebounceMs, [](gpointer data) -> gboolean {
            if (idle_gate().dead.load()) return G_SOURCE_REMOVE;
            auto *self = static_cast<Controller *>(data);
            self->query_timer = 0;
            if (self->shut_down || idle_gate().dead.load()) return G_SOURCE_REMOVE;
            const char *now = gtk_editable_get_text(GTK_EDITABLE(self->entry));
            self->apply_query(now ? now : "");
            return G_SOURCE_REMOVE;
        }, this);
    }

    void apply_query(const std::string &text)
    {
        query = text;
        query_gen.fetch_add(1);
        clear_heatmap();
        if (query.empty()) {
            if (index_ready) set_status(index_status, false);
            return;
        }
        if (!index_ready) return;
        run_query();
    }

    // Worker thread only. Never score a new backend against the old index.
    bool rebuild_after_backend_change(uint64_t before, uint64_t gen)
    {
        if (!invalidate_changed_backend(*provider, before, *index, cache)) return false;
        // Invalidate already queued UI results before requesting the rebuild.
        if (!build_gen.compare_exchange_strong(gen, gen + 1)) return true;
        post_main([this, expected = gen + 1] {
            if (build_gen.load() != expected || idle_gate().dead.load()) return;
            index_ready = false;
            clear_heatmap();
            rebuild_now();
        });
        return true;
    }

    void run_query()
    {
        uint64_t qgen = query_gen.load();
        uint64_t bgen = build_gen.load();
        std::string q = query;
        worker.post([this, q, qgen, bgen] {
            if (qgen != query_gen.load() || bgen != build_gen.load() || !provider || !index) return;
            std::vector<float> qvec;
            std::string error;
            const auto backend = provider->backend_generation();
            bool embedded = provider->embed_batch({q}, qvec, error, {});
            if (rebuild_after_backend_change(backend, bgen)) return;
            if (!embedded) {
                if (qgen != query_gen.load() || bgen != build_gen.load()) return;
                post_main([this, qgen, bgen, error] {
                    if (qgen != query_gen.load() || bgen != build_gen.load()) return;
                    set_status(error.empty() ? "Query could not be embedded" : error, false);
                });
                return;
            }
            size_t n = index->count();
            std::vector<SemanticHit> hits(n);
            size_t got = n ? index->score_all(qvec.data(), hits.data(), n) : 0;
            hits.resize(got);
            post_main([this, qgen, bgen, hits = std::move(hits), got, n] {
                if (idle_gate().dead.load()) return;
                /* Both generations are read on this thread. A mismatch
                 * means a newer query or document already updated the
                 * screen, so this result must not paint or clear it. */
                if (qgen != query_gen.load() || bgen != build_gen.load()) return;
                if (got != n) {
                    clear_heatmap();
                    set_status("Could not score sentence embeddings", false);
                    return;
                }
                HeatmapApply action = heatmap_apply(false, bgen, build_gen.load(),
                                                    qgen, query_gen.load(),
                                                    query.empty(), hits.empty());
                if (action == HeatmapApply::Clear) {
                    clear_heatmap();
                    set_status(index_status, false);
                    return;
                }
                if (action != HeatmapApply::Paint) return;
                last_hits = hits;
                paint(last_hits);
                set_status(index_status, false);
            });
        });
    }

    void schedule_rebuild()
    {
        if (shut_down || idle_gate().dead.load()) return;
        if (rebuild_timer) g_source_remove(rebuild_timer);
        rebuild_timer = g_timeout_add(kRebuildDebounceMs, [](gpointer data) -> gboolean {
            if (idle_gate().dead.load()) return G_SOURCE_REMOVE;
            auto *self = static_cast<Controller *>(data);
            self->rebuild_timer = 0;
            if (self->shut_down || idle_gate().dead.load()) return G_SOURCE_REMOVE;
            self->rebuild_now();
            return G_SOURCE_REMOVE;
        }, this);
    }

    void rebuild_now()
    {
        if (shut_down || idle_gate().dead.load()) return;
        if (!open || !sci || !buffer) return;
        uint64_t gen = build_gen.fetch_add(1) + 1;
        // Document generations invalidate sentence results, not model assets.
        // Reuse a download already running for this model/backend selection.
        const auto download_ticket = downloads.ticket();
        index_ready = false;
        clear_heatmap();
        sptr_t doc_len = send(sci, SCI_GETLENGTH, 0, 0);
        int codepage = static_cast<int>(send(sci, SCI_GETCODEPAGE, 0, 0));
        if (doc_len > kMaxBytes) {
            set_status("Document too large for semantic search", false);
            return;
        }
        if (codepage != 0 && codepage != SC_CP_UTF8) {
            set_status("Document is not valid UTF-8", false);
            return;
        }
        const char *chars = reinterpret_cast<const char *>(send(sci, SCI_GETCHARACTERPOINTER, 0, 0));
        std::string bytes = (chars && doc_len > 0) ? std::string(chars, static_cast<size_t>(doc_len))
                                                    : std::string();
        if (!bytes.empty() && !g_utf8_validate(bytes.data(), static_cast<gssize>(bytes.size()), nullptr)) {
            set_status("Document is not valid UTF-8", false);
            return;
        }
        int64_t first = next_id;
        std::string q = query;
        uint64_t qgen = query_gen.load();
        std::string want_model = model_id;
        std::string want_ep = ep_id;
        set_status("Indexing...", true);
        // This build generation supersedes all pending builds and queries.
        // Keep only its snapshot while the worker finishes a model download.
        worker.post_latest([this, gen, bytes = std::move(bytes), first, q, qgen, want_model, want_ep, download_ticket] {
            if (idle_gate().dead.load() || worker.stopping() || gen != build_gen.load()) return;
            build_job(gen, bytes, first, q, qgen, want_model, want_ep, download_ticket);
        });
    }

    bool ensure_provider(const std::string &want_model, const std::string &want_ep, CancellableSpawn::Ticket ticket, std::string &error)
    {
        const ModelSpec *spec = zoo.find(want_model);
        if (!spec) {
            error = "Unknown semantic model";
            return false;
        }
        bool same = provider && provider->available() && loaded_model == spec->id && loaded_ep == want_ep;
        if (same) return true;
        if (downloads.cancelled(ticket)) { error = "cancelled"; return false; }
        if (!npp_ort_runtime_available()) {
            error = npp_ort_missing_message();
            return false;
        }
        std::string dir = storage_dir(spec->id);
        std::string name = spec->name;
        bool installed = zoo.ensure(*spec, dir, error, &downloads, ticket, [this, ticket, name] {
            post_main([this, ticket, name] {
                if (downloads.cancelled(ticket) || !open) return;
                downloading_ticket = ticket;
                downloading_status = "Downloading " + name + "...";
                render_status();
            });
        });
        post_main([this, ticket] {
            // An older completion must not clear a newer model's status.
            if (downloading_ticket != ticket) return;
            downloading_ticket.reset();
            downloading_status.clear();
            render_status();
        });
        if (!installed) return false;
        if (downloads.cancelled(ticket)) { error = "cancelled"; return false; }
        OnnxProviderConfig cfg;
        cfg.model_path = dir + "/" + spec->model.filename;
        cfg.vocab_path = dir + "/" + spec->vocab.filename;
        cfg.dimension = spec->dimension;
        cfg.max_tokens = spec->max_tokens;
        cfg.pooling = spec->pooling;
        cfg.execution_provider = want_ep;
        auto created = make_onnx_provider(cfg, error);
        if (!created) return false;
        if (downloads.cancelled(ticket)) { error = "cancelled"; return false; }
        cache.clear(); // A new session must never inherit a previous backend's vectors.
        provider = std::move(created);
        loaded_model = spec->id;
        loaded_ep = want_ep;
        backend_name = provider->backend_name();
        if (!engine) engine = make_cpu_similarity_engine();
        index = make_memory_vector_index(*engine);
        return true;
    }

    void build_job(uint64_t gen, const std::string &bytes, int64_t first,
                   const std::string &q, uint64_t qgen,
                   const std::string &want_model, const std::string &want_ep,
                   CancellableSpawn::Ticket ticket)
    {
        if (idle_gate().dead.load() || worker.stopping() || gen != build_gen.load()) return;
        auto spans_now = split_sentences(bytes);
        if (spans_now.size() > kMaxSentences) {
            post_main([this, gen] {
                if (gen != build_gen.load()) return;
                set_status("Document has too many sentences for semantic search", false);
            });
            return;
        }
        std::vector<Span> light;
        light.reserve(spans_now.size());
        for (const SentenceSpan &s : spans_now) {
            Span span;
            span.byte_start = s.byte_start;
            span.byte_length = s.byte_length;
            light.push_back(span);
        }
        post_main([this, gen, first, light] {
            if (gen != build_gen.load()) return;
            spans = light;
            first_id = first;
        });
        if (spans_now.empty()) {
            post_main([this, gen] {
                if (gen != build_gen.load()) return;
                next_id = first_id;
                index_ready = true;
                index_status = "No sentences to search";
                set_status(index_status, false);
            });
            return;
        }
        std::string error;
        if (!ensure_provider(want_model, want_ep, ticket, error)) {
            if (gen != build_gen.load() || error == "cancelled") return;
            post_main([this, gen, error] {
                if (gen != build_gen.load()) return;
                set_status(error.empty() ? "Could not load the embedding model" : error, false);
            });
            return;
        }
        if (gen != build_gen.load()) return;
        {
            std::string backend = provider->backend_name();
            size_t total = spans_now.size();
            post_main([this, gen, backend, total] {
                if (idle_gate().dead.load() || gen != build_gen.load()) return;
                char buf[256];
                std::snprintf(buf, sizeof buf, "Indexing 0 of %u... · %s",
                              static_cast<unsigned>(total), backend.c_str());
                set_status(buf, true);
            });
        }
        int dim = provider->dimension();
        index->reset(static_cast<size_t>(dim));

        std::vector<float> qvec;
        bool have_q = false;
        auto backend_before = provider->backend_generation();
        if (!q.empty() && qgen == query_gen.load()) {
            std::string qerr;
            if (provider->embed_batch({q}, qvec, qerr, {}) &&
                static_cast<int>(qvec.size()) == dim)
                have_q = true;
        }
        /* A query that knocked the session down to CPU must not reuse
         * sentence vectors cached from the GPU session. */
        if (provider->backend_generation() != backend_before) cache.clear();

        struct Row {
            std::string text;
            bool added = false;
        };
        std::vector<Row> rows(spans_now.size());
        std::vector<std::string> miss_text;
        size_t embedded = 0;
        auto add_row = [&](size_t row, const std::vector<float> &vec) {
            if (rows[row].added) return;
            if (static_cast<int>(vec.size()) != dim) return;
            if (!index->add(vec.data(), first + static_cast<int64_t>(row))) return;
            rows[row].added = true;
            ++embedded;
        };
        for (size_t i = 0; i < spans_now.size(); ++i) {
            rows[i].text = spans_now[i].text;
            auto found = cache.find(rows[i].text);
            if (found != cache.end() && static_cast<int>(found->second.size()) == dim) {
                add_row(i, found->second);
                continue;
            }
            bool queued = false;
            for (size_t m = 0; m < miss_text.size(); ++m) {
                if (miss_text[m] == rows[i].text) {
                    queued = true;
                    break;
                }
            }
            if (!queued) miss_text.push_back(rows[i].text);
        }

        auto publish = [&](bool done) {
            if (idle_gate().dead.load() || gen != build_gen.load()) return;
            std::vector<SemanticHit> hits;
            if (have_q && qgen == query_gen.load() && index->count()) {
                hits.resize(index->count());
                size_t got = index->score_all(qvec.data(), hits.data(), hits.size());
                hits.resize(got);
            }
            size_t total = spans_now.size();
            size_t n_embedded = embedded;
            std::string backend = provider->backend_name();
            std::string model_label = loaded_model;
            if (const ModelSpec *shown = zoo.find(loaded_model)) {
                if (!shown->name.empty()) model_label = shown->name;
            }
            post_main([this, gen, qgen, hits = std::move(hits), done, total,
                       n_embedded, backend, first, model_label] {
                if (idle_gate().dead.load()) return;
                if (gen != build_gen.load()) return;
                next_id = first + static_cast<int64_t>(total);
                /* query_gen is compared here, on the UI thread. Skip
                 * leaves whatever a newer query already drew. Clear
                 * drops colours and this feature's bookmarks when the
                 * current query is empty or this batch has no hits. */
                HeatmapApply action = heatmap_apply(false, gen, build_gen.load(),
                                                    qgen, query_gen.load(),
                                                    query.empty(), hits.empty());
                if (action == HeatmapApply::Clear) clear_heatmap();
                else if (action == HeatmapApply::Paint) {
                    last_hits = hits;
                    paint(hits);
                }
                char buf[256];
                if (done) {
                    index_status = std::to_string(n_embedded) + " of " +
                                   std::to_string(total) +
                                   " sentences indexed · " + model_label +
                                   " · " + backend;
                    index_ready = true;
                    set_status(index_status, false);
                    if (!query.empty()) run_query();
                } else {
                    std::snprintf(buf, sizeof buf, "Indexing %u of %u... · %s",
                                  static_cast<unsigned>(n_embedded),
                                  static_cast<unsigned>(total),
                                  backend.c_str());
                    set_status(buf, true);
                }
            });
        };

        auto embed_list = [&](const std::vector<std::string> &texts) -> bool {
            if (texts.empty()) return true;
            std::vector<float> out;
            std::string emb_error;
            bool ok = provider->embed_batch(texts, out, emb_error, [&](const std::vector<size_t> &done_idx) {
                if (gen != build_gen.load() || worker.stopping()) return false;
                /* An empty list is the GPU-to-CPU restart. Drop every
                 * vector added so far, including cache hits from this
                 * session, and let the retry fill the index again. */
                if (done_idx.empty()) {
                    cache.clear();
                    index->reset(static_cast<size_t>(dim));
                    for (auto &row : rows) row.added = false;
                    embedded = 0;
                    have_q = false;
                    publish(false);
                    return gen == build_gen.load() && !worker.stopping();
                }
                for (size_t di : done_idx) {
                    if (di >= texts.size()) continue;
                    const float *src = out.data() + di * static_cast<size_t>(dim);
                    std::vector<float> vec(src, src + dim);
                    cache[texts[di]] = vec;
                    for (size_t row = 0; row < rows.size(); ++row) {
                        if (!rows[row].added && rows[row].text == texts[di])
                            add_row(row, vec);
                    }
                }
                publish(false);
                return gen == build_gen.load() && !worker.stopping();
            });
            if (!ok) {
                if (gen != build_gen.load() || emb_error == "cancelled") return false;
                post_main([this, gen, emb_error] {
                    if (gen != build_gen.load()) return;
                    set_status(emb_error.empty() ? "Could not embed sentences" : emb_error, false);
                });
                return false;
            }
            return true;
        };

        if (!embed_list(miss_text)) return;
        /* Cache hits added before a fallback are the wrong backend.
         * The reset above marked them not added. Embed those texts now,
         * on whatever provider is actually running. */
        std::vector<std::string> left;
        for (const Row &row : rows) {
            if (row.added) continue;
            bool queued = false;
            for (const std::string &have : left) {
                if (have == row.text) {
                    queued = true;
                    break;
                }
            }
            if (!queued) left.push_back(row.text);
        }
        if (!embed_list(left)) return;
        if (!have_q && !q.empty() && qgen == query_gen.load()) {
            std::string qerr;
            std::vector<float> again;
            const auto backend = provider->backend_generation();
            bool embedded_query = provider->embed_batch({q}, again, qerr, {});
            if (rebuild_after_backend_change(backend, gen)) return;
            if (embedded_query && static_cast<int>(again.size()) == dim) {
                qvec = std::move(again);
                have_q = true;
            }
        }
        if (cache.size() > kCacheLimit) cache.clear();
        if (gen != build_gen.load()) return;
        publish(true);
    }

    void attach(GtkWidget *next)
    {
        if (shut_down || idle_gate().dead.load()) return;
        void *next_buf = next ? sci_buffer_id(next) : nullptr;
        std::uintptr_t next_view = reinterpret_cast<std::uintptr_t>(next);
        std::uintptr_t next_doc_ptr = doc_pointer(next);
        DocAttach kind = track.classify_attach(next_view, next_doc_ptr);
        if (kind == DocAttach::SameView) {
            if (open && !index_ready) rebuild_now();
            return;
        }
        if (kind == DocAttach::SameDocument) {
            track.retarget(next_view);
            sci = next;
            buffer = next_buf;
            configure_indicator();
            if (open && !index_ready) rebuild_now();
            return;
        }
        build_gen.fetch_add(1);
        query_gen.fetch_add(1);
        if (rebuild_timer) {
            g_source_remove(rebuild_timer);
            rebuild_timer = 0;
        }
        clear_heatmap();
        track.switch_to(next_view, next_doc_ptr);
        sci = next;
        buffer = next_buf;
        index_ready = false;
        spans.clear();
        if (!sci || !buffer || next_doc_ptr == 0) return;
        configure_indicator();
        rebuild_now();
    }

    void close_bar()
    {
        if (shut_down || idle_gate().dead.load()) return;
        open = false;
        if (query_timer) g_source_remove(query_timer);
        if (rebuild_timer) g_source_remove(rebuild_timer);
        if (restyle_timer) g_source_remove(restyle_timer);
        query_timer = 0;
        rebuild_timer = 0;
        restyle_timer = 0;
        build_gen.fetch_add(1);
        query_gen.fetch_add(1);
        downloads.cancel();
        clear_heatmap();
        track.switch_to(0, 0);
        index_ready = false;
        sci = nullptr;
        buffer = nullptr;
        editor_hide_panel(panel_handle);
        editor_set_menu_check(false);
        GtkWidget *cur = static_cast<GtkWidget *>(editor_current_sci());
        if (cur) gtk_widget_grab_focus(cur);
    }

    /* Idempotent. The destructor calls this so the worker is joined
     * before the provider, index, and model registry are destroyed.
     * editor_close_all_quit() calls it earlier, while document widgets
     * are still alive. */
    void shutdown()
    {
        if (shut_down) return;
        shut_down = true;
        {
            std::lock_guard<std::mutex> lock(idle_gate().mu);
            idle_gate().dead.store(true);
        }
        if (query_timer) g_source_remove(query_timer);
        if (rebuild_timer) g_source_remove(rebuild_timer);
        if (restyle_timer) g_source_remove(restyle_timer);
        query_timer = 0;
        rebuild_timer = 0;
        restyle_timer = 0;
        build_gen.fetch_add(1);
        query_gen.fetch_add(1);
        downloads.retire();
        /* The session is released on the worker, after the in-flight job
         * returns and before that thread exits. join then returns. */
        worker.set_on_stop([this] { provider.reset(); });
        worker.shutdown();
        /* The index holds a reference to the engine. */
        index.reset();
        engine.reset();
        provider.reset();
    }

    ~Controller() { shutdown(); }
};

Controller &ctrl()
{
    static Controller c;
    return c;
}

void on_entry_changed(GtkEditable *, gpointer)
{
    if (idle_gate().dead.load() || !ctrl().ui_ready) return;
    ctrl().schedule_query();
}

gboolean on_entry_key(GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer)
{
    if (idle_gate().dead.load()) return FALSE;
    if (keyval == GDK_KEY_Escape) {
        ctrl().close_bar();
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (ctrl().query_timer) g_source_remove(ctrl().query_timer);
        ctrl().query_timer = 0;
        const char *text = gtk_editable_get_text(GTK_EDITABLE(ctrl().entry));
        ctrl().apply_query(text ? text : "");
        return TRUE;
    }
    return FALSE;
}

void on_sensitivity(GObject *, GParamSpec *, gpointer)
{
    if (idle_gate().dead.load() || !ctrl().ui_ready || !ctrl().sens_dd) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctrl().sens_dd));
    if (sel > 2) return;
    ctrl().sensitivity = static_cast<int>(sel) - 1;
    ctrl().save_prefs();
    ctrl().paint(ctrl().last_hits);
}

void on_model(GObject *, GParamSpec *, gpointer)
{
    if (idle_gate().dead.load() || !ctrl().ui_ready || !ctrl().model_dd) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctrl().model_dd));
    if (sel >= ctrl().model_ids.size()) return;
    if (ctrl().model_ids[sel] == ctrl().model_id) return;
    ctrl().downloads.cancel();
    ctrl().model_id = ctrl().model_ids[sel];
    ctrl().refresh_legend();
    ctrl().save_prefs();
    if (ctrl().open) ctrl().rebuild_now();
}

void on_ep(GObject *, GParamSpec *, gpointer)
{
    if (idle_gate().dead.load() || !ctrl().ui_ready || !ctrl().ep_dd) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctrl().ep_dd));
    if (sel >= ctrl().ep_ids.size()) return;
    if (ctrl().ep_ids[sel] == ctrl().ep_id) return;
    ctrl().downloads.cancel();
    ctrl().ep_id = ctrl().ep_ids[sel];
    ctrl().save_prefs();
    if (ctrl().open) ctrl().rebuild_now();
}

void on_swatch(GtkToggleButton *button, gpointer)
{
    if (idle_gate().dead.load() || !ctrl().ui_ready) return;
    int band = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "band"));
    if (band < 0 || band > 5) return;
    if (gtk_toggle_button_get_active(button)) ctrl().band_mask |= (1u << band);
    else ctrl().band_mask &= ~(1u << band);
    ctrl().paint(ctrl().last_hits);
}

void on_close_clicked(GtkButton *, gpointer)
{
    if (idle_gate().dead.load()) return;
    ctrl().close_bar();
}

void on_panel_unmap(GtkWidget *widget, gpointer)
{
    if (idle_gate().dead.load()) return;
    Controller &c = ctrl();
    if (!c.open) return;
    GtkWidget *root = widget ? GTK_WIDGET(gtk_widget_get_root(widget)) : nullptr;
    if (root && !gtk_widget_get_mapped(root)) return;
    c.close_bar();
}

} /* namespace */

static void ensure_panel()
{
    Controller &c = ctrl();
    if (c.panel) return;
    std::string manifest_error;
    std::string manifest = editor_manifest_path();
    if (!c.zoo.load_file(manifest, manifest_error))
        c.index_status = manifest_error.empty() ? "Semantic model list is missing" : manifest_error;
    c.load_prefs();
    if (c.model_id.empty() || !c.zoo.find(c.model_id))
        c.model_id = c.zoo.default_id();
    c.bookmark_marker = editor_bookmark_marker();

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 8);
    gtk_widget_set_margin_top(outer, 6);
    gtk_widget_set_margin_bottom(outer, 6);

    /* The host docks plugin panels on the right, so a single horizontal
     * strip leaves the query a few characters wide. Stack the rows. */
    GtkWidget *row_query = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *row_models = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *row_legend = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(outer), row_query);
    gtk_box_append(GTK_BOX(outer), row_models);
    gtk_box_append(GTK_BOX(outer), row_legend);

    GtkWidget *label = gtk_label_new("Find:");
    c.entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(c.entry), "Describe what you're looking for...");
    gtk_accessible_update_property(GTK_ACCESSIBLE(c.entry),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Semantic search query", -1);
    gtk_widget_set_hexpand(c.entry, TRUE);
    g_signal_connect(c.entry, "changed", G_CALLBACK(on_entry_changed), nullptr);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_entry_key), nullptr);
    gtk_widget_add_controller(c.entry, keys);

    GtkStringList *models = gtk_string_list_new(nullptr);
    for (const ModelSpec &spec : c.zoo.models()) {
        gtk_string_list_append(models, spec.name.c_str());
        c.model_ids.push_back(spec.id);
    }
    c.model_dd = gtk_drop_down_new(G_LIST_MODEL(models), nullptr);
    gtk_widget_set_tooltip_text(c.model_dd, "Sentence embedding model");

    GtkStringList *eps = gtk_string_list_new(nullptr);
    for (const ExecProviderInfo &ep : execution_provider_catalog()) {
        std::string label_ep = ep.id;
        for (char &ch : label_ep)
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        if (ep.id == "cpu") label_ep = "CPU";
        if (ep.id == "auto") label_ep = "Automatic";
        gtk_string_list_append(eps, label_ep.c_str());
        c.ep_ids.push_back(ep.id);
    }
    if (c.ep_ids.empty()) {
        gtk_string_list_append(eps, "CPU");
        c.ep_ids.push_back("cpu");
    }
    c.ep_dd = gtk_drop_down_new(G_LIST_MODEL(eps), nullptr);
    gtk_widget_set_tooltip_text(c.ep_dd,
        "Execution provider. A missing GPU provider falls back to CPU.");

    GtkWidget *close = gtk_button_new_with_label("x");
    gtk_widget_set_tooltip_text(close, "Close semantic search");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), nullptr);

    gtk_widget_set_hexpand(c.model_dd, TRUE);
    gtk_widget_set_hexpand(c.ep_dd, TRUE);
    gtk_box_append(GTK_BOX(row_query), label);
    gtk_box_append(GTK_BOX(row_query), c.entry);
    gtk_box_append(GTK_BOX(row_models), c.model_dd);
    gtk_box_append(GTK_BOX(row_models), c.ep_dd);
    gtk_box_append(GTK_BOX(row_models), close);

    GtkStringList *sens = gtk_string_list_new(nullptr);
    gtk_string_list_append(sens, "Strict");
    gtk_string_list_append(sens, "Standard");
    gtk_string_list_append(sens, "Broad");
    c.sens_dd = gtk_drop_down_new(G_LIST_MODEL(sens), nullptr);
    gtk_widget_set_tooltip_text(c.sens_dd,
        "Heatmap sensitivity. Broad paints weaker matches green. Strict requires stronger matches. Scores stay the same.");

    GtkWidget *swatches = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    for (int i = 0; i < 6; ++i) {
        c.swatch[i] = gtk_toggle_button_new();
        char cls[32];
        std::snprintf(cls, sizeof cls, "sem-band-%d", i);
        gtk_widget_add_css_class(c.swatch[i], cls);
        gtk_widget_set_tooltip_text(c.swatch[i],
            "Show only the selected similarity bands and bookmark their lines. Click again to deselect. Several bands can be selected.");
        char alabel[64];
        std::snprintf(alabel, sizeof alabel, "Similarity band %d of 6", i + 1);
        gtk_accessible_update_property(GTK_ACCESSIBLE(c.swatch[i]),
                                       GTK_ACCESSIBLE_PROPERTY_LABEL, alabel, -1);
        g_object_set_data(G_OBJECT(c.swatch[i]), "band", GINT_TO_POINTER(i));
        g_signal_connect(c.swatch[i], "toggled", G_CALLBACK(on_swatch), nullptr);
        gtk_box_append(GTK_BOX(swatches), c.swatch[i]);
    }
    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        c.legend_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(c.legend_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(c.legend_css);
        c.refresh_legend();
    }

    c.spinner = gtk_spinner_new();
    c.status = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(c.status), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(c.status), 0.f);
    gtk_widget_set_hexpand(c.status, TRUE);

    gtk_box_append(GTK_BOX(row_legend), c.sens_dd);
    gtk_box_append(GTK_BOX(row_legend), swatches);
    gtk_box_append(GTK_BOX(row_legend), c.spinner);
    gtk_box_append(GTK_BOX(outer), c.status);
    g_signal_connect(outer, "unmap", G_CALLBACK(on_panel_unmap), nullptr);

    guint sens_index = static_cast<guint>(c.sensitivity + 1);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(c.sens_dd), sens_index);
    guint model_index = 0;
    for (guint i = 0; i < c.model_ids.size(); ++i)
        if (c.model_ids[i] == c.model_id) model_index = i;
    if (!c.model_ids.empty())
        gtk_drop_down_set_selected(GTK_DROP_DOWN(c.model_dd), model_index);
    guint ep_index = 0;
    bool ep_ok = false;
    for (guint i = 0; i < c.ep_ids.size(); ++i) {
        if (c.ep_ids[i] == c.ep_id) {
            ep_index = i;
            ep_ok = true;
        }
    }
    if (!ep_ok) c.ep_id = c.ep_ids.empty() ? "cpu" : c.ep_ids[0];
    gtk_drop_down_set_selected(GTK_DROP_DOWN(c.ep_dd), ep_index);

    g_signal_connect(c.sens_dd, "notify::selected", G_CALLBACK(on_sensitivity), nullptr);
    g_signal_connect(c.model_dd, "notify::selected", G_CALLBACK(on_model), nullptr);
    g_signal_connect(c.ep_dd, "notify::selected", G_CALLBACK(on_ep), nullptr);
    c.ui_ready = true;
    c.indicator = editor_allocate_indicator();
    if (c.indicator < 0 && c.index_status.empty())
        c.index_status = "Semantic search could not allocate an indicator";
    if (!c.index_status.empty()) c.set_status(c.index_status, false);
    c.panel = outer;
    c.panel_handle = editor_register_panel(outer);
}

void semantic_search_toggle(void)
{
    if (idle_gate().dead.load()) return;
    Controller &c = ctrl();
    ensure_panel();
    if (c.open) {
        c.close_bar();
        return;
    }
    c.open = true;
    editor_show_panel(c.panel_handle);
    editor_set_menu_check(true);
    if (c.entry) gtk_widget_grab_focus(c.entry);
    c.attach(static_cast<GtkWidget *>(editor_current_sci()));
}

void semantic_search_note_modified(GtkWidget *sci)
{
    if (idle_gate().dead.load()) return;
    Controller &c = ctrl();
    if (!c.open || !sci) return;
    std::uintptr_t ptr = c.doc_pointer(sci);
    if (!c.track.accepts_modification(ptr)) return;
    c.configure_indicator();
    c.index_ready = false;
    c.build_gen.fetch_add(1);
    c.clear_heatmap();
    c.set_status("Indexing...", true);
    c.schedule_rebuild();
}

void semantic_search_note_activated(GtkWidget *sci)
{
    if (idle_gate().dead.load()) return;
    Controller &c = ctrl();
    if (!c.open) return;
    c.attach(sci);
}

void semantic_search_note_closed(void *buffer_id)
{
    if (!buffer_id || idle_gate().dead.load()) return;
    Controller &c = ctrl();
    std::vector<void *> views;
    editor_collect_views(views);
    GtkWidget *closing_sci = nullptr;
    std::vector<DocView> open;
    std::vector<GtkWidget *> open_sci;
    for (void *view : views) {
        auto *w = static_cast<GtkWidget *>(view);
        void *buf = sci_buffer_id(w);
        if (!buf) continue;
        if (buf == buffer_id) closing_sci = w;
        DocView item;
        item.view = reinterpret_cast<std::uintptr_t>(w);
        item.doc = c.doc_pointer(w);
        open.push_back(item);
        open_sci.push_back(w);
    }
    if (!closing_sci) return;
    std::uintptr_t closed_view = reinterpret_cast<std::uintptr_t>(closing_sci);
    std::uintptr_t closed_doc = c.doc_pointer(closing_sci);
    DocCloseResult result = c.track.classify_close(closed_view, closed_doc, open);
    if (result.action == DocClose::Unrelated || result.action == DocClose::Keep)
        return;
    if (result.action == DocClose::Retarget) {
        c.track.apply_close(result);
        for (GtkWidget *w : open_sci) {
            if (reinterpret_cast<std::uintptr_t>(w) != result.view) continue;
            c.sci = w;
            c.buffer = sci_buffer_id(w);
            c.configure_indicator();
            break;
        }
        return;
    }
    c.build_gen.fetch_add(1);
    c.query_gen.fetch_add(1);
    bool another_tab = false;
    for (GtkWidget *w : open_sci)
        if (sci_buffer_id(w) != buffer_id) another_tab = true;
    if (!another_tab) c.downloads.cancel();
    if (!c.sci) c.sci = closing_sci;
    c.clear_heatmap();
    c.track.apply_close(result);
    c.sci = nullptr;
    c.buffer = nullptr;
    c.index_ready = false;
    c.spans.clear();
}

void semantic_search_shutdown(void)
{
    ctrl().shutdown();
}

void semantic_search_restyle(void)
{
    if (idle_gate().dead.load()) return;
    ctrl().schedule_restyle();
}

void semantic_search_smoke_start(void)
{
    const char *flag = std::getenv("NPP_SEMANTIC_SMOKE");
    if (!flag || !*flag) return;
    g_timeout_add(1200, [](gpointer) -> gboolean {
        if (idle_gate().dead.load()) return G_SOURCE_REMOVE;
        const char *now = std::getenv("NPP_SEMANTIC_SMOKE");
        std::string query = (now && std::strcmp(now, "1") != 0) ? now : "cats and kittens";
        Controller &c = ctrl();
        if (!c.open) semantic_search_toggle();
        if (c.entry) gtk_editable_set_text(GTK_EDITABLE(c.entry), query.c_str());
        return G_SOURCE_REMOVE;
    }, nullptr);
}
