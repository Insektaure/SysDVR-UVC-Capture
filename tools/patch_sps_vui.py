#!/usr/bin/env python3
"""Patch the Switch's hardcoded H.264 SPS to add VUI parameters.

Why: the grc:d SPS is High profile with NO VUI. Without
bitstream_restriction/max_num_reorder_frames=0, spec-respecting decoders
(FFmpeg in OBS, Windows DirectShow consumers) reserve a frame REORDER buffer
for B-frames that never come -> 1-2s of constant capture latency that only
`-flags low_delay` players avoid. Adding VUI with max_num_reorder_frames=0
and explicit 30fps timing makes every decoder open in low-delay mode.

The script parses the original SPS (verifying every field), re-emits it
bit-identically up to vui_parameters_present_flag, then appends the VUI.
It then re-parses its own output as a self-check.
"""

ORIG_SPS_NAL = bytes([0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B, 0x40, 0x28,
                      0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80])


class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0  # bit position

    def u(self, n: int) -> int:
        v = 0
        for _ in range(n):
            byte = self.data[self.pos // 8]
            bit = (byte >> (7 - self.pos % 8)) & 1
            v = (v << 1) | bit
            self.pos += 1
        return v

    def ue(self) -> int:
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
            assert zeros < 32, "malformed exp-golomb"
        return (1 << zeros) - 1 + (self.u(zeros) if zeros else 0)

    def se(self) -> int:
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


class BitWriter:
    def __init__(self):
        self.bits = []

    def u(self, v: int, n: int):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def ue(self, v: int):
        v += 1
        nbits = v.bit_length()
        self.u(0, nbits - 1)
        self.u(v, nbits)

    def copy_bits(self, data: bytes, start: int, end: int):
        for p in range(start, end):
            self.bits.append((data[p // 8] >> (7 - p % 8)) & 1)

    def rbsp_trailing(self):
        self.bits.append(1)
        while len(self.bits) % 8:
            self.bits.append(0)

    def tobytes(self) -> bytes:
        assert len(self.bits) % 8 == 0
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            b = 0
            for bit in self.bits[i:i + 8]:
                b = (b << 1) | bit
            out.append(b)
        return bytes(out)


def strip_emulation(rbsp: bytes) -> bytes:
    out = bytearray()
    zeros = 0
    for b in rbsp:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue  # emulation prevention byte
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def add_emulation(rbsp: bytes) -> bytes:
    out = bytearray()
    zeros = 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def parse_sps(rbsp: bytes, expect_vui: bool):
    """Parse an SPS RBSP. Returns (fields dict, bit position of
    vui_parameters_present_flag)."""
    r = BitReader(rbsp)
    f = {}
    f["profile_idc"] = r.u(8)
    f["constraint_flags"] = r.u(8)
    f["level_idc"] = r.u(8)
    f["sps_id"] = r.ue()
    assert f["profile_idc"] in (100, 110, 122, 244, 44, 83, 86, 118, 128), \
        f"unexpected profile {f['profile_idc']}"
    f["chroma_format_idc"] = r.ue()
    assert f["chroma_format_idc"] != 3
    f["bit_depth_luma_minus8"] = r.ue()
    f["bit_depth_chroma_minus8"] = r.ue()
    f["qpprime_y_zero_flag"] = r.u(1)
    f["seq_scaling_matrix_present"] = r.u(1)
    assert f["seq_scaling_matrix_present"] == 0, "scaling lists not handled"
    f["log2_max_frame_num_minus4"] = r.ue()
    f["pic_order_cnt_type"] = r.ue()
    if f["pic_order_cnt_type"] == 0:
        f["log2_max_poc_lsb_minus4"] = r.ue()
    elif f["pic_order_cnt_type"] == 1:
        f["delta_pic_order_always_zero"] = r.u(1)
        f["offset_for_non_ref_pic"] = r.se()
        f["offset_for_top_to_bottom"] = r.se()
        n = r.ue()
        f["offsets"] = [r.se() for _ in range(n)]
    f["max_num_ref_frames"] = r.ue()
    f["gaps_in_frame_num_allowed"] = r.u(1)
    f["pic_width_in_mbs_minus1"] = r.ue()
    f["pic_height_in_map_units_minus1"] = r.ue()
    f["frame_mbs_only_flag"] = r.u(1)
    if not f["frame_mbs_only_flag"]:
        f["mb_adaptive_frame_field"] = r.u(1)
    f["direct_8x8_inference_flag"] = r.u(1)
    f["frame_cropping_flag"] = r.u(1)
    if f["frame_cropping_flag"]:
        f["crop"] = [r.ue() for _ in range(4)]
    vui_flag_pos = r.pos
    f["vui_present"] = r.u(1)
    if expect_vui:
        assert f["vui_present"] == 1
        v = {}
        v["aspect_ratio_info"] = r.u(1); assert v["aspect_ratio_info"] == 0
        v["overscan_info"] = r.u(1); assert v["overscan_info"] == 0
        v["video_signal_type"] = r.u(1); assert v["video_signal_type"] == 0
        v["chroma_loc_info"] = r.u(1); assert v["chroma_loc_info"] == 0
        v["timing_info"] = r.u(1); assert v["timing_info"] == 1
        v["num_units_in_tick"] = r.u(32)
        v["time_scale"] = r.u(32)
        v["fixed_frame_rate"] = r.u(1)
        v["nal_hrd"] = r.u(1); assert v["nal_hrd"] == 0
        v["vcl_hrd"] = r.u(1); assert v["vcl_hrd"] == 0
        v["pic_struct_present"] = r.u(1); assert v["pic_struct_present"] == 0
        v["bitstream_restriction"] = r.u(1); assert v["bitstream_restriction"] == 1
        v["mv_over_pic_boundaries"] = r.u(1)
        v["max_bytes_per_pic_denom"] = r.ue()
        v["max_bits_per_mb_denom"] = r.ue()
        v["log2_max_mv_h"] = r.ue()
        v["log2_max_mv_v"] = r.ue()
        v["max_num_reorder_frames"] = r.ue()
        v["max_dec_frame_buffering"] = r.ue()
        f["vui"] = v
    return f, vui_flag_pos


def main():
    nal_header, orig_rbsp_ep = ORIG_SPS_NAL[0], ORIG_SPS_NAL[1:]
    assert nal_header == 0x67
    rbsp = strip_emulation(orig_rbsp_ep)

    fields, vui_flag_pos = parse_sps(rbsp, expect_vui=False)

    # Sanity-check the parser against known facts about this stream
    width = (fields["pic_width_in_mbs_minus1"] + 1) * 16
    height = (fields["pic_height_in_map_units_minus1"] + 1) * 16
    assert fields["vui_present"] == 0, "SPS already has VUI?!"
    print(f"Parsed original SPS: profile={fields['profile_idc']} "
          f"level={fields['level_idc']} {width}x{height} "
          f"ref_frames={fields['max_num_ref_frames']} "
          f"poc_type={fields['pic_order_cnt_type']} "
          f"crop={fields.get('crop')}")
    assert width == 1280 and height == 720, "parser failed: not 1280x720"

    # Re-emit: identical bits up to (excluding) vui_parameters_present_flag
    w = BitWriter()
    w.copy_bits(rbsp, 0, vui_flag_pos)

    # vui_parameters_present_flag = 1, then the VUI
    w.u(1, 1)
    w.u(0, 1)            # aspect_ratio_info_present_flag
    w.u(0, 1)            # overscan_info_present_flag
    w.u(0, 1)            # video_signal_type_present_flag
    w.u(0, 1)            # chroma_loc_info_present_flag
    w.u(1, 1)            # timing_info_present_flag
    w.u(1, 32)           # num_units_in_tick
    w.u(60, 32)          # time_scale  (fps = time_scale / (2*num_units) = 30)
    w.u(1, 1)            # fixed_frame_rate_flag
    w.u(0, 1)            # nal_hrd_parameters_present_flag
    w.u(0, 1)            # vcl_hrd_parameters_present_flag
    w.u(0, 1)            # pic_struct_present_flag
    w.u(1, 1)            # bitstream_restriction_flag
    w.u(1, 1)            #   motion_vectors_over_pic_boundaries_flag
    w.ue(0)              #   max_bytes_per_pic_denom (0 = unlimited)
    w.ue(0)              #   max_bits_per_mb_denom (0 = unlimited)
    w.ue(16)             #   log2_max_mv_length_horizontal
    w.ue(16)             #   log2_max_mv_length_vertical
    w.ue(0)              #   max_num_reorder_frames  <-- THE FIX
    w.ue(fields["max_num_ref_frames"])  # max_dec_frame_buffering
    w.rbsp_trailing()

    new_rbsp = w.tobytes()

    # Self-check: re-parse our own output, all original fields must match
    nf, _ = parse_sps(new_rbsp, expect_vui=True)
    for k, val in fields.items():
        if k == "vui_present":
            continue
        assert nf[k] == val, f"field {k} changed: {val} -> {nf[k]}"
    assert nf["vui"]["max_num_reorder_frames"] == 0
    assert nf["vui"]["time_scale"] == 60
    print(f"Self-check OK: VUI added, max_num_reorder_frames=0, "
          f"max_dec_frame_buffering={nf['vui']['max_dec_frame_buffering']}, "
          f"30fps timing")

    new_nal = bytes([nal_header]) + add_emulation(new_rbsp)
    full = bytes([0, 0, 0, 1]) + new_nal
    print(f"\nOriginal SPS: {len(ORIG_SPS_NAL) + 4} bytes -> "
          f"patched: {len(full)} bytes")
    print("\nconst u8 SPS[] = {")
    for i in range(0, len(full), 8):
        row = ", ".join(f"0x{b:02X}" for b in full[i:i + 8])
        print(f"    {row},")
    print("};")


if __name__ == "__main__":
    main()
