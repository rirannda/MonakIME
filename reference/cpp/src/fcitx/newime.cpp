// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime.h"
#include "newime/text.h"
#include <algorithm>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <filesystem>
#include <list>
#include <memory>
#include <utility>

namespace newime::fcitxaddon {
namespace {
constexpr size_t candidateLimit = 9;
class Word final : public fcitx::CandidateWord {
  public:
    Word(Engine *engine, size_t index, std::string text)
        : CandidateWord(fcitx::Text(std::move(text))), engine_(engine), index_(index) {}
    void select(fcitx::InputContext *ic) const override {
        engine_->choose(ic, index_);
    }

  private:
    Engine *engine_;
    size_t index_;
};

ZenzConfig modelConfig(const RuntimeConfig &config) {
    ZenzConfig result;
    result.model_path = config.model_path;
    result.threads = config.threads;
    result.budget_ms = config.inference_budget_ms;
    return result;
}
} // namespace

Engine::Engine(fcitx::Instance *instance)
    : instance_(instance), config_(RuntimeConfig::fromEnvironment()), converter_(dictionary_) {
    if (!config_.system_dictionary_path.empty())
        dictionary_.load(config_.system_dictionary_path);
    dictionary_.load(config_.dictionary_path);
    if (!config_.user_dictionary_path.empty())
        dictionary_.load(config_.user_dictionary_path, true);
    if (!config_.english_dictionary_path.empty())
        dictionary_.loadEnglish(config_.english_dictionary_path);
    if (config_.enable_learning)
        dictionary_.loadLearning(config_.learning_path);
    instance_->inputContextManager().registerProperty("newime-state", &stateFactory_);
    dispatcher_.attach(&instance_->eventLoop());
    std::error_code error;
    modelAvailable_ = ZenzEngine::compiled_with_llama() && !config_.model_path.empty() &&
                      std::filesystem::is_regular_file(config_.model_path, error);
    // Zenz stays unloaded until explicit conversion. Mozc has its own sleeping worker.
    if (modelAvailable_)
        worker_ = std::thread([this] { run(); });
    mozcAvailable_ = !config_.mozc_helper_path.empty() &&
                     std::filesystem::is_regular_file(config_.mozc_helper_path, error);
    if (mozcAvailable_)
        mozcWorker_ = std::thread([this] { runMozc(); });
    else if (!config_.mozc_helper_path.empty())
        FCITX_WARN() << "NewIME: Mozc helper unavailable; run scripts/setup-mozc.sh";
}

Engine::~Engine() {
    stopping_.store(true, std::memory_order_release);
    latest_.fetch_add(1, std::memory_order_relaxed);
    condition_.notify_one();
    mozcCondition_.notify_one();
    if (worker_.joinable())
        worker_.join();
    if (mozcWorker_.joinable())
        mozcWorker_.join();
    dispatcher_.detach();
    saveTimer_.reset();
    saveLearning();
}

bool Engine::sensitive(fcitx::InputContext *ic) const {
    const auto flags = ic->capabilityFlags();
    return flags.testAny(fcitx::CapabilityFlag::PasswordOrSensitive) ||
           flags.test(fcitx::CapabilityFlag::Disable);
}

void Engine::invalidate(State &state) {
    state.generation = latest_.fetch_add(1, std::memory_order_relaxed) + 1;
    state.pending = false;
    std::lock_guard lock(mutex_);
    nextJob_.reset();
    nextMozcJob_.reset();
    mozcCondition_.notify_one();
}

void Engine::clear(fcitx::InputContext *ic, bool context) {
    auto *state = ic->propertyFor(&stateFactory_);
    invalidate(*state);
    state->raw.clear();
    state->forced.clear();
    state->conversion = {};
    state->live = {};
    state->sentences.clear();
    state->sentence = state->segment = 0;
    state->converting = false;
    state->sentenceMode = true;
    state->neuralApplied = false;
    if (context)
        state->leftContext.clear();
    update(ic);
}

void Engine::activate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) {
    if (sensitive(event.inputContext()))
        clear(event.inputContext(), true);
    else
        update(event.inputContext());
}

