// Water reflection: a second pass over the water that lays the mirrored world
// over it, from the reflection target gfx_pc draws before the frame.
//
// The reflection was drawn from the same camera, so the part of it that shows
// in a pixel of water is the one at that pixel's own place on the screen -
// which the program gets from the processor in v2 and v3, as floats, because
// the container asks for POS_X/Y_FLOAT_ENA (build_shaders.sh sets them). The
// place is pushed about by the slope of the same swells and ripple the water
// program draws, so the image wavers with the surface.
//
// How much shows is a fresnel term: little looking straight down, most at a
// glancing angle. It needs the angle between the view ray and the surface, and
// a pixel program here has no constants, so the vertices carry them in their
// colour: rgb the water's normal in view space, from -1..1 as 0..1, and alpha
// tan(fov / 2) halved. The ray is rebuilt from the pixel's place, with the
// 16:9 screen. Where nothing was reflected - the sky - the target is still
// transparent and the water is left alone.
//
// Inputs: attr0.xy texture coordinates, attr0.z time in seconds, attr1 as above.
// The container declares VGPRS 4, as it uses v11-v15 across the sample.
.text
	s_inst_prefetch 0x1
	s_mov_b32 m0, s12
	s_mov_b64 vcc, exec
	s_wqm_b64 exec, exec
	// v11, v12 = this pixel's place on the 3840x2160 scene, as 0..1 - read
	// first, before the interpolants below write over v2 and v3
	v_mul_f32_e32 v11, 0.000260417, v2
	v_mul_f32_e32 v12, 0.000462963, v3
	v_interp_p1_f32_e32 v6, v0, attr0.x
	v_interp_p1_f32_e32 v7, v0, attr0.y
	v_interp_p1_f32_e32 v2, v0, attr0.z
	v_interp_p1_f32_e32 v3, v0, attr1.x
	v_interp_p1_f32_e32 v4, v0, attr1.y
	v_interp_p1_f32_e32 v5, v0, attr1.z
	v_interp_p1_f32_e32 v8, v0, attr1.w
	v_interp_p2_f32_e32 v6, v1, attr0.x
	v_interp_p2_f32_e32 v7, v1, attr0.y
	v_interp_p2_f32_e32 v2, v1, attr0.z
	v_interp_p2_f32_e32 v3, v1, attr1.x
	v_interp_p2_f32_e32 v4, v1, attr1.y
	v_interp_p2_f32_e32 v5, v1, attr1.z
	v_interp_p2_f32_e32 v8, v1, attr1.w

	// v6, v7 = u, v   v2 = t   v3, v4, v5 = normal   v8 = tan(fov / 2) / 2
	v_mul_f32_e32 v3, 2.0, v3
	v_add_f32_e32 v3, -1.0, v3
	v_mul_f32_e32 v4, 2.0, v4
	v_add_f32_e32 v4, -1.0, v4
	v_mul_f32_e32 v5, 2.0, v5
	v_add_f32_e32 v5, -1.0, v5
	v_mul_f32_e32 v8, 2.0, v8

	// v9, v10 = the view ray through this pixel, (x, y, -1):
	// x = (2 sx - 1) tan(fov / 2) 16/9, y = (1 - 2 sy) tan(fov / 2)
	v_mul_f32_e32 v9, 2.0, v11
	v_add_f32_e32 v9, -1.0, v9
	v_mul_f32_e32 v9, v9, v8
	v_mul_f32_e32 v9, 1.777778, v9
	v_mul_f32_e32 v10, -2.0, v12
	v_add_f32_e32 v10, 1.0, v10
	v_mul_f32_e32 v10, v10, v8

	// v13 = cos^2 = (n . ray)^2 / |ray|^2
	v_mul_f32_e32 v13, v3, v9
	v_mul_f32_e32 v14, v4, v10
	v_add_f32_e32 v13, v13, v14
	v_mul_f32_e32 v14, -1.0, v5
	v_add_f32_e32 v13, v13, v14
	v_mul_f32_e32 v13, v13, v13
	v_mul_f32_e32 v14, v9, v9
	v_mul_f32_e32 v15, v10, v10
	v_add_f32_e32 v14, v14, v15
	v_add_f32_e32 v14, 1.0, v14
	v_rsq_f32_e32 v14, v14
	v_mul_f32_e32 v13, v13, v14
	v_mul_f32_e32 v13, v13, v14
	// v13 = cos, as x rsq(x)
	v_add_f32_e32 v13, 0.000001, v13
	v_rsq_f32_e32 v14, v13
	v_mul_f32_e32 v13, v13, v14
	v_min_f32_e32 v13, 1.0, v13
	// v15 = fresnel = 0.2 + 0.7 (1 - cos)^3
	v_mul_f32_e32 v13, -1.0, v13
	v_add_f32_e32 v13, 1.0, v13
	v_mul_f32_e32 v15, v13, v13
	v_mul_f32_e32 v15, v15, v13
	v_mul_f32_e32 v15, 0.7, v15
	v_add_f32_e32 v15, 0.2, v15

	// the water program's waves: c1 = cos(2.4 u + 0.7 v + 1.3 t)
	v_mul_f32_e32 v3, 2.4, v6
	v_mul_f32_e32 v4, 0.7, v7
	v_add_f32_e32 v3, v3, v4
	v_mul_f32_e32 v4, 1.3, v2
	v_add_f32_e32 v3, v3, v4
	v_cos_f32_e32 v3, v3
	// c2 = cos(-1.0 u + 3.0 v + 1.0 t)
	v_mul_f32_e32 v4, -1.0, v6
	v_mul_f32_e32 v5, 3.0, v7
	v_add_f32_e32 v4, v4, v5
	v_add_f32_e32 v4, v4, v2
	v_cos_f32_e32 v4, v4
	// c3 = cos(5 u - 4 v + 1.8 t)
	v_mul_f32_e32 v5, 5.0, v6
	v_mul_f32_e32 v8, -4.0, v7
	v_add_f32_e32 v5, v5, v8
	v_mul_f32_e32 v8, 1.8, v2
	v_add_f32_e32 v5, v5, v8
	v_cos_f32_e32 v5, v5
	// slope: v9 = gx = 0.50 c1 - 0.12 c2 + 0.04 c3, v10 = gy = 0.15 c1 + 0.42 c2 - 0.03 c3
	v_mul_f32_e32 v9, 0.50, v3
	v_mul_f32_e32 v8, -0.12, v4
	v_add_f32_e32 v9, v9, v8
	v_mul_f32_e32 v8, 0.04, v5
	v_add_f32_e32 v9, v9, v8
	v_mul_f32_e32 v10, 0.15, v3
	v_mul_f32_e32 v8, 0.42, v4
	v_add_f32_e32 v10, v10, v8
	v_mul_f32_e32 v8, -0.03, v5
	v_add_f32_e32 v10, v10, v8

	// v6, v7 = where to read the reflection: this pixel's place, pushed by the slope
	v_mul_f32_e32 v9, 0.012, v9
	v_add_f32_e32 v6, v11, v9
	v_mul_f32_e32 v10, 0.012, v10
	v_add_f32_e32 v7, v12, v10

	// v[2:5] = reflection rgba
	image_sample v[2:5], v[6:7], s[0:7], s[8:11] dmask:0xf dim:SQ_RSRC_IMG_2D
	s_waitcnt vmcnt(0)

	// the reflection, as opaque as fresnel says where anything was drawn into it
	v_mul_f32_e32 v5, v5, v15
	v_cvt_pkrtz_f16_f32_e32 v1, v2, v3
	v_cvt_pkrtz_f16_f32_e32 v0, v4, v5
	s_mov_b64 exec, vcc
	exp mrt0 v1, v1, v0, v0 done compr vm
	s_endpgm
