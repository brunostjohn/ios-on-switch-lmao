// SPDX-License-Identifier: GPL-3.0-or-later
//! One IPSW kind's IM4P members, decoded here and by //ipsw's rules: the bytes must be the same.

const TRUST_CACHES: [&str; 3] = [
    "STATIC_TRUST_CACHE",
    "SYSTEM_CRYPTEX_TRUST_CACHE",
    "APP_CRYPTEX_TRUST_CACHE",
];

fn env(name: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| panic!("{name} is not set"))
}

fn read(name: &str) -> Vec<u8> {
    let path = env(name);
    std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

fn hex_env<const N: usize>(name: &str) -> [u8; N] {
    hex::decode(env(name)).unwrap().try_into().unwrap()
}

fn same(role: &str, got: &[u8], oracle: &[u8]) {
    assert!(
        got == oracle,
        "{role}: {} bytes differ from //ipsw's {} bytes",
        got.len(),
        oracle.len()
    );
    println!("{role}: {} bytes, the same as //ipsw's", got.len());
}

#[test]
fn the_kernelcache_is_ipsws() {
    let pin = hex_env("SWITCH_IMG4_KERNELCACHE_SHA256");
    let got = switch_img4::kernelcache(&read("SWITCH_IMG4_KERNELCACHE_IM4P"), &pin).unwrap();
    same("kernelcache", &got, &read("SWITCH_IMG4_KERNELCACHE"));
}

#[test]
fn the_device_tree_is_ipsws() {
    let got = switch_img4::device_tree(&read("SWITCH_IMG4_DEVICE_TREE_IM4P")).unwrap();
    same("device tree", &got, &read("SWITCH_IMG4_DEVICE_TREE"));
}

#[test]
fn the_trust_caches_are_ipsws() {
    for role in TRUST_CACHES {
        let fourcc = env(&format!("SWITCH_IMG4_{role}_FOURCC"))
            .into_bytes()
            .try_into()
            .unwrap();
        let digest = hex_env(&format!("SWITCH_IMG4_{role}_DIGEST"));
        let got = switch_img4::payload(&read(&format!("SWITCH_IMG4_{role}_IM4P")), fourcc, Some(&digest)).unwrap();
        same(role, &got, &read(&format!("SWITCH_IMG4_{role}")));
    }
}
