#include "../TemporalAA.cpp"
#include <array>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

namespace {
// The accepted main shader is kept independently of the candidate so the
// comparison cannot silently change when the candidate implementation changes.
constexpr const char *baselineCode = R"(
cbuffer Resolve:register(b0){float weight,valid,jitterX,jitterY;float blurStrength;float3 padding;};
Texture2D<float4> scene:register(t0);Texture2D<float4> history:register(t1);SamplerState linearSampler:register(s0);
float4 ps(float4 p:SV_POSITION):SV_TARGET{
 uint w,h;scene.GetDimensions(w,h);float2 uv=(p.xy+float2(jitterX,jitterY))/float2(w,h);
 float4 current=scene.SampleLevel(linearSampler,uv,0);float3 lo=current.rgb,hi=current.rgb;float3 left=current.rgb,right=current.rgb,up=current.rgb,down=current.rgb;
 [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++){
  float3 c=scene.SampleLevel(linearSampler,uv+float2(x,y)/float2(w,h),0).rgb;lo=min(lo,c);hi=max(hi,c);if(y==0&&x==-1)left=c;if(y==0&&x==1)right=c;if(x==0&&y==-1)up=c;if(x==0&&y==1)down=c;
 }
 float3 old=history.Load(int3(p.xy,0)).rgb;
 float disagreement=max(abs(old.r-current.r),max(abs(old.g-current.g),abs(old.b-current.b)));
 float3 lr=(left-current.rgb)*(right-current.rgb),ud=(up-current.rgb)*(down-current.rgb);
 float thin=saturate(24*sqrt(max(0,max(max(lr.r,lr.g),max(lr.b,max(max(ud.r,ud.g),ud.b))))));
 float confidence=1-smoothstep(.035,lerp(.16,.30,thin),disagreement);
 float accumulation=lerp(weight,max(weight,.78),thin);
 float3 filtered=lerp(current.rgb,(current.rgb*4+left+right+up+down)/8,blurStrength);
 old=clamp(old,lo,hi);return float4(lerp(filtered,old,accumulation*valid*confidence),current.a);
})";

ID3D11PixelShader *compileBaseline(ID3D11Device *device, const std::string &source) {
    ID3DBlob *compiled = nullptr, *errors = nullptr;
    HRESULT result =
        D3DCompile(source.data(), source.size(), "StableTaaBaseline", nullptr, nullptr, "ps",
                   "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiled, &errors);
    if (errors)
        fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
    taa::drop(errors);
    assert(SUCCEEDED(result));
    ID3D11PixelShader *shader = nullptr;
    assert(SUCCEEDED(device->CreatePixelShader(compiled->GetBufferPointer(),
                                               compiled->GetBufferSize(), nullptr, &shader)));
    taa::drop(compiled);
    return shader;
}

struct ImageFixture {
    ID3D11DeviceContext *context;
    UINT width, height;
    ID3D11Texture2D *scene = nullptr, *staging = nullptr;
    ID3D11RenderTargetView *target = nullptr;
    std::vector<unsigned char> pixels;
    static constexpr unsigned char dark[3] = {28, 46, 60};
    static constexpr unsigned char bright[3] = {236, 216, 190};
    // D3D11's standard four-sample pattern, expressed inside each pixel.
    static constexpr float coverageSamples[4][2] = {
        {.375f, .125f}, {.875f, .375f}, {.125f, .625f}, {.625f, .875f}};

    ImageFixture(ID3D11Device *device, ID3D11DeviceContext *ctx, UINT w, UINT h)
        : context(ctx), width(w), height(h), pixels(size_t(w) * h * 4) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &scene)));
        assert(SUCCEEDED(device->CreateRenderTargetView(scene, nullptr, &target)));
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    }
    ~ImageFixture() {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        taa::drop(target);
        taa::drop(staging);
        taa::drop(scene);
    }
    static float edgeAt(float y, float boundary, float slope) {
        return boundary + slope * y;
    }
    void edge(float boundary, float slope, float jitterX, float jitterY) {
        edge(boundary, slope, jitterX, jitterY, dark, bright);
    }
    void edge(float boundary, float slope, float jitterX, float jitterY, const unsigned char *dark,
              const unsigned char *bright) {
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x) {
                unsigned covered = 0;
                for (const auto &sample : coverageSamples) {
                    float sampleX = x + sample[0] - jitterX;
                    float sampleY = y + sample[1] - jitterY;
                    covered += sampleX >= edgeAt(sampleY, boundary, slope);
                }
                size_t offset = (size_t(y) * width + x) * 4;
                for (unsigned channel = 0; channel < 3; ++channel)
                    pixels[offset + channel] = static_cast<unsigned char>(
                        (dark[channel] * (4 - covered) + bright[channel] * covered + 2) / 4);
                pixels[offset + 3] = 255;
            }
        upload();
    }
    void uniform(unsigned char red, unsigned char green, unsigned char blue) {
        for (size_t offset = 0; offset < pixels.size(); offset += 4) {
            pixels[offset] = red;
            pixels[offset + 1] = green;
            pixels[offset + 2] = blue;
            pixels[offset + 3] = 255;
        }
        upload();
    }
    void upload() {
        context->UpdateSubresource(scene, 0, nullptr, pixels.data(), width * 4, 0);
    }
    void resolve(ID3D11PixelShader *shader, float jitterX, float jitterY) {
        taa::ps = shader;
        taa::jitterX = jitterX;
        taa::jitterY = jitterY;
        taa::finished = false;
        taa::viewport = {0, 0, float(width), float(height), 0, 1};
        taa::target = target;
        target->AddRef();
        context->OMSetRenderTargets(1, &target, nullptr);
        assert(taa::apply());
    }
    std::vector<unsigned char> read() {
        context->CopyResource(staging, scene);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        assert(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
        std::vector<unsigned char> result(pixels.size());
        for (UINT y = 0; y < height; ++y)
            memcpy(result.data() + size_t(y) * width * 4,
                   static_cast<unsigned char *>(mapped.pData) + size_t(y) * mapped.RowPitch,
                   width * 4);
        context->Unmap(staging, 0);
        return result;
    }
};

