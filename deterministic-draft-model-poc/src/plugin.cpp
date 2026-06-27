// plugins/deterministic-draft/plugin.cpp -- XGrammar-based deterministic draft plugin
//
// =============================================================================
// Overview
// =============================================================================
// Implements the deterministic_draft_plugin.h v3 C API using XGrammar for
// grammar-constrained decoding with jump-forward support:
//   - CAPABILITY_BITMASK: Pre-generation token constraint
//   - CAPABILITY_JUMP_FORWARD: Deterministic token skipping
//
// =============================================================================
// Architecture
// =============================================================================
//   deterministic-draft.so
//   ├── GrammarCompiler (shared, one per vocab)
//   ├── SlotState (per-inference-slot)
//   │   ├── GrammarMatcher (stateful parser)
//   │   └── jump_forward_cache (last computed string)
//   └── TokenizerInfo (shared vocabulary)
//
// =============================================================================
// Usage Flow
// =============================================================================
// 1. Host calls create() -> plugin instance
// 2. Host calls set_vocab() with tokenizer vocabulary
// 3. Host calls set_grammar() or set_language() to load a grammar
// 4. For each generation step:
//    a. Host calls fill_bitmask() -> constrained token IDs
//    b. Host samples from constrained distribution
//    c. Host calls commit() with sampled token
//    d. Host calls get_jump_forward() -> deterministic string (if any)
//    e. Host emits jump-forward tokens, calls commit() for each
// 5. On reset: reset() clears matcher state
// 6. On shutdown: destroy() frees resources
// =============================================================================

#include "deterministic_draft_plugin.h"

#include <xgrammar/xgrammar.h>
#include <dlpack/dlpack.h>

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Environment variable names used for plugin configuration.
// These are set by the host process before loading the plugin.
#define DETERMINISTIC_DRAFT_ENV_LANGUAGE  "DETERMINISTIC_DRAFT_LANGUAGE"
#define DETERMINISTIC_DRAFT_ENV_GRAMMAR_DIR "DETERMINISTIC_DRAFT_GRAMMAR_DIR"

// XGrammar compiler configuration
#define XGRAMMAR_COMPILER_MAX_THREADS 8

// ══════════════════════════════════════════════════════════════════════
// Per-slot state
// ══════════════════════════════════════════════════════════════════════

struct SlotState {
    std::unique_ptr<xgrammar::GrammarMatcher> matcher;
    std::string jump_forward_cache;
    bool terminated = false;
    std::vector<uint32_t> bitmask;  // reusable bitmask buffer

    void reset_matcher() {
        if (matcher) {
            matcher->Reset();
        }
        jump_forward_cache.clear();
        terminated = false;
        bitmask.clear();
    }
};

// ══════════════════════════════════════════════════════════════════════
// Plugin state
// ══════════════════════════════════════════════════════════════════════

struct XGrammarPlugin {
    // Shared components (initialized once)
    std::unique_ptr<xgrammar::TokenizerInfo> tokenizer_info;
    std::unique_ptr<xgrammar::GrammarCompiler> compiler;
    std::unique_ptr<xgrammar::CompiledGrammar> compiled_grammar;

    // Per-slot state
    std::unordered_map<int, SlotState> slots;
    std::mutex slots_mutex;

    // Configuration
    int vocab_size = 0;
    std::vector<std::string> vocab_strings;
    std::vector<int32_t> stop_token_ids;

    // Currently active language (set via deterministic_draft_set_language),
    // or empty if a raw grammar was set via deterministic_draft_set_grammar.
    std::string current_language;

    // Compiled-grammar cache keyed by cache key (language name, or raw EBNF
    // text for set_grammar callers) - avoids recompiling when switching back
    // to a previously-seen grammar within the same process.
    std::unordered_map<std::string, std::shared_ptr<xgrammar::CompiledGrammar>> grammar_cache;

    // Capabilities
    uint32_t capabilities = DETERMINISTIC_DRAFT_CAPABILITY_BITMASK |
                            DETERMINISTIC_DRAFT_CAPABILITY_JUMP_FORWARD;

