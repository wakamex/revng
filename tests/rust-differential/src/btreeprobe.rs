//! The steps of corpus4's btree_ranges, one per function
use std::collections::BTreeMap;
use std::hint::black_box;

fn lcg(x: &mut u64) -> u64 {
    *x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *x >> 17
}

fn book(n: u64, seed: u64) -> BTreeMap<u64, u64> {
    let mut x = seed;
    let mut book: BTreeMap<u64, u64> = BTreeMap::new();
    for _ in 0..black_box(n % 500) {
        *book.entry(lcg(&mut x) % 2000).or_default() += lcg(&mut x) % 100;
    }
    book
}

/// corpus4's btree_ranges unchanged
#[unsafe(no_mangle)]
pub extern "C" fn btree_all(n: u64, seed: u64, mid: u64) -> u64 {
    let book = book(n, seed);
    let m = mid % 2000;
    let below: u64 = book.range(..m).rev().take(5).map(|(p, s)| p * s).sum();
    let above: u64 = book.range(m..).take(5).map(|(p, s)| p ^ s).sum();
    let first = book.first_key_value().map_or(0, |(p, _)| *p);
    let last = book.last_key_value().map_or(0, |(p, _)| *p);
    below ^ above.rotate_left(20) ^ first.rotate_left(40) ^ last
}

/// Every key and value in order, through the plain iterator
#[unsafe(no_mangle)]
pub extern "C" fn btree_iter(n: u64, seed: u64, _c: u64) -> u64 {
    book(n, seed).iter().fold(0u64, |acc, (p, s)| acc.wrapping_mul(31).wrapping_add(p * 1000 + s))
}

/// The number of entries
#[unsafe(no_mangle)]
pub extern "C" fn btree_len(n: u64, seed: u64, _c: u64) -> u64 {
    book(n, seed).len() as u64
}

/// The first five entries from a lower bound
#[unsafe(no_mangle)]
pub extern "C" fn btree_above(n: u64, seed: u64, mid: u64) -> u64 {
    let book = book(n, seed);
    book.range(mid % 2000..).take(5).fold(0u64, |acc, (p, s)| acc.wrapping_mul(31).wrapping_add(p * 1000 + s))
}

/// The last five entries below an upper bound
#[unsafe(no_mangle)]
pub extern "C" fn btree_below(n: u64, seed: u64, mid: u64) -> u64 {
    let book = book(n, seed);
    book.range(..mid % 2000).rev().take(5).fold(0u64, |acc, (p, s)| acc.wrapping_mul(31).wrapping_add(p * 1000 + s))
}

/// The first and last keys
#[unsafe(no_mangle)]
pub extern "C" fn btree_first_last(n: u64, seed: u64, _c: u64) -> u64 {
    let book = book(n, seed);
    let first = book.first_key_value().map_or(0, |(p, _)| *p);
    let last = book.last_key_value().map_or(0, |(p, _)| *p);
    first << 32 | last
}
