// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
use std::env;

fn main() {
    for name in [
        "MONAKIME_NATIVE_DIR",
        "MONAKIME_INSTALL_DATA",
        "MONAKIME_INSTALL_LIBEXEC",
        "NEWIME_NATIVE_DIR",
        "NEWIME_INSTALL_DATA",
        "NEWIME_INSTALL_LIBEXEC",
    ] {
        println!("cargo:rerun-if-env-changed={name}");
    }
    if env::var_os("CARGO_FEATURE_LLAMA").is_some() {
        let path = env::var("MONAKIME_NATIVE_DIR")
            .or_else(|_| env::var("NEWIME_NATIVE_DIR"))
            .expect("Build CPU support with CMake, or omit the llama feature");
        println!("cargo:rustc-link-search=native={path}");
        for lib in ["monakime_llama", "llama", "ggml", "ggml-cpu", "ggml-base"] {
            println!("cargo:rerun-if-changed={path}/lib{lib}.a");
            println!("cargo:rustc-link-lib=static={lib}");
        }
        for lib in ["stdc++", "m", "pthread", "dl"] {
            println!("cargo:rustc-link-lib={lib}");
        }
    }
}
