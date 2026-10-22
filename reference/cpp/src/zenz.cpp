// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/zenz.h"
#include "newime/text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#ifdef NEWIME_WITH_LLAMA
#include <ggml-cpu.h>
#include <llama.h>
#endif

namespace newime {
namespace {
using Clock = std::chrono::steady_clock;

bool private_or_control(char32_t cp) {
    return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || (cp >= 0xe000 && cp <= 0xf8ff) ||
           (cp >= 0xf0000 && cp <= 0xffffd) || (cp >= 0x100000 && cp <= 0x10fffd) || cp == 0xfffd;
}

bool valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const auto a = static_cast<unsigned char>(s[i]);
        size_t count = 0;
        char32_t cp = 0;
        if (a < 0x80) {
            count = 1;
            cp = a;
        } else if (a >= 0xc2 && a <= 0xdf) {
            count = 2;
            cp = a & 0x1f;
        } else if (a >= 0xe0 && a <= 0xef) {
            count = 3;
            cp = a & 0x0f;
        } else if (a >= 0xf0 && a <= 0xf4) {
            count = 4;
            cp = a & 7;
        } else
            return false;
        if (i + count > s.size())
            return false;
        for (size_t n = 1; n < count; ++n) {
            const auto b = static_cast<unsigned char>(s[i + n]);
            if ((b & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (b & 0x3f);
        }
        if ((count == 2 && cp < 0x80) || (count == 3 && cp < 0x800) ||
            (count == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        i += count;
    }
    return true;
}

std::string clean_context(std::string_view input) {
    // Bound allocation even for a CLI caller that passes a very large context.
    if (input.size() > 4096) {
        input.remove_prefix(input.size() - 4096);
        while (!input.empty() && (static_cast<unsigned char>(input.front()) & 0xc0) == 0x80)
            input.remove_prefix(1);
    }
    if (!valid_utf8(input))
        return {};
    auto cps = text::utf8ToU32(input);
    std::u32string clean;
    clean.reserve(std::min<size_t>(cps.size(), 96));
    const size_t start = cps.size() > 96 ? cps.size() - 96 : 0;
    for (size_t i = start; i < cps.size(); ++i) {
        if (!private_or_control(cps[i]))
            clean.push_back(cps[i] == U' ' ? U'　' : cps[i]);
        else if (cps[i] == U'\n' || cps[i] == U'\t')
            clean.push_back(U'　');
    }
    return text::u32ToUtf8(clean);
}
} // namespace

struct ZenzEngine::Impl {
    explicit Impl(ZenzConfig value) : config(std::move(value)) {
        config.threads = std::clamp(config.threads, 1, 8);
        config.context_size = std::clamp(config.context_size, 256, 1024);
        config.max_tokens = std::clamp(config.max_tokens, 16, 256);
        config.budget_ms = std::clamp(config.budget_ms, 10, 5000);
    }
    ZenzConfig config;
    std::string error;
    std::string load_error;
    bool load_failed = false;
#ifdef NEWIME_WITH_LLAMA
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
    ~Impl() {
        release();
    }
};

ZenzEngine::ZenzEngine(ZenzConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
ZenzEngine::~ZenzEngine() = default;

bool ZenzEngine::compiled_with_llama() noexcept {
#ifdef NEWIME_WITH_LLAMA
    return true;
#else
    return false;
#endif
}
bool ZenzEngine::loaded() const noexcept {
#ifdef NEWIME_WITH_LLAMA
    return impl_->context != nullptr;
#else
    return false;
#endif
}
const std::string &ZenzEngine::last_error() const noexcept {
    return impl_->error;
}
void ZenzEngine::unload() {
    impl_->release();
    impl_->load_failed = false;
    impl_->load_error.clear();
    impl_->error.clear();
}

std::string ZenzEngine::make_prompt(std::string_view reading, std::string_view left_context) {
    if (reading.empty() || reading.size() > 512 || !valid_utf8(reading))
        return {};
    const auto chars = text::utf8ToU32(reading);
    if (chars.size() > 128 || std::any_of(chars.begin(), chars.end(), private_or_control))
        return {};
    std::string prompt;
    const auto context = clean_context(left_context);
    if (!context.empty())
        prompt = "\uEE02" + context;
    prompt += "\uEE00";
    auto katakana = text::utf8ToU32(text::katakana(reading));
    for (auto &cp : katakana)
        if (cp == U' ')
            cp = U'　';
    prompt += text::u32ToUtf8(katakana);
    prompt += "\uEE01";
    return prompt;
}

bool ZenzEngine::valid_output(std::string_view reading, std::string_view output) {
    if (output.empty() || output.size() > 1024 || !valid_utf8(output))
        return false;
    const auto result = text::utf8ToU32(output);
    if (result.size() > std::max<size_t>(8, text::codepoints(reading) * 2 + 4))
        return false;
    if (std::any_of(result.begin(), result.end(), private_or_control))
        return false;
    if (output.find("<s>") != std::string_view::npos ||
        output.find("</s>") != std::string_view::npos ||
        output.find("[PAD]") != std::string_view::npos)
        return false;
    return true;
}

std::optional<std::string> ZenzEngine::convert(std::string_view reading,
                                               std::string_view left_context,
                                               const std::function<bool()> &cancelled) {
    impl_->error.clear();
    if (cancelled && cancelled()) {
        impl_->error = "zenz cancelled";
        return std::nullopt;
    }
    const auto prompt = make_prompt(reading, left_context);
    if (prompt.empty()) {
        impl_->error = "invalid or oversized zenz input";
        return std::nullopt;
    }
#ifndef NEWIME_WITH_LLAMA
    impl_->error = "NewIME was built without llama.cpp";
    return std::nullopt;
#else
    Impl::AbortState abort{&cancelled, Clock::time_point::max(), false};
    if (!impl_->load(abort))
        return std::nullopt;
    // The cold load runs off the UI thread and supports cancellation; the
    // inference budget starts after it so a first conversion can still succeed.
    abort.deadline = Clock::now() + std::chrono::milliseconds(impl_->config.budget_ms);
    llama_set_abort_callback(impl_->context, Impl::abort_callback, &abort);
    struct Cleanup {
        Impl &engine;
        ~Cleanup() {
            llama_set_abort_callback(engine.context, nullptr, nullptr);
            llama_memory_clear(llama_get_memory(engine.context), true);
            ggml_threadpool_pause(engine.pool);
        }
    } cleanup{*impl_};
    if (Impl::abort_callback(&abort)) {
        impl_->error = "zenz cancelled";
        return std::nullopt;
    }
    ggml_threadpool_resume(impl_->pool);
    const auto *vocab = llama_model_get_vocab(impl_->model);
    std::vector<llama_token> tokens(impl_->config.context_size);
    const int count =
        llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()), tokens.data(),
                       static_cast<int32_t>(tokens.size()), true, false);
    const auto context_size = llama_n_ctx(impl_->context);
    if (count <= 0 || static_cast<uint32_t>(count + 2) >= context_size) {
        impl_->error = "zenz prompt does not fit context";
        return std::nullopt;
    }
    tokens.resize(count);
    llama_memory_clear(llama_get_memory(impl_->context), false);
    if (llama_decode(impl_->context, llama_batch_get_one(tokens.data(), count)) != 0) {
        impl_->error =
            abort.stopped ? "zenz inference cancelled or timed out" : "zenz prompt decode failed";
        return std::nullopt;
    }
    const int n_vocab = llama_vocab_n_tokens(vocab);
    const int max_tokens =
        std::min(impl_->config.max_tokens, static_cast<int>(context_size) - count - 1);
    std::string output;
    output.reserve(std::min<size_t>(reading.size() + 32, 1024));
    bool complete = false;
    for (int n = 0; n < max_tokens; ++n) {
        if (Impl::abort_callback(&abort))
            break;
        const float *logits = llama_get_logits_ith(impl_->context, -1);
        if (!logits) {
            impl_->error = "zenz logits unavailable";
            return std::nullopt;
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
            impl_->error = "invalid zenz logits";
            return std::nullopt;
        }
        if (llama_vocab_is_eog(vocab, next)) {
            complete = true;
            break;
        }
        char piece[256];
        int bytes = llama_token_to_piece(vocab, next, piece, sizeof(piece), 0, false);
        if (bytes <= 0 || bytes > static_cast<int>(sizeof(piece))) {
            impl_->error = "invalid zenz output token";
            return std::nullopt;
        }
        output.append(piece, static_cast<size_t>(bytes));
        if (output.size() > 1024)
            break;
        if (llama_decode(impl_->context, llama_batch_get_one(&next, 1)) != 0) {
            impl_->error =
                abort.stopped ? "zenz inference cancelled or timed out" : "zenz generation failed";
            return std::nullopt;
        }
    }
    if (!complete || abort.stopped) {
        impl_->error =
            abort.stopped ? "zenz inference cancelled or timed out" : "zenz token limit reached";
        return std::nullopt; // A partial completion must never be committed.
    }
    if (!valid_output(reading, output)) {
        impl_->error = "zenz output rejected by UTF-8/control/length checks";
        return std::nullopt;
    }
    return output;
#endif
}
} // namespace newime
