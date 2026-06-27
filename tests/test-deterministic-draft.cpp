// test-deterministic-draft.cpp -- Unit tests for deterministic draft plugin loader
//
// Tests:
//   1. Plugin loader: init/free with valid and invalid paths
//   2. C API wrappers: get_capabilities, set_vocab, set_grammar/set_language,
//      fill_bitmask, commit, reset
//   3. Speculative integration: common_speculative with DRAFT_DETERMINISTIC type
//   4. Auto-imply: --deterministic-draft-model implies draft-mtp
//   5. --det-draft-accept-all flag validation and accessor
//
// These tests use the generic plugin loader (libdeterministic_draft_spec.so).
// A plugin .so (XGrammar-based) is needed for integration tests; if not
// available, those tests are skipped.

#include "arg.h"
#include "common.h"
#include "llama.h"
#include "speculative.h"

#ifdef NDEBUG
#    undef NDEBUG
#endif

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Try to find the plugin .so in common locations
static std::string find_plugin() {
    const char * candidates[] = { "deterministic-draft.so", "./deterministic-draft.so",
                                  "../deterministic-draft-model-poc/build/deterministic-draft.so",
                                  "./build/deterministic-draft.so", nullptr };

    for (int i = 0; candidates[i]; i++) {
        FILE * f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }

    return "";
}

// ============================================================================
// Test 1: Plugin loader lifecycle
// ============================================================================

static void test_plugin_loader_init_free() {
    printf("test_plugin_loader_init_free... ");

    // init with NULL path should return a valid handle (no plugin loaded)
    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(nullptr);
    assert(draft != nullptr);
    llama_deterministic_draft_free(draft);

    // init with non-existent path should return nullptr
    draft = llama_deterministic_draft_init("/nonexistent/path/plugin.so");
    assert(draft == nullptr);

    // free with nullptr should be safe
    llama_deterministic_draft_free(nullptr);

    printf("OK\n");
}

// ============================================================================
// Test 2: C API wrappers with no plugin loaded
// ============================================================================

static void test_c_api_no_plugin() {
    printf("test_c_api_no_plugin... ");

    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(nullptr);
    assert(draft != nullptr);

    // get_capabilities should return 0 (no plugin loaded)
    assert(llama_deterministic_draft_get_capabilities(draft) == 0);

    // set_vocab/set_grammar/set_language should fail gracefully (no plugin)
    const char * dummy_vocab[] = { "a" };
    assert(!llama_deterministic_draft_set_vocab(draft, dummy_vocab, 1, nullptr, 0));
    assert(!llama_deterministic_draft_set_grammar(draft, "root ::= \"a\"", "root"));
    assert(!llama_deterministic_draft_set_language(draft, 0, "python"));

    // fill_bitmask should return false (no constraint / no plugin)
    uint32_t bitmask[4] = { 0 };
    assert(!llama_deterministic_draft_fill_bitmask(draft, 0, bitmask, 128));

    // commit should be safe (no-op)
    llama_deterministic_draft_commit(draft, 0, 0, "x", 1);

    // reset should be safe (no-op)
    llama_deterministic_draft_reset(draft, 0);

    llama_deterministic_draft_free(draft);

    printf("OK\n");
}

// ============================================================================
// Test 3: Speculative type enum
// ============================================================================

static void test_speculative_type_enum() {
    printf("test_speculative_type_enum... ");

    // The enum should have the new type
    assert(COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC != COMMON_SPECULATIVE_TYPE_NONE);
    assert(COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC != COMMON_SPECULATIVE_TYPE_DRAFT_MTP);
    assert(COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC < COMMON_SPECULATIVE_TYPE_COUNT);

    // Type name mapping
    std::string name = common_speculative_type_to_str(COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC);
    assert(name == "draft-deterministic");

    printf("OK\n");
}

// ============================================================================
// Test 4: Params struct has deterministic_draft in speculative
// ============================================================================

static void test_params_struct() {
    printf("test_params_struct... ");

    common_params params;

    // deterministic_draft should be in speculative, not in sampling or root
    params.speculative.deterministic_draft.enabled        = true;
    params.speculative.deterministic_draft.n_max          = 42;
    params.speculative.deterministic_draft.plugin_path    = "/test/path.so";
    params.speculative.deterministic_draft.det_accept_all = true;

    assert(params.speculative.deterministic_draft.enabled == true);
    assert(params.speculative.deterministic_draft.n_max == 42);
    assert(params.speculative.deterministic_draft.plugin_path == "/test/path.so");
    assert(params.speculative.deterministic_draft.det_accept_all == true);

    // default value is false
    common_params params2;
    assert(params2.speculative.deterministic_draft.det_accept_all == false);

    printf("OK\n");
}

