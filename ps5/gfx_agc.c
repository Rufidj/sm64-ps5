/* gfx_agc - a GfxRenderingAPI backend that draws with the PS5's graphics
 * processor, replacing the OpenGL one. Pairs with gfx_ps5.c, which owns the
 * display and the frame loop.
 *
 * The interesting constraint is that there is no shader compiler here. The
 * OpenGL backend writes a fresh GLSL program for every colour-combiner
 * configuration the game asks for; this one cannot, so it has a single fixed
 * program - texture multiplied by a vertex colour - and folds the combiner
 * into that vertex colour instead, drawing twice where once cannot express it.
 * See read_vertex() and gfx_agc_draw_triangles() for how far that goes.
 *
 * The other consequence is that the vertex format is fixed while the caller's
 * is not: it packs a different number of floats per vertex depending on the
 * combiner. draw_triangles() therefore reads the caller's layout and repacks.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "gfx/gfx_cc.h"
#include "gfx/gfx_rendering_api.h"
#include "ps5gpu.h"
#include "shaders_build/water_p_sb.h"
#include "shaders_build/lava_p_sb.h"
#include "shaders_build/water_reflect_p_sb.h"
#include "shaders_build/water_reflect_1x_p_sb.h"
#include "shaders_build/shadow_depth_p_sb.h"
#include "shaders_build/shadow_recv_p_sb.h"
#include "ps5_settings.h"
#include "hd_textures.h"

/* The game's own water and lava textures, whose pixels identify the draws that
 * get the water and lava programs. All 32x32 RGBA16. */
extern const uint8_t texture_waterbox_water[];
extern const uint8_t texture_waterbox_jrb_water[];
extern const uint8_t texture_waterbox_unknown_water[];
extern const uint8_t texture_waterbox_lava[];

#define MAX_SHADERS 64
#define MAX_TEXTURE_IDS 1024          /* the size of ps5gpu's texture table */
#define MAX_BATCH_VERTS (256 * 3)     /* the caller flushes at 256 triangles */
/* Clipping can split each triangle in two. */
#define MAX_VERTS_PER_DRAW 8192

/* How far a decal is pulled towards the camera, in normalised depth. Stands in
 * for the OpenGL backend's polygon offset: a constant rather than a slope, so
 * it is generous near the camera and leaks slightly far away. */
#define DECAL_DEPTH_BIAS 2e-5f

/* How closely a vertex's w has to follow the projection fitted to its group,
 * relative to w. A tenth of a pixel at the edge of a 1080p screen. */
#define FIT_TOLERANCE 1e-4

/* Below this, a combiner term counts as absent. */
#define TERM_EPSILON (1.0f / 512.0f)

struct ShaderProgram {
    uint32_t shader_id;
    struct CCFeatures cc;
    /* The colour is a mix of a textured operand and an untextured one, by the
     * texel's alpha - a decal over shading, as on Mario's face and cap. */
    bool blends_by_texel_alpha;
    bool in_use;
};

/* A vertex still in clip space, with its colour already combined. */
typedef struct {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
    float lu, lv, ld;      /* where the shadow map sees it, when the caller says */
} ClipVertex;

/* What this backend keeps per texture. OpenGL stores the sampler state in the
 * texture object, so the caller only reports it when that texture's own state
 * changes; with one sampler for everything it has to be kept here and put back
 * whenever the texture is. The average colour stands in for the texture where
 * the combiner cannot be expressed as texture times colour. */
typedef struct {
    bool linear;
    uint32_t cms, cmt;
    float average[4];
    uint8_t material;
} TextureInfo;

/* What a texture's pixels say the surface is, and so which program draws it. */
enum { MATERIAL_PLAIN, MATERIAL_WATER, MATERIAL_LAVA, MATERIAL_COUNT };
static int s_material_program[MATERIAL_COUNT];

/* A fingerprint of each material texture as the caller uploads it. */
typedef struct { uint64_t hash; uint8_t material; } MaterialTexture;
static MaterialTexture s_material_textures[4];
static int s_material_texture_count;

/* Frames since start, for the time the water and lava programs animate by,
 * and the value the vertices' spare normal component carries this batch. */
static uint32_t s_frame;
static float s_npad;

/* Reflections. gfx_pc draws the world mirrored in the water's plane before the
 * frame, between gfx_agc_reflection_begin() and gfx_agc_reflection_end(), into
 * ps5gpu's reflection target - everything but the water itself. The water
 * then gets a second pass that lays that image over it. The normal of the
 * plane in view space and the camera's tan(fov / 2) travel to that program in
 * the vertex colour, which is otherwise unused there. */
static int s_reflect_program;
static int s_reflect_program_1x;   /* the same, for a scene drawn without supersampling */
static bool s_reflecting;
static bool s_reflecting_sky;
static bool s_reflection_ready;
static float s_reflect_normal[3], s_reflect_tan_half_fov;
/* Whether reflections are drawn at all, and how strongly the sky shows in the
 * water on top of the fresnel term, come from the options menu. The sky goes
 * into the reflection with that strength as its alpha: fully, it pales the
 * water away from the game's own colours. */

/* Real shadows. gfx_pc draws what casts into ps5gpu's shadow map first,
 * between gfx_agc_shadow_begin() and gfx_agc_shadow_end(), as depth packed into
 * two colour channels. Then, while the world is drawn, its vertices carry where
 * the shadow map sees them (gfx_agc_set_light_attributes), and each opaque
 * surface gets a second pass that darkens what the map says the sun cannot
 * reach. */
static int s_shadow_depth_program, s_shadow_receive_program;
static bool s_shadowing, s_shadow_ready, s_light_attributes;
#define SHADOW_STRENGTH 0.45f

/* What the vertices' spare slots carry as a group is packed. */
enum { PACK_NORMAL, PACK_SHADOW_DEPTH, PACK_LIGHT };
static int s_pack = PACK_NORMAL;

