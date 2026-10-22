// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! Two sleeping workers: Mozc for live input, Zenz only for explicit conversion.
//! There is one pending request per backend. A generation number cancels old work.
use crate::{
    config::Config,
    converter::Conversion,
    mozc::Mozc,
    zenz::{self, Zenz},
};
use std::{
    collections::VecDeque,
    path::Path,
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    thread::{self, JoinHandle},
    time::Instant,
};

#[derive(Clone)]
pub struct Job {
    pub context: u64,
    pub generation: u64,
    pub reading: String,
    pub left_context: String,
    pub explicit: bool,
    pub ready: Instant,
}
#[derive(Clone)]
pub enum Answer {
    Mozc(Option<Conversion>),
    Neural(Option<String>),
}
pub struct Completed {
    pub job: Job,
    pub answer: Answer,
}

/// This callback may run on a worker; the native adapter only posts a UI event.
#[derive(Clone, Copy)]
pub struct Wake {
    pub callback: extern "C" fn(*mut std::ffi::c_void),
    pub user: usize,
}
struct Shared {
    latest: AtomicU64,
    stopping: AtomicBool,
    pending: [Mutex<Option<Job>>; 2],
    changed: [Condvar; 2],
    results: Mutex<VecDeque<Completed>>,
    wake: Option<Wake>,
}
pub struct Workers {
    shared: Arc<Shared>,
    threads: Vec<JoinHandle<()>>,
    pub mozc_enabled: bool,
    pub neural_enabled: bool,
}

impl Workers {
    pub fn new(config: &Config, wake: Option<Wake>) -> Self {
        let shared = Arc::new(Shared {
            latest: AtomicU64::new(0),
            stopping: AtomicBool::new(false),
            pending: std::array::from_fn(|_| Mutex::new(None)),
            changed: std::array::from_fn(|_| Condvar::new()),
            results: Mutex::new(VecDeque::new()),
            wake,
        });
        let mut out = Self {
            shared,
            threads: Vec::new(),
            mozc_enabled: !config.mozc_helper_path.is_empty()
                && Path::new(&config.mozc_helper_path).is_file(),
            neural_enabled: zenz::compiled()
                && !config.model_path.is_empty()
                && Path::new(&config.model_path).is_file(),
        };
        for kind in 0..2 {
            if !(if kind == 0 {
                out.mozc_enabled
            } else {
                out.neural_enabled
            }) {
                continue;
            }
            let shared = out.shared.clone();
            let config = config.clone();
            out.threads.push(
                thread::Builder::new()
                    .name(
                        if kind == 0 {
                            "monakime-mozc"
                        } else {
                            "monakime-zenz"
                        }
                        .into(),
                    )
                    .spawn(move || run(shared, config, kind))
                    .expect("worker thread"),
            );
        }
        out
    }
    pub fn invalidate(&self) -> u64 {
        let generation = self.shared.latest.fetch_add(1, Ordering::Relaxed) + 1;
        for kind in 0..2 {
            *self.shared.pending[kind].lock().unwrap() = None;
            self.shared.changed[kind].notify_one();
        }
        generation
    }
    pub fn is_current(&self, generation: u64) -> bool {
        self.shared.latest.load(Ordering::Relaxed) == generation
    }
    pub fn submit(&self, kind: usize, job: Job) {
        *self.shared.pending[kind].lock().unwrap() = Some(job);
        self.shared.changed[kind].notify_one();
    }
    pub fn drain(&self) -> Vec<Completed> {
        self.shared.results.lock().unwrap().drain(..).collect()
    }
}
impl Drop for Workers {
    fn drop(&mut self) {
        self.shared.stopping.store(true, Ordering::Release);
        self.shared.latest.fetch_add(1, Ordering::Relaxed);
        for condition in &self.shared.changed {
            condition.notify_one();
        }
        for thread in self.threads.drain(..) {
            let _ = thread.join();
        }
    }
}

fn run(shared: Arc<Shared>, config: Config, kind: usize) {
    // Construct each native resource on its owner thread. Zenz::new is lazy.
    let mut mozc = (kind == 0).then(|| {
        Mozc::new(
            config.mozc_helper_path.clone(),
            config.mozc_profile_path.clone(),
        )
    });
    let mut model = (kind == 1).then(|| Zenz::new(&config));
    struct Cached {
        reading: String,
        context: String,
        answer: Answer,
        bytes: usize,
    }
    let mut cache: VecDeque<Cached> = VecDeque::new();
    let mut cache_bytes = 0;
    let mut warned = false;
    loop {
        let job = {
            let mut pending = shared.pending[kind].lock().unwrap();
            loop {
                if shared.stopping.load(Ordering::Acquire) {
                    return;
                }
                match pending.as_ref() {
                    None => pending = shared.changed[kind].wait(pending).unwrap(),
                    Some(job) if job.ready > Instant::now() => {
                        let delay = job.ready.saturating_duration_since(Instant::now());
                        pending = shared.changed[kind].wait_timeout(pending, delay).unwrap().0;
                    }
                    Some(_) => break pending.take().unwrap(),
                }
            }
        };
        let cancelled = || {
            shared.stopping.load(Ordering::Relaxed)
                || shared.latest.load(Ordering::Relaxed) != job.generation
        };
        if cancelled() {
            continue;
        }
        let answer = if let Some(index) = cache
            .iter()
            .position(|c| c.reading == job.reading && c.context == job.left_context)
        {
            let cached = cache.remove(index).unwrap();
            let answer = cached.answer.clone();
            cache.push_front(cached);
            answer
        } else {
            let answer = if let Some(engine) = &mut mozc {
                let result = engine.convert(&job.reading, &job.left_context, &cancelled);
                if result.is_none() && !cancelled() && !warned {
                    eprintln!("MonakIME: {}", engine.error);
                    warned = true;
                }
                Answer::Mozc(result)
            } else {
                Answer::Neural(model.as_mut().unwrap().convert(
                    &job.reading,
                    &job.left_context,
                    &cancelled,
                ))
            };
            let bytes = match &answer {
                Answer::Mozc(Some(c)) => c.weight(),
                Answer::Neural(Some(s)) => s.len(),
                _ => 0,
            } + job.reading.len()
                + job.left_context.len()
                + std::mem::size_of::<Cached>();
            if !cancelled()
                && matches!(&answer, Answer::Mozc(Some(_)) | Answer::Neural(Some(_)))
                && bytes <= 512 * 1024
            {
                cache.push_front(Cached {
                    reading: job.reading.clone(),
                    context: job.left_context.clone(),
                    answer: answer.clone(),
                    bytes,
                });
                cache_bytes += bytes;
                while cache.len() > 32 || cache_bytes > 512 * 1024 {
                    cache_bytes -= cache.pop_back().unwrap().bytes;
                }
            }
            answer
        };
        if cancelled() {
            continue;
        }
        let notify = {
            let mut results = shared.results.lock().unwrap();
            results.retain(|r| r.job.generation == job.generation);
            let notify = results.is_empty();
            if results.len() == 2 {
                results.pop_front();
            }
            results.push_back(Completed { job, answer });
            notify
        };
        if notify {
            if let Some(wake) = shared.wake {
                (wake.callback)(wake.user as *mut std::ffi::c_void);
            }
        }
    }
}
