// Shadow map: how far along the sun's light each point of what casts a shadow
// lies, written as colour because the target is 8 bits a channel and is read
// back as a texture.
//
// The depth d, from 0 to 1, is split across two channels: green is the
// fraction left of d * 255, red is d with that fraction taken off - a whole
// number of 255ths, which 8 bits hold exactly. Reading back, d = red + green /
// 255, good to 1/65025 of the range.
//
// Alpha is the texel's times the vertex's: for a cut-out the alpha test uses
// it to leave the holes out of the shadow; for anything else the test is off.
//
// Inputs: attr0.xy texture coordinates, attr0.z depth, attr1.w alpha.
// The container declares VGPRS 4, as it uses v11-v13.
.text
	s_inst_prefetch 0x1
	s_mov_b32 m0, s12
	s_mov_b64 vcc, exec
	s_wqm_b64 exec, exec
	v_interp_p1_f32_e32 v6, v0, attr0.x
	v_interp_p1_f32_e32 v7, v0, attr0.y
	v_interp_p1_f32_e32 v8, v0, attr0.z
	v_interp_p1_f32_e32 v9, v0, attr1.w
	v_interp_p2_f32_e32 v6, v1, attr0.x
	v_interp_p2_f32_e32 v7, v1, attr0.y
	v_interp_p2_f32_e32 v8, v1, attr0.z
	v_interp_p2_f32_e32 v9, v1, attr1.w

	// v[2:5] = texel rgba
	image_sample v[2:5], v[6:7], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)

	// alpha
	v_mul_f32_e32 v5, v5, v9
	// v11 = green = fract(d * 255), v12 = red = d - green / 255
	v_mul_f32_e32 v11, 255.0, v8
	v_fract_f32_e32 v11, v11
	v_mul_f32_e32 v12, -0.003921569, v11
	v_add_f32_e32 v12, v8, v12
	// v13 = blue, unused
	v_mul_f32_e32 v13, 0.0, v8

	v_cvt_pkrtz_f16_f32_e32 v1, v12, v11
	v_cvt_pkrtz_f16_f32_e32 v0, v13, v5
	s_mov_b64 exec, vcc
	exp mrt0 v1, v1, v0, v0 done compr vm
	s_endpgm
