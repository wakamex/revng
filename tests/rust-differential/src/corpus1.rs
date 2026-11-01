//! Functions shaped like hl-node's, for comparing rev.ng's C with the native code. Each is extern "C" so a harness
//! can call the original and the recovered library with the same arguments. None takes a Rust type at the boundary.

use std::cell::Cell;
use std::hint::black_box;

/// The spot unit conversion's shape: decimal units from digit counts, divisions with remainder checks, status codes.
#[unsafe(no_mangle)]
pub extern "C" fn unit_convert(price: u64, size: u64, quote_decimals: u8, size_decimals: u8, out: *mut u64) -> u16 {
    if quote_decimals < size_decimals {
        return 103;
    }
    let price_digits = quote_decimals - size_decimals;
    if price_digits > 8 {
        return 105;
    }
    let price_unit = 10u64.pow(u32::from(8 - price_digits));
    if price % price_unit != 0 || price_unit > price {
        return 105;
    }
    if size_decimals > 8 {
        return 106;
    }
    let size_unit = 10u64.pow(u32::from(8 - size_decimals));
    if size % size_unit != 0 {
        return 106;
    }
    unsafe {
        *out = price / price_unit;
        *out.add(1) = size / size_unit;
    }
    0
}

/// A Rust enum matched through a jump table, with seven arms doing different work.
#[derive(Clone, Copy)]
#[repr(u8)]
enum Kind {
    Spot,
    Perp,
    Vault,
    Staking,
    Bridge,
    Auction,
    Oracle,
}

#[inline(never)]
fn kind_of(tag: u8) -> Option<Kind> {
    Some(match tag {
        0 => Kind::Spot,
        1 => Kind::Perp,
        2 => Kind::Vault,
        3 => Kind::Staking,
        4 => Kind::Bridge,
        5 => Kind::Auction,
        6 => Kind::Oracle,
        _ => return None,
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dispatch(tag: u8, x: u64, y: u64) -> u64 {
    match kind_of(tag) {
        Some(Kind::Spot) => x.wrapping_add(y),
        Some(Kind::Perp) => x.wrapping_mul(3) ^ y,
        Some(Kind::Vault) => x.rotate_left(7).wrapping_sub(y),
        Some(Kind::Staking) => if y == 0 { 0 } else { x / y },
        Some(Kind::Bridge) => x.count_ones() as u64 + y.leading_zeros() as u64,
        Some(Kind::Auction) => x.max(y) - x.min(y),
        Some(Kind::Oracle) => (x >> 3) | (y << 61),
        None => u64::MAX,
    }
}

/// Checked arithmetic that reports overflow instead of panicking.
#[unsafe(no_mangle)]
pub extern "C" fn checked_notional(price: u64, size: u64, fee_bps: u64) -> u64 {
    let Some(notional) = price.checked_mul(size) else { return u64::MAX };
    let Some(fee) = notional.checked_mul(fee_bps) else { return u64::MAX - 1 };
    notional.checked_add(fee / 10_000).unwrap_or(u64::MAX - 2)
}

thread_local! {
    static CALLS: Cell<u64> = const { Cell::new(0) };
}

/// A thread-local counter: the C must address it through the thread pointer.
#[unsafe(no_mangle)]
pub extern "C" fn count_calls(step: u64) -> u64 {
    CALLS.with(|calls| {
        let next = calls.get().wrapping_add(step);
        calls.set(next);
        next
    })
}

/// Allocation, a call into another crate through the allocator, and a drop.
#[unsafe(no_mangle)]
pub extern "C" fn vec_checksum(n: u32, seed: u64) -> u64 {
    let n = black_box(n % 4096) as usize;
    let mut values: Vec<u64> = Vec::with_capacity(n);
    let mut x = seed;
    for _ in 0..n {
        x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        values.push(x >> 11);
    }
    values.sort_unstable();
    values.iter().enumerate().fold(0u64, |acc, (i, v)| acc.rotate_left(5) ^ v.wrapping_add(i as u64))
}

/// unwrap on a missing value panics; an extern "C" function cannot unwind, so the process aborts.
#[unsafe(no_mangle)]
pub extern "C" fn unwrap_lookup(table: *const u64, len: usize, index: usize) -> u64 {
    let slice = unsafe { std::slice::from_raw_parts(table, len) };
    let value = slice.get(index).copied();
    value.unwrap().wrapping_mul(2)
}

/// Division by a value the caller controls: the native code checks for zero and panics.
#[unsafe(no_mangle)]
pub extern "C" fn ratio(numerator: u64, denominator: u64) -> u64 {
    numerator / black_box(denominator)
}
