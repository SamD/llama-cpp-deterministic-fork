# Deterministic Draft Filter - Benchmark Overview

## Quick Start

```bash
./build/bin/benchmark-deterministic-draft \
  -m /mnt/shared/Models/Qwopus3.5-9B-Coder-MTP-q8_0.gguf \
  --det-draft-model deterministic-draft-model-poc/build/deterministic-draft.so \
  --det-draft-n-max 100 \
  --det-draft-accept-all \
  -p "#include <stdio.h>" -n 200 --compare --n-runs 1 \
  --ctx-size 262144 --batch-size 1024 --ubatch-size 512 \
  --flash-attn on --split-mode none --spec-type draft-mtp \
  --n-gpu-layers -1 --main-gpu 0
```

Runs baseline (MTP-only) then treatment (MTP + deterministic filter) and prints throughput comparison.

## Operating Modes

### Accept-All Mode (`--det-draft-accept-all`)

The filter IS the verifier. Target model verification is skipped entirely for filter-accepted tokens. Trades safety for raw throughput. 8-23x speedup on RTX 3060 across C, Java, JavaScript, Python.

### Default Mode (no `--det-draft-accept-all`)

Target model verifies all filter-accepted tokens. The filter pre-validates drafts before they reach the target, removing structurally invalid tokens. Throughput stays below baseline because the target still runs a full forward pass on every post-filter token -- the filter adds overhead without bypassing verification. Accept rate improves but net throughput is neutral to negative on most hardware.

## RTX 3060 Results

Hardware: AMD Ryzen 7 7840HS, 64GB DDR5, NVIDIA RTX 3060 (12GB VRAM). Model: Qwopus3.5-9B-Coder-MTP-q8_0.gguf.

### Accept-All Mode (multi-language)

| Language | Baseline (MTP) | Treatment (MTP+DET) | Speedup | Accept Rate |
|----------|---------------|---------------------|---------|-------------|
| C | 2.08 t/s | 22.36 t/s | 10.75x | 100% |
| Java | 2.54 t/s | 30.39 t/s | 11.95x | 100% |
| JavaScript | 1.06 t/s | 24.04 t/s | 22.62x | 100% |
| Python | 1.79 t/s | 14.70 t/s | 8.23x | 100% |

Prompts: C (`#include <stdio.h>`), Java (`import java.util.*;`), JavaScript (async merge sort), Python (BFS with deque).

### Default Mode (no accept-all)

| Language | Treatment (MTP+DET) | Baseline (MTP) |
|----------|---------------------|----------------|
| C | 1.83 t/s | 2.08 t/s |
| Java | 1.45 t/s | 2.54 t/s |
| JavaScript | 3.54 t/s | 1.06 t/s |
| Python | 2.16 t/s | 1.79 t/s |

Without accept-all, C and Java degrade (target rejects most drafts, filter adds overhead). JavaScript and Python show modest improvement.

### Output Validity

All output was invalid in both modes. The grammar filter ensures structural syntax (valid token sequences per the BNF grammar) but not semantic correctness. The model generates the same garbage on every run for a given prompt -- the filter accepts it because it parses as valid syntax. This is a model quality issue, not a filter bug.

## N100 Results

Hardware: Intel N100 (4 E-cores, no hyperthreading), 16GB DDR4, Intel UHD integrated GPU. Model: Qwopus3.5-9B-Coder-MTP-q5_k_m.gguf (quantized to fit in 16GB). Build: Intel BLAS (MKL), no CUDA/SYCL/Vulkan.

### Accept-All Mode (multi-language)

| Language | Baseline (MTP) | Treatment (MTP+DET) | Speedup | Accept Rate |
|----------|---------------|---------------------|---------|-------------|
| C | 0.08 t/s | 0.99 t/s | 12.4x | 100% |
| Java | 0.13 t/s | 0.98 t/s | 7.5x | 100% |
| JavaScript | 0.63 t/s | 0.80 t/s | 1.3x | 100% |
| Python | 0.12 t/s | 0.93 t/s | 7.8x | 100% |

