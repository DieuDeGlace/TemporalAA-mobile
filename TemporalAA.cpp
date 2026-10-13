// TemporalAA: temporal anti-aliasing for Cube World (Steam, 64-bit, D3D11),
// made with AI assistance. No ReShade integration.
//
// How it works:
//   * Map/Unmap/UpdateSubresource hooks remember the contents of every 64-byte
//     constant buffer (as private data on the buffer), so the projection matrix
//     of a draw can be read back without a GPU readback.
//   * Draw/DrawIndexed hooks: world draws get a sub-pixel jittered copy of the
//     projection (8-frame Halton sequence) bound in place of the native one.
//   * The first orthographic (HUD) draw into the scene target marks the end of
//     the 3D world: the jittered frame is blended with the previous frames'
//     history (apply()), after MoonandShadow and alpha-lookalike finish their
//     own world passes.
//   * F9 toggles the effect.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "NativeScene.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include "GpuPassTimer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
namespace taa {
static GpuPassTimer gpuTimer;
template <class T> void drop(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ===========================================================================
// Hooked ID3D11DeviceContext methods and constant-buffer shadow copies
// ===========================================================================

// ID3D11DeviceContext vtable slots.
constexpr UINT SLOT_DRAW_INDEXED = 12, SLOT_DRAW = 13, SLOT_MAP = 14, SLOT_UNMAP = 15,
               SLOT_UPDATE_SUBRESOURCE = 48;

using DI = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using D = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using Map = HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP,
                                         UINT, D3D11_MAPPED_SUBRESOURCE *);
using Unmap = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
using Update = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                         const D3D11_BOX *, const void *, UINT, UINT);
static Map oldMap;
static Unmap oldUnmap;
static Update oldUpdate;
static void **table = nullptr; // Hooked context vtable (set once attached).

// Private-data key under which the last written contents of each 64-byte
// constant buffer are stored.
static const GUID cacheGuid = {
    0x7435cd29, 0x9d35, 0x4ef2, {0xa1, 0x25, 0x54, 0x18, 0x81, 0x27, 0x22, 0x93}};
struct Mapped {
    void *data;
    UINT size;
};
static thread_local std::unordered_map<ID3D11Resource *, Mapped> mapped; // Currently mapped.

static void log(const char *s) {
    FILE *f = nullptr;
    fopen_s(&f, "Mods\\TemporalAA.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}
// 64 if `r` is a 64-byte constant buffer (a single matrix), otherwise 0.
static UINT bytes(ID3D11Resource *r) {
    ID3D11Buffer *b = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Buffer), (void **)&b)))
        return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    b->Release();
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth == 64 ? 64 : 0;
}
// Reads the shadow copy of a constant buffer's contents.
static bool readBuffer(ID3D11Buffer *b, void *out, UINT n) {
    UINT got = n;
    return b && SUCCEEDED(b->GetPrivateData(cacheGuid, &got, out)) && got == n;
}

// ===========================================================================
// State
// ===========================================================================

static bool enabled = true, internal = false, finished = false;
// Settings (see TemporalAA.ini).
static float historyWeight = .65f, edgeHistoryWeight = .90f;
static float blurStrength = .12f, jitterScale = .75f;

// What before() learned about the current draw, reused by jitter().
struct PreparedDraw {
    bool checked = false; // before() ran for the current draw.
    bool valid = false;   // ...and found a full-screen perspective scene draw.
    float projection[16]{};
    D3D11_VIEWPORT viewport{};
};
static PreparedDraw preparedDraw;
static UINT frame = 0;
static bool historyValid = false;
static float jitterX = 0, jitterY = 0; // Current jitter in pixels.
static ID3D11Texture2D *history = nullptr;
static ID3D11ShaderResourceView *historyView = nullptr;
static ID3D11SamplerState *sampler = nullptr;
static ID3D11Buffer *jitterBuffer = nullptr; // Our jittered projection constant buffer.
static float uploadedProjection[16]{};
static bool projectionUploaded = false;
static unsigned projectionUploads = 0;

// The private jitter buffer is written only here. Reuse identical contents
// across draws; binding and restoring the native buffer remains per draw.
static void uploadProjection(ID3D11DeviceContext *c, const float *projection) {
    if (projectionUploaded && memcmp(uploadedProjection, projection, 64) == 0)
        return;
    oldUpdate(c, jitterBuffer, 0, nullptr, projection, 0, 0);
    memcpy(uploadedProjection, projection, 64);
    projectionUploaded = true;
    ++projectionUploads;
}

