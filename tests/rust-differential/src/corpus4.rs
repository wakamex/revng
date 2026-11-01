//! Floating point and further Rust idioms common in hl-node, each behind `extern "C" fn(u64, u64, u64) -> u64` like
//! the other corpora. Float results are returned through `to_bits`, so a difference in the last bit counts.

use std::collections::BTreeMap;
use std::fmt::Write;
use std::hint::black_box;

fn lcg(x: &mut u64) -> u64 {
    *x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *x >> 17
}

/// Prices and sizes as fixed point integers converted to floats, multiplied, rounded and converted back
#[unsafe(no_mangle)]
pub extern "C" fn notional_f64(price: u64, size: u64, decimals: u64) -> u64 {
    let scale = 10f64.powi((decimals % 10) as i32);
    let p = (price % 10_000_000) as f64 / scale;
    let s = (size % 1_000_000) as f64 / 1e4;
    let notional = p * s;
    let rounded = (notional * 100.0).round() / 100.0;
    rounded.to_bits() ^ ((notional as u64).rotate_left(17)) ^ (notional.floor() as i64 as u64)
}

/// Square roots, fused multiply-adds and the sign of zero
#[unsafe(no_mangle)]
pub extern "C" fn sqrt_fma(a: u64, b: u64, c: u64) -> u64 {
    let x = f64::from_bits((a & 0x3fff_ffff_ffff_ffff) | 0x3ff0_0000_0000_0000);
    let y = (b % 100_000) as f64 - 50_000.0;
    let z = (c % 1000) as f64 * 0.125;
    let r = x.sqrt().mul_add(y, z);
    let neg_zero = (-0.0f64 * y).to_bits();
    r.to_bits() ^ neg_zero.rotate_left(5) ^ (y.abs().sqrt() as u64)
}

/// Conversions between integers, f32 and f64, saturating casts included
#[unsafe(no_mangle)]
pub extern "C" fn conversions(a: u64, b: u64, _c: u64) -> u64 {
    let f = f64::from_bits(a);
    let to_i = f as i64 as u64;
    let to_u = f as u64;
    let to_i32 = f as i32 as u32 as u64;
    let f32v = (b as i64) as f32;
    let back = f32v as f64;
    let i_to_f = (b as i64 as f64).to_bits();
    to_i ^ to_u.rotate_left(7) ^ to_i32.rotate_left(13) ^ u64::from(f32v.to_bits()) ^ back.to_bits().rotate_left(3) ^ i_to_f
}

/// Floats sorted by total order, then min, max and clamp over them, NaN included
#[unsafe(no_mangle)]
pub extern "C" fn float_sort(n: u64, seed: u64, _c: u64) -> u64 {
    let mut x = seed;
    let mut v: Vec<f64> = (0..black_box(n % 200))
        .map(|i| if i % 37 == 5 { f64::NAN } else { (lcg(&mut x) % 20_000) as f64 / 7.0 - 1000.0 })
        .collect();
    v.sort_by(|a, b| a.total_cmp(b));
    let lo = v.iter().copied().fold(f64::INFINITY, f64::min);
    let hi = v.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let clamped: f64 = v.iter().map(|f| f.clamp(-500.0, 500.0)).filter(|f| !f.is_nan()).sum();
    v.iter().fold(0u64, |acc, f| acc.wrapping_mul(31) ^ f.to_bits()) ^ lo.to_bits() ^ hi.to_bits().rotate_left(9) ^ clamped.to_bits()
}

fn digest(s: &str) -> u64 {
    s.bytes().fold(s.len() as u64, |acc, b| acc.wrapping_mul(31).wrapping_add(u64::from(b)))
}

/// Floats formatted with precision and exponent, then parsed back
#[unsafe(no_mangle)]
pub extern "C" fn format_f64(a: u64, b: u64, _c: u64) -> u64 {
    let f = (a % 1_000_000_000) as f64 / 1000.0 - (b % 997) as f64;
    let mut s = String::new();
    write!(s, "{f:.3} {:e} {}", f * 1e-7, f / 3.0).unwrap();
    let parsed: f64 = s.split(' ').next().unwrap().parse().unwrap_or(-1.0);
    digest(&s) ^ parsed.to_bits()
}