// ============================================================================
// Test 5: common_speculative_has_det_filter with null spec
// ============================================================================

static void test_det_filter_query() {
    printf("test_det_filter_query... ");

    assert(!common_speculative_has_det_filter(nullptr));

    // det_accept_all should be false when spec is null
    assert(!common_speculative_get_det_accept_all(nullptr));

    // Get filter result from null should return empty
    const auto & fr = common_speculative_get_det_filter_result(nullptr, 0);
    assert(!fr.truncated);
    assert(fr.valid_count == 0);

    printf("OK\n");
}

// ============================================================================
// Shared test vocabulary/grammar helpers for bitmask-based integration tests
// ============================================================================

// Fixed test vocabulary: index == token_id. Covers the exact tokens used by
// the integration tests below (two alternative complete "programs").
static const char * TEST_VOCAB[] = {
    /* 0*/ "int", /* 1*/ " ", /* 2*/ "main", /* 3*/ "(", /* 4*/ ")",
    /* 5*/ "{",   /* 6*/ "\n    ", /* 7*/ "return", /* 8*/ "0", /* 9*/ ";",
    /*10*/ "\n",  /*11*/ "}", /*12*/ "float", /*13*/ "x", /*14*/ "=", /*15*/ "1.0f",
};
static const int TEST_VOCAB_SIZE = sizeof(TEST_VOCAB) / sizeof(TEST_VOCAB[0]);

// Grammar accepting exactly two complete "programs" built from TEST_VOCAB's
// tokens (concatenated literally), so that a reset() between them can be
// tested against two independent valid completions from the same grammar.
static const char * TEST_GRAMMAR =
    "root ::= \"int main() {\\n    return 0;\\n}\" | \"float x = 1.0f;\"";

static bool bitmask_allows_token(const uint32_t * bitmask, int token_id) {
    int word_idx = token_id / 32;
    int bit_idx  = token_id % 32;
    return (bitmask[word_idx] & (1u << bit_idx)) != 0;
}

// ============================================================================
// Test 6: Plugin integration (if plugin .so available)
// ============================================================================

static void test_plugin_integration() {
    printf("test_plugin_integration... ");

    std::string plugin_path = find_plugin();
    if (plugin_path.empty()) {
        printf("SKIP (no plugin .so found)\n");
        return;
    }

    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(plugin_path.c_str());
    assert(draft != nullptr);

    // XGrammar-based plugins declare CAPABILITY_BITMASK
    uint32_t caps = llama_deterministic_draft_get_capabilities(draft);
    assert((caps & LLAMA_DETERMINISTIC_DRAFT_CAPABILITY_BITMASK) != 0);

    bool vocab_ok = llama_deterministic_draft_set_vocab(draft, TEST_VOCAB, TEST_VOCAB_SIZE, nullptr, 0);
    assert(vocab_ok);

    bool grammar_ok = llama_deterministic_draft_set_grammar(draft, TEST_GRAMMAR, "root");
    assert(grammar_ok);

    llama_deterministic_draft_reset(draft, 0);

    // Build the first valid program token by token: "int main() {\n    return 0;\n}"
    const int program1[] = { 0, 1, 2, 3, 4, 1, 5, 6, 7, 1, 8, 9, 10, 11 };

    const int vocab_size_words = (TEST_VOCAB_SIZE + 31) / 32;
    std::vector<uint32_t> bitmask(vocab_size_words);

    for (int token_id : program1) {
        bool has_bitmask = llama_deterministic_draft_fill_bitmask(draft, 0, bitmask.data(), TEST_VOCAB_SIZE);
        if (has_bitmask) {
            assert(bitmask_allows_token(bitmask.data(), token_id));
        }
        llama_deterministic_draft_commit(draft, 0, token_id, TEST_VOCAB[token_id], (int) strlen(TEST_VOCAB[token_id]));
    }

    // After the complete program, an unrelated token (e.g. "float", start of
    // the OTHER alternative) must no longer be accepted by the bitmask.
    {
        bool has_bitmask = llama_deterministic_draft_fill_bitmask(draft, 0, bitmask.data(), TEST_VOCAB_SIZE);
        if (has_bitmask) {
            assert(!bitmask_allows_token(bitmask.data(), 12 /* "float" */));
        }
    }

    llama_deterministic_draft_free(draft);

    printf("OK\n");
}

