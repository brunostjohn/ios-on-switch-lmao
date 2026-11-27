// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;

pub const ENABLE_RGB: u8 = 1;
pub const ENABLE_RGBA: u8 = 3;

pub const OGL_ZERO: u32 = 0x4000;
pub const OGL_ONE: u32 = 0x4001;
pub const OGL_ONE_MINUS_SRC_A: u32 = 0x4303;

pub const E_GUARD: u16 = 0x068E;
pub const W601: [u16; 3] = [0x34CD, 0x38B8, 0x2F0A];
pub const W709: [u16; 3] = [0x32CD, 0x39B9, 0x2C9D];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AlphaMode {
    Fixed(u32, u32),
    Ucode,
}

pub const FF_OVER: AlphaMode = AlphaMode::Fixed(OGL_ONE, OGL_ONE_MINUS_SRC_A);
pub const FF_DST: AlphaMode = AlphaMode::Fixed(OGL_ZERO, OGL_ONE);
pub const FF_ADD: AlphaMode = AlphaMode::Fixed(OGL_ONE, OGL_ONE);
pub const UCODE_ALPHA: AlphaMode = AlphaMode::Ucode;

/// Exact IEEE-754 binary64 to binary16 conversion with round-to-nearest-even (matches Python `struct.pack('<e', x)`).
pub fn half_bits(x: f64) -> u16 {
    let bits = x.to_bits();
    let sign = ((bits >> 48) & 0x8000) as u16;
    let exp = ((bits >> 52) & 0x7FF) as i32;
    let frac = bits & ((1u64 << 52) - 1);

    if exp == 0x7FF {
        if frac == 0 {
            return sign | 0x7C00;
        }
        let payload = ((frac >> 42) as u16) | 0x0200;
        return sign | 0x7C00 | payload;
    }

    let unbiased = exp - 1023;
    if unbiased > 15 {
        return sign | 0x7C00;
    }
    if unbiased >= -14 {
        let h_exp = (unbiased + 15) as u16;
        let mant = frac >> 42;
        let rem = frac & ((1u64 << 42) - 1);
        let half = 1u64 << 41;
        let round_up = rem > half || (rem == half && (mant & 1) != 0);
        let combined = ((u32::from(h_exp) << 10) | (mant as u32)) + u32::from(round_up);
        return sign | (combined as u16);
    }
    if unbiased < -25 {
        return sign;
    }
    let full = (1u64 << 52) | frac;
    let shift = (-14 - unbiased) as u32 + 42;
    let mant = full >> shift;
    let mask = (1u64 << shift) - 1;
    let rem = full & mask;
    let half = 1u64 << (shift - 1);
    let round_up = rem > half || (rem == half && (mant & 1) != 0);
    sign | ((mant + u64::from(round_up)) as u16)
}

pub fn splat(bits: u16) -> [u16; 3] {
    [bits, bits, bits]
}

#[derive(Clone, Copy, Debug)]
pub struct Insn {
    pub test: u32,
    pub op: u32,
    pub k: u32,
    pub a: u32,
    pub b: u32,
    pub c: u32,
    pub d: u32,
    pub swz: u32,
    pub mask: u32,
    pub dest: u32,
    pub set_cc: u32,
}

impl Insn {
    pub fn encode(self) -> u32 {
        self.test
            | (self.op << 3)
            | (self.k << 6)
            | (self.a << 9)
            | (self.b << 12)
            | (self.c << 16)
            | (self.d << 19)
            | (self.swz << 23)
            | (self.mask << 26)
            | (self.dest << 28)
            | (self.set_cc << 31)
    }
}

fn test_cc(name: &str) -> u32 {
    match name {
        "F" => 0,
        "T" => 1,
        "EQ" => 2,
        "NE" => 3,
        "LT" => 4,
        "LE" => 5,
        "GT" => 6,
        "GE" => 7,
        _ => panic!("unknown test_cc {name}"),
    }
}

fn op_code(name: &str) -> u32 {
    match name {
        "MMADD" => 0,
        "MMSUB" => 1,
        "MIN" => 2,
        "MAX" => 3,
        "RCP" => 4,
        "ADD" => 5,
        "SUB" => 6,
        _ => panic!("unknown op {name}"),
    }
}

fn op_ac(name: &str) -> u32 {
    match name {
        "SRC" => 0,
        "DST" => 1,
        "SRC_A" => 2,
        "DST_A" => 3,
        "T0" => 4,
        "T1" => 5,
        "T2" => 6,
        "PBR" => 7,
        _ => panic!("unknown op_ac {name}"),
    }
}

