# Discussion Notes: deterministic-spec

> **Status**: Historical design exploration notes. The PoC approach described here (sampler extension, three-strategy error handling) has been replaced by the production architecture in `common_speculative`. See `proposal.md` for current design and `deterministic-draft-model-poc/docs/` for current implementation docs.
>
> Design decisions from this document that were carried forward are marked below. Code sketches, line number references, and implementation details from the removed PoC are retained for reference only.

Captured from explore session on 2026-06-20. Reference for design decisions and code sketches.

---

## Key Decisions

1. **AST first, style second** - structural validation (bracket matching, syntax) precedes checkstyle/style rules
2. **No GBNF required** - grammar-constrained decoder replaces GBNF masking; GBNF remains opt-in additive layer
3. **XGrammar C++ library** - bitmask-based grammar-constrained decoding, per-language GBNF grammars
4. **Fail-fast draft validation** - one error per draft stops validation; preserves valid prefix
5. **Deterministic fix injection** - bracket matching and missing closers derivable from parse tree
6. **Byte-native gatekeeper** - gatekeeper owns text buffer; appends `token_text`; no token-index table
7. **Optional `common_sampler` extension** - avoids core API invasiveness; composes with spec decoding
8. **Draft size flag** - `--deterministic-draft-model-n` specifies MTX draft token count independently
9. **UTF-8 boundary guard** - BPE models preserve UTF-8; Unigram mid-character splits not in scope for PoC
10. **Error recovery terminal case** - Strategy B (diagnostic injection) is final fallback; generation never halts
11. **Rule config format** - No config for phase 1; enters with style rules (checkstyle) in phase 2

---

## Sample Code Sketch (C++) -- SUPERSEDED

The initial PoC explored a per-token `ASTGatekeeper` pattern where each draft token was validated one at a time via `validate()`/`commit()` with fix injection. This was replaced by the current batch truncate + reverify approach implemented in `common_speculative`. See "Session Recovery Notes" below for the rearchitecture decisions.

---

## Naming

- **Deterministic Spec** (DSpec) - parallel to speculative decoding; deterministic draft + verification
- **MTX** (Multi-Token eXchange) - internal handle for draft-scale validation (100s-1000s tokens)

---

## Open Threads

- Draft size policy: fixed N vs. AST-boundary (function/class completion) - **Resolved** (flag-based)
- MISSING node tolerance at trailing edge: accept incomplete code mid-generation - **Resolved** (gatekeeper triggers on error only)
- Rule config format (YAML/TOML/DSL) for follow-on style phase - **Resolved** (deferred to phase 2)
- Serialization of tree-sitter state for server contexts - **Resolved** (phase 2, memory-only)
- Batch decode optimization for MTX throughput
- Integration point: `common_sampler` extension vs. `common_speculative_impl` subclass - **Resolved** (speculative pipeline type via `common_speculative`)

## Plugin Approach (Shared Library)

Recommended: compile gatekeeper + tree-sitter language into a shared library (.so/.dylib/.dll) loaded at runtime via dlopen/dlsym.

**Rationale:**
- No recompilation of llama.cpp to add new languages
- Clean separation - gatekeeper logic in own repo
- Plugin distributed independently - drop .so into directory
- Security - in-process, isolated by C API contract
- Familiar pattern - similar to CUDA/Metal backend loading

**Plugin C API contract** (`ast_gatekeeper_plugin.h`): Opaque handle with lifecycle (create/destroy), validate, commit, get_code, reset, get_language, get_version.

**Naming convention update:** Use `deterministic-draft` / `MTX` namespace instead of `ast_gatekeeper`. Plugin files/APIs should carry `deterministic_draft_*` or `mtx_*` prefixes.

**Directory structure per plugin package:**
```
deterministic-draft-java/
├── lib/
│   ├── java_ast_gatekeeper.so     Linux
│   ├── java_ast_gatekeeper.dylib macOS
│   └── java_ast_gatekeeper.dll   Windows
├── manifest.json                   Metadata
└── README.md
```