static ID3D11Device *device = nullptr;
static ID3D11DeviceContext *context = nullptr;
static ID3D11DeviceContext1 *context1 = nullptr;
static ID3DDeviceContextState *state = nullptr; // Isolated pipeline state for our pass.
static ID3D11Texture2D *copy = nullptr;         // This frame's scene, as shader input.
static ID3D11ShaderResourceView *view = nullptr;
static ID3D11RenderTargetView *target = nullptr; // The scene target of this frame.
static D3D11_VIEWPORT viewport{};
static ID3D11VertexShader *vs = nullptr;
static ID3D11PixelShader *ps = nullptr;
static ID3D11Buffer *params = nullptr;
static ID3D11RasterizerState *raster = nullptr;
static DI oldDI = nullptr;
static D oldD = nullptr;
// Camera of the previous frame: a cut or zoom invalidates history.
static float previousView[16]{}, previousProjection[16]{};
static bool cameraKnown = false;
static UINT width = 0, height = 0;
static DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

// ===========================================================================
// Resolve shader
// ===========================================================================

// vs: full-screen triangle from SV_VertexID. ps: blends this frame with the
// clamped history, keeping more history on edges and thin details.
static const char *code = R"(
cbuffer Resolve:register(b0){float weight,valid,jitterX,jitterY;float blurStrength,edgeWeight;float2 padding;};
Texture2D<float4> scene:register(t0);
Texture2D<float4> history:register(t1);
SamplerState linearSampler:register(s0);

float4 vs(uint id:SV_VertexID):SV_POSITION{
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}

float maxChannel(float3 value){return max(value.r,max(value.g,value.b));}
float4 ps(float4 p:SV_POSITION):SV_TARGET{
    uint w,h;
    scene.GetDimensions(w,h);
    float2 uv=(p.xy+float2(jitterX,jitterY))/float2(w,h);
    float4 current=scene.SampleLevel(linearSampler,uv,0);
    float3 lo=current.rgb,hi=current.rgb;
    float3 left=current.rgb,right=current.rgb,up=current.rgb,down=current.rgb;
    [unroll]for(int y=-1;y<=1;y++)
        [unroll]for(int x=-1;x<=1;x++){
            // The center is already fetched above; reuse it instead of sampling twice.
            if(x==0&&y==0)continue;
            float3 c=scene.SampleLevel(linearSampler,uv+float2(x,y)/float2(w,h),0).rgb;
            lo=min(lo,c);
            hi=max(hi,c);
            if(y==0&&x==-1)left=c;
            if(y==0&&x==1)right=c;
            if(x==0&&y==-1)up=c;
            if(x==0&&y==1)down=c;
        }
    float3 raw=history.Load(int3(p.xy,0)).rgb;
    // Bound history to this frame's neighborhood by moving it along the line
    // toward this pixel's own color until it enters the box. The result is
    // always a mix of two real colors. A per-channel clamp could instead land
    // on a box corner no neighbor has, which showed as green/red/yellow/blue
    // lines on edges once history was kept longer.
    float3 offset=raw-current.rgb;
    float3 room=offset>0?hi-current.rgb:lo-current.rgb;
    float3 limits=abs(offset)>1e-5?saturate(room/offset):1;
    float3 clipped=current.rgb+offset*min(limits.x,min(limits.y,limits.z));
    float3 old=clipped;
    // Jitter only moves edge coverage within the local range, so history that
    // already lies inside the neighborhood is kept even when it differs from
    // this frame's pixel; rejecting that difference is what made strong edges
    // shimmer. History the clip had to move is a real change (motion,
    // disocclusion, lighting) and is released.
    float outside=maxChannel(abs(raw-clipped));
    float confidence=1-smoothstep(.02,.12,outside);
    // Edges and thin details carry the jitter's coverage changes; accumulate
    // them longer. Flat areas keep the configured history weight.
    float edge=smoothstep(.04,.20,maxChannel(hi-lo));
    float accumulation=lerp(weight,max(weight,edgeWeight),edge)*valid*confidence;
    float3 filtered=lerp(current.rgb,(current.rgb*4+left+right+up+down)/8,blurStrength);
    // Luminance-weighted blend (Karis): bright samples cannot dominate the
    // average across jitter phases, which steadies bright/dark edges.
    float currentWeight=(1-accumulation)/(1+maxChannel(filtered));
    float historyWeight=accumulation/(1+maxChannel(old));
    return float4((filtered*currentWeight+old*historyWeight)/(currentWeight+historyWeight),current.a);
})";

