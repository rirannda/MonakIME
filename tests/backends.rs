// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
use monakime::{config::Config, mozc::Mozc, zenz::Zenz};
use std::{
    cell::Cell,
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};
struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        static SERIAL: AtomicU64 = AtomicU64::new(0);
        // Atomic creation also works when parallel test runners have the same
        // PID inside separate namespaces but share /tmp.
        let path = loop {
            let path = std::env::temp_dir().join(format!(
                "monakime-rust-backend-{}-{}",
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
    fn profile(&self, name: &str) -> String {
        self.0.join(name).to_string_lossy().into()
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
#[test]
fn cancelled_stream_and_failed_peers() {
    let temp = Temp::new();
    let peer = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/support/mozc-peer.py").to_owned();
    let mut engine = Mozc::new(peer.clone(), temp.profile("normal"));
    let calls = Cell::new(0);
    assert!(
        engine
            .convert("にほんご", "", &|| {
                calls.set(calls.get() + 1);
                calls.get() >= 2
            })
            .is_none()
    );
    assert_eq!(
        engine.convert("にほんご", "", &|| false).unwrap().text(),
        "日本語"
    );
    for name in ["timeout", "exit", "malformed"] {
        let mut engine = Mozc::new(peer.clone(), temp.profile(name));
        let started = Instant::now();
        assert!(
            engine.convert("にほんご", "", &|| false).is_none(),
            "{name}"
        );
        assert!(started.elapsed() < Duration::from_secs(2));
        assert!(!engine.error.is_empty());
    }
    let mut missing = Mozc::new(
        "/nonexistent-monakime-rust-peer".into(),
        temp.profile("missing"),
    );
    assert!(missing.convert("かな\nQ", "", &|| false).is_none());
    assert!(missing.convert("かな", "", &|| false).is_none());
    assert_eq!(
        missing
            .convert("GitHub12.5%", "", &|| false)
            .unwrap()
            .text(),
        "GitHub12.5%"
    );
}
#[test]
fn lazy_model_and_cancel_before_loading() {
    let mut config = Config::from_environment();
    config.model_path = "/nonexistent-monakime-rust.gguf".into();
    let mut model = Zenz::new(&config);
    assert!(!model.loaded());
    assert!(model.convert("かな", "", &|| true).is_none());
    assert!(!model.loaded());
    assert!(model.convert("かな", "", &|| false).is_none());
    assert!(!model.error.is_empty());
    assert!(model.convert("かな", "", &|| false).is_none());
}
#[test]
#[ignore = "requires scripts/setup-mozc.sh; also run by CTest"]
fn real_mozc() {
    let temp = Temp::new();
    let mut engine = Mozc::new(
        concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/third_party/mozc/meltype_mozc_helper"
        )
        .into(),
        temp.profile("real"),
    );
    for (reading, expected) in [
        ("はしでごはんをたべる", "橋でご飯を食べる"),
        (
            "Googleでしらべて、GitHubのこーどをしゅうせいしました。",
            "Googleで調べて、GitHubのコードを修正しました。",
        ),
        (
            "CPUしようりつは12.5%、めもりしようりょうは256MBでした。",
            "CPU使用率は12.5%、メモリ使用量は256MBでした。",
        ),
        ("にほんごk", "日本語k"),
    ] {
        let result = engine.convert(reading, "", &|| false).unwrap();
        assert_eq!(result.text(), expected);
        assert_eq!(
            result
                .segments
                .iter()
                .map(|s| s.reading.as_str())
                .collect::<String>(),
            reading
        );
    }
}
#[cfg(feature = "llama")]
#[test]
#[ignore = "requires scripts/setup-model.sh; also run by CTest"]
fn real_zenz_cpu() {
    let mut config = Config::from_environment();
    config.model_path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/models/zenz-v3.2-xsmall-Q5_K_M.gguf"
    )
    .into();
    let mut model = Zenz::new(&config);
    assert!(!model.loaded());
    assert_eq!(
        model
            .convert("はしでごはんをたべる", "", &|| false)
            .unwrap(),
        "箸でご飯を食べる"
    );
    assert!(model.loaded());
    assert_eq!(
        model.convert("はしをわたる", "", &|| false).unwrap(),
        "橋を渡る"
    );
}
