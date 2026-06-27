# Deterministic Draft Filter: Quick Start

> **llama.cpp**: build 9733 (commit f449e0553), GNU 15.2.0, Linux x86_64 (documented benchmark build; build from source for current version)

## Prerequisites

### System packages

```bash
sudo apt install build-essential cmake git python3
```

### pybind11 (required by XGrammar's build)

The plugin uses [XGrammar](https://github.com/mlc-ai/xgrammar) for grammar-constrained decoding, fetched automatically via CMake `FetchContent`. XGrammar's build requires pybind11:

```bash
pip install pybind11
```

### Model

The benchmark requires an MTP (Multi-Token Prediction) model. Download:

```bash
huggingface-cli download unsloth/Qwen3.5-2B-MTP-GGUF --include "*Q4_K_M*" --local-dir ~/models/Qwen3.5-2B-MTP
```



## Build Steps

### Step 1: Build llama.cpp core with spec enabled

```bash
cmake -B build -DDETERMINISTIC_SPEC_ENABLED=ON
cmake --build build --config Release -j$(nproc)
```

Produces:
- `build/libllama.a` / `build/libllama-common.a` - core libraries with plugin loader
- `build/bin/benchmark-deterministic-draft` - benchmark tool
- `external/include/deterministic_draft_plugin.h` - SDK header
- `external/lib/libdeterministic_draft_spec.so` - spec loader shared library

### Step 2: Link SDK artifacts into the PoC project

```bash
cd deterministic-draft-model-poc
./link-sdk.sh
```

Symlinks the header and `.so` from `external/` into `deterministic-draft-model-poc/lib/`.

### Step 3: Build the PoC plugin (standalone)

```bash
cd deterministic-draft-model-poc
rm -rf build
./link-sdk.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-march=native -O3"
cmake --build build -j$(nproc)
```

Produces:
- `deterministic-draft-model-poc/build/deterministic-draft.so` - the plugin

### Step 4: Verify setup

```bash
./build/bin/benchmark-deterministic-draft \
    -hf unsloth/Qwen3.5-2B-MTP-GGUF:Q4_K_M \
    --spec-type draft-mtp --lang c -p "test" -n 1
```

Or manually download from `unsloth/Qwen3.5-2B-MTP-GGUF` on HuggingFace and pass via `-m <path>`.

### Step 5: Run

```bash
# Baseline: MTP only
./build/bin/benchmark-deterministic-draft -m <model.gguf> --spec-type draft-mtp \
    --lang c \
    -p "int fib(int n) {" -n 200

# Treatment: MTP + deterministic filter
./build/bin/benchmark-deterministic-draft -m <model.gguf> \
    --deterministic-draft-model deterministic-draft-model-poc/build/deterministic-draft.so \
    --lang c \
    -p "int fib(int n) {" -n 200

# Comparison (runs both automatically)
./build/bin/benchmark-deterministic-draft -m <model.gguf> \
    --deterministic-draft-model deterministic-draft-model-poc/build/deterministic-draft.so \
    --lang c \
    -p "int fib(int n) {" -n 200 --compare --n-runs 3
```

## Language / Grammar Configuration

The plugin resolves grammars by language name at runtime. Grammars are plain `.gbnf` files bundled alongside the `.so` during build (via CMake `POST_BUILD` step) -- no manual copy needed.

**Select a language** with `--lang` on the benchmark tool:

| `--lang` value | Grammar file | Notes |
|----------------|-------------|-------|
| `c` | `grammars/c.gbnf` | |
| `java` | `grammars/java.gbnf` | |
| `python` | `grammars/python.gbnf` | |
| `javascript` | `grammars/javascript.gbnf` | |

**Override grammar location** with the `DETERMINISTIC_DRAFT_GRAMMAR_DIR` environment variable:

```bash
export DETERMINISTIC_DRAFT_GRAMMAR_DIR=/path/to/custom/grammars
./build/bin/benchmark-deterministic-draft ...
```

If unset, the plugin defaults to `<plugin_directory>/grammars/` (the directory containing the `.so`).

**Auto-select grammar by language** with the `DETERMINISTIC_DRAFT_LANGUAGE` environment variable (alternative to `--lang` on the benchmark tool):

```bash
DETERMINISTIC_DRAFT_LANGUAGE=c ./build/bin/benchmark-deterministic-draft ...
```

This is useful when using the plugin programmatically (not via the benchmark tool). The plugin reads this env var during initialization and loads the corresponding bundled grammar.

**Via the C API** (for programmatic use), call `deterministic_draft_set_language()` or `deterministic_draft_set_grammar()` with an EBNF string.

## Build Structure

The deterministic draft system uses abstraction and separation of concerns to keep changes to the main llama.cpp tree minimal. There are three independent layers:

### 1. Changes to main llama.cpp core

The core changes are small and domain-agnostic. The core knows nothing about XGrammar or any specific validation strategy.

```
llama.cpp/ (main tree)
  ├── include/
  │   ├── deterministic_draft_plugin.h     -- Public C API contract (the "SDK" header)
  │   └── llama.h                          -- C API declarations (extern "C")
  ├── external/deterministic_draft_spec.cpp  -- Plugin loader (dlopen/dlsym, no llama dep)
  ├── external/include/
  │   ├── deterministic_draft_plugin.h       -- Plugin contract header (for plugin authors)
  │   └── llama_deterministic_draft.h        -- Consumer API header (for end users)
  ├── common/speculative.{h,cpp}            -- Pipeline integration (calls plugin via C API)
  ├── common/arg.cpp                        -- 3 CLI flags + auto-imply MTP
  ├── common/common.h                       -- Enum + params struct
  ├── tools/server/server-context.cpp       -- Server uses speculative pipeline (no bolt-on)
  └── tools/deterministic-draft-bench/      -- Benchmark tool
```

No XGrammar-specific code. No domain-specific logic. No plugin implementation details. The core provides a generic plugin loader (`dlopen` + `dlsym`) and integrates it into the speculative pipeline.

### 2. External / shared distributed artifacts

Built when `-DDETERMINISTIC_SPEC_ENABLED=ON` is passed to the main cmake. These artifacts are what third-party plugin authors consume.

```
llama.cpp/external/
  ├── CMakeLists.txt                        -- Installs headers + builds spec .so
  ├── deterministic_draft_spec.cpp          -- dlopen loader + C API (no llama dependency)
  ├── include/
  │   ├── deterministic_draft_plugin.h      -- For plugin authors (implements the plugin)
  │   └── llama_deterministic_draft.h       -- For consumers (links against the .so)
  └── lib/
      └── libdeterministic_draft_spec.so    -- Shared library (plugin loader, no llama dep)
```

The `.so` contains the `dlopen`/`dlsym` loader and all C API wrappers except `generate_draft` (which needs llama types and stays in libllama).

**Two headers, two audiences:**
- `deterministic_draft_plugin.h` - for **plugin authors** who implement a .so (defines `deterministic_draft_create`, `deterministic_draft_fill_bitmask`, `deterministic_draft_commit`, `deterministic_draft_filter_draft`, etc. that the plugin must export)
- `llama_deterministic_draft.h` - for **consumers** who link against `libdeterministic_draft_spec.so` to load and call plugins (declares `llama_deterministic_draft_init`, `llama_deterministic_draft_load`, etc.)

A consumer who also builds their own plugin (the typical case) needs both headers.

### 3. deterministic-draft-model-poc/ (consumer project)

A standalone project simulating an external consumer - e.g., a law firm, government agency, or commercial vendor with a private implementation in a secure corporate repository. This directory has **no dependency on the main llama.cpp build tree** and the main tree has **no dependency on this directory**.

```
deterministic-draft-model-poc/
  ├── CMakeLists.txt                        -- Standalone CMake project
  ├── link-sdk.sh                           -- Symlinks external artifacts into lib/
  ├── build_grammars.sh                     -- (deprecated: grammars now built by CMake)
  ├── lib/
  │   ├── deterministic_draft_plugin.h      -- Symlinked from external/include/ (plugin author header)
  │   ├── llama_deterministic_draft.h       -- Symlinked from external/include/ (consumer header)
  │   └── libdeterministic_draft_spec.so    -- Symlinked from external/lib/
  ├── src/
  │   ├── plugin.cpp                        -- XGrammar plugin implementation
  │   └── common/                           -- grammar handler, language handlers
  └── build/
      └── deterministic-draft.so            -- The plugin, loaded at runtime
```

A real consumer would receive the SDK artifacts via distribution (package manager, bundle, internal artifact repo) instead of symlinks. Their private implementation (XGrammar, regex, schema validators, legal citation parsers, etc.) would live entirely in their own repository.

## CLI Flags

| Flag | Alias | Description |
|------|-------|-------------|
| `--deterministic-draft-model <path>` | `--det-draft-model` | Path to the plugin (.so/.dylib/.dll). Auto-enables `--spec-type draft-mtp`. Requires an MTP-enabled model (`n_layer_nextn > 0`). |
| `--deterministic-draft-n-max <N>` | `--det-draft-n-max` | Controls the draft token budget. See behaviour table below. |
| `--det-draft-accept-all` | | Bypass target verification entirely. The filter becomes the sole verifier. Only active with `--det-draft-model`. See Disclosures and Scope below. |
| `--lang <language>` | | Select grammar by language name (benchmark tool only). Resolves from `grammars/<lang>.gbnf` bundled alongside the `.so`. Alternatively, set `DETERMINISTIC_DRAFT_LANGUAGE` env var (useful for programmatic/plugin-only use). Override grammar directory with `DETERMINISTIC_DRAFT_GRAMMAR_DIR` env var. |

### --det-draft-n-max behaviour

| Value | Filter cap | --spec-draft-n-max | MTP auxiliary heads draft |
|-------|------------|-------------------|--------------------------|
| -1 (default) | no cap | untouched (llama.cpp default: 3) | 3 tokens per step |
| 0 | filter disabled (warning emitted) | untouched | 3 tokens per step |
| N > 0 | N tokens | auto-set to N | N tokens per step |

When `--det-draft-n-max` is set to a value greater than 0, it controls the
draft budget at both ends simultaneously -- the filter validates up to N tokens
and the MTP auxiliary heads draft up to N tokens. You do not need to set
`--spec-draft-n-max` separately; it is auto-derived.

This is why n_max matters for benchmark results. At n_max=100, the auxiliary
heads draft up to 100 tokens per step. With a 0.4% baseline accept rate (N100),
that means roughly 99 wasted draft tokens per step before the filter. With the
filter and `--det-draft-accept-all`, those 100 tokens are validated
structurally and committed directly -- no target verification overhead.

Do not set `--spec-draft-n-max` manually when using `--det-draft-n-max > 0`.
If both are set explicitly and disagree, behaviour is undefined.

## Disclosures and Scope

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

## Building a Custom Plugin

A custom plugin (e.g., a private legal citation validator) follows the same pattern as the PoC. The consumer needs both SDK headers and the shared library:

1. Copy the SDK artifacts from `external/`:
    - `deterministic_draft_plugin.h` - **plugin author header**: implement these functions in your plugin .so
    - `llama_deterministic_draft.h` - **consumer header**: use these to load and call the plugin via the .so
    - `libdeterministic_draft_spec.so` - the loader library to link against for standalone testing

2. Implement the plugin: write a .so that exports the `deterministic_draft_*` functions declared in `deterministic_draft_plugin.h`

3. Compile as a shared library (.so/.dylib/.dll), linking against `libdeterministic_draft_spec.so` if you want to test the plugin standalone (without llama.cpp)

4. Load via `--deterministic-draft-model /path/to/your-plugin.so` in any llama.cpp binary built with `-DDETERMINISTIC_SPEC_ENABLED=ON`

No llama.cpp source or build system is needed. The plugin lives in its own repository, built and distributed independently. A law firm, government agency, or commercial vendor would keep their plugin implementation in a private repo, consuming only the distributed SDK artifacts.