// Halton low-discrepancy sequence in [0, 1).
static float halton(UINT i, UINT base) {
    float f = 1, r = 0;
    while (i) {
        f /= base;
        r += f * (i % base);
        i /= base;
    }
    return r;
}

// Called once per presented frame: advances the jitter.
static void reset() {
    if (!finished) // The previous frame never resolved.
        historyValid = false;
    drop(target);
    finished = false;
    frame++;
    jitterX = (halton(frame % 8 + 1, 2) - .5f) * jitterScale;
    jitterY = (halton(frame % 8 + 1, 3) - .5f) * jitterScale;
}

// Creates the size-independent resources on first use (for context `c`).
static bool initialize(ID3D11DeviceContext *c) {
    if (device)
        return context == c && vs && ps && params && state && raster && sampler && jitterBuffer;
    context = c;
    context->AddRef();
    c->GetDevice(&device);
    ID3D11Device1 *d1 = nullptr;
    bool ok = SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1)) &&
              SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&context1));
    auto level = device->GetFeatureLevel();
    if (ok)
        ok = SUCCEEDED(d1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                                                    __uuidof(ID3D11Device), nullptr, &state));
    drop(d1);
    for (int i = 0; i < 2 && ok; i++) {
        bool pixel = i == 1;
        ID3DBlob *b = nullptr, *error = nullptr;
        ok = SUCCEEDED(D3DCompile(code, strlen(code), "TemporalResolve", nullptr, nullptr,
                                  pixel ? "ps" : "vs", pixel ? "ps_4_0" : "vs_4_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &error));
        if (error)
            log((char *)error->GetBufferPointer());
        if (ok)
            ok = SUCCEEDED(pixel ? device->CreatePixelShader(b->GetBufferPointer(),
                                                             b->GetBufferSize(), nullptr, &ps)
                                 : device->CreateVertexShader(b->GetBufferPointer(),
                                                              b->GetBufferSize(), nullptr, &vs));
        drop(b);
        drop(error);
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 32;
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (ok)
        ok = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &params));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (ok)
        ok = SUCCEEDED(device->CreateRasterizerState(&rd, &raster));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (ok)
        ok = SUCCEEDED(device->CreateSamplerState(&sd, &sampler));
    cb.ByteWidth = 64;
    if (ok)
        ok = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &jitterBuffer));
    return ok;
}

// (Re)creates the scene copy and history textures for the target's size/format.
static bool texture(ID3D11Texture2D *source, DXGI_FORMAT typed) {
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    if (copy && width == d.Width && height == d.Height && format == typed)
        return true;
    drop(view);
    drop(copy);
    drop(historyView);
    drop(history);
    historyValid = false;
    width = d.Width;
    height = d.Height;
    format = typed;
    d.Format = typed;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &copy)) &&
           SUCCEEDED(device->CreateShaderResourceView(copy, nullptr, &view)) &&
           SUCCEEDED(device->CreateTexture2D(&d, nullptr, &history)) &&
           SUCCEEDED(device->CreateShaderResourceView(history, nullptr, &historyView));
}

// ===========================================================================
// Resolve pass
// ===========================================================================

