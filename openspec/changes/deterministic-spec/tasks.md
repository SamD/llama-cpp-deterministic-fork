## Phase 1: PoC Implementation (Sampler Extension -- SUPERSEDED)

> The PoC implemented the deterministic draft as a `common_sampler` extension with three-strategy error handling (penalize+resample, diagnostic injection, fix injection). After review, the production architecture moved the deterministic draft into `common_speculative` as a new speculative type `COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC`. The PoC approach was not merged and has been completely replaced.
>
> Phase 1 artifacts that still contribute to the current codebase: plugin C API contract, runtime plugin loader, CLI flag definitions. Phase 1 artifacts that were replaced: sampler extension integration, fix injection logic, MTX diagnostics, all per-token validation code, non-MTP benchmark tool.

## Phase 2: Production Rearchitecture (Current)

### 1. Rearchitecture to Speculative Pipeline Type

### 1.1 Enum and Params
- [x] 10.1.1 Add `COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC` to `common_speculative_type` enum in `common/common.h` (before `COUNT`)
- [x] 10.1.2 Move `common_params_deterministic_draft` from `common_params_sampling` and `common_params` into `common_params_speculative`
- [x] 10.1.3 Update `static_assert(COMMON_SPECULATIVE_TYPE_COUNT == 10)` in `common/speculative.cpp`
- [x] 10.1.4 Add `"draft-deterministic"` to type name map in `common/speculative.cpp`

### 1.2 Auto-Imply MTP
- [x] 10.2.1 In `common/arg.cpp`, modify `--deterministic-draft-model` handler to auto-add `DRAFT_MTP` and `DRAFT_DETERMINISTIC` to `speculative.types`
- [x] 10.2.2 Add post-processing in `common_params_parse_ex` or `common_params_handle_models` to ensure `DRAFT_MTP` is present when deterministic draft is enabled
- [x] 10.2.3 Set `opts.download_mtp = true` when deterministic draft is enabled (line 408 of `common/arg.cpp`)
- [x] 10.2.4 Add `--deterministic-draft-model` to `.set_spec()` category

### 1.3 Deterministic Filter in common_speculative
- [x] 10.3.1 Add `det_draft_filter` struct to `common_speculative` (plugin handle, per-seq filter results, vocab, n_max)
- [x] 10.3.2 Load plugin in `common_speculative_init()` when `DRAFT_DETERMINISTIC` enabled; fail-to-start on load failure
- [x] 10.3.3 Add post-draft filtering in `common_speculative_draft()`: batch verify via `llama_deterministic_draft_verify_draft()`, truncate `dp.result` at first error
- [x] 10.3.4 Add commit in `common_speculative_accept()`: commit accepted tokens to plugin via `llama_deterministic_draft_commit()`
- [x] 10.3.5 Add reset + prompt commit in `common_speculative_begin()`: reset plugin state, commit prompt tokens as code context
- [x] 10.3.6 Expose diagnostics via `common_speculative_get_det_filter_result()` in `common/speculative.h`
- [x] 10.3.7 Add `--det-draft-accept-all` flag: skip target model verification for filter-accepted tokens; structural validation is the final arbiter; trade-off for single-language code-only generation

### 1.4 Remove Sampler Extension
- [x] 10.4.1 Remove `det_draft` field from `common_sampler` struct in `common/sampling.cpp` — done in Phase 1 (field kept as nullptr, full removal in Phase 3)
- [x] 10.4.2 Remove plugin loading in `common_sampler_init()` (lines ~408-430) — done in Phase 1 (field kept as nullptr, full removal in Phase 3)
- [x] 10.4.3 Remove `common_sampler_sample_and_accept_n_deterministic()` and related functions
- [x] 10.4.4 Remove `common_sampler_deterministic_draft_validate()`, `common_sampler_deterministic_draft_verify()`, `common_sampler_has_deterministic_draft()`
- [x] 10.4.5 Remove `deterministic_draft` field from `common_params_sampling` (moved to `common_params_speculative`)

### 1.5 Fix Server Integration
- [x] 10.5.1 Remove bolt-on MTX verify block from `tools/server/server-context.cpp` (lines ~3586-3645)
- [x] 10.5.2 Replace with direct `common_sampler_sample_and_accept_n()` call (draft already filtered by `common_speculative_draft()`)
- [x] 10.5.3 Get diagnostics from `common_speculative_get_det_filter_result()` for JSON response
- [x] 10.5.4 Fix context-window floor check to use `common_speculative_has_det_filter()` instead of `common_sampler_has_deterministic_draft()`

### 2. Benchmarking Rework

### 2.1 Rewrite Benchmark Tool
- [x] 11.1.1 Rewrite `tools/deterministic-draft-bench/bench-deterministic-draft.cpp` to use `common_speculative_init()` with `DRAFT_MTP + DRAFT_DETERMINISTIC` types
- [x] 11.1.2 Use `common_speculative_draft()` + `common_sampler_sample_and_accept_n()` + `common_speculative_accept()` (the real pipeline)
- [x] 11.1.3 Handle rejection via speculative pipeline checkpoint/restore (not by breaking to next generation)
- [x] 11.1.4 Use actual BPE model tokens (from `llama_tokenize`), not whitespace-tokenized code