    // Get or create slot
    SlotState& get_slot(int slot_id) {
        int key = (slot_id == DETERMINISTIC_DRAFT_SLOT_DEFAULT) ? 0 : slot_id;
        std::lock_guard<std::mutex> lock(slots_mutex);
        auto it = slots.find(key);
        if (it != slots.end()) {
            return it->second;
        }

        SlotState& slot = slots[key];
        if (compiled_grammar) {
            slot.matcher = std::make_unique<xgrammar::GrammarMatcher>(
                *compiled_grammar,
                stop_token_ids.empty() ? std::nullopt : std::optional<std::vector<int32_t>>(stop_token_ids));
        }
        return slot;
    }

    bool has_slot(int slot_id) const {
        int key = (slot_id == DETERMINISTIC_DRAFT_SLOT_DEFAULT) ? 0 : slot_id;
        return slots.find(key) != slots.end();
    }

    // Reinitialize all slot matchers (called after grammar changes)
    void reinit_matchers() {
        std::lock_guard<std::mutex> lock(slots_mutex);
        for (auto& [id, slot] : slots) {
            if (compiled_grammar) {
                slot.matcher = std::make_unique<xgrammar::GrammarMatcher>(
                    *compiled_grammar,
                    stop_token_ids.empty() ? std::nullopt : std::optional<std::vector<int32_t>>(stop_token_ids));
                slot.terminated = false;
            }
        }
    }
};

// ══════════════════════════════════════════════════════════════════════
// C API Implementation
// ══════════════════════════════════════════════════════════════════════

extern "C" {

// ── Lifecycle ────────────────────────────────────────────────────────

DeterministicDraftPlugin* deterministic_draft_create(void) {
    auto* plugin = new (std::nothrow) XGrammarPlugin();
    if (!plugin) {
        fprintf(stderr, "[xgrammar-draft] ERROR: failed to allocate plugin\n");
        return nullptr;
    }
    return reinterpret_cast<DeterministicDraftPlugin*>(plugin);
}

void deterministic_draft_destroy(DeterministicDraftPlugin* state) {
    delete reinterpret_cast<XGrammarPlugin*>(state);
}

// ── Capabilities ─────────────────────────────────────────────────────

uint32_t deterministic_draft_get_capabilities(DeterministicDraftPlugin* state) {
    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin) {
        return 0;
    }
    return plugin->capabilities;
}

// ── Vocabulary setup ─────────────────────────────────────────────────

bool deterministic_draft_set_vocab(
        DeterministicDraftPlugin* state,
        const char** vocab_entries,
        int vocab_size,
        const int32_t* stop_tokens,
        int n_stop) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin || !vocab_entries || vocab_size <= 0) {
        return false;
    }

    // Store vocabulary
    plugin->vocab_size = vocab_size;
    plugin->vocab_strings.clear();
    plugin->vocab_strings.reserve(vocab_size);
    for (int i = 0; i < vocab_size; i++) {
        plugin->vocab_strings.push_back(vocab_entries[i] ? vocab_entries[i] : "");
    }

    // Store stop tokens
    plugin->stop_token_ids.clear();
    if (stop_tokens && n_stop > 0) {
        plugin->stop_token_ids.assign(stop_tokens, stop_tokens + n_stop);
    }

    // Create TokenizerInfo
    // XGrammar expects the encoded vocabulary (raw token bytes)
    try {
        plugin->tokenizer_info = std::make_unique<xgrammar::TokenizerInfo>(
            plugin->vocab_strings,
            xgrammar::VocabType::BYTE_LEVEL,
            vocab_size,
            plugin->stop_token_ids.empty() ? std::nullopt : std::optional<std::vector<int32_t>>(plugin->stop_token_ids),
            false  // add_prefix_space
        );

        // Create compiler
        plugin->compiler = std::make_unique<xgrammar::GrammarCompiler>(
            *plugin->tokenizer_info,
            XGRAMMAR_COMPILER_MAX_THREADS,
            true   // cache_enabled
        );

        // Auto-load grammar from DETERMINISTIC_DRAFT_LANGUAGE env var if set
        const char* lang_env = std::getenv(DETERMINISTIC_DRAFT_ENV_LANGUAGE);
        if (lang_env && lang_env[0] != '\0') {
            fprintf(stderr, "[xgrammar-draft] Auto-loading grammar for language '%s' from env var\n", lang_env);
            if (!deterministic_draft_set_language(state, DETERMINISTIC_DRAFT_SLOT_DEFAULT, lang_env)) {
                fprintf(stderr, "[xgrammar-draft] WARNING: failed to load grammar for language '%s'\n", lang_env);
            }
        }

        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[xgrammar-draft] ERROR: failed to create tokenizer info: %s\n", e.what());
        return false;
    }
}

