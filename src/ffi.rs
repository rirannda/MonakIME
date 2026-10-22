// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! The narrow C ABI used by fcitx's C++ adapter.
//! Each view owns its strings; native code reads it and then calls view_free.
use crate::{
    config::Config,
    session::{Core, Key, View},
    worker::Wake,
};
use std::{
    ffi::{CStr, CString, c_char, c_void},
    panic::{AssertUnwindSafe, catch_unwind},
    ptr,
};

#[repr(C)]
pub struct FfiView {
    pub preedit: *const c_char,
    pub commit: *const c_char,
    pub auxiliary: *const c_char,
    pub candidates: *const *const c_char,
    pub count: usize,
    pub selection: usize,
    pub cursor: usize,
    pub highlight_start: usize,
    pub highlight_end: usize,
    pub flags: u32,
    strings: Vec<CString>,
    candidate_pointers: Vec<*const c_char>,
}
impl From<View> for FfiView {
    fn from(view: View) -> Self {
        let strings: Vec<_> = [view.preedit, view.commit, view.auxiliary]
            .into_iter()
            .chain(view.candidates)
            .map(|s| CString::new(s).unwrap_or_default())
            .collect();
        let pointers: Vec<_> = strings[3..].iter().map(|s| s.as_ptr()).collect();
        Self {
            preedit: strings[0].as_ptr(),
            commit: strings[1].as_ptr(),
            auxiliary: strings[2].as_ptr(),
            candidates: pointers.as_ptr(),
            count: pointers.len(),
            selection: view.selection,
            cursor: view.cursor,
            highlight_start: view.highlight_start,
            highlight_end: view.highlight_end,
            flags: u32::from(view.accepted)
                | (u32::from(view.latin) << 1)
                | (u32::from(view.save_requested) << 2),
            strings,
            candidate_pointers: pointers,
        }
    }
}
fn boundary<T>(default: T, operation: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(result) => result,
        Err(_) => {
            eprintln!("MonakIME: Rust operation panicked; no unwind crossed the native ABI");
            default
        }
    }
}
fn boxed(view: View) -> *mut FfiView {
    Box::into_raw(Box::new(view.into()))
}

// SAFETY CONTRACT FOR ALL FUNCTIONS BELOW:
// `handle` is a live, uniquely borrowed Box<Core> from core_new, used only on the UI
// thread. Strings are valid NUL-terminated buffers. Free functions are called once.
// The wake callback/user remain valid until core_free has joined both workers.
#[unsafe(no_mangle)]
/// Create the core; the returned pointer is uniquely owned by the native adapter.
/// # Safety
/// The callback and user pointer must remain valid until core_free joins the workers.
pub unsafe extern "C" fn monakime_core_new(
    callback: Option<extern "C" fn(*mut c_void)>,
    user: *mut c_void,
) -> *mut Core {
    boundary(ptr::null_mut(), || {
        Box::into_raw(Box::new(Core::new(
            Config::from_environment(),
            callback.map(|callback| Wake {
                callback,
                user: user as usize,
            }),
        )))
    })
}
#[unsafe(no_mangle)]
/// # Safety
/// `handle` must follow the ownership/thread contract above and be freed once.
pub unsafe extern "C" fn monakime_core_free(handle: *mut Core) {
    if !handle.is_null() {
        boundary((), || unsafe {
            drop(Box::from_raw(handle));
        });
    }
}
#[unsafe(no_mangle)]
/// # Safety
/// `handle` follows the contract above. `text` is null or a valid C string.
pub unsafe extern "C" fn monakime_key(
    handle: *mut Core,
    id: u64,
    action: u32,
    text: *const c_char,
    shift: bool,
    sensitive: bool,
    index: usize,
) -> *mut FfiView {
    if handle.is_null() {
        return ptr::null_mut();
    }
    boundary(ptr::null_mut(), || {
        let key = match action {
            0 => Key::Text(if text.is_null() {
                String::new()
            } else {
                unsafe { CStr::from_ptr(text) }
                    .to_str()
                    .unwrap_or_default()
                    .into()
            }),
            1 => Key::Convert,
            2 => Key::Enter,
            3 => Key::Escape,
            4 => Key::Backspace,
            5 => Key::Left,
            6 => Key::Right,
            7 => Key::Previous,
            8 => Key::Next,
            9 => Key::Hiragana,
            10 => Key::Katakana,
            11 => Key::LatinText,
            12 => Key::Muhenkan,
            13 => Key::JapaneseMode,
            14 => Key::Shortcut,
            15 => Key::OtherCommit,
            16 => Key::Select(index),
            _ => Key::Modifier,
        };
        boxed(unsafe { &mut *handle }.key(id, key, shift, sensitive))
    })
}
#[unsafe(no_mangle)]
/// # Safety
/// `handle` follows the ownership/thread contract above.
pub unsafe extern "C" fn monakime_reset(handle: *mut Core, id: u64) -> *mut FfiView {
    if handle.is_null() {
        return ptr::null_mut();
    }
    boundary(ptr::null_mut(), || {
        boxed(unsafe { &mut *handle }.reset(id, true))
    })
}
#[unsafe(no_mangle)]
/// # Safety
/// `handle` is live and has no concurrent mutable borrow.
pub unsafe extern "C" fn monakime_view(handle: *const Core, id: u64) -> *mut FfiView {
    if handle.is_null() {
        return ptr::null_mut();
    }
    boundary(ptr::null_mut(), || boxed(unsafe { &*handle }.view(id)))
}
#[unsafe(no_mangle)]
/// # Safety
/// `view` is a live pointer returned by this module; free it exactly once.
pub unsafe extern "C" fn monakime_view_free(view: *mut FfiView) {
    if !view.is_null() {
        unsafe {
            drop(Box::from_raw(view));
        }
    }
}
#[unsafe(no_mangle)]
/// # Safety
/// `handle` follows the ownership/thread contract above.
pub unsafe extern "C" fn monakime_remove(handle: *mut Core, id: u64) {
    if !handle.is_null() {
        boundary((), || unsafe { &mut *handle }.remove(id));
    }
}
#[unsafe(no_mangle)]
/// # Safety
/// `handle` follows the contract above. `ids` has space for `capacity` writable u64s.
pub unsafe extern "C" fn monakime_poll(handle: *mut Core, ids: *mut u64, capacity: usize) -> usize {
    if handle.is_null() || ids.is_null() {
        return 0;
    }
    boundary(0, || {
        let changed = unsafe { &mut *handle }.poll();
        let count = changed.len().min(capacity);
        unsafe {
            ptr::copy_nonoverlapping(changed.as_ptr(), ids, count);
        }
        count
    })
}
#[unsafe(no_mangle)]
/// # Safety
/// `handle` follows the ownership/thread contract above.
pub unsafe extern "C" fn monakime_save(handle: *mut Core) -> bool {
    !handle.is_null() && boundary(false, || unsafe { &mut *handle }.save())
}
