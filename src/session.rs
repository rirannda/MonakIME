// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! IME behavior, independent of fcitx. Raw input remains editable during live conversion.
//! Native code translates keys to `Key`, and renders the returned `View`.
use crate::{
    compose::{SourceSpan, compose, compose_with_spans},
    config::Config,
    converter::{self, Conversion, ReadingCheck},
    dictionary::Dictionary,
    text,
    worker::{Answer, Job, Wake, Workers},
};
use std::{
    collections::HashMap,
    time::{Duration, Instant},
};

#[derive(Debug)]
pub enum Key {
    Text(String),
    Convert,
    Enter,
    Escape,
    Backspace,
    Left,
    Right,
    Previous,
    Next,
    Hiragana,
    Katakana,
    LatinText,
    Muhenkan,
    JapaneseMode,
    Shortcut,
    OtherCommit,
    Select(usize),
    Modifier,
}
#[derive(Default)]
pub struct View {
    pub preedit: String,
    pub commit: String,
    pub auxiliary: String,
    pub candidates: Vec<String>,
    pub cursor: usize,
    pub highlight_start: usize,
    pub highlight_end: usize,
    pub selection: usize,
    pub accepted: bool,
    pub latin: bool,
    pub save_requested: bool,
}
#[derive(Default)]
struct Session {
    raw: String,
    // Backspace can leave an unfinished English word (google -> googl).
    // Preserve that prefix until the normal composer can recognize it again.
    protected_prefix: usize,
    forced: String,
    left_context: String,
    conversion: Conversion,
    live: Conversion,
    sentence: usize,
    segment: usize,
    generation: u64,
    converting: bool,
    segmented: bool,
    pending: bool,
    latin: bool,
    neural_applied: bool,
    // Created only when the user edits a boundary. Live typing keeps no source map.
    source_spans: Option<Vec<SourceSpan>>,
}
struct Context<'a> {
    id: u64,
    config: &'a Config,
    dictionary: &'a mut Dictionary,
    workers: &'a Workers,
}
impl Session {
    fn reading(&self, dictionary: &Dictionary, final_input: bool) -> String {
        let reading = compose(dictionary, &self.raw, final_input);
        let prefix = &self.raw[..self.protected_prefix];
        if reading.starts_with(prefix) {
            reading
        } else {
            prefix.to_owned()
                + &compose(dictionary, &self.raw[self.protected_prefix..], final_input)
        }
    }
    fn reading_with_spans(
        &self,
        dictionary: &Dictionary,
        final_input: bool,
    ) -> (String, Vec<SourceSpan>) {
        let (reading, spans) = compose_with_spans(dictionary, &self.raw, final_input);
        let prefix = &self.raw[..self.protected_prefix];
        if reading.starts_with(prefix) {
            return (reading, spans);
        }
        let (tail, mut spans) =
            compose_with_spans(dictionary, &self.raw[self.protected_prefix..], final_input);
        for span in &mut spans {
            span.raw = span.raw.start + self.protected_prefix..span.raw.end + self.protected_prefix;
            span.reading = span.reading.start + prefix.len()..span.reading.end + prefix.len();
        }
        let mut mapped: Vec<_> = prefix
            .char_indices()
            .map(|(i, c)| SourceSpan {
                raw: i..i + c.len_utf8(),
                reading: i..i + c.len_utf8(),
            })
            .collect();
        mapped.extend(spans);
        (prefix.to_owned() + &tail, mapped)
    }
    fn invalidate(&mut self, ctx: &Context<'_>) {
        self.generation = ctx.workers.invalidate();
        self.pending = false;
    }
    fn clear(&mut self, ctx: &Context<'_>, context: bool) {
        self.invalidate(ctx);
        let latin = self.latin;
        let left_context = if context {
            String::new()
        } else {
            std::mem::take(&mut self.left_context)
        };
        let generation = self.generation;
        *self = Self {
            latin,
            left_context,
            generation,
            ..Self::default()
        };
    }
    fn current(&self, dictionary: &Dictionary, final_input: bool) -> String {
        if !self.forced.is_empty() {
            return self.forced.clone();
        }
        if self.converting {
            return if self.segmented {
                self.conversion.text()
            } else {
                self.conversion
                    .candidates
                    .get(self.sentence)
                    .map_or_else(|| self.conversion.text(), |c| c.text.clone())
            };
        }
        let reading = self.reading(dictionary, final_input);
        if !self.live.candidates.is_empty() && reading.starts_with(&self.live.reading) {
            self.live.candidates[0].text.clone() + &reading[self.live.reading.len()..]
        } else {
            reading
        }
    }
    fn view(&self, dictionary: &Dictionary) -> View {
        let mut out = View {
            latin: self.latin,
            ..View::default()
        };
        if self.raw.is_empty() {
            return out;
        }
        out.preedit = self.current(dictionary, self.converting || !self.forced.is_empty());
        out.cursor = out.preedit.len();
        if self.converting && self.forced.is_empty() {
            let choices = if self.segmented {
                for (i, segment) in self.conversion.segments.iter().enumerate() {
                    if i < self.segment {
                        out.highlight_start += segment.text().len();
                    }
                }
                let segment = &self.conversion.segments[self.segment];
                out.highlight_end = out.highlight_start + segment.text().len();
                out.cursor = out.highlight_end;
                out.selection = segment.selected;
                &segment.candidates
            } else {
                out.selection = self.sentence;
                &self.conversion.candidates
            };
            out.candidates = choices.iter().map(|c| c.text.clone()).collect();
            out.auxiliary = if self.pending {
                "変換中…  ←→ 文節  Enter 確定"
            } else {
                "←→ 文節  Shift+←→ 区切り  Enter 確定"
            }
            .into();
        }
        out
    }
    fn request_mozc(&self, ctx: &Context<'_>, explicit: bool) {
        if !ctx.workers.mozc_enabled || self.raw.is_empty() || !self.forced.is_empty() {
            return;
        }
        ctx.workers.submit(
            0,
            Job {
                context: ctx.id,
                generation: self.generation,
                reading: self.reading(ctx.dictionary, explicit),
                left_context: self.left_context.clone(),
                explicit,
                ready: Instant::now()
                    + Duration::from_millis(if explicit {
                        0
                    } else {
                        ctx.config.live_delay_ms
                    }),
            },
        );
    }
    fn begin(&mut self, ctx: &Context<'_>) {
        self.invalidate(ctx);
        self.forced.clear();
        self.source_spans = None;
        let reading = self.reading(ctx.dictionary, true);
        self.conversion = if self.live.reading == reading && !self.live.segments.is_empty() {
            self.live.clone()
        } else {
            converter::convert(ctx.dictionary, &reading, 9)
        };
        if self.conversion.segments.is_empty() {
            return;
        }
        self.sentence = 0;
        self.segment = 0;
        self.converting = true;
        self.segmented = false;
        self.neural_applied = false;
        if self.live.reading != reading {
            self.request_mozc(ctx, true);
        }
        if ctx.workers.neural_enabled && reading.chars().any(text::is_kana) {
            self.pending = true;
            ctx.workers.submit(
                1,
                Job {
                    context: ctx.id,
                    generation: self.generation,
                    reading,
                    left_context: self.left_context.clone(),
                    explicit: true,
                    ready: Instant::now(),
                },
            );
        }
    }
    fn cycle(&mut self, ctx: &Context<'_>, direction: isize) {
        self.invalidate(ctx);
        let (selection, count) = if self.segmented {
            let segment = &mut self.conversion.segments[self.segment];
            (&mut segment.selected, segment.candidates.len())
        } else {
            (&mut self.sentence, self.conversion.candidates.len())
        };
        if count > 0 {
            *selection = (*selection as isize + direction).rem_euclid(count as isize) as usize;
        }
    }
    fn resize(&mut self, ctx: &Context<'_>, delta: i32) {
        if self.source_spans.is_none() {
            let (reading, spans) = self.reading_with_spans(ctx.dictionary, true);
            self.source_spans = Some(if reading == self.conversion.reading {
                spans
            } else {
                Vec::new()
            });
        }
        let spans = self.source_spans.as_ref().unwrap();
        if !converter::resize(ctx.dictionary, &mut self.conversion, self.segment, delta) {
            return;
        }
        let begin: usize = self.conversion.segments[..self.segment]
            .iter()
            .map(|segment| segment.reading.len())
            .sum();
        let segment = &mut self.conversion.segments[self.segment];
        if segment.literal && !spans.is_empty() {
            let end = begin + segment.reading.len();
            let mut spelling = String::new();
            for span in spans.iter() {
                let start = begin.max(span.reading.start);
                let stop = end.min(span.reading.end);
                if start >= stop {
                    continue;
                }
                let source = &self.raw[span.raw.clone()];
                let reading = &self.conversion.reading[span.reading.clone()];
                if start == span.reading.start && stop == span.reading.end {
                    spelling.push_str(source);
                } else if source == reading {
                    spelling
                        .push_str(&source[start - span.reading.start..stop - span.reading.start]);
                } else {
                    // A boundary can split きゃ into き/ゃ. Keep that partial
                    // syllable as kana instead of duplicating its original kya.
                    spelling.push_str(&self.conversion.reading[start..stop]);
                }
            }
            if !spelling.is_empty() && spelling != segment.reading {
                segment
                    .candidates
                    .insert(0, converter::Candidate::new(spelling, 0.1));
                segment.selected = 0;
                self.conversion.rebuild_candidates(9);
            }
        }
    }
    fn erase_reading(&mut self, dictionary: &Dictionary) {
        let (mut reading, spans) = self.reading_with_spans(dictionary, false);
        if reading.pop().is_none() {
            return;
        }
        let removed_at = reading.len();
        if let Some(index) = spans
            .iter()
            .position(|span| span.reading.start <= removed_at && removed_at < span.reading.end)
        {
            // Usually deleting one completed kana just removes its romaji
            // syllable. Preserve the earlier original spelling for F10.
            for span in spans[index.saturating_sub(1)..=index].iter().rev() {
                let mut edited = self.raw[..span.raw.start].to_owned();
                edited.push_str(&reading[span.reading.start.min(reading.len())..]);
                if compose(dictionary, &edited, false) == reading {
                    self.raw = edited;
                    self.protected_prefix = 0;
                    self.live = Conversion::default();
                    self.source_spans = None;
                    return;
                }
            }
        }
        // English recognition can change when its final letter is removed.
        // The visible reading is authoritative when that makes the raw prefix
        // ambiguous. This also keeps a completed ん from reverting to pending n.
        self.protected_prefix = reading.len();
        self.raw = reading;
        self.live = Conversion::default();
        self.source_spans = None;
    }
    fn choose(&mut self, ctx: &Context<'_>, index: usize) {
        if !self.converting {
            return;
        }
        self.invalidate(ctx);
        if self.segmented {
            let segment = &mut self.conversion.segments[self.segment];
            if index < segment.candidates.len() {
                segment.selected = index;
            }
        } else if index < self.conversion.candidates.len() {
            self.sentence = index;
        }
    }
    fn commit(&mut self, ctx: &mut Context<'_>, out: &mut View) {
        if self.raw.is_empty() {
            return;
        }
        let answer = self.current(ctx.dictionary, true);
        let reading = self.reading(ctx.dictionary, true);
        if ctx.config.enable_learning
            && self.forced.is_empty()
            && (self.converting || self.live.reading == reading)
        {
            ctx.dictionary.learn(&reading, &answer);
            if self.segmented {
                for segment in &self.conversion.segments {
                    if !segment.literal {
                        ctx.dictionary.learn(&segment.reading, segment.text());
                    }
                }
            }
            out.save_requested = true;
        }
        self.left_context.push_str(&answer);
        self.left_context = text::suffix(&self.left_context, 64).into();
        out.commit.push_str(&answer);
        self.clear(ctx, false);
    }
    fn key(&mut self, key: Key, shift: bool, ctx: &mut Context<'_>) -> View {
        let mut effect = View::default();
        if matches!(key, Key::Shortcut) {
            self.commit(ctx, &mut effect);
            return effect;
        }
        if matches!(key, Key::Modifier) {
            return effect;
        }
        if matches!(key, Key::Muhenkan) {
            if self.raw.is_empty() {
                self.latin = true;
            } else {
                self.invalidate(ctx);
                self.forced = self.raw.clone();
            }
            effect.accepted = true;
        } else if matches!(key, Key::JapaneseMode) {
            self.latin = false;
            if !self.raw.is_empty() {
                self.begin(ctx);
            }
            effect.accepted = true;
        } else if self.latin {
            return effect;
        } else if !self.raw.is_empty() && matches!(key, Key::Convert) {
            if !self.converting || !self.forced.is_empty() {
                self.begin(ctx);
            } else {
                self.cycle(ctx, if shift { -1 } else { 1 });
            }
            effect.accepted = true;
        } else if !self.raw.is_empty() && matches!(key, Key::Enter) {
            self.commit(ctx, &mut effect);
            effect.accepted = true;
        } else if !self.raw.is_empty() && matches!(key, Key::Escape) {
            if self.converting || !self.forced.is_empty() || !self.live.candidates.is_empty() {
                self.invalidate(ctx);
                self.converting = false;
                self.forced.clear();
                self.live = Conversion::default();
            } else {
                self.clear(ctx, false);
            }
            effect.accepted = true;
        } else if !self.raw.is_empty() && matches!(key, Key::Backspace) {
            self.invalidate(ctx);
            if self.converting || !self.forced.is_empty() {
                self.converting = false;
                self.forced.clear();
                self.live = Conversion::default();
            } else {
                self.erase_reading(ctx.dictionary);
            }
            self.request_mozc(ctx, false);
            effect.accepted = true;
        } else if !self.raw.is_empty()
            && matches!(key, Key::Hiragana | Key::Katakana | Key::LatinText)
        {
            self.invalidate(ctx);
            let reading = self.reading(ctx.dictionary, true);
            self.forced = match key {
                Key::Hiragana => text::hiragana(&reading),
                Key::Katakana => text::katakana(&reading),
                _ => self.raw.clone(),
            };
            effect.accepted = true;
        } else if self.converting
            && self.forced.is_empty()
            && matches!(key, Key::Previous | Key::Next)
        {
            self.cycle(
                ctx,
                if matches!(key, Key::Previous) || shift {
                    -1
                } else {
                    1
                },
            );
            effect.accepted = true;
        } else if self.converting && self.forced.is_empty() && matches!(key, Key::Left | Key::Right)
        {
            self.invalidate(ctx);
            self.segmented = true;
            if shift {
                self.resize(ctx, if matches!(key, Key::Left) { -1 } else { 1 });
            } else if matches!(key, Key::Left) {
                self.segment = self.segment.saturating_sub(1);
            } else {
                self.segment = (self.segment + 1).min(self.conversion.segments.len() - 1);
            }
            effect.accepted = true;
        } else if let Key::Select(index) = key {
            self.choose(ctx, index);
            effect.accepted = true;
        } else if let Key::Text(added) = key {
            if self.converting
                && self.forced.is_empty()
                && added.len() == 1
                && matches!(added.as_bytes()[0], b'1'..=b'9')
            {
                self.choose(ctx, (added.as_bytes()[0] - b'1') as usize);
                effect.accepted = true;
            } else {
                if self.converting
                    || !self.forced.is_empty()
                    || self.raw.chars().count() + added.chars().count() > ctx.config.max_input_chars
                {
                    self.commit(ctx, &mut effect);
                }
                self.invalidate(ctx);
                self.raw.push_str(&added);
                self.request_mozc(ctx, false);
                effect.accepted = true;
            }
        } else if matches!(
            key,
            Key::OtherCommit | Key::Left | Key::Right | Key::Next | Key::Previous
        ) {
            self.commit(ctx, &mut effect);
        }
        effect
    }
}