// ── Grammar configuration ────────────────────────────────────────────

// Compile (or reuse from cache) an EBNF grammar and apply it as the plugin's
// active grammar, reinitializing all slot matchers. `cache_key` identifies
// this grammar for reuse (e.g. a language name, or the raw EBNF text itself).
static bool compile_and_apply_grammar(
        XGrammarPlugin* plugin,
        const std::string& ebnf_str,
        const std::string& root_rule,
        const std::string& cache_key) {

    if (!plugin->compiler) {
        fprintf(stderr, "[xgrammar-draft] ERROR: vocabulary not set, call set_vocab first\n");
        return false;
    }

    try {
        auto it = plugin->grammar_cache.find(cache_key);
        if (it != plugin->grammar_cache.end()) {
            plugin->compiled_grammar = std::make_unique<xgrammar::CompiledGrammar>(*it->second);
            fprintf(stderr, "[xgrammar-draft] Reusing cached grammar for '%s'\n", cache_key.c_str());
        } else {
            fprintf(stderr, "[xgrammar-draft] Compiling grammar '%s'...\n", cache_key.c_str());
            auto grammar = xgrammar::Grammar::FromEBNF(ebnf_str, root_rule);
            auto compiled = std::make_shared<xgrammar::CompiledGrammar>(
                plugin->compiler->CompileGrammar(grammar));
            plugin->grammar_cache[cache_key] = compiled;
            plugin->compiled_grammar = std::make_unique<xgrammar::CompiledGrammar>(*compiled);
            fprintf(stderr, "[xgrammar-draft] Grammar compiled and cached for '%s'\n", cache_key.c_str());
        }

        // Reinitialize all slot matchers with new grammar
        plugin->reinit_matchers();

        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[xgrammar-draft] ERROR: failed to compile grammar '%s': %s\n",
                cache_key.c_str(), e.what());
        return false;
    }
}

bool deterministic_draft_set_grammar(
        DeterministicDraftPlugin* state,
        const char* ebnf_str,
        const char* root_rule) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin || !ebnf_str) {
        return false;
    }

    std::string root = root_rule ? root_rule : "root";
    bool ok = compile_and_apply_grammar(plugin, ebnf_str, root, ebnf_str);
    if (ok) {
        // raw grammar set directly by the host - not a named bundled language
        plugin->current_language.clear();
    }
    return ok;
}

// ── Bitmask (CAPABILITY_BITMASK) ─────────────────────────────────────