/* How a batch's vertex colours are settled; see set_colours(). */
typedef enum {
    COLOUR_FOR_WHITE_TEXEL,
    COLOUR_WITH_AVERAGE,
    COLOUR_BASE,
    COLOUR_OVERLAY
} ColourMode;

static struct ShaderProgram s_shaders[MAX_SHADERS];
static struct ShaderProgram *s_current;
static TextureInfo s_textures[MAX_TEXTURE_IDS];
static uint32_t s_white_texture;          /* stands in when a draw uses no texture */
static uint32_t s_unit_texture[2];        /* what the caller last selected per unit */
static uint32_t s_upload_target;          /* what it last selected on any unit      */
static bool s_decal;
static bool s_use_alpha;                  /* the state the caller last set, to put */
static bool s_depth_mask;                 /* back after an overlay pass            */
static bool s_depth_test;

/* The caller's vertices once read: colour still split into what multiplies the
 * texel and what does not, decided per draw in draw_triangles(). */
static ClipVertex s_batch[MAX_BATCH_VERTS];
static float s_tex_coef[MAX_BATCH_VERTS][4];
static float s_constant[MAX_BATCH_VERTS][4];
static float s_fog[MAX_BATCH_VERTS][4];
static float s_base[MAX_BATCH_VERTS][3];
static float s_overlay[MAX_BATCH_VERTS][3];

/* The group being gathered for one draw call, and the projection it shares. */
static Ps5GpuVertex s_verts[MAX_VERTS_PER_DRAW];
static int s_vert_count;
static bool s_group_open;
static double s_group_gamma, s_group_delta;

/* The last perspective fit, reused while it holds so that neighbouring batches
 * reconstruct w identically and meet without cracks. */
static bool s_have_perspective;
static double s_perspective_gamma, s_perspective_delta;

static float absf(float v) { return v < 0.0f ? -v : v; }
static double absd(double v) { return v < 0.0 ? -v : v; }
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static bool gfx_agc_z_is_from_0_to_1(void) {
    return true;   /* the projection here puts depth in [0,1], as Direct3D does */
}

static void gfx_agc_unload_shader(struct ShaderProgram *old_prg) {
    (void)old_prg;
    s_current = NULL;
}

static void gfx_agc_load_shader(struct ShaderProgram *new_prg) {
    s_current = new_prg;
}

static bool is_texel(uint8_t item) {
    return item == SHADER_TEXEL0 || item == SHADER_TEXEL0A || item == SHADER_TEXEL1;
}

/* (a - b) * texel alpha + b, with a a texel and b free of one. */
static bool blends_by_texel_alpha(const struct CCFeatures *cc) {
    const uint8_t *c = cc->c[0];
    return c[2] == SHADER_TEXEL0A && c[1] == c[3] && !is_texel(c[1])
        && (c[0] == SHADER_TEXEL0 || c[0] == SHADER_TEXEL1);
}

static struct ShaderProgram *gfx_agc_create_and_load_new_shader(uint32_t shader_id) {
    for (int i = 0; i < MAX_SHADERS; i++) {
        if (!s_shaders[i].in_use) {
            s_shaders[i].in_use = true;
            s_shaders[i].shader_id = shader_id;
            gfx_cc_get_features(shader_id, &s_shaders[i].cc);
            s_shaders[i].blends_by_texel_alpha = blends_by_texel_alpha(&s_shaders[i].cc);
            s_current = &s_shaders[i];
            return s_current;
        }
    }
    return NULL;
}

static struct ShaderProgram *gfx_agc_lookup_shader(uint32_t shader_id) {
    for (int i = 0; i < MAX_SHADERS; i++)
        if (s_shaders[i].in_use && s_shaders[i].shader_id == shader_id)
            return &s_shaders[i];
    return NULL;
}

/* The caller builds its vertex buffer from what this reports, so it has to
 * describe the combiner honestly even though the drawing afterwards
 * approximates it. Lying here would misalign the whole buffer. */
static void gfx_agc_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = (uint8_t)prg->cc.num_inputs;
    used_textures[0] = prg->cc.used_textures[0];
    used_textures[1] = prg->cc.used_textures[1];
}

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* Fingerprints a 32x32 RGBA16 texture as it will reach upload_texture: the
 * caller's own conversion to RGBA32, bit for bit, then hashed. */
static void remember_material_texture(const uint8_t *rgba16, uint8_t material) {
    uint8_t rgba32[32 * 32 * 4];
    for (int i = 0; i < 32 * 32; i++) {
        uint16_t col16 = (uint16_t)((rgba16[2 * i] << 8) | rgba16[2 * i + 1]);
        uint8_t r = (uint8_t)(col16 >> 11), g = (col16 >> 6) & 0x1f, b = (col16 >> 1) & 0x1f;
        rgba32[4 * i + 0] = (uint8_t)((r * 0xFF) / 0x1F);
        rgba32[4 * i + 1] = (uint8_t)((g * 0xFF) / 0x1F);
        rgba32[4 * i + 2] = (uint8_t)((b * 0xFF) / 0x1F);
        rgba32[4 * i + 3] = (col16 & 1) ? 255 : 0;
    }
    if (s_material_texture_count < 4) {
        s_material_textures[s_material_texture_count].hash = fnv1a(rgba32, sizeof rgba32);
        s_material_textures[s_material_texture_count].material = material;
        s_material_texture_count++;
    }
}

static TextureInfo *texture_info(uint32_t id) {
    return id < MAX_TEXTURE_IDS ? &s_textures[id] : NULL;
}

static uint32_t gfx_agc_new_texture(void) {
    return ps5gpu_texture_new();
}

