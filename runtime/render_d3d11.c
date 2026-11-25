/* Direct3D 11 presenter: scales the 768x240 CD-i frame to any window or
   monitor resolution (up to 4K and beyond) with selectable filters and
   aspect-ratio modes, including blurred side panels for 16:9 displays. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include "render.h"
#include "runtime.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swap;
static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *tex;
static ID3D11ShaderResourceView *srv;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11Buffer *cbuf;
static ID3D11SamplerState *samp_lin, *samp_point;
static int tex_w, tex_h, tex_mips;
static UINT buf_w, buf_h;
static HWND rwnd;

typedef struct {
    float rect[4];   /* destination rect in NDC: left, top, right, bottom */
    float src[4];    /* texture w, h, destination w, h (pixels) */
    float prm[4];    /* filter, mip level for backdrop, brightness, mode (0 image, 1 backdrop) */
    float uvx[4];    /* uv offset xy, uv scale zw */
    float up[4];     /* upscaler mode (0 off, 1 HD, 2 HD+), logical w, logical h, step_x */
} CB;

extern int frontend_get_upscaler(void);

int render_gpu_active(void) {
    return dev != NULL;
}

static float cur_u0 = 0.0f, cur_v0 = 0.0f, cur_us = 1.0f, cur_vs = 1.0f;

/* Detect symmetric black CD-i overscan borders around the active picture so the
   4:3 frame and its blurred 16:10/16:9 side panels sit flush against the screen
   edges without black bars at the top, left, or right. */
static void compute_active_uv(const uint32_t *fb, int w, int h,
                              float *u0, float *v0, float *us, float *vs) {
    if (!render_settings.crop_borders || h == 480) {
        *u0 = 0.0f; *v0 = 0.0f; *us = 1.0f; *vs = 1.0f;
        return;
    }
    int max_top = h / 8;
    int max_side = w / 10;
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
    int side = left < right ? left : right;
    if (top >= max_top && bot >= max_top && side >= max_side) {
        *u0 = cur_u0; *v0 = cur_v0; *us = cur_us; *vs = cur_vs;
        return;
    }
    float target_u0 = (float)side / (float)w;
    float target_v0 = (float)top / (float)h;
    float target_us = (float)(w - 2 * side) / (float)w;
    float target_vs = (float)(h - top - bot) / (float)h;
    if (target_us < 0.75f) { target_u0 = 0.0f; target_us = 1.0f; }
    if (target_vs < 0.75f) { target_v0 = 0.0f; target_vs = 1.0f; }

    cur_u0 = target_u0;
    cur_v0 = target_v0;
    cur_us = target_us;
    cur_vs = target_vs;
    *u0 = cur_u0; *v0 = cur_v0; *us = cur_us; *vs = cur_vs;
}

