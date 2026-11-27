// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace switch_seed {

inline void appendOwnStateLines(std::vector<std::string> &lines)
{
    for (const char *out : {"115 | v ycbcr_vert | f ycbcr_converted_frag",
                            "125 | v ycbcr_vert | f ycbcr_srgb_frag",
                            "70 | v ycbcr_vert | f ycbcr_codes_frag",
                            "90 | v ycbcr_vert | f ycbcr_codes10_frag",
                            "115 | v ycbcr_vert | f ycbcr_packed_converted_frag",
                            "125 | v ycbcr_vert | f ycbcr_packed_srgb_frag",
                            "90 | v ycbcr_vert | f ycbcr_packed_codes10_frag",
                            "90 | v ycbcr_vert | f ycbcr_packed_plane_frag",
                            "55 | v ycbcr_vert | f ycbcr_load_plane_packed_r_frag",
                            "105 | v ycbcr_vert | f ycbcr_load_plane_packed_rg_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_converted_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_srgb_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_codes_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_luma_first_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_packed_converted_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_packed_srgb_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_packed_codes10_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_packed_luma_first_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_store_luma8_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_store_luma16_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_store_chroma8_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_store_chroma16_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_store_luma8_srgb_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_store_luma16_srgb_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_store_chroma8_srgb_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_store_chroma16_srgb_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_luma_packed_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_chroma_packed_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_luma_packed_srgb_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_chroma_packed_srgb_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_plane_packed_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_store_luma8_masked_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_store_luma16_masked_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_store_chroma8_masked_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_store_chroma16_masked_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_store_luma8_srgb_masked_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_store_luma16_srgb_masked_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_store_chroma8_srgb_masked_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_store_chroma16_srgb_masked_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_luma_packed_masked_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_chroma_packed_masked_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_luma_packed_srgb_masked_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_chroma_packed_srgb_masked_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_store_words_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_store_words_srgb_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_store_words_masked_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_store_words_srgb_masked_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_overlap_r8_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_overlap_r16_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_overlap_rg8_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_overlap_rg16_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_overlap_rgb10a2_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_overlap_r32ui_frag",
                            "103 | v ycbcr_store_vert | f ycbcr_overlap_rg32ui_frag",
                            "123 | v ycbcr_store_vert | f ycbcr_overlap_rgba32ui_frag",
                            "13 | v ycbcr_store_vert | f ycbcr_overlap_planes_frag",
                            "13 | v ycbcr_store_vert | f ycbcr_overlap_meet_frag",
                            "13 | v turn_vert | f turn_copy8_frag",
                            "23 | v turn_vert | f turn_copy16_frag",
                            "53 | v turn_vert | f turn_copy32_frag",
                            "103 | v turn_vert | f turn_copy64_frag",
                            "123 | v turn_vert | f turn_copy128_frag",
                            "13 | v turn_vert | f turn_masked8_frag",
                            "23 | v turn_vert | f turn_masked16_frag",
                            "53 | v turn_vert | f turn_masked32_frag",
                            "103 | v turn_vert | f turn_masked64_frag",
                            "123 | v turn_vert | f turn_masked128_frag",
                            "13 | v turn_vert | f turn_reach_frag",
                            "13 | v turn_vert | f turn_reach_reduce_frag",
                            "115 | v xr_vert | f xr_bgr10_frag",
                            "115 | v xr_vert | f xr_bgr10_srgb_frag",
                            "115 | v xr_vert | f xr_bgra10_frag",
                            "115 | v xr_vert | f xr_bgra10_srgb_frag",
                            "115 | v xr_vert | f xr_planes_frag",
                            "115 | v xr_vert | f xr_planes_srgb_frag",
                            "115 | v xr_vert | f xr_two_planes_frag",
                            "115 | v xr_vert | f xr_two_planes_srgb_frag",
                            "103 | v xr_vert | f xr_planes_merge_frag",
                            "113 | v xr_vert | f xr_copy_clamp_frag",
                            "70 | v resolve_vert | f resolve_rgba8_frag",
                            "70 | v resolve_vert | f resolve_rgba8_srgb_frag",
                            "80 | v resolve_vert | f resolve_bgra8_frag",
                            "80 | v resolve_vert | f resolve_bgra8_srgb_frag",
                            "70 | v resolve_vert | f resolve_rgba8_2x_frag",
                            "70 | v resolve_vert | f resolve_rgba8_4x_frag",
                            "80 | v resolve_vert | f resolve_bgra8_2x_frag",
                            "80 | v resolve_vert | f resolve_bgra8_4x_frag",
                            "70 | v resolve_vert | f resolve_rgba8_srgb_2x_frag",
                            "70 | v resolve_vert | f resolve_rgba8_srgb_4x_frag",
                            "80 | v resolve_vert | f resolve_bgra8_srgb_2x_frag",
                            "80 | v resolve_vert | f resolve_bgra8_srgb_4x_frag",
                            "92 | v resolve_vert | f resolve_rg11b10f_frag",
                            "115 | v resolve_vert | f resolve_rgba16f_frag",
                            "1 | v resolve_vert | f resolve_a8_frag",
                            "10 | v resolve_vert | f resolve_r8_frag",
                            "30 | v resolve_vert | f resolve_rg8_frag",
                            "20 | v resolve_vert | f resolve_r16_frag",
                            "60 | v resolve_vert | f resolve_rg16_frag",
                            "110 | v resolve_vert | f resolve_rgba16_frag",
                            "90 | v resolve_vert | f resolve_rgb10a2_frag",
                            "94 | v resolve_vert | f resolve_bgr10a2_frag",
                            "12 | v resolve_vert | f resolve_r8snorm_frag",
                            "32 | v resolve_vert | f resolve_rg8snorm_frag",
                            "72 | v resolve_vert | f resolve_rgba8snorm_frag",
                            "25 | v resolve_vert | f resolve_r16f_frag",
                            "65 | v resolve_vert | f resolve_rg16f_frag",
                            "55 | v resolve_vert | f resolve_r32f_frag",
                            "105 | v resolve_vert | f resolve_rg32f_frag",
                            "125 | v resolve_vert | f resolve_rgba32f_frag",
                            "40 | v resolve_vert | f resolve_b5g6r5_frag",
                            "43 | v resolve_vert | f resolve_bgr5a1_frag",
                            "1 | v mip_vert | f mip_a8_frag",
                            "10 | v mip_vert | f mip_r8_frag",
                            "12 | v mip_vert | f mip_r8snorm_frag",
                            "20 | v mip_vert | f mip_r16_frag",
                            "22 | v mip_vert | f mip_r16snorm_frag",
                            "25 | v mip_vert | f mip_r16f_frag",
                            "30 | v mip_vert | f mip_rg8_frag",
                            "32 | v mip_vert | f mip_rg8snorm_frag",
                            "40 | v mip_vert | f mip_b5g6r5_frag",
                            "43 | v mip_vert | f mip_bgr5a1_frag",
                            "55 | v mip_vert | f mip_r32f_frag",
                            "60 | v mip_vert | f mip_rg16_frag",
                            "62 | v mip_vert | f mip_rg16snorm_frag",
                            "65 | v mip_vert | f mip_rg16f_frag",
                            "70 | v mip_vert | f mip_rgba8_frag",
                            "72 | v mip_vert | f mip_rgba8snorm_frag",
                            "71 | v mip_vert | f mip_rgba8_srgb_frag",
                            "80 | v mip_vert | f mip_bgra8_frag",
                            "81 | v mip_vert | f mip_bgra8_srgb_frag",
                            "90 | v mip_vert | f mip_rgb10a2_frag",
                            "92 | v mip_vert | f mip_rg11b10f_frag",
                            "94 | v mip_vert | f mip_bgr10a2_frag",
                            "105 | v mip_vert | f mip_rg32f_frag",
                            "110 | v mip_vert | f mip_rgba16_frag",
                            "112 | v mip_vert | f mip_rgba16snorm_frag",
                            "115 | v mip_vert | f mip_rgba16f_frag",
                            "125 | v mip_vert | f mip_rgba32f_frag",
                            "1 | v mip_vert | f mip_3d_a8_frag",
                            "10 | v mip_vert | f mip_3d_r8_frag",
                            "12 | v mip_vert | f mip_3d_r8snorm_frag",
                            "20 | v mip_vert | f mip_3d_r16_frag",
                            "22 | v mip_vert | f mip_3d_r16snorm_frag",
                            "25 | v mip_vert | f mip_3d_r16f_frag",
                            "30 | v mip_vert | f mip_3d_rg8_frag",
                            "32 | v mip_vert | f mip_3d_rg8snorm_frag",
                            "40 | v mip_vert | f mip_3d_b5g6r5_frag",
                            "43 | v mip_vert | f mip_3d_bgr5a1_frag",
                            "55 | v mip_vert | f mip_3d_r32f_frag",
                            "60 | v mip_vert | f mip_3d_rg16_frag",
                            "62 | v mip_vert | f mip_3d_rg16snorm_frag",
                            "65 | v mip_vert | f mip_3d_rg16f_frag",
                            "70 | v mip_vert | f mip_3d_rgba8_frag",
                            "72 | v mip_vert | f mip_3d_rgba8snorm_frag",
                            "71 | v mip_vert | f mip_3d_rgba8_srgb_frag",
                            "80 | v mip_vert | f mip_3d_bgra8_frag",
                            "81 | v mip_vert | f mip_3d_bgra8_srgb_frag",
                            "90 | v mip_vert | f mip_3d_rgb10a2_frag",
                            "92 | v mip_vert | f mip_3d_rg11b10f_frag",
                            "94 | v mip_vert | f mip_3d_bgr10a2_frag",
                            "105 | v mip_vert | f mip_3d_rg32f_frag",
                            "110 | v mip_vert | f mip_3d_rgba16_frag",
                            "112 | v mip_vert | f mip_3d_rgba16snorm_frag",
                            "115 | v mip_vert | f mip_3d_rgba16f_frag",
                            "125 | v mip_vert | f mip_3d_rgba32f_frag"}) {
        std::string o = out;
        const size_t bar = o.find(' ');
        lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=" + o.substr(0, bar) + ":15:0:1:0:0:1:0:0" +
                        o.substr(bar));
    }
    for (const char *out : {"125 | v fmt_vert | f fmt_load_frag",
                            "125 | v fmt_vert | f fmt_load_rgb9e5_frag",
                            "23 | v fmt_vert | f fmt_store_abgr4_frag",
                            "23 | v fmt_vert | f fmt_store_a1bgr5_frag",
                            "23 | v fmt_vert | f fmt_store_bgra4_frag",
                            "53 | v fmt_vert | f fmt_store_rgb9e5_frag",
                            "23 | v fmt_vert | f fmt_mip_abgr4_frag",
                            "23 | v fmt_vert | f fmt_mip_a1bgr5_frag",
                            "53 | v fmt_vert | f fmt_mip_rgb9e5_frag",
                            "13 | v fmt_vert | f fmt_mip_srgb8_frag",
                            "53 | v fmt_vert | f fmt_mip_bgr10_xr_frag",
                            "53 | v fmt_vert | f fmt_mip_bgr10_xr_srgb_frag",
                            "113 | v fmt_vert | f fmt_mip_bgra10_xr_frag",
                            "113 | v fmt_vert | f fmt_mip_bgra10_xr_srgb_frag",
                            "53 | v fmt_vert | f fmt_mip_3d_bgr10_xr_frag",
                            "53 | v fmt_vert | f fmt_mip_3d_bgr10_xr_srgb_frag",
                            "113 | v fmt_vert | f fmt_mip_3d_bgra10_xr_frag",
                            "113 | v fmt_vert | f fmt_mip_3d_bgra10_xr_srgb_frag",
                            "13 | v fmt_vert | f fmt_mip_3d_srgb8_frag",
                            "23 | v fmt_vert | f fmt_mip_3d_abgr4_frag",
                            "23 | v fmt_vert | f fmt_mip_3d_a1bgr5_frag",
                            "53 | v fmt_vert | f fmt_mip_3d_rgb9e5_frag"}) {
        std::string o = out;
        const size_t bar = o.find(' ');
        lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=" + o.substr(0, bar) + ":15:0:1:0:0:1:0:0" +
                        o.substr(bar));
    }
    for (const char *out : {"115 | v xr_target_vert | f xr_target_load_bgr10_frag",
                            "115 | v xr_target_vert | f xr_target_load_bgra10_frag",
                            "115 | v xr_target_vert | f xr_target_load_planes_frag",
                            "115 | v xr_target_vert | f xr_target_load_two_planes_frag",
                            "115 | v xr_target_vert | f xr_target_load_bgr10_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_load_bgra10_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_load_planes_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_load_two_planes_srgb_frag",
                            "53 | v xr_target_vert | f xr_target_store_bgr10_frag",
                            "113 | v xr_target_vert | f xr_target_store_bgra10_frag",
                            "103 | v xr_target_vert | f xr_target_store_planes_frag",
                            "53 | v xr_target_vert | f xr_target_store_bgr10_srgb_frag",
                            "113 | v xr_target_vert | f xr_target_store_bgra10_srgb_frag",
                            "103 | v xr_target_vert | f xr_target_store_planes_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_resolve_bgr10_frag",
                            "115 | v xr_target_vert | f xr_target_resolve_bgra10_frag",
                            "115 | v xr_target_vert | f xr_target_resolve_bgr10_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_resolve_bgra10_srgb_frag"}) {
        std::string o = out;
        const size_t bar = o.find(' ');
        lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=" + o.substr(0, bar) + ":15:0:1:0:0:1:0:0" +
                        o.substr(bar));
    }
    for (const char *out :
         {"53 | v decode_vert | f decode_pvrtc_frag", "53 | v decode_vert | f decode_astc_unorm8_frag",
          "113 | v decode_vert | f decode_astc_unorm16_frag", "113 | v decode_vert | f decode_astc_half_frag"}) {
        std::string o = out;
        const size_t bar = o.find(' ');
        lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=" + o.substr(0, bar) + ":15:0:1:0:0:1:0:0" +
                        o.substr(bar));
    }
    for (const char *depth :
         {"250/0 | v resolve_vert | f resolve_depth16_", "252/0 | v resolve_vert | f resolve_depth32f_",
          "260/260 | v resolve_vert | f resolve_depth32fs8_"})
        for (const char *filter : {"sample0_frag", "min_frag", "max_frag"})
            lines.push_back(std::string("@O R ok=1 label=raw.own samples=1 topo=0 ds=") + depth + filter);
    for (const char *depth :
         {"250/0 | v turn_vert | f turn_depth16_frag", "252/0 | v turn_vert | f turn_depth32f_frag",
          "260/260 | v turn_vert | f turn_depth32fs8_frag", "0/253 | v stencil_bits_vert | f stencil_bits_s8_frag",
          "260/260 | v stencil_bits_vert | f stencil_bits_d32fs8_frag",
          "0/253 | v stencil_bits_vert | f stencil_bits_ms_s8_frag",
          "260/260 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_frag",
          "260/260 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_min_frag",
          "260/260 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_max_frag"})
        lines.push_back(std::string("@O R ok=1 label=raw.own samples=1 topo=0 ds=") + depth);
    for (const char *samples : {"2", "4"})
        for (const char *depth :
             {"250/0 | v turn_vert | f turn_depth16_ms", "260/260 | v turn_vert | f turn_depth32fs8_ms"})
            lines.push_back(std::string("@O R ok=1 label=raw.own samples=") + samples + " topo=0 ds=" + depth +
                            samples + "_frag");
    for (const char *samples : {"2", "4"})
        for (const char *out : {"125 | v fmt_vert | f fmt_load_ms", "125 | v fmt_vert | f fmt_load_rgb9e5_ms",
                                "23 | v fmt_vert | f fmt_store_abgr4_ms", "23 | v fmt_vert | f fmt_store_bgra4_ms",
                                "23 | v fmt_vert | f fmt_store_a1bgr5_ms", "53 | v fmt_vert | f fmt_store_rgb9e5_ms",
                                "115 | v xr_target_vert | f xr_target_load_bgr10_ms",
                                "115 | v xr_target_vert | f xr_target_load_bgra10_ms",
                                "115 | v xr_target_vert | f xr_target_load_bgr10_srgb_ms",
                                "115 | v xr_target_vert | f xr_target_load_bgra10_srgb_ms",
                                "53 | v xr_target_vert | f xr_target_store_bgr10_ms",
                                "113 | v xr_target_vert | f xr_target_store_bgra10_ms",
                                "53 | v xr_target_vert | f xr_target_store_bgr10_srgb_ms",
                                "113 | v xr_target_vert | f xr_target_store_bgra10_srgb_ms"}) {
            const std::string o = out;
            const size_t bar = o.find(' ');
            lines.push_back(std::string("@O R ok=1 label=raw.own samples=") + samples + " topo=0 ds=0/0 c0=" +
                            o.substr(0, bar) + ":15:0:1:0:0:1:0:0" + o.substr(bar) + samples + "_frag");
        }
    lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=125:15:0:1:0:0:1:0:0 | v fmt_vert | f "
                    "fmt_resolve_rgb9e5_frag");
    for (const char *out : {"c0=115:15:0:1:0:0:1:0:0 | v two_plane_cover | f two_plane_load",
                            "c0=10:15:0:1:0:0:1:0:0 c1=30:15:0:1:0:0:1:0:0 | v two_plane_cover | f two_plane_store",
                            "c0=53:15:0:1:0:0:1:0:0 c1=13:15:0:1:0:0:1:0:0 | v xr_vert | f xr_planes_split_frag",
                            "c0=53:15:0:1:0:0:1:0:0 c1=13:15:0:1:0:0:1:0:0 | v xr_target_vert | f "
                            "xr_target_store_two_planes_frag",
                            "c0=53:15:0:1:0:0:1:0:0 c1=13:15:0:1:0:0:1:0:0 | v xr_target_vert | f "
                            "xr_target_store_two_planes_srgb_frag",
                            "c0=13:15:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_s8_colour_frag",
                            "c0=103:15:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_d32fs8_colour_frag",
                            "c0=13:15:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_ms_s8_colour_frag",
                            "c0=103:4:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_colour_frag",
                            "c0=103:4:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_min_colour_frag",
                            "c0=103:4:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_max_colour_frag"})
        lines.push_back(std::string("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 ") + out);
}

}  // namespace switch_seed
