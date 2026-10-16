#include "../TemporalAA.cpp"
#include <chrono>
#include <cassert>
#include <vector>
#include <algorithm>
int main() {
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &d, nullptr, &c)));
    taa::oldUpdate = (taa::Update)(*(void ***)c)[48];
    assert(taa::initialize(c));
    ID3D11Buffer *buffers[6]{};
    const UINT sizes[] = {64, 64, 64, 16, 320, 48};
    for (unsigned i = 0; i < 6; ++i) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizes[i];
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        assert(SUCCEEDED(d->CreateBuffer(&desc, nullptr, &buffers[i])));
    }
    float projection[16]{};
    projection[0] = projection[5] = 1;
    projection[10] = 1.001f;
    projection[11] = -.1001f;
    projection[14] = 1;
    buffers[0]->SetPrivateData(taa::cacheGuid, 64, projection);
    float camera[16]{};
    camera[0] = camera[5] = camera[10] = camera[15] = 1;
    camera[11] = -10;
    buffers[1]->SetPrivateData(taa::cacheGuid, 64, camera);
    // Use a background layout: it is jittered but does not capture history.
    // Also time the solid layout with a depth-backed world target separately.
    D3D11_VIEWPORT vp{0, 0, 1920, 1080, 0, 1};
    c->RSSetViewports(1, &vp);
    ID3D11Texture2D *color = nullptr, *depth = nullptr;
    ID3D11RenderTargetView *rt = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1920;
    td.Height = 1080;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &color)));
    assert(SUCCEEDED(d->CreateRenderTargetView(color, nullptr, &rt)));
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &depth)));
    assert(SUCCEEDED(d->CreateDepthStencilView(depth, nullptr, &dsv)));
    c->OMSetRenderTargets(1, &rt, dsv);
    for (bool solid : {false, true}) {
        ID3D11Buffer *bound[6] = {buffers[0],
                                  buffers[1],
                                  buffers[2],
                                  solid ? buffers[3] : nullptr,
                                  solid ? buffers[4] : nullptr,
                                  solid ? buffers[5] : nullptr};
        c->VSSetConstantBuffers(0, 6, bound);
        std::vector<double> times;
        for (unsigned frame = 0; frame < 140; ++frame) {
            taa::reset();
            auto start = std::chrono::steady_clock::now();
            for (unsigned draw = 0; draw < 3000; ++draw) {
                taa::before(c);
                auto original = taa::jitter(c);
                assert(original == buffers[0]);
                c->VSSetConstantBuffers(0, 1, &original);
                taa::drop(original);
            }
            auto end = std::chrono::steady_clock::now();
            if (frame >= 20)
                times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
        std::sort(times.begin(), times.end());
        printf("%s, 3000 draw setups/restores, CPU median %.4f ms, p95 %.4f ms\n",
               solid ? "Solid" : "Background", times[times.size() / 2],
               times[times.size() * 95 / 100]);
    }
    puts("Synthetic CPU fixture excludes native draw calls, buffer writes and the GPU resolve.");
    ID3D11Buffer *empty[6]{};
    c->VSSetConstantBuffers(0, 6, empty);
    for (auto &buffer : buffers)
        taa::drop(buffer);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    taa::drop(rt);
    taa::drop(dsv);
    taa::drop(color);
    taa::drop(depth);
    taa::drop(c);
    taa::drop(d);
}