static const char *shader_src =
"Texture2D tex : register(t0);\n"
"SamplerState sl : register(s0);\n"
"SamplerState sp : register(s1);\n"
"cbuffer C : register(b0) { float4 rect; float4 src; float4 prm; float4 uvx; float4 up_prm; };\n"
"struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VO vsmain(uint id : SV_VertexID) {\n"
"  float2 c = float2(id & 1, id >> 1);\n"
"  VO o;\n"
"  o.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0, 1);\n"
"  o.uv = uvx.xy + c * uvx.zw;\n"
"  return o;\n"
"}\n"
/* sharp bilinear: crisp pixels without shimmer at non-integer scales */
"float3 sharp(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 scale = src.zw / ts;\n"
"  float2 texel = uv * ts;\n"
"  float2 pre = max(floor(scale), 1.0);\n"
"  float2 fl = floor(texel);\n"
"  float2 cd = frac(texel) - 0.5;\n"
"  float2 rr = 0.5 - 0.5 / pre;\n"
"  float2 f = (cd - clamp(cd, -rr, rr)) * pre + 0.5;\n"
"  return tex.SampleLevel(sl, (fl + f) / ts, 0).rgb;\n"
"}\n"
/* Catmull-Rom bicubic with 9 bilinear taps */
"float3 bicubic(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 sp2 = uv * ts;\n"
"  float2 t1 = floor(sp2 - 0.5) + 0.5;\n"
"  float2 f = sp2 - t1;\n"
"  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
"  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
"  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
"  float2 w12 = w1 + w2;\n"
"  float2 o12 = w2 / w12;\n"
"  float2 t0 = (t1 - 1) / ts, t3 = (t1 + 2) / ts, t12 = (t1 + o12) / ts;\n"
"  float3 r = 0;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;\n"
"  return saturate(r);\n"
"}\n"
/* GPU Cartoon HD / Cartoon HD+ (xBR edge-directed scaling + inking + adaptive sharpen) */
"float3 fpx(int2 p, int2 ilts, int sx) {\n"
"  int2 cp = clamp(p, int2(0, 0), ilts - int2(1, 1));\n"
"  return tex.Load(int3(cp.x * sx, cp.y, 0)).rgb;\n"
"}\n"
"float3 to_yuv(float3 c) {\n"
"  return float3(\n"
"    dot(c, float3(0.299, 0.587, 0.114)),\n"
"    dot(c, float3(-0.169, -0.331, 0.500)),\n"
"    dot(c, float3(0.500, -0.419, -0.081))\n"
"  );\n"
"}\n"
"float ydist(float3 a, float3 b) {\n"
"  float3 d = abs(a - b);\n"
"  return 48.0 * d.x + 7.0 * d.y + 6.0 * d.z;\n"
"}\n"
"bool neq(float3 a, float3 b) {\n"
"  return any(abs(a - b) > 0.003);\n"
"}\n"
"float3 xbr_corner(float3 res, float u, float v, float aa,\n"
"                  float3 cE, float3 cB, float3 cC, float3 cD, float3 cF, float3 cG, float3 cH,\n"
"                  float3 qE, float3 qB, float3 qC, float3 qD, float3 qF, float3 qG, float3 qH, float3 qI,\n"
"                  float3 qF4, float3 qI4, float3 qH5, float3 qI5) {\n"
"  float e = ydist(qE, qC) + ydist(qE, qG) + ydist(qI, qF4) + ydist(qI, qH5) + 4.0 * ydist(qH, qF);\n"
"  float i = ydist(qH, qD) + ydist(qH, qI5) + ydist(qF, qI4) + ydist(qF, qB) + 4.0 * ydist(qE, qI);\n"
"  if (e >= i) return res;\n"
"  float3 px = (ydist(qE, qF) <= ydist(qE, qH)) ? cF : cH;\n"
"  float ke = ydist(qF, qG), ki = ydist(qH, qC);\n"
"  bool shallow = (2.0 * ke <= ki) && neq(cE, cG) && neq(cD, cG);\n"
"  bool steep   = (2.0 * ki <= ke) && neq(cE, cC) && neq(cB, cC);\n"
"  float d;\n"
"  if (shallow && !steep) d = 0.5 * u + v - 1.0;\n"
"  else if (steep && !shallow) d = u + 0.5 * v - 1.0;\n"
"  else d = u + v - 1.5;\n"
"  float w = saturate(d * aa + 0.5);\n"
"  return lerp(res, px, w);\n"
"}\n"
"float3 xbr_cartoon(float2 uv, bool hdplus) {\n"
"  float2 lts = up_prm.yz;\n"
"  int2 ilts = int2((int)(lts.x + 0.5), (int)(lts.y + 0.5));\n"
"  int sx = (int)(up_prm.w + 0.5);\n"
"  float2 pos = uv * lts;\n"
"  int2 ip = (int2)floor(pos);\n"
"  float2 fp = pos - (float2)ip;\n"
"  float3 cA = fpx(ip + int2(-1, -1), ilts, sx);\n"
"  float3 cB = fpx(ip + int2( 0, -1), ilts, sx);\n"
"  float3 cC = fpx(ip + int2( 1, -1), ilts, sx);\n"
"  float3 cD = fpx(ip + int2(-1,  0), ilts, sx);\n"
"  float3 cE = fpx(ip,                ilts, sx);\n"
"  float3 cF = fpx(ip + int2( 1,  0), ilts, sx);\n"
"  float3 cG = fpx(ip + int2(-1,  1), ilts, sx);\n"
"  float3 cH = fpx(ip + int2( 0,  1), ilts, sx);\n"
"  float3 cI = fpx(ip + int2( 1,  1), ilts, sx);\n"
"  if (!neq(cE, cA) && !neq(cE, cB) && !neq(cE, cC) &&\n"
"      !neq(cE, cD) && !neq(cE, cF) &&\n"
"      !neq(cE, cG) && !neq(cE, cH) && !neq(cE, cI)) {\n"
"    return cE;\n"
"  }\n"
"  float3 qA = to_yuv(cA), qB = to_yuv(cB), qC = to_yuv(cC);\n"
"  float3 qD = to_yuv(cD), qE = to_yuv(cE), qF = to_yuv(cF);\n"
"  float3 qG = to_yuv(cG), qH = to_yuv(cH), qI = to_yuv(cI);\n"
"  float3 qA1 = to_yuv(fpx(ip + int2(-1, -2), ilts, sx));\n"
"  float3 qB1 = to_yuv(fpx(ip + int2( 0, -2), ilts, sx));\n"
"  float3 qC1 = to_yuv(fpx(ip + int2( 1, -2), ilts, sx));\n"
"  float3 qA0 = to_yuv(fpx(ip + int2(-2, -1), ilts, sx));\n"
"  float3 qC4 = to_yuv(fpx(ip + int2( 2, -1), ilts, sx));\n"
"  float3 qD0 = to_yuv(fpx(ip + int2(-2,  0), ilts, sx));\n"
"  float3 qF4 = to_yuv(fpx(ip + int2( 2,  0), ilts, sx));\n"
"  float3 qG0 = to_yuv(fpx(ip + int2(-2,  1), ilts, sx));\n"
"  float3 qI4 = to_yuv(fpx(ip + int2( 2,  1), ilts, sx));\n"
"  float3 qG5 = to_yuv(fpx(ip + int2(-1,  2), ilts, sx));\n"
"  float3 qH5 = to_yuv(fpx(ip + int2( 0,  2), ilts, sx));\n"
"  float3 qI5 = to_yuv(fpx(ip + int2( 1,  2), ilts, sx));\n"
"  float aa = clamp(src.w / lts.y, 2.0, 6.0);\n"
"  float3 res = cE;\n"
"  res = xbr_corner(res, fp.x, fp.y, aa, cE, cB, cC, cD, cF, cG, cH, qE, qB, qC, qD, qF, qG, qH, qI, qF4, qI4, qH5, qI5);\n"
"  res = xbr_corner(res, fp.y, 1.0 - fp.x, aa, cE, cF, cI, cB, cH, cA, cD, qE, qF, qI, qB, qH, qA, qD, qG, qH5, qG5, qD0, qG0);\n"
"  res = xbr_corner(res, 1.0 - fp.x, 1.0 - fp.y, aa, cE, cH, cG, cF, cD, cC, cB, qE, qH, qG, qF, qD, qC, qB, qA, qD0, qA0, qB1, qA1);\n"
"  res = xbr_corner(res, 1.0 - fp.y, fp.x, aa, cE, cD, cA, cH, cB, cI, cF, qE, qD, qA, qH, qB, qI, qF, qC, qB1, qC1, qF4, qC4);\n"
"  if (hdplus) {\n"
"    float mean_y = (qA.x + qB.x + qC.x + qD.x + qE.x + qF.x + qG.x + qH.x + qI.x +\n"
"                    qA1.x + qB1.x + qC1.x + qA0.x + qC4.x + qD0.x + qF4.x + qG0.x + qI4.x + qG5.x + qH5.x + qI5.x) / 21.0;\n"
"    float cy = dot(res, float3(0.299, 0.587, 0.114));\n"
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
"      float3 m4 = 0.25 * (cB + cH + cD + cF);\n"
"      res = saturate(res + (res - m4) * k);\n"
"    }\n"
"  }\n"
"  return res;\n"
"}\n"
/* simple CRT: gaussian scanlines + aperture grille */
"float3 crt_effect(float3 a, float2 uv, float2 pos) {\n"
"  float ty = uv.y * src.y;\n"
"  float yc = floor(ty) + 0.5;\n"
"  float d = ty - yc;\n"
"  float lum = dot(a, float3(0.3, 0.6, 0.1));\n"
"  float width = lerp(6.0, 2.5, lum);\n"
"  float beam = exp(-d * d * width * 4.0);\n"
"  float strength = saturate((src.w / src.y - 2.5) / 4.0);\n"
"  float3 c = a * lerp(1.0, beam * 1.35, strength);\n"
"  int m = (int)pos.x % 3;\n"
"  float3 mask = m == 0 ? float3(1.0, 0.85, 0.85) : (m == 1 ? float3(0.85, 1.0, 0.85) : float3(0.85, 0.85, 1.0));\n"
"  mask = lerp(1.0, mask, strength);\n"
"  return saturate(c * mask * lerp(1.0, 1.1, strength));\n"
"}\n"
"float3 crt(float2 uv, float2 pos) {\n"
"  float ty = uv.y * src.y;\n"
"  float yc = floor(ty) + 0.5;\n"
"  float3 a = sharp(float2(uv.x, yc / src.y));\n"
"  return crt_effect(a, uv, pos);\n"
"}\n"
"float4 psmain(VO i) : SV_Target {\n"
"  if (prm.w > 0.5) {\n"
"    float2 st = exp2(prm.y) / src.xy;\n"
"    float3 b = 0; float wsum = 0;\n"
"    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
"      float w = exp(-(x * x + y * y) / 4.0);\n"
"      b += tex.SampleLevel(sl, i.uv + float2(x, y) * st, prm.y).rgb * w; wsum += w;\n"
"    }\n"
"    return float4(b / wsum * prm.z, 1);\n"
"  }\n"
"  int f = (int)(prm.x + 0.5);\n"
"  int up = (int)(up_prm.x + 0.5);\n"
"  float3 c;\n"
"  if (up > 0) {\n"
"    c = xbr_cartoon(i.uv, up >= 2);\n"
"    if (f == 2) c = crt_effect(c, i.uv, i.pos.xy);\n"
"  } else {\n"
"    if (f == 0) c = sharp(i.uv);\n"
"    else if (f == 1) c = bicubic(i.uv);\n"
"    else if (f == 2) c = crt(i.uv, i.pos.xy);\n"
"    else c = tex.SampleLevel(sp, i.uv, 0).rgb;\n"
"  }\n"
"  return float4(c, 1);\n"
"}\n";

