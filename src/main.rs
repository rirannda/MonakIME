// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! Command-line entry point. The fcitx frontend uses the same Rust modules.
use monakime::{
    compose::compose,
    config::Config,
    converter::{self, Conversion, ReadingCheck},
    mozc::Mozc,
    zenz::{self, Zenz},
};
use serde_json::{Value, json};
use std::{
    env,
    io::{self, BufRead, Write},
    path::Path,
    time::Instant,
};

#[derive(Default)]
struct Options {
    input: String,
    context: String,
    json: bool,
    stdin: bool,
    live: bool,
    mozc: bool,
    diagnose: bool,
    benchmark: usize,
    learn: Option<(String, String)>,
}
fn usage() {
    println!(
        "MonakIME 0.2.0 (Rust)\nUsage: monakime-cli [options] [romaji-or-kana]\n  --json                JSON candidates and segments\n  --stdin               One conversion per line; backends stay loaded\n  --live                Mozc preview, never Zenz\n  --mozc                Mozc followed by explicit CPU Zenz\n  --no-model            Disable Zenz\n  --model PATH          Zenz v3 GGUF file\n  --context TEXT        Left context\n  --threads N           CPU threads (1..8)\n  --budget-ms N         Inference budget (10..5000)\n  --benchmark N         Dictionary timing over N conversions\n  --learn READING TEXT  Save preferred candidate\n  --diagnose            Paths and compiled capabilities"
    );
}
fn parse(config: &mut Config) -> Result<Options, String> {
    let mut options = Options::default();
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut next = || {
            args.next()
                .ok_or_else(|| format!("missing argument after {arg}"))
        };
        match arg.as_str() {
            "--help" | "-h" => {
                usage();
                std::process::exit(0);
            }
            "--json" => options.json = true,
            "--stdin" => options.stdin = true,
            "--live" => {
                options.live = true;
                options.mozc = true;
            }
            "--mozc" => options.mozc = true,
            "--diagnose" => options.diagnose = true,
            "--no-model" => config.model_path.clear(),
            "--model" => config.model_path = next()?,
            "--context" => options.context = next()?,
            "--threads" => config.threads = number(&next()?, 1, 8)? as i32,
            "--budget-ms" => config.inference_budget_ms = number(&next()?, 10, 5000)? as i32,
            "--benchmark" => options.benchmark = number(&next()?, 1, 1_000_000)?,
            "--learn" => options.learn = Some((next()?, next()?)),
            value if value.starts_with("--") => return Err(format!("unknown option: {value}")),
            value if options.input.is_empty() => options.input = value.into(),
            _ => return Err("provide one input argument (quote spaces)".into()),
        }
    }
    if options.live {
        config.model_path.clear();
    }
    Ok(options)
}
fn number(value: &str, min: usize, max: usize) -> Result<usize, String> {
    value
        .parse::<usize>()
        .ok()
        .filter(|n| (min..=max).contains(n))
        .ok_or_else(|| "numeric option out of range".into())
}
fn usage_stats() -> libc::rusage {
    // SAFETY: getrusage initializes the output struct on success; zero is also valid.
    let mut out = unsafe { std::mem::zeroed() };
    unsafe {
        libc::getrusage(libc::RUSAGE_SELF, &mut out);
    }
    out
}
fn cpu_seconds(value: &libc::rusage) -> f64 {
    value.ru_utime.tv_sec as f64
        + value.ru_stime.tv_sec as f64
        + (value.ru_utime.tv_usec + value.ru_stime.tv_usec) as f64 / 1_000_000.0
}
fn print(conversion: &Conversion, result: &Value, as_json: bool) {
    if as_json {
        println!("{result}");
    } else {
        for (i, candidate) in conversion.candidates.iter().enumerate() {
            println!("{}\t{}", i + 1, candidate.text);
        }
    }
}
fn main() {
    if let Err(error) = run() {
        eprintln!("monakime-cli: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), String> {
    let mut config = Config::from_environment();
    let options = parse(&mut config)?;
    let mut dictionary = config.dictionary();
    if let Some((reading, surface)) = options.learn {
        if !config.enable_learning {
            return Err("learning is disabled".into());
        }
        dictionary.learn(&reading, &surface);
        dictionary
            .save_learning(&config.learning_path)
            .map_err(|e| e.to_string())?;
        println!("Saved {reading} -> {surface}");
        return Ok(());
    }
    if options.diagnose {
        println!(
            "{}",
            json!({"name":"MonakIME","version":"0.2.0","implementation":"rust","llama":zenz::compiled(),"cpu_only":true,
            "dictionary_entries":dictionary.len(),"learning_entries":dictionary.learning_len(),"system_dictionary":config.system_dictionary_path,
            "model":config.model_path,"model_exists":Path::new(&config.model_path).is_file(),"threads":config.threads,"budget_ms":config.inference_budget_ms,
            "learning_path":config.learning_path,"user_dictionary":config.user_dictionary_path,"mozc_helper":config.mozc_helper_path,"mozc_exists":Path::new(&config.mozc_helper_path).is_file(),
            "mozc_profile":config.mozc_profile_path,"live_delay_ms":config.live_delay_ms})
        );
        return Ok(());
    }
    if dictionary.is_empty() {
        return Err("no dictionary found; set MONAKIME_DATA_DIR".into());
    }
    let mut mozc = Mozc::new(
        config.mozc_helper_path.clone(),
        config.mozc_profile_path.clone(),
    );
    let mut model = Zenz::new(&config);
    let mut convert = |raw: &str| -> Result<(Conversion, Value), String> {
        if raw.len() > config.max_input_chars * 4 || raw.chars().count() > config.max_input_chars {
            return Err("input exceeds configured max_input_chars".into());
        }
        let reading = compose(&dictionary, raw, !options.live);
        let mozc_conversion = options
            .mozc
            .then(|| mozc.convert(&reading, &options.context, &|| false))
            .flatten();
        let used_mozc = mozc_conversion.is_some();
        let mut conversion =
            mozc_conversion.unwrap_or_else(|| converter::convert(&dictionary, &reading, 9));
        if used_mozc {
            converter::rank(&dictionary, &mut conversion, 9);
        }
        let (mut neural, mut verified, mut model_ms) = (false, false, 0.0);
        let mut error = String::new();
        if !config.model_path.is_empty() && reading.chars().any(monakime::text::is_kana) {
            let started = Instant::now();
            let answer = model.convert(&reading, &options.context, &|| false);
            model_ms = started.elapsed().as_secs_f64() * 1000.0;
            error = model.error.clone();
            if let Some(answer) = answer {
                let check = converter::add_neural(&dictionary, &mut conversion, &answer);
                if check != ReadingCheck::Mismatch {
                    neural = true;
                    verified = check == ReadingCheck::Verified;
                } else {
                    error = "zenz output changed literal characters or kana reading".into();
                }
            }
        }
        let value = json!({"reading":reading,"neural":neural,"reading_verified":verified,"model_ms":model_ms,"model_error":error,
            "mozc":used_mozc,"mozc_error":if options.mozc { mozc.error.as_str() } else { "" },
            "candidates":conversion.candidates.iter().map(|c| c.text.as_str()).collect::<Vec<_>>(),
            "segments":conversion.segments.iter().map(|s| json!({"reading":s.reading,"text":s.text()})).collect::<Vec<_>>()});
        Ok((conversion, value))
    };
    if options.benchmark > 0 {
        let corpus = if options.input.is_empty() {
            vec![
                "kyouhatennkigaii",
                "nihongonyuuryoku",
                "toukyou",
                "hashi",
                "googlede検索",
                "konnichiha",
                "shinkansen",
                "jinkouchinou",
            ]
        } else {
            vec![options.input.as_str()]
        };
        for raw in &corpus {
            std::hint::black_box(converter::convert(
                &dictionary,
                &compose(&dictionary, raw, true),
                9,
            ));
        }
        let mut times = Vec::with_capacity(options.benchmark);
        let before = usage_stats();
        let started = Instant::now();
        for i in 0..options.benchmark {
            let request = Instant::now();
            std::hint::black_box(converter::convert(
                &dictionary,
                &compose(&dictionary, corpus[i % corpus.len()], true),
                9,
            ));
            times.push(request.elapsed().as_secs_f64() * 1_000_000.0);
        }
        let wall = started.elapsed().as_secs_f64();
        let after = usage_stats();
        times.sort_by(f64::total_cmp);
        let percentile = |p: f64| times[((times.len() - 1) as f64 * p) as usize];
        println!(
            "{}",
            json!({"requests":options.benchmark,"core_p50_us":percentile(0.5),"core_p95_us":percentile(0.95),"core_p99_us":percentile(0.99),
            "core_wall_s":wall,"core_cpu_s":cpu_seconds(&after)-cpu_seconds(&before),"core_peak_rss_kib":after.ru_maxrss,"dictionary_entries":dictionary.len()})
        );
        if !config.model_path.is_empty() {
            let (conversion, result) = convert(corpus[0])?;
            print(&conversion, &result, true);
        }
    } else if options.stdin {
        for line in io::stdin().lock().lines() {
            let line = line.map_err(|e| e.to_string())?;
            if line.is_empty() {
                continue;
            }
            let (conversion, result) = convert(&line)?;
            print(&conversion, &result, options.json);
            io::stdout().flush().map_err(|e| e.to_string())?;
        }
    } else if options.input.is_empty() {
        usage();
    } else {
        let (conversion, result) = convert(&options.input)?;
        print(&conversion, &result, options.json);
    }
    Ok(())
}
