// Camera reprojection: history must be fetched where a surface was last
// frame, and released when the depth shows a different surface was there.
#include "../TemporalAA.cpp"
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
constexpr UINT Size = 64;
constexpr int Shift = 4; // Camera move per frame, in pixels.

struct Scene {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Texture2D *color = nullptr, *staging = nullptr, *depthTexture = nullptr;
    ID3D11RenderTargetView *target = nullptr;
    ID3D11DepthStencilView *depth = nullptr;

    Scene(ID3D11Device *d, ID3D11DeviceContext *c) : device(d), context(c) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = Size;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        assert(SUCCEEDED(d->CreateTexture2D(&desc, nullptr, &color)));
        assert(SUCCEEDED(d->CreateRenderTargetView(color, nullptr, &target)));
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        assert(SUCCEEDED(d->CreateTexture2D(&desc, nullptr, &staging)));
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        assert(SUCCEEDED(d->CreateTexture2D(&desc, nullptr, &depthTexture)));
        assert(SUCCEEDED(d->CreateDepthStencilView(depthTexture, nullptr, &depth)));
    }
    ~Scene() {
        taa::drop(depth);
        taa::drop(depthTexture);
        taa::drop(target);
        taa::drop(staging);
        taa::drop(color);
    }
    // A 1-pixel black/white checkerboard: every pixel's neighborhood spans the
    // full color range, so any fetched history survives the clip unchanged.
    void checkerboard() {
        std::vector<unsigned char> pixels(Size * Size * 4);
        for (UINT y = 0; y < Size; ++y)
            for (UINT x = 0; x < Size; ++x) {
                unsigned char v = (x + y) % 2 ? 255 : 0;
                auto i = (y * Size + x) * 4;
                pixels[i] = pixels[i + 1] = pixels[i + 2] = v;
                pixels[i + 3] = 255;
            }
        context->UpdateSubresource(color, 0, nullptr, pixels.data(), Size * 4, 0);
    }
    // Resolves one frame with the given camera translation (in pixels).
    void resolve(float cameraPixels, float clearDepth) {
        checkerboard();
        context->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, clearDepth, 0);
        float projection[16]{}, view[16]{};
        projection[0] = projection[5] = projection[10] = projection[15] = 1;
        view[0] = view[5] = view[10] = view[15] = 1;
        view[3] = 2.f * cameraPixels / Size; // NDC x per pixel is 2/width.
        memcpy(taa::sceneProjection, projection, 64);
        memcpy(taa::sceneView, view, 64);
        taa::sceneCameraValid = true;
        taa::drop(taa::sceneDepth);
        taa::sceneDepth = depth;
        depth->AddRef();
        taa::target = target;
        target->AddRef();
        context->OMSetRenderTargets(1, &target, nullptr);
        taa::viewport = {0, 0, (float)Size, (float)Size, 0, 1};
        taa::finished = false;
        taa::jitterX = taa::jitterY = 0;
        assert(taa::apply());
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    // Replaces the stored history with a horizontal red gradient.
    void seedHistory(int offset) {
        std::vector<unsigned char> pixels(Size * Size * 4);
        for (UINT y = 0; y < Size; ++y)
            for (UINT x = 0; x < Size; ++x) {
                auto i = (y * Size + x) * 4;
                pixels[i] = (unsigned char)std::clamp(((int)x - offset) * 4, 0, 255);
                pixels[i + 1] = pixels[i + 2] = 128;
                pixels[i + 3] = 255;
            }
        context->UpdateSubresource(taa::history, 0, nullptr, pixels.data(), Size * 4, 0);
    }
    std::vector<unsigned char> read() {
        context->CopyResource(staging, color);
        D3D11_MAPPED_SUBRESOURCE map{};
        assert(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map)));
        std::vector<unsigned char> pixels(Size * Size * 4);
        for (UINT y = 0; y < Size; ++y)
            memcpy(&pixels[y * Size * 4], (unsigned char *)map.pData + y * map.RowPitch, Size * 4);
        context->Unmap(staging, 0);
        return pixels;
    }
};