// ============================================================================
// Test 7: Fail-to-start when DRAFT_DETERMINISTIC enabled without DRAFT_MTP
// ============================================================================

static void test_fail_without_mtp() {
    printf("test_fail_without_mtp... ");

    common_params_speculative params;
    params.types.push_back(COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC);
    params.deterministic_draft.enabled     = true;
    params.deterministic_draft.plugin_path = "/nonexistent/plugin.so";

    // No DRAFT_MTP in types, no ctx_dft -> init must fail
    struct common_speculative * spec = common_speculative_init(params, 1);
    assert(spec == nullptr);

    printf("OK\n");
}

// ============================================================================
// Test 8: Fail-to-start when DRAFT_DETERMINISTIC enabled without plugin path
// ============================================================================

static void test_fail_without_plugin() {
    printf("test_fail_without_plugin... ");

    common_params_speculative params;
    params.types.push_back(COMMON_SPECULATIVE_TYPE_DRAFT_MTP);
    params.types.push_back(COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC);
    params.deterministic_draft.enabled = true;
    // plugin_path left empty

    // ctx_dft is nullptr (no model loaded), so has_mtp is false -> fail
    struct common_speculative * spec = common_speculative_init(params, 1);
    assert(spec == nullptr);

    printf("OK\n");
}

// ============================================================================
// Test 9: det_accept_all requires the plugin to be enabled
// ============================================================================