void Engine::reset(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) {
    clear(event.inputContext(), true);
}

std::string Engine::subModeLabelImpl(const fcitx::InputMethodEntry &, fcitx::InputContext &ic) {
    return ic.propertyFor(&stateFactory_)->latin ? "A" : "あ";
}

std::string Engine::current(const State &state) const {
    if (!state.forced.empty())
        return state.forced;
    if (!state.converting) {
        const auto reading = converter_.compose(state.raw, true);
        return !state.live.candidates.empty() && reading.starts_with(state.live.reading)
                   ? state.live.candidates.front().text + reading.substr(state.live.reading.size())
                   : reading;
    }
    if (state.sentenceMode && state.sentence < state.sentences.size())
        return state.sentences[state.sentence].text;
    return state.conversion.text();
}

void Engine::update(fcitx::InputContext *ic) {
    auto *state = ic->propertyFor(&stateFactory_);
    auto &panel = ic->inputPanel();
    panel.reset();
    if (!state->raw.empty()) {
        fcitx::Text preedit;
        if (state->converting && !state->sentenceMode && state->forced.empty()) {
            int cursor = 0;
            for (size_t i = 0; i < state->conversion.segments.size(); ++i) {
                const auto segment = state->conversion.segments[i].text();
                auto format = fcitx::TextFormatFlags(fcitx::TextFormatFlag::Underline);
                if (i == state->segment)
                    format |= fcitx::TextFormatFlag::HighLight;
                preedit.append(segment, format);
                if (i <= state->segment)
                    cursor += static_cast<int>(segment.size());
            }
            preedit.setCursor(cursor);
        } else {
            auto text = !state->forced.empty() || state->converting
                            ? current(*state)
                            : converter_.compose(state->raw, false);
            if (!state->converting && state->forced.empty() && !state->live.candidates.empty() &&
                text.starts_with(state->live.reading))
                text =
                    state->live.candidates.front().text + text.substr(state->live.reading.size());
            preedit.append(text, fcitx::TextFormatFlag::Underline);
            preedit.setCursor(static_cast<int>(text.size()));
        }
        if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit))
            panel.setClientPreedit(preedit);
        else
            panel.setPreedit(preedit);
        if (state->converting && state->forced.empty()) {
            const auto &choices = state->sentenceMode
                                      ? state->sentences
                                      : state->conversion.segments[state->segment].candidates;
            const auto selection = state->sentenceMode
                                       ? state->sentence
                                       : state->conversion.segments[state->segment].selected;
            auto list = std::make_unique<fcitx::CommonCandidateList>();
            list->setPageSize(9);
            list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
            list->setSelectionKey({fcitx::Key("1"), fcitx::Key("2"), fcitx::Key("3"),
                                   fcitx::Key("4"), fcitx::Key("5"), fcitx::Key("6"),
                                   fcitx::Key("7"), fcitx::Key("8"), fcitx::Key("9")});
            for (size_t i = 0; i < choices.size(); ++i)
                list->append<Word>(this, i, choices[i].text);
            list->setGlobalCursorIndex(static_cast<int>(selection));
            panel.setCandidateList(std::move(list));
            panel.setAuxDown(fcitx::Text(state->pending ? "変換中…  ←→ 文節  Enter 確定"
                                                        : "←→ 文節  Shift+←→ 区切り  Enter 確定"));
        }
    }
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void Engine::begin(fcitx::InputContext *ic) {
    auto *state = ic->propertyFor(&stateFactory_);
    invalidate(*state);
    state->forced.clear();
    const auto reading = converter_.compose(state->raw, true);
    state->conversion = state->live.reading == reading && !state->live.segments.empty()
                            ? state->live
                            : converter_.convert(reading, candidateLimit);
    if (state->conversion.segments.empty())
        return;
    state->sentences = state->conversion.candidates;
    if (state->sentences.empty())
        state->sentences.push_back({state->conversion.text(), 0});
    state->sentence = state->segment = 0;
    state->converting = true;
    state->sentenceMode = true;
    state->neuralApplied = false;
    if (state->live.reading != reading)
        requestMozc(ic, true);
    request(ic);
    update(ic);
}