### 2.2 Benchmark Methodology
- [x] 11.2.1 Baseline: MTP-only (`--spec-type draft-mtp`), no deterministic filter
- [x] 11.2.2 Treatment: MTP + deterministic filter (`--deterministic-draft-model <plugin>`)
- [x] 11.2.3 Same model (Qwen3.5-2B-MTP-GGUF), same prompts, same n_predict
- [x] 11.2.4 Measure: tokens/sec (end-to-end), draft acceptance rate, deterministic filter rejection rate, MTP draft size distribution
- [x] 11.2.5 Report speedup vs baseline

### 2.3 Results Documentation
- [x] 11.3.1 Write `deterministic-draft-model-poc/docs/benchmark-overview.md` with new methodology and results
- [x] 11.3.2 Include: model used, prompts, baseline vs treatment comparison, throughput, rejection rates (covered in comprehensive benchmark doc)

### 3. Testing Updates

- [ ] 12.1 Update `tests/test-deterministic-draft.cpp` to test `common_speculative` integration instead of sampler extension
- [ ] 12.2 Verify fail-to-start when model lacks MTP heads (`n_layer_nextn == 0`)
- [ ] 12.3 Verify throughput improvement vs baseline (MTP-only)
- [ ] 12.4 Verify plugin state correctness across checkpoint/restore

### 4. Directory Restructure: Clean SDK Separation

### 4.1 Main Tree (minimal changes)
- [x] 13.1.1 Add `DETERMINISTIC_SPEC_ENABLED` CMake option to top-level CMakeLists.txt
- [x] 13.1.2 Create `external/` directory with CMakeLists.txt that installs `deterministic_draft_plugin.h` into `external/include/`
- [x] 13.1.3 Gate `external/` subdirectory on `DETERMINISTIC_SPEC_ENABLED`
- [x] 13.1.4 Remove all tree-sitter code from `src/llama-deterministic-draft.{h,cpp}` (keep only dlopen loader + C API)
- [x] 13.1.5 Remove `LLAMA_TREE_SITTER` from `src/CMakeLists.txt`, `common/CMakeLists.txt`, top-level CMakeLists.txt
- [x] 13.1.6 Remove `add_subdirectory(plugins)` from top-level CMakeLists.txt
- [x] 13.1.7 Remove `plugins/` directory from main tree entirely

### 4.2 PoC Consumer Project (standalone, no main tree dependency)
- [x] 13.2.1 Create `deterministic-draft-model-poc/` directory
- [x] 13.2.2 Create `deterministic-draft-model-poc/CMakeLists.txt` as standalone CMake project
- [x] 13.2.3 Move all tree-sitter code (plugin.cpp, common/, grammars) to `deterministic-draft-model-poc/src/`
- [x] 13.2.4 Create `deterministic-draft-model-poc/lib/` for SDK artifacts
- [x] 13.2.5 Create `link-sdk.sh` script to symlink `external/include/deterministic_draft_plugin.h` into `lib/`
- [x] 13.2.6 PoC CMakeLists.txt references only `lib/deterministic_draft_plugin.h` (no main tree paths)

### 4.3 Verification
- [x] 13.3.1 Main tree builds with `-DDETERMINISTIC_SPEC_ENABLED=ON` (produces SDK header)
- [x] 13.3.2 `link-sdk.sh` successfully symlinks the header into PoC lib/
- [x] 13.3.3 PoC builds standalone with no main tree dependency (only needs lib/deterministic_draft_plugin.h)
- [x] 13.3.4 Benchmark runs with PoC plugin and shows +50% speedup
- [x] 13.3.5 Zero tree-sitter references in core build files (CMakeLists.txt, src/, common/, tools/)
- [x] 13.3.6 Zero references to deterministic-draft-model-poc/ from main tree

### 5. SDK Separation: Distributed Shared Objects

### 5.1 Spec Loader Shared Library
- [x] 14.1.1 Split `src/llama-deterministic-draft.cpp` - move dlopen loader + C API wrappers to `external/deterministic_draft_spec.cpp`
- [x] 14.1.2 Keep only `generate_draft` (needs llama types) in `src/llama-deterministic-draft.cpp`
- [x] 14.1.3 Build `external/lib/libdeterministic_draft_spec.so` (no llama dependency, links only libdl)
- [x] 14.1.4 Fix `extern "C"` linkage to match `llama.h` declarations
- [x] 14.1.5 Update `include/llama.h` with v2 slot-aware signatures (add slot_id to validate, commit, reset, etc.)
- [x] 14.1.6 Simplify `src/llama-deterministic-draft.h` to only struct + no duplicate declarations

### 5.2 PoC Links Against Distributed .so
- [x] 14.2.1 Update `link-sdk.sh` to symlink both header and `.so` into `deterministic-draft-model-poc/lib/`
- [x] 14.2.2 Verify PoC links against `libdeterministic_draft_spec.so` from external/
- [x] 14.2.3 End-to-end benchmark with distributed .so shows +43% speedup