std::pair<float, float> phase(unsigned frame) {
    return {(taa::halton(frame % 8 + 1, 2) - .5f) * .75f,
            (taa::halton(frame % 8 + 1, 3) - .5f) * .75f};
}

double staticEdgeVariance(ImageFixture &fixture, ID3D11PixelShader *shader) {
    constexpr float boundary = 42.3f, slope = .37f;
    taa::historyValid = false;
    std::vector<std::vector<unsigned char>> frames;
    for (unsigned frame = 0; frame < 40; ++frame) {
        auto jitter = phase(frame);
        fixture.edge(boundary, slope, jitter.first, jitter.second);
        fixture.resolve(shader, jitter.first, jitter.second);
        if (frame >= 32)
            frames.push_back(fixture.read());
    }
    double total = 0;
    unsigned count = 0;
    for (UINT y = 4; y + 4 < fixture.height; ++y)
        for (UINT x = 4; x + 4 < fixture.width; ++x) {
            if (fabsf(float(x) + .5f - ImageFixture::edgeAt(float(y) + .5f, boundary, slope)) > 2)
                continue;
            size_t offset = (size_t(y) * fixture.width + x) * 4;
            double mean = 0;
            for (const auto &frame : frames)
                mean += frame[offset] / double(frames.size());
            for (const auto &frame : frames) {
                double difference = frame[offset] - mean;
                total += difference * difference / double(frames.size());
            }
            ++count;
        }
    assert(count > 200);
    return total / count;
}

// Distance (0-255 units) from a color to the segment between two colors.
static double segmentDistance(const double *color, const unsigned char *a, const unsigned char *b) {
    double axis[3], rel[3], length = 0, t = 0;
    for (int c = 0; c < 3; ++c) {
        axis[c] = double(b[c]) - a[c];
        rel[c] = color[c] - a[c];
        length += axis[c] * axis[c];
    }
    for (int c = 0; c < 3; ++c)
        t += rel[c] * axis[c] / length;
    t = std::clamp(t, 0.0, 1.0);
    double distance = 0;
    for (int c = 0; c < 3; ++c)
        distance += (rel[c] - t * axis[c]) * (rel[c] - t * axis[c]);
    return sqrt(distance);
}

