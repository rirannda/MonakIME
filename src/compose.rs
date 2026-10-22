// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! Decide which parts of a romaji token are English and which are Japanese.
//! Each position has two possible paths: the previous span was Japanese or English.
use crate::{dictionary::Dictionary, romaji};
use std::ops::Range;

/// Byte ranges connecting a converted syllable to the original editable input.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SourceSpan {
    pub raw: Range<usize>,
    pub reading: Range<usize>,
}

fn acronym(word: &str) -> bool {
    word.len() >= 2 && word.bytes().all(|c| c.is_ascii_uppercase())
}
fn has_latin(word: &str) -> bool {
    word.bytes().any(|c| c.is_ascii_alphabetic())
}

#[derive(Clone)]
struct Step {
    score: f64,
    previous: usize,
    previous_english: usize,
    value: String,
    source_nn: Option<bool>,
}
impl Default for Step {
    fn default() -> Self {
        Self {
            score: f64::NEG_INFINITY,
            previous: 0,
            previous_english: 0,
            value: String::new(),
            source_nn: None,
        }
    }
}

pub fn compose(dictionary: &Dictionary, raw: &str, final_input: bool) -> String {
    compose_internal(dictionary, raw, final_input, None)
}

pub fn compose_with_spans(
    dictionary: &Dictionary,
    raw: &str,
    final_input: bool,
) -> (String, Vec<SourceSpan>) {
    let mut spans = Vec::new();
    let reading = compose_internal(dictionary, raw, final_input, Some(&mut spans));
    (reading, spans)
}

fn append_span(
    spans: &mut Option<&mut Vec<SourceSpan>>,
    raw: Range<usize>,
    reading: Range<usize>,
    literal: bool,
) {
    if let Some(spans) = spans {
        // Keeping English letters separate lets the segment editor extend M to
        // Mo instead of treating an entire acronym as an indivisible unit.
        if literal {
            spans.extend((0..raw.len()).map(|i| SourceSpan {
                raw: raw.start + i..raw.start + i + 1,
                reading: reading.start + i..reading.start + i + 1,
            }));
        } else {
            spans.push(SourceSpan { raw, reading });
        }
    }
}

fn compose_internal(
    dictionary: &Dictionary,
    raw: &str,
    final_input: bool,
    mut spans: Option<&mut Vec<SourceSpan>>,
) -> String {
    let domain = [
        ".com", ".org", ".net", ".dev", ".io", ".jp", ".edu", ".gov", ".local",
    ]
    .iter()
    .any(|s| raw.len() > s.len() && raw.ends_with(s));
    if raw.len() > 1024
        || (raw.is_ascii()
            && (domain || raw.contains("://") || raw.contains(['@', '_', '/', '\\'])))
    {
        if let Some(spans) = &mut spans {
            spans.extend(raw.char_indices().map(|(i, c)| SourceSpan {
                raw: i..i + c.len_utf8(),
                reading: i..i + c.len_utf8(),
            }));
        }
        return raw.into();
    }
    let mut output = String::with_capacity(raw.len() * 2);
    let mut start = 0;
    while start < raw.len() {
        let c = raw[start..].chars().next().unwrap();
        if !c.is_ascii_alphabetic() {
            let output_start = output.len();
            let literal_before = output
                .as_bytes()
                .last()
                .is_some_and(u8::is_ascii_alphanumeric);
            if literal_before && matches!(c, '.' | '-') {
                output.push(c);
            } else {
                output.push_str(&romaji::convert(&c.to_string(), final_input));
            }
            append_span(
                &mut spans,
                start..start + c.len_utf8(),
                output_start..output.len(),
                c.is_ascii() && output[output_start..] == raw[start..start + c.len_utf8()],
            );
            start += c.len_utf8();
            continue;
        }
        let mut end = start;
        while end < raw.len()
            && (raw.as_bytes()[end].is_ascii_alphabetic() || raw.as_bytes()[end] == b'\'')
        {
            end += 1;
        }
        let token = &raw[start..end];
        let token_final = final_input || end < raw.len();
        let known_prefix = (2..=token.len().min(48))
            .any(|len| acronym(&token[..len]) || (len >= 3 && dictionary.english(&token[..len])));
        if acronym(token)
            || (token.len() >= 3 && token.as_bytes()[0].is_ascii_uppercase() && !known_prefix)
        {
            let output_start = output.len();
            output.push_str(token);
            append_span(&mut spans, start..end, output_start..output.len(), true);
            start = end;
            continue;
        }
        let token_reading = compose_token(
            dictionary,
            token,
            token_final,
            start,
            output.len(),
            &mut spans,
        );
        output.push_str(&token_reading);
        start = end;
    }
    output
}

