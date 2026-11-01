//! One sorting path per export, to find which part of `slice::sort` the recovered C gets wrong above the small-sort
//! threshold of 32 elements. Each takes `u64 f(u64, u64, u64)` like the corpora and returns a checksum of the result.

use std::hint::black_box;

fn lcg(x: &mut u64) -> u64 {
    *x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *x >> 17
}

fn checksum(values: impl Iterator<Item = u64>) -> u64 {
    values.fold(0, |acc, v| acc.wrapping_mul(31).wrapping_add(v))
}

#[derive(Clone, Copy)]
struct Order {
    price: u64,
    size: u32,
    id: u32,
}

fn orders(n: u64, seed: u64) -> Vec<Order> {
    let mut x = seed;
    (0..black_box(n % 300))
        .map(|i| Order { price: lcg(&mut x) % 1000, size: (lcg(&mut x) % 50) as u32, id: i as u32 })
        .collect()
}

/// Stable sort of integers, driftsort above 32 elements
#[unsafe(no_mangle)]
pub extern "C" fn stable_u64(n: u64, seed: u64, _c: u64) -> u64 {
    let mut x = seed;
    let mut v: Vec<u64> = (0..black_box(n % 300)).map(|_| lcg(&mut x) % 1000).collect();
    v.sort();
    checksum(v.into_iter())
}

/// Unstable sort of integers, ipnsort above 32 elements
#[unsafe(no_mangle)]
pub extern "C" fn unstable_u64(n: u64, seed: u64, _c: u64) -> u64 {
    let mut x = seed;
    let mut v: Vec<u64> = (0..black_box(n % 300)).map(|_| lcg(&mut x) % 1000).collect();
    v.sort_unstable();
    checksum(v.into_iter())
}

/// Stable sort of structs by one key, the first sort of corpus3's sort_orders
#[unsafe(no_mangle)]
pub extern "C" fn stable_by_key(n: u64, seed: u64, _c: u64) -> u64 {
    let mut v = orders(n, seed);
    v.sort_by_key(|o| o.price);
    checksum(v.iter().map(|o| u64::from(o.id)))
}

/// Stable sort of structs by two keys, the second sort of corpus3's sort_orders
#[unsafe(no_mangle)]
pub extern "C" fn stable_by_two(n: u64, seed: u64, _c: u64) -> u64 {
    let mut v = orders(n, seed);
    v.sort_by(|a, b| b.size.cmp(&a.size).then(a.id.cmp(&b.id)));
    checksum(v.iter().map(|o| u64::from(o.id)))
}

/// Unstable sort of structs by one key
#[unsafe(no_mangle)]
pub extern "C" fn unstable_by_key(n: u64, seed: u64, _c: u64) -> u64 {
    let mut v = orders(n, seed);
    v.sort_unstable_by_key(|o| (o.price, o.id));
    checksum(v.iter().map(|o| u64::from(o.id)))
}