**CLI integration:**
```bash
llama-cli -m codellama-7b.Q4_K_M.gguf \
    --deterministic-draft-model ./deterministic-draft-java/lib/java_ast_gatekeeper.so \
    -p "..." -n 256
```

---

## Session Recovery Notes (2026-06-24)

> **Status**: These notes describe the rearchitecture from the old PoC approach to the current production approach. The decisions below have been implemented. The "Git State" section at the end contains file paths and line numbers from an intermediate state that no longer reflect the current codebase.

### Rarchitecture Decision

The PoC implemented the deterministic draft as a `common_sampler` extension. After review, the production architecture moves it into `common_speculative` as a new speculative type `COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC` that composes with `COMMON_SPECULATIVE_TYPE_DRAFT_MTP`.

**Data flow:**
```
MTP draft heads (draft()) -> deterministic filter (tree-sitter) -> main model verify (one sweep)
```

**Key decisions:**
1. New spec type (not sampler extension) - cleanest separation, mirrors how other spec types chain
2. Auto-imply draft-mtp - `--deterministic-draft-model` auto-adds `draft-mtp`; existing `n_layer_nextn == 0` check fails to start if no MTP heads
3. Truncate + reverify - on rejection, pass valid prefix to main model; no fix injection; mirrors MTP rejection
4. Plugin state via accept() only - state advances only during `common_speculative_accept()`; reset + prompt commit in `common_speculative_begin()`
5. Commit full prompt as code context - tree-sitter needs bracket context from prompt code

### Benchmarking Bugs Discovered

The PoC benchmarking was fundamentally flawed:
1. Used non-MTP models (CodeLlama, TinyLlama, Stable Code) - deterministic filter was never in the speculative pipeline
2. Standalone bench tool bypassed `common_speculative` entirely
3. `bench_mtx_pipeline` generated draft tokens by sampling from the main model one-by-one (plain autoregressive), NOT using MTP draft heads
4. No KV rollback on rejection (broke to next generation instead)
5. Wrong throughput metric (counted all draft tokens including rejected, excluded main model verification)
6. Tokenization mismatch (whitespace-tokenized code, not actual BPE model tokens)
7. Metric reporting bug (rejection counts not in JSON output)

Results showed 0% rejection rate and "value proposition unproven" - all invalid due to the above bugs.

### MTP Model Selection

For benchmarking, selected `unsloth/Qwen3.5-2B-MTP-GGUF` (Q4_K_M, ~1.3GB):
- Qwen3.5 architecture supports MTP/nextn heads in llama.cpp
- 2B size is small, fast, and will hallucinate often (ideal for testing the deterministic filter)
- Has 1 nextn layer (`n_layer_nextn == 1`)
- Confirmed llama.cpp MTP support (merged May 2026)

Secondary: `unsloth/Qwen3.5-4B-MTP-GGUF` (better coding, higher draft acceptance).

### Git State (Historical -- PoC Phase Only)

> **Note**: The line numbers below refer to the PoC intermediate state and do not match the current codebase. The files listed have been refactored as part of the SDK separation (Phase 2). List retained for reference.

All PoC work is staged (not committed). Key files:
- `common/arg.cpp` - `--deterministic-draft-model` flag
- `common/sampling.cpp` - sampler extension (removed)
- `tools/server/server-context.cpp` - bolt-on MTX verify (removed)
- `tools/deterministic-draft-bench/bench-deterministic-draft.cpp` - standalone bench (rewritten)
- `src/llama-deterministic-draft.cpp` - plugin loader (retained, refactored)
- `deterministic_draft_plugin.h` - plugin C API (retained, now in external/)
- `plugins/*/plugin.cpp` - tree-sitter plugins (moved to deterministic-draft-model-poc/)