static const char *aspect_names[] = {"4:3 + blurred sides", "4:3", "stretch to fill", "zoom to fill width"};
static const char *filter_names[] = {"sharp", "smooth", "CRT", "nearest"};
RenderSettings render_settings = {0, 0, 1};

const char *render_aspect_name(int a) { return aspect_names[a % RENDER_ASPECTS]; }
const char *render_filter_name(int f) { return filter_names[f % RENDER_FILTERS]; }

static void release_rtv(void) {
    if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
}

static int make_rtv(void) {
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return -1;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    ID3D11Texture2D_Release(bb);
    return FAILED(hr) ? -1 : 0;
}

int render_init(HWND hwnd) {
    rwnd = hwnd;
    RECT r;
    GetClientRect(hwnd, &r);
    buf_w = r.right > 0 ? (UINT)r.right : 1;
    buf_h = r.bottom > 0 ? (UINT)r.bottom : 1;
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = buf_w;
    sd.BufferDesc.Height = buf_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                               &sd, &swap, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                           &sd, &swap, &dev, NULL, &ctx);
    }
    if (FAILED(hr)) { LOG("Direct3D 11 unavailable (%08lx); using GDI", (unsigned long)hr); return -1; }
    /* we handle Alt+Enter ourselves (borderless fullscreen) */
    {
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *fa = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) &&
            SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fa)))
            IDXGIFactory_MakeWindowAssociation(fa, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (fa) IDXGIFactory_Release(fa);
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
    }
    if (make_rtv()) return -1;

    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    size_t sl = strlen(shader_src);
    if (FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "vsmain", "vs_4_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "psmain", "ps_4_0", 0, 0, &pb, &err))) {
        LOG("shader compile failed: %s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return -1;
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &ps);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf);

    D3D11_SAMPLER_DESC s = {0};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_lin);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_point);
    LOG("renderer: Direct3D 11, %ux%u", buf_w, buf_h);
    return 0;
}

