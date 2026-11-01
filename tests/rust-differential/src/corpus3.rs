//! More Rust idioms common in hl-node, each behind `extern "C" fn(u64, u64, u64) -> u64` like corpus2. No thread-local
//! storage: the hash map uses a fixed hasher instead of `RandomState`, whose keys are thread-local.

use std::collections::{BinaryHeap, HashMap, VecDeque};
use std::hash::{BuildHasherDefault, DefaultHasher};
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

/// Sorting structs by a key with `sort_by_key`, then by two keys with `sort_by`.
#[unsafe(no_mangle)]
pub extern "C" fn sort_orders(n: u64, seed: u64, _c: u64) -> u64 {
    let mut x = seed;
    let mut orders: Vec<Order> = (0..black_box(n % 300))
        .map(|i| Order { price: lcg(&mut x) % 1000, size: (lcg(&mut x) % 50) as u32, id: i as u32 })
        .collect();
    orders.sort_by_key(|o| o.price);
    let first = orders.iter().take(8).fold(0u64, |acc, o| acc.rotate_left(7) ^ u64::from(o.id));
    orders.sort_by(|a, b| b.size.cmp(&a.size).then(a.id.cmp(&b.id)));
    orders.iter().fold(first, |acc, o| acc.wrapping_mul(31).wrapping_add(u64::from(o.id) ^ o.price))
}

/// A max-heap of generated values, popped in order.
#[unsafe(no_mangle)]
pub extern "C" fn heap_order(n: u64, seed: u64, take: u64) -> u64 {
    let mut x = seed;
    let mut heap: BinaryHeap<u64> = (0..black_box(n % 500)).map(|_| lcg(&mut x) % 10_000).collect();
    let mut acc = heap.len() as u64;
    for _ in 0..(take % 64) {
        match heap.pop() {
            Some(v) => acc = acc.rotate_left(5) ^ v,
            None => break,
        }
    }
    acc
}

/// Signed 128-bit products, quotients and remainders, as signed fixed-point math does.
#[unsafe(no_mangle)]
pub extern "C" fn signed_wide(a: u64, b: u64, d: u64) -> u64 {
    let product = i128::from(a as i64) * i128::from(b as i64);
    let divisor = i128::from((d as i64) | 1);
    let quotient = product / divisor;
    let remainder = product % divisor;
    (quotient as u64) ^ ((quotient >> 64) as u64).rotate_left(11) ^ (remainder as u64).wrapping_mul(7)
}

/// Integer parsing from text built from the arguments, with the error mapped to a code.
#[unsafe(no_mangle)]
pub extern "C" fn parse_fields(a: u64, b: u64, c: u64) -> u64 {
    let text = format!("{a},{b},x{c}");
    let mut acc = 0u64;
    for field in text.split(',') {
        acc = acc.rotate_left(9)
            ^ match field.parse::<u64>() {
                Ok(v) => v,
                Err(_) => 0xdead,
            };
    }
    acc
}

trait Shape {
    fn area(&self) -> u64;
    fn scale(&self, k: u64) -> Box<dyn Shape>;
}
struct Square(u64);
struct Rect(u64, u64);
impl Shape for Square {
    fn area(&self) -> u64 { self.0.wrapping_mul(self.0) }
    fn scale(&self, k: u64) -> Box<dyn Shape> { Box::new(Square(self.0.wrapping_mul(k))) }
}
impl Shape for Rect {
    fn area(&self) -> u64 { self.0.wrapping_mul(self.1) }
    fn scale(&self, k: u64) -> Box<dyn Shape> { Box::new(Rect(self.0.wrapping_mul(k), self.1)) }
}

/// Boxed trait objects whose methods return new boxed trait objects.
#[unsafe(no_mangle)]
pub extern "C" fn dyn_shapes(a: u64, b: u64, k: u64) -> u64 {
    let shapes: Vec<Box<dyn Shape>> =
        vec![Box::new(Square(a % 1000)), Box::new(Rect(a % 777, b % 555)), Box::new(Square(b % 333))];
    shapes.iter().map(|s| s.scale(k % 17).area()).fold(0, |acc, v| acc.rotate_left(3) ^ v)
}