fn op_bd(name: &str) -> u32 {
    match name {
        "ZERO" => 0,
        "ONE" => 1,
        "SRC" => 2,
        "SRC_A" => 3,
        "1-SRC_A" => 4,
        "DST" => 5,
        "DST_A" => 6,
        "1-DST_A" => 7,
        "T0" => 9,
        "T1" => 10,
        "T2" => 11,
        "PBR" => 12,
        "K" => 13,
        "ZERO_PRODUCT" => 14,
        _ => panic!("unknown op_bd {name}"),
    }
}

fn parse_dest(spec: &str) -> (u32, u32, u32, u32) {
    let mut parts = spec.split('.');
    let reg = match parts.next().expect("dest reg") {
        "T0" => 0,
        "T1" => 1,
        "T2" => 2,
        "PBR" => 3,
        other => panic!("unknown dest reg {other}"),
    };
    let mut swz = 0;
    let mut mask = 0;
    let mut set_cc = 0;
    for m in parts {
        match m {
            "CC" => set_cc = 1,
            "RGB" => swz = 0,
            "GBR" => swz = 1,
            "RRR" => swz = 2,
            "GGG" => swz = 3,
            "BBB" => swz = 4,
            "R_TO_A" => swz = 5,
            "R" => mask = 1,
            "G" => mask = 2,
            "B" => mask = 3,
            other => panic!("unknown dest modifier {other} in {spec}"),
        }
    }
    (reg, swz, mask, set_cc)
}

#[derive(Default)]
pub struct Asm {
    pub code: Vec<Insn>,
}

impl Asm {
    #[allow(clippy::too_many_arguments)]
    pub fn emit(&mut self, op: &str, dest: &str, a: &str, b: &str, c: &str, d: &str, cc: &str, k: u32) {
        let (reg, swz, mask, set_cc) = parse_dest(dest);
        self.code.push(Insn {
            test: test_cc(cc),
            op: op_code(op),
            k,
            a: op_ac(a),
            b: op_bd(b),
            c: op_ac(c),
            d: op_bd(d),
            swz,
            mask,
            dest: reg,
            set_cc,
        });
    }

    pub fn mul(&mut self, dest: &str, a: &str, b: &str) {
        self.emit("MMADD", dest, a, b, "SRC", "ZERO", "T", 0);
    }

    pub fn mul_k(&mut self, dest: &str, a: &str, b: &str, k: u32) {
        self.emit("MMADD", dest, a, b, "SRC", "ZERO", "T", k);
    }

    pub fn mul_cc(&mut self, dest: &str, a: &str, b: &str, cc: &str) {
        self.emit("MMADD", dest, a, b, "SRC", "ZERO", cc, 0);
    }

    pub fn madd(&mut self, dest: &str, a: &str, b: &str, c: &str) {
        self.emit("MMADD", dest, a, b, c, "ONE", "T", 0);
    }

    pub fn madd_k(&mut self, dest: &str, a: &str, b: &str, c: &str, k: u32) {
        self.emit("MMADD", dest, a, b, c, "ONE", "T", k);
    }

    pub fn madd_cc(&mut self, dest: &str, a: &str, b: &str, c: &str, cc: &str) {
        self.emit("MMADD", dest, a, b, c, "ONE", cc, 0);
    }

    pub fn mmadd(&mut self, dest: &str, a: &str, b: &str, c: &str, d: &str) {
        self.emit("MMADD", dest, a, b, c, d, "T", 0);
    }

    pub fn mmadd_cc(&mut self, dest: &str, a: &str, b: &str, c: &str, d: &str, cc: &str) {
        self.emit("MMADD", dest, a, b, c, d, cc, 0);
    }

    pub fn mmsub(&mut self, dest: &str, a: &str, b: &str, c: &str, d: &str) {
        self.emit("MMSUB", dest, a, b, c, d, "T", 0);
    }

    pub fn mmsub_cc(&mut self, dest: &str, a: &str, b: &str, c: &str, d: &str, cc: &str) {
        self.emit("MMSUB", dest, a, b, c, d, cc, 0);
    }

    pub fn min(&mut self, dest: &str, a: &str, b: &str) {
        self.emit("MIN", dest, a, b, "SRC", "ZERO", "T", 0);
    }

    pub fn min_cc(&mut self, dest: &str, a: &str, b: &str, cc: &str) {
        self.emit("MIN", dest, a, b, "SRC", "ZERO", cc, 0);
    }

