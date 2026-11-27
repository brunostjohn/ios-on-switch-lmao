// SPDX-License-Identifier: GPL-3.0-or-later
use std::ops::Range;

/// One bit per container block, set when the block is in use. A 1 TiB container's is 32 MiB.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bitmap {
    words: Vec<u64>,
    len: u64,
}

impl Bitmap {
    pub fn new(len: u64) -> Self {
        Self {
            words: vec![0; len.div_ceil(64) as usize],
            len,
        }
    }

    pub fn len(&self) -> u64 {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn get(&self, i: u64) -> bool {
        i < self.len && self.words[(i / 64) as usize] >> (i % 64) & 1 == 1
    }

    pub fn set_range(&mut self, range: Range<u64>) {
        self.update(range, true);
    }

    pub fn clear_range(&mut self, range: Range<u64>) {
        self.update(range, false);
    }

    fn update(&mut self, range: Range<u64>, value: bool) {
        assert!(
            range.end <= self.len,
            "{range:?} is past the bitmap's {} bits",
            self.len
        );
        let mut i = range.start;
        while i < range.end {
            let word = (i / 64) as usize;
            let lo = i % 64;
            let hi = (range.end - i + lo).min(64);
            let mask = if hi - lo == 64 {
                u64::MAX
            } else {
                ((1u64 << (hi - lo)) - 1) << lo
            };
            if value {
                self.words[word] |= mask;
            } else {
                self.words[word] &= !mask;
            }
            i += hi - lo;
        }
    }

    pub fn count_set(&self, range: Range<u64>) -> u64 {
        assert!(range.end <= self.len);
        let mut count = 0u64;
        let mut i = range.start;
        while i < range.end {
            let word = (i / 64) as usize;
            let lo = i % 64;
            let hi = (range.end - i + lo).min(64);
            let mask = if hi - lo == 64 {
                u64::MAX
            } else {
                ((1u64 << (hi - lo)) - 1) << lo
            };
            count += u64::from((self.words[word] & mask).count_ones());
            i += hi - lo;
        }
        count
    }

    pub fn all_set(&self, range: Range<u64>) -> bool {
        self.count_set(range.clone()) == range.end - range.start
    }

    pub fn all_clear(&self, range: Range<u64>) -> bool {
        self.count_set(range) == 0
    }

    /// The lowest run of `count` clear bits.
    pub fn first_clear_run(&self, count: u64) -> Option<u64> {
        let mut start = 0u64;
        while start + count <= self.len {
            let clear = self.clear_run_at(start, count);
            if clear == count {
                return Some(start);
            }
            start = self.next_clear(start + clear + 1)?;
        }
        None
    }

    fn clear_run_at(&self, start: u64, want: u64) -> u64 {
        let mut i = start;
        while i < start + want && !self.get(i) {
            if i % 64 == 0 && i + 64 <= start + want && self.words[(i / 64) as usize] == 0 {
                i += 64;
            } else {
                i += 1;
            }
        }
        i - start
    }

    fn next_clear(&self, from: u64) -> Option<u64> {
        let mut i = from;
        while i < self.len {
            if i % 64 == 0 && self.words[(i / 64) as usize] == u64::MAX {
                i += 64;
            } else if self.get(i) {
                i += 1;
            } else {
                return Some(i);
            }
        }
        None
    }

    /// Every maximal run of set bits, in order.
    pub fn set_runs(&self) -> impl Iterator<Item = Range<u64>> + '_ {
        let mut i = 0u64;
        std::iter::from_fn(move || {
            while i < self.len && !self.get(i) {
                i = if i % 64 == 0 && self.words[(i / 64) as usize] == 0 {
                    i + 64
                } else {
                    i + 1
                };
            }
            if i >= self.len {
                return None;
            }
            let start = i;
            while i < self.len && self.get(i) {
                i = if i % 64 == 0 && i + 64 <= self.len && self.words[(i / 64) as usize] == u64::MAX {
                    i + 64
                } else {
                    i + 1
                };
            }
            Some(start..i)
        })
    }

    /// Bits `range` of this map as the chunk bitmap block Apple stores: bit `i` is bit `i % 8` of byte `i / 8`.
    /// `range.start` is chunk-aligned.
    pub fn to_bytes(&self, range: Range<u64>, out: &mut [u8]) {
        assert!(range.start % 64 == 0 && range.end <= self.len);
        let count = range.end - range.start;
        for n in 0..count.div_ceil(64) {
            let remaining = count - n * 64;
            let mask = if remaining >= 64 {
                u64::MAX
            } else {
                (1u64 << remaining) - 1
            };
            let word = self.words[(range.start / 64 + n) as usize] & mask;
            out[n as usize * 8..n as usize * 8 + 8].copy_from_slice(&word.to_le_bytes());
        }
    }

    /// Sets the bits a chunk bitmap block holds for `count` blocks from `start`, which is chunk-aligned.
    pub fn set_from_bytes(&mut self, start: u64, count: u64, bytes: &[u8]) {
        assert!(start % 64 == 0 && start + count <= self.len);
        let (words, _) = bytes[..count.div_ceil(64) as usize * 8].as_chunks::<8>();
        for (n, word) in words.iter().enumerate() {
            let remaining = count - n as u64 * 64;
            let mask = if remaining >= 64 {
                u64::MAX
            } else {
                (1u64 << remaining) - 1
            };
            self.words[(start / 64) as usize + n] |= u64::from_le_bytes(*word) & mask;
        }
    }
}
