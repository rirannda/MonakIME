// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
// Native llama.cpp adapter. Input protocol and output validation live in Rust.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <ggml-cpu.h>
#include <limits>
#include <llama.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#define MONAKIME_WITH_LLAMA 1
using Clock = std::chrono::steady_clock;
struct ZenzConfig {
    std::string model_path;
    int threads = 2, context_size = 512, max_tokens = 128, budget_ms = 500;
};
struct Backend {
    explicit Backend(ZenzConfig value) : config(std::move(value)) {
        config.threads = std::clamp(config.threads, 1, 8);
        config.context_size = std::clamp(config.context_size, 256, 1024);
        config.max_tokens = std::clamp(config.max_tokens, 16, 256);
        config.budget_ms = std::clamp(config.budget_ms, 10, 5000);
    }
    ZenzConfig config;
    std::string error;
    std::string load_error;
    bool load_failed = false;
#ifdef MONAKIME_WITH_LLAMA
    llama_model *model = nullptr;
    llama_context *context = nullptr;
    ggml_threadpool_t pool = nullptr;
    struct AbortState {
        const std::function<bool()> *cancelled = nullptr;
        Clock::time_point deadline = Clock::time_point::max();
        bool stopped = false;
    };

    static bool abort_callback(void *user) {
        auto &state = *static_cast<AbortState *>(user);
        state.stopped = (state.cancelled && *state.cancelled && (*state.cancelled)()) ||
                        Clock::now() >= state.deadline;
        return state.stopped;
    }
    static bool load_callback(float, void *user) {
        return !abort_callback(user);
    }

    bool load(AbortState &abort) {
        if (context)
            return true;
        if (load_failed) {
            error = load_error;
            return false;
        }
        std::error_code file_error;
        if (config.model_path.empty() ||
            !std::filesystem::is_regular_file(config.model_path, file_error)) {
            error = "zenz model file not found: " + config.model_path;
            load_error = error;
            load_failed = true;
            return false;
        }
        static std::once_flag backend_once;
        std::call_once(backend_once, [] {
            llama_backend_init();
            // Avoid megabytes of startup diagnostics in an fcitx session log.
            llama_log_set(
                [](ggml_log_level level, const char *message, void *) {
                    if (level == GGML_LOG_LEVEL_ERROR)
                        std::fputs(message, stderr);
                },
                nullptr);
        });
        auto mp = llama_model_default_params();
        mp.n_gpu_layers = 0;
        mp.load_mode = LLAMA_LOAD_MODE_MMAP;
        mp.use_extra_bufts = false;
        mp.progress_callback = load_callback;
        mp.progress_callback_user_data = &abort;
        // Zenz's character vocabulary is encoded as GPT-2 BPE in the GGUF.
        // Upstream llama.cpp does not recognize its custom pre-tokenizer name.
        llama_model_kv_override overrides[2]{};
        std::strcpy(overrides[0].key, "tokenizer.ggml.pre");
        overrides[0].tag = LLAMA_KV_OVERRIDE_TYPE_STR;
        std::strcpy(overrides[0].val_str, "gpt-2");
        mp.kv_overrides = overrides;
        model = llama_model_load_from_file(config.model_path.c_str(), mp);
        if (!model) {
            error = abort.stopped ? "zenz loading cancelled" : "cannot load zenz GGUF model";
            load_error = error;
            load_failed = !abort.stopped;
            return false;
        }
        auto cp = llama_context_default_params();
        cp.n_ctx = std::min(config.context_size, llama_model_n_ctx_train(model));
        cp.n_batch = cp.n_ctx;
        cp.n_ubatch = std::min<uint32_t>(64, cp.n_batch);
        cp.n_seq_max = 1;
        cp.n_outputs_max = 1;
        cp.n_outputs_max_per_seq = 1;
        cp.n_threads = config.threads;
        cp.n_threads_batch = config.threads;
        cp.offload_kqv = false;
        cp.op_offload = false;
        cp.no_perf = true;
        cp.abort_callback = abort_callback;
        cp.abort_callback_data = &abort;
        context = llama_init_from_model(model, cp);
        if (!context) {
            error = "cannot create zenz CPU context";
            load_error = error;
            release();
            load_failed = true;
            return false;
        }
        auto tp = ggml_threadpool_params_default(config.threads);
        tp.poll = 0; // No busy waiting between keystrokes or decode operations.
        tp.paused = true;
        pool = ggml_threadpool_new(&tp);
        if (!pool) {
            error = "cannot create zenz CPU threadpool";
            load_error = error;
            release();
            load_failed = true;
            return false;
        }
        llama_attach_threadpool(context, pool, pool);
        return true;
    }

