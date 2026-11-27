// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, HashMap};
use std::sync::OnceLock;

use crate::iterated_short::{base, dot, pld, reduce3_max, reduce3_min, sds, separable, sover_by_luma, unpremultiply};
use crate::iterated_ucode::{
    Asm, E_GUARD, FF_DST, FF_OVER, IteratedProgram, UCODE_ALPHA, W601, build_program, half_bits, splat,
};

const LUM: u32 = 0;
const EPS: u32 = 1;

const SEPARABLE: &[&str] = &["mpy", "pdi", "drk", "ltn", "dif", "ovl", "hlt", "pin", "lbn", "slt"];
const RANGED: &[&str] = &["cdg", "cbn", "sbt", "div", "llt"];

pub fn dlc(unbounded: bool) -> IteratedProgram {
    let mut consts = BTreeMap::new();
    consts.insert(LUM, W601);
    consts.insert(EPS, splat(E_GUARD));
    build_program(
        if unbounded { "dlc.u" } else { "dlc" },
        |asm| {
            dot(asm, "SRC", LUM, "T2");
            dot(asm, "DST", LUM, "PBR");
            asm.sub("PBR", "T2", "PBR");
            asm.min("PBR", "PBR", "ZERO");
            asm.add("T0", "PBR", "DST");
            if !unbounded {
                dot(asm, "T0", LUM, "T2");
                reduce3_min(asm, "T0", "PBR");
                asm.sub("T1", "ZERO", "PBR");
                reduce3_max(asm, "T0", "PBR");
                asm.sub("PBR", "PBR", "ONE");
                asm.max("PBR.CC", "T1", "PBR");
                asm.add_cc("PBR", "T2", "T1", "GT");
                asm.max_cc_k("PBR", "PBR", "K", "GT", EPS);
                asm.rcp_cc("PBR", "PBR", "GT");
                asm.mul_cc("T1", "T2", "PBR", "GT");
                asm.sub_cc("PBR", "T0", "T2", "GT");
                asm.madd_cc("T0", "T1", "PBR", "T2", "GT");
            }
            asm.mov("T0", "T0");
        },
        consts,
        FF_DST,
    )
}

fn set_lum(asm: &mut Asm) {
    dot(asm, "T1", LUM, "PBR");
    asm.sub("PBR", "T2", "PBR");
    asm.add("T1", "PBR", "T1");
    dot(asm, "T1", LUM, "T2");
    reduce3_max(asm, "T1", "T0");
    asm.sub("PBR.CC", "T0", "ONE");
    asm.sub_cc("PBR", "T0", "T2", "GT");
    asm.max_cc_k("PBR", "PBR", "K", "GT", EPS);
    asm.rcp_cc("T0", "PBR", "GT");
    asm.sub_cc("PBR", "ONE", "T2", "GT");
    asm.mul_cc("T0", "PBR", "T0", "GT");
    asm.sub_cc("PBR", "T1", "T2", "GT");
    asm.madd_cc("T0", "T0", "PBR", "T2", "GT");
    asm.mov_cc("T0", "T1", "LE");
    asm.mov("PBR.GBR", "T1");
    asm.min("PBR.GBR", "PBR", "T1");
    asm.min("PBR.GBR.CC", "PBR", "T1");
    asm.sub_cc("PBR", "T2", "PBR", "LT");
    asm.max_cc_k("PBR", "PBR", "K", "LT", EPS);
    asm.rcp_cc("PBR", "PBR", "LT");
    asm.mul_cc("T0", "T2", "PBR", "LT");
    asm.sub_cc("PBR", "T1", "T2", "LT");
    asm.madd_cc("T0", "T0", "PBR", "T2", "LT");
}

fn set_sat(asm: &mut Asm) {
    reduce3_min(asm, "T2", "PBR");
    asm.sub("T2", "T2", "PBR");
    asm.mov("PBR.GBR", "T2");
    asm.max("PBR.GBR", "PBR", "T2");
    asm.max("PBR.GBR.CC", "PBR", "T2");
    asm.rcp_cc("T0", "PBR", "GT");
    asm.mul_cc("T2", "T2", "T1", "GT");
    asm.mul_cc("T2", "T2", "T0", "GT");
    asm.mov_cc("T2", "ZERO", "LE");
}

fn saturation(asm: &mut Asm, color: &str, alpha: &str) {
    unpremultiply(asm, color, alpha, "T2", EPS);
    reduce3_min(asm, "T2", "T1");
    reduce3_max(asm, "T2", "PBR");
    asm.sub("T1", "PBR", "T1");
}

fn finish_hsl(asm: &mut Asm) {
    asm.mul("PBR", "SRC_A", "DST_A");
    asm.mul("T0", "T0", "PBR");
    base(asm, "PBR");
    asm.add("T0", "T0", "PBR");
}

pub fn hsl(name: &str) -> IteratedProgram {
    let mut consts = BTreeMap::new();
    consts.insert(LUM, W601);
    consts.insert(EPS, splat(E_GUARD));
    build_program(
        name,
        |asm| {
            let l_of = if matches!(name, "col" | "lum") {
                let (c_of, l_of) = if name == "lum" {
                    (("DST", "DST_A"), ("SRC", "SRC_A"))
                } else {
                    (("SRC", "SRC_A"), ("DST", "DST_A"))
                };
                unpremultiply(asm, c_of.0, c_of.1, "T1", EPS);
                l_of
            } else {
                let (sat_of, sat_on) = if name == "hue" {
                    (("DST", "DST_A"), ("SRC", "SRC_A"))
                } else {
                    (("SRC", "SRC_A"), ("DST", "DST_A"))
                };
                saturation(asm, sat_of.0, sat_of.1);
                unpremultiply(asm, sat_on.0, sat_on.1, "T2", EPS);
                set_sat(asm);
                asm.mov("T1", "T2");
                ("DST", "DST_A")
            };
            unpremultiply(asm, l_of.0, l_of.1, "T2", EPS);
            dot(asm, "T2", LUM, "T2");
            set_lum(asm);
            finish_hsl(asm);
        },
        consts,
        FF_OVER,
    )
}

