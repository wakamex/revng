//! Probe of Rust's float formatting and parsing, one operation per function
use std::fmt::Write;

fn digest(s: &str) -> u64 {
    s.bytes().fold(s.len() as u64, |acc, b| acc.wrapping_mul(31).wrapping_add(u64::from(b)))
}

fn value(a: u64, b: u64) -> f64 {
    (a % 1_000_000_000) as f64 / 1000.0 - (b % 997) as f64
}

/// Fixed precision, Rust's exact formatting
#[unsafe(no_mangle)]
pub extern "C" fn fmt_precision(a: u64, b: u64, _c: u64) -> u64 {
    let mut s = String::new();
    write!(s, "{:.3}", value(a, b)).unwrap();
    digest(&s)
}

/// Exponent notation, shortest digits
#[unsafe(no_mangle)]
pub extern "C" fn fmt_exponent(a: u64, b: u64, _c: u64) -> u64 {
    let mut s = String::new();
    write!(s, "{:e}", value(a, b) * 1e-7).unwrap();
    digest(&s)
}

/// Plain display, shortest digits
#[unsafe(no_mangle)]
pub extern "C" fn fmt_display(a: u64, b: u64, _c: u64) -> u64 {
    let mut s = String::new();
    write!(s, "{}", value(a, b) / 3.0).unwrap();
    digest(&s)
}

/// Parsing a decimal string
#[unsafe(no_mangle)]
pub extern "C" fn parse_decimal(a: u64, b: u64, _c: u64) -> u64 {
    let s = format!("{}.{:03}", a % 1_000_000, b % 1000);
    s.parse::<f64>().map(f64::to_bits).unwrap_or(1)
}

/// Integer to float and back
#[unsafe(no_mangle)]
pub extern "C" fn int_float_round(a: u64, b: u64, _c: u64) -> u64 {
    (value(a, b) * 1000.0).round() as i64 as u64
}

fn bytes_at(s: &str, at: usize) -> u64 {
    let mut b = [0u8; 8];
    for (i, c) in s.bytes().skip(at).take(8).enumerate() {
        b[i] = c;
    }
    u64::from_le_bytes(b)
}

/// Bytes 0 to 7 of the fixed precision string
#[unsafe(no_mangle)]
pub extern "C" fn precision_bytes_0(a: u64, b: u64, _c: u64) -> u64 {
    bytes_at(&format!("{:.3}", value(a, b)), 0)
}

/// Bytes 8 to 15 of the fixed precision string
#[unsafe(no_mangle)]
pub extern "C" fn precision_bytes_8(a: u64, b: u64, _c: u64) -> u64 {
    bytes_at(&format!("{:.3}", value(a, b)), 8)
}
