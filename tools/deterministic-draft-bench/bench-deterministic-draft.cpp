// bench-deterministic-draft.cpp
//
// Benchmark for deterministic draft filtering with MTP speculative decoding.
//
// Measures end-to-end throughput (tokens/sec) using the actual common_speculative
// pipeline with MTP draft heads + deterministic (plugin-based) filter.
//
// Usage:
//   # Baseline (MTP only, no deterministic filter)
//   ./benchmark-deterministic-draft -m <model> --spec-type draft-mtp
//       -p "int main() {" -n 200
//
//   # Treatment (MTP + deterministic filter)
//   ./benchmark-deterministic-draft -m <model>
//       --deterministic-draft-model <plugin.so>
//       -p "int main() {" -n 200
//
//   # Compare both (runs baseline then treatment automatically)
//   ./benchmark-deterministic-draft -m <model>
//       --deterministic-draft-model <plugin.so>
//       -p "int main() {" -n 200 --compare

#include "arg.h"
#include "common.h"
#include "llama.h"
#include "log.h"
#include "sampling.h"
#include "speculative.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct bench_config {
    bool        compare       = false;
    int         n_runs        = 3;
    std::string language;
};

static bool validate_output(const std::string & text, const std::string & lang, std::string & err);

struct bench_result {
    double t_enc_sec       = 0.0;
    double t_dec_sec       = 0.0;
    int    n_input         = 0;
    int    n_predict       = 0;
    int    n_drafted_pre   = 0;  // raw draft tokens from MTP (before filter)
    int    n_drafted_post  = 0;  // filtered draft tokens (after det filter)
    int    n_accepted      = 0;
    int    n_det_truncated = 0;
    int    n_target_syncs  = 0;  // number of actual target llama_decode calls (deferred-sync mode)
    int    n_errors        = 0;  // content errors vs baseline (--compare mode)
    double tps             = 0.0;
    double accept_rate     = 0.0;  // accepted / drafted_post
    bool   output_valid    = true;
    std::string output_error;
    std::string prompt_text;
    std::string generated_text;
};