bool deterministic_draft_fill_bitmask(
        DeterministicDraftPlugin* state,
        int slot_id,
        uint32_t* bitmask,
        int vocab_size) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin || !bitmask || vocab_size <= 0) {
        return false;
    }

    SlotState& slot = plugin->get_slot(slot_id);
    if (!slot.matcher || slot.terminated) {
        return false;
    }

    try {
        // Allocate DLTensor for bitmask
        // XGrammar expects shape (GetBitmaskSize(),) with dtype int32
        int bitmask_size = xgrammar::GetBitmaskSize(vocab_size);
        std::vector<int32_t> bitmask_data(bitmask_size, 0);

        DLTensor bitmask_tensor;
        bitmask_tensor.data = bitmask_data.data();
        bitmask_tensor.device = DLDevice{kDLCPU, 0};
        bitmask_tensor.ndim = 1;
        bitmask_tensor.dtype = xgrammar::GetBitmaskDLType();
        bitmask_tensor.shape = new int64_t[1]{bitmask_size};
        bitmask_tensor.strides = nullptr;
        bitmask_tensor.byte_offset = 0;

        // Fill bitmask
        slot.matcher->FillNextTokenBitmask(&bitmask_tensor);

        delete[] bitmask_tensor.shape;

        // Copy to output (convert int32 to uint32)
        for (int i = 0; i < bitmask_size && i < (vocab_size + 31) / 32; i++) {
            bitmask[i] = static_cast<uint32_t>(bitmask_data[i]);
        }

        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[xgrammar-draft] ERROR: fill_bitmask failed: %s\n", e.what());
        return false;
    }
}

// ── Jump-forward (CAPABILITY_JUMP_FORWARD) ───────────────────────────

const char* deterministic_draft_get_jump_forward(
        DeterministicDraftPlugin* state,
        int slot_id,
        int* out_length) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin) {
        if (out_length) *out_length = 0;
        return nullptr;
    }

    if (!plugin->has_slot(slot_id)) {
        if (out_length) *out_length = 0;
        return nullptr;
    }

    SlotState& slot = plugin->get_slot(slot_id);
    if (!slot.matcher || slot.terminated) {
        if (out_length) *out_length = 0;
        return nullptr;
    }

    try {
        slot.jump_forward_cache = slot.matcher->FindJumpForwardString();

        if (slot.jump_forward_cache.empty()) {
            if (out_length) *out_length = 0;
            return nullptr;
        }

        if (out_length) {
            *out_length = static_cast<int>(slot.jump_forward_cache.size());
        }
        return slot.jump_forward_cache.c_str();
    } catch (const std::exception& e) {
        fprintf(stderr, "[xgrammar-draft] ERROR: get_jump_forward failed: %s\n", e.what());
        if (out_length) *out_length = 0;
        return nullptr;
    }
}

// ── Commit ───────────────────────────────────────────────────────────

void deterministic_draft_commit(
        DeterministicDraftPlugin* state,
        int slot_id,
        int32_t token_id,
        const char* token_text,
        int token_length) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin) return;

    SlotState& slot = plugin->get_slot(slot_id);
    if (!slot.matcher) return;

    std::string token(token_text, static_cast<size_t>(token_length));

    try {
        bool accepted = slot.matcher->AcceptToken(token_id);
        if (!accepted) {
            fprintf(stderr, "[xgrammar-draft] WARNING: AcceptToken rejected token_id=%d '%s'\n",
                    token_id, token.c_str());
        }

        if (slot.matcher->IsTerminated()) {
            slot.terminated = true;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[xgrammar-draft] ERROR: commit failed: %s\n", e.what());
    }
}

bool deterministic_draft_rollback(
        DeterministicDraftPlugin* state,
        int slot_id,
        int n_tokens) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin || n_tokens <= 0) return n_tokens == 0;

    SlotState& slot = plugin->get_slot(slot_id);
    if (!slot.matcher) return false;

    try {
        slot.matcher->Rollback(n_tokens);
        slot.terminated = false;
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[xgrammar-draft] ERROR: rollback failed: %s\n", e.what());
        return false;
    }
}

// ── High-level filter helpers (CAPABILITY_BITMASK) ──────────────────

static void bitmask_apply_to_logits(
        const uint32_t* bitmask,
        int vocab_size,
        float* logits) {
    const int bitmask_words = (vocab_size + 31) / 32;
    for (int w = 0; w < bitmask_words; w++) {
        if (bitmask[w] == 0xFFFFFFFFu) {
            continue;
        }
        for (int b = 0; b < 32; b++) {
            const int i = w * 32 + b;
            if (i >= vocab_size) {
                break;
            }
            if (!(bitmask[w] & (1u << b))) {
                logits[i] = -1e30f;
            }
        }
    }
}

