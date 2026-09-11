// Shadow over a surface: black, as opaque as the share of shadow map texels
// around this point that hold something nearer the sun than it is.
//
// The shadow map holds two cascades side by side, each 2048 texels square: on
// the left a near one around the camera, on the right a far one eight times as
// large, as wide and as deep, about the same centre. The vertices carry the
// point in the near cascade; its place in the far one follows as
// 0.5 + (near - 0.5) / 8, for each of u, v and depth.
//
// In each cascade the 2x2 texels around the point are read without filtering,
// since the depth is packed into two channels (see shadow_depth_p.s) and a
// blend of packed values means nothing. Each unpacks to d = red + green / 255
// and counts as in the way when the point lies beyond it by more than a small
// bias, which keeps a surface from shadowing itself; the far cascade's texels
// are coarser, and its bias larger in its own depth units. The near cascade's
// answer is used well inside it and the far's outside, fading from one to the
// other over the near cascade's last stretch.
//
// Inputs: attr0.xy the point in the near cascade (0..1), attr0.z its depth
// there, attr1.w how dark a full shadow is.
// The container declares VGPRS 4, as it keeps state in v11-v15 across samples.
.text
	s_inst_prefetch 0x1
	s_mov_b32 m0, s12
	s_mov_b64 vcc, exec
	s_wqm_b64 exec, exec
	v_interp_p1_f32_e32 v14, v0, attr0.x
	v_interp_p1_f32_e32 v11, v0, attr0.y
	v_interp_p1_f32_e32 v8, v0, attr0.z
	v_interp_p1_f32_e32 v9, v0, attr1.w
	v_interp_p2_f32_e32 v14, v1, attr0.x
	v_interp_p2_f32_e32 v11, v1, attr0.y
	v_interp_p2_f32_e32 v8, v1, attr0.z
	v_interp_p2_f32_e32 v9, v1, attr1.w

	// v14, v11 = u, v in the near cascade
	// v0 = depth in the far cascade, no deeper than the map, less its bias
	v_add_f32_e32 v0, -0.5, v8
	v_mul_f32_e32 v0, 0.125, v0
	v_add_f32_e32 v0, 0.5, v0
	v_min_f32_e32 v0, 0.999, v0
	v_add_f32_e32 v0, -0.0005, v0
	// v8 = depth in the near cascade, less its bias
	v_add_f32_e32 v8, -0.001, v8
	// v15, v10 = texels in the way in the near and the far cascade
	v_mul_f32_e32 v15, 0.0, v8
	v_mul_f32_e32 v10, 0.0, v8

	// ---- near cascade: u = v14 / 2, v = v11, half a texel either way ----
	v_mul_f32_e32 v12, 0.5, v14
	v_add_f32_e32 v12, -0.000122, v12
	v_add_f32_e32 v13, -0.000244, v11
	image_sample v[2:5], v[12:13], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)
	v_mul_f32_e32 v3, 0.003921569, v3
	v_add_f32_e32 v2, v2, v3
	v_mul_f32_e32 v2, -1.0, v2
	v_add_f32_e32 v2, v8, v2
	v_mul_f32_e32 v2, 5000.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_add_f32_e32 v15, v15, v2

	v_mul_f32_e32 v12, 0.5, v14
	v_add_f32_e32 v12, 0.000122, v12
	v_add_f32_e32 v13, -0.000244, v11
	image_sample v[2:5], v[12:13], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)
	v_mul_f32_e32 v3, 0.003921569, v3
	v_add_f32_e32 v2, v2, v3
	v_mul_f32_e32 v2, -1.0, v2
	v_add_f32_e32 v2, v8, v2
	v_mul_f32_e32 v2, 5000.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_add_f32_e32 v15, v15, v2

	v_mul_f32_e32 v12, 0.5, v14
	v_add_f32_e32 v12, -0.000122, v12
	v_add_f32_e32 v13, 0.000244, v11
	image_sample v[2:5], v[12:13], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)
	v_mul_f32_e32 v3, 0.003921569, v3
	v_add_f32_e32 v2, v2, v3
	v_mul_f32_e32 v2, -1.0, v2
	v_add_f32_e32 v2, v8, v2
	v_mul_f32_e32 v2, 5000.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_add_f32_e32 v15, v15, v2

	v_mul_f32_e32 v12, 0.5, v14
	v_add_f32_e32 v12, 0.000122, v12
	v_add_f32_e32 v13, 0.000244, v11
	image_sample v[2:5], v[12:13], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)
	v_mul_f32_e32 v3, 0.003921569, v3
	v_add_f32_e32 v2, v2, v3
	v_mul_f32_e32 v2, -1.0, v2
	v_add_f32_e32 v2, v8, v2
	v_mul_f32_e32 v2, 5000.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_add_f32_e32 v15, v15, v2

	// ---- far cascade: u = 0.75 + (v14 - 0.5) / 16, v = 0.5 + (v11 - 0.5) / 8 ----
	v_add_f32_e32 v12, -0.5, v14
	v_mul_f32_e32 v12, 0.0625, v12
	v_add_f32_e32 v12, 0.749878, v12
	v_add_f32_e32 v13, -0.5, v11
	v_mul_f32_e32 v13, 0.125, v13
	v_add_f32_e32 v13, 0.499756, v13
	image_sample v[2:5], v[12:13], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)
	v_mul_f32_e32 v3, 0.003921569, v3
	v_add_f32_e32 v2, v2, v3
	v_mul_f32_e32 v2, -1.0, v2
	v_add_f32_e32 v2, v0, v2
	v_mul_f32_e32 v2, 5000.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_add_f32_e32 v10, v10, v2

	v_add_f32_e32 v12, -0.5, v14
	v_mul_f32_e32 v12, 0.0625, v12
	v_add_f32_e32 v12, 0.750122, v12
	v_add_f32_e32 v13, -0.5, v11
	v_mul_f32_e32 v13, 0.125, v13
	v_add_f32_e32 v13, 0.499756, v13
	image_sample v[2:5], v[12:13], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)
	v_mul_f32_e32 v3, 0.003921569, v3
	v_add_f32_e32 v2, v2, v3
	v_mul_f32_e32 v2, -1.0, v2
	v_add_f32_e32 v2, v0, v2
	v_mul_f32_e32 v2, 5000.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_add_f32_e32 v10, v10, v2

	v_add_f32_e32 v12, -0.5, v14
	v_mul_f32_e32 v12, 0.0625, v12
	v_add_f32_e32 v12, 0.749878, v12
	v_add_f32_e32 v13, -0.5, v11
	v_mul_f32_e32 v13, 0.125, v13
	v_add_f32_e32 v13, 0.500244, v13
	image_sample v[2:5], v[12:13], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)
	v_mul_f32_e32 v3, 0.003921569, v3
	v_add_f32_e32 v2, v2, v3
	v_mul_f32_e32 v2, -1.0, v2
	v_add_f32_e32 v2, v0, v2
	v_mul_f32_e32 v2, 5000.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_add_f32_e32 v10, v10, v2

	v_add_f32_e32 v12, -0.5, v14
	v_mul_f32_e32 v12, 0.0625, v12
	v_add_f32_e32 v12, 0.750122, v12
	v_add_f32_e32 v13, -0.5, v11
	v_mul_f32_e32 v13, 0.125, v13
	v_add_f32_e32 v13, 0.500244, v13
	image_sample v[2:5], v[12:13], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)
	v_mul_f32_e32 v3, 0.003921569, v3
	v_add_f32_e32 v2, v2, v3
	v_mul_f32_e32 v2, -1.0, v2
	v_add_f32_e32 v2, v0, v2
	v_mul_f32_e32 v2, 5000.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_add_f32_e32 v10, v10, v2

	// ---- v4 = how much the near cascade's answer counts: 1 inside, 0 outside,
	// fading over its last 2.5% on each side and in depth ----
	v_add_f32_e32 v2, -0.01, v14
	v_mul_f32_e32 v2, 40.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v4, 1.0, v2
	v_mul_f32_e32 v2, -1.0, v14
	v_add_f32_e32 v2, 0.99, v2
	v_mul_f32_e32 v2, 40.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_mul_f32_e32 v4, v4, v2
	v_add_f32_e32 v2, -0.01, v11
	v_mul_f32_e32 v2, 40.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_mul_f32_e32 v4, v4, v2
	v_mul_f32_e32 v2, -1.0, v11
	v_add_f32_e32 v2, 0.99, v2
	v_mul_f32_e32 v2, 40.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_mul_f32_e32 v4, v4, v2
	v_add_f32_e32 v2, -0.01, v8
	v_mul_f32_e32 v2, 40.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_mul_f32_e32 v4, v4, v2
	v_mul_f32_e32 v2, -1.0, v8
	v_add_f32_e32 v2, 0.99, v2
	v_mul_f32_e32 v2, 40.0, v2
	v_max_f32_e32 v2, 0, v2
	v_min_f32_e32 v2, 1.0, v2
	v_mul_f32_e32 v4, v4, v2

	// shadow = far + v4 (near - far), each a quarter per texel in the way
	v_mul_f32_e32 v15, 0.25, v15
	v_mul_f32_e32 v10, 0.25, v10
	v_mul_f32_e32 v2, -1.0, v10
	v_add_f32_e32 v2, v15, v2
	v_mul_f32_e32 v2, v2, v4
	v_add_f32_e32 v2, v10, v2

	// black, as opaque as the shadow times the darkness
	v_mul_f32_e32 v15, v2, v9
	v_mul_f32_e32 v10, 0.0, v15
	v_cvt_pkrtz_f16_f32_e32 v1, v10, v10
	v_cvt_pkrtz_f16_f32_e32 v0, v10, v15
	s_mov_b64 exec, vcc
	exp mrt0 v1, v1, v0, v0 done compr vm
	s_endpgm