void Engine::cycle(fcitx::InputContext *ic, int direction) {
    auto *state = ic->propertyFor(&stateFactory_);
    invalidate(*state);
    auto &selection =
        state->sentenceMode ? state->sentence : state->conversion.segments[state->segment].selected;
    const auto count = state->sentenceMode
                           ? state->sentences.size()
                           : state->conversion.segments[state->segment].candidates.size();
    if (count)
        selection = (selection + count + direction) % count;
    update(ic);
}

void Engine::choose(fcitx::InputContext *ic, size_t index) {
    if (!ic || sensitive(ic))
        return;
    auto *state = ic->propertyFor(&stateFactory_);
    if (!state->converting)
        return;
    invalidate(*state);
    if (state->sentenceMode) {
        if (index >= state->sentences.size())
            return;
        state->sentence = index;
    } else {
        auto &segment = state->conversion.segments[state->segment];
        if (index >= segment.candidates.size())
            return;
        segment.selected = index;
    }
    update(ic);
}

void Engine::commit(fcitx::InputContext *ic) {
    auto *state = ic->propertyFor(&stateFactory_);
    if (state->raw.empty() || sensitive(ic))
        return;
    const auto result = current(*state);
    const auto reading = converter_.compose(state->raw, true);
    if (config_.enable_learning && state->forced.empty() &&
        (state->converting || state->live.reading == reading)) {
        dictionary_.learn(reading, result);
        if (!state->sentenceMode)
            for (const auto &segment : state->conversion.segments)
                if (!segment.literal)
                    dictionary_.learn(segment.reading, segment.text());
        scheduleLearningSave();
    }
    state->leftContext = text::suffix(state->leftContext + result, 64);
    ic->commitString(result);
    clear(ic);
}

void Engine::request(fcitx::InputContext *ic) {
    if (!modelAvailable_ || sensitive(ic))
        return;
    auto *state = ic->propertyFor(&stateFactory_);
    const auto chars = text::utf8ToU32(state->conversion.reading);
    if (std::none_of(chars.begin(), chars.end(), text::isKana))
        return;
    state->pending = true;
    {
        std::lock_guard lock(mutex_);
        nextJob_ =
            Job{ic->uuid(), state->generation, state->conversion.reading, state->leftContext};
    }
    condition_.notify_one();
}

void Engine::requestMozc(fcitx::InputContext *ic, bool explicitConversion) {
    if (!mozcAvailable_ || sensitive(ic))
        return;
    auto *state = ic->propertyFor(&stateFactory_);
    if (state->raw.empty() || !state->forced.empty())
        return;
    Job job{ic->uuid(),
            state->generation,
            converter_.compose(state->raw, explicitConversion),
            state->leftContext,
            explicitConversion,
            std::chrono::steady_clock::now() +
                std::chrono::milliseconds(explicitConversion ? 0 : config_.live_delay_ms)};
    {
        std::lock_guard lock(mutex_);
        nextMozcJob_ = std::move(job);
    }
    mozcCondition_.notify_one();
}

