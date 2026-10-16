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
#include "BufferLayout.h"
#include "CpuHookTimer.h"
#include "CameraReprojection.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
namespace taa {
static GpuPassTimer gpuTimer;
static CpuHookTimer hookTimer;
static reprojection::CameraHistory cameraHistory;
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
    D3D11_RESOURCE_DIMENSION dimension{};
    r->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
        return 0;
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer *>(r)->GetDesc(&d);
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
// Sample history where each surface was last frame (from scene depth and the
// camera) instead of at the same screen pixel. See CameraReprojection.h.
static bool cameraReprojection = true;

// What before() learned about the current draw, reused by jitter().
struct PreparedDraw {
    bool checked = false; // before() ran for the current draw.
    bool valid = false;   // ...and found a full-screen perspective scene draw.
    float projection[16]{};
    D3D11_VIEWPORT viewport{};
    ID3D11Buffer *original = nullptr; // Owned until jitter() takes it or the next draw resets it.
};
static PreparedDraw preparedDraw;
static BufferLayout bufferLayout;
static UINT frame = 0;
static bool historyValid = false;
static float jitterX = 0, jitterY = 0; // Current jitter in pixels.
// History is double-buffered: the resolve reads `history` and renders the new
// history into `nextHistory` (single-sample), which is then shown in the scene
// target with a one-tap blit and becomes `history`. This replaces resolving
// the (multisampled) scene target back into history every frame.
static ID3D11Texture2D *history = nullptr, *nextHistory = nullptr;
static ID3D11ShaderResourceView *historyView = nullptr, *nextHistoryView = nullptr;
static ID3D11RenderTargetView *historyTarget = nullptr, *nextHistoryTarget = nullptr;
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
static ID3D11DepthStencilView *sceneDepth = nullptr; // ...and its depth buffer.
// Native (unjittered) camera of this frame's world draws, for reprojection.
static float sceneView[16]{}, sceneProjection[16]{};
static bool sceneCameraValid = false;
static D3D11_VIEWPORT viewport{};
static ID3D11VertexShader *vs = nullptr;
static ID3D11PixelShader *ps = nullptr, *blit = nullptr;
static ID3D11Buffer *params = nullptr;
static ID3D11RasterizerState *raster = nullptr;
// Resolve shader constants (cbuffer Resolve).
struct ResolveParameters {
    float weight, valid, jitterX, jitterY;
    float blurStrength, edgeWeight, reproject, depthTolerance;
    float clipToPrevious[16];
    float previousJitter[4];
};
static_assert(sizeof(ResolveParameters) == 112, "matches cbuffer Resolve");
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

// vs: full-screen triangle from SV_VertexID. ps: blends this frame with
// history found where this pixel's surface was last frame (camera
// reprojection from scene depth), bounded to this frame's neighborhood.
static const char *code = R"(
cbuffer Resolve:register(b0){
    float weight,valid,jitterX,jitterY;
    float blurStrength,edgeWeight,reproject,depthTolerance;
    row_major float4x4 clipToPrevious; // Current jittered clip -> previous jittered clip.
    float4 previousJitter;             // xy: last frame's jitter in pixels.
};
Texture2D<float4> scene:register(t0);
Texture2D<float4> history:register(t1);
Texture2D<float> depth:register(t2);
Texture2D<float> previousDepth:register(t3);
SamplerState linearSampler:register(s0);

float4 vs(uint id:SV_VertexID):SV_POSITION{
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}

float maxChannel(float3 value){return max(value.r,max(value.g,value.b));}

// Catmull-Rom history lookup in five bilinear taps (the four corner taps have
// negligible weight). Unlike one bilinear tap it does not soften the image a
// little more on every frame of camera motion.
float3 historyAt(float2 uv,float2 size){
    float2 position=uv*size,center=floor(position-.5)+.5,f=position-center;
    float2 w0=f*(-.5+f*(1-.5*f)),w1=1+f*f*(-2.5+1.5*f),w2=f*(.5+f*(2-1.5*f)),w3=f*f*(-.5+.5*f);
    float2 w12=w1+w2,t0=(center-1)/size,t3=(center+2)/size,t12=(center+w2/w12)/size;
    float3 sum=history.SampleLevel(linearSampler,float2(t12.x,t0.y),0).rgb*(w12.x*w0.y)
              +history.SampleLevel(linearSampler,float2(t0.x,t12.y),0).rgb*(w0.x*w12.y)
              +history.SampleLevel(linearSampler,t12,0).rgb*(w12.x*w12.y)
              +history.SampleLevel(linearSampler,float2(t3.x,t12.y),0).rgb*(w3.x*w12.y)
              +history.SampleLevel(linearSampler,float2(t12.x,t3.y),0).rgb*(w12.x*w3.y);
    float total=w12.x*w0.y+w0.x*w12.y+w12.x*w12.y+w3.x*w12.y+w12.x*w3.y;
    return max(sum/total,0); // The negative lobes can undershoot at hard edges.
}

float4 ps(float4 p:SV_POSITION):SV_TARGET{
    uint w,h;
    scene.GetDimensions(w,h);
    float2 size=float2(w,h);
    float2 uv=(p.xy+float2(jitterX,jitterY))/size;
    float4 current=scene.SampleLevel(linearSampler,uv,0);
    float3 lo=current.rgb,hi=current.rgb;
    float3 left=current.rgb,right=current.rgb,up=current.rgb,down=current.rgb;
    [unroll]for(int y=-1;y<=1;y++)
        [unroll]for(int x=-1;x<=1;x++){
            // The center is already fetched above; reuse it instead of sampling twice.
            if(x==0&&y==0)continue;
            float3 c=scene.SampleLevel(linearSampler,uv+float2(x,y)/size,0).rgb;
            lo=min(lo,c);
            hi=max(hi,c);
            if(y==0&&x==-1)left=c;
            if(y==0&&x==1)right=c;
            if(x==0&&y==-1)up=c;
            if(x==0&&y==1)down=c;
        }
    float accepted=valid;
    float3 raw;
    [branch]if(reproject>.5){
        // Use the nearest depth of a small cross, so a silhouette's edge pixels
        // follow the foreground surface instead of smearing it over the background.
        int2 pixel=clamp(int2(uv*size),int2(0,0),int2(w,h)-1),nearest=pixel;
        float d=depth.Load(int3(pixel,0));
        const int2 cross[4]={int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
        [unroll]for(int i=0;i<4;i++){
            int2 q=clamp(pixel+cross[i],int2(0,0),int2(w,h)-1);
            float dq=depth.Load(int3(q,0));
            if(dq<d){d=dq;nearest=q;}
        }
        float2 nearestUV=(float2(nearest)+.5)/size;
        // Sky/far-plane pixels still reproject: that corrects camera rotation.
        float4 previous=mul(clipToPrevious,float4(nearestUV*float2(2,-2)+float2(-1,1),d,1));
        float3 previousNdc=previous.xyz/max(previous.w,1e-8);
        float2 motion=previousNdc.xy*float2(.5,-.5)+.5-nearestUV;
        // Last frame's jittered position of this surface, then its resolved
        // (unjittered) history coordinate.
        float2 previousUV=uv+motion;
        float2 historyUV=previousUV-previousJitter.xy/size;
        bool inside=previous.w>0&&all(historyUV>=0)&&all(historyUV<=1);
        // Reject history for surfaces that were hidden or elsewhere last frame.
        int2 previousPixel=clamp(int2(previousUV*size),int2(0,0),int2(w,h)-1);
        float previousSurface=previousDepth.Load(int3(previousPixel,0));
        float tolerance=depthTolerance+4*(abs(ddx(previousNdc.z))+abs(ddy(previousNdc.z)));
        bool sameSurface=d>=.99999||abs(previousSurface-previousNdc.z)<=tolerance;
        accepted*=inside&&sameSurface?1:0;
        raw=historyAt(saturate(historyUV),size);
    }else
        raw=history.Load(int3(p.xy,0)).rgb;
    // Bound history to this frame's neighborhood by moving it along the line
    // toward this pixel's own color until it enters the box. The result is
    // always a mix of two real colors. A per-channel clamp could instead land
    // on a box corner no neighbor has, which showed as green/red/yellow/blue
    // lines on edges once history was kept longer.
    float3 offset=raw-current.rgb;
    float3 room=offset>0?hi-current.rgb:lo-current.rgb;
    float3 limits=abs(offset)>1e-5?saturate(room/offset):1;
    float3 old=current.rgb+offset*min(limits.x,min(limits.y,limits.z));
    // Jitter only moves edge coverage within the local range, so history that
    // already lies inside the neighborhood is kept even when it differs from
    // this frame's pixel; rejecting that difference is what made strong edges
    // shimmer. History the clip had to move is a real change (motion,
    // disocclusion, lighting) and is released.
    float outside=maxChannel(abs(raw-old));
    float confidence=1-smoothstep(.02,.12,outside);
    // Edges and thin details carry the jitter's coverage changes; accumulate
    // them longer. Flat areas keep the configured history weight.
    float edge=smoothstep(.04,.20,maxChannel(hi-lo));
    float accumulation=lerp(weight,max(weight,edgeWeight),edge)*accepted*confidence;
    float3 filtered=lerp(current.rgb,(current.rgb*4+left+right+up+down)/8,blurStrength);
    // Luminance-weighted blend (Karis): bright samples cannot dominate the
    // average across jitter phases, which steadies bright/dark edges.
    float currentWeight=(1-accumulation)/(1+maxChannel(filtered));
    float historyWeight=accumulation/(1+maxChannel(old));
    return float4((filtered*currentWeight+old*historyWeight)/(currentWeight+historyWeight),current.a);
}

// Shows the new history in the scene target (every sample of an MSAA pixel).
float4 show(float4 p:SV_POSITION):SV_TARGET{return history.Load(int3(p.xy,0));}
)";

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
    drop(preparedDraw.original);
    if (!finished) // The previous frame never resolved.
        historyValid = false;
    drop(target);
    drop(sceneDepth);
    sceneCameraValid = false;
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
    for (int i = 0; i < 3 && ok; i++) {
        bool pixel = i > 0;
        const char *entry = i == 0 ? "vs" : i == 1 ? "ps" : "show";
        ID3DBlob *b = nullptr, *error = nullptr;
        ok = SUCCEEDED(D3DCompile(code, strlen(code), "TemporalResolve", nullptr, nullptr, entry,
                                  pixel ? "ps_4_0" : "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  &b, &error));
        if (error)
            log((char *)error->GetBufferPointer());
        if (ok)
            ok = SUCCEEDED(pixel ? device->CreatePixelShader(b->GetBufferPointer(),
                                                             b->GetBufferSize(), nullptr,
                                                             i == 1 ? &ps : &blit)
                                 : device->CreateVertexShader(b->GetBufferPointer(),
                                                              b->GetBufferSize(), nullptr, &vs));
        drop(b);
        drop(error);
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(ResolveParameters);
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
    drop(historyTarget);
    drop(history);
    drop(nextHistoryView);
    drop(nextHistoryTarget);
    drop(nextHistory);
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
    if (FAILED(device->CreateTexture2D(&d, nullptr, &copy)) ||
        FAILED(device->CreateShaderResourceView(copy, nullptr, &view)))
        return false;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &history)) &&
           SUCCEEDED(device->CreateShaderResourceView(history, nullptr, &historyView)) &&
           SUCCEEDED(device->CreateRenderTargetView(history, nullptr, &historyTarget)) &&
           SUCCEEDED(device->CreateTexture2D(&d, nullptr, &nextHistory)) &&
           SUCCEEDED(device->CreateShaderResourceView(nextHistory, nullptr, &nextHistoryView)) &&
           SUCCEEDED(device->CreateRenderTargetView(nextHistory, nullptr, &nextHistoryTarget));
}

// ===========================================================================
// Resolve pass
// ===========================================================================

// Copies scene depth and fills the reprojection constants for this frame.
static bool prepareReprojection(ResolveParameters &values) {
    double viewProjection[16];
    reprojection::jitteredViewProjection(sceneProjection, sceneView, jitterX, jitterY,
                                         viewport.Width, viewport.Height, viewProjection);
    float jitter[2] = {jitterX, jitterY};
    if (!cameraHistory.prepare(device, context, sceneDepth, width, height, viewProjection, jitter,
                               values.clipToPrevious, values.previousJitter))
        return false;
    values.reproject = 1;
    return true;
}
// Logs when reprojection starts or stops being used, with the reason.
static void reportReprojection(bool active) {
    static int reported = -1;
    if (reported == (int)active)
        return;
    reported = (int)active;
    char message[192];
    sprintf_s(message, "Camera reprojection %s: %s.", active ? "active" : "unavailable",
              !cameraReprojection  ? "disabled in TemporalAA.ini"
              : !sceneCameraValid ? "no world camera this frame"
                                  : cameraHistory.status());
    log(message);
}

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
                   "TAA (input copy/resolve + output)");
    context1->SwapDeviceContextState(state, &saved);
    // Copy (or MSAA-resolve) the scene so it can be read while writing `target`.
    if (td.SampleDesc.Count > 1)
        context->ResolveSubresource(copy, 0, source, 0, rt.Format);
    else
        context->CopyResource(copy, source);
    context->OMSetRenderTargets(1, &nextHistoryTarget, nullptr);
    context->OMSetBlendState(nullptr, nullptr, ~0u);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetState(raster);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    if (!historyValid) {
        context->CopyResource(history, copy);
        cameraHistory.invalidate();
    }
    gpuTimer.split(context);
    ResolveParameters values{};
    values.weight = historyWeight;
    values.valid = historyValid ? 1.f : 0.f;
    values.jitterX = jitterX;
    values.jitterY = jitterY;
    values.blurStrength = blurStrength;
    values.edgeWeight = edgeHistoryWeight;
    values.depthTolerance = .0005f;
    bool reproject = cameraReprojection && sceneCameraValid && prepareReprojection(values);
    if (!reproject)
        cameraHistory.invalidate();
    reportReprojection(reproject);
    context->UpdateSubresource(params, 0, nullptr, &values, 0, 0);
    context->PSSetConstantBuffers(0, 1, &params);
    ID3D11ShaderResourceView *inputs[] = {view, historyView};
    context->PSSetShaderResources(0, 2, inputs);
    if (reproject)
        cameraHistory.bind(context, 2);
    context->PSSetSamplers(0, 1, &sampler);
    oldD(context, 3, 0); // Full-screen triangle into the new history.
    ID3D11ShaderResourceView *empty[4] = {};
    context->PSSetShaderResources(0, 4, empty);
    // Show it in the scene target; every sample of an MSAA pixel gets the
    // same colour, exactly what resolving the target back into history saw.
    context->OMSetRenderTargets(1, &target, nullptr);
    context->PSSetShader(blit, nullptr, 0);
    context->PSSetShaderResources(1, 1, &nextHistoryView);
    oldD(context, 3, 0);
    context->PSSetShaderResources(0, 4, empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    std::swap(history, nextHistory);
    std::swap(historyView, nextHistoryView);
    std::swap(historyTarget, nextHistoryTarget);
    if (reproject)
        cameraHistory.commit(context);
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
    BoundConstantBuffers buffers(c);
    return bufferLayout.inspect(buffers.data(), world);
}