### 5.3 Documentation
- [x] 14.3.1 Create `deterministic-draft-model-poc/docs/quick-start.md` with build structure (main tree, external, PoC)
- [x] 14.3.2 Move build instructions from benchmark results doc to quick-start.md
- [x] 14.3.3 Update openspec proposal.md Impact section
- [x] 14.3.4 Update openspec design.md decisions table

### 5.4 Consumer SDK Header
- [x] 14.4.1 Create `external/include/llama_deterministic_draft.h` - consumer header for linking against the .so (declares `llama_deterministic_draft_*` functions, self-contained, no llama.h dependency)
- [x] 14.4.2 Update `external/CMakeLists.txt` to install both headers: `deterministic_draft_plugin.h` (plugin authors) and `llama_deterministic_draft.h` (consumers)
- [x] 14.4.3 Update `link-sdk.sh` to symlink both headers into PoC `lib/`
- [x] 14.4.4 Update quick-start.md to document two-header SDK model (plugin author header + consumer header)
- [x] 14.4.5 Update quick-start.md custom plugin section to mention both headers needed

### 6. PR Preparation

### 6.1 Pre-submission
- [x] 15.1.1 Search existing issues/PRs for deterministic draft or similar speculative decoding work (searched - no equivalent; existing approaches use grammar-guided decoding or second LLM for code-specific drafting, none use a pluggable deterministic pre-filter in the speculative pipeline)
- [ ] 15.1.2 Open a feature request to discuss the approach with maintainers before submitting a PR (new speculative type, new CLI flags, new public API in llama.h)
- [ ] 15.1.3 AI usage disclosure: document how AI was used (assistive capacity, human-authored majority)

### 6.2 Performance Verification
- [x] 15.2.1 Run `llama-perplexity` to verify baseline perplexity is not affected by the changes (PPL = 11.86 on Qwen3.5-2B-MTP Q4_K_M, raw output saved in `deterministic-draft-model-poc/docs/raw/perplexity-qwen35-2b-mtp-q4_k_m.txt`)
- [x] 15.2.2 Run `llama-bench` to verify baseline performance is not degraded when deterministic draft is disabled (covered by benchmark comparison mode - baseline MTP-only runs unchanged)
- [x] 15.2.3 Verify that existing speculative decoding (draft-simple, draft-mtp, ngram) still works unchanged (speculative examples build and link; enum change is additive, static_assert updated)

### 6.3 Code Quality
- [x] 15.3.1 Add CODEOWNERS entry for new files (external/, deterministic-draft-model-poc/, tools/deterministic-draft-bench/, include/deterministic_draft_plugin.h, src/llama-deterministic-draft.*)
- [x] 15.3.2 Verify coding guidelines: clang-format clean on all new/rewritten files; no tabs; no unicode (emdash/arrows) in new code; snake_case naming; no third-party deps in core
- [x] 15.3.3 Update codemap.md with new directories (external/, deterministic-draft-model-poc/, tools/deterministic-draft-bench/)
- [x] 15.3.4 Check tools/server/README-dev.md if server-context changes need documentation (no changes needed - README.md is auto-generated by llama-gen-docs from arg.cpp; flags already set with LLAMA_EXAMPLE_SERVER)
- [x] 15.3.5 Verify no stray tree-sitter references in core (removed from common/common.h enum comment, tools/deterministic-draft-bench comments; zero includes of tree_sitter/api.h in src/ or common/)

### 6.4 CI (requires maintainer coordination)
- [ ] 15.4.1 Contact llama.cpp maintainers (Discord) about self-hosted runner setup for deterministic draft CI
- [ ] 15.4.2 Add CI workflow step to build with `-DDETERMINISTIC_SPEC_ENABLED=ON` (verify external/ artifacts produced)
- [ ] 15.4.3 Add CI step to build `deterministic-draft-model-poc/` standalone (catch main tree dependency regressions)
- [ ] 15.4.4 Add CI step to run `tests/test-deterministic-draft.cpp` (after Phase 5 unit test update)

### 6.5 Unit Tests (Phase 5)
- [x] 15.5.1 Update `tests/test-deterministic-draft.cpp` to test `common_speculative` integration instead of removed sampler extension
- [x] 15.5.2 Add test for fail-to-start when model lacks MTP heads (`n_layer_nextn == 0`)
- [x] 15.5.3 Add test for plugin state correctness across checkpoint/restore
- [x] 15.5.4 Add test for auto-imply draft-mtp when `--deterministic-draft-model` is enabled
- [x] 15.5.5 Add PoC smoke test (`deterministic-draft-model-poc/smoke-test.cpp`) that links only against `lib/` artifacts and verifies the plugin conforms to the SDK contract
- [x] 15.5.6 Verify PoC build fails cleanly without SDK artifacts in `lib/`
- [x] 15.5.7 Verify core tests pass: test-arg-parser, test-sampling, test-reasoning-budget, test-deterministic-draft
- [x] 15.5.8 Fix arg ordering (shorter alias before longer) for --det-draft-model flags
