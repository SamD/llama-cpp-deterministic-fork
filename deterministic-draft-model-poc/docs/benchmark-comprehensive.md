# Deterministic Draft Filter: Comprehensive Multi-Language Benchmark

**Status: XGrammar-based (bitmask-constrained decoding).** The plugin uses XGrammar's token-level bitmask constraint (`fill_bitmask`/`commit`), not post-hoc text validation. The historical results below (RTX 4070, `Qwen3.5-4B-Coder-MTP-Q4_K_M`) predate this and were produced with the older tree-sitter-based implementation - treat them as illustrative of the methodology, not as current numbers. Re-run section 3.7's automation script against your own model/hardware for current results. For multi-hardware comparison (RTX 4070, GTi15 Arc PRO B70, N100) see [benchmark-overview.md](benchmark-overview.md).

## 1. Prerequisites

### Build the benchmark tool

```bash
cmake -B build -DDETERMINISTIC_SPEC_ENABLED=ON
cmake --build build --target benchmark-deterministic-draft -j$(nproc)
```

### Build the deterministic draft plugin

The plugin is a standalone project under `deterministic-draft-model-poc/`. It builds independently of the main llama.cpp tree. The plugin uses XGrammar for grammar-constrained decoding with jump-forward support.

```bash
cd deterministic-draft-model-poc
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-march=native -O3"
cmake --build build -j$(nproc)
```

Produces: `deterministic-draft-model-poc/build/deterministic-draft.so`

Building also bundles the `grammars/` directory alongside the `.so` (via a CMake `POST_BUILD` step), so the plugin can resolve grammars by language name at runtime (`--lang <language>` on the benchmark tool) without any host-side file path configuration. Grammars are plain `.gbnf` data files - editable without rebuilding the plugin. To use a different grammar bundle location, set `DETERMINISTIC_DRAFT_GRAMMAR_DIR` in the environment before running.

The plugin requires pybind11 for XGrammar's build (fetched automatically via CMake `FetchContent`). If not installed:
```bash
pip install pybind11
```

### Model

Download an MTP-capable model. The examples below use `unsloth/Qwen3.5-2B-MTP-GGUF`:

```bash
huggingface-cli download unsloth/Qwen3.5-2B-MTP-GGUF \
  --include "*Q4_K_M*" \
  --local-dir ~/models/Qwen3.5-2B-MTP
```

Set `MODEL=~/models/Qwen3.5-2B-MTP/unsloth_Qwen3.5-2B-MTP-GGUF_Q4_K_M.gguf`
(or your download path).

### Verify artifacts

```bash
ls external/lib/libdeterministic_draft_spec.so
ls deterministic-draft-model-poc/build/deterministic-draft.so
```

---

## 2. Test Prompts

Test files are in `deterministic-draft-model-poc/tests/` organized by language:

### C (4 files)

| File | Description |
|------|-------------|
| `tests/c/01_quicksort.c.test` | Complete quicksort with partition |
| `tests/c/02_linked_list.c.test` | Singly linked list with insert/delete/search |
| `tests/c/03_hash_table.c.test` | Hash table with chaining |
| `tests/c/04_simple_valid.c.test` | Simple valid C fragment |

### Python (2 files)

| File | Description |
|------|-------------|
| `tests/python/01_graph.py.test` | Graph class with BFS/DFS traversal |
| `tests/python/02_lru_cache.py.test` | LRU cache with ordered dict |

### JavaScript (2 files)

| File | Description |
|------|-------------|
| `tests/javascript/01_async_queue.js.test` | Async task queue with Promises |
| `tests/javascript/02_merge_sort.js.test` | Merge sort implementation |

### Java (2 files)

| File | Description |
|------|-------------|
| `tests/java/01_bst.java.test` | Generic binary search tree |
| `tests/java/02_producer_consumer.java.test` | Producer-consumer with wait/notify |

### Prompt extraction

Each test file has 3 comment header lines that must be stripped:

```
// TEST: <language>
// DESC: <description>
// EXPECT: accept_all
```

After stripping these, the first ~20 lines of actual code are used as the prompt. For example, `01_quicksort.c.test` yields:

```c
#include <stdio.h>

void swap(int *a, int *b) {
    int temp = *a;
    *a = *b;
    *b = temp;
}

int partition(int arr[], int low, int high) {
    int pivot = arr[high];
    int i = low - 1;
    for (int j = low; j < high; j++) {
```