// Worst off-hue output while a grass/stone edge pans across a static
// horizon with sky below: pixels near the horizon see three colors at once.
// A correct resolve only blends the colors meeting at that pixel's edge.
// Per-channel history clamping to a three-color box can leave off-hue
// fringes (green/red/yellow/blue lines). Pixels near the moving triple
// junction are excluded, since a genuine three-way blend is valid there.
double panningEdgeHueError(ImageFixture &fixture, ID3D11PixelShader *shader) {
    static constexpr unsigned char grass[3] = {72, 168, 40}, stone[3] = {150, 146, 172},
                                   sky[3] = {70, 120, 230};
    constexpr float horizon = 48.3f, slope = .23f;
    taa::historyValid = false;
    double worst = 0;
    for (unsigned frame = 0; frame < 48; ++frame) {
        auto jitter = phase(frame);
        float boundary = 40.f + 2.3f * frame;
        fixture.edge(boundary, slope, jitter.first, jitter.second, grass, stone);
        // Overwrite rows below the horizon with sky, with jittered coverage.
        auto &pixels = fixture.pixels;
        for (UINT y = 0; y < fixture.height; ++y)
            for (UINT x = 0; x < fixture.width; ++x) {
                unsigned covered = 0;
                for (const auto &sample : ImageFixture::coverageSamples)
                    covered += y + sample[1] - jitter.second >= horizon;
                size_t offset = (size_t(y) * fixture.width + x) * 4;
                for (unsigned c = 0; c < 3; ++c)
                    pixels[offset + c] = static_cast<unsigned char>(
                        (pixels[offset + c] * (4 - covered) + sky[c] * covered + 2) / 4);
            }
        fixture.upload();
        fixture.resolve(shader, jitter.first, jitter.second);
        if (frame < 8)
            continue;
        auto output = fixture.read();
        for (UINT y = 4; y + 4 < fixture.height; ++y)
            for (UINT x = 4; x + 4 < fixture.width; ++x) {
                float edgeX = ImageFixture::edgeAt(float(y) + .5f, boundary, slope);
                if (fabsf(float(x) + .5f - edgeX) < 4 && fabsf(float(y) + .5f - horizon) < 4)
                    continue;
                size_t offset = (size_t(y) * fixture.width + x) * 4;
                double color[3] = {double(output[offset]), double(output[offset + 1]),
                                   double(output[offset + 2])};
                double best = std::min({segmentDistance(color, grass, stone),
                                        segmentDistance(color, grass, sky),
                                        segmentDistance(color, stone, sky)});
                worst = std::max(worst, best);
            }
    }
    return worst;
}

void motionAndUniformChecks(ImageFixture &fixture, ID3D11PixelShader *shader) {
    taa::historyValid = false;
    constexpr float slope = .11f;
    for (unsigned frame = 0; frame < 10; ++frame) {
        float boundary = 96.f - 7.f * frame;
        auto jitter = phase(frame);
        fixture.edge(boundary, slope, jitter.first, jitter.second);
        fixture.resolve(shader, jitter.first, jitter.second);
        auto pixels = fixture.read();
        // The edge travels seven pixels per frame. Newly revealed pixels at
        // least three pixels behind it must be correct on their first frame.
        for (UINT y = 4; y + 4 < fixture.height; ++y)
            for (UINT x = 4; x + 4 < fixture.width; ++x) {
                float distance =
                    float(x) + .5f - ImageFixture::edgeAt(float(y) + .5f, boundary, slope);
                if (fabsf(distance) < 3)
                    continue;
                const auto &expected = distance > 0 ? ImageFixture::bright : ImageFixture::dark;
                size_t offset = (size_t(y) * fixture.width + x) * 4;
                for (unsigned channel = 0; channel < 3; ++channel)
                    assert(abs(int(pixels[offset + channel]) - int(expected[channel])) <= 1);
            }
    }
    for (const auto &color : {std::array<unsigned char, 3>{18, 91, 203}, {230, 28, 130}}) {
        fixture.uniform(color[0], color[1], color[2]);
        auto jitter = phase(2);
        fixture.resolve(shader, jitter.first, jitter.second);
        auto pixels = fixture.read();
        for (size_t offset = 0; offset < pixels.size(); offset += 4)
            for (unsigned channel = 0; channel < 3; ++channel)
                assert(abs(int(pixels[offset + channel]) - int(color[channel])) <= 1);
    }
    puts(
        "Moving silhouette: no stale trail outside the neighborhood; uniform colors preserved PASS");
}

struct TimerSample {
    ID3D11Query *start = nullptr, *end = nullptr;
    unsigned mode = 0;
};
bool waitQuery(ID3D11DeviceContext *context, ID3D11Query *query, void *result, UINT bytes) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    HRESULT status;
    while ((status = context->GetData(query, result, bytes, 0)) == S_FALSE) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        Sleep(1); // Test-only bounded query wait; never part of the mod's render path.
    }
    return SUCCEEDED(status);
}

