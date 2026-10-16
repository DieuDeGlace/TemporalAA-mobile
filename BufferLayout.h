#pragma once
#include <d3d11.h>
#include <array>

class BoundConstantBuffers {
    std::array<ID3D11Buffer *, 6> buffers{};

  public:
    explicit BoundConstantBuffers(ID3D11DeviceContext *context) {
        context->VSGetConstantBuffers(0, UINT(buffers.size()), buffers.data());
    }
    BoundConstantBuffers(const BoundConstantBuffers &) = delete;
    BoundConstantBuffers &operator=(const BoundConstantBuffers &) = delete;
    ~BoundConstantBuffers() {
        for (auto *buffer : buffers)
            if (buffer)
                buffer->Release();
    }
    ID3D11Buffer *operator[](unsigned index) const {
        return buffers[index];
    }
    ID3D11Buffer *const *data() const {
        return buffers.data();
    }
    ID3D11Buffer *take(unsigned index) {
        auto *result = buffers[index];
        buffers[index] = nullptr;
        return result;
    }
};

// Buffer descriptions never change. Retain the last binding tuple so pointer
// reuse cannot turn a cache hit into the description of a destroyed buffer.
class BufferLayout {
    std::array<ID3D11Buffer *, 6> retained{};
    bool known = false, matrices = false, solid = false;

  public:
    BufferLayout() = default;
    BufferLayout(const BufferLayout &) = delete;
    BufferLayout &operator=(const BufferLayout &) = delete;
    unsigned inspections = 0;
    ~BufferLayout() {
        clear();
    }
    void clear() {
        for (auto &buffer : retained) {
            if (buffer)
                buffer->Release();
            buffer = nullptr;
        }
        known = false;
    }
    bool inspect(ID3D11Buffer *const *buffers, bool &world) {
        bool same = known;
        for (unsigned i = 0; i < retained.size(); ++i)
            same &= retained[i] == buffers[i];
        if (!same) {
            clear();
            const UINT sizes[] = {64, 64, 64, 16, 320, 48};
            matrices = solid = true;
            for (unsigned i = 0; i < retained.size(); ++i) {
                retained[i] = buffers[i];
                D3D11_BUFFER_DESC desc{};
                if (buffers[i]) {
                    buffers[i]->AddRef();
                    buffers[i]->GetDesc(&desc);
                }
                if (i < 3)
                    matrices &= desc.ByteWidth == 64;
                solid &= desc.ByteWidth == sizes[i];
            }
            known = true;
            ++inspections;
        }
        world = solid;
        return matrices;
    }
};