// Largest red difference between two outputs, away from the borders.
int interiorDifference(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b) {
    int worst = 0;
    for (UINT y = 4; y < Size - 4; ++y)
        for (UINT x = Shift + 4; x < Size - 4; ++x) {
            auto i = (y * Size + x) * 4;
            worst = std::max(worst, abs(int(a[i]) - int(b[i])));
        }
    return worst;
}

// Runs: establish the camera at 0, seed history, then resolve at `camera`.
std::vector<unsigned char> run(Scene &scene, float camera, int historyOffset, float firstDepth,
                               float secondDepth) {
    taa::historyValid = false;
    scene.resolve(0, firstDepth);
    scene.seedHistory(historyOffset);
    scene.resolve(camera, secondDepth);
    return scene.read();
}
} // namespace

int main() {
    // Matrix helpers: inverse round trip and singular rejection.
    double m[16] = {2, 0, 0, 1, 0, 3, 0, -2, 0, 0, 1, .5, 0, 0, 0, 1}, inv[16], id[16];
    assert(reprojection::inverse(m, inv));
    reprojection::multiply(m, inv, id);
    for (int i = 0; i < 16; ++i)
        assert(fabs(id[i] - (i % 5 == 0 ? 1 : 0)) < 1e-12);
    double singular[16]{};
    assert(!reprojection::inverse(singular, inv));
    float toPrevious[16];
    assert(reprojection::clipToPrevious(m, m, toPrevious));
    for (int i = 0; i < 16; ++i)
        assert(fabsf(toPrevious[i] - (i % 5 == 0 ? 1.f : 0.f)) < 1e-6f);

    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &d, nullptr, &c)));
    taa::oldD = (taa::D)(*(void ***)c)[13];
    assert(taa::initialize(c));
    taa::blurStrength = 0;
    {
        Scene scene(d, c);
        // The camera moves 4 px: the surface now at x was at x-4 last frame.
        auto moved = run(scene, Shift, 0, .5f, .5f);
        assert(!strcmp(taa::cameraHistory.status(), "active"));
        // Equivalent: static camera with history already shifted by 4 px.
        auto shifted = run(scene, 0, Shift, .5f, .5f);
        int difference = interiorDifference(moved, shifted);
        printf("Moved camera vs pre-shifted history, worst red difference: %d\n", difference);
        assert(difference <= 2);
        // Same-pixel history (reprojection off) fetches the wrong surface.
        taa::cameraReprojection = false;
        auto samePixel = run(scene, Shift, 0, .5f, .5f);
        taa::cameraReprojection = true;
        assert(interiorDifference(samePixel, shifted) >= 10);
        // A different surface (depth .2 -> .5) was there last frame: reject
        // history, leaving exactly this frame's checkerboard.
        auto uncovered = run(scene, Shift, 0, .2f, .5f);
        for (UINT y = 4; y < Size - 4; ++y)
            for (UINT x = Shift + 4; x < Size - 4; ++x) {
                auto i = (y * Size + x) * 4;
                int expected = (x + y) % 2 ? 255 : 0;
                assert(abs(int(uncovered[i]) - expected) <= 1);
            }
        // The resolve leaves no reprojection inputs bound.
        ID3DDeviceContextState *saved = nullptr;
        taa::context1->SwapDeviceContextState(taa::state, &saved);
        ID3D11ShaderResourceView *views[4]{};
        c->PSGetShaderResources(0, 4, views);
        for (auto v : views)
            assert(!v);
        taa::context1->SwapDeviceContextState(saved, nullptr);
        taa::drop(saved);
    }
    taa::drop(taa::sceneDepth);
    puts("Camera reprojection: moved history, same-pixel contrast, disocclusion rejection, "
         "binding release PASS");
    taa::drop(c);
    taa::drop(d);
}
