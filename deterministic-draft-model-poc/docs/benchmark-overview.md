# Deterministic Draft Filter: Benchmark Overview

**NOTE**: Before covering this be certain to have gone through the steps listed in the [quick-start](./quick-start.md)

> **Date**: 2026-06-26
> **llama.cpp**: build 9733+ (GNU 15.2.0, Linux x86_64) (documented benchmark build; build from source for current version)
> **Model**: unsloth/Qwen3.5-2B-MTP-GGUF (Q4_K_M, ~1.3GB) - 2B params, 1 MTP/nextn layer
> **Plugin**: deterministic-draft.so (XGrammar: C, Java, Python, JavaScript)

## 1. Quick Start

One command showing accept-all mode with GPU (-ngl 99):

```bash


./build/bin/benchmark-deterministic-draft \
  -m ~/models/Qwen3.5-2B-MTP/unsloth_Qwen3.5-2B-MTP-GGUF_Q4_K_M.gguf \
  --det-draft-model deterministic-draft-model-poc/build/deterministic-draft.so \
  --det-draft-n-max 100 \
  --det-draft-accept-all \
  --lang c \
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

All results use the same prompt `int fib(int n) {`, 200 tokens, `--det-draft-n-max 100 --det-draft-accept-all --lang c --compare --n-runs 3`.

### Summary by Hardware

| Hardware | Baseline TPS | Treatment TPS | Speedup |
|----------|-------------|--------------|---------|
| RTX 4070 (CUDA) | 21.94 | 27.30 | 1.24x |
| GTi15 Arc PRO B70 (oneAPI) | 19.47 | 130.88 | 6.72x |
| N100 (CPU) | 0.12 | 3.30 | 28.1x |

> **Note**: These are example results from one model/prompt/configuration. Speedup is highly dependent on hardware, model, prompt, grammar, and n_max. The filter's speedup is generally higher on slower hardware because the baseline model spends more time verifying rejected drafts; fast GPUs have less verification waste to save. Treat the numbers above as illustrative, not guaranteed.

**Row descriptions** (applies to all detail tables below):
- **throughput tps**: tokens generated per second of decode time
- **accept rate**: fraction of post-filter draft tokens accepted (100% with accept-all since filter IS the verifier)
- **n_predict**: total tokens generated (sampled + accepted drafts)
- **drafted (pre)**: total raw MTP draft tokens, before filter truncation
- **drafted (post)**: total draft tokens after filter truncation (with accept-all, committed directly - no target verification)
- **det truncated**: number of draft rounds where the filter truncated at least one token

### RTX 4070 (CUDA) - Accept-All Mode

| Metric | Baseline (MTP) | Treatment (MTP+DET) | Improvement |
|--------|---------------|---------------------|-------------|
| throughput tps | 21.94 | 27.30 | +24% |
| accept rate | 3.6% | 100.0% | +96.4pp |
| n_predict | 43 | 202 | 4.7x more |
| drafted (pre) | 933 | 200 | 4.7x fewer |
| drafted (post) | 933 | 145 | 6.4x fewer |
| det truncated | N/A | 63 | - |

### N100 (CPU) - Accept-All Mode

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
| `--lang <language>` | c | Language for grammar selection (`c`, `java`, `javascript`, `python`). Grammars are bundled alongside the `.so`. Alternatively, set `DETERMINISTIC_DRAFT_LANGUAGE` env var (useful for programmatic/plugin-only use). Override grammar directory with `DETERMINISTIC_DRAFT_GRAMMAR_DIR` env var. |

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