int deterministic_draft_filter_draft(
        DeterministicDraftPlugin* state,
        int slot_id,
        const int32_t* tokens,
        int n_tokens) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin || !tokens || n_tokens <= 0) {
        return 0;
    }

    SlotState& slot = plugin->get_slot(slot_id);
    if (!slot.matcher || slot.terminated) {
        return 0;
    }

    const int vocab_size = plugin->vocab_size;
    const int bitmask_words = (vocab_size + 31) / 32;
    slot.bitmask.assign(bitmask_words, 0xFFFFFFFFu);

    int n_accepted = 0;
    for (int i = 0; i < n_tokens; i++) {
        const int32_t token_id = tokens[i];

        // Fill bitmask for current grammar state
        bool has_bitmask = false;
        if (!slot.bitmask.empty()) {
            try {
                int bitmask_size = xgrammar::GetBitmaskSize(vocab_size);
                std::vector<int32_t> bitmask_data(bitmask_size, 0);

                DLTensor bitmask_tensor;
                bitmask_tensor.data = bitmask_data.data();
                bitmask_tensor.device = DLDevice{kDLCPU, 0};
                bitmask_tensor.ndim = 1;
                bitmask_tensor.dtype = xgrammar::GetBitmaskDLType();
                bitmask_tensor.shape = new int64_t[1]{bitmask_size};
                bitmask_tensor.strides = nullptr;
                bitmask_tensor.byte_offset = 0;

                slot.matcher->FillNextTokenBitmask(&bitmask_tensor);
                delete[] bitmask_tensor.shape;

                // Convert to uint32 bitmask
                for (int j = 0; j < bitmask_size && j < bitmask_words; j++) {
                    slot.bitmask[j] = static_cast<uint32_t>(bitmask_data[j]);
                }
                has_bitmask = true;
            } catch (const std::exception& e) {
                fprintf(stderr, "[xgrammar-draft] ERROR: fill_bitmask failed: %s\n", e.what());
            }
        }

        // Check if token is valid
        bool token_valid = true;
        if (has_bitmask) {
            const size_t word_idx = (size_t) token_id / 32;
            const int    bit_idx  = token_id % 32;
            token_valid = (slot.bitmask[word_idx] & (1u << bit_idx)) != 0;
        }

        if (!token_valid) {
            break;
        }

        // Commit valid token
        const std::string& piece = (token_id >= 0 && token_id < (int) plugin->vocab_strings.size())
            ? plugin->vocab_strings[token_id]
            : std::string();
        try {
            slot.matcher->AcceptToken(token_id);
            if (slot.matcher->IsTerminated()) {
                slot.terminated = true;
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "[xgrammar-draft] ERROR: commit failed: %s\n", e.what());
            break;
        }

        n_accepted++;
    }

    return n_accepted;
}

bool deterministic_draft_apply_bitmask(
        DeterministicDraftPlugin* state,
        int slot_id,
        uint32_t* bitmask,
        int vocab_size,
        float* logits) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin || !bitmask || vocab_size <= 0 || !logits) {
        return false;
    }

    SlotState& slot = plugin->get_slot(slot_id);
    if (!slot.matcher || slot.terminated) {
        return false;
    }

    try {
        int bitmask_size = xgrammar::GetBitmaskSize(vocab_size);
        std::vector<int32_t> bitmask_data(bitmask_size, 0);

        DLTensor bitmask_tensor;
        bitmask_tensor.data = bitmask_data.data();
        bitmask_tensor.device = DLDevice{kDLCPU, 0};
        bitmask_tensor.ndim = 1;
        bitmask_tensor.dtype = xgrammar::GetBitmaskDLType();
        bitmask_tensor.shape = new int64_t[1]{bitmask_size};
        bitmask_tensor.strides = nullptr;
        bitmask_tensor.byte_offset = 0;

        slot.matcher->FillNextTokenBitmask(&bitmask_tensor);
        delete[] bitmask_tensor.shape;

        // Convert to uint32 bitmask
        const int bitmask_words = (vocab_size + 31) / 32;
        for (int i = 0; i < bitmask_size && i < bitmask_words; i++) {
            bitmask[i] = static_cast<uint32_t>(bitmask_data[i]);
        }

        // Apply to logits
        bitmask_apply_to_logits(bitmask, vocab_size, logits);

        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[xgrammar-draft] ERROR: apply_bitmask failed: %s\n", e.what());
        return false;
    }
}

