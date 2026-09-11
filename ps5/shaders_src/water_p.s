// Water: a surface of three travelling waves - two long swells and a faint
// ripple - whose slope is worked out per pixel, as the analytic derivative, and
// used three ways: to bend the texture coordinates as refraction would, to
// shade the surface by a fixed sun, and to raise an occasional soft sheen.
//
// The game repeats the water texture many times over a surface, and the waves
// follow its coordinates, so far off many waves fall inside one pixel. A sharp
// highlight there turns into a scatter of flickering dots; so the sheen only
// appears where a wave faces the sun almost exactly, fades in rather than
// switching on, and is gathered further into patches by a slow mask.
//
// Inputs: attr0.xy texture coordinates, attr0.z time in seconds, attr1 colour.
//
// The container must declare VGPRS 4 (build_shaders.sh does). imgui_p's own
// declaration, 2, covers only v0-v10; a program reaching higher reads garbage
// there, which drew the water a flat white.
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
	// swell: c1 = cos(2.4 u + 0.7 v + 1.3 t)
	v_mul_f32_e32 v3, 2.4, v6
	v_mul_f32_e32 v4, 0.7, v7
	v_add_f32_e32 v3, v3, v4
	v_mul_f32_e32 v4, 1.3, v2
	v_add_f32_e32 v3, v3, v4
	v_cos_f32_e32 v3, v3
	// swell: c2 = cos(-1.0 u + 3.0 v + 1.0 t)
	v_mul_f32_e32 v4, -1.0, v6
	v_mul_f32_e32 v5, 3.0, v7
	v_add_f32_e32 v4, v4, v5
	v_mul_f32_e32 v5, 1.0, v2
	v_add_f32_e32 v4, v4, v5
	v_cos_f32_e32 v4, v4
	// ripple: c3 = cos(5 u - 4 v + 1.8 t)
	v_mul_f32_e32 v5, 5.0, v6
	v_mul_f32_e32 v11, -4.0, v7
	v_add_f32_e32 v5, v5, v11
	v_mul_f32_e32 v11, 1.8, v2
	v_add_f32_e32 v5, v5, v11
	v_cos_f32_e32 v5, v5

	// slope: v11 = gx = 0.50 c1 - 0.12 c2 + 0.04 c3, v12 = gy = 0.15 c1 + 0.42 c2 - 0.03 c3
	v_mul_f32_e32 v11, 0.50, v3
	v_mul_f32_e32 v12, -0.12, v4
	v_add_f32_e32 v11, v11, v12
	v_mul_f32_e32 v12, 0.04, v5
	v_add_f32_e32 v11, v11, v12
	v_mul_f32_e32 v12, 0.15, v3
	v_mul_f32_e32 v13, 0.42, v4
	v_add_f32_e32 v12, v12, v13
	v_mul_f32_e32 v13, -0.03, v5
	v_add_f32_e32 v12, v12, v13

	// v14 = sheen mask = (0.5 + 0.5 sin(1.1 u + 0.7 v + 0.3 t))^2
	v_mul_f32_e32 v14, 1.1, v6
	v_mul_f32_e32 v15, 0.7, v7
	v_add_f32_e32 v14, v14, v15
	v_mul_f32_e32 v15, 0.3, v2
	v_add_f32_e32 v14, v14, v15
	v_sin_f32_e32 v14, v14
	v_mul_f32_e32 v14, 0.5, v14
	v_add_f32_e32 v14, 0.5, v14
	v_mul_f32_e32 v14, v14, v14

	// refraction: the texture seen through a tilted surface shifts with the slope
	v_mul_f32_e32 v13, 0.08, v11
	v_add_f32_e32 v6, v6, v13
	v_mul_f32_e32 v13, 0.08, v12
	v_add_f32_e32 v7, v7, v13

	// v13 = 1 / |(-gx, -gy, 1)|, to normalise the surface normal
	v_mul_f32_e32 v13, v11, v11
	v_mul_f32_e32 v15, v12, v12
	v_add_f32_e32 v13, v13, v15
	v_add_f32_e32 v13, 1.0, v13
	v_rsq_f32_e32 v13, v13

	// v[2:5] = texel rgba
	image_sample v[2:5], v[6:7], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)

	// texel times colour
	v_mul_f32_e32 v10, v2, v10
	v_mul_f32_e32 v9, v9, v3
	v_mul_f32_e32 v8, v8, v5
	v_mul_f32_e32 v0, v0, v4

	// v15 = shade = 0.55 + 0.6 (N . L), L = (-0.331, 0.497, 0.828)
	v_mul_f32_e32 v15, 0.331, v11
	v_mul_f32_e32 v6, -0.497, v12
	v_add_f32_e32 v15, v15, v6
	v_add_f32_e32 v15, 0.828, v15
	v_mul_f32_e32 v15, v15, v13
	v_mul_f32_e32 v15, 0.6, v15
	v_add_f32_e32 v15, 0.55, v15

	// v6 = sheen = 0.4 mask s^2, s = clamp((N . H - 0.97) * 25, 0, 1),
	// H = (0.304, 0.391, 0.869)
	v_mul_f32_e32 v6, -0.304, v11
	v_mul_f32_e32 v7, -0.391, v12
	v_add_f32_e32 v6, v6, v7
	v_add_f32_e32 v6, 0.869, v6
	v_mul_f32_e32 v6, v6, v13
	v_add_f32_e32 v6, -0.97, v6
	v_mul_f32_e32 v6, 25.0, v6
	v_max_f32_e32 v6, 0, v6
	v_min_f32_e32 v6, 1.0, v6
	v_mul_f32_e32 v6, v6, v6
	v_mul_f32_e32 v6, v6, v14
	v_mul_f32_e32 v6, 0.4, v6

	// shaded, drawn a little towards blue-green, sheen on top
	v_mul_f32_e32 v10, v10, v15
	v_mul_f32_e32 v10, 0.85, v10
	v_mul_f32_e32 v9, v9, v15
	v_mul_f32_e32 v9, 0.97, v9
	v_mul_f32_e32 v0, v0, v15
	v_mul_f32_e32 v0, 1.08, v0
	v_add_f32_e32 v10, v10, v6
	v_add_f32_e32 v9, v9, v6
	v_add_f32_e32 v0, v0, v6
	v_cvt_pkrtz_f16_f32_e32 v1, v10, v9
	v_cvt_pkrtz_f16_f32_e32 v0, v0, v8
	s_mov_b64 exec, vcc
	exp mrt0 v1, v1, v0, v0 done compr vm
	s_endpgm
