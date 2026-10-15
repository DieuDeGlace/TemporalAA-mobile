#pragma once
// Depth-based camera reprojection for the TAA resolve.
//
// The resolve maps each output pixel to where its surface was last frame:
// current jittered clip position (screen + scene depth) -> world -> previous
// jittered clip position. The combined matrix previousViewProjection *
// inverse(currentViewProjection) is built here in double precision and
// uploaded once, so large world coordinates do not lose float precision.
//
// Scene depth is copied (or, for MSAA, resolved to the nearest sample by a
// small compute shader) each frame; the previous frame's copy lets the shader
// reject history for newly uncovered surfaces.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <utility>

namespace reprojection {
// Row-major 4x4 helpers; clip = matrix * column vector.
inline void multiply(const double *a, const double *b, double *out) {
    double result[16]{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                result[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    memcpy(out, result, sizeof(result));
}
inline bool inverse(const double *matrix, double *out) {
    double rows[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            rows[r][c] = matrix[r * 4 + c];
        rows[r][r + 4] = 1;
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs(rows[r][c]) > std::abs(rows[pivot][c]))
                pivot = r;
        if (!std::isfinite(rows[pivot][c]) || std::abs(rows[pivot][c]) < 1e-12)
            return false;
        for (int k = 0; k < 8; ++k)
            std::swap(rows[c][k], rows[pivot][k]);
        double divisor = rows[c][c];
        for (int k = 0; k < 8; ++k)
            rows[c][k] /= divisor;
        for (int r = 0; r < 4; ++r) {
            if (r == c)
                continue;
            double factor = rows[r][c];
            for (int k = 0; k < 8; ++k)
                rows[r][k] -= factor * rows[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = rows[r][c + 4];
            if (!std::isfinite(out[r * 4 + c]))
                return false;
        }
    return true;
}
// Jittered view-projection: the native projection with this frame's subpixel
// offset (exactly as the draw hook uploads it), times the native view.
inline void jitteredViewProjection(const float *projection, const float *view, float jitterX,
                                   float jitterY, float width, float height, double *out) {
    double p[16], v[16];
    for (int i = 0; i < 16; ++i) {
        p[i] = projection[i];
        v[i] = view[i];
    }
    for (int i = 0; i < 4; ++i) {
        p[i] += 2.0 * jitterX / width * p[12 + i];
        p[4 + i] -= 2.0 * jitterY / height * p[12 + i];
    }
    multiply(p, v, out);
}
// previous * inverse(current): current jittered clip -> previous jittered clip.
inline bool clipToPrevious(const double *current, const double *previous, float *out) {
    double inverted[16], combined[16];
    if (!inverse(current, inverted))
        return false;
    multiply(previous, inverted, combined);
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(combined[i]))
            return false;
        out[i] = (float)combined[i];
    }
    return true;
}

// Typeless storage and the depth-readable view format for a depth target.
inline bool depthFormats(DXGI_FORMAT format, DXGI_FORMAT &typeless, DXGI_FORMAT &readable) {
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
        readable = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        typeless = DXGI_FORMAT_R24G8_TYPELESS;
        readable = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        typeless = DXGI_FORMAT_R32_TYPELESS;
        readable = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        typeless = DXGI_FORMAT_R16_TYPELESS;
        readable = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

// Current and previous scene depth plus the previous frame's jittered camera.
class CameraHistory {
    ID3D11Texture2D *current = nullptr, *previous = nullptr, *multisampled = nullptr;
    ID3D11ShaderResourceView *currentView = nullptr, *previousView = nullptr,
                             *multisampledView = nullptr;
    ID3D11UnorderedAccessView *resolvedUav = nullptr, *previousUav = nullptr;
    // A scene depth texture that is already shader-readable (ReShade, or the
    // device hook in TemporalAA.cpp) is read in place instead of copied.
    ID3D11Texture2D *directSource = nullptr;
    ID3D11ShaderResourceView *directView = nullptr;
    ID3D11ComputeShader *nearestSample = nullptr;
    UINT width = 0, height = 0, samples = 0, quality = 0;
    DXGI_FORMAT family = DXGI_FORMAT_UNKNOWN;
    double previousMatrix[16]{}, currentMatrix[16]{};
    float previousJitter[2]{}, currentJitter[2]{};
    bool valid = false;
    const char *statusText = "not prepared";

    template <class T> static void release(T *&p) {
        if (p)
            p->Release();
        p = nullptr;
    }
    void clear() {
        release(multisampledView);
        release(resolvedUav);
        release(previousUav);
        release(directView);
        release(directSource);
        release(multisampled);
        release(currentView);
        release(previousView);
        release(current);
        release(previous);
        width = height = samples = quality = 0;
        family = DXGI_FORMAT_UNKNOWN;
        valid = false;
    }
    bool allocate(ID3D11Device *device, D3D11_TEXTURE2D_DESC desc, DXGI_FORMAT typeless,
                  DXGI_FORMAT readable) {
        clear();
        bool msaa = desc.SampleDesc.Count > 1;
        bool sourceReadable = (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
        desc.Format = typeless;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = desc.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = readable;
        bool ok = true;
        if (msaa) {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            // Only a non-readable source needs a full multisampled copy.
            if (!sourceReadable)
                ok = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &multisampled)) &&
                     SUCCEEDED(device->CreateShaderResourceView(multisampled, &view,
                                                                &multisampledView));
            // The nearest-sample resolve writes a single-sample float copy.
            desc.SampleDesc = {1, 0};
            desc.Format = view.Format = DXGI_FORMAT_R32_FLOAT;
            desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        }
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &current)) &&
             SUCCEEDED(device->CreateShaderResourceView(current, &view, &currentView));
        if (msaa)
            ok = ok && SUCCEEDED(device->CreateUnorderedAccessView(current, nullptr, &resolvedUav));
        // For MSAA both single-sample depth textures take the nearest-sample
        // UAV, so commit() can swap them instead of copying.
        if (!msaa)
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &previous)) &&
             SUCCEEDED(device->CreateShaderResourceView(previous, &view, &previousView));
        if (msaa)
            ok = ok && SUCCEEDED(device->CreateUnorderedAccessView(previous, nullptr, &previousUav));
        if (!ok)
            clear();
        return ok;
    }
    // The source's own multisampled view when it is shader-readable.
    ID3D11ShaderResourceView *sourceView(ID3D11Device *device, ID3D11Texture2D *source,
                                         DXGI_FORMAT readable) {
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
            return nullptr;
        if (source != directSource) {
            release(directView);
            release(directSource);
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = readable;
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            if (FAILED(device->CreateShaderResourceView(source, &view, &directView)))
                return nullptr;
            directSource = source;
            source->AddRef();
        }
        return directView;
    }
    bool resolveNearest(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *source,
                        DXGI_FORMAT readable) {
        // D3D11 cannot ResolveSubresource a depth texture; keep the nearest sample.
        if (!nearestSample) {
            static const char *shader = R"(
Texture2DMS<float> source:register(t0);RWTexture2D<float> output:register(u0);
[numthreads(8,8,1)]void main(uint3 pixel:SV_DispatchThreadID){
 uint w,h,n;source.GetDimensions(w,h,n);if(pixel.x>=w||pixel.y>=h)return;
 float nearest=1;[loop]for(uint i=0;i<n;i++)nearest=min(nearest,source.Load(pixel.xy,i));
 output[pixel.xy]=nearest;
})";
            ID3DBlob *blob = nullptr;
            HRESULT result = D3DCompile(shader, strlen(shader), "NearestDepth", nullptr, nullptr,
                                        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob,
                                        nullptr);
            if (SUCCEEDED(result))
                result = device->CreateComputeShader(blob->GetBufferPointer(),
                                                     blob->GetBufferSize(), nullptr, &nearestSample);
            release(blob);
            if (FAILED(result))
                return false;
        }
        ID3D11ShaderResourceView *input = sourceView(device, source, readable);
        if (!input) {
            if (!multisampled)
                return false;
            context->CopyResource(multisampled, source);
            input = multisampledView;
        }
        context->CSSetShader(nearestSample, nullptr, 0);
        context->CSSetShaderResources(0, 1, &input);
        context->CSSetUnorderedAccessViews(0, 1, &resolvedUav, nullptr);
        context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        ID3D11ShaderResourceView *noView = nullptr;
        ID3D11UnorderedAccessView *noUav = nullptr;
        context->CSSetShaderResources(0, 1, &noView);
        context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
        return true;
    }

  public:
    ~CameraHistory() {
        clear();
        release(nearestSample);
    }
    const char *status() const {
        return statusText;
    }
    void invalidate() {
        valid = false;
    }
    // Copies this frame's depth and builds the clip-to-previous matrix. On
    // failure the caller falls back to same-pixel history.
    bool prepare(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11DepthStencilView *depth,
                 UINT w, UINT h, const double *viewProjection, const float jitter[2],
                 float clipToPreviousOut[16], float previousJitterOut[2]) {
        double unused[16];
        if (!depth || !inverse(viewProjection, unused)) {
            statusText = !depth ? "missing scene depth" : "singular camera transform";
            valid = false;
            return false;
        }
        ID3D11Resource *resource = nullptr;
        ID3D11Texture2D *source = nullptr;
        depth->GetResource(&resource);
        bool texture =
            SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&source));
        release(resource);
        if (!texture) {
            statusText = "depth is not a 2D texture";
            valid = false;
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        DXGI_FORMAT typeless, readable;
        if (!depthFormats(desc.Format, typeless, readable) || desc.Width != w ||
            desc.Height != h || desc.ArraySize != 1 || desc.MipLevels != 1) {
            statusText = !depthFormats(desc.Format, typeless, readable)
                             ? "unsupported depth format"
                             : "depth size/layout differs from the scene";
            release(source);
            valid = false;
            return false;
        }
        if (!current || width != w || height != h || family != typeless ||
            samples != desc.SampleDesc.Count || quality != desc.SampleDesc.Quality) {
            if (!allocate(device, desc, typeless, readable)) {
                statusText = "depth texture allocation failed";
                release(source);
                return false;
            }
            width = w;
            height = h;
            family = typeless;
            samples = desc.SampleDesc.Count;
            quality = desc.SampleDesc.Quality;
        }
        bool copied = true;
        if (samples > 1)
            copied = resolveNearest(device, context, source, readable);
        else
            context->CopyResource(current, source);
        bool inPlace = samples > 1 && directSource == source;
        release(source);
        if (!copied) {
            statusText = "multisample depth resolve unavailable";
            valid = false;
            return false;
        }
        memcpy(currentMatrix, viewProjection, sizeof(currentMatrix));
        currentJitter[0] = jitter[0];
        currentJitter[1] = jitter[1];
        if (!valid) {
            // First frame (or after a cut): previous = current, so history maps 1:1.
            memcpy(previousMatrix, currentMatrix, sizeof(previousMatrix));
            previousJitter[0] = currentJitter[0];
            previousJitter[1] = currentJitter[1];
            context->CopyResource(previous, current);
        }
        if (!clipToPrevious(currentMatrix, previousMatrix, clipToPreviousOut)) {
            statusText = "singular camera transform";
            valid = false;
            return false;
        }
        previousJitterOut[0] = previousJitter[0];
        previousJitterOut[1] = previousJitter[1];
        statusText = samples == 1 ? "active"
                     : inPlace    ? "active (nearest-sample MSAA depth, read in place)"
                                  : "active (nearest-sample MSAA depth, copied)";
        return true;
    }
    void bind(ID3D11DeviceContext *context, UINT slot) {
        ID3D11ShaderResourceView *views[] = {currentView, previousView};
        context->PSSetShaderResources(slot, 2, views);
    }
    // After the resolve: this frame becomes the previous frame. Swapping the
    // two depth textures (and, for MSAA, their nearest-sample UAVs) avoids a
    // full-size copy; the next prepare() rewrites `current`.
    void commit(ID3D11DeviceContext *) {
        std::swap(current, previous);
        std::swap(currentView, previousView);
        std::swap(resolvedUav, previousUav);
        memcpy(previousMatrix, currentMatrix, sizeof(previousMatrix));
        previousJitter[0] = currentJitter[0];
        previousJitter[1] = currentJitter[1];
        valid = true;
    }
};
} // namespace reprojection
