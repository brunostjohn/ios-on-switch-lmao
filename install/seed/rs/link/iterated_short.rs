// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;

use crate::iterated_ucode::{
    Asm, E_GUARD, FF_ADD, FF_DST, FF_OVER, IteratedProgram, UCODE_ALPHA, W709, build_program, half_bits, splat,
};

pub fn base(asm: &mut Asm, dest: &str) {
    asm.mmadd(dest, "DST", "1-SRC_A", "SRC", "1-DST_A");
}

pub fn finish_w3c(asm: &mut Asm) {
    base(asm, "PBR");
    asm.add("T0", "T0", "PBR");
}

pub fn dot(asm: &mut Asm, x: &str, k: u32, dest: &str) {
    asm.mul_k("PBR.RRR", x, "K", k);
    asm.madd_k("PBR.GGG", x, "K", "PBR", k);
    asm.madd_k(&format!("{dest}.BBB"), x, "K", "PBR", k);
}

pub fn reduce3_min(asm: &mut Asm, x: &str, dest: &str) {
    asm.mov("PBR.GBR", x);
    asm.min("PBR.GBR", "PBR", x);
    asm.min(&format!("{dest}.GBR"), "PBR", x);
}

pub fn reduce3_max(asm: &mut Asm, x: &str, dest: &str) {
    asm.mov("PBR.GBR", x);
    asm.max("PBR.GBR", "PBR", x);
    asm.max(&format!("{dest}.GBR"), "PBR", x);
}

pub fn unpremultiply(asm: &mut Asm, color: &str, alpha: &str, dest: &str, k: u32) {
    asm.max_k("PBR", alpha, "K", k);
    asm.rcp("PBR", "PBR");
    asm.mul(dest, color, "PBR");
}

fn clamp_to_alpha(asm: &mut Asm, flags: &str, k_quarter: u32) {
    if flags.contains('u') {
        return;
    }
    asm.mmadd("PBR", "DST_A", "1-SRC_A", "SRC_A", "1-DST_A");
    asm.mmadd("PBR", "SRC_A", "DST_A", "PBR", "ONE");
    asm.min("PBR", "PBR", "ONE");
    if flags.contains('x') {
        asm.madd_k("PBR", "PBR", "K", "PBR", k_quarter);
    }
    asm.min("T0", "T0", "PBR");
}

fn ucode_alpha(asm: &mut Asm, a: &str, b: &str, c: &str, d: &str) {
    asm.mmadd("T1.R_TO_A", a, b, c, d);
    asm.mov("T0", "T0");
}

