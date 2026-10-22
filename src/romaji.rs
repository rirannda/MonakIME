// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! Romaji rules and incremental conversion. An unfinished syllable stays editable.
const RULES: &[(&str, &str)] = &[
    ("a", "あ"),
    ("i", "い"),
    ("u", "う"),
    ("e", "え"),
    ("o", "お"),
    ("ka", "か"),
    ("ki", "き"),
    ("ku", "く"),
    ("ke", "け"),
    ("ko", "こ"),
    ("ga", "が"),
    ("gi", "ぎ"),
    ("gu", "ぐ"),
    ("ge", "げ"),
    ("go", "ご"),
    ("sa", "さ"),
    ("si", "し"),
    ("su", "す"),
    ("se", "せ"),
    ("so", "そ"),
    ("shi", "し"),
    ("za", "ざ"),
    ("zi", "じ"),
    ("zu", "ず"),
    ("ze", "ぜ"),
    ("zo", "ぞ"),
    ("ji", "じ"),
    ("ta", "た"),
    ("ti", "ち"),
    ("tu", "つ"),
    ("te", "て"),
    ("to", "と"),
    ("chi", "ち"),
    ("tsu", "つ"),
    ("da", "だ"),
    ("di", "ぢ"),
    ("du", "づ"),
    ("de", "で"),
    ("do", "ど"),
    ("na", "な"),
    ("ni", "に"),
    ("nu", "ぬ"),
    ("ne", "ね"),
    ("no", "の"),
    ("ha", "は"),
    ("hi", "ひ"),
    ("hu", "ふ"),
    ("he", "へ"),
    ("ho", "ほ"),
    ("fu", "ふ"),
    ("ba", "ば"),
    ("bi", "び"),
    ("bu", "ぶ"),
    ("be", "べ"),
    ("bo", "ぼ"),
    ("pa", "ぱ"),
    ("pi", "ぴ"),
    ("pu", "ぷ"),
    ("pe", "ぺ"),
    ("po", "ぽ"),
    ("ma", "ま"),
    ("mi", "み"),
    ("mu", "む"),
    ("me", "め"),
    ("mo", "も"),
    ("ya", "や"),
    ("yu", "ゆ"),
    ("yo", "よ"),
    ("yi", "い"),
    ("ye", "いぇ"),
    ("ra", "ら"),
    ("ri", "り"),
    ("ru", "る"),
    ("re", "れ"),
    ("ro", "ろ"),
    ("wa", "わ"),
    ("wi", "うぃ"),
    ("we", "うぇ"),
    ("wo", "を"),
    ("wu", "う"),
    ("kya", "きゃ"),
    ("kyu", "きゅ"),
    ("kyo", "きょ"),
    ("kyi", "きぃ"),
    ("kye", "きぇ"),
    ("gya", "ぎゃ"),
    ("gyu", "ぎゅ"),
    ("gyo", "ぎょ"),
    ("gye", "ぎぇ"),
    ("sha", "しゃ"),
    ("shu", "しゅ"),
    ("sho", "しょ"),
    ("she", "しぇ"),
    ("sya", "しゃ"),
    ("syu", "しゅ"),
    ("syo", "しょ"),
    ("sye", "しぇ"),
    ("ja", "じゃ"),
    ("ju", "じゅ"),
    ("jo", "じょ"),
    ("je", "じぇ"),
    ("jya", "じゃ"),
    ("jyu", "じゅ"),
    ("jyo", "じょ"),
    ("jye", "じぇ"),
    ("zya", "じゃ"),
    ("zyu", "じゅ"),
    ("zyo", "じょ"),
    ("zye", "じぇ"),
    ("cha", "ちゃ"),
    ("chu", "ちゅ"),
    ("cho", "ちょ"),
    ("che", "ちぇ"),
    ("tya", "ちゃ"),
    ("tyu", "ちゅ"),
    ("tyo", "ちょ"),
    ("tye", "ちぇ"),
    ("cya", "ちゃ"),
    ("cyu", "ちゅ"),
    ("cyo", "ちょ"),
    ("cye", "ちぇ"),
    ("dya", "ぢゃ"),
    ("dyu", "ぢゅ"),
    ("dyo", "ぢょ"),
    ("nya", "にゃ"),
    ("nyu", "にゅ"),
    ("nyo", "にょ"),
    ("nye", "にぇ"),
    ("hya", "ひゃ"),
    ("hyu", "ひゅ"),
    ("hyo", "ひょ"),
    ("hye", "ひぇ"),
    ("bya", "びゃ"),
    ("byu", "びゅ"),
    ("byo", "びょ"),
    ("bye", "びぇ"),
    ("pya", "ぴゃ"),
    ("pyu", "ぴゅ"),
    ("pyo", "ぴょ"),
    ("pye", "ぴぇ"),
    ("mya", "みゃ"),
    ("myu", "みゅ"),
    ("myo", "みょ"),
    ("mye", "みぇ"),
    ("rya", "りゃ"),
    ("ryu", "りゅ"),
    ("ryo", "りょ"),
    ("rye", "りぇ"),
    ("fa", "ふぁ"),
    ("fi", "ふぃ"),
    ("fe", "ふぇ"),
    ("fo", "ふぉ"),
    ("fyu", "ふゅ"),
    ("va", "ゔぁ"),
    ("vi", "ゔぃ"),
    ("vu", "ゔ"),
    ("ve", "ゔぇ"),
    ("vo", "ゔぉ"),
    ("vyu", "ゔゅ"),
    ("tsa", "つぁ"),
    ("tsi", "つぃ"),
    ("tse", "つぇ"),
    ("tso", "つぉ"),
    ("thi", "てぃ"),
    ("thu", "てゅ"),
    ("dhi", "でぃ"),
    ("dhu", "でゅ"),
    ("twa", "とぁ"),
    ("twi", "とぃ"),
    ("twu", "とぅ"),
    ("twe", "とぇ"),
    ("two", "とぉ"),
    ("dwa", "どぁ"),
    ("dwi", "どぃ"),
    ("dwu", "どぅ"),
    ("dwe", "どぇ"),
    ("dwo", "どぉ"),
    ("qa", "くぁ"),
    ("qi", "くぃ"),
    ("qu", "く"),
    ("qe", "くぇ"),
    ("qo", "くぉ"),
    ("kwa", "くぁ"),
    ("kwi", "くぃ"),
    ("kwe", "くぇ"),
    ("kwo", "くぉ"),
    ("xa", "ぁ"),
    ("xi", "ぃ"),
    ("xu", "ぅ"),
    ("xe", "ぇ"),
    ("xo", "ぉ"),
    ("la", "ぁ"),
    ("li", "ぃ"),
    ("lu", "ぅ"),
    ("le", "ぇ"),
    ("lo", "ぉ"),
    ("xya", "ゃ"),
    ("xyu", "ゅ"),
    ("xyo", "ょ"),
    ("lya", "ゃ"),
    ("lyu", "ゅ"),
    ("lyo", "ょ"),
    ("xtu", "っ"),
    ("xtsu", "っ"),
    ("ltu", "っ"),
    ("ltsu", "っ"),
    ("xwa", "ゎ"),
    ("lwa", "ゎ"),
    ("xka", "ゕ"),
    ("xke", "ゖ"),
    ("lka", "ゕ"),
    ("lke", "ゖ"),
];

