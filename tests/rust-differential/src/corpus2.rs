//! Rust idioms common in hl-node, each behind `extern "C" fn(u64, u64, u64) -> u64` so that one harness can call
//! every export of the original and the recovered library with the same arguments. No function uses thread-local
//! storage: in a shared library it goes through `__tls_get_addr`, while hl-node, an executable, addresses it from fs.

use std::collections::BTreeMap;
use std::fmt::Write;
use std::hint::black_box;
use std::sync::atomic::{AtomicU64, Ordering};

trait Op: Sync {
    fn apply(&self, x: u64, y: u64) -> u64;
}
struct Add;
struct Mix(u64);
struct Shift(u32);
struct Clamp { low: u64, high: u64 }
impl Op for Add {
    fn apply(&self, x: u64, y: u64) -> u64 { x.wrapping_add(y) }
}
impl Op for Mix {
    fn apply(&self, x: u64, y: u64) -> u64 { (x ^ self.0).wrapping_mul(y | 1) }
}
impl Op for Shift {
    fn apply(&self, x: u64, y: u64) -> u64 { x.rotate_right(self.0) ^ y }
}
impl Op for Clamp {
    fn apply(&self, x: u64, y: u64) -> u64 { x.clamp(self.low, self.high).wrapping_sub(y) }
}
static OPS: [&dyn Op; 4] = [&Add, &Mix(0x9e37_79b9_7f4a_7c15), &Shift(13), &Clamp { low: 1000, high: 1 << 40 }];

/// A call through a trait object's vtable, chosen at run time.
#[unsafe(no_mangle)]
pub extern "C" fn trait_dispatch(kind: u64, x: u64, y: u64) -> u64 {
    let op = black_box(OPS[(kind % 4) as usize]);
    op.apply(x, y)
}

/// An iterator chain with closures capturing a local.
#[unsafe(no_mangle)]
pub extern "C" fn closure_fold(n: u64, seed: u64, k: u64) -> u64 {
    let k = k | 1;
    (0..black_box(n % 2000))
        .map(|i| i.wrapping_mul(seed) ^ (i >> 3))
        .filter(|v| v % k != 0)
        .enumerate()
        .fold(0u64, |acc, (i, v)| acc.rotate_left(7) ^ v.wrapping_add(i as u64))
}

/// 128-bit products and quotients, as fixed-point notional math does.
#[unsafe(no_mangle)]
pub extern "C" fn wide_math(a: u64, b: u64, d: u64) -> u64 {
    let product = u128::from(a) * u128::from(b);
    let quotient = product / u128::from(d.max(1));
    let remainder = product % u128::from(d.max(1));
    (quotient as u64) ^ ((quotient >> 64) as u64).rotate_left(17) ^ (remainder as u64).wrapping_mul(3)
}

/// Checked, saturating and bit operations, packed into one value.
#[unsafe(no_mangle)]
pub extern "C" fn overflow_ops(a: u64, b: u64, op: u64) -> u64 {
    match op % 8 {
        0 => a.checked_add(b).unwrap_or(u64::MAX - 1),
        1 => a.checked_sub(b).map_or(7, |v| v ^ 0xff),
        2 => a.checked_mul(b).unwrap_or(3),
        3 => a.saturating_mul(b).saturating_add(a),
        4 => u64::from(a.leading_zeros()) << 32 | u64::from(b.trailing_zeros()) << 16 | u64::from((a ^ b).count_ones()),
        5 => (a as i64).checked_div(b as i64).map_or(11, |v| v as u64),
        6 => (a as i64).wrapping_rem((b as i64) | 1) as u64,
        _ => a.abs_diff(b).swap_bytes(),
    }
}

/// An enum carrying data, built in one function and consumed in another.
enum Msg {
    Order { price: u64, size: u32, buy: bool },
    Cancel(u64),
    Batch(u8, u64),
    Noop,
}

#[inline(never)]
fn build(tag: u64, x: u64, y: u64) -> Msg {
    match tag % 5 {
        0 => Msg::Order { price: x, size: y as u32, buy: y & 1 == 1 },
        1 => Msg::Cancel(x ^ y),
        2 => Msg::Batch((y % 7) as u8, x),
        3 => Msg::Order { price: y, size: x as u32, buy: false },
        _ => Msg::Noop,
    }
}