void Engine::runMozc() {
    MozcEngine engine(config_.mozc_helper_path, config_.mozc_profile_path);
    struct Cached {
        std::string reading, context;
        Conversion conversion;
        size_t bytes;
    };
    std::list<Cached> cache;
    size_t cacheBytes = 0;
    bool warned = false;
    while (!stopping_.load(std::memory_order_acquire)) {
        std::optional<Job> job;
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                mozcCondition_.wait(
                    lock, [this] { return stopping_.load() || nextMozcJob_.has_value(); });
                if (stopping_.load())
                    return;
                const auto deadline = nextMozcJob_->ready;
                if (std::chrono::steady_clock::now() < deadline &&
                    mozcCondition_.wait_until(lock, deadline) != std::cv_status::timeout)
                    continue;
                if (!nextMozcJob_)
                    continue;
                job = std::move(nextMozcJob_);
                nextMozcJob_.reset();
                break;
            }
        }
        const auto cancelled = [this, generation = job->generation] {
            return stopping_.load(std::memory_order_relaxed) ||
                   latest_.load(std::memory_order_relaxed) != generation;
        };
        if (cancelled())
            continue;
        std::optional<Conversion> result;
        const auto cached = std::find_if(cache.begin(), cache.end(), [&](const auto &entry) {
            return entry.reading == job->reading && entry.context == job->leftContext;
        });
        if (cached != cache.end()) {
            result = cached->conversion;
            cache.splice(cache.begin(), cache, cached);
        } else {
            result = engine.convert(job->reading, job->leftContext, cancelled);
            if (result && !cancelled()) {
                size_t bytes = sizeof(Cached) + job->reading.size() + job->leftContext.size() +
                               result->reading.size();
                for (const auto &segment : result->segments) {
                    bytes += sizeof(Segment) + segment.reading.size();
                    for (const auto &candidate : segment.candidates)
                        bytes += sizeof(Candidate) + candidate.text.size();
                }
                for (const auto &candidate : result->candidates)
                    bytes += sizeof(Candidate) + candidate.text.size();
                if (bytes <= 512 * 1024) {
                    cache.push_front({job->reading, job->leftContext, *result, bytes});
                    cacheBytes += bytes;
                }
                while (!cache.empty() && (cache.size() > 32 || cacheBytes > 512 * 1024)) {
                    cacheBytes -= cache.back().bytes;
                    cache.pop_back();
                }
            }
        }
        if (cancelled())
            continue;
        if (!result && !warned) {
            FCITX_WARN() << "NewIME: Mozc conversion unavailable: " << engine.lastError();
            warned = true;
        }
        dispatcher_.schedule([this, job = std::move(*job), result = std::move(result)]() mutable {
            auto *ic = instance_->inputContextManager().findByUUID(job.context);
            if (!ic || !ic->hasFocus() || sensitive(ic))
                return;
            auto *state = ic->propertyFor(&stateFactory_);
            if (state->generation != job.generation || !state->forced.empty() ||
                state->converting != job.explicitConversion ||
                converter_.compose(state->raw, job.explicitConversion) != job.reading)
                return;
            // Native fallback keeps live conversion usable if the external helper fails.
            if (!result)
                result = converter_.convert(job.reading, candidateLimit);
            converter_.rank(*result, candidateLimit);
            if (job.explicitConversion) {
                // Never replace a neural answer or a manually selected candidate.
                if (!state->sentenceMode || state->neuralApplied)
                    return;
                state->conversion = std::move(*result);
                state->sentences = state->conversion.candidates;
                state->sentence = state->segment = 0;
            } else
                state->live = std::move(*result);
            update(ic);
        });
    }
}

void Engine::scheduleLearningSave() {
    learningDirty_ = true;
    const auto deadline = fcitx::now(CLOCK_MONOTONIC) + 2000000;
    if (!saveTimer_) {
        saveTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, deadline, 100000, [this](fcitx::EventSourceTime *, uint64_t) {
                saveLearning();
                return true;
            });
    } else
        saveTimer_->setTime(deadline);
    saveTimer_->setOneShot();
}

void Engine::saveLearning() {
    if (!learningDirty_)
        return;
    if (dictionary_.saveLearning(config_.learning_path))
        learningDirty_ = false;
    else
        FCITX_WARN() << "NewIME: could not save learning data";
}

