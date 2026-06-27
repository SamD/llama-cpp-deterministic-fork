// llama_deterministic_draft.h -- Consumer API for deterministic draft plugins
//
// This header declares the C API used to load and interact with deterministic
// draft plugins (.so/.dylib/.dll). The primary consumer is llama.cpp core
// (src/llama-deterministic-draft.cpp, linked into libllama.a), which implements
// this API via dlopen/dlsym at runtime when --det-draft-model is passed.
//
// This header is also distributed with libdeterministic_draft_spec.so for
// standalone testing of plugins without the full llama.cpp build.
//
// This header is self-contained - it does not require llama.h or any other
// llama.cpp headers. It only requires the opaque struct forward declaration
// and standard C types.

#ifndef LLAMA_DETERMINISTIC_DRAFT_API_H
#define LLAMA_DETERMINISTIC_DRAFT_API_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the plugin loader instance.
struct llama_deterministic_draft;

// Capability flags (bitmask) - returned by llama_deterministic_draft_get_capabilities
#include "deterministic_draft_capabilities.h"
#ifndef LLAMA_DETERMINISTIC_DRAFT_CAPABILITY_BITMASK
#define LLAMA_DETERMINISTIC_DRAFT_CAPABILITY_BITMASK      DETERMINISTIC_DRAFT_CAPABILITY_BITMASK
#define LLAMA_DETERMINISTIC_DRAFT_CAPABILITY_JUMP_FORWARD DETERMINISTIC_DRAFT_CAPABILITY_JUMP_FORWARD
#endif

// --- Lifecycle ---

// Initialize a deterministic draft plugin from a shared library path.
// Loads the .so via dlopen, resolves symbols, creates plugin state.
// Returns NULL on failure.
struct llama_deterministic_draft * llama_deterministic_draft_init(const char * plugin_path);

// Free a deterministic draft plugin instance.
// Destroys plugin state and unloads the .so.
void llama_deterministic_draft_free(struct llama_deterministic_draft * draft);

// --- Capabilities ---

// Query plugin capabilities. Returns bitmask of CAPABILITY_* flags,
// or 0 if the plugin doesn't implement capability negotiation.
uint32_t llama_deterministic_draft_get_capabilities(struct llama_deterministic_draft * draft);

// --- Tokenizer setup (for BITMASK/JUMP_FORWARD plugins) ---

// Provide vocabulary information to the plugin. Returns true on success.
bool llama_deterministic_draft_set_vocab(struct llama_deterministic_draft * draft,
                                          const char **                     vocab_entries,
                                          int                               vocab_size,
                                          const int32_t *                   stop_tokens,
                                          int                               n_stop);

// --- Grammar configuration ---

// Load a grammar from an EBNF/GBNF string. Returns true on success.
bool llama_deterministic_draft_set_grammar(struct llama_deterministic_draft * draft,
                                            const char *                      ebnf_str,
                                            const char *                      root_rule);

// Select a bundled grammar for the given slot by language name (e.g. "python",
// "c", "java", "javascript"). The plugin resolves and loads the grammar itself
// from its own bundled grammar directory. Returns true on success, false if
// the language is unknown or failed to load.
bool llama_deterministic_draft_set_language(struct llama_deterministic_draft * draft,
                                             int                                slot_id,
                                             const char *                       lang);

// Get the currently active language name for the given slot.
const char * llama_deterministic_draft_get_language(struct llama_deterministic_draft * draft, int slot_id);

// --- Bitmask (CAPABILITY_BITMASK) ---

// Fill a bitmask indicating which token IDs are valid for the next step.
// bitmask must be pre-allocated with size (vocab_size + 31) / 32.
// Returns true if the bitmask was filled and should be applied.
bool llama_deterministic_draft_fill_bitmask(struct llama_deterministic_draft * draft,
                                             int                                slot_id,
                                             uint32_t *                         bitmask,
                                             int                                vocab_size);

// --- Jump-forward (CAPABILITY_JUMP_FORWARD) ---

// Get the longest string uniquely determined by the current grammar state.
// Returns NULL if no jump-forward is available. The returned string is owned
// by the plugin and must not be freed by the caller.
const char * llama_deterministic_draft_get_jump_forward(struct llama_deterministic_draft * draft,
                                                         int                                slot_id,
                                                         int *                              out_length);

// --- Commit ---

// Commit an accepted token to the plugin's grammar state.
// token_id is used by tokenizer-aware plugins (e.g. XGrammar's AcceptToken).
void llama_deterministic_draft_commit(struct llama_deterministic_draft * draft,
                                       int                                slot_id,
                                       int32_t                            token_id,
                                       const char *                       token_text,
                                       int                                token_length);

// Undo the last n_tokens commit() calls for the given slot, restoring the
// grammar matcher to its prior state. Returns true on success, false if the
// plugin doesn't support rollback or n_tokens is invalid.
bool llama_deterministic_draft_rollback(struct llama_deterministic_draft * draft,
                                         int                                slot_id,
                                         int                                n_tokens);

// --- High-level filter helpers ---

// Filter a batch of draft tokens against the grammar bitmask.
// For each token, fills the bitmask, checks if the token is valid, and
// commits valid tokens to the grammar state. Stops at the first invalid token.
// Returns the number of leading valid tokens (committed to grammar state).
int llama_deterministic_draft_filter_draft(struct llama_deterministic_draft * draft,
                                            int                                slot_id,
                                            const int32_t *                    tokens,
                                            int                                n_tokens);

// Fill a bitmask and apply it to a logits array.
// Sets logits[i] = -1e30f for invalid tokens (bit clear in bitmask).
// Returns true if a bitmask was applied, false if no constraint needed.
bool llama_deterministic_draft_apply_bitmask(struct llama_deterministic_draft * draft,
                                              int                                slot_id,
                                              uint32_t *                         bitmask,
                                              int                                vocab_size,
                                              float *                            logits);

// Commit multiple tokens to the grammar state.
// Converts token IDs to text internally using the vocabulary from set_vocab().
void llama_deterministic_draft_commit_tokens(struct llama_deterministic_draft * draft,
                                              int                                slot_id,
                                              const int32_t *                    tokens,
                                              int                                n_tokens);

// --- State access ---

// Reset the plugin state for the given slot.
void llama_deterministic_draft_reset(struct llama_deterministic_draft * draft, int slot_id);

// --- Metadata ---

// Return the plugin version string (e.g. "3.0.0").
// The returned string is valid for the lifetime of the plugin.
const char * llama_deterministic_draft_get_version(struct llama_deterministic_draft * draft);

#ifdef __cplusplus
}
#endif

#endif  // LLAMA_DETERMINISTIC_DRAFT_API_H
