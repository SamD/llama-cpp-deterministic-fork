// src/llama-deterministic-draft.cpp -- Deterministic draft plugin loader
//
// Built unconditionally into libllama so that the llama_deterministic_draft_*
// API declared in include/llama.h is always available.
//
// The same source is also used by external/CMakeLists.txt to build
// libdeterministic_draft_spec.so (a standalone distributable for plugin
// authors who don't want to link against full libllama), gated by
// DETERMINISTIC_SPEC_ENABLED.

#include "deterministic_draft_plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef SPEC_API
#define SPEC_API
#endif

#ifdef _WIN32
#    include <windows.h>
#    define DL_HANDLE            HMODULE
#    define DL_OPEN(path)        LoadLibraryA(path)
#    define DL_SYM(handle, name) GetProcAddress(handle, name)
#    define DL_CLOSE(handle)     FreeLibrary(handle)
#else
#    include <dlfcn.h>
#    define DL_HANDLE            void *
#    define DL_OPEN(path)        dlopen(path, RTLD_LAZY)
#    define DL_SYM(handle, name) dlsym(handle, name)
#    define DL_CLOSE(handle)     dlclose(handle)
#endif

// ─── Loader struct ─────────────────────────────────────────────────
// This struct is the canonical definition of the loader's internal state.
// All external code accesses the plugin through the C API declared in
// llama.h (opaque pointer); this file is the only place where the struct
// layout is known.

struct llama_deterministic_draft {
    DL_HANDLE plugin_handle;

    DeterministicDraftPlugin * (*create)(void);
    void (*destroy)(DeterministicDraftPlugin *);
    void (*commit)(DeterministicDraftPlugin *, int, int32_t, const char *, int);
    bool (*rollback)(DeterministicDraftPlugin *, int, int);
    void (*reset)(DeterministicDraftPlugin *, int);
    const char * (*get_language)(DeterministicDraftPlugin *, int);
    const char * (*get_version)(DeterministicDraftPlugin *);
    bool (*set_language)(DeterministicDraftPlugin *, int, const char *);

    // v3.0.0 capabilities API (optional - NULL for plugins that don't implement it)
    uint32_t (*get_capabilities)(DeterministicDraftPlugin *);
    bool (*set_vocab)(DeterministicDraftPlugin *, const char **, int, const int32_t *, int);
    bool (*set_grammar)(DeterministicDraftPlugin *, const char *, const char *);
    bool (*fill_bitmask)(DeterministicDraftPlugin *, int, uint32_t *, int);
    const char * (*get_jump_forward)(DeterministicDraftPlugin *, int, int *);

    // High-level filter helpers (optional - NULL for plugins that don't implement them)
    int (*filter_draft)(DeterministicDraftPlugin *, int, const int32_t *, int);
    bool (*apply_bitmask)(DeterministicDraftPlugin *, int, uint32_t *, int, float *);
    void (*commit_tokens)(DeterministicDraftPlugin *, int, const int32_t *, int);

    DeterministicDraftPlugin * state;

    bool load(const std::string & path);
    void unload();
};

// ─── Plugin Loader ─────────────────────────────────────────────────

