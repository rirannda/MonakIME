// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! UTF-8 strings stay valid throughout the core. Character indices and byte offsets
//! are kept separate; fcitx's cursor offsets are bytes.
pub fn hiragana(input: &str) -> String {
    input
        .chars()
        .map(|c| match c {
            'ァ'..='ヶ' => char::from_u32(c as u32 - 0x60).unwrap(),
            _ => c,
        })
        .collect()
}

pub fn katakana(input: &str) -> String {
    input
        .chars()
        .map(|c| match c {
            'ぁ'..='ゖ' => char::from_u32(c as u32 + 0x60).unwrap(),
            _ => c,
        })
        .collect()
}

pub fn is_kana(c: char) -> bool {
    matches!(c, 'ぁ'..='ゖ' | 'ァ'..='ヺ' | 'ー')
}

pub fn prefix(input: &str, count: usize) -> &str {
    &input[..input
        .char_indices()
        .nth(count)
        .map_or(input.len(), |(i, _)| i)]
}

pub fn suffix(input: &str, count: usize) -> &str {
    let count = input.chars().count().saturating_sub(count);
    &input[input
        .char_indices()
        .nth(count)
        .map_or(input.len(), |(i, _)| i)..]
}

pub fn valid_field(input: &str) -> bool {
    !input.is_empty()
        && input.len() <= 1024
        && !input
            .bytes()
            .any(|c| matches!(c, b'\t' | b'\r' | b'\n' | 0))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unicode_boundaries() {
        assert_eq!(prefix("日本語🙂", 2), "日本");
        assert_eq!(suffix("日本語🙂", 2), "語🙂");
        assert_eq!(hiragana("カタカナ"), "かたかな");
        assert_eq!(katakana("ひらがな"), "ヒラガナ");
    }
}