fn compose_token(
    dictionary: &Dictionary,
    token: &str,
    final_input: bool,
    raw_offset: usize,
    reading_offset: usize,
    spans: &mut Option<&mut Vec<SourceSpan>>,
) -> String {
    // A complete Japanese dictionary word must not be split merely because an
    // interior suffix is also English: katime is かちめ, not か + time.
    let japanese = romaji::convert(token, final_input);
    if !has_latin(&japanese) && dictionary.contains(&japanese) {
        if spans.is_some() {
            return romaji::convert_with(token, final_input, false, |raw, reading| {
                append_span(
                    spans,
                    raw.start + raw_offset..raw.end + raw_offset,
                    reading.start + reading_offset..reading.end + reading_offset,
                    false,
                );
            });
        }
        return japanese;
    }
    let n = token.len();
    let bytes = token.as_bytes();
    let has_ambiguous_nn = bytes
        .windows(3)
        .any(|w| w[0..2] == *b"nn" && b"aiueo".contains(&w[2]));
    let mut best = vec![[Step::default(), Step::default()]; n + 1];
    best[0][0].score = 0.0;
    for i in 0..n {
        // Only ambiguous nn tokens need additional dictionary probes. This also
        // preserves familiar words inside phrases: annaishimasu -> あんないします.
        let mut nn_words = Vec::new();
        if has_ambiguous_nn
            && bytes[i..n.min(i + 48)]
                .windows(3)
                .enumerate()
                .any(|(offset, window)| {
                    offset > 0 && window[0..2] == *b"nn" && b"aiueo".contains(&window[2])
                })
        {
            for len in 3..=48.min(n - i) {
                let raw = &token[i..i + len];
                if !raw
                    .as_bytes()
                    .windows(3)
                    .enumerate()
                    .any(|(offset, window)| {
                        offset > 0 && window[0..2] == *b"nn" && b"aiueo".contains(&window[2])
                    })
                {
                    continue;
                }
                let value = romaji::convert(raw, final_input && i + len == n);
                if !has_latin(&value) && dictionary.contains(&value) {
                    nn_words.push((i + len, value, false));
                } else {
                    let legacy =
                        romaji::convert_with(raw, final_input && i + len == n, true, |_, _| {});
                    if legacy != value && !has_latin(&legacy) && dictionary.contains(&legacy) {
                        nn_words.push((i + len, legacy, true));
                    }
                }
            }
        }
        // Keep the longest dictionary word; when both readings have equal
        // evidence, nn remains a completed ん (単位 wins over 単に). The longer
        // word こんにちは still wins over the shorter 懇意/今イチ prefixes.
        if let Some(longest) = nn_words
            .iter()
            .map(|(_, value, _)| value.chars().count())
            .max()
        {
            let prefer_new = nn_words
                .iter()
                .any(|(_, value, legacy)| !legacy && value.chars().count() == longest);
            nn_words.retain(|(_, value, legacy)| {
                value.chars().count() == longest && (!prefer_new || !legacy)
            });
        }
        for previous_english in 0..2 {
            let score = best[i][previous_english].score;
            if !score.is_finite() {
                continue;
            }
            let mut offer = |end: usize, english: usize, gain: f64, value: String, source_nn| {
                if score + gain > best[end][english].score {
                    best[end][english] = Step {
                        score: score + gain,
                        previous: i,
                        previous_english,
                        value,
                        source_nn,
                    };
                }
            };
            let (mut consume, mut kana) = (1, String::new());
            let double_n = bytes[i] == b'n' && bytes.get(i + 1) == Some(&b'n');
            let n_before_consonant =
                bytes[i] == b'n' && i + 1 < n && !b"naiueoy'".contains(&bytes[i + 1]);
            let doubled_consonant = i + 1 < n
                && bytes[i] == bytes[i + 1]
                && bytes[i].is_ascii_lowercase()
                && !b"aiueonxl".contains(&bytes[i]);
            if double_n {
                consume = if bytes.get(i + 2) == Some(&b'y') {
                    1
                } else {
                    2
                };
                kana.push('ん');
            } else if n_before_consonant {
                kana.push('ん');
            } else if doubled_consonant || token[i..].starts_with("tch") {
                kana.push('っ');
            }
            if kana.is_empty() {
                for len in 1..=4.min(n - i) {
                    let value = romaji::convert(&token[i..i + len], final_input && i + len == n);
                    if !has_latin(&value) {
                        consume = len;
                        kana = value;
                        break;
                    }
                }
            }
            if kana.is_empty() {
                kana = romaji::convert(&token[i..i + 1], final_input && i + 1 == n);
            }
            offer(
                i + consume,
                0,
                if has_latin(&kana) { -0.7 } else { 0.0 },
                kana,
                None,
            );
            for (end, value, legacy) in &nn_words {
                offer(
                    *end,
                    0,
                    value.chars().count() as f64 * 0.0001,
                    value.clone(),
                    Some(*legacy),
                );
            }
            for len in 2..=48.min(n - i) {
                let word = &token[i..i + len];
                if !acronym(word) && (len < 3 || !dictionary.english(word)) {
                    continue;
                }
                let marked = word.bytes().any(|c| c.is_ascii_uppercase());
                if !marked
                    && i + len < n
                    && b"aiueoy".contains(&bytes[i + len])
                    && b"bcdfghjklmpqrstvwxyz".contains(word.as_bytes().last().unwrap())
                {
                    continue;
                }
                let japanese = romaji::convert(word, true);
                if !marked && len == 3 && i > 0 && previous_english == 0 && !has_latin(&japanese) {
                    continue;
                }
                if !marked && !has_latin(&japanese) && dictionary.contains(&japanese) {
                    continue;
                }
                // Protect a Japanese dictionary word crossing the proposed
                // English boundary, including words in the middle of a phrase.
                // Probes are limited to the preceding 48 romaji bytes.
                if !marked
                    && i > 0
                    && previous_english == 0
                    && !has_latin(&japanese)
                    && (i.saturating_sub(48)..i).rev().any(|start| {
                        let reading = romaji::convert(&token[start..i + len], false);
                        !has_latin(&reading) && dictionary.contains(&reading)
                    })
                {
                    continue;
                }
                let gain =
                    0.12 * (len * len) as f64 - if previous_english == 1 { 2.5 } else { 0.5 };
                offer(i + len, 1, gain, word.into(), None);
            }
        }
    }
    let mut state = usize::from(best[n][1].score > best[n][0].score);
    let mut i = n;
    let mut steps = Vec::new();
    while i > 0 {
        let step = &best[i][state];
        steps.push((step.previous..i, step.value.as_str(), step.source_nn));
        i = step.previous;
        state = step.previous_english;
    }
    let mut output = String::with_capacity(token.len() * 2);
    for (raw, value, source_nn) in steps.into_iter().rev() {
        let start = output.len();
        output.push_str(value);
        if let Some(legacy) = source_nn.filter(|_| spans.is_some()) {
            romaji::convert_with(
                &token[raw.clone()],
                final_input && raw.end == n,
                legacy,
                |source, reading| {
                    append_span(
                        spans,
                        raw.start + raw_offset + source.start..raw.start + raw_offset + source.end,
                        start + reading_offset + reading.start
                            ..start + reading_offset + reading.end,
                        false,
                    );
                },
            );
        } else {
            append_span(
                spans,
                raw.start + raw_offset..raw.end + raw_offset,
                start + reading_offset..output.len() + reading_offset,
                value == &token[raw.clone()],
            );
        }
    }
    output
}
