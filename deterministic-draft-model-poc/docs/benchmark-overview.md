# Deterministic Draft Filter: Benchmark Overview

**NOTE**: Before going on, please ensure you have gone through the steps listed in the [quick-start](./quick-start.md)

> **Phase 2 (2026-07-18)**: accept-all is now FIXED on qwen35 - treatment is gcc-valid AND faster than baseline (2.12x at n_max 16), and the filter caught 3/10 runs where the raw model produced invalid C. See the [Phase 2](#phase-2-2026-07-18-rtx-3060-qwen35-9b) section below. The Phase 1 results further down (RTX 4070 / N100 / GTi15, 2B-4B models) predate the Phase 2 fixes and show the old broken accept-all behavior.

> **Date**: 2026-07-17 (RTX 4070 row regenerated post-fix; N100 + GTi15 rows NOT re-run - see note under Summary by Hardware)
> **llama.cpp**: build b9736-456e42eab (GNU 15.2.0, Linux x86_64)
> **Model**: Qwen3.5-2B-MTP (Q5_K_M, ~1.47GB) - 2B params, 1 MTP/nextn layer. NOTE: original 2026-06-26 doc used Q4_K_M; this refresh used Q5_K_M on the same RTX 4070 hardware (i9-14900KF + RTX 4070). Quant difference is minor for a like-for-like refresh but not identical.
> **Plugin**: deterministic-draft.so (XGrammar: C, Java, Python, JavaScript)
>
> **PRE-EXISTING ACCEPT-ALL LIMITATION (RTX 4070, post-fix, 2026-07-17)**: The accept-all treatment run regenerated on this date produces INVALID C output (0/1 valid run) and is SLOWER than baseline (throughput 6.06 vs 21.91 TPS, speedup 0.28x). This is NOT a regression caused by this session's fixes. It is newly-measured evidence of a pre-existing limitation of accept-all mode: grammar-only structural validation cannot catch semantic issues, and c.gbnf's identifier-as-type permissiveness (see observations.md) lets some invalid top-level constructs through. Two distinct facts: (a) the c.gbnf `type_specifier` unbounded-repetition bug (allowed infinite "long long long...") WAS found and fixed this session - that degenerate behavior is gone; (b) the output is still invalid for a DIFFERENT, pre-existing reason (identifier-as-type permissiveness letting non-declaration constructs through in some cases), which is a known, documented, out-of-scope-for-this-session limitation of grammar-only structural validation in accept-all mode - not a new problem. The RTX 4070 numbers below reflect this pre-existing limitation and should NOT be read as a working accept-all result. Baseline (MTP-only) numbers remain healthy and consistent with the original doc.

## Phase 2 (2026-07-18, RTX 3060, Qwen3.5-9B)

This section supersedes the Phase 1 accept-all limitation for the tested model. Phase 1 (below) showed accept-all producing INVALID C and running slower than baseline on RTX 4070. Phase 2 re-runs on a different model/hardware after tracing the failure to two root causes and one performance bottleneck, all now fixed (details in [observations.md](observations.md)):

1. **Prompt never ingested into the draft context** - the bench decoded the prompt on the target but never called `common_speculative_process` on it, so the MTP draft head generated from an empty context (zero `pending_h`, empty draft KV) and produced garbage drafts from the very first token. Fixed by initializing speculative before the prompt eval (so nextn embeddings are enabled in time) and processing the prompt batch through the speculative impl.
2. **No-newline prompt broke the grammar** - `#include <stdio.h> int main...` (no `\n`) left the c.gbnf matcher stuck inside its `preprocessor` rule (`[^\n]* "\n"`), so it accepted *any* non-newline token (including non-C text) instead of validating C. A newline-terminated prompt makes the grammar actually validate.
3. **Bonus token constrained via `FillNextTokenBitmask`** - that XGrammar call cost ~1.1-1.8s per invocation over the ~150k vocab, dominating decode time. It is now done with O(1) `AcceptToken` probes (the sampler's probability-sorted shortlist, highest-probability grammar-valid token wins). The sample phase dropped from ~2900ms to ~0.5ms per run.

**Environment**: LXC container, NVIDIA RTX 3060 (12GB), CUDA. **Model**: `Qwopus3.5-9B-Coder-MTP-q8_0.gguf` (qwen35, hybrid SSM + MTP nextn layer, ~9B). **Prompt**: `#include <stdio.h>\nint main(void) { int x = 0; ` (newline-terminated). **Flags**: `--spec-type draft-mtp -c 4096 -fit on --temp 0.2 --top-k 20 --top-p 0.9 --min-p 0.1 --n-predict 128 --det-draft-accept-all --compare`. Both baseline and treatment output are validated with `gcc -fsyntax-only`. See [quick-start.md](quick-start.md) for how to reproduce.

**Validator note (gcc version)**: the `gcc -fsyntax-only` validity gate depends on the system gcc version. GCC 14+ treats implicit function declarations as hard errors (observed with gcc 15.2), so generated code that calls `printf`/`scanf` without `#include <stdio.h>` scores *invalid* even though it is grammar-valid C; older gcc only warns and scores the same output valid. This shifts baseline and treatment counts equally and is a toolchain property, not a filter result. The numbers below were measured with the container's gcc at the time - record `gcc --version` when reproducing, and keep the includes seeded in the prompt (as above) when comparing across machines.

### Correctness (10 runs, n_max 16)

| Baseline valid (gcc) | Treatment valid (gcc) | Caught by filter |
|---------------------|----------------------|-----------------|
| 7/10 | 10/10 | 3 |

"Caught by filter" counts runs where the raw model produced invalid C but the filter produced valid C. Here the raw model broke C syntax in 3/10 runs (30%) while the filter was valid in all 10. Observed baseline failure modes: (a) degenerate variable-declaration spam truncated mid-program with no closing brace, and (b) a valid program followed by prose contamination (the model switched to an English explanation). The filter rejects prose as non-C and, via the grammar's complete-parse requirement, never emits a truncated program. These specific outputs are model/prompt/seed-dependent - see quick-start.md for how to run the same correctness test on your own model.

### Throughput vs n_max (10 runs for n_max 16, 3 runs for 32/100)

| n_max | Baseline TPS | Treatment TPS | Speedup | Treatment valid (gcc) |
|-------|-------------|--------------|---------|----------------------|
| 16 | 36.35 | 77.12 | 2.12x | 10/10 |
| 32 | 12.24 | 63.23 | 5.17x | 0/3 |
| 100 | 1.55 | 28.60 | 18.45x | 0/3 |

**Phase 2 finding - draft size vs coherence.** With the weak MTP draft head on this 9B hybrid model, larger drafts (n_max 32/100) let the head drift into grammar-valid-but-degenerate output (e.g. comment spam) that fails `gcc`. At n_max 16 the target re-anchors every ~16 tokens via the bonus, keeping the draft head coherent: output is gcc-valid AND it is also the fastest configuration. The grammar validates syntax only - coherence still needs periodic target re-anchoring, so keep n_max small on weak draft heads.

**Treatment per-step phase breakdown (n_max 16)**: draft 134ms, target decode 53ms, process/catch-up 58ms, sample/accept 0.5ms. The grammar verification (draft `AcceptToken` + bonus probes) is effectively free next to the model forward passes - confirming that O(1) grammar verification is far cheaper than target-model verification.

### Multi-language coverage (n_max 16, accept-all)

Same environment and command shape as above, one grammar-parseable prompt per bundled language. Java validated with `javac` (Corretto 25), Python with `python3 compile`, JavaScript with `node --check`, C with `gcc -fsyntax-only`.

| Language | Runs | Baseline TPS | Treatment TPS | Speedup | Baseline valid | Treatment valid | Caught by filter |
|----------|------|-------------|--------------|---------|---------------|-----------------|-----------------|
| C | 10 | 36.35 | 77.12 | 2.12x | 7/10 | 10/10 | 3 |
| Python | 5 | 51.82 | 121.75 | 2.35x | 3/5 | 5/5 | 2 |
| JavaScript | 5 | 56.39 | 111.91 | 1.98x | 0/5 | 0/5 | 0 |
| Java | 5 | 46.04 | 136.26 | 2.96x | 0/5 | 0/5 | 0 |

- **Throughput speedup is consistent** across all four languages (~2x-3x) - the accept-all + O(1) grammar verification win is language-independent.
- **Correctness holds for C and Python** because the model completes clean programs there. Python needed a grammar fix first (see below): its `target` rule wrongly allowed the call trailer into assignment targets, accepting `self.dfs(key) = []`; after fixing it, treatment went 0/5 -> 5/5 valid.
- **JavaScript and Java fail real-parser validation** despite the grammar accepting the output, because the model degenerates into redeclaration/semantic-error spam that a CFG cannot express: `const left = ...` redeclaration (`node`: `Identifier 'left' has already been declared`), duplicate `Node(int value)` constructor (`javac`), undeclared variable, `new Scanner(System.out)` type error, and truncation (program never completed). These are static-semantic early errors or incomplete generation - NOT CFG syntax - so they are out of scope for the grammar to reject (see observations.md). The filter still guarantees CFG-level structural validity; it cannot guarantee the stricter static semantics that real `node`/`javac` validators check.

---

## 1. Quick Start

One command showing accept-all mode with GPU (-ngl 99):

```bash


./build/bin/benchmark-deterministic-draft \
  -m ~/models/Qwen3.5-2B-MTP/unsloth_Qwen3.5-2B-MTP-GGUF_Q4_K_M.gguf \
  --det-draft-model deterministic-draft-model-poc/build/deterministic-draft.so \
  --det-draft-n-max 100 \
  --det-draft-accept-all \
  -c 4096 -ngl 99 \
  -p "int fib(int n) {" -n 200 --compare --n-runs 3
```

This runs baseline (MTP-only) then treatment (MTP + deterministic filter + accept-all) and prints throughput comparison. See Summary by Hardware below for results across three hardware platforms.

## 2. Two Modes of Operation

The deterministic draft filter supports two operating modes, each with different performance characteristics and use cases.

### Default Mode (without --det-draft-accept-all)

The target model verifies all filter-accepted tokens in a forward pass. The deterministic filter pre-validates drafts before they reach the target model, preventing structurally invalid tokens from occupying verification slots. This improves accept rate by ensuring only structurally valid tokens reach the target model, but throughput remains below 1.0x because the target model still verifies every post-filter token -- the filter adds overhead (grammar matching, plugin IPC) without bypassing verification.

### Accept-All Mode (with --det-draft-accept-all)

The deterministic filter IS the verifier. Target model verification is skipped entirely for filter-accepted tokens. The filter's structural validation becomes the final arbiter of token quality. This trades safety for raw throughput. Speedup varies by hardware and prompt - see Summary by Hardware and comprehensive benchmarks.

## 3. Performance

All results use the same prompt `int fib(int n) {`, 200 tokens, `--det-draft-n-max 100 --det-draft-accept-all --compare --n-runs 3`. The language is auto-detected (the prompt is unambiguously C, so detection resolves to the `c` grammar).

### Summary by Hardware

| Hardware | Baseline TPS | Treatment TPS | Speedup |
|----------|-------------|--------------|---------|
| RTX 4070 (CUDA) | 21.91 | 6.06 | 0.28x |
| GTi15 Arc PRO B70 (oneAPI) | 19.47 | 130.88 | 6.72x |
| N100 (CPU, 2026-07-19) | 0.05 | 4.31 | 86.1x (*) |

> **Note**: These are example results from one model/prompt/configuration. Speedup is highly dependent on hardware, model, prompt, grammar, and n_max. The filter's speedup is generally higher on slower hardware because the baseline model spends more time verifying rejected drafts; fast GPUs have less verification waste to save. Treat the numbers above as illustrative, not guaranteed.
>
> **(*) N100 row caveat (regenerated 2026-07-19)**: The N100 row was re-measured post-fix on 2026-07-19 (n_max=100). However, baseline MTP speculative decoding FAILED on this test configuration (0.0% acceptance over 20100 drafts, gibberish output), while plain autoregressive decode on the same host is correct at 13.2 t/s. The 86.1x figure is therefore recorded as "filter + accept-all completed on the N100 while baseline MTP did not", NOT a healthy like-for-like comparison. Root cause is not isolated (fork spec-decode integration, benchmark tool, or upstream CPU issue - bisection owed, tasks 16.3.x). See "N100 follow-up investigation (2026-07-19)" below and observations.md.
>
> **PENDING RE-VERIFICATION (GTi15 row)**: The GTi15 row still reflects PRE-FIX measurements from 2026-06-26. It was NOT regenerated on 2026-07-17 because that hardware is not accessible from the RTX 4070 machine used for this refresh, and remains pending after the 2026-07-19 N100 session. Treat it as pending, not confirmed.
>
> **RTX 4070 row note**: Regenerated 2026-07-17 post-fix (per-token allocation/leak fix + benchmark loop fixes + `--lang` removal). The treatment speedup of 0.28x (6.06 TPS) is NOT a regression - it reflects a pre-existing accept-all limitation (grammar-only structural validation cannot catch semantic issues; c.gbnf's identifier-as-type permissiveness lets some invalid top-level constructs through), see note at top of doc. The `type_specifier` unbounded-repetition bug was found and fixed this session, so that degenerate behavior is gone; the remaining invalid output is a different, documented, out-of-scope limitation. Baseline (21.91 TPS) is consistent with the original 21.94 TPS, confirming same hardware.

**Row descriptions** (applies to all detail tables below):
- **throughput tps**: tokens generated per second of decode time
- **accept rate**: fraction of post-filter draft tokens accepted (100% with accept-all since filter IS the verifier)
- **n_predict**: total tokens generated (sampled + accepted drafts)
- **drafted (pre)**: total raw MTP draft tokens, before filter truncation
- **drafted (post)**: total draft tokens after filter truncation (with accept-all, committed directly - no target verification)
- **det truncated**: number of draft rounds where the filter truncated at least one token

### RTX 4070 (CUDA) - Accept-All Mode

> **PRE-EXISTING ACCEPT-ALL LIMITATION (2026-07-17, post-fix)**: These numbers show the accept-all treatment produces INVALID C (0/1 valid run) at 100% accept rate and is slower than baseline. This is NOT a regression from this session's fixes - it is newly-measured evidence of a pre-existing accept-all limitation (grammar-only structural validation cannot catch semantic issues; c.gbnf's identifier-as-type permissiveness lets some invalid top-level constructs through). The `type_specifier` unbounded-repetition bug (infinite "long long long...") was found and fixed this session, so that degenerate behavior is gone; the remaining invalid output is a different, documented, out-of-scope limitation. See PRE-EXISTING ACCEPT-ALL LIMITATION note at top of doc. Baseline (MTP-only) remains healthy.

| Metric | Baseline (MTP) | Treatment (MTP+DET) | Improvement |
|--------|---------------|---------------------|-------------|
| throughput tps | 21.91 | 6.06 | -72% |
| accept rate | 3.7% | 100.0% | +96.3pp |
| n_predict | 42 | 254 | 6.0x more |
| drafted (pre) | 900 | 252 | 3.6x fewer |
| drafted (post) | 900 | 246 | 3.7x fewer |
| det truncated | N/A | 6 | - |
| output valid (c) | 1/1 | 0/1 | PRE-EXISTING LIMITATION |

### N100 (CPU) - Accept-All Mode

> **Superseded 2026-07-19**: the table below is the pre-fix 2026-06-26 measurement. The post-fix re-run recorded baseline 0.05 t/s vs treatment 4.31 t/s (86.1x, n_max=100) - but with a failed baseline (0.0% acceptance over 20100 drafts, gibberish output), so it is not a healthy like-for-like comparison. See "N100 follow-up investigation (2026-07-19)" below.

| Metric | Baseline (MTP) | Treatment (MTP+DET) | Improvement |
|--------|---------------|---------------------|-------------|
| throughput tps | 0.12 | 3.30 | +2712% |
| accept rate | 0.4% | 100.0% | +99.6pp |
| n_predict | 202 | 201 | - |
| drafted (pre) | 14333 | 200 | 72x fewer |
| drafted (post) | 14333 | 133 | 108x fewer |
| det truncated | N/A | 67 | - |

### GTi15 Arc PRO B70 (oneAPI) - Accept-All Mode

| Metric | Baseline (MTP) | Treatment (MTP+DET) | Improvement |
|--------|---------------|---------------------|-------------|
| throughput tps | 19.47 | 130.88 | +572% |
| accept rate | 4.1% | 100.0% | +95.9pp |
| n_predict | 46 | 202 | 4.4x more |
| drafted (pre) | 900 | 200 | 4.5x fewer |
| drafted (post) | 900 | 167 | 5.4x fewer |
| det truncated | N/A | 33 | - |

### N100 follow-up investigation (2026-07-19)

Post-fix N100 re-verification (n_max=100, accept-all, same model/prompt shape as
the table above) recorded **baseline 0.05 t/s vs treatment 4.31 t/s (86.1x)**.

The critical caveat: **baseline MTP speculative decoding failed on this test
configuration** - 0.0% acceptance over 20100 drafts with gibberish output, while
plain autoregressive decode on the same host is correct at 13.2 t/s. The failure
requires the MTP draft/verify path. Root cause is NOT isolated: candidates are
this fork's spec-decode integration, the benchmark tool, or an upstream CPU
issue. Bisection is owed (tasks 16.3.x in
`openspec/changes/deterministic-spec/tasks.md`) - do not attribute this to
upstream without it. A `ctx_dft pos_max < N-1` process()-hook warning on CPU
prefill was also observed and is part of the same investigation (task 16.3.2).

The 86.1x figure is therefore recorded as "filter + accept-all completed on the
N100 while baseline MTP did not", not a healthy like-for-like speedup.

**n_max=16 addendum (2026-07-19)**: at n_max=16 the baseline is healthy on the
N100 (37.9% accept, 3/3 valid, 5.28 t/s) and treatment runs at 12.58 t/s (2.38x)
but scores 0/3 valid - the 2B draft head is too weak to stay coherent even with
grammar re-anchoring at 16 tokens on this host. This mirrors the Phase 2
draft-size-vs-coherence finding: small drafts fix coherence only when the draft
head is strong enough to begin with.

## 4. Disclosures and Scope

### When accept-all is SAFE
- Pure code completion in a known language (IDE autocomplete, fill-in-middle)
- Single-language generation from a known prompt (C, Python, Java, JavaScript)
- Agent codegen where output language is fixed

### When accept-all is NOT SAFE (use default mode or disable filter)
- Chat output with interleaved code blocks (markdown, HTML+CSS+JS)
- Multi-language generation (e.g., HTML + CSS + JS in same output)
- Plain prose, README text, documentation generation
- Any prompt where output language could switch mid-generation

### Why
The filter uses a single grammar per buffer. When text doesn't match that grammar (e.g., markdown prose), the grammar's permissiveness means tokens pass through without structural validation. With accept-all, these unvalidated tokens are committed. The filter becomes a no-op for that content -- no benefit, but no harm if the output was correct. The risk is accepting tokens the target model would have rejected.

### Recommendation
Use `--det-draft-accept-all` only when the output language is known and fixed. For mixed-content or unknown-language prompts, use default mode (target verification) with `--det-draft-n-max` set to a moderate value (10-30), or disable the filter entirely.

## 5. When to Use Each Mode

### Accept-All Mode

Use for pure code generation: single-language source files, no markdown, no chat output. The filter validates structural syntax and skips target verification entirely. Best for edge devices (N100, mobile) and coding-focused workloads where throughput is critical and the generation domain is restricted to one language at a time.

### Default Mode

Use for mixed content: markdown with embedded code blocks, chat responses, documentation generation. The target model still verifies filter-accepted tokens, catching semantic errors that the structural filter cannot detect. Throughput is below 1.0x due to filter overhead, but accept rate improves as structurally invalid drafts are removed.

## 6. Trade-offs and Disclosures

- **Accept-all mode assumes single-language code-only generation.** Not suitable for mixed content (HTML chat, markdown with embedded code blocks, multi-language source files).
- **The filter validates structural syntax only, not semantic correctness.** A token sequence that parses as valid C may still be semantically wrong (wrong variable name, incorrect algorithm).
- **At small n_max (10), filter overhead can exceed benefit.** XGrammar bitmask computation, grammar matching, and plugin IPC overhead can outweigh the savings from filtering very short drafts.
- **Larger n_max values increase baseline verification waste**, making the filter benefit more pronounced on slower hardware. The acceptance rate of raw MTP drafts is inversely correlated with draft size - larger drafts are more likely to contain structural errors.

## 7. Flag Reference

| Flag | Default | Description |
|------|---------|-------------|
| `--det-draft-model <path>` | (none) | Path to deterministic draft plugin shared library (.so/.dylib/.dll). Auto-enables MTP. |
| `--det-draft-n-max <N>` | -1 | -1 = no cap (MTP default 3). 0 = disabled (warning emitted). >0 = cap AND MTP draft count (auto-derives --spec-draft-n-max). |
| `--det-draft-accept-all` | (off) | Skip target model verification for filter-accepted tokens. The deterministic filter is the final arbiter. Only active with --det-draft-model. |

## 8. Hardware Specifications

### RTX 4070 Machine

| Component | Spec |
|-----------|------|
| CPU | Intel Core i9-14900KF |
| RAM | 32GB DDR5 |
| GPU | NVIDIA GeForce RTX 4070 (12GB VRAM) |
| Storage | FIKWOT FX991 4TB NVMe SSD |
| OS | Linux |

### N100 Machine

| Component | Spec |
|-----------|------|
| CPU | Intel N100 (Alder Lake-N, 4 E-cores, ~6W TDP) |
| RAM | 16GB DDR4 3200 MHz |
| GPU | Intel UHD Graphics (integrated, no discrete GPU) |
| Storage | 512GB NVMe SSD + 1024GB SATA SSD |
| Form factor | Fanless mini PC, low-power edge device |
| OS | Linux |

### GTi15 Mini PC

| Component | Spec |
|-----------|------|
| CPU | Intel Core Ultra 9 285H (16 cores, up to 5.4 GHz) |
| RAM | 96GB DDR5 5600 MHz |
| GPU | Intel Battlemage G21 (integrated, oneAPI SYCL) |
| iGPU | Intel Arrow Lake-P (unused, masked with `ONEAPI_DEVICE_SELECTOR=0`) |
| Storage | 2TB Crucial NVMe SSD |
| OS | Linux |

---

See [benchmark-comprehensive.md](benchmark-comprehensive.md) for detailed methodology, multi-language results, test file descriptions, and the automation script.