static void gfx_agc_select_texture(int tile, uint32_t texture_id) {
    /* Only the first texture unit is bound: the fixed program samples one
     * texture, and the combiner treats the second as if it were the first. */
    s_upload_target = texture_id;
    s_unit_texture[tile ? 1 : 0] = texture_id;
    if (tile == 0) {
        ps5gpu_texture_select(texture_id);
        TextureInfo *t = texture_info(texture_id);
        if (t) ps5gpu_sampler_set(t->linear ? 1 : 0, t->cms, t->cmt);
    }
}

/* The caller uploads into whichever texture it last selected, on either unit,
 * so that has to be tracked here - the id is not passed in. Tracking unit 0
 * alone sent a second-unit upload over the first unit's texture. */
/* The bytes the texture being imported was made from, as gfx_pc reports just
 * before converting them: what the HD replacement recognises it by. */
static const uint8_t *s_source_addr;
static uint32_t s_source_size;

void gfx_agc_texture_source(const uint8_t *addr, uint32_t size_bytes) {
    s_source_addr = addr;
    s_source_size = size_bytes;
}

static void gfx_agc_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    /* An HD image takes the texture's place on the processor. What is worked
     * out from the pixels below - the average, and whether it is water or lava
     * - still comes from the game's own texture, which is what it knows. */
    const uint8_t *source = s_source_addr;
    s_source_addr = NULL;
    uint8_t *hd = NULL;
    int hd_width = 0, hd_height = 0;
    if (source && g_ps5_settings.hd_textures)
        hd = hd_texture_load(source, s_source_size, &hd_width, &hd_height);
    if (hd) {
        ps5gpu_texture_upload(s_upload_target, hd, hd_width, hd_height);
        hd_texture_free(hd);
    } else {
        ps5gpu_texture_upload(s_upload_target, rgba32_buf, width, height);
    }

    TextureInfo *t = texture_info(s_upload_target);
    if (t == NULL || width <= 0 || height <= 0) return;
    uint64_t sum[4] = { 0, 0, 0, 0 };
    size_t texels = (size_t)width * (size_t)height;
    for (size_t i = 0; i < texels; i++)
        for (int c = 0; c < 4; c++) sum[c] += rgba32_buf[i * 4 + (size_t)c];
    for (int c = 0; c < 4; c++)
        t->average[c] = (float)sum[c] / ((float)texels * 255.0f);

    t->material = MATERIAL_PLAIN;
    if (width == 32 && height == 32) {
        uint64_t hash = fnv1a(rgba32_buf, 32 * 32 * 4);
        for (int i = 0; i < s_material_texture_count; i++)
            if (s_material_textures[i].hash == hash) t->material = s_material_textures[i].material;
    }
}

static void gfx_agc_set_sampler_parameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) {
    TextureInfo *t = texture_info(s_unit_texture[sampler ? 1 : 0]);
    if (t) { t->linear = linear_filter; t->cms = cms; t->cmt = cmt; }
    if (sampler == 0) ps5gpu_sampler_set(linear_filter ? 1 : 0, cms, cmt);
}

static void gfx_agc_set_depth_test(bool depth_test)  { s_depth_test = depth_test; ps5gpu_set_depth_test(depth_test ? 1 : 0); }
static void gfx_agc_set_depth_mask(bool z_upd)       { s_depth_mask = z_upd; ps5gpu_set_depth_mask(z_upd ? 1 : 0); }
static void gfx_agc_set_zmode_decal(bool zmode_decal){ s_decal = zmode_decal; ps5gpu_set_depth_decal(zmode_decal ? 1 : 0); }
static void gfx_agc_set_use_alpha(bool use_alpha)    { s_use_alpha = use_alpha; ps5gpu_set_blend(use_alpha ? 1 : 0); }

static void gfx_agc_set_viewport(int x, int y, int width, int height) {
    ps5gpu_set_viewport(x, y, width, height);
}
static void gfx_agc_set_scissor(int x, int y, int width, int height) {
    ps5gpu_set_scissor(x, y, width, height);
}

/* ---- combining ---- */

/* One combiner operand as m * texel + k, per channel. */
typedef struct { float m, k; } Term;

static Term term_of(uint8_t item, const float inputs[4][4], int ch) {
    Term t = { 0.0f, 0.0f };
    switch (item) {
        case SHADER_INPUT_1: case SHADER_INPUT_2: case SHADER_INPUT_3: case SHADER_INPUT_4:
            t.k = inputs[item - SHADER_INPUT_1][ch];
            break;
        case SHADER_TEXEL0:
        case SHADER_TEXEL1:            /* the second texture is read as the first */
            t.m = 1.0f;
            break;
        case SHADER_TEXEL0A:
            /* The texel's alpha. In the alpha channel that is the texel itself;
             * in a colour channel it cannot be expressed, and opaque is the
             * common answer. The combiners that use it as a mix factor are
             * drawn in two passes instead - see blends_by_texel_alpha(). */
            if (ch == 3) t.m = 1.0f; else t.k = 1.0f;
            break;
        default:
            break;
    }
    return t;
}

/* Evaluates (a - b) * c + d for one channel as m * texel + k. Every form the
 * OpenGL backend specialises - single, multiply, mix - is this formula with
 * operands set to zero or repeated, so evaluating it in full gives the same
 * answer. A texel times a texel is taken as one texel, exact at 0 and 1. */
static Term combine_channel(const uint8_t c[4], const float inputs[4][4], int ch) {
    Term a = term_of(c[0], inputs, ch), b = term_of(c[1], inputs, ch);
    Term m = term_of(c[2], inputs, ch), d = term_of(c[3], inputs, ch);
    Term diff = { a.m - b.m, a.k - b.k };
    Term out = { diff.m * m.m + diff.m * m.k + diff.k * m.m + d.m, diff.k * m.k + d.k };
    return out;
}

/* Reads one of the caller's vertices: position, texture coordinates, fog, and
 * the combiner already evaluated as a texel coefficient and a constant - and,
 * for a mix by texel alpha, its two operands apart. */