---

## 3. Running Benchmarks

### 3.1 Command template

```bash
./build/bin/benchmark-deterministic-draft \
  -m $MODEL \
  --det-draft-model ./deterministic-draft-model-poc/build/deterministic-draft.so \
  --det-draft-n-max <NMAX> \
  --lang <LANGUAGE> \
  -p "<prompt>" \
  -n 200 \
  -ngl 99 \
  -fa on \
  --compare \
  --n-runs 3 \
  2>&1
```

`--lang` (one of `c`, `java`, `javascript`, `python`) is required - it tells the plugin which bundled grammar to load. Without it, the benchmark defaults to `c` regardless of the prompt's actual language.

Note: when `--det-draft-n-max > 0`, it auto-derives `--spec-draft-n-max`. You do NOT need to set `--spec-draft-n-max` separately. You may still set both if you need explicit control.

### 3.2 Key flags

| Flag | Purpose |
|------|---------|
| `--compare` | Runs baseline (MTP only) then treatment (MTP + det filter). Outputs comparison table and JSON. |
| `--n-runs 3` | 3 runs per mode, averaged. |
| `--det-draft-n-max <N>` | Sets BOTH the filter cap and MTP draft count when > 0 (auto-derives --spec-draft-n-max). -1 = no cap (MTP default of 3). 0 = filter disabled (warning emitted). |
| `--det-draft-accept-all` | Skip target model verification for filter-accepted tokens. Provides speedup proportional to hardware slowness (see Summary by Hardware). See disclosures for scope limitations. |
| `--lang <language>` | **Required.** One of `c`, `java`, `javascript`, `python` - selects the bundled grammar to load. Defaults to `c` if omitted. Alternatively, set `DETERMINISTIC_DRAFT_LANGUAGE` env var (useful for programmatic/plugin-only use). |
| `-n 128` | Number of tokens to predict before stopping. |
| `-ngl 99` | Offload all layers to GPU (0 for CPU-only). |
| `-fa on` | Enable flash attention. |

### 3.3 Single test example

```bash
PROMPT=$(tail -n +4 tests/c/01_quicksort.c.test | head -20)

./build/bin/benchmark-deterministic-draft \
  -m $MODEL \
  --det-draft-model ./deterministic-draft-model-poc/build/deterministic-draft.so \
  --det-draft-n-max 3 \
  --lang c \
  -p "$PROMPT" \
  -n 200 \
  -ngl 99 \
  -fa on \
  --compare \
  --n-runs 3 \
  2>&1
```

### 3.4 Sanity check

A healthy run produces zero "inconsistent sequence positions" errors (KV cache desync bug -- previously fixed, confirmed across all benchmarks):

```bash
./build/bin/benchmark-deterministic-draft ... 2>&1 | grep -c "inconsistent sequence positions"
```

A non-zero count indicates a regression. Zero is expected.

### 3.5 Capturing results

The `--compare` flag produces two outputs. The values below are example formatting only - they come from the older tree-sitter-based implementation and will differ with the current XGrammar plugin. Run the automation script in section 3.7 against your own model/hardware for current numbers.

**Comparison table (stderr):**
```
=== Comparison Results ===

                    Baseline (MTP)    Treatment (MTP+DET)    Improvement
  throughput tps:       17.66               441.10             +2398%
  accept rate:           2.6%               100.0%            +97.4pp
  n_predict:              204                 202
  drafted (pre):         5600                 200             28x fewer
  drafted (post):        5600                 200             28x fewer
  det truncated:          N/A                   0
```

**JSON output (stdout):**
```json
{
  "baseline": {"tps": 17.66, "accept_rate": 2.6, "n_predict": 204, "n_drafted_pre": 5600, "n_drafted_post": 5600},
  "treatment": {"tps": 441.10, "accept_rate": 100.0, "n_predict": 202, "n_drafted_pre": 200, "n_drafted_post": 200, "det_truncated": 0},
  "speedup": 24.98
}
```

**Capturing both streams:**
```bash
# Capture stderr (log + comparison) to a file, stdout (JSON) to another
./build/bin/benchmark-deterministic-draft ... 2> run_output.txt > run_results.json
```

### 3.6 Full test suite

The full matrix covers 10 test files at 4 n_max values for two modes (accept-all and default). The results below show 5 representative files; the automation script in section 4.7 covers all 10.

