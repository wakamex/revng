//! The steps of corpus3's sort_orders and corpus4's float_sort, one per function
use std::hint::black_box;

fn lcg(x: &mut u64) -> u64 {
    *x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *x >> 17
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

fn ids(v: &[Order]) -> u64 {
    v.iter().fold(0u64, |acc, o| acc.wrapping_mul(31).wrapping_add(u64::from(o.id)))
}

/// corpus3's sort_orders unchanged
#[unsafe(no_mangle)]
pub extern "C" fn orders_both(n: u64, seed: u64, _c: u64) -> u64 {
    let mut orders = orders(n, seed);
    orders.sort_by_key(|o| o.price);
    let first = orders.iter().take(8).fold(0u64, |acc, o| acc.rotate_left(7) ^ u64::from(o.id));
    orders.sort_by(|a, b| b.size.cmp(&a.size).then(a.id.cmp(&b.id)));
    orders.iter().fold(first, |acc, o| acc.wrapping_mul(31).wrapping_add(u64::from(o.id) ^ o.price))
}

/// The first sort, every id in order
#[unsafe(no_mangle)]
pub extern "C" fn orders_first_sort(n: u64, seed: u64, _c: u64) -> u64 {
    let mut orders = orders(n, seed);
    orders.sort_by_key(|o| o.price);
    ids(&orders)
}

/// The fold of the first eight ids after the first sort
#[unsafe(no_mangle)]
pub extern "C" fn orders_first_eight(n: u64, seed: u64, _c: u64) -> u64 {
    let mut orders = orders(n, seed);
    orders.sort_by_key(|o| o.price);
    orders.iter().take(8).fold(0u64, |acc, o| acc.rotate_left(7) ^ u64::from(o.id))
}

/// The second sort after the first, every id in order
#[unsafe(no_mangle)]
pub extern "C" fn orders_second_sort(n: u64, seed: u64, _c: u64) -> u64 {
    let mut orders = orders(n, seed);
    orders.sort_by_key(|o| o.price);
    orders.sort_by(|a, b| b.size.cmp(&a.size).then(a.id.cmp(&b.id)));
    ids(&orders)
}

fn floats(n: u64, seed: u64) -> Vec<f64> {
    let mut x = seed;
    (0..black_box(n % 200))
        .map(|i| if i % 37 == 5 { f64::NAN } else { (lcg(&mut x) % 20_000) as f64 / 7.0 - 1000.0 })
        .collect()
}

/// corpus4's float_sort sort, every value's bits in order
#[unsafe(no_mangle)]
pub extern "C" fn floats_sorted(n: u64, seed: u64, _c: u64) -> u64 {
    let mut v = floats(n, seed);
    v.sort_by(|a, b| a.total_cmp(b));
    v.iter().fold(0u64, |acc, f| acc.wrapping_mul(31) ^ f.to_bits())
}

/// The minimum and maximum through f64::min and f64::max, unsorted
#[unsafe(no_mangle)]
pub extern "C" fn floats_min_max(n: u64, seed: u64, _c: u64) -> u64 {
    let v = floats(n, seed);
    let lo = v.iter().copied().fold(f64::INFINITY, f64::min);
    let hi = v.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    lo.to_bits() ^ hi.to_bits().rotate_left(9)
}

/// The sum of clamped values that are not NaN, unsorted
#[unsafe(no_mangle)]
pub extern "C" fn floats_clamped_sum(n: u64, seed: u64, _c: u64) -> u64 {
    let v = floats(n, seed);
    let s: f64 = v.iter().map(|f| f.clamp(-500.0, 500.0)).filter(|f| !f.is_nan()).sum();
    s.to_bits()
}
