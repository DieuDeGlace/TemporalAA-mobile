#pragma once
#include <d3d11.h>
#include <cstdio>

// Sparse asynchronous GPU timings. Never flush or wait for a result.
class GpuPassTimer {
    struct Slot {
        ID3D11Query *disjoint = nullptr;
        ID3D11Query *stamp[3]{};
        bool pending = false;
    };
    Slot slots[4]{};
    Slot *active = nullptr;
    unsigned frame = 0, samples = 0;
    double firstMs = 0, secondMs = 0;
    bool attempted = false, ready = false;

  public:
    ~GpuPassTimer() {
        for (auto &slot : slots) {
            if (slot.disjoint)
                slot.disjoint->Release();
            for (auto *stamp : slot.stamp)
                if (stamp)
                    stamp->Release();
        }
    }
    void begin(ID3D11Device *device, ID3D11DeviceContext *context, const char *path,
               const char *label) {
        active = nullptr;
        if (!attempted) {
            attempted = true;
            ready = true;
            for (auto &slot : slots) {
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                if (FAILED(device->CreateQuery(&desc, &slot.disjoint)))
                    ready = false;
                desc.Query = D3D11_QUERY_TIMESTAMP;
                for (auto &stamp : slot.stamp)
                    if (FAILED(device->CreateQuery(&desc, &stamp)))
                        ready = false;
            }
        }
        if (!ready)
            return;
        for (auto &slot : slots) {
            if (!slot.pending)
                continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
            UINT64 stamps[3]{};
            if (context->GetData(slot.disjoint, &timing, sizeof(timing),
                                 D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            bool complete = true;
            for (unsigned i = 0; i < 3; ++i)
                if (context->GetData(slot.stamp[i], &stamps[i], sizeof(stamps[i]),
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                    complete = false;
            if (!complete)
                continue;
            slot.pending = false;
            if (timing.Disjoint || !timing.Frequency || stamps[1] < stamps[0] ||
                stamps[2] < stamps[1])
                continue;
            firstMs += 1000.0 * (stamps[1] - stamps[0]) / timing.Frequency;
            secondMs += 1000.0 * (stamps[2] - stamps[1]) / timing.Frequency;
            if (++samples == 10) {
                FILE *file = nullptr;
                fopen_s(&file, path, "a");
                if (file) {
                    fprintf(
                        file,
                        "%s GPU: first=%.3f ms second=%.3f ms total=%.3f ms (10 sparse samples)\n",
                        label, firstMs / samples, secondMs / samples,
                        (firstMs + secondMs) / samples);
                    fclose(file);
                }
                samples = 0;
                firstMs = secondMs = 0;
            }
        }
        if (frame++ % 60 != 0)
            return;
        for (auto &slot : slots) {
            if (slot.pending)
                continue;
            active = &slot;
            context->Begin(slot.disjoint);
            context->End(slot.stamp[0]);
            break;
        }
    }
    void split(ID3D11DeviceContext *context) {
        if (active)
            context->End(active->stamp[1]);
    }
    void end(ID3D11DeviceContext *context) {
        if (!active)
            return;
        context->End(active->stamp[2]);
        context->End(active->disjoint);
        active->pending = true;
        active = nullptr;
    }
};