static void read_vertex(const struct ShaderProgram *prg, const float *v, bool use_texture, int comps, size_t i) {
    const struct CCFeatures *cc = &prg->cc;
    ClipVertex *o = &s_batch[i];
    size_t k = 4;
    o->x = v[0]; o->y = v[1]; o->z = v[2]; o->w = v[3];

    if (use_texture) { o->u = v[k]; o->v = v[k + 1]; k += 2; }
    else             { o->u = 0.0f; o->v = 0.0f; }

    if (cc->opt_fog) {
        for (int f = 0; f < 4; f++) s_fog[i][f] = v[k + (size_t)f];
        k += 4;
    } else {
        s_fog[i][3] = 0.0f;
    }

    /* Each input carries its colour from the colour combiner's source and its
     * alpha from the alpha combiner's source. */
    float inputs[4][4] = { { 0 } };
    for (int n = 0; n < cc->num_inputs && n < 4; n++) {
        inputs[n][0] = v[k]; inputs[n][1] = v[k + 1]; inputs[n][2] = v[k + 2];
        inputs[n][3] = cc->opt_alpha ? v[k + 3] : 1.0f;
        k += (size_t)comps;
    }
    if (s_light_attributes) { o->lu = v[k]; o->lv = v[k + 1]; o->ld = v[k + 2]; }
    else                    { o->lu = 0.0f; o->lv = 0.0f; o->ld = 0.0f; }

    for (int ch = 0; ch < 3; ch++) {
        Term t = combine_channel(cc->c[0], inputs, ch);
        s_tex_coef[i][ch] = t.m; s_constant[i][ch] = t.k;
        if (prg->blends_by_texel_alpha) {
            Term base = term_of(cc->c[0][1], inputs, ch), over = term_of(cc->c[0][0], inputs, ch);
            s_base[i][ch] = base.m + base.k;
            s_overlay[i][ch] = over.m + over.k;
        }
    }
    if (cc->opt_alpha) {
        Term t = combine_channel(cc->c[1], inputs, 3);
        s_tex_coef[i][3] = t.m; s_constant[i][3] = t.k;
    } else {
        s_tex_coef[i][3] = 0.0f; s_constant[i][3] = 1.0f;
    }
}

/* Settles the batch's vertex colours.
 *
 * For a white texel: the program multiplies by the texel afterwards, so this
 * is exact whenever the combiner is a texel times a colour or ignores the
 * texel, which is most of what the game draws.
 *
 * With the average: a combiner that adds a colour on top of a texture-weighted
 * one - the shine on Mario's face in the title - cannot be split that way, so
 * it is drawn untextured with the texture's average in place of the texel.
 *
 * Base and overlay: the two passes of a mix by texel alpha. The base is the
 * untextured operand; the overlay is the textured one, blended over it by the
 * texel's alpha, which together give the mix exactly. */
static void set_colours(size_t count, ColourMode mode, const float *average) {
    for (size_t i = 0; i < count; i++) {
        float out[4];
        for (int c = 0; c < 4; c++)
            out[c] = mode == COLOUR_WITH_AVERAGE ? s_tex_coef[i][c] * average[c] + s_constant[i][c]
                                                 : s_tex_coef[i][c] + s_constant[i][c];
        if (mode == COLOUR_BASE)    for (int c = 0; c < 3; c++) out[c] = s_base[i][c];
        if (mode == COLOUR_OVERLAY) for (int c = 0; c < 3; c++) out[c] = s_overlay[i][c];
        for (int c = 0; c < 4; c++) out[c] = clamp01(out[c]);

        /* Fog blends towards its colour by a per-vertex factor. Blending the
         * vertex colour instead of the final pixel tints the texture rather
         * than covering it, which is close on bright textures and too dark on
         * dark ones. */
        float f = s_fog[i][3];
        if (f > 0.0f)
            for (int c = 0; c < 3; c++) out[c] += (s_fog[i][c] - out[c]) * f;

        ClipVertex *o = &s_batch[i];
        o->r = out[0]; o->g = out[1]; o->b = out[2]; o->a = out[3];
    }
}

/* ---- projecting ---- */

static void pack_colour(Ps5GpuVertex *o, const ClipVertex *cv) {
    unsigned int ir = (unsigned int)(cv->r * 255.0f + 0.5f);
    unsigned int ig = (unsigned int)(cv->g * 255.0f + 0.5f);
    unsigned int ib = (unsigned int)(cv->b * 255.0f + 0.5f);
    unsigned int ia = (unsigned int)(cv->a * 255.0f + 0.5f);
    o->color = (ia << 24) | (ir << 16) | (ig << 8) | ib;
    if (s_pack == PACK_LIGHT) {
        o->u = cv->lu; o->v = cv->lv; o->n_pad = cv->ld;
    } else {
        o->u = cv->u; o->v = cv->v;
        /* The shadow map's depth: z is already set, and w is one there. */
        o->n_pad = s_pack == PACK_SHADOW_DEPTH ? o->z : s_npad;
    }
    o->uv_unused[0] = 0.0f; o->uv_unused[1] = 0.0f;
}

/* Draws the gathered group with its projection. The vertices carry clip-space
 * x, y and z; the matrix rebuilds w from z, as w = gamma * z + delta, so the
 * processor divides and interpolates with perspective itself. */
static void flush_group(void) {
    if (s_vert_count == 0) return;
    float m[16];
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = 1.0f;
    m[11] = (float)s_group_gamma;
    m[15] = (float)s_group_delta;
    ps5gpu_set_mvp(m);
    ps5gpu_draw(s_verts, s_vert_count);
    s_vert_count = 0;
}

static void use_projection(double gamma, double delta) {
    if (s_group_open && s_group_gamma == gamma && s_group_delta == delta) return;
    flush_group();
    s_group_open = true;
    s_group_gamma = gamma;
    s_group_delta = delta;
}

