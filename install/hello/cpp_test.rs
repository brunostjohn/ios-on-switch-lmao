// SPDX-License-Identifier: GPL-3.0-or-later
#[test]
fn cpp17_ffi_checksum_matches() {
    const EXPECTED_PAYLOAD: &[u8] = b"pack.bin|alpha:10,beta:20,gamma:30,|1,3,5,7,9,|1:2,1:4,2:1,2:3,";
    let mut hash: u32 = 0x811c_9dc5;
    for &byte in EXPECTED_PAYLOAD {
        hash ^= u32::from(byte);
        hash = hash.wrapping_mul(0x0100_0193);
    }
    let expected = (hash & 0x7fff_ffff) as i32;
    assert_eq!(switch_hello_cpp_core::cpp_check(), expected);
}
