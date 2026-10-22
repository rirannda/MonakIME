// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! Small dictionary fallback and common candidate operations.
//! Mozc provides the normal live conversion; this lattice also checks Zenz readings.
use crate::{dictionary::Dictionary, text};

#[derive(Clone, Debug)]
pub struct Candidate {
    pub text: String,
    pub score: f64,
}
impl Candidate {
    pub fn new(text: impl Into<String>, score: f64) -> Self {
        Self {
            text: text.into(),
            score,
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Segment {
    pub reading: String,
    pub candidates: Vec<Candidate>,
    pub selected: usize,
    pub literal: bool,
}
impl Segment {
    pub fn text(&self) -> &str {
        self.candidates
            .get(self.selected)
            .map_or(&self.reading, |c| &c.text)
    }
}

#[derive(Clone, Debug, Default)]
pub struct Conversion {
    pub reading: String,
    pub segments: Vec<Segment>,
    pub candidates: Vec<Candidate>,
}
impl Conversion {
    pub fn literal(reading: &str) -> Self {
        let candidates = vec![Candidate::new(reading, 0.0)];
        Self {
            reading: reading.into(),
            segments: vec![Segment {
                reading: reading.into(),
                candidates: candidates.clone(),
                selected: 0,
                literal: true,
            }],
            candidates,
        }
    }
    pub fn text(&self) -> String {
        self.segments.iter().map(Segment::text).collect()
    }
    pub fn rebuild_candidates(&mut self, limit: usize) {
        let mut beam = vec![Candidate::new("", 0.0)];
        for segment in &self.segments {
            let mut next = Vec::with_capacity(beam.len() * segment.candidates.len());
            for prefix in &beam {
                for suffix in &segment.candidates {
                    next.push(Candidate::new(
                        format!("{}{}", prefix.text, suffix.text),
                        prefix.score + suffix.score,
                    ));
                }
            }
            unique(&mut next, limit.clamp(1, 16));
            beam = next;
        }
        self.candidates = beam;
    }
    pub fn weight(&self) -> usize {
        self.reading.len()
            + self
                .segments
                .iter()
                .map(|s| {
                    std::mem::size_of::<Segment>()
                        + s.reading.len()
                        + s.candidates
                            .iter()
                            .map(|c| std::mem::size_of::<Candidate>() + c.text.len())
                            .sum::<usize>()
                })
                .sum::<usize>()
            + self
                .candidates
                .iter()
                .map(|c| std::mem::size_of::<Candidate>() + c.text.len())
                .sum::<usize>()
    }
}

pub fn unique(candidates: &mut Vec<Candidate>, limit: usize) {
    candidates.sort_by(|a, b| b.score.total_cmp(&a.score));
    // The beam keeps at most 16 choices. Compare those few retained strings in
    // place instead of allocating and hashing a copy of every discarded string.
    let mut kept = 0;
    for index in 0..candidates.len() {
        if kept == limit {
            break;
        }
        if candidates[..kept]
            .iter()
            .any(|c| c.text == candidates[index].text)
        {
            continue;
        }
        candidates.swap(kept, index);
        kept += 1;
    }
    candidates.truncate(kept);
}
fn suffix_word(word: &str) -> bool {
    [
        "は",
        "が",
        "を",
        "に",
        "で",
        "と",
        "も",
        "の",
        "へ",
        "から",
        "まで",
        "より",
        "だけ",
        "です",
        "ます",
        "でした",
        "ました",
        "します",
        "した",
        "する",
        "して",
        "される",
        "された",
        "では",
        "には",
        "とは",
        "ので",
        "ても",
        "って",
        "ない",
        "たい",
        "た",
        "て",
    ]
    .contains(&word)
}
pub fn candidates(dictionary: &Dictionary, reading: &str, limit: usize) -> Vec<Candidate> {
    let limit = limit.clamp(1, 16);
    let mut choices = dictionary.lookup(reading, limit);
    choices.push(Candidate::new(reading, -30.0));
    choices.push(Candidate::new(text::katakana(reading), -32.0));
    unique(&mut choices, limit);
    choices
}
pub fn rank(dictionary: &Dictionary, conversion: &mut Conversion, limit: usize) {
    for segment in &mut conversion.segments {
        if segment.literal {
            continue;
        }
        for preferred in dictionary
            .lookup(&segment.reading, limit)
            .into_iter()
            .filter(|c| c.score >= 4.0)
        {
            segment.candidates.retain(|c| c.text != preferred.text);
            segment.candidates.push(preferred);
        }
        unique(&mut segment.candidates, limit);
        segment.selected = 0;
    }
    conversion.rebuild_candidates(limit);
    conversion.candidates.extend(
        dictionary
            .lookup(&conversion.reading, limit)
            .into_iter()
            .filter(|c| c.score >= 4.0),
    );
    unique(&mut conversion.candidates, limit);
}

pub fn convert(dictionary: &Dictionary, source: &str, limit: usize) -> Conversion {
    let limit = limit.clamp(1, 16);
    if source.len() > 1024 || source.chars().count() > 256 {
        return Conversion::literal(source);
    }
    let source = text::hiragana(source);
    let chars: Vec<_> = source.chars().collect();
    if chars.is_empty() {
        return Conversion::default();
    }
    #[derive(Clone, Copy)]
    struct Path {
        cost: f64,
        previous: usize,
        known: bool,
    }
    let mut best = vec![
        Path {
            cost: f64::INFINITY,
            previous: 0,
            known: false
        };
        chars.len() + 1
    ];
    best[0].cost = 0.0;
    for i in 0..chars.len() {
        let previous_cost = best[i].cost;
        if !previous_cost.is_finite() {
            continue;
        }
        let mut offer = |end: usize, cost: f64, known: bool| {
            if previous_cost + cost < best[end].cost {
                best[end] = Path {
                    cost: previous_cost + cost,
                    previous: i,
                    known,
                };
            }
        };
        if !text::is_kana(chars[i]) {
            let mut end = i + 1;
            while end < chars.len() && !text::is_kana(chars[end]) {
                end += 1;
            }
            offer(end, 0.0, false);
            continue;
        }
        offer(i + 1, 5.0, false);
        let mut word = String::new();
        for end in i..chars.len().min(i + 48) {
            if !text::is_kana(chars[end]) {
                break;
            }
            word.push(chars[end]);
            if i > 0 && suffix_word(&word) {
                offer(end + 1, 0.2, true);
            }
            let found = dictionary.lookup(&word, 1);
            let Some(candidate) = found.first() else {
                continue;
            };
            let len = (end - i + 1) as f64;
            let mut cost = 3.0 + (-candidate.score).max(-8.0) * 0.35 - (len * len * 0.1).min(3.0);
            for split in 3..end - i + 1 {
                let stem: String = chars[i..i + split].iter().collect();
                let tail: String = chars[i + split..=end].iter().collect();
                if !suffix_word(&tail) {
                    continue;
                }
                if let Some(stem_match) = dictionary.lookup(&stem, 1).first() {
                    if stem_match.score >= -2.0 && !candidate.text.starts_with(&stem_match.text) {
                        cost += 2.0;
                        break;
                    }
                }
            }
            offer(end + 1, cost, true);
        }
    }
    let mut spans = Vec::new();
    let mut end = chars.len();
    while end > 0 {
        let step = best[end];
        spans.push((step.previous, end, step.known));
        end = step.previous;
    }
    spans.reverse();
    let mut out = Conversion {
        reading: source,
        ..Conversion::default()
    };
    let mut i = 0;
    while i < spans.len() {
        let (begin, mut end, known) = spans[i];
        i += 1;
        if !known && text::is_kana(chars[begin]) {
            while i < spans.len() && !spans[i].2 && text::is_kana(chars[spans[i].0]) {
                end = spans[i].1;
                i += 1;
            }
        }
        let reading: String = chars[begin..end].iter().collect();
        let literal = !text::is_kana(chars[begin]);
        if !literal && suffix_word(&reading) && out.segments.last().is_some_and(|s| !s.literal) {
            let previous = out.segments.last_mut().unwrap();
            previous.reading.push_str(&reading);
            for candidate in &mut previous.candidates {
                candidate.text.push_str(&reading);
            }
            previous
                .candidates
                .extend(dictionary.lookup(&previous.reading, limit));
            unique(&mut previous.candidates, limit);
        } else {
            let choices = if literal {
                vec![Candidate::new(&reading, 0.0)]
            } else {
                candidates(dictionary, &reading, limit)
            };
            out.segments.push(Segment {
                reading,
                candidates: choices,
                selected: 0,
                literal,
            });
        }
    }
    out.rebuild_candidates(limit);
    out.candidates
        .extend(dictionary.lookup(&out.reading, limit));
    unique(&mut out.candidates, limit);
    out
}

pub fn resize(
    dictionary: &Dictionary,
    conversion: &mut Conversion,
    segment: usize,
    delta: i32,
) -> bool {
    let Some(current) = conversion.segments.get(segment) else {
        return false;
    };
    if delta == 0 {
        return false;
    }
    let mut chars: Vec<_> = current.reading.chars().collect();
    let remainder = if delta < 0 {
        if chars.len() <= 1 {
            return false;
        }
        let tail = chars.pop().unwrap().to_string();
        tail + &conversion.segments[segment + 1..]
            .iter()
            .map(|s| s.reading.as_str())
            .collect::<String>()
    } else {
        let Some(next) = conversion.segments.get(segment + 1) else {
            return false;
        };
        let Some(c) = next.reading.chars().next() else {
            return false;
        };
        chars.push(c);
        next.reading[c.len_utf8()..].to_owned()
            + &conversion.segments[segment + 2..]
                .iter()
                .map(|s| s.reading.as_str())
                .collect::<String>()
    };
    let current = &mut conversion.segments[segment];
    current.reading = chars.iter().collect();
    current.candidates = if current.literal {
        vec![Candidate::new(&current.reading, 0.0)]
    } else {
        candidates(dictionary, &current.reading, 9)
    };
    current.selected = 0;
    conversion.segments.truncate(segment + 1);
    conversion
        .segments
        .extend(convert(dictionary, &remainder, 9).segments);
    conversion.rebuild_candidates(9);
    true
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReadingCheck {
    Verified,
    Unverified,
    Mismatch,
}
pub fn check_reading(dictionary: &Dictionary, reading: &str, surface: &str) -> ReadingCheck {
    let input: Vec<_> = text::hiragana(reading).chars().collect();
    let output: Vec<_> = text::hiragana(surface).chars().collect();
    if input.is_empty() || input.len() > 128 || output.is_empty() || output.len() > 256 {
        return ReadingCheck::Mismatch;
    }
    let latin_runs = |value: &[char]| {
        value
            .split(|c| !c.is_ascii_alphanumeric())
            .filter(|s| !s.is_empty())
            .map(|s| s.to_vec())
            .collect::<Vec<_>>()
    };
    if latin_runs(&input) != latin_runs(&output) {
        return ReadingCheck::Mismatch;
    }
    if output.iter().copied().all(text::is_kana) {
        return if input == output {
            ReadingCheck::Verified
        } else {
            ReadingCheck::Mismatch
        };
    }
    let width = output.len() + 1;
    let mut reachable = vec![false; (input.len() + 1) * width];
    reachable[0] = true;
    for i in 0..input.len() {
        if !reachable[i * width..(i + 1) * width].iter().any(|v| *v) {
            continue;
        }
        let mut words = Vec::new();
        let mut part = String::new();
        for (end, &character) in input.iter().enumerate().skip(i).take(48) {
            part.push(character);
            for candidate in dictionary.lookup(&part, 32) {
                words.push((
                    end + 1,
                    text::hiragana(&candidate.text).chars().collect::<Vec<_>>(),
                ));
            }
        }
        for j in 0..output.len() {
            if !reachable[i * width + j] {
                continue;
            }
            if input[i] == output[j] || (input[i] == ' ' && output[j] == '　') {
                reachable[(i + 1) * width + j + 1] = true;
            }
            for (end, value) in &words {
                if output[j..].starts_with(value) {
                    reachable[end * width + j + value.len()] = true;
                }
            }
        }
    }
    if reachable[input.len() * width + output.len()] {
        ReadingCheck::Verified
    } else {
        ReadingCheck::Unverified
    }
}

pub fn add_neural(
    dictionary: &Dictionary,
    conversion: &mut Conversion,
    answer: &str,
) -> ReadingCheck {
    let check = check_reading(dictionary, &conversion.reading, answer);
    if check == ReadingCheck::Mismatch {
        return check;
    }
    let preferred = dictionary
        .lookup(&conversion.reading, 1)
        .first()
        .is_some_and(|c| c.score >= 4.0)
        || conversion.segments.iter().any(|s| {
            dictionary
                .lookup(&s.reading, 1)
                .first()
                .is_some_and(|c| c.score >= 4.0)
        });
    let already = preferred
        && conversion
            .candidates
            .first()
            .is_some_and(|c| c.text == answer);
    if !already {
        conversion.candidates.retain(|c| c.text != answer);
        let position = usize::from(preferred && !conversion.candidates.is_empty());
        conversion
            .candidates
            .insert(position, Candidate::new(answer, -100.0));
    }
    conversion.candidates.truncate(9);
    check
}