static bench_result run_benchmark(common_params & params, const bench_config & cfg, int run_idx);

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");

    // pre-parse extra args and remove them from argv so common_params_parse doesn't reject them
    bench_config cfg;
    {
        std::vector<char *> filtered_args;
        filtered_args.push_back(argv[0]);
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--compare") {
                cfg.compare = true;
            } else if (arg == "--n-runs" && i + 1 < argc) {
                cfg.n_runs = std::atoi(argv[++i]);
            } else if (arg == "--lang" && i + 1 < argc) {
                cfg.language = argv[++i];
            } else {
                filtered_args.push_back(argv[i]);
            }
        }
        argc = (int) filtered_args.size();
        for (int i = 0; i < argc; i++) {
            argv[i] = filtered_args[i];
        }
    }

    common_params params;
    common_init();

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_SPECULATIVE)) {
        return 1;
    }

    llama_backend_init();
    llama_numa_init(params.numa);

    const bool has_det_filter = params.speculative.deterministic_draft.enabled;

    if (cfg.compare && has_det_filter) {
        // run baseline (MTP only) then treatment (MTP + det filter)

        // baseline: temporarily disable det filter
        LOG("\n=== Baseline: MTP only (no deterministic filter) ===\n\n");
        auto params_base                                    = params;
        params_base.speculative.deterministic_draft.enabled = false;
        params_base.speculative.deterministic_draft.det_accept_all = false;
        params_base.speculative.deterministic_draft.plugin_path.clear();
        // remove DRAFT_DETERMINISTIC from types, keep DRAFT_MTP
        auto & types = params_base.speculative.types;
        types.erase(std::remove(types.begin(), types.end(), COMMON_SPECULATIVE_TYPE_DRAFT_DETERMINISTIC), types.end());

        std::vector<bench_result> baseline_results;
        for (int r = 0; r < cfg.n_runs; r++) {
            LOG("--- Baseline run %d/%d ---\n", r + 1, cfg.n_runs);
            baseline_results.push_back(run_benchmark(params_base, cfg, r));
        }

        // treatment: MTP + det filter (original params)
        LOG("\n=== Treatment: MTP + deterministic filter ===\n\n");
        std::vector<bench_result> treatment_results;
        for (int r = 0; r < cfg.n_runs; r++) {
            LOG("--- Treatment run %d/%d ---\n", r + 1, cfg.n_runs);
            treatment_results.push_back(run_benchmark(params, cfg, r));
        }

        // output validation against real parser (correctness, not byte diff)
        int base_valid = 0;
        int trat_valid = 0;
        if (!cfg.language.empty()) {
            for (size_t r = 0; r < baseline_results.size(); r++) {
                if (baseline_results[r].output_valid) base_valid++;
            }
            for (size_t r = 0; r < treatment_results.size(); r++) {
                if (treatment_results[r].output_valid) trat_valid++;
            }
            LOG("\n=== Output Correctness (%s) ===\n\n", cfg.language.c_str());
            LOG("  baseline valid:   %d/%zu runs\n", base_valid, baseline_results.size());
            LOG("  treatment valid:  %d/%zu runs\n", trat_valid, treatment_results.size());
            if (base_valid < (int) baseline_results.size()) {
                LOG_ERR("  baseline FAIL: %d invalid runs (model output is broken without filter)\n",
                        (int) baseline_results.size() - base_valid);
            }
            if (trat_valid < (int) treatment_results.size()) {
                LOG_ERR("  treatment FAIL: %d invalid runs (filter did not ensure validity)\n",
                        (int) treatment_results.size() - trat_valid);
            }
            for (size_t r = 0; r < treatment_results.size(); r++) {
                if (!treatment_results[r].output_valid) {
                    fprintf(stderr, "\n--- invalid treatment text (run %zu) ---\n%s\n--- end ---\n",
                            r, treatment_results[r].generated_text.c_str());
                }
            }
        }

        // print comparison
        auto avg_d = [](const std::vector<bench_result> & v, double bench_result::* field) {
            double sum = 0;
            for (const auto & r : v) {
                sum += r.*field;
            }
            return sum / v.size();
        };
        auto avg_i = [](const std::vector<bench_result> & v, int bench_result::* field) {
            double sum = 0;
            for (const auto & r : v) {
                sum += r.*field;
            }
            return sum / v.size();
        };

        double base_tps     = avg_d(baseline_results, &bench_result::tps);
        double treat_tps    = avg_d(treatment_results, &bench_result::tps);
        double base_accept  = avg_d(baseline_results, &bench_result::accept_rate);
        double treat_accept = avg_d(treatment_results, &bench_result::accept_rate);

        LOG("\n=== Comparison Results ===\n\n");
        LOG("                    Baseline (MTP)    Treatment (MTP+DET)    Delta\n");
        LOG("  throughput tps:   %8.2f           %8.2f             %+.1f%%\n", base_tps, treat_tps,
            100.0 * (treat_tps - base_tps) / base_tps);
        LOG("  accept rate:      %8.1f%%          %8.1f%%            %+.1f%%\n", base_accept, treat_accept,
            treat_accept - base_accept);
        if (!cfg.language.empty()) {
            LOG("  output valid:     %8d/%zu        %8d/%zu\n",
                base_valid, baseline_results.size(), trat_valid, treatment_results.size());
        }
        LOG("  n_predict:        %8.0f           %8.0f\n", avg_i(baseline_results, &bench_result::n_predict),
            avg_i(treatment_results, &bench_result::n_predict));
        LOG("  drafted (pre):    %8.0f           %8.0f\n", avg_i(baseline_results, &bench_result::n_drafted_pre),
            avg_i(treatment_results, &bench_result::n_drafted_pre));
        LOG("  drafted (post):   %8.0f           %8.0f\n", avg_i(baseline_results, &bench_result::n_drafted_post),
            avg_i(treatment_results, &bench_result::n_drafted_post));
        LOG("  det truncated:    %8s           %8.0f\n", "N/A",
            avg_i(treatment_results, &bench_result::n_det_truncated));

        // output validation (always on when --lang is specified)
        if (!cfg.language.empty()) {
            LOG("\n=== Output Validation (%s) ===\n\n", cfg.language.c_str());
            int n_invalid = 0;
            for (size_t r = 0; r < treatment_results.size(); r++) {
                if (treatment_results[r].output_valid) {
                    LOG("  run %zu: PASS\n", r);
                } else {
                    n_invalid++;
                    LOG_ERR("  run %zu: FAIL - %s\n", r, treatment_results[r].output_error.c_str());
                    fprintf(stderr, "\n--- invalid generated text (run %zu) ---\n%s\n--- end ---\n",
                            r, treatment_results[r].generated_text.c_str());
                }
            }
            if (n_invalid > 0) {
                LOG_ERR("\n  *** %d/%zu runs produced INVALID %s code ***\n\n", n_invalid, treatment_results.size(), cfg.language.c_str());
            }
        }

        // JSON output
        printf("\n{\n");
        printf("  \"baseline\": {\n");
        printf("    \"tps\": %.2f,\n", base_tps);
        printf("    \"accept_rate\": %.1f,\n", base_accept);
        printf("    \"n_predict\": %.0f,\n", avg_i(baseline_results, &bench_result::n_predict));
        printf("    \"n_drafted_pre\": %.0f,\n", avg_i(baseline_results, &bench_result::n_drafted_pre));
        printf("    \"n_drafted_post\": %.0f", avg_i(baseline_results, &bench_result::n_drafted_post));
        if (!cfg.language.empty()) {
            printf(",\n    \"output_valid_runs\": %d", base_valid);
        }
        printf("\n  },\n");
        printf("  \"treatment\": {\n");
        printf("    \"tps\": %.2f,\n", treat_tps);
        printf("    \"accept_rate\": %.1f,\n", treat_accept);
        printf("    \"n_predict\": %.0f,\n", avg_i(treatment_results, &bench_result::n_predict));
        printf("    \"n_drafted_pre\": %.0f,\n", avg_i(treatment_results, &bench_result::n_drafted_pre));
        printf("    \"n_drafted_post\": %.0f,\n", avg_i(treatment_results, &bench_result::n_drafted_post));
        printf("    \"det_truncated\": %.0f", avg_i(treatment_results, &bench_result::n_det_truncated));
        if (!cfg.language.empty()) {
            printf(",\n    \"output_valid_runs\": %d", trat_valid);
        }
        printf("\n  },\n");
        printf("  \"speedup\": %.3f\n", treat_tps / base_tps);
        printf("}\n");

    } else {
        // single mode run
        const char * mode = has_det_filter ? "MTP + deterministic filter" : "MTP only";
        LOG("\n=== Single run: %s ===\n\n", mode);

        std::vector<bench_result> results;
        for (int r = 0; r < cfg.n_runs; r++) {
            LOG("--- Run %d/%d ---\n", r + 1, cfg.n_runs);
            results.push_back(run_benchmark(params, cfg, r));
        }

        auto avg_d = [](const std::vector<bench_result> & v, double bench_result::* field) {
            double sum = 0;
            for (const auto & r : v) {
                sum += r.*field;
            }
            return sum / v.size();
        };
        auto avg_i = [](const std::vector<bench_result> & v, int bench_result::* field) {
            double sum = 0;
            for (const auto & r : v) {
                sum += r.*field;
            }
            return sum / v.size();
        };

        LOG("\n=== Results ===\n\n");
        LOG("  throughput:       %.2f tokens/sec\n", avg_d(results, &bench_result::tps));
        LOG("  accept rate:      %.1f%%\n", avg_d(results, &bench_result::accept_rate));
        LOG("  n_predict:        %.0f\n", avg_i(results, &bench_result::n_predict));
        LOG("  drafted (pre):    %.0f\n", avg_i(results, &bench_result::n_drafted_pre));
        LOG("  drafted (post):   %.0f\n", avg_i(results, &bench_result::n_drafted_post));
        if (has_det_filter) {
            LOG("  det truncated:    %.0f\n", avg_i(results, &bench_result::n_det_truncated));
        }

        // output validation (always on when --lang is specified)
        if (!cfg.language.empty()) {
            LOG("\n=== Output Validation (%s) ===\n\n", cfg.language.c_str());
            int n_invalid = 0;
            for (size_t r = 0; r < results.size(); r++) {
                if (results[r].output_valid) {
                    LOG("  run %zu: PASS\n", r);
                } else {
                    n_invalid++;
                    LOG_ERR("  run %zu: FAIL - %s\n", r, results[r].output_error.c_str());
                    fprintf(stderr, "\n--- invalid generated text (run %zu) ---\n%s\n--- end ---\n",
                            r, results[r].generated_text.c_str());
                }
            }
            if (n_invalid > 0) {
                LOG_ERR("\n  *** %d/%zu runs produced INVALID %s code ***\n\n", n_invalid, results.size(), cfg.language.c_str());
            }
        }

        printf("\n{\n");
        printf("  \"tps\": %.2f,\n", avg_d(results, &bench_result::tps));
        printf("  \"accept_rate\": %.1f,\n", avg_d(results, &bench_result::accept_rate));
        printf("  \"n_predict\": %.0f,\n", avg_i(results, &bench_result::n_predict));
        printf("  \"n_drafted_pre\": %.0f,\n", avg_i(results, &bench_result::n_drafted_pre));
        printf("  \"n_drafted_post\": %.0f", avg_i(results, &bench_result::n_drafted_post));
        if (has_det_filter) {
            printf(",\n  \"det_truncated\": %.0f", avg_i(results, &bench_result::n_det_truncated));
        }
        if (!cfg.language.empty()) {
            int n_valid = 0;
            for (size_t r = 0; r < results.size(); r++) {
                if (results[r].output_valid) n_valid++;
            }
            printf(",\n  \"output_valid_runs\": %d", n_valid);
        }
        printf("\n}\n");
    }

    llama_backend_free();

    return 0;
}