static bool fits(double gamma, double delta, const ClipVertex *v) {
    double w = v->w;
    return absd(gamma * v->z + delta - w) <= FIT_TOLERANCE * absd(w) + 1e-6;
}

static bool fits_all(double gamma, double delta, const ClipVertex *p[3]) {
    return fits(gamma, delta, p[0]) && fits(gamma, delta, p[1]) && fits(gamma, delta, p[2]);
}

/* In a perspective projection, clip-space z is an affine function of w, so w
 * can be recovered from z through the two vertices furthest apart in depth. */
static bool fit_between(const ClipVertex *p, const ClipVertex *q, double *gamma, double *delta) {
    double dz = (double)q->z - (double)p->z;
    if (absd(dz) <= 1e-6 * (1.0 + absd(q->z))) return false;
    *gamma = ((double)q->w - (double)p->w) / dz;
    *delta = (double)p->w - *gamma * (double)p->z;
    return true;
}

/* Every vertex at once, for a triangle no shared projection fits: w and z
 * both constant, or no affine relation between them. Divided here instead,
 * with w left at one - affine, but it only happens where nothing better
 * holds. */
static void emit_divided(const ClipVertex *p[3]) {
    use_projection(0.0, 1.0);
    for (int i = 0; i < 3; i++) {
        Ps5GpuVertex *o = &s_verts[s_vert_count++];
        float inv = 1.0f / p[i]->w;
        o->x = p[i]->x * inv; o->y = p[i]->y * inv; o->z = p[i]->z * inv;
        pack_colour(o, p[i]);
    }
}

static void emit_triangle(const ClipVertex *p0, const ClipVertex *p1, const ClipVertex *p2,
                          bool batch_fit, double batch_gamma, double batch_delta) {
    const ClipVertex *p[3] = { p0, p1, p2 };
    if (p0->w <= 1e-6f || p1->w <= 1e-6f || p2->w <= 1e-6f) return;
    if (s_vert_count + 3 > MAX_VERTS_PER_DRAW) flush_group();

    double gamma = 0.0, delta = 0.0;
    bool found = false;
    if (s_group_open && fits_all(s_group_gamma, s_group_delta, p)) {
        gamma = s_group_gamma; delta = s_group_delta; found = true;
    } else if (batch_fit && fits_all(batch_gamma, batch_delta, p)) {
        gamma = batch_gamma; delta = batch_delta; found = true;
    } else if (fits_all(0.0, 1.0, p)) {
        gamma = 0.0; delta = 1.0; found = true;               /* orthographic */
    } else {
        int a = 0, b = 1;
        float spread = absf(p[1]->z - p[0]->z);
        if (absf(p[2]->z - p[0]->z) > spread) { b = 2; spread = absf(p[2]->z - p[0]->z); }
        if (absf(p[2]->z - p[1]->z) > spread) { a = 1; b = 2; }
        if (fit_between(p[a], p[b], &gamma, &delta) && fits_all(gamma, delta, p)) found = true;
        else if (fits_all(0.0, (double)p0->w, p)) { gamma = 0.0; delta = (double)p0->w; found = true; }
    }
    if (!found) { emit_divided(p); return; }

    use_projection(gamma, delta);
    for (int i = 0; i < 3; i++) {
        Ps5GpuVertex *o = &s_verts[s_vert_count++];
        o->x = p[i]->x; o->y = p[i]->y; o->z = p[i]->z;
        pack_colour(o, p[i]);
    }
}

static ClipVertex lerp_vertex(const ClipVertex *p, const ClipVertex *q, float t) {
    ClipVertex r;
    r.x = p->x + (q->x - p->x) * t;  r.y = p->y + (q->y - p->y) * t;
    r.z = p->z + (q->z - p->z) * t;  r.w = p->w + (q->w - p->w) * t;
    r.u = p->u + (q->u - p->u) * t;  r.v = p->v + (q->v - p->v) * t;
    r.r = p->r + (q->r - p->r) * t;  r.g = p->g + (q->g - p->g) * t;
    r.b = p->b + (q->b - p->b) * t;  r.a = p->a + (q->a - p->a) * t;
    r.lu = p->lu + (q->lu - p->lu) * t;  r.lv = p->lv + (q->lv - p->lv) * t;
    r.ld = p->ld + (q->ld - p->ld) * t;
    return r;
}

/* The caller rejects a triangle only when all of it lies outside one plane, so
 * triangles crossing the near plane arrive whole. They are clipped here, in
 * clip space, where every attribute interpolates linearly - which also keeps
 * the new vertices on the same z-to-w line as the old. The caller has already
 * turned z into (z + w) / 2, which puts the OpenGL near plane, z >= -w, at
 * z >= 0. Past it every w is positive, and the other planes are the
 * processor's to clip. */
static void clip_and_emit(const ClipVertex *tri, bool batch_fit, double gamma, double delta) {
    ClipVertex poly[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const ClipVertex *p = &tri[i], *q = &tri[(i + 1) % 3];
        bool p_in = p->z >= 0.0f, q_in = q->z >= 0.0f;
        if (p_in) poly[n++] = *p;
        if (p_in != q_in) poly[n++] = lerp_vertex(p, q, p->z / (p->z - q->z));
    }
    if (n >= 3) emit_triangle(&poly[0], &poly[1], &poly[2], batch_fit, gamma, delta);
    if (n == 4) emit_triangle(&poly[0], &poly[2], &poly[3], batch_fit, gamma, delta);
}

static void emit_batch(size_t count, bool batch_fit, double gamma, double delta) {
    s_vert_count = 0;
    s_group_open = false;
    for (size_t t = 0; t + 3 <= count; t += 3)
        clip_and_emit(&s_batch[t], batch_fit, gamma, delta);
    flush_group();
    s_group_open = false;
}