    void release() {
        if (context) {
            llama_detach_threadpool(context);
            llama_free(context);
            context = nullptr;
        }
        if (pool) {
            ggml_threadpool_free(pool);
            pool = nullptr;
        }
        if (model) {
            llama_model_free(model);
            model = nullptr;
        }
    }
#else
    void release() {}
#endif
    ~Backend() {
        release();
    }
};

extern "C" {
void *monakime_llama_create(const char *path, int threads, int budget_ms) {
    return new Backend(ZenzConfig{path, threads, 512, 128, budget_ms});
}
void monakime_llama_destroy(void *handle) {
    delete static_cast<Backend *>(handle);
}
bool monakime_llama_loaded(void *handle) {
    return static_cast<Backend *>(handle)->context != nullptr;
}
const char *monakime_llama_error(void *handle) {
    return static_cast<Backend *>(handle)->error.c_str();
}
int monakime_llama_generate(void *handle, const char *input, bool (*cancel)(void *), void *user,
                          char *out, size_t capacity) {
    auto *engine = static_cast<Backend *>(handle);
    engine->error.clear();
    const std::string prompt(input);
    const std::function<bool()> cancelled = [cancel, user] { return cancel && cancel(user); };
    Backend::AbortState abort{&cancelled, Clock::time_point::max(), false};
    if (!engine->load(abort))
        return -1;
    // The cold load runs off the UI thread and supports cancellation; the
    // inference budget starts after it so a first conversion can still succeed.
    abort.deadline = Clock::now() + std::chrono::milliseconds(engine->config.budget_ms);
    llama_set_abort_callback(engine->context, Backend::abort_callback, &abort);
    struct Cleanup {
        Backend &engine;
        ~Cleanup() {
            llama_set_abort_callback(engine.context, nullptr, nullptr);
            llama_memory_clear(llama_get_memory(engine.context), true);
            ggml_threadpool_pause(engine.pool);
        }
    } cleanup{*engine};
    if (Backend::abort_callback(&abort)) {
        engine->error = "zenz cancelled";
        return -1;
    }
    ggml_threadpool_resume(engine->pool);
    const auto *vocab = llama_model_get_vocab(engine->model);
    std::vector<llama_token> tokens(engine->config.context_size);
    const int count =
        llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()), tokens.data(),
                       static_cast<int32_t>(tokens.size()), true, false);
    const auto context_size = llama_n_ctx(engine->context);
    if (count <= 0 || static_cast<uint32_t>(count + 2) >= context_size) {
        engine->error = "zenz prompt does not fit context";
        return -1;
    }
    tokens.resize(count);
    llama_memory_clear(llama_get_memory(engine->context), false);
    if (llama_decode(engine->context, llama_batch_get_one(tokens.data(), count)) != 0) {
        engine->error =
            abort.stopped ? "zenz inference cancelled or timed out" : "zenz prompt decode failed";
        return -1;
    }
    const int n_vocab = llama_vocab_n_tokens(vocab);
    const int max_tokens =
        std::min(engine->config.max_tokens, static_cast<int>(context_size) - count - 1);
    std::string output;
    output.reserve(std::min<size_t>(512, 1024));
    bool complete = false;
    for (int n = 0; n < max_tokens; ++n) {
        if (Backend::abort_callback(&abort))
            break;
        const float *logits = llama_get_logits_ith(engine->context, -1);
        if (!logits) {
            engine->error = "zenz logits unavailable";
            return -1;
        }
        llama_token next = 0;
        float best = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < n_vocab; ++i) {
            if (logits[i] > best) {
                next = i;
                best = logits[i];
            }
        }
        if (!std::isfinite(best)) {
            engine->error = "invalid zenz logits";
            return -1;
        }
        if (llama_vocab_is_eog(vocab, next)) {
            complete = true;
            break;
        }
        char piece[256];
        int bytes = llama_token_to_piece(vocab, next, piece, sizeof(piece), 0, false);
        if (bytes <= 0 || bytes > static_cast<int>(sizeof(piece))) {
            engine->error = "invalid zenz output token";
            return -1;
        }
        output.append(piece, static_cast<size_t>(bytes));
        if (output.size() > 1024)
            break;
        if (llama_decode(engine->context, llama_batch_get_one(&next, 1)) != 0) {
            engine->error =
                abort.stopped ? "zenz inference cancelled or timed out" : "zenz generation failed";
            return -1;
        }
    }
    if (!complete || abort.stopped) {
        engine->error =
            abort.stopped ? "zenz inference cancelled or timed out" : "zenz token limit reached";
        return -1; // A partial completion must never be committed.
    }
    if (output.size() >= capacity) {
        engine->error = "zenz output buffer exceeded";
        return -1;
    }
    std::memcpy(out, output.data(), output.size());
    return static_cast<int>(output.size());
}
}