void Engine::run() {
    ZenzEngine model(modelConfig(config_));
    struct Cached {
        std::string reading;
        std::string context;
        std::string result;
    };
    std::list<Cached> cache;
    while (!stopping_.load(std::memory_order_acquire)) {
        std::optional<Job> job;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_.load() || nextJob_.has_value(); });
            if (stopping_.load())
                break;
            job = std::move(nextJob_);
            nextJob_.reset();
        }
        const auto cancelled = [this, generation = job->generation] {
            return stopping_.load(std::memory_order_relaxed) ||
                   latest_.load(std::memory_order_relaxed) != generation;
        };
        std::optional<std::string> result;
        const auto cached = std::find_if(cache.begin(), cache.end(), [&](const auto &entry) {
            return entry.reading == job->reading && entry.context == job->leftContext;
        });
        if (cached != cache.end()) {
            result = cached->result;
            cache.splice(cache.begin(), cache, cached);
        } else {
            result = model.convert(job->reading, job->leftContext, cancelled);
            if (result && !cancelled()) {
                cache.push_front({job->reading, job->leftContext, *result});
                if (cache.size() > 32)
                    cache.pop_back();
            }
        }
        if (cancelled())
            continue;
        dispatcher_.schedule([this, job = std::move(*job), result = std::move(result)] {
            auto *ic = instance_->inputContextManager().findByUUID(job.context);
            if (!ic || !ic->hasFocus() || sensitive(ic))
                return;
            auto *state = ic->propertyFor(&stateFactory_);
            if (state->generation != job.generation || !state->converting || !state->sentenceMode ||
                state->conversion.reading != job.reading)
                return;
            state->pending = false;
            if (result && !result->empty() &&
                converter_.checkReading(job.reading, *result) != ReadingCheck::Mismatch) {
                state->neuralApplied = true;
                auto learned = dictionary_.lookup(job.reading, 1);
                bool preferLearning = !learned.empty() && learned.front().score >= 4;
                for (const auto &segment : state->conversion.segments) {
                    learned = dictionary_.lookup(segment.reading, 1);
                    if (!learned.empty() && learned.front().score >= 4)
                        preferLearning = true;
                }
                if (!preferLearning || state->sentences.empty() ||
                    state->sentences.front().text != *result) {
                    std::erase_if(state->sentences,
                                  [&](const auto &entry) { return entry.text == *result; });
                    const auto position = preferLearning && !state->sentences.empty() ? 1 : 0;
                    state->sentences.insert(state->sentences.begin() + position, {*result, -100});
                }
                if (state->sentences.size() > candidateLimit)
                    state->sentences.resize(candidateLimit);
                state->sentence = 0;
            }
            update(ic);
        });
    }
}

