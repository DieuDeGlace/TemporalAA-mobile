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
// Original world-only grading, made with AI assistance. No ReShade integration.
namespace taa {
static GpuPassTimer gpuTimer;
template <class T> void drop(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
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
static void **table = nullptr;
static const GUID cacheGuid = {
    0x7435cd29, 0x9d35, 0x4ef2, {0xa1, 0x25, 0x54, 0x18, 0x81, 0x27, 0x22, 0x93}};
struct Mapped {
    void *data;
    UINT size;
};
static thread_local std::unordered_map<ID3D11Resource *, Mapped> mapped;
static void log(const char *s) {
    FILE *f = nullptr;
    fopen_s(&f, "Mods\\TemporalAA.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}
static UINT bytes(ID3D11Resource *r) {
    ID3D11Buffer *b = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Buffer), (void **)&b)))
        return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    b->Release();
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth == 64 ? 64 : 0;
}
static bool readBuffer(ID3D11Buffer *b, void *out, UINT n) {
    UINT got = n;
    return b && SUCCEEDED(b->GetPrivateData(cacheGuid, &got, out)) && got == n;
}
static bool enabled = true, internal = false, finished = false;
static float historyWeight = .65f;
static float blurStrength = .12f, jitterScale = .75f;
struct PreparedDraw {
    bool valid = false;
    float projection[16]{};
    D3D11_VIEWPORT viewport{};
};
static PreparedDraw preparedDraw;
static UINT frame = 0;
static bool historyValid = false;
static float jitterX = 0, jitterY = 0;
static ID3D11Texture2D *history = nullptr;
static ID3D11ShaderResourceView *historyView = nullptr;
static ID3D11SamplerState *sampler = nullptr;
static ID3D11Buffer *jitterBuffer = nullptr;
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
static ID3DDeviceContextState *state = nullptr;
static ID3D11Texture2D *copy = nullptr;
static ID3D11ShaderResourceView *view = nullptr;
static ID3D11RenderTargetView *target = nullptr;
static D3D11_VIEWPORT viewport{};
static ID3D11VertexShader *vs = nullptr;
static ID3D11PixelShader *ps = nullptr;
static ID3D11Buffer *params = nullptr;
static ID3D11RasterizerState *raster = nullptr;
static DI oldDI = nullptr;
static D oldD = nullptr;
static float previousView[16]{}, previousProjection[16]{};
static bool cameraKnown = false;
static UINT width = 0, height = 0;
static DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
static const char *code = R"(
cbuffer Resolve:register(b0){float weight,valid,jitterX,jitterY;float blurStrength;float3 padding;};
Texture2D<float4> scene:register(t0);Texture2D<float4> history:register(t1);SamplerState linearSampler:register(s0);
float4 vs(uint id:SV_VertexID):SV_POSITION{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
float4 ps(float4 p:SV_POSITION):SV_TARGET{
 uint w,h;scene.GetDimensions(w,h);float2 uv=(p.xy+float2(jitterX,jitterY))/float2(w,h);
 float4 current=scene.SampleLevel(linearSampler,uv,0);float3 lo=current.rgb,hi=current.rgb;float3 left=current.rgb,right=current.rgb,up=current.rgb,down=current.rgb;
 [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++){
  float3 c=scene.SampleLevel(linearSampler,uv+float2(x,y)/float2(w,h),0).rgb;lo=min(lo,c);hi=max(hi,c);if(y==0&&x==-1)left=c;if(y==0&&x==1)right=c;if(x==0&&y==-1)up=c;if(x==0&&y==1)down=c;
 }
 float3 old=history.Load(int3(p.xy,0)).rgb;
 float disagreement=max(abs(old.r-current.r),max(abs(old.g-current.g),abs(old.b-current.b)));
 // Opposing neighbors distinguish a thin peak/trough from a broad silhouette.
 float3 lr=(left-current.rgb)*(right-current.rgb),ud=(up-current.rgb)*(down-current.rgb);
 float thin=saturate(24*sqrt(max(0,max(max(lr.r,lr.g),max(lr.b,max(max(ud.r,ud.g),ud.b))))));
 float confidence=1-smoothstep(.035,lerp(.16,.30,thin),disagreement);
 float accumulation=lerp(weight,max(weight,.78),thin);
 // History is bounded to this frame's neighborhood and rejected on changes.
 float3 filtered=lerp(current.rgb,(current.rgb*4+left+right+up+down)/8,blurStrength);
 old=clamp(old,lo,hi);return float4(lerp(filtered,old,accumulation*valid*confidence),current.a);
})";
static float halton(UINT i, UINT base) {
    float f = 1, r = 0;
    while (i) {
        f /= base;
        r += f * (i % base);
        i /= base;
    }
    return r;
}
static void reset() {
    if (!finished)
        historyValid = false;
    drop(target);
    finished = false;
    frame++;
    jitterX = (halton(frame % 8 + 1, 2) - .5f) * jitterScale;
    jitterY = (halton(frame % 8 + 1, 3) - .5f) * jitterScale;
}
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
        ID3DBlob *b = nullptr, *error = nullptr;
        ok = SUCCEEDED(D3DCompile(code, strlen(code), "TemporalResolve", nullptr, nullptr,
                                  i ? "ps" : "vs", i ? "ps_4_0" : "vs_4_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &error));
        if (error)
            log((char *)error->GetBufferPointer());
        if (ok)
            ok = SUCCEEDED(i ? device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(),
                                                         nullptr, &ps)
                             : device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(),
                                                          nullptr, &vs));
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
    float values[8] = {
        historyWeight, historyValid ? 1.f : 0.f, jitterX, jitterY, blurStrength, 0, 0, 0};
    context->UpdateSubresource(params, 0, nullptr, values, 0, 0);
    context->PSSetConstantBuffers(0, 1, &params);
    ID3D11ShaderResourceView *inputs[] = {view, historyView};
    context->PSSetShaderResources(0, 2, inputs);
    context->PSSetSamplers(0, 1, &sampler);
    oldD(context, 3, 0);
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
static void before(ID3D11DeviceContext *c) {
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
    if (fabsf(projection[15]) > .5f) {
        if (target) {
            ID3D11RenderTargetView *current = nullptr;
            c->OMGetRenderTargets(1, &current, nullptr);
            bool same = current == target;
            drop(current);
            if (same) {
                auto module = GetModuleHandleW(L"MoonandShadow.dll");
                auto finish = module
                                  ? (void(WINAPI *)())GetProcAddress(module, "FinishWorldLighting")
                                  : nullptr;
                if (finish)
                    finish();
                auto gradeModule = GetModuleHandleW(L"alpha-lookalike.dll");
                auto grade = gradeModule ? (void(WINAPI *)())GetProcAddress(gradeModule,
                                                                            "FinishAlphaLookalike")
                                         : nullptr;
                if (grade)
                    grade();
                apply();
            }
        }
        return;
    }
    ID3D11Buffer *worldBuffers[6]{};
    c->VSGetConstantBuffers(0, 6, worldBuffers);
    bool native = true;
    const UINT sizes[] = {64, 64, 64, 16, 320, 48};
    for (int i = 0; i < 6; i++) {
        D3D11_BUFFER_DESC d{};
        if (worldBuffers[i])
            worldBuffers[i]->GetDesc(&d);
        if (d.ByteWidth != sizes[i])
            native = false;
        drop(worldBuffers[i]);
    }
    if (!native)
        return;
    UINT n = 1;
    D3D11_VIEWPORT vp{};
    c->RSGetViewports(&n, &vp);
    if (!n || vp.Width < 320 || vp.Height < 180 || vp.TopLeftX || vp.TopLeftY)
        return;
    preparedDraw.valid = true;
    memcpy(preparedDraw.projection, projection, 64);
    preparedDraw.viewport = vp;
    if (!target) {
        ID3D11Buffer *viewBuffer = nullptr;
        c->VSGetConstantBuffers(1, 1, &viewBuffer);
        float camera[16]{};
        bool known = readBuffer(viewBuffer, camera, 64);
        drop(viewBuffer);
        if (known) {
            if (cameraKnown) {
                bool cut = false;
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
        }
        context = c;
        viewport = vp;
        c->OMGetRenderTargets(1, &target, nullptr);
    }
}
static ID3D11Buffer *jitter(ID3D11DeviceContext *c) {
    if (internal || !enabled)
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
        ID3D11Buffer *buffers[6]{};
        c->VSGetConstantBuffers(0, 6, buffers);
        const UINT sizes[] = {64, 64, 64, 16, 320, 48};
        bool native = true;
        for (int i = 0; i < 6; i++) {
            D3D11_BUFFER_DESC d{};
            if (buffers[i])
                buffers[i]->GetDesc(&d);
            native &= d.ByteWidth == sizes[i];
            drop(buffers[i]);
        }
        UINT count = 1;
        c->RSGetViewports(&count, &vp);
        if (!native || !count || vp.Width < 320 || vp.Height < 180 || vp.TopLeftX || vp.TopLeftY ||
            !initialize(c)) {
            drop(b);
            return nullptr;
        }
    }
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
static void attach() {
    if (table || !nativeScene::supported())
        return;
    enabled = GetPrivateProfileIntW(L"TAA", L"Enabled", 1, L".\\Mods\\TemporalAA.ini") != 0;
    historyWeight = std::clamp((int)GetPrivateProfileIntW(L"TAA", L"HistoryPercent", 65,
                                                          L".\\Mods\\TemporalAA.ini"),
                               0, 85) *
                    .01f;
    blurStrength = std::clamp((int)GetPrivateProfileIntW(L"TAA", L"BlurPercent", 12,
                                                         L".\\Mods\\TemporalAA.ini"),
                              0, 40) *
                   .01f;
    jitterScale = std::clamp((int)GetPrivateProfileIntW(L"TAA", L"JitterPercent", 75,
                                                        L".\\Mods\\TemporalAA.ini"),
                             25, 100) *
                  .01f;
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
    Hook hooks[] = {{12, (void *)di, (void **)&oldDI},
                    {13, (void *)draw, (void **)&oldD},
                    {14, (void *)map, (void **)&oldMap},
                    {15, (void *)unmap, (void **)&oldUnmap},
                    {48, (void *)update, (void **)&oldUpdate}};
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
