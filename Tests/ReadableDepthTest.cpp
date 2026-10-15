// The device hooks make game depth targets shader-readable without changing
// how the game uses them: typeless storage with a shader-resource binding,
// typed depth-stencil views (also for a null view description), and other
// textures left alone.
#include "../TemporalAA.cpp"
#include <cassert>
#include <cstdio>

int main() {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, nullptr, &context)));
    auto table = *(void ***)device;
    taa::oldCreateTexture2D = (taa::CreateTexture2D)table[taa::SLOT_CREATE_TEXTURE_2D];
    taa::oldCreateDepthView = (taa::CreateDepthView)table[taa::SLOT_CREATE_DEPTH_STENCIL_VIEW];
    for (UINT samples : {1u, 4u}) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 640;
        desc.Height = 360;
        desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc = {samples, 0};
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11Texture2D *depth = nullptr;
        assert(SUCCEEDED(taa::createTexture2D(device, &desc, nullptr, &depth)));
        D3D11_TEXTURE2D_DESC made{};
        depth->GetDesc(&made);
        assert(made.Format == DXGI_FORMAT_R24G8_TYPELESS);
        assert(made.BindFlags == (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE));
        // The game's usual null-description view still works and is typed.
        ID3D11DepthStencilView *view = nullptr;
        assert(SUCCEEDED(taa::createDepthView(device, depth, nullptr, &view)));
        D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
        view->GetDesc(&viewDesc);
        assert(viewDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT);
        assert(viewDesc.ViewDimension == (samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                                                      : D3D11_DSV_DIMENSION_TEXTURE2D));
        context->ClearDepthStencilView(view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, .25f, 7);
        // An explicit typed description passes through unchanged.
        ID3D11DepthStencilView *explicitView = nullptr;
        assert(SUCCEEDED(taa::createDepthView(device, depth, &viewDesc, &explicitView)));
        // And the depth can now be read in a shader.
        D3D11_SHADER_RESOURCE_VIEW_DESC read{};
        read.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        read.ViewDimension =
            samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        read.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView *srv = nullptr;
        assert(SUCCEEDED(device->CreateShaderResourceView(depth, &read, &srv)));
        taa::drop(srv);
        taa::drop(explicitView);
        taa::drop(view);
        taa::drop(depth);
    }
    // Non-depth textures and small depth targets are untouched.
    D3D11_TEXTURE2D_DESC color{};
    color.Width = color.Height = 512;
    color.MipLevels = color.ArraySize = 1;
    color.SampleDesc = {1, 0};
    color.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *texture = nullptr;
    assert(SUCCEEDED(taa::createTexture2D(device, &color, nullptr, &texture)));
    D3D11_TEXTURE2D_DESC made{};
    texture->GetDesc(&made);
    assert(made.Format == DXGI_FORMAT_R8G8B8A8_UNORM && made.BindFlags == D3D11_BIND_RENDER_TARGET);
    taa::drop(texture);
    D3D11_TEXTURE2D_DESC tiny = color;
    tiny.Width = tiny.Height = 128;
    tiny.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    tiny.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    assert(SUCCEEDED(taa::createTexture2D(device, &tiny, nullptr, &texture)));
    texture->GetDesc(&made);
    assert(made.Format == DXGI_FORMAT_D24_UNORM_S8_UINT);
    taa::drop(texture);
    // Disabled: depth targets are created exactly as requested.
    taa::readableDepth = false;
    D3D11_TEXTURE2D_DESC depthDesc = tiny;
    depthDesc.Width = depthDesc.Height = 640;
    assert(SUCCEEDED(taa::createTexture2D(device, &depthDesc, nullptr, &texture)));
    texture->GetDesc(&made);
    assert(made.Format == DXGI_FORMAT_D24_UNORM_S8_UINT);
    taa::drop(texture);
    taa::drop(context);
    taa::drop(device);
    puts("Readable depth hooks PASS");
}