#[inline(never)]
fn handle(msg: &Msg) -> u64 {
    match *msg {
        Msg::Order { price, size, buy } => price.wrapping_mul(u64::from(size)) ^ u64::from(buy),
        Msg::Cancel(id) => id.rotate_left(9),
        Msg::Batch(n, base) => (0..u64::from(n)).fold(base, |acc, i| acc.wrapping_add(i * i)),
        Msg::Noop => 42,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn enum_data(tag: u64, x: u64, y: u64) -> u64 {
    handle(&black_box(build(tag, x, y)))
}

/// `?` through a chain of fallible steps with an error enum mapped to codes.
#[derive(Debug)]
enum Error {
    Zero,
    TooLarge(u64),
    Misaligned { value: u64, unit: u64 },
}

#[inline(never)]
fn step_nonzero(v: u64) -> Result<u64, Error> {
    if v == 0 { Err(Error::Zero) } else { Ok(v) }
}

#[inline(never)]
fn step_bounded(v: u64, limit: u64) -> Result<u64, Error> {
    if v > limit { Err(Error::TooLarge(v - limit)) } else { Ok(v) }
}

#[inline(never)]
fn step_aligned(v: u64, unit: u64) -> Result<u64, Error> {
    let unit = unit % 1000 + 1;
    if v % unit != 0 { Err(Error::Misaligned { value: v, unit }) } else { Ok(v / unit) }
}

fn validate(a: u64, b: u64, c: u64) -> Result<u64, Error> {
    let a = step_nonzero(a)?;
    let a = step_bounded(a, b)?;
    step_aligned(a, c)
}

#[unsafe(no_mangle)]
pub extern "C" fn result_chain(a: u64, b: u64, c: u64) -> u64 {
    match validate(a, b, c) {
        Ok(v) => v,
        Err(Error::Zero) => 1 << 63,
        Err(Error::TooLarge(by)) => (1 << 62) | (by & 0xffff),
        Err(Error::Misaligned { value, unit }) => (1 << 61) | (value % unit) << 16 | unit,
    }
}

/// An ordered map built from generated keys, then folded in order.
#[unsafe(no_mangle)]
pub extern "C" fn btree_fold(n: u64, seed: u64, modulus: u64) -> u64 {
    let modulus = modulus % 512 + 1;
    let mut map = BTreeMap::new();
    let mut x = seed;
    for _ in 0..black_box(n % 3000) {
        x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        *map.entry((x >> 33) % modulus).or_insert(0u64) += x >> 50;
    }
    map.iter().fold(map.len() as u64, |acc, (k, v)| acc.rotate_left(3) ^ k.wrapping_mul(*v | 1))
}

/// Integer and boolean formatting through `core::fmt`, measured by length and a byte checksum. Floats are left out:
/// the recovered C reaches QEMU's softfloat through CPU-state globals the harness does not define.
#[unsafe(no_mangle)]
pub extern "C" fn format_digest(x: u64, y: u64, z: u64) -> u64 {
    let mut s = String::new();
    write!(s, "{x}:{y:08x}:{:>+12}:{:>6}", z as i64, (z % 3 == 0)).unwrap();
    s.bytes().fold(s.len() as u64, |acc, b| acc.wrapping_mul(31).wrapping_add(u64::from(b)))
}

/// Sorting, rotating and searching a stack array.
#[unsafe(no_mangle)]
pub extern "C" fn stack_slices(seed: u64, k: u64, needle: u64) -> u64 {
    let mut a = [0u64; 64];
    let mut x = seed;
    for v in a.iter_mut() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        *v = x % 1000;
    }
    a.rotate_left((k % 64) as usize);
    let mut b = [0u64; 64];
    b.copy_from_slice(&a);
    b.sort_unstable();
    let found = match b.binary_search(&(needle % 1000)) {
        Ok(i) => i as u64,
        Err(i) => 1000 + i as u64,
    };
    found ^ a[0] << 20 ^ b[63] << 40
}

/// A match returning string literals, read back as bytes.
#[unsafe(no_mangle)]
pub extern "C" fn names(x: u64, y: u64, _z: u64) -> u64 {
    let name = match black_box(x % 12) {
        0 => "spot",
        1 => "perp",
        2 => "vault",
        3 => "staking",
        4 => "bridge",
        5 => "auction",
        6 => "oracle",
        7 => "funding",
        8 => "liquidation",
        9 => "deposit",
        10 => "withdrawal",
        _ => "unknown",
    };
    let byte = name.as_bytes()[(y as usize) % name.len()];
    (name.len() as u64) << 8 | u64::from(byte)
}

/// Large values copied by value and through raw copies.
#[derive(Clone, Copy)]
struct Wide {
    fields: [u64; 12],
}

#[inline(never)]
fn widen(seed: u64) -> Wide {
    let mut fields = [0u64; 12];
    for (i, f) in fields.iter_mut().enumerate() {
        *f = seed.wrapping_mul(i as u64 + 3).rotate_left(i as u32);
    }
    Wide { fields }
}

#[unsafe(no_mangle)]
pub extern "C" fn copy_wide(a: u64, b: u64, c: u64) -> u64 {
    let first = black_box(widen(a));
    let mut second = widen(b);
    let n = (c % 12) as usize;
    unsafe { std::ptr::copy_nonoverlapping(first.fields.as_ptr(), second.fields.as_mut_ptr(), n) };
    let third = black_box(second);
    third.fields.iter().fold(0, |acc, f| acc.rotate_left(5) ^ f)
}

/// A static atomic counter: the data segment, read and written by the recovered code.
static CALLS: AtomicU64 = AtomicU64::new(1000);

#[unsafe(no_mangle)]
pub extern "C" fn atomic_counter(add: u64, _y: u64, _z: u64) -> u64 {
    CALLS.fetch_add(add % 1000, Ordering::Relaxed) + add % 1000
}
