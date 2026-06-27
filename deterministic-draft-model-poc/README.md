# Deterministic Draft Filter - Plugin Framework for MTP Speculative Decoding

A plugin framework that intercepts MTP draft tokens before target verification,
filtering structurally invalid tokens using domain-specific validators loaded at
runtime via `dlopen`/`dlsym`.

This is a proof of concept. The contribution is the plugin contract and the
integration point - not the specific validator. The reference implementation
uses XGrammar for grammar-constrained decoding with jump-forward support
as a concrete demonstration of one domain. Any domain with a deterministic
correctness criterion - legal citation formats, schema validation, regulatory
constraints, structured query languages, private proprietary rules - can
implement the same contract and plug in without touching llama.cpp core.

## Background: MTP and why accept rate matters

In standard autoregressive decoding, the main output head produces one token
per forward pass - 1:1. MTP-enabled models add auxiliary prediction heads that
run on top of the same shared backbone hidden state, each drafting one
additional token. A model with 3 MTP heads produces 4 candidate tokens per
forward pass: 1 from the main head plus 3 draft tokens from the auxiliary heads
(`--spec-draft-n-max 3`).

The auxiliary heads are cheap relative to the shared backbone computation, which
is already done. The efficiency gain depends entirely on accept rate - how many
draft tokens the target accepts before rejecting. Rejected draft tokens waste
the cost of the auxiliary heads and fall back to single-token decoding.

On constrained hardware, this failure mode is severe. On the N100, baseline MTP
accept rate is 0.4%: the auxiliary heads are generating draft tokens that get
rejected 99.6% of the time. The problem is not model capacity but the absence
of any structural guarantee between draft and target. The filter addresses this
at the source - validating draft tokens against a language grammar before they
reach target verification, so only structurally valid tokens consume
verification slots.

## Core changes to llama.cpp

Changes to core are confined to:

- Shared data structures and headers
- Three CLI flags (see below)
- `external/deterministic_draft_spec.cpp` - plugin loader (dlopen/dlsym),
  feeds draft tokens from the MTP auxiliary heads to the plugin via the C API
  contract
- `external/include/llama_deterministic_draft.h` - consumer API header
- `external/include/deterministic_draft_plugin.h` - plugin contract header
- `common/speculative.{h,cpp}` - pipeline integration, including a conditional
  bypass of `common_sampler_sample_and_accept_n` when both `--det-draft-model`
  and `--det-draft-accept-all` are set

The bypass follows the same gating pattern as MTP's own conditional behaviour.
Without `--det-draft-model` the code path is identical to unmodified llama.cpp.
With `--det-draft-model` but without `--det-draft-accept-all`, the plugin
validates tokens and the result feeds into the existing verification path -
`common_sampler_sample_and_accept_n` runs as normal. Only when both flags are
set does core bypass target verification entirely.

The plugin has no access to core sampling routines. It receives tokens via the
C API contract, returns a validation result, and core decides what to do with
that result based on the flags. Domain logic stays entirely outside core.

Build with `-DDETERMINISTIC_SPEC_ENABLED=ON`. Without this flag the core is
unchanged.

## Plugin architecture

```
PLUGIN AUTHOR          DISTRIBUTION           END USER
─────────────          ────────────           ────────
plugin.cpp
#include plugin.h
implements contract
      |
      v
deterministic-        ships .so         downloads .so
draft.so         ─────────────────►           |
                                              v
                                    llama.cpp
                                    --det-draft-model ./plugin.so
                                         |
                                    libdeterministic_draft_spec.so
                                    dlopen / dlsym
                                     deterministic_draft_filter_draft
                                     deterministic_draft_commit
                                    deterministic_draft_reset
                                    deterministic_draft_destroy
                                         |
                                    common/speculative.cpp
                                    feeds draft tokens to plugin
                                    applies filter result
```

Three parties, three distinct concerns:

**Plugin author** - writes a domain-specific validator, includes
`deterministic_draft_plugin.h`, implements `deterministic_draft_create`,
`deterministic_draft_filter_draft`, `deterministic_draft_commit`,
`deterministic_draft_reset`, `deterministic_draft_destroy`, and compiles to a
`.so`. The only party that needs the headers. The reference implementation
(XGrammar grammar-constrained decoding) is one example; a regulated organisation's
private validator is another.

**Distribution** - the plugin author ships the `.so`. Open source on GitHub,
a private artifact in a corporate repo, or anything in between. llama.cpp
carries no opinion on how plugins are distributed.

**End user** - downloads or receives the `.so`, passes it via
`--det-draft-model ./plugin.so`, and runs llama.cpp. They never see a header.
`libdeterministic_draft_spec.so` (built as part of core with
`-DDETERMINISTIC_SPEC_ENABLED=ON`) handles `dlopen`/`dlsym` at runtime,
resolves the function pointer table, and calls into the plugin through
`common/speculative.cpp`.

The plugin has no dependency on llama.cpp internals. It receives draft tokens,
returns a validation result, and core decides what to do with that result based
on the active flags.

## Relationship to MTP

MTP is reused as-is. The filter composes with the existing MTP framework rather
than replacing or forking it. Enabling `--det-draft-model` auto-enables
`--spec-type draft-mtp`. If the model lacks MTP auxiliary heads
(`n_layer_nextn == 0`), llama.cpp fails to start with the standard MTP error.

## Usage guidance

| Flag | Description |
|---|---|
| `--det-draft-model FNAME` | Path to the plugin (.so/.dylib/.dll). Auto-enables `--spec-type draft-mtp`. Requires an MTP-enabled model. |
| `--det-draft-n-max N` | Max draft tokens to validate per step (-1=no cap, 0=disabled). When >0, also sets `--spec-draft-n-max` to N - controls the draft budget at both the filter and the MTP auxiliary heads with a single flag. |
| `--det-draft-accept-all` | Bypass target verification entirely. The filter is the sole verifier. Default: false. |

| Environment Variable | Description |
|---|---|
| `DETERMINISTIC_DRAFT_LANGUAGE` | Language name for grammar-constrained decoding (e.g., "python", "c", "java", "javascript"). If set, the plugin auto-loads the corresponding bundled grammar during initialization. |
| `DETERMINISTIC_DRAFT_GRAMMAR_DIR` | Override directory for bundled grammar files. Defaults to `<plugin_dir>/grammars/`. |

`--det-draft-accept-all` is appropriate only where the domain validator can be
trusted as the authority on token validity - single-language code-only
generation, not mixed content, markdown, or chat output.

The filter validates structural correctness only, not semantic correctness. Code
that parses as valid C may still be semantically wrong - a shadowed variable,
an incorrect algorithm, an off-by-one. XGrammar operates at the grammar
level; name resolution, type checking, and logic errors are outside its scope
and remain the caller's responsibility.

## Supported languages (reference implementation)

C, Java, Python, JavaScript

## Documentation

- [Quick Start](docs/quick-start.md) - build and run instructions
- [Benchmark Overview](docs/benchmark-overview.md) - results and usage guidance
- [Comprehensive Benchmark](docs/benchmark-comprehensive.md) - full methodology and data