// checkpoint helper using common infrastructure
using common_ckpt = common_prompt_checkpoint;

static bool validate_output(const std::string & text, const std::string & lang, std::string & err) {
    // java (and any future lang needing a file) must write to a temp file
    if (lang == "java") {
        const char * tmp_path = "/tmp/llama_validate_output.java";
        FILE * f = fopen(tmp_path, "w");
        if (!f) {
            err = "failed to create temp file " + std::string(tmp_path);
            return false;
        }
        fwrite(text.c_str(), 1, text.size(), f);
        fclose(f);

        char cmd_buf[512];
        snprintf(cmd_buf, sizeof(cmd_buf), "javac --release 21 -d /tmp %s 2>&1", tmp_path);
        FILE * pipe = popen(cmd_buf, "r");
        if (!pipe) {
            err = "failed to execute javac";
            return false;
        }
        char out_buf[1024];
        std::string stderr_output;
        while (fgets(out_buf, sizeof(out_buf), pipe)) {
            stderr_output += out_buf;
        }
        int rc = pclose(pipe);
        remove(tmp_path);
        if (rc != 0) {
            int exit_code = WEXITSTATUS(rc);
            if (!stderr_output.empty()) {
                err = stderr_output;
                // trim trailing newline
                while (!err.empty() && err.back() == '\n') err.pop_back();
            } else {
                err = "javac failed with exit code " + std::to_string(exit_code);
            }
            return false;
        }
        return true;
    }

    const char * cmd = nullptr;

    if (lang == "c")                        { cmd = "gcc -fsyntax-only -x c - -o /dev/null 2>&1";    }
    if (lang == "cpp" || lang == "c++")     { cmd = "g++ -fsyntax-only -x c++ - -o /dev/null 2>&1";  }
    if (lang == "python" || lang == "py")   { cmd = "python3 -c \"import sys; compile(sys.stdin.read(), '<string>', 'exec')\" 2>&1"; }
    if (lang == "javascript" || lang == "js") { cmd = "node --check - 2>&1";                          }
    if (lang == "typescript" || lang == "ts") { cmd = "npx tsc --noEmit --strict /dev/stdin 2>&1";    }

    if (!cmd) {
        err = "unsupported language: " + lang;
        return false;
    }

    FILE * pipe = popen(cmd, "w");
    if (!pipe) {
        err = "failed to execute parser";
        return false;
    }

    fwrite(text.c_str(), 1, text.size(), pipe);
    int rc = pclose(pipe);

    if (rc != 0) {
        int exit_code = WEXITSTATUS(rc);
        err = "validation failed with exit code " + std::to_string(exit_code);
        return false;
    }

    return true;
}