/* The reflection over a batch of water just drawn: the same triangles at the
 * same depth, blended by the program's fresnel term, without writing depth.
 * The caller's texture, sampler, blending and depth writes are put back. */
static void draw_reflection(size_t count, bool batch_fit, double gamma, double delta) {
    for (size_t i = 0; i < count; i++) {
        ClipVertex *o = &s_batch[i];
        o->r = clamp01(s_reflect_normal[0] * 0.5f + 0.5f);
        o->g = clamp01(s_reflect_normal[1] * 0.5f + 0.5f);
        o->b = clamp01(s_reflect_normal[2] * 0.5f + 0.5f);
        o->a = clamp01(s_reflect_tan_half_fov * 0.5f);
    }
    /* The program reads its place on the scene in pixels, which are twice as
     * many each way when the scene is supersampled or drawn for 4K. */
    ps5gpu_set_program(ps5gpu_render_scale() == 1 && s_reflect_program_1x ? s_reflect_program_1x : s_reflect_program);
    ps5gpu_texture_select(ps5gpu_reflection_texture());
    ps5gpu_sampler_set(1, PS5GPU_CLAMP, PS5GPU_CLAMP);
    ps5gpu_set_blend(1);
    ps5gpu_set_depth_mask(0);
    emit_batch(count, batch_fit, gamma, delta);
    ps5gpu_set_blend(s_use_alpha ? 1 : 0);
    ps5gpu_set_depth_mask(s_depth_mask ? 1 : 0);
    ps5gpu_texture_select(s_unit_texture[0]);
    TextureInfo *t = texture_info(s_unit_texture[0]);
    if (t) ps5gpu_sampler_set(t->linear ? 1 : 0, t->cms, t->cmt);
}

/* Whether a batch just drawn takes a shadow: an opaque surface of the world,
 * drawn in the frame itself once the shadow map is. Cut-outs and see-through
 * surfaces do not - the pass would darken their holes too. */
static bool shadow_receivable(bool cutout) {
    return s_light_attributes && s_shadow_ready && !s_reflecting && !s_shadowing && !cutout && !s_use_alpha;
}

/* The shadow over a batch just drawn: the same triangles at the same depth, in
 * black, as opaque as the shadow map says the sun is hidden, without writing
 * depth. The caller's program, texture, sampler, blending and depth writes are
 * put back. */
static void draw_shadow_overlay(size_t count, bool batch_fit, double gamma, double delta) {
    for (size_t i = 0; i < count; i++) {
        ClipVertex *o = &s_batch[i];
        o->r = 0.0f; o->g = 0.0f; o->b = 0.0f;
        o->a = SHADOW_STRENGTH;
    }
    ps5gpu_set_program(s_shadow_receive_program);
    ps5gpu_texture_select(ps5gpu_shadow_texture());
    ps5gpu_sampler_set(0, PS5GPU_CLAMP, PS5GPU_CLAMP);     /* packed depth: never filtered */
    ps5gpu_set_alpha_test(0);
    ps5gpu_set_blend(1);
    ps5gpu_set_depth_mask(0);
    s_pack = PACK_LIGHT;
    emit_batch(count, batch_fit, gamma, delta);
    s_pack = PACK_NORMAL;
    ps5gpu_set_program(0);
    ps5gpu_set_blend(s_use_alpha ? 1 : 0);
    ps5gpu_set_depth_mask(s_depth_mask ? 1 : 0);
    ps5gpu_texture_select(s_unit_texture[0]);
    TextureInfo *t = texture_info(s_unit_texture[0]);
    if (t) ps5gpu_sampler_set(t->linear ? 1 : 0, t->cms, t->cmt);
}

/* Asked by gfx_pc each frame: 0 no shadows, 1 objects cast, 2 everything does. */
int gfx_agc_shadow_casters(void) {
    if (!s_shadow_depth_program || !s_shadow_receive_program) return 0;
    return g_ps5_settings.shadows;
}

bool gfx_agc_shadow_begin(int cascade) {
    if (s_shadowing) return false;
    ps5gpu_set_shadow_cascade(cascade);
    ps5gpu_set_target(PS5GPU_TARGET_SHADOW);
    s_shadowing = true;
    s_shadow_ready = true;
    return true;
}

void gfx_agc_shadow_end(void) {
    if (!s_shadowing) return;
    ps5gpu_set_target(PS5GPU_TARGET_SCENE);
    s_shadowing = false;
}

bool gfx_agc_shadow_ready(void) {
    return s_shadow_ready;
}

/* Whether the caller's vertices carry three more floats, where the shadow map
 * sees them. Changed only between batches. */
void gfx_agc_set_light_attributes(bool on) {
    s_light_attributes = on;
}

/* Starts drawing into the reflection. A frame may do it more than once - the
 * mirrored sky, then the mirrored world - and only the world brings the view
 * the water's pass needs, so only that makes the reflection ready to use. */
bool gfx_agc_reflection_begin(bool has_view, float nx, float ny, float nz, float tan_half_fov) {
    if (!s_reflect_program || s_reflecting) return false;
    if (!g_ps5_settings.reflections) return false;
    if (!has_view && ps5_settings_sky_strength() <= 0.0f) return false;   /* no sky: skip drawing it */
    s_reflecting_sky = !has_view;
    if (has_view) {
        s_reflect_normal[0] = nx; s_reflect_normal[1] = ny; s_reflect_normal[2] = nz;
        s_reflect_tan_half_fov = tan_half_fov;
        s_reflection_ready = true;
    }
    ps5gpu_set_target(PS5GPU_TARGET_REFLECTION);
    s_reflecting = true;
    return true;
}

void gfx_agc_reflection_end(void) {
    if (!s_reflecting) return;
    ps5gpu_set_target(PS5GPU_TARGET_SCENE);
    s_reflecting = false;
    s_reflecting_sky = false;
}