pub fn separable(name: &str, flags: &str) -> IteratedProgram {
    let mut consts = BTreeMap::new();
    let clamp = matches!(name, "cdg" | "cbn" | "sbt" | "div" | "llt");
    if clamp && flags.contains('x') {
        consts.insert(1, splat(half_bits(0.25)));
    }
    if matches!(name, "div" | "cdg" | "cbn" | "slt") {
        consts.insert(0, splat(E_GUARD));
    }
    let alpha = if name == "pdi" { FF_DST } else { FF_OVER };
    let full_name = if flags.is_empty() {
        name.to_owned()
    } else {
        format!("{name}.{flags}")
    };
    build_program(
        full_name,
        |asm| {
            match name {
                "mpy" => {
                    asm.add("PBR", "1-SRC_A", "SRC");
                    asm.mmadd("T0", "PBR", "DST", "SRC", "1-DST_A");
                    return;
                }
                "pdi" => {
                    asm.add("PBR", "SRC", "DST");
                    asm.sub("T0", "PBR", "SRC_A");
                    return;
                }
                "drk" => {
                    asm.mul("T0", "DST", "SRC_A");
                    asm.mul("PBR", "SRC", "DST_A");
                    asm.min("T0", "T0", "PBR");
                }
                "ltn" => {
                    asm.mul("T0", "DST", "SRC_A");
                    asm.mul("PBR", "SRC", "DST_A");
                    asm.max("T0", "T0", "PBR");
                }
                "dif" => {
                    asm.mul("T1", "DST", "SRC_A");
                    asm.mul("PBR", "SRC", "DST_A");
                    asm.max("T0", "T1", "PBR");
                    asm.mul("PBR", "SRC", "DST_A");
                    asm.min("T1", "T1", "PBR");
                    asm.sub("T0", "T0", "T1");
                }
                "ovl" => {
                    asm.add("PBR", "DST", "DST");
                    asm.sub("PBR.CC", "PBR", "DST_A");
                    asm.mmadd_cc("T0", "SRC", "DST", "SRC", "DST", "LT");
                    asm.sub_cc("PBR", "SRC_A", "SRC", "GE");
                    asm.mmadd_cc("T0", "PBR", "DST", "SRC", "DST_A", "GE");
                    asm.add_cc("T0", "T0", "T0", "GE");
                    asm.mmsub_cc("T0", "T0", "ONE", "SRC_A", "DST_A", "GE");
                }
                "hlt" => {
                    asm.add("PBR", "SRC", "SRC");
                    asm.sub("PBR.CC", "PBR", "SRC_A");
                    asm.mmadd_cc("T0", "SRC", "DST", "SRC", "DST", "LT");
                    asm.sub_cc("T1", "DST_A", "DST", "GE");
                    asm.add_cc("T1", "T1", "T1", "GE");
                    asm.sub_cc("PBR", "SRC_A", "SRC", "GE");
                    asm.mul_cc("T1", "T1", "PBR", "GE");
                    asm.mmsub_cc("T0", "SRC_A", "DST_A", "T1", "ONE", "GE");
                }
                "pin" => {
                    asm.add("PBR", "SRC", "SRC");
                    asm.sub("PBR.CC", "PBR", "SRC_A");
                    asm.mul("T1", "SRC_A", "DST");
                    asm.mmadd("T2", "SRC", "DST_A", "SRC", "DST_A");
                    asm.min_cc("T0", "T1", "T2", "LT");
                    asm.mmsub_cc("PBR", "T2", "ONE", "SRC_A", "DST_A", "GE");
                    asm.max_cc("T0", "T1", "PBR", "GE");
                }
                "sbt" => {
                    asm.mul("T0", "SRC", "DST_A");
                    asm.mul("PBR", "SRC_A", "DST");
                    asm.sub("T0", "T0", "PBR");
                }
                "div" => {
                    asm.mul("T1", "SRC", "DST_A");
                    asm.mul("PBR", "SRC_A", "DST");
                    asm.max("PBR", "PBR", "K");
                    asm.rcp("PBR", "PBR");
                    asm.mul("T0", "T1", "PBR");
                }
                "cdg" => {
                    asm.mul("T1", "SRC_A", "SRC_A");
                    asm.mul("T1", "T1", "DST");
                    asm.sub("PBR", "SRC_A", "SRC");
                    asm.max("PBR", "PBR", "K");
                    asm.rcp("PBR", "PBR");
                    asm.mul("PBR", "T1", "PBR");
                    asm.min("T0", "SRC_A", "PBR");
                }
                "cbn" => {
                    asm.mul("T1", "SRC_A", "SRC_A");
                    asm.sub("PBR", "DST_A", "DST");
                    asm.mul("T1", "T1", "PBR");
                    asm.max("PBR", "SRC", "K");
                    asm.rcp("PBR", "PBR");
                    asm.mul("PBR", "T1", "PBR");
                    asm.mmsub("T0", "SRC_A", "DST_A", "PBR", "ONE");
                    asm.sub("PBR.CC", "SRC", "K");
                    asm.mov_cc("T0", "ZERO", "LT");
                }
                "lbn" => {
                    asm.sub("PBR", "SRC_A", "SRC");
                    asm.mmsub("T0", "SRC_A", "DST", "PBR", "DST_A");
                }
                "llt" => {
                    asm.add("PBR", "SRC", "SRC");
                    asm.sub("PBR", "SRC_A", "PBR");
                    asm.mmsub("T0", "SRC_A", "DST", "PBR", "DST_A");
                }
                "slt" => {
                    asm.add("T1", "SRC", "SRC");
                    asm.mul("T2", "DST", "DST");
                    asm.sub("PBR", "T1", "SRC_A");
                    asm.mul("T2", "PBR", "T2");
                    asm.max("PBR", "DST_A", "K");
                    asm.rcp("PBR", "PBR");
                    asm.mul("T2", "T2", "PBR");
                    asm.mul("PBR", "T1", "DST");
                    asm.sub("T0", "PBR", "T2");
                }
                other => panic!("unknown separable blend {other}"),
            }
            finish_w3c(asm);
            if clamp {
                clamp_to_alpha(asm, flags, 1);
            }
        },
        consts,
        alpha,
    )
}

pub fn pld(f16: bool) -> IteratedProgram {
    build_program(
        if f16 { "pld.f16" } else { "pld" },
        |asm| {
            asm.add("T2", "SRC_A", "DST_A");
            asm.min("T1", "T2", "ONE");
            asm.sub("T2", "T1", "T2");
            asm.add("PBR", "SRC", "DST");
            asm.add("T0", "T2", "PBR");
            if f16 {
                asm.mov("T1.R_TO_A", "T1");
                asm.mov("T0", "T0");
            }
        },
        BTreeMap::new(),
        if f16 { UCODE_ALPHA } else { FF_ADD },
    )
}

pub fn sds() -> IteratedProgram {
    build_program(
        "sds",
        |asm| {
            asm.mul("PBR", "SRC_A", "DST_A");
            asm.sub("PBR", "ONE", "PBR");
            asm.mmadd("T0", "PBR", "DST", "SRC", "DST_A");
            asm.mov("T1.R_TO_A", "ONE");
            asm.mov("T0", "T0");
        },
        BTreeMap::new(),
        UCODE_ALPHA,
    )
}

pub fn sover_by_luma(lighten: bool) -> IteratedProgram {
    let mut consts = BTreeMap::new();
    consts.insert(0, W709);
    build_program(
        if lighten { "lso" } else { "dso" },
        |asm| {
            dot(asm, "DST", 0, "PBR");
            if lighten {
                asm.sub("PBR", "ONE", "PBR");
            }
            asm.max("PBR", "PBR", "ZERO");
            asm.min("PBR", "PBR", "ONE");
            asm.mul("PBR", "PBR", "PBR");
            asm.mul("T1", "PBR", "PBR");
            asm.mul("T2", "T1", "SRC_A");
            asm.sub("PBR", "ONE", "T2");
            asm.mmadd("T0", "PBR", "DST", "T1", "SRC");
            asm.sub("PBR", "ONE", "T2");
            ucode_alpha(asm, "PBR", "DST_A", "T2", "ONE");
        },
        consts,
        UCODE_ALPHA,
    )
}