bool llama_deterministic_draft::load(const std::string & path) {
    plugin_handle = DL_OPEN(path.c_str());
    if (!plugin_handle) {
        fprintf(stderr, "[deterministic-draft] Failed to load plugin: %s\n", path.c_str());
        return false;
    }

#define LOAD_SYM(name)                                                                                    \
    name = (decltype(name)) DL_SYM(plugin_handle, "deterministic_draft_" #name);                          \
    if (!name) {                                                                                          \
        fprintf(stderr, "[deterministic-draft] Symbol 'deterministic_draft_%s' not found in %s\n", #name, \
                path.c_str());                                                                            \
        DL_CLOSE(plugin_handle);                                                                          \
        return false;                                                                                     \
    }

    LOAD_SYM(create);
    LOAD_SYM(destroy);
    LOAD_SYM(commit);
    LOAD_SYM(reset);
    LOAD_SYM(get_language);
    LOAD_SYM(get_version);

    set_language = (decltype(set_language)) DL_SYM(plugin_handle, "deterministic_draft_set_language");
    rollback     = (decltype(rollback)) DL_SYM(plugin_handle, "deterministic_draft_rollback");

    // v3.0.0 capabilities API - optional, graceful fallback for v2.0.0 plugins
    get_capabilities = (decltype(get_capabilities)) DL_SYM(plugin_handle, "deterministic_draft_get_capabilities");
    set_vocab        = (decltype(set_vocab))        DL_SYM(plugin_handle, "deterministic_draft_set_vocab");
    set_grammar      = (decltype(set_grammar))      DL_SYM(plugin_handle, "deterministic_draft_set_grammar");
    fill_bitmask     = (decltype(fill_bitmask))     DL_SYM(plugin_handle, "deterministic_draft_fill_bitmask");
    get_jump_forward = (decltype(get_jump_forward)) DL_SYM(plugin_handle, "deterministic_draft_get_jump_forward");

    // High-level filter helpers - optional, graceful fallback
    filter_draft  = (decltype(filter_draft))  DL_SYM(plugin_handle, "deterministic_draft_filter_draft");
    apply_bitmask = (decltype(apply_bitmask)) DL_SYM(plugin_handle, "deterministic_draft_apply_bitmask");
    commit_tokens = (decltype(commit_tokens)) DL_SYM(plugin_handle, "deterministic_draft_commit_tokens");

    state = create();
    if (!state) {
        fprintf(stderr, "[deterministic-draft] Plugin create() returned NULL\n");
        DL_CLOSE(plugin_handle);
        return false;
    }

    return true;
}

void llama_deterministic_draft::unload() {
    if (state && destroy) {
        destroy(state);
        state = nullptr;
    }
    if (plugin_handle) {
        DL_CLOSE(plugin_handle);
        plugin_handle = nullptr;
    }
}

// ─── C API ─────────────────────────────────────────────────────────
// extern "C" to match the declarations in llama.h (which wraps everything
// in extern "C"). The struct itself has C++ members (std::string) but the
// C API functions use C linkage for ABI compatibility.

extern "C" {

SPEC_API struct llama_deterministic_draft * llama_deterministic_draft_init(const char * plugin_path) {
    auto * draft = new llama_deterministic_draft{};
    if (plugin_path && !draft->load(plugin_path)) {
        draft->unload();
        delete draft;
        return nullptr;
    }
    return draft;
}

SPEC_API void llama_deterministic_draft_free(struct llama_deterministic_draft * draft) {
    if (!draft) {
        return;
    }
    draft->unload();
    delete draft;
}

SPEC_API void llama_deterministic_draft_commit(struct llama_deterministic_draft * draft,
                                                int                                slot_id,
                                                int32_t                            token_id,
                                                const char *                       token_text,
                                                int                                token_length) {
    if (!draft || !draft->commit || !draft->state) {
        return;
    }
    draft->commit(draft->state, slot_id, token_id, token_text, token_length);
}

SPEC_API bool llama_deterministic_draft_rollback(struct llama_deterministic_draft * draft,
                                                  int                                slot_id,
                                                  int                                n_tokens) {
    if (!draft || !draft->rollback || !draft->state) {
        return false;
    }
    return draft->rollback(draft->state, slot_id, n_tokens);
}

SPEC_API void llama_deterministic_draft_reset(struct llama_deterministic_draft * draft, int slot_id) {
    if (!draft || !draft->reset || !draft->state) {
        return;
    }
    draft->reset(draft->state, slot_id);
}

SPEC_API bool llama_deterministic_draft_set_language(struct llama_deterministic_draft * draft,
                                                     int                                slot_id,
                                                     const char *                       lang) {
    if (!draft || !draft->set_language || !draft->state) {
        return false;
    }
    return draft->set_language(draft->state, slot_id, lang);
}

SPEC_API const char * llama_deterministic_draft_get_language(struct llama_deterministic_draft * draft, int slot_id) {
    if (!draft || !draft->get_language || !draft->state) {
        return nullptr;
    }
    return draft->get_language(draft->state, slot_id);
}

SPEC_API const char * llama_deterministic_draft_get_version(struct llama_deterministic_draft * draft) {
    if (!draft || !draft->get_version || !draft->state) {
        return "unknown";
    }
    return draft->get_version(draft->state);
}

SPEC_API uint32_t llama_deterministic_draft_get_capabilities(struct llama_deterministic_draft * draft) {
    if (!draft || !draft->state) {
        return 0;
    }
    if (!draft->get_capabilities) {
        return 0;
    }
    return draft->get_capabilities(draft->state);
}

SPEC_API bool llama_deterministic_draft_set_vocab(struct llama_deterministic_draft * draft,
                                                    const char **                     vocab_entries,
                                                    int                               vocab_size,
                                                    const int32_t *                   stop_tokens,
                                                    int                               n_stop) {
    if (!draft || !draft->state) {
        return false;
    }
    if (!draft->set_vocab) {
        return true;
    }
    return draft->set_vocab(draft->state, vocab_entries, vocab_size, stop_tokens, n_stop);
}

SPEC_API bool llama_deterministic_draft_set_grammar(struct llama_deterministic_draft * draft,
                                                      const char *                       ebnf_str,
                                                      const char *                       root_rule) {
    if (!draft || !draft->state) {
        return false;
    }
    if (!draft->set_grammar) {
        return false;
    }
    return draft->set_grammar(draft->state, ebnf_str, root_rule);
}

SPEC_API bool llama_deterministic_draft_fill_bitmask(struct llama_deterministic_draft * draft,
                                                       int                                slot_id,
                                                       uint32_t *                         bitmask,
                                                       int                                vocab_size) {
    if (!draft || !draft->state || !bitmask || vocab_size <= 0) {
        return false;
    }
    if (!draft->fill_bitmask) {
        return false;
    }
    return draft->fill_bitmask(draft->state, slot_id, bitmask, vocab_size);
}

SPEC_API const char * llama_deterministic_draft_get_jump_forward(struct llama_deterministic_draft * draft,
                                                                   int                                slot_id,
                                                                   int *                              out_length) {
    if (!draft || !draft->state) {
        if (out_length) {
            *out_length = 0;
        }
        return nullptr;
    }
    if (!draft->get_jump_forward) {
        if (out_length) {
            *out_length = 0;
        }
        return nullptr;
    }
    return draft->get_jump_forward(draft->state, slot_id, out_length);
}

SPEC_API int llama_deterministic_draft_filter_draft(struct llama_deterministic_draft * draft,
                                                      int                                slot_id,
                                                      const int32_t *                    tokens,
                                                      int                                n_tokens) {
    if (!draft || !draft->filter_draft || !draft->state) {
        return 0;
    }
    return draft->filter_draft(draft->state, slot_id, tokens, n_tokens);
}

SPEC_API bool llama_deterministic_draft_apply_bitmask(struct llama_deterministic_draft * draft,
                                                        int                                slot_id,
                                                        uint32_t *                         bitmask,
                                                        int                                vocab_size,
                                                        float *                            logits) {
    if (!draft || !draft->apply_bitmask || !draft->state) {
        return false;
    }
    return draft->apply_bitmask(draft->state, slot_id, bitmask, vocab_size, logits);
}

SPEC_API void llama_deterministic_draft_commit_tokens(struct llama_deterministic_draft * draft,
                                                        int                                slot_id,
                                                        const int32_t *                    tokens,
                                                        int                                n_tokens) {
    if (!draft || !draft->commit_tokens || !draft->state) {
        return;
    }
    draft->commit_tokens(draft->state, slot_id, tokens, n_tokens);
}

}  // extern "C"