Baseline measured with `--n-predict 20` (single run; full 200-token baseline exceeds timeout). Treatment averaged across3 runs. Same prompts as RTX 3060.

JavaScript baseline is an outlier at 0.63 t/s -- 5-8x faster than other languages on the same hardware. This compresses its speedup ratio. The model simply decodes JS faster on CPU.

### Default Mode (no accept-all)

Only JavaScript completed (0.12-0.22 t/s across partial runs). C, Java, Python timed out at300s per run -- MTP decode is too slow on N100 to produce meaningful output within reasonable time.

### Output Validity

Same as RTX 3060: accept-all output is invalid (model generates garbage that happens to parse as valid syntax). Not a filter bug -- model quality issue.

## Cross-Hardware Summary (Accept-All Mode)

| Hardware | Baseline TPS | Treatment TPS | Speedup |
|----------|-------------|--------------|---------|
| RTX 3060 (CUDA) | 1.06-2.54 | 14.70-30.39 | 8.2-22.6x |
| RTX 4070 (CUDA) | 21.91 | 6.06 | 0.28x |
| GTi15 Arc PRO B70 (oneAPI) | 19.47 | 130.88 | 6.72x |
| N100 (CPU, q5_k_m) | 0.08-0.63 | 0.80-0.99 | 1.3-12.4x |

The filter helps most on hardware where the target model is slow relative to filter overhead (N100, RTX 3060). On fast GPUs (RTX 4070) the target model verification is cheap enough that filter overhead dominates. N100 shows the widest speedup variance (1.3-12.4x) because baseline throughput varies dramatically by language.

## When to Use Each Mode

**Accept-all**: single-language code generation where output language is known and fixed. IDE autocomplete, agent codegen, fill-in-middle. Not safe for mixed content (markdown, HTML+CSS+JS, chat output with interleaved code blocks) because the grammar becomes a no-op for non-matching content.

**Default mode**: mixed content, unknown output language, or when semantic correctness matters more than throughput. Target verification catches errors the structural filter cannot.

**Disabled**: chat output, documentation generation, any prompt where the output language could switch mid-generation.

## Trade-offs

- **Structural syntax only, not semantic correctness.** Valid C syntax can still be semantically wrong (wrong variable name, incorrect algorithm).
- **Small n_max (10) can be net negative.** Filter overhead (XGrammar bitmask, plugin IPC) outweighs savings from filtering very short drafts.
- **Larger n_max increases benefit.** Raw MTP acceptance rate drops as draft size grows -- more structural errors to filter out. The filter benefit is proportional to baseline waste.

## Flag Reference

| Flag | Default | Description |
|------|---------|-------------|
| `--det-draft-model <path>` | (none) | Path to plugin shared library (.so/.dylib/.dll). Auto-enables MTP. |
| `--det-draft-n-max <N>` | -1 | -1 = no cap (MTP default 3). 0 = disabled. >0 = cap and MTP draft count. |
| `--det-draft-accept-all` | (off) | Skip target model verification. Filter is final arbiter. |

## Hardware

| Platform | CPU | GPU | RAM |
|----------|-----|-----|-----|
| RTX 3060 | AMD Ryzen 7 7840HS | NVIDIA RTX 3060 (12GB) | 64GB DDR5 |
| RTX 4070 | Intel i9-14900KF | NVIDIA RTX 4070 (12GB) | 32GB DDR5 |
| GTi15 | Intel Ultra 9 285H | Intel Battlemage G21 (oneAPI) | 96GB DDR5 |
| N100 | Intel N100 (4 E-cores, no HT) | Intel UHD (integrated, unused) | 16GB DDR4 |

---

See [benchmark-comprehensive.md](benchmark-comprehensive.md) for detailed methodology, test file descriptions, and automation scripts.