/* Asked by gfx_pc before it runs the display list for the reflection at all. */
bool gfx_agc_reflections_enabled(void) {
    return s_reflect_program != 0 && g_ps5_settings.reflections;
}

static void gfx_agc_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (s_current == NULL) return;
    (void)buf_vbo_len;

    const struct CCFeatures *cc = &s_current->cc;
    bool use_texture = cc->used_textures[0] || cc->used_textures[1];
    int comps = cc->opt_alpha ? 4 : 3;

    /* The water does not reflect itself. */
    if (s_reflecting && use_texture) {
        TextureInfo *t = texture_info(s_unit_texture[0]);
        if (t && t->material == MATERIAL_WATER) return;
    }

    /* The caller's stride, in floats: position, then texture coordinates, then
     * fog, then one colour per combiner input. */
    size_t stride = 4;
    if (use_texture) stride += 2;
    if (cc->opt_fog) stride += 4;
    stride += (size_t)cc->num_inputs * (size_t)comps;
    if (s_light_attributes) stride += 3;

    size_t count = buf_vbo_num_tris * 3;
    if (count > MAX_BATCH_VERTS) count = MAX_BATCH_VERTS;

    bool mixed = false, texel_counts = false;
    for (size_t i = 0; i < count; i++) {
        read_vertex(s_current, buf_vbo + i * stride, use_texture, comps, i);
        for (int c = 0; c < 4; c++) {
            if (absf(s_tex_coef[i][c]) > TERM_EPSILON && absf(s_constant[i][c]) > TERM_EPSILON) mixed = true;
            if (absf(s_tex_coef[i][c]) > TERM_EPSILON) texel_counts = true;
        }
    }

    /* The z range of the batch gives the best-conditioned projection fit. The
     * previous one is kept if it still holds, so batches agree on w. */
    size_t zmin = 0, zmax = 0;
    for (size_t i = 0; i < count; i++) {
        ClipVertex *v = &s_batch[i];
        if (s_decal) v->z -= DECAL_DEPTH_BIAS * v->w;
        if (v->z < s_batch[zmin].z) zmin = i;
        if (v->z > s_batch[zmax].z) zmax = i;
    }
    bool batch_fit = false;
    double gamma = 0.0, delta = 0.0;
    if (count > 0) {
        if (s_have_perspective && fits(s_perspective_gamma, s_perspective_delta, &s_batch[zmin])
                               && fits(s_perspective_gamma, s_perspective_delta, &s_batch[zmax])) {
            gamma = s_perspective_gamma; delta = s_perspective_delta; batch_fit = true;
        } else if (fit_between(&s_batch[zmin], &s_batch[zmax], &gamma, &delta) && gamma != 0.0) {
            s_perspective_gamma = gamma; s_perspective_delta = delta; s_have_perspective = true;
            batch_fit = true;
        }
    }

    /* A texture edge is a cut-out: the OpenGL backend discards texels whose
     * alpha is under 0.3 and draws the rest at full alpha. Here the processor's
     * alpha test drops them - at one half - and blending is off for the rest. */
    bool cutout = cc->opt_texture_edge && cc->opt_alpha;
    /* The mirrored sky: its alpha carries the chosen strength into the
     * reflection, so it is drawn unblended and untested. */
    bool sky = s_reflecting && s_reflecting_sky;
    if (sky) cutout = false;
    ps5gpu_set_alpha_test(cutout ? 1 : 0);

    /* Into the shadow map: depth, and for a cut-out the texel's alpha, which
     * the alpha test uses to leave its holes out. Water and lava, decals and
     * see-through surfaces cast nothing. */
    if (s_shadowing) {
        TextureInfo *t = use_texture ? texture_info(s_unit_texture[0]) : NULL;
        if (s_decal || (s_use_alpha && !cutout) || (t && t->material != MATERIAL_PLAIN)) return;
        set_colours(count, COLOUR_FOR_WHITE_TEXEL, NULL);
        ps5gpu_set_program(s_shadow_depth_program);
        if (!use_texture) ps5gpu_texture_select(s_white_texture);
        ps5gpu_set_blend(0);
        ps5gpu_set_depth_test(1);
        ps5gpu_set_depth_mask(1);
        s_pack = PACK_SHADOW_DEPTH;
        emit_batch(count, batch_fit, gamma, delta);
        s_pack = PACK_NORMAL;
        ps5gpu_set_program(0);
        if (!use_texture) ps5gpu_texture_select(s_unit_texture[0]);
        ps5gpu_set_blend(s_use_alpha ? 1 : 0);
        ps5gpu_set_depth_test(s_depth_test ? 1 : 0);
        ps5gpu_set_depth_mask(s_depth_mask ? 1 : 0);
        return;
    }

    if (use_texture && s_current->blends_by_texel_alpha) {
        /* The base in the caller's own state; the overlay blended over it by
         * the texel's alpha, without writing depth so that it lands exactly on
         * the base it covers. Then the caller's state goes back. */
        set_colours(count, COLOUR_BASE, NULL);
        ps5gpu_texture_select(s_white_texture);
        emit_batch(count, batch_fit, gamma, delta);

        set_colours(count, COLOUR_OVERLAY, NULL);
        ps5gpu_texture_select(s_unit_texture[0]);
        ps5gpu_set_blend(1);
        ps5gpu_set_depth_mask(0);
        emit_batch(count, batch_fit, gamma, delta);
        ps5gpu_set_blend(s_use_alpha ? 1 : 0);
        ps5gpu_set_depth_mask(s_depth_mask ? 1 : 0);
        if (shadow_receivable(false))
            draw_shadow_overlay(count, batch_fit, gamma, delta);
        return;
    }

    /* A combiner can leave the texel out altogether for a whole batch - the
     * shine on Mario's head in the title, (PRIM - SHADE) * TEXEL0 + SHADE,
     * wherever the shading has reached PRIM. The colour alone is then the
     * answer, so the batch is drawn untextured: multiplying it by the texel,
     * the nearly black shine texture, turned the white of his cap's badge
     * black whenever the light fell that way. */
    bool textured = use_texture && texel_counts && !mixed;
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float *average = white;
    if (use_texture && mixed) {
        TextureInfo *t = texture_info(s_unit_texture[0]);
        if (t) average = t->average;
    }
    set_colours(count, use_texture && mixed ? COLOUR_WITH_AVERAGE : COLOUR_FOR_WHITE_TEXEL, average);
    if (sky)
        for (size_t i = 0; i < count; i++) s_batch[i].a *= ps5_settings_sky_strength();

    /* An untextured draw borrows the white texel so the fixed "texture times
     * colour" program yields just the colour. The caller's own selection is
     * put back afterwards: it only re-selects when the texture changes, so
     * leaving white bound would silently mistexture the next draw. */
    /* Water and lava get their own programs, which animate by the time carried
     * in the vertices' spare normal component. */
    int program = 0;
    uint8_t material = MATERIAL_PLAIN;
    if (textured) {
        TextureInfo *t = texture_info(s_unit_texture[0]);
        if (t) { material = t->material; program = s_material_program[material]; }
    }
    if (program) {
        ps5gpu_set_program(program);
        s_npad = (float)(s_frame % 18000u) / 30.0f;   /* seconds, wrapping every ten minutes */
    }

    if (!textured) ps5gpu_texture_select(s_white_texture);
    if (cutout || sky) ps5gpu_set_blend(0);
    emit_batch(count, batch_fit, gamma, delta);
    if (cutout || sky) ps5gpu_set_blend(s_use_alpha ? 1 : 0);
    if (!textured) ps5gpu_texture_select(s_unit_texture[0]);

    if (material == MATERIAL_WATER && s_reflection_ready && !s_reflecting)
        draw_reflection(count, batch_fit, gamma, delta);

    if (material == MATERIAL_PLAIN && shadow_receivable(cutout))
        draw_shadow_overlay(count, batch_fit, gamma, delta);

    if (program) {
        ps5gpu_set_program(0);
        s_npad = 0.0f;
    }
}

