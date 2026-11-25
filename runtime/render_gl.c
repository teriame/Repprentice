/* OpenGL 3.x / GLX presenter for Linux & SteamOS: scales the 768x240 CD-i
   frame (or 1536x960 Cartoon HD frame) to any window or display resolution
   with selectable filters and aspect-ratio modes, including blurred side
   panels for 16:9 and 16:10 (Steam Deck 1280x800) displays. */
#ifndef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include "render.h"
#include "runtime.h"

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

typedef GLuint (*PFNGLCREATESHADERPROC)(GLenum);
typedef void (*PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (*PFNGLCOMPILESHADERPROC)(GLuint);
typedef void (*PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint *);
typedef void (*PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (*PFNGLDELETESHADERPROC)(GLuint);
typedef GLuint (*PFNGLCREATEPROGRAMPROC)(void);
typedef void (*PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void (*PFNGLLINKPROGRAMPROC)(GLuint);
typedef void (*PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint *);
typedef void (*PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (*PFNGLUSEPROGRAMPROC)(GLuint);
typedef GLint (*PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar *);
typedef void (*PFNGLUNIFORM4FPROC)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void (*PFNGLGENERATEMIPMAPPROC)(GLenum);
typedef void (*PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDVERTEXARRAYPROC)(GLuint);
typedef void (*PFNGLGENBUFFERSPROC)(GLsizei, GLuint *);
typedef void (*PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void (*PFNGLVERTEXATTRIBPOINTERPROC)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (*PFNGLBINDATTRIBLOCATIONPROC)(GLuint, GLuint, const GLchar *);

static PFNGLCREATESHADERPROC p_glCreateShader;
static PFNGLSHADERSOURCEPROC p_glShaderSource;
static PFNGLCOMPILESHADERPROC p_glCompileShader;
static PFNGLGETSHADERIVPROC p_glGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC p_glGetShaderInfoLog;
static PFNGLDELETESHADERPROC p_glDeleteShader;
static PFNGLCREATEPROGRAMPROC p_glCreateProgram;
static PFNGLATTACHSHADERPROC p_glAttachShader;
static PFNGLLINKPROGRAMPROC p_glLinkProgram;
static PFNGLGETPROGRAMIVPROC p_glGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC p_glGetProgramInfoLog;
static PFNGLUSEPROGRAMPROC p_glUseProgram;
static PFNGLGETUNIFORMLOCATIONPROC p_glGetUniformLocation;
static PFNGLUNIFORM4FPROC p_glUniform4f;
static PFNGLUNIFORM1IPROC p_glUniform1i;
static PFNGLGENERATEMIPMAPPROC p_glGenerateMipmap;
static PFNGLGENVERTEXARRAYSPROC p_glGenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray;
static PFNGLGENBUFFERSPROC p_glGenBuffers;
static PFNGLBINDBUFFERPROC p_glBindBuffer;
static PFNGLBUFFERDATAPROC p_glBufferData;
static PFNGLENABLEVERTEXATTRIBARRAYPROC p_glEnableVertexAttribArray;
static PFNGLVERTEXATTRIBPOINTERPROC p_glVertexAttribPointer;
static PFNGLBINDATTRIBLOCATIONPROC p_glBindAttribLocation;

static const char *aspect_names[] = {"4:3 + blurred sides", "4:3", "stretch to fill", "zoom to fill width"};
static const char *filter_names[] = {"sharp", "smooth", "CRT", "nearest"};
RenderSettings render_settings = {0, 0, 1};

const char *render_aspect_name(int a) { return aspect_names[a % RENDER_ASPECTS]; }
const char *render_filter_name(int f) { return filter_names[f % RENDER_FILTERS]; }

static Display *rdpy;
static int rdpy_owned;
static Window rwin;
static GLXContext rctx;
static GLuint prog, tex, vao, vbo;
static GLint u_rect, u_src, u_prm, u_uvx, u_up, u_tex;
static int tex_w, tex_h, tex_mips;
static volatile int win_cw = 1280, win_ch = 800;
static float cur_u0 = 0.0f, cur_v0 = 0.0f, cur_us = 1.0f, cur_vs = 1.0f;

extern int frontend_get_upscaler(void);

int render_gpu_active(void) {
    return rdpy != NULL && rctx != NULL;
}

void render_set_window_size(int w, int h) {
    if (w > 0) win_cw = w;
    if (h > 0) win_ch = h;
}

/* Detect symmetric black CD-i overscan borders around the active picture so the
   4:3 frame and its blurred 16:10/16:9 side panels sit flush against the screen
   edges without black bars at the top, left, or right. */
static void compute_active_uv(const uint32_t *fb, int w, int h,
                              float *u0, float *v0, float *us, float *vs) {
    /* Only crop CD-i game frames (768x240 original or 1536x960 Cartoon HD),
       never the 768x480 GUI launcher/overlay, and only when crop_borders is ON. */
    if (!render_settings.crop_borders || h == 480) {
        *u0 = 0.0f; *v0 = 0.0f; *us = 1.0f; *vs = 1.0f;
        return;
    }
    int max_top = h / 8;       /* up to 12.5% top/bottom border (30 lines of 240) */
    int max_side = w / 10;     /* up to 10% left/right border */
    int step_x = w >= 1536 ? 8 : 4;
    int step_y = h >= 960  ? 4 : 1;

    int top = 0;
    for (; top < max_top; top += step_y) {
        const uint32_t *row = fb + top * w;
        int hit = 0;
        for (int x = max_side; x < w - max_side; x += step_x) {
            if ((row[x] & 0xF8F8F8u) != 0) { hit = 1; break; }
        }
        if (hit) break;
    }
    int bot = 0;
    for (; bot < max_top; bot += step_y) {
        const uint32_t *row = fb + (h - 1 - bot) * w;
        int hit = 0;
        for (int x = max_side; x < w - max_side; x += step_x) {
            if ((row[x] & 0xF8F8F8u) != 0) { hit = 1; break; }
        }
        if (hit) break;
    }
    int left = 0;
    for (; left < max_side; left += step_x) {
        int hit = 0;
        for (int y = top; y < h - bot; y += step_y * 2) {
            if ((fb[y * w + left] & 0xF8F8F8u) != 0) { hit = 1; break; }
        }
        if (hit) break;
    }
    int right = 0;
    for (; right < max_side; right += step_x) {
        int hit = 0;
        int x = w - 1 - right;
        for (int y = top; y < h - bot; y += step_y * 2) {
            if ((fb[y * w + x] & 0xF8F8F8u) != 0) { hit = 1; break; }
        }
        if (hit) break;
    }
    /* Keep left/right border crop symmetric so the horizontal center never shifts */
    int side = left < right ? left : right;
    if (top >= max_top && bot >= max_top && side >= max_side) {
        /* Whole screen is dark (fade to black): keep previous crop */
        *u0 = cur_u0; *v0 = cur_v0; *us = cur_us; *vs = cur_vs;
        return;
    }
    float target_u0 = (float)side / (float)w;
    float target_v0 = (float)top / (float)h;
    float target_us = (float)(w - 2 * side) / (float)w;
    float target_vs = (float)(h - top - bot) / (float)h;
    if (target_us < 0.75f) { target_u0 = 0.0f; target_us = 1.0f; }
    if (target_vs < 0.75f) { target_v0 = 0.0f; target_vs = 1.0f; }

    /* Snap immediately on scene changes, smooth tiny 1-pixel oscillations */
    cur_u0 = target_u0;
    cur_v0 = target_v0;
    cur_us = target_us;
    cur_vs = target_vs;
    *u0 = cur_u0; *v0 = cur_v0; *us = cur_us; *vs = cur_vs;
}

static const char *vs_src =
    "#version 130\n"
    "in vec2 a_corner;\n"
    "out vec2 v_uv;\n"
    "uniform vec4 rect;\n"
    "uniform vec4 uvx;\n"
    "void main() {\n"
    "  gl_Position = vec4(mix(rect.x, rect.z, a_corner.x), mix(rect.y, rect.w, a_corner.y), 0.0, 1.0);\n"
    "  v_uv = uvx.xy + a_corner * uvx.zw;\n"
    "}\n";

static const char *fs_src =
    "#version 130\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 src;\n"
    "uniform vec4 prm;\n"
    "uniform vec4 up_prm;\n"
    /* sharp bilinear: crisp pixels without shimmer at non-integer scales */
    "vec3 sharp(vec2 uv) {\n"
    "  vec2 ts = src.xy;\n"
    "  vec2 scale = src.zw / ts;\n"
    "  vec2 texel = uv * ts;\n"
    "  vec2 pre = max(floor(scale), vec2(1.0));\n"
    "  vec2 fl = floor(texel);\n"
    "  vec2 cd = fract(texel) - 0.5;\n"
    "  vec2 rr = 0.5 - 0.5 / pre;\n"
    "  vec2 f = (cd - clamp(cd, -rr, rr)) * pre + 0.5;\n"
    "  return textureLod(tex, (fl + f) / ts, 0.0).rgb;\n"
    "}\n"
    /* Catmull-Rom bicubic with 9 bilinear taps */
    "vec3 bicubic(vec2 uv) {\n"
    "  vec2 ts = src.xy;\n"
    "  vec2 sp2 = uv * ts;\n"
    "  vec2 t1 = floor(sp2 - 0.5) + 0.5;\n"
    "  vec2 f = sp2 - t1;\n"
    "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "  vec2 w3 = f * f * (-0.5 + 0.5 * f);\n"
    "  vec2 w12 = w1 + w2;\n"
    "  vec2 o12 = w2 / w12;\n"
    "  vec2 t0 = (t1 - 1.0) / ts, t3 = (t1 + 2.0) / ts, t12 = (t1 + o12) / ts;\n"
    "  vec3 r = vec3(0.0);\n"
    "  r += textureLod(tex, vec2(t0.x, t0.y), 0.0).rgb * w0.x * w0.y;\n"
    "  r += textureLod(tex, vec2(t12.x, t0.y), 0.0).rgb * w12.x * w0.y;\n"
    "  r += textureLod(tex, vec2(t3.x, t0.y), 0.0).rgb * w3.x * w0.y;\n"
    "  r += textureLod(tex, vec2(t0.x, t12.y), 0.0).rgb * w0.x * w12.y;\n"
    "  r += textureLod(tex, vec2(t12.x, t12.y), 0.0).rgb * w12.x * w12.y;\n"
    "  r += textureLod(tex, vec2(t3.x, t12.y), 0.0).rgb * w3.x * w12.y;\n"
    "  r += textureLod(tex, vec2(t0.x, t3.y), 0.0).rgb * w0.x * w3.y;\n"
    "  r += textureLod(tex, vec2(t12.x, t3.y), 0.0).rgb * w12.x * w3.y;\n"
    "  r += textureLod(tex, vec2(t3.x, t3.y), 0.0).rgb * w3.x * w3.y;\n"
    "  return clamp(r, 0.0, 1.0);\n"
    "}\n"
    /* nearest-neighbour sampling */
    "vec3 nearest_sample(vec2 uv) {\n"
    "  vec2 ts = src.xy;\n"
    "  vec2 tc = (floor(uv * ts) + 0.5) / ts;\n"
    "  return textureLod(tex, tc, 0.0).rgb;\n"
    "}\n"
    /* GPU Cartoon HD / Cartoon HD+ (xBR edge-directed scaling + inking + adaptive sharpen) */
    "vec3 fpx(ivec2 p, ivec2 ilts, int sx) {\n"
    "  ivec2 cp = clamp(p, ivec2(0), ilts - ivec2(1));\n"
    "  return texelFetch(tex, ivec2(cp.x * sx, cp.y), 0).rgb;\n"
    "}\n"
    "vec3 to_yuv(vec3 c) {\n"
    "  return vec3(\n"
    "    dot(c, vec3(0.299, 0.587, 0.114)),\n"
    "    dot(c, vec3(-0.169, -0.331, 0.500)),\n"
    "    dot(c, vec3(0.500, -0.419, -0.081))\n"
    "  );\n"
    "}\n"
    "float ydist(vec3 a, vec3 b) {\n"
    "  vec3 d = abs(a - b);\n"
    "  return 48.0 * d.x + 7.0 * d.y + 6.0 * d.z;\n"
    "}\n"
    "bool neq(vec3 a, vec3 b) {\n"
    "  return any(greaterThan(abs(a - b), vec3(0.003)));\n"
    "}\n"
    "vec3 xbr_corner(vec3 res, float u, float v, float aa,\n"
    "                vec3 cE, vec3 cB, vec3 cC, vec3 cD, vec3 cF, vec3 cG, vec3 cH,\n"
    "                vec3 qE, vec3 qB, vec3 qC, vec3 qD, vec3 qF, vec3 qG, vec3 qH, vec3 qI,\n"
    "                vec3 qF4, vec3 qI4, vec3 qH5, vec3 qI5) {\n"
    "  float e = ydist(qE, qC) + ydist(qE, qG) + ydist(qI, qF4) + ydist(qI, qH5) + 4.0 * ydist(qH, qF);\n"
    "  float i = ydist(qH, qD) + ydist(qH, qI5) + ydist(qF, qI4) + ydist(qF, qB) + 4.0 * ydist(qE, qI);\n"
    "  if (e >= i) return res;\n"
    "  vec3 px = (ydist(qE, qF) <= ydist(qE, qH)) ? cF : cH;\n"
    "  float ke = ydist(qF, qG), ki = ydist(qH, qC);\n"
    "  bool shallow = (2.0 * ke <= ki) && neq(cE, cG) && neq(cD, cG);\n"
    "  bool steep   = (2.0 * ki <= ke) && neq(cE, cC) && neq(cB, cC);\n"
    "  float d;\n"
    "  if (shallow && !steep) d = 0.5 * u + v - 1.0;\n"
    "  else if (steep && !shallow) d = u + 0.5 * v - 1.0;\n"
    "  else d = u + v - 1.5;\n"
    "  float w = clamp(d * aa + 0.5, 0.0, 1.0);\n"
    "  return mix(res, px, w);\n"
    "}\n"
    "vec3 xbr_cartoon(vec2 uv, bool hdplus) {\n"
    "  vec2 lts = up_prm.yz;\n"
    "  ivec2 ilts = ivec2(int(lts.x + 0.5), int(lts.y + 0.5));\n"
    "  int sx = int(up_prm.w + 0.5);\n"
    "  vec2 pos = uv * lts;\n"
    "  ivec2 ip = ivec2(floor(pos));\n"
    "  vec2 fp = pos - vec2(ip);\n"
    "  vec3 cA = fpx(ip + ivec2(-1, -1), ilts, sx);\n"
    "  vec3 cB = fpx(ip + ivec2( 0, -1), ilts, sx);\n"
    "  vec3 cC = fpx(ip + ivec2( 1, -1), ilts, sx);\n"
    "  vec3 cD = fpx(ip + ivec2(-1,  0), ilts, sx);\n"
    "  vec3 cE = fpx(ip,                 ilts, sx);\n"
    "  vec3 cF = fpx(ip + ivec2( 1,  0), ilts, sx);\n"
    "  vec3 cG = fpx(ip + ivec2(-1,  1), ilts, sx);\n"
    "  vec3 cH = fpx(ip + ivec2( 0,  1), ilts, sx);\n"
    "  vec3 cI = fpx(ip + ivec2( 1,  1), ilts, sx);\n"
    "  if (!neq(cE, cA) && !neq(cE, cB) && !neq(cE, cC) &&\n"
    "      !neq(cE, cD) && !neq(cE, cF) &&\n"
    "      !neq(cE, cG) && !neq(cE, cH) && !neq(cE, cI)) {\n"
    "    return cE;\n"
    "  }\n"
    "  vec3 qA = to_yuv(cA), qB = to_yuv(cB), qC = to_yuv(cC);\n"
    "  vec3 qD = to_yuv(cD), qE = to_yuv(cE), qF = to_yuv(cF);\n"
    "  vec3 qG = to_yuv(cG), qH = to_yuv(cH), qI = to_yuv(cI);\n"
    "  vec3 qA1 = to_yuv(fpx(ip + ivec2(-1, -2), ilts, sx));\n"
    "  vec3 qB1 = to_yuv(fpx(ip + ivec2( 0, -2), ilts, sx));\n"
    "  vec3 qC1 = to_yuv(fpx(ip + ivec2( 1, -2), ilts, sx));\n"
    "  vec3 qA0 = to_yuv(fpx(ip + ivec2(-2, -1), ilts, sx));\n"
    "  vec3 qC4 = to_yuv(fpx(ip + ivec2( 2, -1), ilts, sx));\n"
    "  vec3 qD0 = to_yuv(fpx(ip + ivec2(-2,  0), ilts, sx));\n"
    "  vec3 qF4 = to_yuv(fpx(ip + ivec2( 2,  0), ilts, sx));\n"
    "  vec3 qG0 = to_yuv(fpx(ip + ivec2(-2,  1), ilts, sx));\n"
    "  vec3 qI4 = to_yuv(fpx(ip + ivec2( 2,  1), ilts, sx));\n"
    "  vec3 qG5 = to_yuv(fpx(ip + ivec2(-1,  2), ilts, sx));\n"
    "  vec3 qH5 = to_yuv(fpx(ip + ivec2( 0,  2), ilts, sx));\n"
    "  vec3 qI5 = to_yuv(fpx(ip + ivec2( 1,  2), ilts, sx));\n"
    "  float aa = clamp(src.w / lts.y, 2.0, 6.0);\n"
    "  vec3 res = cE;\n"
    "  /* r=0: Bottom-Right */"
    "  res = xbr_corner(res, fp.x, fp.y, aa, cE, cB, cC, cD, cF, cG, cH, qE, qB, qC, qD, qF, qG, qH, qI, qF4, qI4, qH5, qI5);\n"
    "  /* r=1: Bottom-Left */"
    "  res = xbr_corner(res, fp.y, 1.0 - fp.x, aa, cE, cF, cI, cB, cH, cA, cD, qE, qF, qI, qB, qH, qA, qD, qG, qH5, qG5, qD0, qG0);\n"
    "  /* r=2: Top-Left */"
    "  res = xbr_corner(res, 1.0 - fp.x, 1.0 - fp.y, aa, cE, cH, cG, cF, cD, cC, cB, qE, qH, qG, qF, qD, qC, qB, qA, qD0, qA0, qB1, qA1);\n"
    "  /* r=3: Top-Right */"
    "  res = xbr_corner(res, 1.0 - fp.y, fp.x, aa, cE, cD, cA, cH, cB, cI, cF, qE, qD, qA, qH, qB, qI, qF, qC, qB1, qC1, qF4, qC4);\n"
    "  if (hdplus) {\n"
    "    float mean_y = (qA.x + qB.x + qC.x + qD.x + qE.x + qF.x + qG.x + qH.x + qI.x +\n"
    "                    qA1.x + qB1.x + qC1.x + qA0.x + qC4.x + qD0.x + qF4.x + qG0.x + qI4.x + qG5.x + qH5.x + qI5.x) / 21.0;\n"
    "    float cy = dot(res, vec3(0.299, 0.587, 0.114));\n"
    "    float d = mean_y - cy - (10.0 / 255.0);\n"
    "    if (d > 0.0) {\n"
    "      float f = min(d * 3.0, 77.0 / 256.0);\n"
    "      res *= (1.0 - f);\n"
    "    }\n"
    "    float mn = min(min(qB.x, qH.x), min(qD.x, qF.x));\n"
    "    float mx = max(max(qB.x, qH.x), max(qD.x, qF.x));\n"
    "    float contrast = mx - mn;\n"
    "    if (contrast > 0.002) {\n"
    "      float k = clamp((6.0 - contrast * 8.0) / 16.0, 1.0 / 16.0, 6.0 / 16.0);\n"
    "      vec3 m4 = 0.25 * (cB + cH + cD + cF);\n"
    "      res = clamp(res + (res - m4) * k, 0.0, 1.0);\n"
    "    }\n"
    "  }\n"
    "  return res;\n"
    "}\n"
    /* simple CRT: gaussian scanlines + aperture grille */
    "vec3 crt_effect(vec3 a, vec2 uv, vec2 pos) {\n"
    "  float ty = uv.y * src.y;\n"
    "  float yc = floor(ty) + 0.5;\n"
    "  float d = ty - yc;\n"
    "  float lum = dot(a, vec3(0.3, 0.6, 0.1));\n"
    "  float width = mix(6.0, 2.5, lum);\n"
    "  float beam = exp(-d * d * width * 4.0);\n"
    "  float strength = clamp((src.w / src.y - 2.5) / 4.0, 0.0, 1.0);\n"
    "  vec3 c = a * mix(1.0, beam * 1.35, strength);\n"
    "  int m = int(pos.x) % 3;\n"
    "  vec3 mask = m == 0 ? vec3(1.0, 0.85, 0.85) : (m == 1 ? vec3(0.85, 1.0, 0.85) : vec3(0.85, 0.85, 1.0));\n"
    "  mask = mix(vec3(1.0), mask, strength);\n"
    "  return clamp(c * mask * mix(1.0, 1.1, strength), 0.0, 1.0);\n"
    "}\n"
    "vec3 crt(vec2 uv, vec2 pos) {\n"
    "  float ty = uv.y * src.y;\n"
    "  float yc = floor(ty) + 0.5;\n"
    "  vec3 a = sharp(vec2(uv.x, yc / src.y));\n"
    "  return crt_effect(a, uv, pos);\n"
    "}\n"
    "void main() {\n"
    "  if (prm.w > 0.5) {\n"
    "    vec2 st = exp2(prm.y) / src.xy;\n"
    "    vec3 b = vec3(0.0); float wsum = 0.0;\n"
    "    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
    "      float w = exp(-float(x * x + y * y) / 4.0);\n"
    "      b += textureLod(tex, v_uv + vec2(float(x), float(y)) * st, prm.y).rgb * w;\n"
    "      wsum += w;\n"
    "    }\n"
    "    fragColor = vec4(b / wsum * prm.z, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  int f = int(prm.x + 0.5);\n"
    "  int up = int(up_prm.x + 0.5);\n"
    "  vec3 c;\n"
    "  if (up > 0) {\n"
    "    c = xbr_cartoon(v_uv, up >= 2);\n"
    "    if (f == 2) c = crt_effect(c, v_uv, gl_FragCoord.xy);\n"
    "  } else {\n"
    "    if (f == 0) c = sharp(v_uv);\n"
    "    else if (f == 1) c = bicubic(v_uv);\n"
    "    else if (f == 2) c = crt(v_uv, gl_FragCoord.xy);\n"
    "    else c = nearest_sample(v_uv);\n"
    "  }\n"
    "  fragColor = vec4(c, 1.0);\n"
    "}\n";

static int load_gl_procs(void) {
#define LOAD(fn, type) do { \
    p_##fn = (type)glXGetProcAddressARB((const GLubyte *)#fn); \
    if (!p_##fn) return -1; \
} while (0)
    LOAD(glCreateShader, PFNGLCREATESHADERPROC);
    LOAD(glShaderSource, PFNGLSHADERSOURCEPROC);
    LOAD(glCompileShader, PFNGLCOMPILESHADERPROC);
    LOAD(glGetShaderiv, PFNGLGETSHADERIVPROC);
    LOAD(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC);
    LOAD(glDeleteShader, PFNGLDELETESHADERPROC);
    LOAD(glCreateProgram, PFNGLCREATEPROGRAMPROC);
    LOAD(glAttachShader, PFNGLATTACHSHADERPROC);
    LOAD(glLinkProgram, PFNGLLINKPROGRAMPROC);
    LOAD(glGetProgramiv, PFNGLGETPROGRAMIVPROC);
    LOAD(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC);
    LOAD(glUseProgram, PFNGLUSEPROGRAMPROC);
    LOAD(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC);
    LOAD(glUniform4f, PFNGLUNIFORM4FPROC);
    LOAD(glUniform1i, PFNGLUNIFORM1IPROC);
    LOAD(glGenerateMipmap, PFNGLGENERATEMIPMAPPROC);
    LOAD(glGenBuffers, PFNGLGENBUFFERSPROC);
    LOAD(glBindBuffer, PFNGLBINDBUFFERPROC);
    LOAD(glBufferData, PFNGLBUFFERDATAPROC);
    LOAD(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC);
    LOAD(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC);
    LOAD(glBindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC);
#undef LOAD
    p_glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)glXGetProcAddressARB((const GLubyte *)"glGenVertexArrays");
    p_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)glXGetProcAddressARB((const GLubyte *)"glBindVertexArray");
    return 0;
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        LOG("OpenGL shader compile failed: %s", log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

int render_init_x11(void *dpy_ptr, unsigned long win_id) {
    /* Tell Mesa / XWayland not to block inside glXSwapBuffers so host_present
       can pace cleanly at FIELD_RATE without double-waiting on VSync. */
    setenv("vblank_mode", "0", 1);
    setenv("__GL_SYNC_TO_VBLANK", "0", 1);
    XSync((Display *)dpy_ptr, False);
    rdpy = XOpenDisplay(NULL);
    if (rdpy) {
        rdpy_owned = 1;
    } else {
        rdpy = (Display *)dpy_ptr;
        rdpy_owned = 0;
    }
    rwin = (Window)win_id;
    XWindowAttributes wa;
    if (XGetWindowAttributes(rdpy, rwin, &wa)) {
        if (wa.width > 0) win_cw = wa.width;
        if (wa.height > 0) win_ch = wa.height;
    }
    int screen = DefaultScreen(rdpy);
    int attribs[] = {
        GLX_RGBA,
        GLX_DOUBLEBUFFER,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        None
    };
    XVisualInfo *vi = glXChooseVisual(rdpy, screen, attribs);
    if (!vi) {
        LOG("glXChooseVisual failed; falling back to X11 software presenter");
        return -1;
    }
    rctx = glXCreateContext(rdpy, vi, NULL, True);
    XFree(vi);
    if (!rctx || !glXMakeCurrent(rdpy, rwin, rctx)) {
        LOG("glXCreateContext / glXMakeCurrent failed; falling back to X11 software presenter");
        if (rctx) { glXDestroyContext(rdpy, rctx); rctx = NULL; }
        return -1;
    }
    if (load_gl_procs() != 0) {
        LOG("OpenGL 3.0 functions unavailable; falling back to X11 software presenter");
        glXMakeCurrent(rdpy, None, NULL);
        glXDestroyContext(rdpy, rctx);
        rctx = NULL;
        return -1;
    }
    /* disable blocking VSync since host_present paces frames at FIELD_RATE */
    typedef void (*PFNGLXSWAPINTERVALEXTPROC)(Display *, GLXDrawable, int);
    typedef int (*PFNGLXSWAPINTERVALMESAPROC)(unsigned int);
    typedef int (*PFNGLXSWAPINTERVALSGIPROC)(int);
    PFNGLXSWAPINTERVALEXTPROC swap_ext = (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    PFNGLXSWAPINTERVALMESAPROC swap_mesa = (PFNGLXSWAPINTERVALMESAPROC)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalMESA");
    PFNGLXSWAPINTERVALSGIPROC swap_sgi = (PFNGLXSWAPINTERVALSGIPROC)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalSGI");
    if (swap_mesa) swap_mesa(0);
    if (swap_ext) swap_ext(rdpy, rwin, 0);
    if (swap_sgi) swap_sgi(0);

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return -1;
    prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    p_glBindAttribLocation(prog, 0, "a_corner");
    p_glLinkProgram(prog);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        p_glGetProgramInfoLog(prog, sizeof log, NULL, log);
        LOG("OpenGL program link failed: %s", log);
        return -1;
    }
    u_rect = p_glGetUniformLocation(prog, "rect");
    u_src  = p_glGetUniformLocation(prog, "src");
    u_prm  = p_glGetUniformLocation(prog, "prm");
    u_uvx  = p_glGetUniformLocation(prog, "uvx");
    u_up   = p_glGetUniformLocation(prog, "up_prm");
    u_tex  = p_glGetUniformLocation(prog, "tex");

    static const float corners[8] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f
    };
    if (p_glGenVertexArrays && p_glBindVertexArray) {
        p_glGenVertexArrays(1, &vao);
        p_glBindVertexArray(vao);
    }
    p_glGenBuffers(1, &vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (const void *)0);

    const char *gl_ver = (const char *)glGetString(GL_VERSION);
    const char *gl_ren = (const char *)glGetString(GL_RENDERER);
    LOG("renderer: OpenGL (%s, %s)", gl_ver ? gl_ver : "?", gl_ren ? gl_ren : "?");
    return 0;
}

void render_bind_thread(void) {
    if (rdpy && rctx) glXMakeCurrent(rdpy, rwin, rctx);
}

void render_unbind_thread(void) {
    if (rdpy && rctx) glXMakeCurrent(rdpy, None, NULL);
}

void render_shutdown(void) {
    if (rdpy && rctx) {
        glXMakeCurrent(rdpy, None, NULL);
        glXDestroyContext(rdpy, rctx);
        rctx = NULL;
    }
    if (rdpy && rdpy_owned) {
        XCloseDisplay(rdpy);
        rdpy = NULL;
        rdpy_owned = 0;
    }
}

static int ensure_texture(int w, int h) {
    if (tex && w == tex_w && h == tex_h) return 0;
    if (tex) { glDeleteTextures(1, &tex); tex = 0; }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 3);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    p_glGenerateMipmap(GL_TEXTURE_2D);
    tex_w = w;
    tex_h = h;
    tex_mips = 4;
    return 0;
}

static void draw(float l, float t, float r, float b, float u0, float v0, float us, float vs_, int backdrop,
                 float mip, float bright, float dw, float dh,
                 int up_mode, float lw, float lh, float sx) {
    p_glUniform4f(u_rect, l, t, r, b);
    p_glUniform4f(u_src, (float)tex_w, (float)tex_h, dw, dh);
    p_glUniform4f(u_prm, (float)render_settings.filter, mip, bright, (float)backdrop);
    p_glUniform4f(u_uvx, u0, v0, us, vs_);
    p_glUniform4f(u_up, (float)up_mode, lw, lh, sx);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

int render_present(const uint32_t *fb, int w, int h) {
    if (!rdpy || !rctx) return -1;
    int cw = win_cw > 0 ? win_cw : 1;
    int ch = win_ch > 0 ? win_ch : 1;

    if (ensure_texture(w, h)) return -1;
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, fb);

    /* Detect whether a 768x240 CD-i game frame uses normal resolution (384x240
       with doubled pixel pairs) or high resolution (768x240), and check whether
       GPU Cartoon HD (1) or Cartoon HD+ (2) should run on this frame. */
    int up_mode = 0;
    float lw = (float)w, lh = (float)h, sx = 1.0f;
    if (w == 768 && h == 240) {
        static int hires_state = 0, normal_run = 0;
        int differ = 0, samples = 0;
        for (int i = 0; i < w * h; i += 16) {
            differ += (fb[i] != fb[i + 1]);
            samples++;
        }
        if (differ * 250 >= samples) { hires_state = 1; normal_run = 0; }
        else if (differ * 1000 < samples && ++normal_run >= 30) hires_state = 0;
        if (!hires_state) { lw = 384.0f; sx = 2.0f; }
        up_mode = frontend_get_upscaler();
    }

    double win_aspect = (double)cw / ch, aspect = 4.0 / 3.0;
    double pw, ph;
    int mode = render_settings.aspect % RENDER_ASPECTS;
    if (mode == 2) { pw = cw; ph = ch; }
    else if (mode == 3) { pw = cw; ph = cw / aspect; if (ph < ch) { ph = ch; pw = ch * aspect; } }
    else if (win_aspect > aspect) { ph = ch; pw = ch * aspect; }
    else { pw = cw; ph = cw / aspect; }
    float l = (float)(-pw / cw), r = (float)(pw / cw), t = (float)(ph / ch), b = (float)(-ph / ch);
    float u0 = 0.0f, v0 = 0.0f, us0 = 1.0f, vs0 = 1.0f;
    compute_active_uv(fb, w, h, &u0, &v0, &us0, &vs0);

    if (mode == 0 && pw < cw - 1) {
        p_glGenerateMipmap(GL_TEXTURE_2D);
    }

    glViewport(0, 0, cw, ch);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    p_glUseProgram(prog);
    p_glUniform1i(u_tex, 0);
    if (p_glBindVertexArray && vao) p_glBindVertexArray(vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (const void *)0);

    /* Note: in OpenGL NDC, y=+1 is top and y=-1 is bottom, whereas fb row 0 is
       the top scanline (v = v0).  Passing t (+ph/ch) as rect.y and b (-ph/ch)
       as rect.w with a_corner.y=0 at the top and a_corner.y=1 at the bottom
       maps row 0 to the top of the screen and row h-1 to the bottom. */
    if (mode == 0 && pw < cw - 1) {
        double cover_w = cw, cover_h = cw / aspect;
        if (cover_h < ch) { cover_h = ch; cover_w = ch * aspect; }
        float us = (float)(cw / cover_w) * us0, vs2 = (float)(ch / cover_h) * vs0;
        float bu0 = u0 + (us0 - us) * 0.5f;
        float bv0 = v0 + (vs0 - vs2) * 0.5f;
        float mip = (float)(tex_mips > 3 ? 3 : tex_mips - 1);
        draw(-1.0f, 1.0f, 1.0f, -1.0f, bu0, bv0, us, vs2, 1, mip, 0.5f, (float)cw, (float)ch, 0, lw, lh, sx);
    }
    draw(l, t, r, b, u0, v0, us0, vs0, 0, 0.0f, 1.0f, (float)(pw / us0), (float)(ph / vs0), up_mode, lw, lh, sx);
    glXSwapBuffers(rdpy, rwin);
    return 0;
}
#endif