// Resolves the jittered scene in `target` against history (once per frame) in
// an isolated device-context state, then stores the result as new history.
static bool apply() {
    if (internal || finished || !target || !initialize(context))
        return false;
    ID3D11Resource *resource = nullptr;
    target->GetResource(&resource);
    ID3D11Texture2D *source = nullptr;
    bool ok = SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&source));
    drop(resource);
    D3D11_RENDER_TARGET_VIEW_DESC rt{};
    target->GetDesc(&rt);
    D3D11_TEXTURE2D_DESC td{};
    if (source)
        source->GetDesc(&td);
    // Only ordinary full-size 2D scene targets; do not affect arrays/mips/offscreen previews.
    ok = ok &&
         (rt.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ||
          rt.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS) &&
         td.ArraySize == 1 && td.MipLevels == 1 && viewport.TopLeftX == 0 &&
         viewport.TopLeftY == 0 && viewport.Width == td.Width && viewport.Height == td.Height;
    if (ok)
        ok = texture(source, rt.Format);
    if (!ok) {
        drop(source);
        return false;
    }
    internal = true;
    ID3DDeviceContextState *saved = nullptr;
    gpuTimer.begin(device, context, "Mods\\TemporalAA.log",
                   "TAA (input copy/resolve + history copy)");
    context1->SwapDeviceContextState(state, &saved);
    // Copy (or MSAA-resolve) the scene so it can be read while writing `target`.
    if (td.SampleDesc.Count > 1)
        context->ResolveSubresource(copy, 0, source, 0, rt.Format);
    else
        context->CopyResource(copy, source);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(nullptr, nullptr, ~0u);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetState(raster);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    if (!historyValid)
        context->CopyResource(history, copy);
    gpuTimer.split(context);
    float values[8] = {historyWeight, historyValid ? 1.f : 0.f, jitterX, jitterY, blurStrength,
                       edgeHistoryWeight, 0, 0};
    context->UpdateSubresource(params, 0, nullptr, values, 0, 0);
    context->PSSetConstantBuffers(0, 1, &params);
    ID3D11ShaderResourceView *inputs[] = {view, historyView};
    context->PSSetShaderResources(0, 2, inputs);
    context->PSSetSamplers(0, 1, &sampler);
    oldD(context, 3, 0); // Full-screen triangle.
    ID3D11ShaderResourceView *empty[2] = {};
    context->PSSetShaderResources(0, 2, empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(history, source);
    historyValid = true;
    context1->SwapDeviceContextState(saved, nullptr);
    gpuTimer.end(context);
    drop(saved);
    drop(source);
    drop(target);
    internal = false;
    finished = true;
    return true;
}

// ===========================================================================
// Draw classification
// ===========================================================================

// Both native vertex shaders use the same projection/view/world matrix slots
// (b0-b2, 64 bytes each). The textured/background shader has only that triplet;
// the solid world shader also has material, point-light and directional-light
// buffers (16, 320 and 48 bytes). Returns whether the matrix triplet is present
// and sets `world` when the full solid-world layout is bound.
static bool perspectiveLayout(ID3D11DeviceContext *c, bool &world) {
    ID3D11Buffer *buffers[6]{};
    c->VSGetConstantBuffers(0, 6, buffers);
    const UINT sizes[] = {64, 64, 64, 16, 320, 48};
    bool matrices = true;
    world = true;
    for (int i = 0; i < 6; ++i) {
        D3D11_BUFFER_DESC description{};
        if (buffers[i])
            buffers[i]->GetDesc(&description);
        if (i < 3)
            matrices &= description.ByteWidth == 64;
        world &= description.ByteWidth == sizes[i];
        drop(buffers[i]);
    }
    return matrices;
}

// A full-screen scene viewport (not a preview or a small render target).
static bool sceneViewport(UINT count, const D3D11_VIEWPORT &vp) {
    return !(!count || vp.Width < 320 || vp.Height < 180 || vp.TopLeftX || vp.TopLeftY);
}

// Lets the other Steam mods finish their world passes before the resolve.
static void finishOtherMods() {
    auto module = GetModuleHandleW(L"MoonandShadow.dll");
    auto finish =
        module ? (void(WINAPI *)())GetProcAddress(module, "FinishWorldLighting") : nullptr;
    if (finish)
        finish();
    auto gradeModule = GetModuleHandleW(L"alpha-lookalike.dll");
    auto grade = gradeModule ? (void(WINAPI *)())GetProcAddress(gradeModule, "FinishAlphaLookalike")
                             : nullptr;
    if (grade)
        grade();
}

