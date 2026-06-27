> **SUPERSEDED** -- This spec describes the original tree-sitter-based AST gatekeeper
> design, which used `ts_node_has_error()`, MISSING/ERROR nodes, and per-token
> incremental parsing. The current implementation uses XGrammar's bitmask-based
> grammar-constrained decoding instead. See `include/deterministic_draft_plugin.h`
> for the current plugin C API and `deterministic-draft-model-poc/docs/quick-start.md`
> for the current architecture.

---

## ADDED Requirements

### Requirement: Structural token validation
The gatekeeper SHALL validate each proposed token by appending its text to the accumulated code buffer and performing an incremental tree-sitter parse, accepting the token only if `ts_node_has_error()` on the resulting root node returns false.

#### Scenario: Valid token accepted
- **WHEN** the model proposes a token that does not introduce a structural error
- **THEN** the gatekeeper accepts the token and updates the parse tree

#### Scenario: Invalid token rejected
- **WHEN** the model proposes a token that introduces an ERROR node in the parse tree
- **THEN** the gatekeeper rejects the token and reports the error position and reason

### Requirement: Trailing-edge tolerance
The gatekeeper SHALL tolerate incomplete code mid-generation by treating MISSING nodes at the trailing edge (byte offset equal to buffer length) as non-errors, accepting tokens that produce trailing MISSING nodes.

#### Scenario: Trailing missing closer accepted
- **WHEN** the model proposes a token that produces a MISSING node for a closer (`}`, `)`, `]`) at the end of the buffer
- **THEN** the gatekeeper accepts the token without reporting an error

### Requirement: Truncate and reverify on structural error
The gatekeeper SHALL truncate the draft at the first structural error, passing only the valid prefix to the main model for verification. The main model verifies the valid prefix in one sweep, then resumes normal generation from the error point. No fix tokens are injected.

#### Scenario: Draft truncated at error
- **WHEN** the MTP draft contains a token that introduces an ERROR node at position K
- **THEN** the gatekeeper truncates the draft to K tokens (valid prefix) and the main model verifies only those K tokens

#### Scenario: Full draft accepted
- **WHEN** the entire MTP draft passes structural validation
- **THEN** the gatekeeper passes the full draft to the main model for verification without truncation

### Requirement: Speculative pipeline integration
The deterministic draft SHALL integrate as a new speculative type `COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC` in `common_speculative`, composing with `COMMON_SPECULATIVE_TYPE_DRAFT_MTP`. MTP draft heads generate raw draft tokens; the deterministic filter validates them before the main model verifies the filtered draft.

#### Scenario: Gatekeeper enabled via CLI flag
- **WHEN** the user passes `--deterministic-draft-model` at CLI or server request
- **THEN** `COMMON_SPECULATIVE_TYPE_DRAFT_MTP` and `COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC` are auto-added to `speculative.types`

#### Scenario: Model lacks MTP heads
- **WHEN** `--deterministic-draft-model` is enabled but the model has `n_layer_nextn == 0`
- **THEN** llama.cpp fails to start with an error indicating MTP heads are required

### Requirement: Draft-scale batch validation
The gatekeeper SHALL validate the entire MTP draft in a single batch sweep using `deterministic_draft_filter_draft()`, stopping at the first structural error and returning the count of valid tokens. This replaces per-token validation for draft-scale throughput.

#### Scenario: Large draft validated in one sweep
- **WHEN** the MTP draft heads generate a draft of 200 tokens
- **THEN** the gatekeeper validates all 200 tokens in a single batch sweep, truncates at the first error if any, and returns the valid prefix

### Requirement: Error diagnostics
The gatekeeper SHALL report precise error position (byte offset), human-readable error reason, offending token text, and suggested fix string when a structural violation occurs, emitting diagnostics to stderr or server JSON stream.

#### Scenario: Error diagnostic emitted
- **WHEN** the gatekeeper rejects a token due to structural violation
- **THEN** the gatekeeper logs error position, reason, bad token, and fix suggestion

### Requirement: Plugin state synchronization
The gatekeeper SHALL synchronize plugin state only during `common_speculative_accept()` (committing accepted tokens to the plugin grammar state via `deterministic_draft_commit()`). In `common_speculative_begin()`, the plugin state SHALL be reset and all prompt tokens committed as code context. Plugin state SHALL NOT advance during `common_speculative_draft()`.

#### Scenario: Plugin state on accept
- **WHEN** the main model accepts K tokens from the filtered draft
- **THEN** the gatekeeper commits those K tokens to the plugin grammar state

#### Scenario: Plugin state on rollback
- **WHEN** the main model rejects tokens and a checkpoint restore occurs
- **THEN** the plugin state remains correct because it only advanced during the prior `accept()` call

### Requirement: Context window floor
The system SHALL ensure the active context window is sufficiently large to accommodate MTP draft generation and deterministic filter validation overhead.
