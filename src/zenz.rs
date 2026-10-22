// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! Zenz v3 prompt/response validation and lazy, CPU-only inference.
//! The C++ adapter handles llama.cpp's changing struct ABI; the IME policy is Rust.
use crate::{config::Config, text};

fn private_or_control(c: char) -> bool {
    matches!(c as u32, 0..=0x1f | 0x7f..=0x9f | 0xe000..=0xf8ff | 0xf0000..=0xffffd | 0x100000..=0x10fffd | 0xfffd)
}
pub fn make_prompt(reading: &str, context: &str) -> Option<String> {
    if reading.is_empty()
        || reading.len() > 512
        || reading.chars().count() > 128
        || reading.chars().any(private_or_control)
    {
        return None;
    }
    let context: String = text::suffix(context, 96)
        .chars()
        .filter_map(|c| {
            if matches!(c, ' ' | '\n' | '\t') {
                Some('　')
            } else if private_or_control(c) {
                None
            } else {
                Some(c)
            }
        })
        .collect();
    let reading = text::katakana(reading).replace(' ', "　");
    let prefix = if context.is_empty() {
        String::new()
    } else {
        format!("\u{ee02}{context}")
    };
    Some(format!("{prefix}\u{ee00}{reading}\u{ee01}"))
}
pub fn valid_output(reading: &str, output: &str) -> bool {
    !output.is_empty()
        && output.len() <= 1024
        && output.chars().count() <= 8.max(reading.chars().count() * 2 + 4)
        && !output.chars().any(private_or_control)
        && !["<s>", "</s>", "[PAD]"].iter().any(|s| output.contains(s))
}

pub const fn compiled() -> bool {
    cfg!(feature = "llama")
}
pub struct Zenz {
    pub error: String,
    #[cfg(feature = "llama")]
    handle: std::ptr::NonNull<std::ffi::c_void>,
    // A model is created, used and destroyed on the same worker thread.
    _single_owner: std::marker::PhantomData<std::rc::Rc<()>>,
}

#[cfg(feature = "llama")]
unsafe extern "C" {
    fn monakime_llama_create(
        path: *const std::ffi::c_char,
        threads: i32,
        budget: i32,
    ) -> *mut std::ffi::c_void;
    fn monakime_llama_destroy(handle: *mut std::ffi::c_void);
    fn monakime_llama_loaded(handle: *mut std::ffi::c_void) -> bool;
    fn monakime_llama_error(handle: *mut std::ffi::c_void) -> *const std::ffi::c_char;
    fn monakime_llama_generate(
        handle: *mut std::ffi::c_void,
        prompt: *const std::ffi::c_char,
        cancel: extern "C" fn(*mut std::ffi::c_void) -> bool,
        user: *mut std::ffi::c_void,
        output: *mut u8,
        capacity: usize,
    ) -> i32;
}
impl Zenz {
    pub fn new(config: &Config) -> Self {
        #[cfg(feature = "llama")]
        let handle = {
            let path = std::ffi::CString::new(config.model_path.as_str()).unwrap_or_default();
            // SAFETY: the adapter copies the path/configuration and creates an opaque
            // object. No model is loaded until generate() on this same thread.
            std::ptr::NonNull::new(unsafe {
                monakime_llama_create(path.as_ptr(), config.threads, config.inference_budget_ms)
            })
            .expect("native allocation")
        };
        #[cfg(not(feature = "llama"))]
        let _ = config;
        Self {
            error: String::new(),
            #[cfg(feature = "llama")]
            handle,
            _single_owner: std::marker::PhantomData,
        }
    }
    pub fn loaded(&self) -> bool {
        #[cfg(feature = "llama")]
        {
            unsafe { monakime_llama_loaded(self.handle.as_ptr()) }
        }
        #[cfg(not(feature = "llama"))]
        {
            false
        }
    }
    pub fn convert(
        &mut self,
        reading: &str,
        context: &str,
        cancelled: &(dyn Fn() -> bool + Sync),
    ) -> Option<String> {
        self.error.clear();
        if cancelled() {
            self.error = "zenz cancelled".into();
            return None;
        }
        let Some(prompt) = make_prompt(reading, context) else {
            self.error = "invalid or oversized zenz input".into();
            return None;
        };
        #[cfg(not(feature = "llama"))]
        {
            let _ = prompt;
            self.error = "MonakIME was built without llama.cpp".into();
            None
        }
        #[cfg(feature = "llama")]
        {
            struct Cancellation<'a>(&'a (dyn Fn() -> bool + Sync));
            extern "C" fn cancel(user: *mut std::ffi::c_void) -> bool {
                // SAFETY: generate() borrows this stack object synchronously and
                // never retains the pointer. Unwinding must not cross the C ABI.
                let state = unsafe { &*(user as *const Cancellation<'_>) };
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (state.0)()))
                    .unwrap_or(true)
            }
            let prompt = std::ffi::CString::new(prompt).unwrap();
            let mut cancellation = Cancellation(cancelled);
            let mut bytes = [0u8; 1025];
            // SAFETY: buffers live for the complete call; the adapter writes at most
            // capacity bytes and clears the callback before returning.
            let count = unsafe {
                monakime_llama_generate(
                    self.handle.as_ptr(),
                    prompt.as_ptr(),
                    cancel,
                    (&mut cancellation as *mut Cancellation<'_>).cast(),
                    bytes.as_mut_ptr(),
                    bytes.len(),
                )
            };
            if count < 0 {
                self.error =
                    unsafe { std::ffi::CStr::from_ptr(monakime_llama_error(self.handle.as_ptr())) }
                        .to_string_lossy()
                        .into_owned();
                return None;
            }
            let answer = std::str::from_utf8(bytes.get(..count as usize)?).ok();
            if let Some(answer) = answer.filter(|answer| valid_output(reading, answer)) {
                Some(answer.into())
            } else {
                self.error = "zenz output rejected by UTF-8/control/length checks".into();
                None
            }
        }
    }
}
impl Drop for Zenz {
    fn drop(&mut self) {
        #[cfg(feature = "llama")]
        // SAFETY: this instance uniquely owns the native context, on its owner thread.
        unsafe {
            monakime_llama_destroy(self.handle.as_ptr());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn v3_protocol_and_validation() {
        assert_eq!(
            make_prompt("きょう", "私の名前は").unwrap(),
            "\u{ee02}私の名前は\u{ee00}キョウ\u{ee01}"
        );
        assert_eq!(
            make_prompt("かな", "abc def\n").unwrap(),
            "\u{ee02}abc　def　\u{ee00}カナ\u{ee01}"
        );
        assert!(make_prompt("かな\u{ee01}", "").is_none());
        assert!(make_prompt(&"あ".repeat(129), "").is_none());
        assert!(valid_output("きょう", "今日"));
        assert!(!valid_output("きょう", "今日\n"));
        assert!(!valid_output("きょう", "今日<s>"));
        assert!(!valid_output("あ", &"a".repeat(60)));
    }
}
