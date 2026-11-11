//! The storage form of a `BGR5A1Unorm` attachment (`A1R5G5B5_UNORM_PACK16`), to which a device
//! without its storage access (NVK) gives the view `R16_UINT` (`StorageView::uint_view`): the
//! function reads and writes the texel's bits. A read is each channel's exact value, `c / (2^b -
//! 1)` correctly rounded, as Metal reads one texel of a packed format (`metal::texel_format`,
//! `PackedTexels::Rgb5A1`); a write packs each channel's code as Metal's write of the output
//! rounds it (`color_write::round` by the attachment's rules, else the nearest code), and takes
//! the channels it keeps from the texel read first, inside the interlock.

use super::{image_and_texel, StorageColor};
use crate::lower::arith::color_write;
use crate::lower::arith::ops::fsplat;
use crate::lower::arith::{FloatFmt, Form, Held};
use crate::lower::emit::EmitCx;
use crate::lower::fragment::rop_blend::exact_quotient;
use crate::lower::sync::memory_model;
use crate::pipeline::lower_inputs::ChannelWrite;
use crate::pipeline::sym::PushField;
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::BResult;
use crate::spirv::ids::Value as SValue;
use crate::spirv::ir::{BinOp, ConvOp, GlslOp, Type as SType};
use crate::vk;

pub(super) const FORMAT: vk::Format = vk::Format::A1R5G5B5_UNORM_PACK16;

/// Each channel's bit offset in the texel and its largest code, red to alpha.
const FIELDS: [(u32, u32); 4] = [(10, 31), (5, 31), (0, 31), (15, 1)];

/// How a write rounds each channel where the pipeline names no rule: to the nearest code.
const NEAREST: [Option<ChannelWrite>; 4] = [
    Some(ChannelWrite::Unorm { bits: 5 }),
    Some(ChannelWrite::Unorm { bits: 5 }),
    Some(ChannelWrite::Unorm { bits: 5 }),
    Some(ChannelWrite::Unorm { bits: 1 }),
];

fn u32_const(fb: &mut FunctionBuilder<'_>, v: u32) -> BResult<SValue> {
    let c = fb.u32_const(v)?;
    fb.module().const_value(c)
}

fn vec4(fb: &mut FunctionBuilder<'_>, elem: SType) -> BResult<crate::spirv::ids::TypeId> {
    let e = fb.module().ty(elem)?;
    fb.module().ty(SType::Vector { elem: e, len: 4 })
}

fn u32_type() -> SType {
    SType::Int {
        width: 32,
        signed: false,
    }
}

/// The `vec4` a fetch reads from texel `word` (a `u32` of the view): each channel's exact value.
pub(super) fn decode(fb: &mut FunctionBuilder<'_>, word: SValue) -> BResult<SValue> {
    let mut codes = Vec::with_capacity(4);
    for (at, top) in FIELDS {
        let shift = u32_const(fb, at)?;
        let mask = u32_const(fb, top)?;
        let field = fb.binary(BinOp::ShiftRightLogical, word, shift)?;
        codes.push(fb.binary(BinOp::BitwiseAnd, field, mask)?);
    }
    let uvec4 = vec4(fb, u32_type())?;
    let codes = fb.composite_construct(uvec4, &codes)?;
    let fvec4 = vec4(fb, SType::Float { width: 32 })?;
    let codes = fb.convert(ConvOp::ConvertUToF, codes, fvec4)?;
    exact_quotient(fb, codes, FIELDS.map(|(_, top)| top))
}