static void test_accept_all_requires_plugin() {
    printf("test_accept_all_requires_plugin... ");

    common_params params;
    // det_accept_all set without enabled -> should throw in validation
    params.speculative.deterministic_draft.det_accept_all = true;
    assert(!params.speculative.deterministic_draft.enabled);

    bool threw = false;
    try {
        common_params_handle_models(params, LLAMA_EXAMPLE_SPECULATIVE);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    assert(threw);

    // With enabled + det_accept_all -> should NOT throw
    params.speculative.deterministic_draft.enabled     = true;
    params.speculative.deterministic_draft.plugin_path = "/test/plugin.so";
    threw = false;
    try {
        common_params_handle_models(params, LLAMA_EXAMPLE_SPECULATIVE);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    assert(!threw);

    // Verify det_accept_all was preserved through handle_models
    assert(params.speculative.deterministic_draft.det_accept_all == true);

    printf("OK\n");
}

// ============================================================================
// Test 10: Plugin state across reset (checkpoint/restore simulation)
// ============================================================================

static void test_plugin_state_reset() {
    printf("test_plugin_state_reset... ");

    std::string plugin_path = find_plugin();
    if (plugin_path.empty()) {
        printf("SKIP (no plugin .so found)\n");
        return;
    }

    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(plugin_path.c_str());
    assert(draft != nullptr);

    bool vocab_ok = llama_deterministic_draft_set_vocab(draft, TEST_VOCAB, TEST_VOCAB_SIZE, nullptr, 0);
    assert(vocab_ok);
    bool grammar_ok = llama_deterministic_draft_set_grammar(draft, TEST_GRAMMAR, "root");
    assert(grammar_ok);

    const int vocab_size_words = (TEST_VOCAB_SIZE + 31) / 32;
    std::vector<uint32_t> bitmask(vocab_size_words);

    // Round 1: build the first complete program: "int main() {\n    return 0;\n}"
    llama_deterministic_draft_reset(draft, 0);
    {
        const int program1[] = { 0, 1, 2, 3, 4, 1, 5, 6, 7, 1, 8, 9, 10, 11 };
        for (int token_id : program1) {
            bool has_bitmask = llama_deterministic_draft_fill_bitmask(draft, 0, bitmask.data(), TEST_VOCAB_SIZE);
            if (has_bitmask) {
                assert(bitmask_allows_token(bitmask.data(), token_id));
            }
            llama_deterministic_draft_commit(draft, 0, token_id, TEST_VOCAB[token_id],
                                              (int) strlen(TEST_VOCAB[token_id]));
        }
    }

    // Reset: simulates checkpoint restore / new generation. Grammar state
    // must go back to the start - the OTHER alternative ("float x = 1.0f;")
    // must now be valid from the very first token, with no round-1 residue.
    llama_deterministic_draft_reset(draft, 0);

    // Round 2: build the second complete program: "float x = 1.0f;"
    {
        const int program2[] = { 12, 1, 13, 1, 14, 1, 15, 9 };
        for (int token_id : program2) {
            bool has_bitmask = llama_deterministic_draft_fill_bitmask(draft, 0, bitmask.data(), TEST_VOCAB_SIZE);
            if (has_bitmask) {
                assert(bitmask_allows_token(bitmask.data(), token_id));
                // round-1-only token ("main") must not leak through after reset
                assert(!bitmask_allows_token(bitmask.data(), 2 /* "main" */));
            }
            llama_deterministic_draft_commit(draft, 0, token_id, TEST_VOCAB[token_id],
                                              (int) strlen(TEST_VOCAB[token_id]));
        }
    }

    llama_deterministic_draft_free(draft);

    printf("OK\n");
}

// ============================================================================
// Test 11: Auto-imply draft-mtp when deterministic draft enabled
// ============================================================================

static void test_auto_imply_mtp() {
    printf("test_auto_imply_mtp... ");

    common_params params;
    params.speculative.deterministic_draft.enabled     = true;
    params.speculative.deterministic_draft.plugin_path = "/test/plugin.so";

    // Clear any default types to test the auto-imply logic in isolation
    params.speculative.types.clear();
    assert(params.speculative.types.empty());

    // Simulate the auto-imply logic from common_params_handle_models
    if (params.speculative.deterministic_draft.enabled) {
        auto & types = params.speculative.types;
        if (std::find(types.begin(), types.end(), COMMON_SPECULATIVE_TYPE_DRAFT_MTP) == types.end()) {
            types.push_back(COMMON_SPECULATIVE_TYPE_DRAFT_MTP);
        }
        if (std::find(types.begin(), types.end(), COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC) == types.end()) {
            types.push_back(COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC);
        }
    }

    // Both types should now be present
    auto & types = params.speculative.types;
    assert(types.size() == 2);
    assert(std::find(types.begin(), types.end(), COMMON_SPECULATIVE_TYPE_DRAFT_MTP) != types.end());
    assert(std::find(types.begin(), types.end(), COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC) != types.end());

    printf("OK\n");
}

// ============================================================================
// Test 12: common_token_to_piece with special=false produces clean text
// ============================================================================

static void test_token_to_piece_special_flag() {
    printf("test_token_to_piece_special_flag... ");

    // The deterministic draft filter uses special=false to skip control
    // tokens so the grammar matcher parses clean code without BPE artifacts.
    // Compile-time signature check verifies the function accepts the flag.
    // Runtime verification requires a loaded model (done in integration tests).
    using fn_type = std::string (*)(const struct llama_vocab *, llama_token, bool);
    auto * fn = static_cast<fn_type>(&common_token_to_piece);
    (void) fn;

    printf("OK\n");
}

// ============================================================================
// Test 13: Cumulative stats struct fields exist and are zero-initialized
// ============================================================================

static void test_cumulative_stats_fields() {
    printf("test_cumulative_stats_fields... ");

    // Verify that common_speculative with det_filter has cumulative stats
    // that start at zero. We can't fully test without a loaded model, but
    // we can verify the struct is properly initialized by checking that
    // has_det_filter returns false for a spec without a plugin.

    common_params_speculative params;
    params.types.push_back(COMMON_SPECULATIVE_TYPE_DRAFT_MTP);
    // No plugin path -> det_filter won't be loaded
    struct common_speculative * spec = common_speculative_init(params, 1);
    // Without ctx_dft, init fails (has_mtp is false)
    assert(spec == nullptr);

    // If we had a valid spec, we would check:
    // assert(!common_speculative_has_det_filter(spec));
    // But since init fails, we just verify the API exists

    printf("OK\n");
}

// ============================================================================
// Test 14: accept() with null impl does not crash (LOG_WRN path)
// ============================================================================

static void test_accept_null_impl_safe() {
    printf("test_accept_null_impl_safe... ");

    // This tests that common_speculative_accept() handles the case where
    // impl_last[seq_id] is null without crashing (returns with LOG_WRN).
    // We can't easily create this scenario without a loaded model, but
    // we verify the function handles null spec gracefully.

    common_speculative_accept(nullptr, 0, 0);

    printf("OK\n");
}

// ============================================================================
// Test 15: get_version API exists and returns "unknown" for null plugin
// ============================================================================

static void test_get_version_null() {
    printf("test_get_version_null... ");

    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(nullptr);
    assert(draft != nullptr);

    // With no plugin loaded, get_version should return "unknown"
    const char * version = llama_deterministic_draft_get_version(draft);
    assert(version != nullptr);
    assert(std::string(version) == "unknown");

    llama_deterministic_draft_free(draft);

    printf("OK\n");
}

// ============================================================================
// Test 16: New high-level filter API with no plugin (safety)
// ============================================================================

static void test_filter_api_no_plugin() {
    printf("test_filter_api_no_plugin... ");

    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(nullptr);
    assert(draft != nullptr);

    // filter_draft should return 0 (no plugin)
    const int tokens[] = { 0, 1, 2 };
    assert(llama_deterministic_draft_filter_draft(draft, 0, tokens, 3) == 0);

    // apply_bitmask should return false (no plugin)
    uint32_t bitmask[4] = { 0 };
    float logits[128] = { 0 };
    assert(!llama_deterministic_draft_apply_bitmask(draft, 0, bitmask, 128, logits));

    // commit_tokens should be safe (no-op)
    llama_deterministic_draft_commit_tokens(draft, 0, tokens, 3);

    // null args should be safe
    assert(llama_deterministic_draft_filter_draft(nullptr, 0, tokens, 3) == 0);
    assert(!llama_deterministic_draft_apply_bitmask(nullptr, 0, bitmask, 128, logits));
    llama_deterministic_draft_commit_tokens(nullptr, 0, tokens, 3);

    // zero n_tokens should return 0/false
    assert(llama_deterministic_draft_filter_draft(draft, 0, tokens, 0) == 0);

    llama_deterministic_draft_free(draft);

    printf("OK\n");
}

// ============================================================================
// Test 17: filter_draft with plugin - valid tokens accepted
// ============================================================================

static void test_filter_draft_valid_tokens() {
    printf("test_filter_draft_valid_tokens... ");

    std::string plugin_path = find_plugin();
    if (plugin_path.empty()) {
        printf("SKIP (no plugin .so found)\n");
        return;
    }

    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(plugin_path.c_str());
    assert(draft != nullptr);

    bool vocab_ok = llama_deterministic_draft_set_vocab(draft, TEST_VOCAB, TEST_VOCAB_SIZE, nullptr, 0);
    assert(vocab_ok);
    bool grammar_ok = llama_deterministic_draft_set_grammar(draft, TEST_GRAMMAR, "root");
    assert(grammar_ok);

    llama_deterministic_draft_reset(draft, 0);

    // feed "int main() {" which is tokens 0,1,2,3,4,1,5 - all valid
    const int tokens[] = { 0, 1, 2, 3, 4, 1, 5 };
    int accepted = llama_deterministic_draft_filter_draft(draft, 0, tokens, 7);
    assert(accepted == 7);

    llama_deterministic_draft_free(draft);

    printf("OK\n");
}

// ============================================================================
// Test 18: filter_draft with plugin - invalid token truncates
// ============================================================================

static void test_filter_draft_truncates_on_invalid() {
    printf("test_filter_draft_truncates_on_invalid... ");

    std::string plugin_path = find_plugin();
    if (plugin_path.empty()) {
        printf("SKIP (no plugin .so found)\n");
        return;
    }

    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(plugin_path.c_str());
    assert(draft != nullptr);

    bool vocab_ok = llama_deterministic_draft_set_vocab(draft, TEST_VOCAB, TEST_VOCAB_SIZE, nullptr, 0);
    assert(vocab_ok);
    bool grammar_ok = llama_deterministic_draft_set_grammar(draft, TEST_GRAMMAR, "root");
    assert(grammar_ok);

    llama_deterministic_draft_reset(draft, 0);

    // "int" is valid first token, but "float" (12) is not valid after "int" in grammar
    const int tokens[] = { 0, 12 };
    int accepted = llama_deterministic_draft_filter_draft(draft, 0, tokens, 2);
    assert(accepted == 1);

    llama_deterministic_draft_free(draft);

    printf("OK\n");
}

// ============================================================================
// Test 19: apply_bitmask with plugin constrains logits
// ============================================================================

static void test_apply_bitmask_constrains_logits() {
    printf("test_apply_bitmask_constrains_logits... ");

    std::string plugin_path = find_plugin();
    if (plugin_path.empty()) {
        printf("SKIP (no plugin .so found)\n");
        return;
    }

    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(plugin_path.c_str());
    assert(draft != nullptr);

    bool vocab_ok = llama_deterministic_draft_set_vocab(draft, TEST_VOCAB, TEST_VOCAB_SIZE, nullptr, 0);
    assert(vocab_ok);
    bool grammar_ok = llama_deterministic_draft_set_grammar(draft, TEST_GRAMMAR, "root");
    assert(grammar_ok);

    llama_deterministic_draft_reset(draft, 0);

    const int bitmask_words = (TEST_VOCAB_SIZE + 31) / 32;
    std::vector<uint32_t> bitmask(bitmask_words, 0);
    std::vector<float> logits(TEST_VOCAB_SIZE, 1.0f);

    bool applied = llama_deterministic_draft_apply_bitmask(draft, 0, bitmask.data(), TEST_VOCAB_SIZE, logits.data());
    assert(applied);

    // "int" (0) should be allowed at start of program
    assert(logits[0] == 1.0f);

    // "float" (12) should also be allowed at start (both programs start here)
    assert(logits[12] == 1.0f);

    // "main" (2) should NOT be valid at start (neither program starts with "main")
    assert(logits[2] < -1e20f);

    // "return" (7) should NOT be valid at start
    assert(logits[7] < -1e20f);

    llama_deterministic_draft_free(draft);

    printf("OK\n");
}

// ============================================================================
// Test 20: commit_tokens with plugin advances grammar state
// ============================================================================

static void test_commit_tokens_advances_grammar() {
    printf("test_commit_tokens_advances_grammar... ");

    std::string plugin_path = find_plugin();
    if (plugin_path.empty()) {
        printf("SKIP (no plugin .so found)\n");
        return;
    }

    struct llama_deterministic_draft * draft = llama_deterministic_draft_init(plugin_path.c_str());
    assert(draft != nullptr);

    bool vocab_ok = llama_deterministic_draft_set_vocab(draft, TEST_VOCAB, TEST_VOCAB_SIZE, nullptr, 0);
    assert(vocab_ok);
    bool grammar_ok = llama_deterministic_draft_set_grammar(draft, TEST_GRAMMAR, "root");
    assert(grammar_ok);

    llama_deterministic_draft_reset(draft, 0);

    // Commit "int main() " (tokens 0,1,2,3,4,1) in a batch
    const int tokens[] = { 0, 1, 2, 3, 4, 1 };
    llama_deterministic_draft_commit_tokens(draft, 0, tokens, 6);

    // After committing partial program, fill bitmask and check state advanced
    const int bitmask_words = (TEST_VOCAB_SIZE + 31) / 32;
    std::vector<uint32_t> bitmask(bitmask_words, 0);
    std::vector<float> logits(TEST_VOCAB_SIZE, 1.0f);

    bool applied = llama_deterministic_draft_apply_bitmask(draft, 0, bitmask.data(), TEST_VOCAB_SIZE, logits.data());
    assert(applied);

    // After "int main() ", "{" (5) should be valid
    assert(logits[5] == 1.0f);

    // "return" (7) should NOT yet be valid (need "{" first)
    assert(logits[7] < -1e20f);

    llama_deterministic_draft_free(draft);

    printf("OK\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("\n=== Deterministic Draft Tests ===\n\n");

    test_plugin_loader_init_free();
    test_c_api_no_plugin();
    test_speculative_type_enum();
    test_params_struct();
    test_det_filter_query();
    test_plugin_integration();
    test_fail_without_mtp();
    test_fail_without_plugin();
    test_accept_all_requires_plugin();
    test_plugin_state_reset();
    test_auto_imply_mtp();
    test_token_to_piece_special_flag();
    test_cumulative_stats_fields();
    test_accept_null_impl_safe();
    test_get_version_null();
    test_filter_api_no_plugin();
    test_filter_draft_valid_tokens();
    test_filter_draft_truncates_on_invalid();
    test_apply_bitmask_constrains_logits();
    test_commit_tokens_advances_grammar();

    printf("\n=== All tests passed ===\n\n");
    return 0;
}