void Engine::keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &event) {
    if (event.isRelease())
        return;
    auto *ic = event.inputContext();
    auto *state = ic->propertyFor(&stateFactory_);
    if (sensitive(ic)) {
        clear(ic, true);
        return;
    }
    const auto key = event.key();
    const auto sym = key.sym();
    const bool shift = key.states().test(fcitx::KeyState::Shift);
    const auto modifiers = fcitx::KeyStates(fcitx::KeyState::Ctrl) | fcitx::KeyState::Alt |
                           fcitx::KeyState::Super | fcitx::KeyState::Super2 |
                           fcitx::KeyState::Hyper | fcitx::KeyState::Meta;
    if (key.states().testAny(modifiers)) {
        // Preserve application shortcuts and avoid retaining hidden composition.
        if (!state->raw.empty() && !key.isModifier())
            commit(ic);
        return;
    }
    if (sym == FcitxKey_Muhenkan) {
        if (!state->raw.empty()) {
            invalidate(*state);
            state->forced = state->raw;
            update(ic);
        } else
            state->latin = true;
        event.filterAndAccept();
        return;
    }
    if (sym == FcitxKey_Henkan || sym == FcitxKey_Hiragana_Katakana) {
        state->latin = false;
        if (!state->raw.empty())
            begin(ic);
        event.filterAndAccept();
        return;
    }
    if (state->latin)
        return;
    if (sym == FcitxKey_space && !state->raw.empty()) {
        if (!state->converting || !state->forced.empty())
            begin(ic);
        else
            cycle(ic, shift ? -1 : 1);
        event.filterAndAccept();
        return;
    }
    if (state->raw.empty() && (sym == FcitxKey_space || key.isModifier()))
        return;
    if ((sym == FcitxKey_Return || sym == FcitxKey_KP_Enter) && !state->raw.empty()) {
        commit(ic);
        event.filterAndAccept();
        return;
    }
    if (sym == FcitxKey_Escape && !state->raw.empty()) {
        if (state->converting || !state->forced.empty()) {
            invalidate(*state);
            state->converting = false;
            state->forced.clear();
            state->live = {};
            update(ic);
        } else if (!state->live.candidates.empty()) {
            invalidate(*state);
            state->live = {};
            update(ic);
        } else
            clear(ic);
        event.filterAndAccept();
        return;
    }
    if (sym == FcitxKey_BackSpace && !state->raw.empty()) {
        invalidate(*state);
        if (state->converting || !state->forced.empty()) {
            state->converting = false;
            state->forced.clear();
            state->live = {};
        } else {
            const auto length = text::codepoints(state->raw);
            state->raw = text::prefix(state->raw, length ? length - 1 : 0);
        }
        if (!state->raw.empty() && !state->converting)
            requestMozc(ic);
        update(ic);
        event.filterAndAccept();
        return;
    }
    if (!state->raw.empty() && (sym == FcitxKey_F6 || sym == FcitxKey_F7 || sym == FcitxKey_F10)) {
        invalidate(*state);
        const auto reading = converter_.compose(state->raw, true);
        state->forced = sym == FcitxKey_F6   ? text::hiragana(reading)
                        : sym == FcitxKey_F7 ? text::katakana(reading)
                                             : state->raw;
        update(ic);
        event.filterAndAccept();
        return;
    }
    if (state->converting && state->forced.empty()) {
        if (sym == FcitxKey_Tab || sym == FcitxKey_ISO_Left_Tab || sym == FcitxKey_Up ||
            sym == FcitxKey_Down) {
            cycle(ic, sym == FcitxKey_Up || sym == FcitxKey_ISO_Left_Tab || shift ? -1 : 1);
            event.filterAndAccept();
            return;
        }
        if (sym == FcitxKey_Left || sym == FcitxKey_Right) {
            invalidate(*state);
            state->sentenceMode = false;
            if (shift)
                converter_.resize(state->conversion, state->segment, sym == FcitxKey_Left ? -1 : 1);
            else if (sym == FcitxKey_Left && state->segment)
                --state->segment;
            else if (sym == FcitxKey_Right &&
                     state->segment + 1 < state->conversion.segments.size())
                ++state->segment;
            update(ic);
            event.filterAndAccept();
            return;
        }
        const auto digit = key.digitSelection();
        if (digit >= 0 && digit < 9) {
            choose(ic, static_cast<size_t>(digit));
            event.filterAndAccept();
            return;
        }
    }
    const auto character = fcitx::Key::keySymToUnicode(sym);
    if (character >= 0x21 && character != 0x7f) {
        if (state->converting || !state->forced.empty())
            commit(ic);
        const auto added = fcitx::Key::keySymToUTF8(sym);
        if (text::codepoints(state->raw) + text::codepoints(added) > config_.max_input_chars)
            commit(ic);
        invalidate(*state);
        state->raw += added;
        requestMozc(ic);
        update(ic);
        event.filterAndAccept();
        return;
    }
    if (!state->raw.empty() &&
        (sym == FcitxKey_Tab || key.isCursorMove() || sym == FcitxKey_Delete))
        commit(ic);
}
} // namespace newime::fcitxaddon