static int ensure_texture(int w, int h) {
    if (tex && w == tex_w && h == tex_h) return 0;
    if (srv) { ID3D11ShaderResourceView_Release(srv); srv = NULL; }
    if (tex) { ID3D11Texture2D_Release(tex); tex = NULL; }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 0;                 /* full chain: low mips give the blurred backdrop */
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) return -1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv))) return -1;
    ID3D11Texture2D_GetDesc(tex, &td);
    tex_w = w; tex_h = h; tex_mips = (int)td.MipLevels;
    return 0;
}

static void draw(float l, float t, float r, float b, float u0, float v0, float us, float vs_, int backdrop,
                 float mip, float bright, float dw, float dh,
                 int up_mode, float lw, float lh, float sx) {
    CB c;
    c.rect[0] = l; c.rect[1] = t; c.rect[2] = r; c.rect[3] = b;
    c.src[0] = (float)tex_w; c.src[1] = (float)tex_h; c.src[2] = dw; c.src[3] = dh;
    c.prm[0] = (float)render_settings.filter; c.prm[1] = mip; c.prm[2] = bright; c.prm[3] = (float)backdrop;
    c.uvx[0] = u0; c.uvx[1] = v0; c.uvx[2] = us; c.uvx[3] = vs_;
    c.up[0] = (float)up_mode; c.up[1] = lw; c.up[2] = lh; c.up[3] = sx;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cbuf, 0, NULL, &c, 0, 0);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
}

