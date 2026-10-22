// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
use monakime::{
    compose::{compose, compose_with_spans},
    dictionary::Dictionary,
    romaji,
};

fn dictionary() -> Dictionary {
    let mut dictionary = Dictionary::default();
    dictionary
        .load(concat!(env!("CARGO_MANIFEST_DIR"), "/data/base.tsv"), false)
        .unwrap();
    dictionary
        .load(
            concat!(env!("CARGO_MANIFEST_DIR"), "/tests/support/compose.tsv"),
            false,
        )
        .unwrap();
    dictionary
        .load_english(concat!(env!("CARGO_MANIFEST_DIR"), "/data/english.txt"))
        .unwrap();
    dictionary
}

#[test]
fn double_n_completes_n_before_vowels() {
    let dictionary = dictionary();
    for (raw, expected) in [
        ("tanni", "たんい"),
        ("tanniha", "たんいは"),
        ("tannni", "たんに"),
        ("tan'i", "たんい"),
        ("konnichiha", "こんにちは"),
        ("konnichihasekai", "こんにちはせかい"),
        ("annaishimasu", "あんないします"),
        ("onnanohito", "おんなのひと"),
        ("kon'nichiha", "こんにちは"),
        ("shinnyuu", "しんにゅう"),
        ("shin'ai", "しんあい"),
    ] {
        assert_eq!(compose(&dictionary, raw, true), expected, "{raw}");
        assert_eq!(
            compose(&dictionary, raw, false),
            expected,
            "incremental {raw}"
        );
    }
    for (raw, expected) in [
        ("nna", "んあ"),
        ("nni", "んい"),
        ("nnu", "んう"),
        ("nne", "んえ"),
        ("nno", "んお"),
    ] {
        assert_eq!(romaji::convert(raw, true), expected, "{raw}");
    }
}

#[test]
fn dictionary_words_win_over_internal_english_suffixes() {
    let dictionary = dictionary();
    for (raw, expected) in [
        ("katime", "かちめ"),
        ("katimehatakai", "かちめはたかい"),
        ("watashihakatimegaaru", "わたしはかちめがある"),
        ("Googledekatime", "Googleでかちめ"),
        ("Googledeshirabete", "Googleでしらべて"),
        ("GitHubnoko-do", "GitHubのこーど"),
        ("githubdecodewokaku", "githubでcodeをかく"),
        ("usedesuka", "useですか"),
    ] {
        assert_eq!(compose(&dictionary, raw, true), expected, "{raw}");
    }
}

#[test]
fn source_spans_cover_source_and_reading_without_splitting_utf8() {
    let dictionary = dictionary();
    for raw in [
        "MonaKIME",
        "Googleでkya",
        "GitHubnoko-do",
        "nihon",
        "tanni",
        "konnichiha",
        "annaishimasu",
        "katimehatakai",
        "shi",
        "日本語🙂k",
        "foo_bar",
        "https://example.com/a",
    ] {
        for final_input in [false, true] {
            let (reading, spans) = compose_with_spans(&dictionary, raw, final_input);
            assert_eq!(reading, compose(&dictionary, raw, final_input));
            let mut raw_end = 0;
            let mut reading_end = 0;
            for span in &spans {
                assert_eq!(span.raw.start, raw_end, "{raw}: {spans:?}");
                assert_eq!(span.reading.start, reading_end, "{raw}: {spans:?}");
                assert!(raw.is_char_boundary(span.raw.start) && raw.is_char_boundary(span.raw.end));
                assert!(
                    reading.is_char_boundary(span.reading.start)
                        && reading.is_char_boundary(span.reading.end)
                );
                raw_end = span.raw.end;
                reading_end = span.reading.end;
            }
            assert_eq!(raw_end, raw.len(), "{raw}");
            assert_eq!(reading_end, reading.len(), "{raw}");
        }
    }
    let (_, spans) = compose_with_spans(&dictionary, "IME", true);
    assert_eq!(spans.len(), 3);
    let (_, spans) = compose_with_spans(&dictionary, "shi", true);
    assert_eq!(spans.len(), 1);
    assert_eq!(spans[0].raw, 0..3);
    assert_eq!(spans[0].reading, 0..3);
}

#[test]
fn shipped_system_dictionary_does_not_steal_nn_or_japanese_words() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/data/system.nimedic");
    if !std::path::Path::new(path).is_file() {
        return; // The mmap dictionary is an optional generated build artifact.
    }
    let mut dictionary = dictionary();
    dictionary.load(path, false).unwrap();
    for (raw, expected) in [
        ("tanni", "たんい"),
        ("tanniha", "たんいは"),
        ("tanniwotsukau", "たんいをつかう"),
        ("konnichiha", "こんにちは"),
        ("konnichihasekai", "こんにちはせかい"),
        ("annaishimasu", "あんないします"),
        ("onnanohito", "おんなのひと"),
        ("shinnyuu", "しんにゅう"),
        ("katimehatakai", "かちめはたかい"),
        ("watashihakatimegaaru", "わたしはかちめがある"),
        ("Googledekatime", "Googleでかちめ"),
    ] {
        for final_input in [false, true] {
            assert_eq!(compose(&dictionary, raw, final_input), expected, "{raw}");
            let (spanned, spans) = compose_with_spans(&dictionary, raw, final_input);
            assert_eq!(spanned, expected, "spanned {raw}");
            for span in spans {
                assert!(raw.is_char_boundary(span.raw.start) && raw.is_char_boundary(span.raw.end));
                assert!(
                    spanned.is_char_boundary(span.reading.start)
                        && spanned.is_char_boundary(span.reading.end)
                );
            }
        }
    }
}