    pub fn max(&mut self, dest: &str, a: &str, b: &str) {
        self.emit("MAX", dest, a, b, "SRC", "ZERO", "T", 0);
    }

    pub fn max_k(&mut self, dest: &str, a: &str, b: &str, k: u32) {
        self.emit("MAX", dest, a, b, "SRC", "ZERO", "T", k);
    }

    pub fn max_cc(&mut self, dest: &str, a: &str, b: &str, cc: &str) {
        self.emit("MAX", dest, a, b, "SRC", "ZERO", cc, 0);
    }

    pub fn max_cc_k(&mut self, dest: &str, a: &str, b: &str, cc: &str, k: u32) {
        self.emit("MAX", dest, a, b, "SRC", "ZERO", cc, k);
    }

    pub fn rcp(&mut self, dest: &str, a: &str) {
        self.emit("RCP", dest, a, "ZERO", "SRC", "ZERO", "T", 0);
    }

    pub fn rcp_cc(&mut self, dest: &str, a: &str, cc: &str) {
        self.emit("RCP", dest, a, "ZERO", "SRC", "ZERO", cc, 0);
    }

    pub fn mov(&mut self, dest: &str, b: &str) {
        self.emit("ADD", dest, "SRC", b, "SRC", "ZERO", "T", 0);
    }

    pub fn mov_k(&mut self, dest: &str, b: &str, k: u32) {
        self.emit("ADD", dest, "SRC", b, "SRC", "ZERO", "T", k);
    }

    pub fn mov_cc(&mut self, dest: &str, b: &str, cc: &str) {
        self.emit("ADD", dest, "SRC", b, "SRC", "ZERO", cc, 0);
    }

    pub fn add(&mut self, dest: &str, b: &str, d: &str) {
        self.emit("ADD", dest, "SRC", b, "SRC", d, "T", 0);
    }

    pub fn add_cc(&mut self, dest: &str, b: &str, d: &str, cc: &str) {
        self.emit("ADD", dest, "SRC", b, "SRC", d, cc, 0);
    }

    pub fn sub(&mut self, dest: &str, b: &str, d: &str) {
        self.emit("SUB", dest, "SRC", b, "SRC", d, "T", 0);
    }

    pub fn sub_cc(&mut self, dest: &str, b: &str, d: &str, cc: &str) {
        self.emit("SUB", dest, "SRC", b, "SRC", d, cc, 0);
    }
}

#[derive(Clone, Debug)]
pub struct IteratedProgram {
    pub name: String,
    pub words: Vec<u32>,
    pub enable: u8,
    pub constants: BTreeMap<u32, [u16; 3]>,
    pub alpha_source: u32,
    pub alpha_dest: u32,
    pub uniform_slot: u8,
    pub uniform_offset: u16,
    pub uniform_words: u8,
}

pub fn build_program(
    name: impl Into<String>,
    build: impl FnOnce(&mut Asm),
    consts: BTreeMap<u32, [u16; 3]>,
    alpha: AlphaMode,
) -> IteratedProgram {
    let mut asm = Asm::default();
    build(&mut asm);
    let (code, constants) = first_pass_on_constant_zero(asm.code, consts);
    let (enable, alpha_source, alpha_dest) = match alpha {
        AlphaMode::Fixed(src, dst) => (ENABLE_RGB, src, dst),
        AlphaMode::Ucode => (ENABLE_RGBA, OGL_ONE, OGL_ZERO),
    };
    IteratedProgram {
        name: name.into(),
        words: code.into_iter().map(Insn::encode).collect(),
        enable,
        constants,
        alpha_source,
        alpha_dest,
        uniform_slot: 0xFF,
        uniform_offset: 0,
        uniform_words: 0,
    }
}

fn first_pass_on_constant_zero(
    mut code: Vec<Insn>,
    consts: BTreeMap<u32, [u16; 3]>,
) -> (Vec<Insn>, BTreeMap<u32, [u16; 3]>) {
    let Some(&first) = code.first() else {
        return (code, consts);
    };
    // GM20B rejects a first pass reading any constant other than K0; swap K0 with first.k across all instructions.
    if (first.b != 13 && first.d != 13) || first.k == 0 {
        return (code, consts);
    }
    let k = first.k;
    let swap = |j: u32| {
        if j == 0 {
            k
        } else if j == k {
            0
        } else {
            j
        }
    };
    for insn in &mut code {
        insn.k = swap(insn.k);
    }
    let swapped = consts.into_iter().map(|(j, v)| (swap(j), v)).collect();
    (code, swapped)
}