/// Boxed closures capturing values, stored in a vector and applied in turn.
#[unsafe(no_mangle)]
pub extern "C" fn closure_chain(a: u64, b: u64, c: u64) -> u64 {
    let ops: Vec<Box<dyn Fn(u64) -> u64>> = vec![
        Box::new(move |x| x.wrapping_add(b)),
        Box::new(move |x| x.rotate_left((c % 64) as u32)),
        Box::new(|x| x ^ 0x5555),
        Box::new(move |x| x.wrapping_mul(b | 1)),
    ];
    ops.iter().fold(a, |acc, f| f(acc))
}

/// A hash map with a fixed hasher, counting generated keys.
#[unsafe(no_mangle)]
pub extern "C" fn hash_counts(n: u64, seed: u64, modulus: u64) -> u64 {
    let modulus = modulus % 256 + 1;
    let mut map: HashMap<u64, u64, BuildHasherDefault<DefaultHasher>> = HashMap::default();
    let mut x = seed;
    for _ in 0..black_box(n % 2000) {
        *map.entry(lcg(&mut x) % modulus).or_insert(0) += 1;
    }
    let mut pairs: Vec<(u64, u64)> = map.into_iter().collect();
    pairs.sort_unstable();
    pairs.iter().fold(pairs.len() as u64, |acc, (k, v)| acc.rotate_left(5) ^ k.wrapping_mul(*v))
}

/// A ring buffer through `VecDeque`, pushed at both ends and rotated.
#[unsafe(no_mangle)]
pub extern "C" fn deque_ops(n: u64, seed: u64, r: u64) -> u64 {
    let mut q: VecDeque<u64> = VecDeque::new();
    let mut x = seed;
    for i in 0..black_box(n % 400) {
        if i % 3 == 0 { q.push_front(lcg(&mut x)) } else { q.push_back(lcg(&mut x)) }
    }
    if !q.is_empty() {
        let k = (r as usize) % q.len();
        q.rotate_left(k);
    }
    q.iter().enumerate().fold(0, |acc, (i, v)| acc.rotate_left(7) ^ v.wrapping_add(i as u64))
}

/// Sliding windows and pairwise differences over a slice.
#[unsafe(no_mangle)]
pub extern "C" fn windows_max(n: u64, seed: u64, w: u64) -> u64 {
    let mut x = seed;
    let values: Vec<u64> = (0..black_box(n % 500 + 1)).map(|_| lcg(&mut x) % 100_000).collect();
    let w = (w % 16 + 1) as usize;
    let maxima: u64 = values.windows(w.min(values.len())).map(|win| *win.iter().max().unwrap()).fold(0, u64::wrapping_add);
    let diffs: u64 = values.windows(2).map(|p| p[0].abs_diff(p[1])).fold(0, u64::wrapping_add);
    maxima ^ diffs.rotate_left(13)
}

struct Node {
    value: u64,
    next: Option<Box<Node>>,
}

/// A singly linked list built and reversed through `Option<Box<T>>`.
#[unsafe(no_mangle)]
pub extern "C" fn linked_reverse(n: u64, seed: u64, _c: u64) -> u64 {
    let mut x = seed;
    let mut head: Option<Box<Node>> = None;
    for _ in 0..black_box(n % 300) {
        head = Some(Box::new(Node { value: lcg(&mut x), next: head }));
    }
    let mut reversed: Option<Box<Node>> = None;
    while let Some(mut node) = head {
        head = node.next.take();
        node.next = reversed;
        reversed = Some(node);
    }
    let mut acc = 0u64;
    let mut cursor = &reversed;
    while let Some(node) = cursor {
        acc = acc.rotate_left(3) ^ node.value;
        cursor = &node.next;
    }
    acc
}