pub struct Core {
    config: Config,
    dictionary: Dictionary,
    workers: Workers,
    sessions: HashMap<u64, Session>,
    dirty: bool,
}
impl Core {
    pub fn new(config: Config, wake: Option<Wake>) -> Self {
        let dictionary = config.dictionary();
        let workers = Workers::new(&config, wake);
        Self {
            config,
            dictionary,
            workers,
            sessions: HashMap::new(),
            dirty: false,
        }
    }
    pub fn key(&mut self, id: u64, key: Key, shift: bool, sensitive: bool) -> View {
        if sensitive {
            return self.reset(id, true);
        }
        let mut context = Context {
            id,
            config: &self.config,
            dictionary: &mut self.dictionary,
            workers: &self.workers,
        };
        let session = self.sessions.entry(id).or_default();
        let effect = session.key(key, shift, &mut context);
        self.dirty |= effect.save_requested;
        let mut view = session.view(&self.dictionary);
        view.commit = effect.commit;
        view.accepted = effect.accepted;
        view.save_requested = effect.save_requested;
        view
    }
    pub fn reset(&mut self, id: u64, clear_context: bool) -> View {
        let context = Context {
            id,
            config: &self.config,
            dictionary: &mut self.dictionary,
            workers: &self.workers,
        };
        let state = self.sessions.entry(id).or_default();
        state.clear(&context, clear_context);
        state.view(&self.dictionary)
    }
    pub fn remove(&mut self, id: u64) {
        if let Some(state) = self.sessions.remove(&id) {
            // Closing an older field must not cancel the newest field's job.
            if self.workers.is_current(state.generation) {
                self.workers.invalidate();
            }
        }
    }
    pub fn view(&self, id: u64) -> View {
        self.sessions
            .get(&id)
            .map_or_else(View::default, |s| s.view(&self.dictionary))
    }
    pub fn save(&mut self) -> bool {
        if !self.dirty {
            return true;
        }
        if self
            .dictionary
            .save_learning(&self.config.learning_path)
            .is_ok()
        {
            self.dirty = false;
            true
        } else {
            false
        }
    }
    pub fn poll(&mut self) -> Vec<u64> {
        let mut changed = Vec::new();
        for result in self.workers.drain() {
            let job = result.job;
            let Some(state) = self.sessions.get_mut(&job.context) else {
                continue;
            };
            if state.generation != job.generation
                || !state.forced.is_empty()
                || state.converting != job.explicit
                || state.reading(&self.dictionary, job.explicit) != job.reading
            {
                continue;
            }
            match result.answer {
                Answer::Mozc(result) => {
                    let mut conversion = result
                        .unwrap_or_else(|| converter::convert(&self.dictionary, &job.reading, 9));
                    converter::rank(&self.dictionary, &mut conversion, 9);
                    if job.explicit {
                        if state.segmented || state.neural_applied {
                            continue;
                        }
                        state.conversion = conversion;
                        state.sentence = 0;
                        state.segment = 0;
                    } else {
                        state.live = conversion;
                    }
                }
                Answer::Neural(answer) => {
                    if state.segmented {
                        continue;
                    }
                    state.pending = false;
                    if let Some(answer) = answer {
                        if converter::add_neural(&self.dictionary, &mut state.conversion, &answer)
                            != ReadingCheck::Mismatch
                        {
                            state.neural_applied = true;
                            state.sentence = 0;
                        }
                    }
                }
            }
            changed.push(job.context);
        }
        changed.sort_unstable();
        changed.dedup();
        changed
    }
}
impl Drop for Core {
    fn drop(&mut self) {
        self.save();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backspace_removes_reading_characters_and_preserves_pending_input() {
        let mut dictionary = Dictionary::default();
        dictionary
            .load_english(concat!(env!("CARGO_MANIFEST_DIR"), "/data/english.txt"))
            .unwrap();
        for (raw, expected) in [
            ("nihongo", vec!["にほん", "にほ", "に", ""]),
            ("gakkou", vec!["がっこ", "がっ", "が", ""]),
            ("kyaku", vec!["きゃ", "き", ""]),
            ("nihongok", vec!["にほんご", "にほん"]),
            ("Googlede", vec!["Google", "Googl"]),
            ("googlede", vec!["google", "googl", "goog"]),
            ("github", vec!["githu", "gith", "git"]),
            ("code", vec!["cod", "co", "c", ""]),
            ("CPU12.5%", vec!["CPU12.5", "CPU12.", "CPU12"]),
        ] {
            let mut state = Session {
                raw: raw.into(),
                ..Session::default()
            };
            for expected in expected {
                state.erase_reading(&dictionary);
                assert_eq!(state.reading(&dictionary, false), expected, "{raw}");
            }
        }
        let mut state = Session {
            raw: "kyaku".into(),
            ..Session::default()
        };
        state.erase_reading(&dictionary);
        state.erase_reading(&dictionary);
        state.raw.push_str("ku");
        assert_eq!(compose(&dictionary, &state.raw, false), "きく");
        let mut state = Session {
            raw: "google".into(),
            ..Session::default()
        };
        state.erase_reading(&dictionary);
        state.raw.push('e');
        assert_eq!(state.reading(&dictionary, false), "google");
        state.raw.push_str("de");
        assert_eq!(state.reading(&dictionary, false), "googleで");
    }

    #[test]
    fn literal_segment_can_recover_original_spelling_across_kana() {
        let mut dictionary = Dictionary::default();
        let mut config = Config::from_environment();
        config.model_path.clear();
        config.mozc_helper_path.clear();
        let workers = Workers::new(&config, None);
        let context = Context {
            id: 1,
            config: &config,
            dictionary: &mut dictionary,
            workers: &workers,
        };
        // A backend may have divided the literal M from the Japanese reading.
        // Keep the original key strokes so Shift+Right can repair that division.
        let mut state = Session {
            raw: "MonakIME".into(),
            conversion: converter::convert(context.dictionary, "MおなkIME", 9),
            source_spans: Some(vec![
                SourceSpan {
                    raw: 0..1,
                    reading: 0..1,
                },
                SourceSpan {
                    raw: 1..2,
                    reading: 1..4,
                },
                SourceSpan {
                    raw: 2..4,
                    reading: 4..7,
                },
                SourceSpan {
                    raw: 4..8,
                    reading: 7..11,
                },
            ]),
            ..Session::default()
        };
        state.resize(&context, 1);
        assert_eq!(state.conversion.segments[0].text(), "Mo");
        state.resize(&context, 1);
        assert_eq!(state.conversion.segments[0].text(), "Mona");
        state.resize(&context, 1);
        assert_eq!(state.conversion.segments[0].text(), "Monak");
        state.resize(&context, -1);
        assert_eq!(state.conversion.segments[0].text(), "Mona");
        assert_eq!(
            state
                .conversion
                .segments
                .iter()
                .map(|s| s.reading.as_str())
                .collect::<String>(),
            "MおなkIME"
        );
    }
}