int render_present(const uint32_t *fb, int w, int h) {
    if (!dev) return -1;
    RECT cr;
    GetClientRect(rwnd, &cr);
    UINT cw = cr.right > 0 ? (UINT)cr.right : 1, ch = cr.bottom > 0 ? (UINT)cr.bottom : 1;
    if (IsIconic(rwnd)) return 0;
    if (cw != buf_w || ch != buf_h) {
        release_rtv();
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return -1;
        buf_w = cw; buf_h = ch;
        if (make_rtv()) return -1;
    }
    if (ensure_texture(w, h)) return -1;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, fb, (UINT)w * 4, 0);

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

    /* picture size in pixels for the chosen aspect mode */
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
        ID3D11DeviceContext_GenerateMips(ctx, srv);
    }

    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)cw, (float)ch, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11SamplerState *ss[2] = {samp_lin, samp_point};
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, ss);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);

    if (mode == 0 && pw < cw - 1) {
        /* blurred, dimmed copy of the picture zoomed to cover the whole window */
        double cover_w = cw, cover_h = cw / aspect;
        if (cover_h < ch) { cover_h = ch; cover_w = ch * aspect; }
        float us = (float)(cw / cover_w) * us0, vs2 = (float)(ch / cover_h) * vs0;
        float bu0 = u0 + (us0 - us) * 0.5f;
        float bv0 = v0 + (vs0 - vs2) * 0.5f;
        float mip = (float)(tex_mips > 3 ? 3 : tex_mips - 1);
        draw(-1.0f, 1.0f, 1.0f, -1.0f, bu0, bv0, us, vs2, 1, mip, 0.5f, (float)cw, (float)ch, 0, lw, lh, sx);
    }
    draw(l, t, r, b, u0, v0, us0, vs0, 0, 0.0f, 1.0f, (float)(pw / us0), (float)(ph / vs0), up_mode, lw, lh, sx);
    IDXGISwapChain_Present(swap, 0, 0);
    return 0;
}
