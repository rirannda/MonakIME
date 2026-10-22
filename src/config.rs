// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! Settings are loaded once at startup. Environment variables override the file.
use std::{collections::BTreeMap, env, fs, path::Path};

#[derive(Clone, Debug)]
pub struct Config {
    pub dictionary_path: String,
    pub system_dictionary_path: String,
    pub english_dictionary_path: String,
    pub user_dictionary_path: String,
    pub learning_path: String,
    pub model_path: String,
    pub mozc_helper_path: String,
    pub mozc_profile_path: String,
    pub live_delay_ms: u64,
    pub threads: i32,
    pub inference_budget_ms: i32,
    pub max_input_chars: usize,
    pub enable_learning: bool,
}

impl Config {
    pub fn from_environment() -> Self {
        fn env_or(key: &str, default: String) -> String {
            env::var(key)
                .ok()
                .filter(|v| !v.is_empty())
                .or_else(|| {
                    key.strip_prefix("MONAKIME_")
                        .and_then(|suffix| env::var(format!("NEWIME_{suffix}")).ok())
                        .filter(|v| !v.is_empty())
                })
                .unwrap_or(default)
        }
        fn first_existing(paths: &[String]) -> String {
            paths
                .iter()
                .find(|p| Path::new(p).is_file())
                .unwrap_or_else(|| paths.last().unwrap())
                .clone()
        }
        let home = env_or("HOME", "/tmp".into());
        let config_root = env_or("XDG_CONFIG_HOME", format!("{home}/.config"));
        let data_root = env_or("XDG_DATA_HOME", format!("{home}/.local/share"));
        let config_home = format!("{config_root}/monakime");
        let data_home = format!("{data_root}/monakime");
        let legacy_config = format!("{config_root}/newime");
        let legacy_data = format!("{data_root}/newime");
        // Keep existing dictionaries and learning files in place. New files use
        // MonakIME's directory; a new-format file always takes precedence.
        let user_file = |new: String, old: String| {
            if !Path::new(&new).exists() && Path::new(&old).exists() {
                old
            } else {
                new
            }
        };
        let mut values = BTreeMap::new();
        for directory in [&legacy_config, &config_home] {
            let Ok(file) = fs::read_to_string(format!("{directory}/config")) else {
                continue;
            };
            for line in file.lines().filter(|l| l.len() <= 4096).take(256) {
                let line = line.trim();
                if line.starts_with(['#', ';']) {
                    continue;
                }
                if let Some((key, value)) = line.split_once('=') {
                    values.insert(key.trim().to_owned(), value.trim().to_owned());
                }
                if values.len() > 32 {
                    break;
                }
            }
        }
        let value = |key: &str, variable: &str, default: String| {
            env_or(variable, values.get(key).cloned().unwrap_or(default))
        };
        let integer = |key, variable, default: u64, min, max| {
            value(key, variable, default.to_string())
                .parse::<u64>()
                .map(|n| n.clamp(min, max))
                .unwrap_or(default)
        };
        let enabled =
            |key, variable| !matches!(value(key, variable, "true".into()).as_str(), "0" | "false");
        let source = env!("CARGO_MANIFEST_DIR");
        let mut data = env_or(
            "MONAKIME_DATA_DIR",
            option_env!("MONAKIME_INSTALL_DATA")
                .or(option_env!("NEWIME_INSTALL_DATA"))
                .unwrap_or("/usr/share/monakime")
                .into(),
        );
        if !Path::new(&format!("{data}/base.tsv")).exists() {
            data = format!("{source}/data");
        }
        let model = first_existing(&[
            format!("{data_home}/models/zenz-v3.2-xsmall-Q5_K_M.gguf"),
            format!("{legacy_data}/models/zenz-v3.2-xsmall-Q5_K_M.gguf"),
            format!("{data}/models/zenz-v3.2-xsmall-Q5_K_M.gguf"),
            format!("{source}/models/zenz-v3.2-xsmall-Q5_K_M.gguf"),
        ]);
        let helper = first_existing(&[
            format!(
                "{}/meltype_mozc_helper",
                option_env!("MONAKIME_INSTALL_LIBEXEC")
                    .or(option_env!("NEWIME_INSTALL_LIBEXEC"))
                    .unwrap_or("/usr/libexec/monakime")
            ),
            format!("{source}/third_party/mozc/meltype_mozc_helper"),
            "/usr/lib/meltype/mozc/meltype_mozc_helper".into(),
        ]);
        Self {
            dictionary_path: value(
                "dictionary",
                "MONAKIME_DICTIONARY",
                format!("{data}/base.tsv"),
            ),
            system_dictionary_path: value(
                "system_dictionary",
                "MONAKIME_SYSTEM_DICTIONARY",
                format!("{data}/system.nimedic"),
            ),
            english_dictionary_path: value(
                "english_dictionary",
                "MONAKIME_ENGLISH_DICTIONARY",
                format!("{data}/english.txt"),
            ),
            user_dictionary_path: value(
                "user_dictionary",
                "MONAKIME_USER_DICTIONARY",
                user_file(
                    format!("{config_home}/user.tsv"),
                    format!("{legacy_config}/user.tsv"),
                ),
            ),
            learning_path: value(
                "learning_path",
                "MONAKIME_LEARNING_PATH",
                user_file(
                    format!("{data_home}/learning.tsv"),
                    format!("{legacy_data}/learning.tsv"),
                ),
            ),
            model_path: if enabled("zenz", "MONAKIME_ZENZ") {
                value("model", "MONAKIME_MODEL", model)
            } else {
                String::new()
            },
            mozc_helper_path: if enabled("live_conversion", "MONAKIME_LIVE_CONVERSION") {
                value("mozc_helper", "MONAKIME_MOZC_HELPER", helper)
            } else {
                String::new()
            },
            mozc_profile_path: value(
                "mozc_profile",
                "MONAKIME_MOZC_PROFILE",
                user_file(format!("{data_home}/mozc"), format!("{legacy_data}/mozc")),
            ),
            live_delay_ms: integer("live_delay_ms", "MONAKIME_LIVE_DELAY_MS", 25, 0, 200),
            threads: integer("threads", "MONAKIME_THREADS", 2, 1, 8) as i32,
            inference_budget_ms: integer("inference_budget_ms", "MONAKIME_BUDGET_MS", 500, 10, 5000)
                as i32,
            max_input_chars: integer("max_input_chars", "MONAKIME_MAX_INPUT_CHARS", 128, 8, 256)
                as usize,
            enable_learning: enabled("learning", "MONAKIME_LEARNING"),
        }
    }
    pub fn dictionary(&self) -> crate::dictionary::Dictionary {
        let mut dictionary = crate::dictionary::Dictionary::default();
        let _ = dictionary.load(&self.system_dictionary_path, false);
        let _ = dictionary.load(&self.dictionary_path, false);
        let _ = dictionary.load(&self.user_dictionary_path, true);
        let _ = dictionary.load_english(&self.english_dictionary_path);
        if self.enable_learning {
            let _ = dictionary.load_learning(&self.learning_path);
        }
        dictionary
    }
}