#### Running accept-all mode (recommended for throughput)

```bash
for lang in c python java javascript; do
  for test_file in deterministic-draft-model-poc/tests/${lang}/*.test; do
    name=$(basename "$test_file" .test | cut -c4-)
    PROMPT=$(tail -n +4 "$test_file" | head -20)
    for nmax in 10 30 50 100; do
      echo "=== $lang/$name n_max=$nmax accept-all ==="
      ./build/bin/benchmark-deterministic-draft \
        -m "$MODEL" \
        --det-draft-model deterministic-draft-model-poc/build/deterministic-draft.so \
        --det-draft-n-max "$nmax" \
        --det-draft-accept-all \
        --lang "$lang" \
        -p "$PROMPT" \
        -n 200 --compare --n-runs 3
      echo ""
    done
  done
done
```

#### Running default mode (target verifies)

Same loop, drop `--det-draft-accept-all` from the command.

### 3.7 Automation script

The following script runs all configurations and captures results to a summary CSV. Set `ACCEPT_ALL=1` for accept-all mode (recommended for throughput benchmarks), or leave unset for default mode.

```bash
#!/bin/bash
# run-comprehensive-bench.sh
# Runs the full multi-language benchmark suite.
#
# Usage:
#   Accept-all mode: ACCEPT_ALL=1 MODEL=<model.gguf> bash run-comprehensive-bench.sh
#   Default mode:    MODEL=<model.gguf> bash run-comprehensive-bench.sh

set -euo pipefail

MODEL="${MODEL:?Set MODEL to the Qwen3.5-2B-MTP GGUF path}"
PLUGIN="deterministic-draft-model-poc/build/deterministic-draft.so"
TEST_DIR="deterministic-draft-model-poc/tests"
ACCEPT_ALL="${ACCEPT_ALL:-}"

OUTPUT_DIR="bench-results-$(date +%Y%m%d-%H%M%S)"
SUMMARY_CSV="${OUTPUT_DIR}/summary.csv"
mode_label="${ACCEPT_ALL:+accept-all}${ACCEPT_ALL:-default}"

N_MAX_VALUES=(10 30 50 100)
N_PREDICT=200
N_RUNS=3
NGL=99

declare -a TESTS
TESTS+=("c,01_quicksort,${TEST_DIR}/c/01_quicksort.c.test")
TESTS+=("c,02_linked_list,${TEST_DIR}/c/02_linked_list.c.test")
TESTS+=("c,03_hash_table,${TEST_DIR}/c/03_hash_table.c.test")
TESTS+=("c,04_simple_valid,${TEST_DIR}/c/04_simple_valid.c.test")
TESTS+=("java,01_bst,${TEST_DIR}/java/01_bst.java.test")
TESTS+=("java,02_producer_consumer,${TEST_DIR}/java/02_producer_consumer.java.test")
TESTS+=("javascript,01_async_queue,${TEST_DIR}/javascript/01_async_queue.js.test")
TESTS+=("javascript,02_merge_sort,${TEST_DIR}/javascript/02_merge_sort.js.test")
TESTS+=("python,01_graph,${TEST_DIR}/python/01_graph.py.test")
TESTS+=("python,02_lru_cache,${TEST_DIR}/python/02_lru_cache.py.test")

mkdir -p "$OUTPUT_DIR"

echo "lang,test,n_max,mode,base_tps,trt_tps,speedup,base_acc_pct,trt_acc_pct,base_n_pre,trt_n_pre,det_trunc,base_n_pred,trt_n_pred,errors" \
  > "$SUMMARY_CSV"

export LD_LIBRARY_PATH=./build/bin:${LD_LIBRARY_PATH:-}

for entry in "${TESTS[@]}"; do
  IFS=',' read -r lang test_name file_path <<< "$entry"

  for n_max in "${N_MAX_VALUES[@]}"; do
    echo ""
    echo "======================================================================"
    echo "  ${lang}: ${test_name} (n_max=${n_max}, mode=${mode_label})"
    echo "======================================================================"

    prompt=$(tail -n +4 "$file_path" | head -20)
    prompt_flat=$(echo "$prompt" | tr '\n' ' ')

    out_file="${OUTPUT_DIR}/${lang}_${test_name}_nmax${n_max}_${mode_label}.txt"
    json_file="${OUTPUT_DIR}/${lang}_${test_name}_nmax${n_max}_${mode_label}.json"

    if [ -n "${ACCEPT_ALL:-}" ]; then
      set +e
      ./build/bin/benchmark-deterministic-draft \
        -m "$MODEL" \
        --det-draft-model "$PLUGIN" \
        --det-draft-n-max "$n_max" \
        --det-draft-accept-all \
        --lang "$lang" \
        -p "$prompt_flat" \
        -n "$N_PREDICT" \
        -ngl "$NGL" \
        -fa on \
        --compare \
        --n-runs "$N_RUNS" \
        2> "$out_file" > "$json_file"
      set -e
    else
      set +e
      ./build/bin/benchmark-deterministic-draft \
        -m "$MODEL" \
        --det-draft-model "$PLUGIN" \
        --det-draft-n-max "$n_max" \
        --lang "$lang" \
        -p "$prompt_flat" \
        -n "$N_PREDICT" \
        -ngl "$NGL" \
        -fa on \
        --compare \
        --n-runs "$N_RUNS" \
        2> "$out_file" > "$json_file"
      set -e
    fi

    errors=$(grep -c "inconsistent sequence positions" "$out_file" || true)

    json_block=$(cat "$json_file")

    base_tps=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['baseline']['tps'])" 2>/dev/null || echo "N/A")
    trt_tps=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['treatment']['tps'])" 2>/dev/null || echo "N/A")
    speedup=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['speedup'])" 2>/dev/null || echo "N/A")
    base_acc=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['baseline']['accept_rate'])" 2>/dev/null || echo "N/A")
    trt_acc=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['treatment']['accept_rate'])" 2>/dev/null || echo "N/A")
    base_n_pre=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['baseline']['n_drafted_pre'])" 2>/dev/null || echo "N/A")
    trt_n_pre=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['treatment']['n_drafted_pre'])" 2>/dev/null || echo "N/A")
    det_trunc=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['treatment']['det_truncated'])" 2>/dev/null || echo "N/A")
    base_n_pred=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['baseline']['n_predict'])" 2>/dev/null || echo "N/A")
    trt_n_pred=$(echo "$json_block" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['treatment']['n_predict'])" 2>/dev/null || echo "N/A")

    echo "${lang},${test_name},${n_max},${mode_label},${base_tps},${trt_tps},${speedup},${base_acc},${trt_acc},${base_n_pre},${trt_n_pre},${det_trunc},${base_n_pred},${trt_n_pred},${errors}" \
      >> "$SUMMARY_CSV"

    echo "  -> Base: ${base_tps} tps, Treatment: ${trt_tps} tps, Speedup: ${speedup}x"
    echo "  -> Errors: ${errors}"
    echo "  -> Saved: ${out_file}"
  done
done

echo ""
echo "======================================================================"
echo "  Complete! Results in ${OUTPUT_DIR}/"
echo "  CSV summary: ${SUMMARY_CSV}"
echo "  Mode: ${mode_label}"
echo "======================================================================"
cat "$SUMMARY_CSV"
```