void benchmark(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11PixelShader *baseline,
               ID3D11PixelShader *candidate) {
    ImageFixture fixture(device, context, 1920, 1080);
    // Upload outside every measured interval. Both variants see the same scene,
    // copies, resources and history traffic; alternate their submission order.
    fixture.edge(720.3f, .37f, 0, 0);
    for (unsigned i = 0; i < 24; ++i)
        fixture.resolve(i % 2 ? baseline : candidate, 0, 0);
    ID3D11Query *disjoint = nullptr;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    assert(SUCCEEDED(device->CreateQuery(&desc, &disjoint)));
    desc.Query = D3D11_QUERY_TIMESTAMP;
    std::vector<TimerSample> samples(160);
    for (unsigned i = 0; i < samples.size(); ++i) {
        auto &sample = samples[i];
        sample.mode = i % 4 == 0 || i % 4 == 3 ? 0 : 1;
        assert(SUCCEEDED(device->CreateQuery(&desc, &sample.start)));
        assert(SUCCEEDED(device->CreateQuery(&desc, &sample.end)));
    }
    context->Begin(disjoint);
    for (auto &sample : samples) {
        fixture.upload();
        context->End(sample.start);
        fixture.resolve(sample.mode ? candidate : baseline, 0, 0);
        context->End(sample.end);
    }
    context->End(disjoint);
    context->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
    bool valid = waitQuery(context, disjoint, &frequency, sizeof(frequency)) &&
                 !frequency.Disjoint && frequency.Frequency;
    std::vector<double> milliseconds[2];
    for (auto &sample : samples) {
        UINT64 start = 0, end = 0;
        valid = waitQuery(context, sample.start, &start, sizeof(start)) &&
                waitQuery(context, sample.end, &end, sizeof(end)) && valid;
        if (frequency.Frequency && end >= start)
            milliseconds[sample.mode].push_back(double(end - start) * 1000 / frequency.Frequency);
        taa::drop(sample.start);
        taa::drop(sample.end);
    }
    taa::drop(disjoint);
    if (!valid) {
        puts("Hardware timing unavailable: invalid/disjoint GPU timestamp interval");
        return;
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        auto &values = milliseconds[mode];
        std::sort(values.begin(), values.end());
        double mean = std::accumulate(values.begin(), values.end(), 0.) / values.size();
        printf(
            "1080p %s, input copy + resolve + history copy: median %.4f ms, mean %.4f ms, p95 %.4f ms (%zu interleaved samples)\n",
            mode ? "candidate" : "stable", values[values.size() / 2], mean,
            values[size_t(.95 * (values.size() - 1))], values.size());
    }
}
} // namespace

int main(int argc, char **argv) {
    bool hardware = false;
    std::string baselineSource = baselineCode;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--benchmark") == 0)
            hardware = true;
        else if (strcmp(argv[i], "--baseline") == 0 && i + 1 < argc) {
            std::ifstream input(argv[++i], std::ios::binary);
            assert(input);
            baselineSource.assign(std::istreambuf_iterator<char>(input), {});
        } else {
            fprintf(stderr,
                    "Usage: TemporalStabilityTest [--baseline shader.hlsl] [--benchmark]\n");
            return 2;
        }
    }
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    HRESULT created =
        D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
                          nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(created) && hardware) {
        puts("Hardware timing unavailable: cannot create D3D11 hardware device");
        return 3;
    }
    assert(SUCCEEDED(created));
    taa::oldD = reinterpret_cast<taa::D>((*(void ***)context)[13]);
    taa::oldUpdate = reinterpret_cast<taa::Update>((*(void ***)context)[48]);
    assert(taa::initialize(context));
    ID3D11PixelShader *candidate = taa::ps;
    ID3D11PixelShader *baseline = compileBaseline(device, baselineSource);
    taa::historyWeight = .65f;
    taa::blurStrength = .12f;
    {
        ImageFixture fixture(device, context, 160, 96);
        double stableVariance = staticEdgeVariance(fixture, baseline);
        double candidateVariance = staticEdgeVariance(fixture, candidate);
        printf("Static slanted-edge phase variance: stable %.4f, candidate %.4f (%+.2f%%)\n",
               stableVariance, candidateVariance, 100 * (candidateVariance / stableVariance - 1));
        assert(stableVariance > .1);
        assert(candidateVariance < stableVariance * .995);
        double stableHue = panningEdgeHueError(fixture, baseline);
        double candidateHue = panningEdgeHueError(fixture, candidate);
        printf("Panning three-color edge, worst off-hue distance: stable %.2f, candidate %.2f\n",
               stableHue, candidateHue);
        fflush(stdout);
        // A few units come from 8-bit rounding and bilinear reconstruction.
        assert(candidateHue <= std::max(stableHue, 6.0));
        motionAndUniformChecks(fixture, candidate);
    }
    if (hardware)
        benchmark(device, context, baseline, candidate);
    taa::ps = candidate;
    taa::drop(baseline);
    taa::drop(context);
    taa::drop(device);
}
