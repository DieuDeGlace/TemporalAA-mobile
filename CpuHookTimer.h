#pragma once
#include <windows.h>
#include <cstdio>

// Time only CPU setup/restoration on one frame in sixty. Never include the
// native draw or wait for GPU queries. Logging uses the existing mod log.
class CpuHookTimer {
    bool sampling = false;
    unsigned frames = 0, samples = 0;
    LONGLONG ticks = 0;

  public:
    class Scope {
        CpuHookTimer &owner;
        LARGE_INTEGER begin{};

      public:
        explicit Scope(CpuHookTimer &timer) : owner(timer) {
            if (owner.sampling)
                QueryPerformanceCounter(&begin);
        }
        ~Scope() {
            if (begin.QuadPart) {
                LARGE_INTEGER end{};
                QueryPerformanceCounter(&end);
                owner.ticks += end.QuadPart - begin.QuadPart;
            }
        }
    };
    void present(const char *path) {
        if (sampling && ++samples == 30) {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            FILE *file = nullptr;
            fopen_s(&file, path, "a");
            if (file) {
                fprintf(file,
                        "Draw setup/restore CPU: %.3f ms/frame (30 sparse frames; "
                        "excludes native draws and buffer-write hooks).\n",
                        1000. * ticks / frequency.QuadPart / samples);
                fclose(file);
            }
            ticks = 0;
            samples = 0;
        }
        sampling = frames++ % 60 == 0;
    }
};
