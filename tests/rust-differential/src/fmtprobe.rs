//! One formatting path per export, to find which part of `core::fmt` the recovered C gets wrong. Each returns a
//! checksum of the formatted string, or `u64::MAX` when formatting reports an error.

use std::fmt::Write;

fn digest(s: &str) -> u64 {
    s.bytes().fold(s.len() as u64, |acc, b| acc.wrapping_mul(31).wrapping_add(u64::from(b)))
}

fn run(f: impl FnOnce(&mut String) -> std::fmt::Result) -> u64 {
    let mut s = String::new();
    match f(&mut s) {
        Ok(()) => digest(&s),
        Err(_) => u64::MAX,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn fmt_literal(_a: u64, _b: u64, _c: u64) -> u64 {
    run(|s| write!(s, "literal text"))
}

#[unsafe(no_mangle)]
pub extern "C" fn fmt_u64(a: u64, _b: u64, _c: u64) -> u64 {
    run(|s| write!(s, "{a}"))
}

#[unsafe(no_mangle)]
pub extern "C" fn fmt_hex(a: u64, _b: u64, _c: u64) -> u64 {
    run(|s| write!(s, "{a:08x}"))
}

#[unsafe(no_mangle)]
pub extern "C" fn fmt_signed_padded(a: u64, _b: u64, _c: u64) -> u64 {
    run(|s| write!(s, "{:>+12}", a as i64))
}

#[unsafe(no_mangle)]
pub extern "C" fn fmt_bool_padded(a: u64, _b: u64, _c: u64) -> u64 {
    run(|s| write!(s, "{:>6}", a % 3 == 0))
}

#[unsafe(no_mangle)]
pub extern "C" fn fmt_two(a: u64, b: u64, _c: u64) -> u64 {
    run(|s| write!(s, "{a}:{b}"))
}