// A full-screen scene viewport (not a preview or a small render target).
static bool sceneViewport(UINT count, const D3D11_VIEWPORT &vp) {
    return !(!count || vp.Width < 320 || vp.Height < 180 || vp.TopLeftX || vp.TopLeftY);
}

// Lets the other Steam mods finish their world passes before the resolve. All
// mods are loaded before the game renders, so the exports are looked up once.
static void finishOtherMods() {
    using Finish = void(WINAPI *)();
    auto find = [](const wchar_t *dll, const char *name) {
        auto module = GetModuleHandleW(dll);
        return module ? (Finish)GetProcAddress(module, name) : nullptr;
    };
    static Finish finish = find(L"MoonandShadow.dll", "FinishWorldLighting"),
                  grade = find(L"alpha-lookalike.dll", "FinishAlphaLookalike");
    if (finish)
        finish();
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
    // Only a target drawn with depth is the world scene; its depth and camera
    // drive this frame's reprojection.
    c->OMGetRenderTargets(1, &target, &sceneDepth);
    if (!sceneDepth) {
        drop(target);
        return;
    }
    memcpy(sceneView, camera, 64);
    memcpy(sceneProjection, projection, 64);
    sceneCameraValid = true;
}

// Runs before every draw. A perspective draw with the native world layout is
// "prepared" for jittering; an orthographic draw into the scene target means
// the world is complete, so the resolve runs.
static void before(ID3D11DeviceContext *c) {
    drop(preparedDraw.original);
    preparedDraw.checked = true;
    preparedDraw.valid = false;
    if (!enabled || internal || finished)
        return;
    BoundConstantBuffers buffers(c);
    float projection[16]{};
    bool known = readBuffer(buffers[0], projection, 64);
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
    if (!bufferLayout.inspect(buffers.data(), world))
        return;
    UINT n = 1;
    D3D11_VIEWPORT vp{};
    c->RSGetViewports(&n, &vp);
    if (!sceneViewport(n, vp))
        return;
    preparedDraw.valid = true;
    preparedDraw.original = buffers.take(0);
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
    if (preparedDraw.valid && preparedDraw.original) {
        b = preparedDraw.original;
        preparedDraw.original = nullptr;
    } else
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
    ID3D11Buffer *original = nullptr;
    {
        CpuHookTimer::Scope timer(hookTimer);
        before(c);
        original = jitter(c);
    }
    oldDI(c, n, s, b);
    if (original) {
        CpuHookTimer::Scope timer(hookTimer);
        c->VSSetConstantBuffers(0, 1, &original);
        drop(original);
    }
}
static void STDMETHODCALLTYPE draw(ID3D11DeviceContext *c, UINT n, UINT s) {
    ID3D11Buffer *original = nullptr;
    {
        CpuHookTimer::Scope timer(hookTimer);
        before(c);
        original = jitter(c);
    }
    oldD(c, n, s);
    if (original) {
        CpuHookTimer::Scope timer(hookTimer);
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
    auto it = mapped.empty() ? mapped.end() : mapped.find(r);
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

// ===========================================================================
// Shader-readable scene depth
// ===========================================================================

// Reprojection needs this frame's depth in a shader. A depth target created
// without a shader-resource binding must be copied every frame (a full
// multisampled copy at 4-8x MSAA, the largest part of the pass). Like ReShade,
// depth targets are created typeless with that binding added, so the depth is
// read in place; depth-stencil views get the matching typed format. Depth
// values, tests and stencil are unchanged.
static bool readableDepth = true;
constexpr UINT SLOT_CREATE_TEXTURE_2D = 5, SLOT_CREATE_DEPTH_STENCIL_VIEW = 10;
using CreateTexture2D = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
                                                     const D3D11_SUBRESOURCE_DATA *,
                                                     ID3D11Texture2D **);
using CreateDepthView = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, ID3D11Resource *,
                                                     const D3D11_DEPTH_STENCIL_VIEW_DESC *,
                                                     ID3D11DepthStencilView **);
