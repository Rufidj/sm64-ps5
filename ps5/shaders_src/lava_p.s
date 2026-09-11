// Lava: the texture carried along by a slow flow, hot spots drifting across
// the surface, the bright cracks of the texture glowing orange and the dark
// crust dimming between them.
//
// Inputs: attr0.xy texture coordinates, attr0.z time in seconds, attr1 colour.
// The container must declare VGPRS 4 (build_shaders.sh does), since this
// reaches past v10.
.text
	s_inst_prefetch 0x1
	s_mov_b32 m0, s12
	s_mov_b64 vcc, exec
	s_wqm_b64 exec, exec
	v_interp_p1_f32_e32 v6, v0, attr0.x
	v_interp_p1_f32_e32 v7, v0, attr0.y
	v_interp_p1_f32_e32 v2, v0, attr0.z
	v_interp_p1_f32_e32 v10, v0, attr1.x
	v_interp_p1_f32_e32 v9, v0, attr1.y
	v_interp_p1_f32_e32 v8, v0, attr1.w
	v_interp_p2_f32_e32 v6, v1, attr0.x
	v_interp_p2_f32_e32 v7, v1, attr0.y
	v_interp_p2_f32_e32 v2, v1, attr0.z
	v_interp_p1_f32_e32 v0, v0, attr1.z
	v_interp_p2_f32_e32 v10, v1, attr1.x
	v_interp_p2_f32_e32 v9, v1, attr1.y
	v_interp_p2_f32_e32 v8, v1, attr1.w
	v_interp_p2_f32_e32 v0, v1, attr1.z

	// v6, v7 = u, v   v2 = t   v10, v9, v0, v8 = colour r, g, b, a
	// flow: u += 0.05 sin(1.5 v + 0.4 t);  v += 0.05 cos(1.3 u + 0.35 t)
	v_mul_f32_e32 v3, 1.5, v7
	v_mul_f32_e32 v4, 0.4, v2
	v_add_f32_e32 v3, v3, v4
	v_sin_f32_e32 v3, v3
	v_mul_f32_e32 v3, 0.05, v3
	v_mul_f32_e32 v4, 1.3, v6
	v_mul_f32_e32 v5, 0.35, v2
	v_add_f32_e32 v4, v4, v5
	v_cos_f32_e32 v4, v4
	v_mul_f32_e32 v4, 0.05, v4
	v_add_f32_e32 v6, v6, v3
	v_add_f32_e32 v7, v7, v4

	// v11 = heat = 0.5 + 0.5 sin(2.1 u + 1.7 v + 0.9 t) sin(-1.4 u + 2.6 v + 0.7 t)
	v_mul_f32_e32 v3, 2.1, v6
	v_mul_f32_e32 v4, 1.7, v7
	v_add_f32_e32 v3, v3, v4
	v_mul_f32_e32 v4, 0.9, v2
	v_add_f32_e32 v3, v3, v4
	v_sin_f32_e32 v3, v3
	v_mul_f32_e32 v4, -1.4, v6
	v_mul_f32_e32 v5, 2.6, v7
	v_add_f32_e32 v4, v4, v5
	v_mul_f32_e32 v5, 0.7, v2
	v_add_f32_e32 v4, v4, v5
	v_sin_f32_e32 v4, v4
	v_mul_f32_e32 v11, v3, v4
	v_mul_f32_e32 v11, 0.5, v11
	v_add_f32_e32 v11, 0.5, v11

	// v[2:5] = texel rgba
	image_sample v[2:5], v[6:7], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)

	// texel times colour
	v_mul_f32_e32 v10, v2, v10
	v_mul_f32_e32 v9, v9, v3
	v_mul_f32_e32 v8, v8, v5
	v_mul_f32_e32 v0, v0, v4

	// v12 = luminance, which is high on the texture's molten cracks
	v_mul_f32_e32 v12, 0.3, v10
	v_mul_f32_e32 v13, 0.59, v9
	v_add_f32_e32 v12, v12, v13
	v_mul_f32_e32 v13, 0.11, v0
	v_add_f32_e32 v12, v12, v13

	// v13 = glow = 0.7 luminance^2 (0.6 + 0.8 heat)
	v_mul_f32_e32 v13, v12, v12
	v_mul_f32_e32 v14, 0.8, v11
	v_add_f32_e32 v14, 0.6, v14
	v_mul_f32_e32 v13, v13, v14
	v_mul_f32_e32 v13, 0.7, v13

	// the surface itself dims and brightens with the heat: 0.75 + 0.45 heat
	v_mul_f32_e32 v14, 0.45, v11
	v_add_f32_e32 v14, 0.75, v14
	v_mul_f32_e32 v10, v10, v14
	v_mul_f32_e32 v9, v9, v14
	v_mul_f32_e32 v0, v0, v14

	// glow added in orange (1, 0.45, 0.1)
	v_add_f32_e32 v10, v10, v13
	v_mul_f32_e32 v15, 0.45, v13
	v_add_f32_e32 v9, v9, v15
	v_mul_f32_e32 v15, 0.1, v13
	v_add_f32_e32 v0, v0, v15
	v_cvt_pkrtz_f16_f32_e32 v1, v10, v9
	v_cvt_pkrtz_f16_f32_e32 v0, v0, v8
	s_mov_b64 exec, vcc
	exp mrt0 v1, v1, v0, v0 done compr vm
	s_endpgm