/// The `uvec4` texel (its first lane the view's `u32`) a write of output `part` (four `float` or
/// `half` lanes of `form`) to attachment `color` stores.
pub(super) fn texel(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    sc: &StorageColor,
    form: Form,
    part: SValue,
) -> BResult<SValue> {
    if form.lanes != 4 || form.float_fmt().is_none() {
        return Err(cx.fail("a packed storage write of another output than four float lanes"));
    }
    let f4 = Form {
        held: Held::Float(FloatFmt::F32),
        lanes: 4,
    };
    let fvec4 = vec4(fb, SType::Float { width: 32 })?;
    let x = match form.float_fmt().map(FloatFmt::container) {
        Some(32) => part,
        _ => fb.convert(ConvOp::FConvert, part, fvec4)?,
    };
    // A blend the shader computes stores as the model's ROP does, to the nearest code
    // (`rop_blend::Assumption::UnormRounding`); an unblended output as Metal writes it.
    let recorded = cx.plan.families.stage_io.interface.color_writes.get(&color);
    let rules = match (sc.view.rop, recorded) {
        (false, Some(rules)) => *rules,
        _ => NEAREST,
    };
    let rounded = color_write::round(fb, f4, x, &rules, None)?;
    let mut word = None;
    for (lane, (at, top)) in FIELDS.into_iter().enumerate() {
        let one = Form { lanes: 1, ..f4 };
        let v = fb.composite_extract(rounded, &[lane as u32])?;
        let m = fsplat(fb, one, f64::from(top))?;
        let scaled = fb.binary_no_contraction(BinOp::FMul, v, m)?;
        let code = fb.glsl(GlslOp::RoundEven, &[scaled])?;
        let u = fb.module().ty(u32_type())?;
        let code = fb.convert(ConvOp::ConvertFToU, code, u)?;
        let shift = u32_const(fb, at)?;
        let field = fb.binary(BinOp::ShiftLeftLogical, code, shift)?;
        word = Some(match word {
            Some(w) => fb.binary(BinOp::BitwiseOr, w, field)?,
            None => field,
        });
    }
    let Some(mut word) = word else {
        return Err(cx.fail("a packed storage write without channels"));
    };
    if sc.view.merges() {
        let keep = kept_bits(cx, fb, color, sc)?;
        let (image, at, ops) = image_and_texel(cx, fb, color, sc.view.samples)?;
        let mem = memory_model::coherence(cx.plan).texel(fb)?;
        let old = fb.read(image, at, &ops, mem)?;
        let old = fb.composite_extract(old, &[0])?;
        let old = fb.binary(BinOp::BitwiseAnd, old, keep)?;
        let new = fb.binary(BinOp::BitwiseAnd, word, keep)?;
        let new = fb.binary(BinOp::BitwiseXor, word, new)?;
        word = fb.binary(BinOp::BitwiseOr, old, new)?;
    }
    let uvec4 = vec4(fb, u32_type())?;
    let zero = u32_const(fb, 0)?;
    fb.composite_construct(uvec4, &[word, zero, zero, zero])
}

/// The texel bits of the channels a write keeps: the draw's (`PushField::StorageKeptChannels`, bit
/// `c` of the color's four keeps channel `c`) where the format takes them at draw, else the
/// view's.
fn kept_bits(
    cx: &mut EmitCx<'_>,
    fb: &mut FunctionBuilder<'_>,
    color: u32,
    sc: &StorageColor,
) -> BResult<SValue> {
    let channels = [
        vk::ColorComponentFlags::R,
        vk::ColorComponentFlags::G,
        vk::ColorComponentFlags::B,
        vk::ColorComponentFlags::A,
    ];
    if !sc.view.kept_at_draw {
        let mask = channels
            .iter()
            .zip(FIELDS)
            .filter(|(c, _)| sc.view.kept.contains(**c))
            .fold(0u32, |m, (_, (at, top))| m | top << at);
        return u32_const(fb, mask);
    }
    let word = cx.push_field(fb, PushField::StorageKeptChannels)?;
    let shift = u32_const(fb, 4 * color)?;
    let draw = fb.binary(BinOp::ShiftRightLogical, word, shift)?;
    let zero = u32_const(fb, 0)?;
    let mut mask = zero;
    for (lane, (at, top)) in FIELDS.into_iter().enumerate() {
        let bit = u32_const(fb, 1 << lane)?;
        let set = fb.binary(BinOp::BitwiseAnd, draw, bit)?;
        let set = fb.binary(BinOp::INotEqual, set, zero)?;
        let field = u32_const(fb, top << at)?;
        let field = fb.select(set, field, zero)?;
        mask = fb.binary(BinOp::BitwiseOr, mask, field)?;
    }
    Ok(mask)
}