/// Iterator adapters over generated values
#[unsafe(no_mangle)]
pub extern "C" fn iter_adapters(n: u64, seed: u64, k: u64) -> u64 {
    let mut x = seed;
    let v: Vec<u64> = (0..black_box(n % 300)).map(|_| lcg(&mut x) % 1000).collect();
    let step = (k % 5 + 1) as usize;
    let zipped: u64 = v.iter().zip(v.iter().rev()).map(|(a, b)| a ^ b).sum();
    let chained: u64 = v.iter().step_by(step).chain(v.iter().take(3)).fold(0, |a, b| a.rotate_left(3) ^ b);
    let windows: u64 = v.windows(3).filter_map(|w| (w[0] < w[2]).then(|| w[1])).sum();
    let chunks: u64 = v.chunks(7).map(|c| c.iter().max().copied().unwrap_or(0)).sum();
    let scanned: u64 = v.iter().scan(0u64, |s, &e| { *s = s.wrapping_add(e); Some(*s) }).take_while(|s| *s < 50_000).count() as u64;
    let flat: u64 = v.iter().take(20).flat_map(|&e| [e, e / 2]).sum();
    zipped ^ chained.rotate_left(11) ^ windows.rotate_left(22) ^ chunks.rotate_left(33) ^ scanned ^ flat.rotate_left(44)
}

#[derive(Clone, Copy, Debug)]
struct Fill {
    px: u64,
    sz: u64,
    side: Option<bool>,
}

fn parse_fill(raw: u64) -> Result<Fill, &'static str> {
    let px = raw & 0xffff;
    if px == 0 {
        return Err("zero price");
    }
    let sz = (raw >> 16) & 0xffff;
    let side = match (raw >> 32) % 3 { 0 => None, 1 => Some(true), _ => Some(false) };
    Ok(Fill { px, sz, side })
}

fn signed_notional(raw: u64) -> Result<i64, &'static str> {
    let fill = parse_fill(raw)?;
    let side = fill.side.ok_or("no side")?;
    let n = fill.px.checked_mul(fill.sz).ok_or("overflow")? as i64;
    Ok(if side { n } else { -n })
}

/// Option and Result chains on structs, with the ? operator and ok_or
#[unsafe(no_mangle)]
pub extern "C" fn result_structs(a: u64, b: u64, c: u64) -> u64 {
    [a, b, c, a ^ b, b.wrapping_mul(c)]
        .iter()
        .map(|&r| match signed_notional(r) { Ok(n) => n as u64, Err(e) => digest(e) })
        .fold(0u64, |acc, v| acc.rotate_left(7) ^ v)
}

/// Range queries on an ordered map of price levels
#[unsafe(no_mangle)]
pub extern "C" fn btree_ranges(n: u64, seed: u64, mid: u64) -> u64 {
    let mut x = seed;
    let mut book: BTreeMap<u64, u64> = BTreeMap::new();
    for _ in 0..black_box(n % 500) {
        *book.entry(lcg(&mut x) % 2000).or_default() += lcg(&mut x) % 100;
    }
    let m = mid % 2000;
    let below: u64 = book.range(..m).rev().take(5).map(|(p, s)| p * s).sum();
    let above: u64 = book.range(m..).take(5).map(|(p, s)| p ^ s).sum();
    let first = book.first_key_value().map_or(0, |(p, _)| *p);
    let last = book.last_key_value().map_or(0, |(p, _)| *p);
    below ^ above.rotate_left(20) ^ first.rotate_left(40) ^ last
}

/// Bit operations: counts, leading and trailing zeros, byte swaps, saturating and checked arithmetic
#[unsafe(no_mangle)]
pub extern "C" fn bit_ops(a: u64, b: u64, c: u64) -> u64 {
    let pop = u64::from(a.count_ones()) | u64::from(b.leading_zeros()) << 8 | u64::from(c.trailing_zeros()) << 16;
    let swapped = a.swap_bytes() ^ b.reverse_bits();
    let sat = (a as i64).saturating_sub(b as i64) as u64 ^ a.saturating_mul(c);
    let checked = a.checked_div(c % 7).unwrap_or(0xdead) ^ (b as u32).checked_shl((c % 40) as u32).map_or(1, u64::from);
    let diff = (a as i64).abs_diff(b as i64);
    pop ^ swapped ^ sat.rotate_left(1) ^ checked.rotate_left(2) ^ diff
}