#### Usage

```bash
export MODEL=~/models/Qwen3.5-2B-MTP/unsloth_Qwen3.5-2B-MTP-GGUF_Q4_K_M.gguf

# Accept-all mode (recommended for throughput benchmarks)
ACCEPT_ALL=1 bash run-comprehensive-bench.sh

# Default mode (target verifies)
bash run-comprehensive-bench.sh
```

#### Expected runtime

With 10 test files x 4 n_max values = 40 configurations x 3 runs each (120 benchmark iterations), the full suite takes approximately 1-2 hours per mode on GPU, longer on CPU.

---

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

---

## 5. Accept-All Benchmark Results

> **Historical data only.** The tables below were produced with the older tree-sitter-based implementation (`Qwen3.5-4B-Coder-MTP-Q4_K_M`, RTX 4070). They are preserved to show the benchmark methodology and the kinds of effects the filter can have, but they are **not current XGrammar numbers**. Re-run section 3.7's automation script for up-to-date results on your model and hardware.

With `--det-draft-accept-all`, target model verification is skipped for all filter-accepted tokens. The deterministic filter's structural validation becomes the final arbiter. This trades verification safety for raw throughput and is suitable for single-language code-only generation (no mixed markdown/chat output).

### RTX 4070 (CUDA) - Accept-All, GPU (-ngl 99)