void deterministic_draft_commit_tokens(
        DeterministicDraftPlugin* state,
        int slot_id,
        const int32_t* tokens,
        int n_tokens) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin || !tokens || n_tokens <= 0) {
        return;
    }

    SlotState& slot = plugin->get_slot(slot_id);
    if (!slot.matcher) {
        return;
    }

    for (int i = 0; i < n_tokens; i++) {
        const int32_t token_id = tokens[i];
        try {
            slot.matcher->AcceptToken(token_id);
            if (slot.matcher->IsTerminated()) {
                slot.terminated = true;
                break;
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "[xgrammar-draft] ERROR: commit_tokens failed: %s\n", e.what());
            break;
        }
    }
}

// ── State access ─────────────────────────────────────────────────────

void deterministic_draft_reset(
        DeterministicDraftPlugin* state,
        int slot_id) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin) return;

    if (!plugin->has_slot(slot_id)) return;

    SlotState& slot = plugin->get_slot(slot_id);
    slot.reset_matcher();
}

// ── Language control (bundled grammar loading) ────────────────────────

// Directory containing this plugin's own shared library file (.so/.dylib),
// or "." if it cannot be determined.
static std::string get_plugin_dir() {
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&deterministic_draft_create), &info) && info.dli_fname) {
        std::string path = info.dli_fname;
        size_t pos = path.find_last_of('/');
        if (pos != std::string::npos) {
            return path.substr(0, pos);
        }
    }
    return ".";
}

// Resolve the bundled grammar directory: DETERMINISTIC_DRAFT_GRAMMAR_DIR
// environment variable if set, otherwise "<plugin_dir>/grammars".
static std::string get_grammar_dir() {
    const char* env_dir = std::getenv(DETERMINISTIC_DRAFT_ENV_GRAMMAR_DIR);
    if (env_dir && env_dir[0] != '\0') {
        return env_dir;
    }
    return get_plugin_dir() + "/grammars";
}

static bool read_file(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) {
        fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(fsize));
    size_t n_read = fsize > 0 ? fread(&out[0], 1, static_cast<size_t>(fsize), f) : 0;
    fclose(f);
    return n_read == static_cast<size_t>(fsize);
}

bool deterministic_draft_set_language(
        DeterministicDraftPlugin* state,
        int /*slot_id*/,
        const char* lang) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin || !lang || lang[0] == '\0') {
        return false;
    }

    std::string language = lang;
    std::string grammar_path = get_grammar_dir() + "/" + language + ".gbnf";

    std::string ebnf_str;
    if (!read_file(grammar_path, ebnf_str)) {
        fprintf(stderr, "[xgrammar-draft] ERROR: no bundled grammar for language '%s' (looked in %s)\n",
                language.c_str(), grammar_path.c_str());
        return false;
    }

    bool ok = compile_and_apply_grammar(plugin, ebnf_str, "root", language);
    if (ok) {
        plugin->current_language = language;
    }
    return ok;
}

const char* deterministic_draft_get_language(
        DeterministicDraftPlugin* state,
        int /*slot_id*/) {

    auto* plugin = reinterpret_cast<XGrammarPlugin*>(state);
    if (!plugin || plugin->current_language.empty()) {
        return "unknown";
    }
    return plugin->current_language.c_str();
}

// ── Metadata ─────────────────────────────────────────────────────────

const char* deterministic_draft_get_version(
        DeterministicDraftPlugin* /*state*/) {
    return "3.0.0";
}

} // extern "C"