static CreateTexture2D oldCreateTexture2D = nullptr;
static CreateDepthView oldCreateDepthView = nullptr;

static DXGI_FORMAT typedDepth(DXGI_FORMAT typeless) {
    switch (typeless) {
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
static HRESULT STDMETHODCALLTYPE createTexture2D(ID3D11Device *d, const D3D11_TEXTURE2D_DESC *desc,
                                                 const D3D11_SUBRESOURCE_DATA *data,
                                                 ID3D11Texture2D **out) {
    DXGI_FORMAT typeless, readable;
    if (readableDepth && desc && !data && out && (desc->BindFlags & D3D11_BIND_DEPTH_STENCIL) &&
        !(desc->BindFlags & D3D11_BIND_SHADER_RESOURCE) && desc->ArraySize == 1 &&
        desc->MipLevels == 1 && desc->Usage == D3D11_USAGE_DEFAULT && desc->Width >= 256 &&
        reprojection::depthFormats(desc->Format, typeless, readable) &&
        typedDepth(typeless) == desc->Format) {
        D3D11_TEXTURE2D_DESC readableDesc = *desc;
        readableDesc.Format = typeless;
        readableDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(oldCreateTexture2D(d, &readableDesc, nullptr, out)))
            return S_OK;
    }
    return oldCreateTexture2D(d, desc, data, out);
}
static HRESULT STDMETHODCALLTYPE createDepthView(ID3D11Device *d, ID3D11Resource *resource,
                                                 const D3D11_DEPTH_STENCIL_VIEW_DESC *desc,
                                                 ID3D11DepthStencilView **out) {
    ID3D11Texture2D *texture = nullptr;
    if (resource && (!desc || desc->Format == DXGI_FORMAT_UNKNOWN) &&
        SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&texture))) {
        D3D11_TEXTURE2D_DESC td{};
        texture->GetDesc(&td);
        drop(texture);
        DXGI_FORMAT typed = typedDepth(td.Format);
        if (typed != DXGI_FORMAT_UNKNOWN) {
            D3D11_DEPTH_STENCIL_VIEW_DESC view{};
            if (desc)
                view = *desc;
            else {
                view.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                                                             : D3D11_DSV_DIMENSION_TEXTURE2D;
            }
            view.Format = typed;
            return oldCreateDepthView(d, resource, &view, out);
        }
    }
    return oldCreateDepthView(d, resource, desc, out);
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
    cameraReprojection = GetPrivateProfileIntW(L"TAA", L"CameraReprojection", 1, ini) != 0;
    readableDepth = cameraReprojection &&
                    GetPrivateProfileIntW(L"TAA", L"ReadableDepth", 1, ini) != 0;
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
    // Device hooks for shader-readable depth (both or neither).
    if (ok && readableDepth) {
        auto dv = *(void ***)d;
        oldCreateTexture2D = (CreateTexture2D)dv[SLOT_CREATE_TEXTURE_2D];
        oldCreateDepthView = (CreateDepthView)dv[SLOT_CREATE_DEPTH_STENCIL_VIEW];
        if (patch(dv + SLOT_CREATE_DEPTH_STENCIL_VIEW, (void *)createDepthView) &&
            !patch(dv + SLOT_CREATE_TEXTURE_2D, (void *)createTexture2D))
            patch(dv + SLOT_CREATE_DEPTH_STENCIL_VIEW, (void *)oldCreateDepthView);
    }
    if (ok) {
        table = v;
        log(cameraReprojection
                ? "TemporalAA enabled; F9 toggles. Depth-based camera reprojection requested."
                : "TemporalAA enabled; F9 toggles. Same-pixel history (CameraReprojection=0).");
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
        taa::hookTimer.present("Mods\\TemporalAA.log");
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