static bench_result run_benchmark(common_params & params, const bench_config & cfg, int run_idx) {
    bench_result result;

    // load the target model
    auto            llama_init_tgt = common_init_from_params(params);
    llama_model *   model_tgt      = llama_init_tgt->model();
    llama_context * ctx_tgt        = llama_init_tgt->context();

    if (!ctx_tgt || !model_tgt) {
        LOG_ERR("failed to create target context (model: %s)\n", params.model.path.c_str());
        return result;
    }

    const llama_vocab * vocab = llama_model_get_vocab(model_tgt);

    // load the draft model / MTP context
    llama_model_ptr   model_dft;
    llama_context_ptr ctx_dft;

    const bool is_mtp = std::find(params.speculative.types.begin(), params.speculative.types.end(),
                                  COMMON_SPECULATIVE_TYPE_DRAFT_MTP) != params.speculative.types.end();

    // --det-draft-accept-all requires the deterministic filter and MTP
    if (params.speculative.deterministic_draft.det_accept_all) {
        if (!params.speculative.deterministic_draft.enabled) {
            LOG_ERR("--det-draft-accept-all requires --deterministic-draft-model (plugin not loaded)\n");
            return result;
        }
        if (!is_mtp) {
            LOG_ERR("--det-draft-accept-all requires MTP speculative decoding (--spec-type draft-mtp)\n");
            return result;
        }
    }

    // sync draft sizes: det-draft-n-max caps filter output, spec-draft-n-max caps MTP output
    // mismatched values waste draft head capacity or cause filter to never see larger batches
    if (params.speculative.deterministic_draft.enabled && params.speculative.deterministic_draft.n_max > 0) {
        const int32_t spec_n = common_speculative_n_max(&params.speculative);
        const int32_t det_n  = params.speculative.deterministic_draft.n_max;
        if (spec_n > 0 && spec_n != det_n) {
            LOG_WRN("--spec-draft-n-max (%d) differs from --det-draft-n-max (%d), "
                    "capping at the smaller value\n", spec_n, det_n);
        }
    }

    if (is_mtp) {
        // MTP: create a separate context from the same target model
        auto cparams_mtp          = common_context_params_to_llama(params);
        cparams_mtp.ctx_type      = LLAMA_CONTEXT_TYPE_MTP;
        cparams_mtp.type_k        = params.speculative.draft.cache_type_k;
        cparams_mtp.type_v        = params.speculative.draft.cache_type_v;
        cparams_mtp.n_rs_seq      = 0;
        cparams_mtp.n_outputs_max = 1;
        cparams_mtp.ctx_other     = ctx_tgt;

        ctx_dft.reset(llama_init_from_model(model_tgt, cparams_mtp));
        if (!ctx_dft) {
            LOG_ERR("failed to create MTP context (model may lack MTP heads)\n");
            return result;
        }

        params.speculative.draft.ctx_tgt = ctx_tgt;
        params.speculative.draft.ctx_dft = ctx_dft.get();
    } else {
        // standalone draft model
        const auto & params_spec = params.speculative.draft;
        auto         params_dft  = params;
        params_dft.devices       = params_spec.devices;
        params_dft.model         = params_spec.mparams;
        params_dft.n_gpu_layers  = params_spec.n_gpu_layers;
        if (params_spec.cpuparams.n_threads > 0) {
            params_dft.cpuparams.n_threads       = params_spec.cpuparams.n_threads;
            params_dft.cpuparams_batch.n_threads = params_spec.cpuparams_batch.n_threads;
        }
        params_dft.tensor_buft_overrides = params_spec.tensor_buft_overrides;

        auto mparams_dft = common_model_params_to_llama(params_dft);
        model_dft.reset(llama_model_load_from_file(params_dft.model.path.c_str(), mparams_dft));
        if (!model_dft) {
            LOG_ERR("failed to load draft model: %s\n", params_dft.model.path.c_str());
            return result;
        }

        auto cparams = common_context_params_to_llama(params_dft);
        ctx_dft.reset(llama_init_from_model(model_dft.get(), cparams));

        params.speculative.draft.ctx_tgt = ctx_tgt;
        params.speculative.draft.ctx_dft = ctx_dft.get();
    }

    const bool use_ckpt_tgt = (common_context_can_seq_rm(ctx_tgt) == COMMON_CONTEXT_SEQ_RM_TYPE_FULL);
    const bool use_ckpt_dft = (common_context_can_seq_rm(ctx_dft.get()) == COMMON_CONTEXT_SEQ_RM_TYPE_FULL);


    // tokenize prompt
    llama_tokens inp = common_tokenize(ctx_tgt, params.prompt, true, true);

    result.prompt_text = params.prompt;

    if (llama_n_ctx(ctx_tgt) < (uint32_t) inp.size()) {
        LOG_ERR("prompt exceeds context size (%d tokens, ctx %d)\n", (int) inp.size(), (int) llama_n_ctx(ctx_tgt));
        return result;
    }

    // eval the prompt on the target context only
    // for MTP, the draft context shares KV cache with the target, so we must not
    // decode the prompt separately on the draft context (would cause M-RoPE position collision)
    // The MTP implementation handles draft context sync internally via common_speculative_begin
    const auto t_enc_start = ggml_time_us();

    llama_decode(ctx_tgt, llama_batch_get_one(inp.data(), inp.size() - 1));

    const auto t_enc_end = ggml_time_us();

    // target model sampler
    common_sampler_ptr smpl(common_sampler_init(model_tgt, params.sampling));

    // init speculative
    struct common_speculative * spec = common_speculative_init(params.speculative, 1);
    if (!spec) {
        LOG_ERR("failed to init speculative decoding\n");
        return result;
    }

    // initialize XGrammar plugin if present (v3.0.0 API)
    if (common_speculative_has_det_filter(spec)) {
        struct llama_deterministic_draft * plugin = common_speculative_get_det_filter_plugin(spec);
        if (plugin) {
            // get vocabulary
            const int vocab_size = llama_vocab_n_tokens(vocab);
            std::vector<std::string> vocab_strings;
            vocab_strings.reserve(vocab_size);
            for (int i = 0; i < vocab_size; i++) {
                std::string token_str = common_token_to_piece(ctx_tgt, i, false);
                vocab_strings.push_back(token_str);
            }

            // convert to C array
            std::vector<const char*> vocab_ptrs;
            vocab_ptrs.reserve(vocab_size);
            for (const auto& s : vocab_strings) {
                vocab_ptrs.push_back(s.c_str());
            }

            // get stop tokens
            std::vector<int32_t> stop_tokens;
            for (int i = 0; i < vocab_size; i++) {
                if (llama_vocab_is_eog(vocab, i)) {
                    stop_tokens.push_back(i);
                }
            }

            // set vocabulary
            LOG("setting vocabulary for plugin\n");
            if (!llama_deterministic_draft_set_vocab(plugin, vocab_ptrs.data(), vocab_size,
                                                      stop_tokens.data(), stop_tokens.size())) {
                LOG_WRN("failed to set vocabulary for deterministic draft plugin\n");
            }
            LOG("vocabulary set complete\n");

            // Grammar loading is now handled by the plugin via
            // DETERMINISTIC_DRAFT_LANGUAGE env var (set during set_vocab).
            // No need to call set_grammar here.
            if (cfg.language.empty()) {
                LOG("Hint: set DETERMINISTIC_DRAFT_LANGUAGE env var to enable grammar-constrained decoding\n");
            }
        }
    }

    llama_seq_id seq_id = 0;

    llama_tokens prompt_tgt = inp;
    prompt_tgt.reserve(llama_n_ctx(ctx_tgt));

    // always call common_speculative_begin to initialize draft context
    LOG("calling common_speculative_begin\n");
    common_speculative_begin(spec, seq_id, prompt_tgt);
    LOG("common_speculative_begin complete\n");
    // Skip grammar priming - start from scratch to avoid corrupted state

    llama_batch batch_tgt = llama_batch_init(llama_n_batch(ctx_tgt), 0, 1);

    llama_token id_last = inp.back();
    int         n_past  = inp.size() - 1;

    int n_predict       = 0;
    int n_drafted_pre   = 0;
    int n_drafted_post  = 0;
    int n_accept        = 0;
    int n_det_truncated = 0;

    bool has_eos = false;

    llama_tokens draft;
    common_ckpt  ckpt;

    size_t n_draft_offered = 0; // original draft size (used for partial acceptance check)

    const auto t_dec_start = ggml_time_us();

    int n_empty_iters = 0; // consecutive iterations with 0 post-filter drafts

    // Deterministic-draft grammar filtering (if a plugin is loaded) happens
    // inside common_speculative_draft()/common_speculative_accept() - this
    // benchmark just drives the standard speculative loop and reports on it,
    // it does not reimplement any of that logic itself.
    while (true) {
        if (draft.empty()) {
            ckpt.update_pos(prompt_tgt.size(), llama_memory_seq_pos_min(llama_get_memory(ctx_tgt), seq_id),
                            llama_memory_seq_pos_max(llama_get_memory(ctx_tgt), seq_id));

            if (use_ckpt_dft) {
                ckpt.update_dft(ctx_dft.get(), seq_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
            }

            common_speculative_get_draft_params(spec, seq_id) = {
                /* .drafting = */ true,
                /* .n_max    = */ -1,
                /* .n_past   = */ n_past,
                /* .id_last  = */ id_last,
                /* .prompt   = */ &prompt_tgt,
                /* .result   = */ &draft,
            };

            common_speculative_draft(spec);

            n_draft_offered = draft.size();

            // track raw draft size (before det filter) via filter result
            size_t n_draft_raw = draft.size();
            if (common_speculative_has_det_filter(spec)) {
                const auto & fr = common_speculative_get_det_filter_result(spec, seq_id);
                n_draft_raw     = fr.valid_count + (fr.truncated ? 1 : 0);  // pre-filter size
                if (fr.truncated) {
                    n_det_truncated++;
                }
            }
            n_drafted_pre += n_draft_raw;
            n_drafted_post += draft.size();

            if (!draft.empty() && use_ckpt_tgt) {
                ckpt.update_tgt(ctx_tgt, seq_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
            }

            if (use_ckpt_dft) {
                ckpt.load_dft(ctx_dft.get(), seq_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
            }

            // always clean up stale draft positions in the KV cache
            // (the deterministic filter may have truncated the draft, leaving orphaned positions)
            llama_memory_seq_rm(llama_get_memory(ctx_dft.get()), seq_id, ckpt.pos_max + 1, -1);

        } else {
            if (use_ckpt_tgt) {
                GGML_ASSERT(ckpt.pos_max >= 0);
            }
        }

        // build target batch: [id_last, draft0, draft1, ...]
        common_batch_clear(batch_tgt);
        common_batch_add(batch_tgt, id_last, n_past++, { seq_id }, true);

        for (size_t i = 0; i < draft.size(); ++i) {
            common_batch_add(batch_tgt, draft[i], n_past + i, { seq_id }, true);
        }

        llama_decode(ctx_tgt, batch_tgt);

        common_speculative_process(spec, batch_tgt);

        // save sampler state for potential restore
        common_sampler_ptr smpl_save;
        if (use_ckpt_tgt) {
            smpl_save.reset(common_sampler_clone(smpl.get()));
        }

        // sample and accept via shared speculative function
        // (handles accept-all mode with deterministic filter internally)
        std::vector<int> idxs(draft.size() + 1);
        for (size_t i = 0; i < idxs.size(); ++i) {
            idxs[i] = i;
        }
        llama_tokens ids = common_speculative_sample_and_accept(
            spec, smpl.get(), ctx_tgt, idxs, draft, seq_id);

        GGML_ASSERT(ids.size() > 0);

        // partial acceptance handling
        if (use_ckpt_tgt && ids.size() - 1 < n_draft_offered) {
            draft = std::move(ids);

            ckpt.load_tgt(ctx_tgt, seq_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
            llama_memory_seq_rm(llama_get_memory(ctx_tgt), seq_id, ckpt.pos_max + 1, -1);

            ckpt.load_dft(ctx_dft.get(), seq_id, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
            llama_memory_seq_rm(llama_get_memory(ctx_dft.get()), seq_id, ckpt.pos_max + 1, -1);

            prompt_tgt.resize(ckpt.n_tokens);
            smpl   = std::move(smpl_save);
            n_past = (int) prompt_tgt.size();

            continue;
        }

        common_speculative_accept(spec, seq_id, ids.size() - 1, ids.back());

        // full acceptance
        n_past += ids.size() - 1;
        n_accept += ids.size() - 1;
        n_predict += ids.size();

        for (size_t i = 0; i < ids.size(); ++i) {
            prompt_tgt.push_back(id_last);
            id_last = ids[i];

            auto piece = common_token_to_piece(ctx_tgt, id_last, false);
            result.generated_text += piece;

            if (llama_vocab_is_eog(vocab, id_last)) {
                has_eos = true;
                break;
            }
        }

        draft.clear();

        llama_memory_seq_rm(llama_get_memory(ctx_tgt), seq_id, n_past, -1);
        llama_memory_seq_rm(llama_get_memory(ctx_dft.get()), seq_id, n_past, -1);

        if ((params.n_predict >= 0 && n_predict > params.n_predict) || has_eos) {
            break;
        }

        // progress guard: if the deterministic filter repeatedly rejects all
        // drafts, the loop would spin at 1 token/iteration (target only, no
        // drafts accepted) which is indistinguishable from a hang at low tps.
        const bool draft_was_empty = n_drafted_post == 0 && !draft.empty();
        if (draft_was_empty) {
            n_empty_iters++;
            if (n_empty_iters >= 5) {
                LOG_WRN("det filter rejecting all drafts for %d consecutive iterations (n_predict=%d) - breaking\n",
                        n_empty_iters, n_predict);
                break;
            }
        } else {
            n_empty_iters = 0;
        }
    }

    const auto t_dec_end = ggml_time_us();

    result.t_enc_sec       = (t_enc_end - t_enc_start) / 1e6f;
    result.t_dec_sec       = (t_dec_end - t_dec_start) / 1e6f;
    result.n_input         = inp.size();
    result.n_predict       = n_predict;
    result.n_drafted_pre   = n_drafted_pre;
    result.n_drafted_post  = n_drafted_post;
    result.n_accepted      = n_accept;
    result.n_det_truncated = n_det_truncated;
    result.tps             = n_predict / result.t_dec_sec;
    result.accept_rate     = n_drafted_post > 0 ? 100.0 * n_accept / n_drafted_post : 0.0;

    // validate generated output against real parser when language is known
    if (!cfg.language.empty()) {
        std::string full_code = result.prompt_text + result.generated_text;
        std::string err;
        result.output_valid = validate_output(full_code, cfg.language, err);
        result.output_error = err;
        if (!result.output_valid) {
            LOG_ERR("output validation FAILED (%s): %s\n", cfg.language.c_str(), err.c_str());
            fprintf(stderr, "\n--- generated text (run %d) ---\n%s\n--- end ---\n",
                    run_idx, result.generated_text.c_str());
        } else {
            LOG("output VALID (%s): generated %zu chars\n", cfg.language.c_str(), full_code.size());
            fprintf(stderr, "\n=== generated text (run %d, VALID) ===\n%s%s\n=== end ===\n",
                    run_idx, result.prompt_text.c_str(), result.generated_text.c_str());
        }
    }

    LOG("encoded %d tokens in %.3f sec, speed: %.2f t/s\n", result.n_input, result.t_enc_sec,
        result.n_input / result.t_enc_sec);
    LOG("decoded %d tokens in %.3f sec, speed: %.2f t/s\n", result.n_predict, result.t_dec_sec, result.tps);
    LOG("drafted (pre-filter): %d, (post-filter): %d, accepted: %d (%.1f%%), det truncated: %d\n", result.n_drafted_pre,
        result.n_drafted_post, result.n_accepted, result.accept_rate, result.n_det_truncated);

    common_speculative_print_stats(spec);

    llama_batch_free(batch_tgt);
    common_speculative_free(spec);

    return result;
}