Model: Qwen3.5-4B-Coder-MTP-Q4_K_M. 10 test files x 4 n_max values. First 20 lines of real code as prompt, 200 tokens, `--det-draft-accept-all --compare --n-runs 3 -ngl 99 --ctx-size 2048`.

#### C (4 files)

| Test | n_max | Base TPS | Trt TPS | Speedup | Base Acc% | Trt Acc% | Det Trunc |
|------|-------|----------|---------|---------|-----------|----------|-----------|
| quicksort | 10 | 81.22 | 118.70 | 1.46x | 22.5 | 100.0 | 40 |
| quicksort | 30 | 34.45 | 62.40 | 1.81x | 8.9 | 97.2 | 7 |
| quicksort | 50 | 23.23 | 51.77 | 2.23x | 5.4 | 97.2 | 7 |
| quicksort | 100 | 10.28 | 40.92 | 3.98x | 2.6 | 97.2 | 7 |
| linked_list | 10 | 85.82 | 103.74 | 1.21x | 22.4 | 100.0 | 40 |
| linked_list | 30 | 36.37 | 51.50 | 1.42x | 8.9 | 100.0 | 30 |
| linked_list | 50 | 22.67 | 50.37 | 2.22x | 5.4 | 100.0 | 30 |
| linked_list | 100 | 11.27 | 17.92 | 1.59x | 2.6 | 100.0 | 30 |
| hash_table | 10 | 68.54 | 121.36 | 1.77x | 21.9 | 100.0 | 40 |
| hash_table | 30 | 29.91 | 153.66 | 5.14x | 5.3 | 100.0 | 30 |
| hash_table | 50 | 18.30 | 106.03 | 5.79x | 4.4 | 100.0 | 30 |
| hash_table | 100 | 9.73 | 42.37 | 4.35x | 1.8 | 100.0 | 30 |
| simple_valid | 10 | 151.34 | 173.53 | 1.15x | 28.2 | 100.0 | 40 |
| simple_valid | 30 | 53.62 | 208.82 | 3.89x | 14.6 | 97.2 | 7 |
| simple_valid | 50 | 48.50 | 200.12 | 4.13x | 11.9 | 100.0 | 4 |
| simple_valid | 100 | 24.75 | 167.23 | 6.76x | 5.8 | 100.0 | 3 |

#### Java (2 files)

| Test | n_max | Base TPS | Trt TPS | Speedup | Base Acc% | Trt Acc% | Det Trunc |
|------|-------|----------|---------|---------|-----------|----------|-----------|
| bst | 10 | 76.94 | 117.65 | 1.53x | 23.6 | 100.0 | 3 |
| bst | 30 | 34.14 | 240.36 | 7.04x | 7.9 | 100.0 | 170 |
| bst | 50 | 21.35 | 228.98 | 10.73x | 4.8 | 100.0 | 150 |
| bst | 100 | 12.12 | 143.54 | 11.84x | 2.7 | 79.8 | 100 |
| producer_consumer | 10 | 42.32 | 137.44 | 3.25x | 9.0 | 100.0 | 0 |
| producer_consumer | 30 | 28.26 | 156.67 | 5.54x | 5.9 | 71.0 | 0 |
| producer_consumer | 50 | 18.11 | 71.78 | 3.96x | 3.7 | 85.4 | 0 |
| producer_consumer | 100 | 9.87 | 114.38 | 11.59x | 2.0 | 89.0 | 0 |

#### JavaScript (2 files)

| Test | n_max | Base TPS | Trt TPS | Speedup | Base Acc% | Trt Acc% | Det Trunc |
|------|-------|----------|---------|---------|-----------|----------|-----------|
| async_queue | 10 | 53.56 | 143.61 | 2.68x | 13.2 | 100.0 | 15 |
| async_queue | 30 | 34.29 | 85.02 | 2.48x | 7.9 | 96.8 | 27 |
| async_queue | 50 | 15.53 | 72.39 | 4.66x | 2.9 | 74.5 | 13 |
| async_queue | 100 | 6.72 | 49.64 | 7.38x | 1.1 | 77.9 | 9 |
| merge_sort | 10 | 56.44 | 135.48 | 2.40x | 15.0 | 100.0 | 29 |
| merge_sort | 30 | 29.34 | 81.26 | 2.77x | 6.5 | 81.2 | 29 |
| merge_sort | 50 | 15.10 | 72.80 | 4.82x | 2.8 | 79.3 | 18 |
| merge_sort | 100 | 12.18 | 81.14 | 6.66x | 2.2 | 100.0 | 5 |