static void gfx_agc_init(void) {
    /* A single white texel, bound whenever a draw uses no texture of its own,
     * so the fixed "texture times colour" program still yields just the
     * colour. */
    static const uint8_t white[4] = { 255, 255, 255, 255 };
    s_white_texture = ps5gpu_texture_new();
    ps5gpu_texture_select(s_white_texture);
    ps5gpu_texture_upload(s_white_texture, white, 1, 1);

    /* The caller starts from a zeroed record of the state it has set and only
     * reports changes from it, so the processor has to start from the same. */
    ps5gpu_set_depth_test(0);
    ps5gpu_set_depth_mask(0);
    ps5gpu_set_blend(0);

    /* The water and lava programs, and the textures that call for them. A
     * program that fails to build stays 0, the built-in one. */
    s_material_program[MATERIAL_PLAIN] = 0;
    s_material_program[MATERIAL_WATER] = ps5gpu_program_new(water_p_sb, water_p_sb_len);
    s_material_program[MATERIAL_LAVA] = ps5gpu_program_new(lava_p_sb, lava_p_sb_len);
    if (!s_material_program[MATERIAL_WATER] || !s_material_program[MATERIAL_LAVA])
        ps5gpu_notify("gfx_agc: no se pudo crear un programa de agua o lava");
    s_reflect_program = ps5gpu_program_new(water_reflect_p_sb, water_reflect_p_sb_len);
    s_reflect_program_1x = ps5gpu_program_new(water_reflect_1x_p_sb, water_reflect_1x_p_sb_len);
    if (!s_reflect_program)
        ps5gpu_notify("gfx_agc: no se pudo crear el programa de reflejos");
    s_shadow_depth_program = ps5gpu_program_new(shadow_depth_p_sb, shadow_depth_p_sb_len);
    s_shadow_receive_program = ps5gpu_program_new(shadow_recv_p_sb, shadow_recv_p_sb_len);
    if (!s_shadow_depth_program || !s_shadow_receive_program)
        ps5gpu_notify("gfx_agc: no se pudieron crear los programas de sombras");
    remember_material_texture(texture_waterbox_water, MATERIAL_WATER);
    remember_material_texture(texture_waterbox_jrb_water, MATERIAL_WATER);
    remember_material_texture(texture_waterbox_unknown_water, MATERIAL_WATER);
    remember_material_texture(texture_waterbox_lava, MATERIAL_LAVA);
}

static void gfx_agc_on_resize(void) { }
static void gfx_agc_start_frame(void) {
    s_frame++;
    s_reflecting = false;
    s_reflecting_sky = false;
    s_reflection_ready = false;
    s_shadowing = false;
    s_shadow_ready = false;
    s_light_attributes = false;
    ps5gpu_begin_frame();
}
static void gfx_agc_end_frame(void) { }
static void gfx_agc_finish_render(void) { }

struct GfxRenderingAPI gfx_agc_api = {
    gfx_agc_z_is_from_0_to_1,
    gfx_agc_unload_shader,
    gfx_agc_load_shader,
    gfx_agc_create_and_load_new_shader,
    gfx_agc_lookup_shader,
    gfx_agc_shader_get_info,
    gfx_agc_new_texture,
    gfx_agc_select_texture,
    gfx_agc_upload_texture,
    gfx_agc_set_sampler_parameters,
    gfx_agc_set_depth_test,
    gfx_agc_set_depth_mask,
    gfx_agc_set_zmode_decal,
    gfx_agc_set_viewport,
    gfx_agc_set_scissor,
    gfx_agc_set_use_alpha,
    gfx_agc_draw_triangles,
    gfx_agc_init,
    gfx_agc_on_resize,
    gfx_agc_start_frame,
    gfx_agc_end_frame,
    gfx_agc_finish_render,
};
