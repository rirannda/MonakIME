// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
use monakime::{
    compose::compose,
    config::Config,
    converter::{self, ReadingCheck},
    dictionary::Dictionary,
    session::{Core, Key},
};
use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        static SERIAL: AtomicU64 = AtomicU64::new(0);
        // Atomic creation also works when parallel test runners have the same
        // PID inside separate namespaces but share /tmp.
        let path = loop {
            let path = std::env::temp_dir().join(format!(
                "monakime-rust-{}-{}",
                std::process::id(),
                SERIAL.fetch_add(1, Ordering::Relaxed)
            ));
            match fs::create_dir(&path) {
                Ok(()) => break path,
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("create test directory: {error}"),
            }
        };
        Self(path)
    }
    fn path(&self, file: &str) -> PathBuf {
        self.0.join(file)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn dictionary() -> Dictionary {
    let mut out = Dictionary::default();
    out.load(concat!(env!("CARGO_MANIFEST_DIR"), "/data/base.tsv"), false)
        .unwrap();
    out.load_english(concat!(env!("CARGO_MANIFEST_DIR"), "/data/english.txt"))
        .unwrap();
    out
}
fn config(temp: &Temp) -> Config {
    let mut config = Config::from_environment();
    config.dictionary_path = concat!(env!("CARGO_MANIFEST_DIR"), "/data/base.tsv").into();
    config.system_dictionary_path.clear();
    config.user_dictionary_path.clear();
    config.english_dictionary_path =
        concat!(env!("CARGO_MANIFEST_DIR"), "/data/english.txt").into();
    config.enable_learning = false;
    config.model_path.clear();
    config.mozc_helper_path.clear();
    config.learning_path = temp.path("learning.tsv").to_string_lossy().into();
    config.mozc_profile_path = temp.path("mozc").to_string_lossy().into();
    config
}

#[test]
fn english_japanese_regressions() {
    let dictionary = dictionary();
    let cases = [
        ("hashiwowataru", "はしをわたる"),
        ("gohanwotaberu", "ごはんをたべる"),
        ("kyouhagoogle", "きょうはgoogle"),
        ("githubdekanji", "githubでかんじ"),
        ("sushi", "すし"),
        ("https://google.com/a", "https://google.com/a"),
        ("foo_bar", "foo_bar"),
        ("IME", "IME"),
        ("Googledeshirabete", "Googleでしらべて"),
        (
            "GitHubnoko-dowosyuuseishimashita.",
            "GitHubのこーどをしゅうせいしました。",
        ),
        ("CPUsiyourituha12.5%", "CPUしようりつは12.5%"),
        ("256MBdesita.", "256MBでした。"),
        ("AIwotsukaimasu", "AIをつかいます"),
        ("syuuseishimashita", "しゅうせいしました"),
        ("usedesuka", "useですか"),
        ("githubdecodewokaku", "githubでcodeをかく"),
        (
            "Googledeshirabete,GitHubnoko-dowosyuuseishimashita.",
            "Googleでしらべて、GitHubのこーどをしゅうせいしました。",
        ),
        (
            "CPUsiyourituha12.5%,memorishiyouryouha256MBdesita.",
            "CPUしようりつは12.5%、めもりしようりょうは256MBでした。",
        ),
        ("ko-hi-", "こーひー"),
        ("google.com", "google.com"),
        ("10.5", "10.5"),
        ("2026-10-22", "2026-10-22"),
    ];
    for (raw, expected) in cases {
        assert_eq!(compose(&dictionary, raw, true), expected, "{raw}");
    }
    assert_eq!(compose(&dictionary, "nihon.", false), "にほん。");
    assert_eq!(compose(&dictionary, "日本語🙂k", false), "日本語🙂k");
}
#[test]
fn words_boundaries_and_reading_check() {
    let dictionary = dictionary();
    assert_eq!(
        converter::convert(&dictionary, "にほんごにゅうりょく", 9).candidates[0].text,
        "日本語入力"
    );
    let unknown = converter::convert(&dictionary, "ぬほぴゃふょ", 9);
    assert_eq!(unknown.segments.len(), 1);
    assert_eq!(unknown.text(), "ぬほぴゃふょ");
    assert_eq!(
        converter::convert(&dictionary, "GitHubでかいはつ", 9).text(),
        "GitHubで開発"
    );
    let mut conversion = converter::convert(&dictionary, "わたしはにほんご", 9);
    assert!(converter::resize(&dictionary, &mut conversion, 0, -1));
    assert_eq!(
        conversion
            .segments
            .iter()
            .map(|s| s.reading.as_str())
            .collect::<String>(),
        conversion.reading
    );
    assert!(converter::resize(&dictionary, &mut conversion, 0, 1));
    assert_eq!(
        conversion
            .segments
            .iter()
            .map(|s| s.reading.as_str())
            .collect::<String>(),
        conversion.reading
    );
    assert_eq!(
        converter::check_reading(&dictionary, "にほんご", "日本語"),
        ReadingCheck::Verified
    );
    assert_eq!(
        converter::check_reading(&dictionary, "githubでかく", "GitHubで書く"),
        ReadingCheck::Mismatch
    );
    assert_eq!(
        converter::check_reading(&dictionary, "かな", "かに"),
        ReadingCheck::Mismatch
    );
    let mut result = converter::convert(&dictionary, "かな", 9);
    let original = result.candidates[0].text.clone();
    assert_eq!(
        converter::add_neural(&dictionary, &mut result, "かに"),
        ReadingCheck::Mismatch
    );
    assert_eq!(result.candidates[0].text, original);
}
#[test]
fn bounded_atomic_learning_and_user_dictionary() {
    use std::os::unix::fs::PermissionsExt;
    let temp = Temp::new();
    let mut dictionary = dictionary();
    dictionary.learn("はし", "箸");
    assert_eq!(converter::convert(&dictionary, "はし", 9).text(), "箸");
    dictionary.save_learning(temp.path("learning.tsv")).unwrap();
    assert_eq!(
        fs::metadata(temp.path("learning.tsv"))
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o600
    );
    let mut loaded = Dictionary::default();
    loaded.load_learning(temp.path("learning.tsv")).unwrap();
    assert_eq!(loaded.lookup("はし", 1)[0].text, "箸");
    for i in 0..4200 {
        dictionary.learn(&format!("よみ{i}"), "語");
    }
    assert_eq!(dictionary.learning_len(), 4096);
    fs::write(temp.path("user.tsv"), "にほんご\t二本語\t1\n").unwrap();
    dictionary.load(temp.path("user.tsv"), true).unwrap();
    let mut conversion = monakime::mozc::parse("にほんご", "にほんご\x1f日本語").unwrap();
    converter::rank(&dictionary, &mut conversion, 9);
    assert_eq!(conversion.candidates[0].text, "二本語");
    fs::write(temp.path("bad.nimedic"), b"NIMEDIC1").unwrap();
    assert!(dictionary.load(temp.path("bad.nimedic"), false).is_err());
}
#[test]
fn input_state_and_privacy() {
    let temp = Temp::new();
    let mut core = Core::new(config(&temp), None);
    for c in "nihongo".chars() {
        core.key(1, Key::Text(c.to_string()), false, false);
    }
    assert_eq!(core.view(1).preedit, "にほんご");
    assert!(core.key(1, Key::Convert, false, false).accepted);
    assert_eq!(core.view(1).preedit, "日本語");
    core.key(1, Key::Next, false, false);
    core.key(1, Key::Previous, false, false);
    assert_eq!(core.view(1).preedit, "日本語");
    assert_eq!(core.key(1, Key::Enter, false, false).commit, "日本語");
    assert!(core.view(1).preedit.is_empty());
    for c in "nihon".chars() {
        core.key(1, Key::Text(c.to_string()), false, false);
    }
    assert_eq!(core.key(1, Key::Enter, false, false).commit, "にほん");
    core.key(1, Key::Text("ka".into()), false, false);
    assert!(!core.key(1, Key::Text("a".into()), false, true).accepted);
    assert!(core.view(1).preedit.is_empty());
    core.key(1, Key::Muhenkan, false, false);
    assert!(core.view(1).latin);
    assert!(!core.key(1, Key::Text("a".into()), false, false).accepted);
    core.key(1, Key::JapaneseMode, false, false);
    core.key(1, Key::Text("ka".into()), false, false);
    assert_eq!(core.key(1, Key::Shortcut, false, false).commit, "か");
    assert!(core.view(2).preedit.is_empty());
    core.remove(1);
    assert!(core.view(1).preedit.is_empty());
}