#### Python (2 files)

| Test | n_max | Base TPS | Trt TPS | Speedup | Base Acc% | Trt Acc% | Det Trunc |
|------|-------|----------|---------|---------|-----------|----------|-----------|
| graph | 10 | 89.16 | 226.05 | 2.54x | 23.1 | 100.0 | 0 |
| graph | 30 | 40.02 | 86.22 | 2.15x | 7.7 | 87.0 | 0 |
| graph | 50 | 29.13 | 149.26 | 5.12x | 5.8 | 91.1 | 0 |
| graph | 100 | 10.31 | 52.45 | 5.09x | 1.7 | 63.8 | 0 |
| lru_cache | 10 | 97.53 | 139.42 | 1.43x | 26.9 | 100.0 | 0 |
| lru_cache | 30 | 34.76 | 115.26 | 3.32x | 6.3 | 91.2 | 0 |
| lru_cache | 50 | 25.23 | 90.27 | 3.58x | 4.8 | 56.7 | 0 |
| lru_cache | 100 | 12.46 | 61.25 | 4.92x | 2.3 | 56.0 | 0 |

### Summary

Observations from the historical tree-sitter run above (not current XGrammar numbers):

- **40/40 runs completed**, 0 timeouts, 0 errors
- Speedups in this historical run ranged from ~1.15x to ~11.84x
- Average speedup across all 40 runs was ~4.5x
- **No hangs** - KV cache rollback fix and circuit breaker resolved hanging issues
- **Filter is actively working** - det truncated > 0 in most C/Java/JS tests
- **Best speedups at higher n_max** - more drafts to filter, more waste eliminated
- **Grammar permissiveness varies** - more permissive grammars pass more drafts through, but speedup still comes from skipped verification

### Key observations

General trends observed with the older implementation (current XGrammar results may differ in magnitude):

1. **Speedup scales with n_max** - higher n_max means more drafts generated, more waste in baseline, more benefit from filtering
2. **Baseline throughput drops sharply with n_max** - the MTP heads generate more garbage at higher n_max, baseline verification rejects most of it (low accept rates)
3. **Treatment throughput stays relatively stable** - the filter truncates garbage drafts, keeping only the valid prefix
4. **Java/BST showed the highest speedups in this run** - the model produced poor Java code that baseline rejected, but the filter kept the syntactically valid prefix
5. **Grammar permissiveness affects filtering** - more permissive grammars (like Python) pass more drafts through, reducing truncation but maintaining speedup from skipped verification
6. **C tests showed consistent speedups** - the filter actively truncated drafts in these tests

### Known limitations

1. **XGrammar validates syntax, not semantics** - syntactically valid but semantically wrong code passes the filter (e.g., `quick{swap` instead of `quicksort(`)
2. **Output quality not guaranteed** - the filter ensures structural validity but not semantic correctness
3. **Grammar coverage varies by language** - some GBNF grammars may be more permissive than others, affecting filter effectiveness
4. **Higher n_max = more risk** - more drafts per iteration means more chances for the model to produce syntactically valid but semantically wrong tokens

### Current architecture (XGrammar)

1. **Bitmask-constrained decoding** - the plugin's `fill_bitmask()` provides a token-level bitmask of grammar-valid tokens *before* sampling, via XGrammar's compiled grammar + tokenizer-aware `AcceptToken`. This constrains generation proactively rather than validating text after the fact.
2. **Bundled, loadable grammars** - grammars are resolved by language name (`--lang <language>`) from a `grammars/` directory bundled alongside the plugin `.so` (or `DETERMINISTIC_DRAFT_GRAMMAR_DIR` if set), not hardcoded or requiring a rebuild to add/edit.
3. **KV cache consistency** - draft truncation is handled via the standard speculative-decoding checkpoint/rollback mechanism already used for the non-deterministic draft path; no separate rollback logic is needed for the filter itself.

Note: the tree-sitter-era circuit breaker, non-ASCII token rejection, and separate bonus-token-vs-filter validation described in earlier revisions of this document no longer exist - they were specific workarounds for tree-sitter's text-based validation model and were removed when the plugin was rewritten around XGrammar's bitmask API. The bonus token (final token sampled after the drafted batch) is now bitmask-constrained the same way as every other token, via `common_speculative_sample_and_accept`.