fn cli_matrix(asm: &mut Asm, src: &str, acc: &str, k0: u32, offset_swz: &str) {
    asm.mov("PBR.GGG", src);
    asm.mul_k(acc, "PBR", "K", k0 + 1);
    asm.mov_k(&format!("PBR.{offset_swz}"), "K", 6);
    asm.add(acc, acc, "PBR");
    asm.mov("PBR.RRR", src);
    asm.madd_k(acc, "PBR", "K", acc, k0);
    asm.mov("PBR.BBB", src);
    asm.madd_k(acc, "PBR", "K", acc, k0 + 2);
}

fn cli_alpha_times(asm: &mut Asm, lane_swz: &str, alpha: &str) {
    asm.mov(&format!("PBR.{lane_swz}"), "SRC");
    asm.mul("PBR", "PBR", alpha);
}

fn cli_clamp(asm: &mut Asm, x: &str, alpha_reg: &str, lane_swz: &str) {
    if alpha_reg == "T2" && lane_swz == "RRR" {
        asm.mul_k("PBR.BBB", "T2", "K", 6);
    } else {
        cli_alpha_times(asm, lane_swz, alpha_reg);
        asm.mul_k("PBR.BBB", "PBR", "K", 6);
    }
    asm.max(x, x, "PBR");
    if alpha_reg == "T2" && lane_swz == "RRR" {
        asm.madd_k("PBR.RRR", "T2", "K", "T2", 7);
    } else {
        cli_alpha_times(asm, lane_swz, alpha_reg);
        asm.madd_k("PBR.RRR", "PBR", "K", "PBR", 7);
    }
    asm.min(x, x, "PBR");
}

pub fn cli() -> IteratedProgram {
    let mut consts = BTreeMap::new();
    consts.insert(0, [0x3FA9, 0xAD71, 0xAD71]);
    consts.insert(1, [0xB493, 0x3EDB, 0xB493]);
    consts.insert(2, [0xA76D, 0xA76D, 0x3FE2]);
    consts.insert(3, [0x3D63, 0xAAC9, 0xAAC9]);
    consts.insert(4, [0xB1BA, 0x3CE2, 0xB1BA]);
    consts.insert(5, [0xA49C, 0xA49C, 0x3D87]);
    consts.insert(6, [0x3B33, 0x3266, 0xBA00]);
    consts.insert(7, [half_bits(1.086_400_032_043_457 - 1.0), 0, 0]);
    build_program(
        "cli",
        |asm| {
            cli_matrix(asm, "DST", "T1", 0, "RRR");
            asm.mov("PBR.RRR", "SRC");
            asm.mul("T2", "PBR", "DST_A");
            asm.mov("PBR.RRR", "SRC");
            asm.mul("T1", "T1", "PBR");
            cli_clamp(asm, "T1", "T2", "RRR");
            asm.sub("PBR", "ONE", "T2");
            asm.madd("T1", "PBR", "DST", "T1");
            asm.sub("PBR", "ONE", "T2");
            asm.madd("T2", "PBR", "DST_A", "T2");
            asm.mov("PBR.GGG", "SRC");
            asm.add("T0", "T2", "PBR");
            asm.max("PBR", "T0", "ZERO");
            asm.min("T2", "PBR", "ONE");
            asm.sub("PBR", "T2", "T0");
            asm.add("T1", "PBR", "T1");
            cli_matrix(asm, "T1", "T0", 3, "GGG");
            asm.mov("PBR.BBB", "SRC");
            asm.mul("T0", "T0", "PBR");
            cli_clamp(asm, "T0", "T2", "BBB");
            cli_alpha_times(asm, "BBB", "T2");
            asm.sub("PBR", "ONE", "PBR");
            asm.mmadd("T0", "PBR", "T1", "T0", "ONE");
            asm.mov("PBR.BBB", "SRC");
            asm.mul("T1", "PBR", "T2");
            asm.sub("PBR", "ONE", "T1");
            asm.mmadd("PBR", "PBR", "T2", "T1", "ONE");
            asm.min("T1.R_TO_A", "PBR", "ONE");
            asm.mov("T0", "T0");
        },
        consts,
        UCODE_ALPHA,
    )
}

pub fn iterated_programs() -> &'static HashMap<String, IteratedProgram> {
    static CACHE: OnceLock<HashMap<String, IteratedProgram>> = OnceLock::new();
    CACHE.get_or_init(|| {
        let mut all = Vec::new();
        for &name in SEPARABLE {
            all.push(separable(name, ""));
        }
        for &name in RANGED {
            for &flags in &["", "x", "u"] {
                all.push(separable(name, flags));
            }
        }
        all.push(pld(false));
        all.push(sds());
        all.push(sover_by_luma(false));
        all.push(sover_by_luma(true));
        all.push(dlc(false));
        all.push(dlc(true));
        for &name in &["hue", "sat", "col", "lum"] {
            all.push(hsl(name));
        }
        all.push(cli());
        all.into_iter().map(|p| (p.name.clone(), p)).collect()
    })
}
