## Context

Current code generation with LLMs uses GBNF grammar masking for syntax enforcement and speculative decoding (`--spec-draft-model`) for token acceleration. Neither validates structural correctness via AST nor provides deterministic error diagnostics. llama.cpp has no tree-sitter integration; it uses a custom PEG parser (`common/peg-parser.h`) and Jinja engine (`common/jinja/`). The PoC was initially implemented as a `common_sampler` extension but is being rearchitected into the `common_speculative` pipeline as a proper speculative type composing with MTP.

## Goals / Non-Goals

**Goals:**
- Guarantee structural validity of generated code via XGrammar bitmask-based grammar-constrained decoding
- Filter MTP draft tokens through deterministic structural validation; truncate at first error and let main model resume naturally (truncate + reverify)
- Integrate as `COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC` speculative type composing with MTP
- Support per-token validation and draft-scale batches (Multi-Token eXchange / MTX: 100s to 1000s of tokens)

**Non-Goals:**
- Style rule enforcement (camelCase, indentation) - follow-on phase after structural validity proven
- Multi-language projects (JSX, Vue SFCs) - single-language gatekeeper for PoC
- Grammar masking replacement (GBNF remains opt-in; grammar-constrained decoder is additive)

## Decisions

| Decision | Rationale | Alternatives |
|---|---|---|
| **Clean SDK separation** | Main tree has zero domain-specific code; plugin loaded at runtime via dlopen; external artifacts distributed as header + .so | Embed XGrammar in core (bloat, coupling), single repo (no separation) |
| **`external/` directory** | Gated by `DETERMINISTIC_SPEC_ENABLED`; produces distributable header + spec loader .so with no llama dependency | Install to system paths (less portable), header-only (insufficient for real SDK) |
| **`deterministic-draft-model-poc/`** | Standalone consumer project; zero main tree dependency; simulates private repo (law firm, gov agency) | Build in main tree (violates separation), no reference implementation (harder to adopt) |
| **Fail-fast draft validation** | One error per draft stops validation; preserves valid prefix without discarding | Full-draft re-parse (slower), per-token only (higher overhead) |
| **Truncate + reverify** | Mirrors how MTP rejection already works; simplest; no fix coherence risk | Fix injection (fix may be rejected by main model), diagnostic injection (consumes context) |
| **New `COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC`** | Proper speculative pipeline stage; composes with MTP; clean separation | Sampler extension (bolt-on, decoupled from MTP), filter inside MTP impl (couples logic) |
| **Auto-imply draft-mtp** | `--deterministic-draft-model` requires MTP heads; auto-adding `draft-mtp` avoids user error | Require explicit `--spec-type draft-mtp` (error-prone) |
| **Plugin state via accept() only** | Plugin state stays correct across checkpoint/restore since accept() is the only mutation point | Advance during draft() (breaks on rollback) |
| **Commit full prompt as code context** | Plugin needs bracket context from prompt code | Empty buffer (misses context), last code block only (requires heuristic parsing) |

## Risks / Trade-offs

- [Tokenization mismatch] Subword tokens may split UTF-8 boundaries; XGrammar rejects incomplete fragments -> **Mitigation**: BPE models preserve UTF-8; verify per target model |
- [XGrammar as new dependency] Adds C++ library + per-language grammars + build complexity -> **Mitigation**: Optional linkage (`FetchContent`); default to no grammar if unavailable |
- [Deep nesting latency] Templates/C++ nested structures increase grammar compilation cost -> **Mitigation**: Benchmark worst-case; lazy grammar loading for inactive rules |
- [Non-code content in chat templates] Chat-formatted prompts include system/user tags that may not match any grammar -> **Mitigation**: Recommend raw completion mode for PoC; plugin should handle gracefully |
- [MTP model availability] Only Qwen3.5/3.6 and Step3 architectures support MTP heads; additionally, Qwen3.5/3.6 use M-RoPE which requires correct position tracking in benchmark tooling -> **Mitigation**: The bench tool position accounting bug has been fixed; Qwen3.5/3.6 are compatible with the deterministic draft filter. |

## Open Questions

- Draft size policy: fixed N vs. AST-boundary (function/class completion) - **Resolved** (flag-based)
- MISSING node tolerance at trailing edge: accept incomplete code mid-generation - **Resolved** (grammar-constrained decoder triggers on error only)
- Rule config format (YAML/TOML/DSL) for follow-on style phase - deferred; no config for phase 1 structural validation
- Serialization of grammar state for server contexts - **Deferred to Phase 2**
- Polyglot prompt parsing: how to extract code from chat-formatted prompts (deferred)
- Non-code extension: pluggable validators for non-coding tasks (future work)

## Decisions Deferred to Phase 2

- Server state serialization: XGrammar matcher state not checkpointed; memory-only, restart loses state
- Grammar fallback: XGrammar only; no alternative grammar library for PoC
- UTF-8 boundary guard: BPE models preserve UTF-8; Unigram mid-character splits not in scope for PoC

## Additional Decisions

| Decision | Rationale | Alternatives |
|---|---|---|
| **Draft size flag (`--det-draft-n-max`)** | `--det-draft-n-max` sets BOTH the filter cap and MTP draft count when > 0 (auto-derives --spec-draft-n-max). Default -1 = no filter cap (MTP default of 3). | Separate flags (more complex), AST-boundary cutoff (requires heuristic) |