// On the first world draw of a frame: remember the scene target and invalidate
// history after a camera cut (big move/turn) or zoom change.
static void beginFrame(ID3D11DeviceContext *c, const float *projection, const D3D11_VIEWPORT &vp) {
    ID3D11Buffer *viewBuffer = nullptr;
    c->VSGetConstantBuffers(1, 1, &viewBuffer);
    float camera[16]{};
    bool known = readBuffer(viewBuffer, camera, 64);
    drop(viewBuffer);
    // A rotation-only sky camera may retain the solid shader's bound lighting
    // buffers. It still gets jitter, but must not become the history target or
    // replace the world camera used for cut detection.
    bool rotationOnly = fabsf(camera[3]) + fabsf(camera[7]) + fabsf(camera[11]) < .01f;
    if (!known || rotationOnly)
        return;
    if (cameraKnown) {
        bool cut = false;
        // Translation (column 3) may move 8 units, rotation entries 0.35.
        for (int i = 0; i < 12; i++)
            cut |= fabsf(camera[i] - previousView[i]) > ((i % 4 == 3) ? 8.f : .35f);
        cut |= fabsf(projection[0] - previousProjection[0]) > .01f ||
               fabsf(projection[5] - previousProjection[5]) > .01f;
        if (cut)
            historyValid = false;
    }
    memcpy(previousView, camera, 64);
    memcpy(previousProjection, projection, 64);
    cameraKnown = true;
    context = c;
    viewport = vp;
    // Only a target drawn with depth is the world scene.
    ID3D11DepthStencilView *sceneDepth = nullptr;
    c->OMGetRenderTargets(1, &target, &sceneDepth);
    if (!sceneDepth)
        drop(target);
    drop(sceneDepth);
}

// Runs before every draw. A perspective draw with the native world layout is
// "prepared" for jittering; an orthographic draw into the scene target means
// the world is complete, so the resolve runs.
static void before(ID3D11DeviceContext *c) {
    preparedDraw.checked = true;
    preparedDraw.valid = false;
    if (!enabled || internal || finished)
        return;
    ID3D11Buffer *b = nullptr;
    c->VSGetConstantBuffers(0, 1, &b);
    float projection[16]{};
    bool known = readBuffer(b, projection, 64);
    drop(b);
    if (!known)
        return;
    bool orthographic = fabsf(projection[15]) > .5f;
    if (orthographic) {
        if (target) {
            ID3D11RenderTargetView *current = nullptr;
            c->OMGetRenderTargets(1, &current, nullptr);
            bool same = current == target;
            drop(current);
            if (same) {
                finishOtherMods();
                apply();
            }
        }
        return;
    }
    bool world = false;
    if (!perspectiveLayout(c, world))
        return;
    UINT n = 1;
    D3D11_VIEWPORT vp{};
    c->RSGetViewports(&n, &vp);
    if (!sceneViewport(n, vp))
        return;
    preparedDraw.valid = true;
    memcpy(preparedDraw.projection, projection, 64);
    preparedDraw.viewport = vp;
    if (!world)
        return; // Jitter backgrounds, but capture history only from the world.
    if (!target)
        beginFrame(c, projection, vp);
}

// Binds a jittered copy of the projection for a world draw. Returns the native
// buffer (to rebind after the draw) or nullptr if nothing was changed.
static ID3D11Buffer *jitter(ID3D11DeviceContext *c) {
    // Skip draws that before() already examined and rejected.
    if (internal || !enabled || finished || (preparedDraw.checked && !preparedDraw.valid))
        return nullptr;
    ID3D11Buffer *b = nullptr;
    c->VSGetConstantBuffers(0, 1, &b);
    float p[16]{};
    D3D11_VIEWPORT vp{};
    if (preparedDraw.valid) {
        memcpy(p, preparedDraw.projection, 64);
        vp = preparedDraw.viewport;
        if (!initialize(c)) {
            drop(b);
            return nullptr;
        }
    } else {
        if (!readBuffer(b, p, 64) || fabsf(p[15]) > .5f) {
            drop(b);
            return nullptr;
        }
        bool world = false;
        bool native = perspectiveLayout(c, world);
        UINT count = 1;
        c->RSGetViewports(&count, &vp);
        if (!native || !sceneViewport(count, vp) || !initialize(c)) {
            drop(b);
            return nullptr;
        }
    }
    // Shift clip-space x/y by the jitter (scaled by w).
    for (int i = 0; i < 4; i++) {
        p[i] += 2 * jitterX / vp.Width * p[12 + i];
        p[4 + i] -= 2 * jitterY / vp.Height * p[12 + i];
    }
    internal = true;
    uploadProjection(c, p);
    c->VSSetConstantBuffers(0, 1, &jitterBuffer);
    internal = false;
    return b;
}

// ===========================================================================
// Hooks
// ===========================================================================

