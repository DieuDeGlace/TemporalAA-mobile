// GPU cost of one complete TAA apply() on the game's configuration: a
// multisampled colour target and multisampled D24S8 depth (no shader-resource
// binding), camera reprojection on. Hardware device; prints median/mean/p95.
//   MsaaBenchmark [width height [copy|readable|noreproject]]
#include "../TemporalAA.cpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
struct Fixture {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    UINT width, height, samples;
    ID3D11Texture2D *color = nullptr, *depthTexture = nullptr;
    ID3D11RenderTargetView *target = nullptr;
    ID3D11DepthStencilView *depth = nullptr;
    Fixture(ID3D11Device *d, ID3D11DeviceContext *c, UINT w, UINT h, UINT n, bool readable)
        : device(d), context(c), width(w), height(h), samples(n) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc = {n, 0};
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        assert(SUCCEEDED(d->CreateTexture2D(&desc, nullptr, &color)));
        assert(SUCCEEDED(d->CreateRenderTargetView(color, nullptr, &target)));
        // As the game creates it, or shader-readable (ReShade / the TAA device hook).
        desc.Format = readable ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | (readable ? D3D11_BIND_SHADER_RESOURCE : 0);
        assert(SUCCEEDED(d->CreateTexture2D(&desc, nullptr, &depthTexture)));
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = n > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        assert(SUCCEEDED(d->CreateDepthStencilView(depthTexture, &dv, &depth)));
    }
    ~Fixture() {
        taa::drop(depth);
        taa::drop(depthTexture);
        taa::drop(target);
        taa::drop(color);
    }
    void frame(unsigned index) {
        float clear[4] = {.2f + .001f * (index % 50), .4f, .6f, 1};
        context->ClearRenderTargetView(target, clear);
        context->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, .5f + .0001f * (index % 7), 0);
        float projection[16]{}, view[16]{};
        projection[0] = projection[5] = projection[15] = 1;
        projection[10] = 1;
        view[0] = view[5] = view[10] = view[15] = 1;
        view[3] = .001f * (index % 9); // a little camera motion
        memcpy(taa::sceneProjection, projection, 64);
        memcpy(taa::sceneView, view, 64);
        taa::sceneCameraValid = true;
        taa::drop(taa::sceneDepth);
        taa::sceneDepth = depth;
        depth->AddRef();
        taa::target = target;
        target->AddRef();
        context->OMSetRenderTargets(1, &target, nullptr);
        taa::viewport = {0, 0, (float)width, (float)height, 0, 1};
        taa::finished = false;
        taa::jitterX = (index % 2) ? .25f : -.25f;
        taa::jitterY = (index % 3) ? .25f : -.25f;
        assert(taa::apply());
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
};
} // namespace

int main(int argc, char **argv) {
    UINT width = argc >= 3 ? (UINT)atoi(argv[1]) : 1920, height = argc >= 3 ? (UINT)atoi(argv[2]) : 1080;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) {
        puts("No hardware D3D11 device.");
        return 0;
    }
    taa::oldD = (taa::D)(*(void ***)context)[13];
    taa::oldUpdate = (taa::Update)(*(void ***)context)[48];
    assert(taa::initialize(context));
    std::string mode = argc >= 4 ? argv[3] : "copy";
    taa::cameraReprojection = mode != "noreproject";
    bool readable = mode == "readable";
    for (UINT samples : {1u, 2u, 4u, 8u}) {
        UINT quality = 0;
        if (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, samples,
                                                         &quality)) || !quality)
            continue;
        Fixture fixture(device, context, width, height, samples, readable);
        taa::historyValid = false;
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr;
        device->CreateQuery(&qd, &disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        device->CreateQuery(&qd, &begin);
        device->CreateQuery(&qd, &end);
        std::vector<double> times;
        for (unsigned i = 0; i < 24 + 120; ++i) {
            bool timed = i >= 24;
            if (timed) {
                context->Begin(disjoint);
                context->End(begin);
            }
            fixture.frame(i);
            if (!timed)
                continue;
            context->End(end);
            context->End(disjoint);
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            while (context->GetData(disjoint, &dj, sizeof dj, 0) == S_FALSE) {
            }
            UINT64 a = 0, b = 0;
            while (context->GetData(begin, &a, sizeof a, 0) == S_FALSE) {
            }
            while (context->GetData(end, &b, sizeof b, 0) == S_FALSE) {
            }
            if (!dj.Disjoint)
                times.push_back(1000.0 * double(b - a) / double(dj.Frequency));
        }
        taa::drop(end);
        taa::drop(begin);
        taa::drop(disjoint);
        std::sort(times.begin(), times.end());
        double mean = 0;
        for (double t : times)
            mean += t;
        mean /= times.size();
        printf("%ux%u %ux MSAA: apply() GPU median=%.4f mean=%.4f p95=%.4f ms (%zu samples, %s)\n",
               width, height, samples, times[times.size() / 2], mean,
               times[times.size() * 95 / 100], times.size(), taa::cameraHistory.status());
    }
    taa::drop(context);
    taa::drop(device);
}