pub fn convert(raw: &str, final_input: bool) -> String {
    convert_with(raw, final_input, false, |_, _| {})
}

/// The callback is optional at compile time: the ordinary converter allocates
/// only its output, while editing can retain the romaji source of each syllable.
pub(crate) fn convert_with(
    raw: &str,
    final_input: bool,
    legacy_nn: bool,
    mut on_span: impl FnMut(std::ops::Range<usize>, std::ops::Range<usize>),
) -> String {
    let mut output = String::with_capacity(raw.len() * 2);
    let mut i = 0;
    while i < raw.len() {
        let (consume, kana) = syllable(raw, i, final_input, legacy_nn);
        let start = output.len();
        output.push_str(kana);
        on_span(i..i + consume, start..output.len());
        i += consume;
    }
    output
}

fn syllable(raw: &str, i: usize, final_input: bool, legacy_nn: bool) -> (usize, &str) {
    let bytes = raw.as_bytes();
    let c = bytes[i];
    let rest = &raw[i..];
    if !c.is_ascii() {
        let len = rest.chars().next().unwrap().len_utf8();
        return (len, &rest[..len]);
    }
    if c == b'n' {
        match bytes.get(i + 1).copied() {
            None => return (1, if final_input { "ん" } else { "n" }),
            Some(b'\'') => return (2, "ん"),
            Some(b'n') => {
                // nn explicitly completes ん. Reusing the second n before a
                // vowel changes tanni (単位) into たんに. Keep the familiar
                // nny consonant cluster for readings such as しんにゅう.
                let reuse = bytes
                    .get(i + 2)
                    .is_some_and(|c| *c == b'y' || (legacy_nn && b"aiueo".contains(c)));
                return (if reuse { 1 } else { 2 }, "ん");
            }
            Some(next) if !b"aiueoy".contains(&next) => return (1, "ん"),
            _ => {}
        }
    }
    if bytes.get(i + 1) == Some(&c) && c.is_ascii_lowercase() && !b"aiueonxl".contains(&c) {
        return (1, "っ");
    }
    if rest.starts_with("tch") {
        return (1, "っ");
    }
    if let Some((latin, kana)) = RULES
        .iter()
        .filter(|(latin, _)| rest.starts_with(latin))
        .max_by_key(|(latin, _)| latin.len())
    {
        return (latin.len(), kana);
    }
    if !final_input && RULES.iter().any(|(latin, _)| latin.starts_with(rest)) {
        return (rest.len(), rest);
    }
    (
        1,
        match c {
            b',' => "、",
            b'.' => "。",
            b'-' => "ー",
            b'[' => "「",
            b']' => "」",
            _ => &rest[..1],
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn incremental_romaji() {
        for (raw, expected) in [
            ("tanni", "たんい"),
            ("tannni", "たんに"),
            ("kon'nichiha", "こんにちは"),
            ("gakkou", "がっこう"),
            ("shinnyuu", "しんにゅう"),
            ("tcha", "っちゃ"),
            ("kan'i", "かんい"),
        ] {
            assert_eq!(convert(raw, true), expected);
        }
        assert_eq!(convert("nihon", false), "にほn");
        assert_eq!(convert("nihon", true), "にほん");
        assert_eq!(convert("日本語k", false), "日本語k");
    }
}