static void STDMETHODCALLTYPE di(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    before(c);
    auto original = jitter(c);
    oldDI(c, n, s, b);
    if (original) {
        c->VSSetConstantBuffers(0, 1, &original);
        drop(original);
    }
}
static void STDMETHODCALLTYPE draw(ID3D11DeviceContext *c, UINT n, UINT s) {
    before(c);
    auto original = jitter(c);
    oldD(c, n, s);
    if (original) {
        c->VSSetConstantBuffers(0, 1, &original);
        drop(original);
    }
}
// Map/Unmap: remember writes to 64-byte constant buffers.
static HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                     D3D11_MAP t, UINT flags, D3D11_MAPPED_SUBRESOURCE *m) {
    auto hr = oldMap(c, r, sub, t, flags, m);
    if (!internal && SUCCEEDED(hr) && sub == 0 && t != D3D11_MAP_READ) {
        UINT n = bytes(r);
        if (n)
            mapped[r] = {m->pData, n};
    }
    return hr;
}
static void STDMETHODCALLTYPE unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub) {
    auto it = mapped.find(r);
    if (it != mapped.end()) {
        r->SetPrivateData(cacheGuid, it->second.size, it->second.data);
        mapped.erase(it);
    }
    oldUnmap(c, r, sub);
}
static void STDMETHODCALLTYPE update(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                     const D3D11_BOX *box, const void *data, UINT row, UINT slice) {
    if (!internal && sub == 0 && !box && data) {
        UINT n = bytes(r);
        if (n)
            r->SetPrivateData(cacheGuid, n, data);
    }
    oldUpdate(c, r, sub, box, data, row, slice);
}

static bool patch(void **slot, void *fn) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return false;
    InterlockedExchangePointer(slot, fn);
    DWORD unused;
    VirtualProtect(slot, sizeof(void *), old, &unused);
    return true;
}

static void loadSettings() {
    const wchar_t *ini = L".\\Mods\\TemporalAA.ini";
    enabled = GetPrivateProfileIntW(L"TAA", L"Enabled", 1, ini) != 0;
    historyWeight =
        std::clamp((int)GetPrivateProfileIntW(L"TAA", L"HistoryPercent", 65, ini), 0, 85) * .01f;
    // History kept on edges/thin details, where jitter alone changes coverage.
    edgeHistoryWeight =
        std::clamp((int)GetPrivateProfileIntW(L"TAA", L"EdgeHistoryPercent", 90, ini), 0, 95) *
        .01f;
    blurStrength =
        std::clamp((int)GetPrivateProfileIntW(L"TAA", L"BlurPercent", 12, ini), 0, 40) * .01f;
    jitterScale =
        std::clamp((int)GetPrivateProfileIntW(L"TAA", L"JitterPercent", 75, ini), 25, 100) * .01f;
}

// Hooks the shared ID3D11DeviceContext vtable (taken from a temporary device).
// All hooks are installed or none are.
static void attach() {
    if (table || !nativeScene::supported())
        return;
    loadSettings();
    if (!enabled)
        return;
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &d, nullptr, &c)))
        return;
    auto v = *(void ***)c;
    struct Hook {
        UINT slot;
        void *fn;
        void **original;
    };
    Hook hooks[] = {{SLOT_DRAW_INDEXED, (void *)di, (void **)&oldDI},
                    {SLOT_DRAW, (void *)draw, (void **)&oldD},
                    {SLOT_MAP, (void *)map, (void **)&oldMap},
                    {SLOT_UNMAP, (void *)unmap, (void **)&oldUnmap},
                    {SLOT_UPDATE_SUBRESOURCE, (void *)update, (void **)&oldUpdate}};
    for (auto &h : hooks)
        *h.original = v[h.slot];
    int done = 0;
    bool ok = true;
    for (auto &h : hooks) {
        if (!patch(v + h.slot, h.fn)) {
            ok = false;
            while (done--) {
                auto &u = hooks[done];
                patch(v + u.slot, *u.original);
            }
            break;
        }
        done++;
    }
    if (ok) {
        table = v;
        log("TemporalAA prototype enabled; F9 toggles. Jitter/history resolve, no motion-vector buffer.");
    }
    drop(c);
    drop(d);
}
} // namespace taa

// ===========================================================================
// Cube World mod-loader entry points
// ===========================================================================

class TemporalAA : public GenericMod {
  public:
    void Initialize() override {
        taa::attach();
    }
    void OnPresent(IDXGISwapChain *, UINT, UINT) override {
        if (GetAsyncKeyState(VK_F9) & 1) {
            taa::enabled = !taa::enabled;
            taa::historyValid = false;
        }
        taa::reset();
    }
};
EXPORT GenericMod *MakeMod() {
    return new TemporalAA();
}
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}
