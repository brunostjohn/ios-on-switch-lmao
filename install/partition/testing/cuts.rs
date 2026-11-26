// SPDX-License-Identifier: GPL-3.0-or-later
//! Power cuts over a recorded run. A barrier is the only order a card promises: writes before one are on the medium,
//! and of the writes since the last one any subset may be. A multi-sector write may land in part.

use switch_card_testing::{Event, SparseImage};

pub struct Cut {
    pub label: String,
    /// What reached the medium, in order.
    pub landed: Vec<(u64, Vec<u8>)>,
}

/// Every write a prefix; every multi-sector write torn after half its sectors; and, with `drops`, every epoch
/// (writes between two barriers) missing each one of its writes in turn.
pub fn cuts(events: &[Event], drops: bool) -> Vec<Cut> {
    let mut writes: Vec<(usize, u64, &[u8])> = Vec::new();
    let mut epoch = 0;
    for e in events {
        match e {
            Event::Write(lba, data) => writes.push((epoch, *lba, data)),
            Event::Barrier => epoch += 1,
        }
    }
    let owned = |w: &[(usize, u64, &[u8])]| w.iter().map(|&(_, l, d)| (l, d.to_vec())).collect::<Vec<_>>();
    let mut out = Vec::new();
    for k in 0..=writes.len() {
        out.push(Cut {
            label: format!("after {k} writes"),
            landed: owned(&writes[..k]),
        });
        if let Some(&(_, lba, data)) = writes.get(k)
            && data.len() > 512
        {
            let mut landed = owned(&writes[..k]);
            landed.push((lba, data[..data.len() / 1024 * 512].to_vec()));
            out.push(Cut {
                label: format!("write {k} torn at half"),
                landed,
            });
        }
    }
    if drops {
        let mut start = 0;
        while start < writes.len() {
            let e = writes[start].0;
            let end = start + writes[start..].iter().take_while(|w| w.0 == e).count();
            if end - start >= 2 {
                for skip in start..end {
                    let mut landed = owned(&writes[..start]);
                    landed.extend(
                        owned(&writes[start..end])
                            .into_iter()
                            .enumerate()
                            .filter(|(i, _)| start + i != skip)
                            .map(|(_, w)| w),
                    );
                    out.push(Cut {
                        label: format!("epoch of writes {start}..{end} without {skip}"),
                        landed,
                    });
                }
            }
            start = end;
        }
    }
    out
}

pub fn state(base: &SparseImage, landed: &[(u64, Vec<u8>)]) -> SparseImage {
    let mut image = base.clone();
    for (lba, data) in landed {
        image.set(*lba, data);
    }
    image
}

/// The same bytes everywhere, a sector never written reading as zeros.
pub fn first_difference(a: &SparseImage, b: &SparseImage) -> Option<u64> {
    a.data
        .keys()
        .chain(b.data.keys())
        .copied()
        .find(|&lba| a.sector(lba) != b.sector(lba))
}

/// The writes of a run whose events were recorded.
pub fn writes(events: &[Event]) -> Vec<(u64, Vec<u8>)> {
    events
        .iter()
        .filter_map(|e| match e {
            Event::Write(l, d) => Some((*l, d.clone())),
            Event::Barrier => None,
        })
        .collect()
}
