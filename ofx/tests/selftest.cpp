// selftest.cpp — checks the library, mixer and kernel outside Resolve.
//
//  1. Library loads and lens search finds the expected profiles.
//  2. Overdrive never produces an invalid (folding) warp, even at absurd settings.
//  3. CPU and Metal render the same test chart to within float tolerance.
//  4. Writes before/after PPM images to the output directory for a visual check.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/LensBrowser.h"
#include "../src/LensKernel.h"
#include "../src/Mixer.h"
#include "../src/Profiles.h"

int LensCharacterRunMetalTest(const lc::LCParams& p_Params, const float* p_In, float* p_Out, double* p_GpuMs = nullptr);

static int s_Failures = 0;
#define CHECK(cond, ...) do { if (cond) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); s_Failures++; } printf("\n"); } while (0)

// Test chart: grey gradient, grid lines (distortion), point lights (bokeh), a white bar (CA).
static std::vector<float> makeChart(int w, int h)
{
    std::vector<float> img((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            float* p = &img[((size_t)y * w + x) * 4];
            float v = 0.12f + 0.1f * (float)x / w;
            bool grid = (x % (w / 16) < 2) || (y % (h / 9) < 2);
            if (grid) v = 0.55f;
            p[0] = v; p[1] = v; p[2] = v * 1.05f; p[3] = 1.0f;
        }
    // point lights
    for (int j = 1; j < 6; j++)
        for (int i = 1; i < 10; i++)
        {
            int cx = i * w / 10, cy = j * h / 6;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++)
                {
                    float* p = &img[((size_t)(cy + dy) * w + (cx + dx)) * 4];
                    p[0] = 1.0f; p[1] = 0.92f; p[2] = 0.75f;
                }
        }
    return img;
}

static void writePPM(const std::string& path, const std::vector<float>& img, int w, int h)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)   // OFX images are bottom-up
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++)
            {
                float v = img[((size_t)y * w + x) * 4 + c];
                v = v < 0 ? 0 : v > 1 ? 1 : v;
                fputc((int)(v * 255.0f + 0.5f), f);
            }
    fclose(f);
}

static std::vector<float> renderCPU(const lc::LCParams& p_Params, const std::vector<float>& src)
{
    lc::LCParams P = p_Params;
    P.decodeOnFetch = 1;
    std::vector<float> out(src.size());
    for (int y = 0; y < P.height; y++)
        for (int x = 0; x < P.width; x++)
        {
            lc::LCColor c = lc::shade(P, src.data(), x, y);
            float* d = &out[((size_t)y * P.width + x) * 4];
            d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = c.a;
        }
    return out;
}

int main(int argc, char** argv)
{
    std::string outDir = argc > 1 ? argv[1] : ".";
    lc::Library& lib = lc::Library::get();

    printf("Library\n");
    CHECK(lib.size() > 1000, "%zu profiles loaded", lib.size());
    const char* queries[] = {"Helios 44", "Helios 44-2 58mm", "Super-Takumar 50", "Sigma 85 Art", "Petzval", "Noctilux"};
    for (const char* q : queries)
    {
        lc::ProfilePtr p = lib.find(q);
        CHECK(p != nullptr, "\"%s\" -> %s", q, p ? p->summary().c_str() : "(none)");
    }
    lc::ProfilePtr helios = lib.findById("builtin:Helios 44-2 58mm (character)");
    CHECK(helios && helios->measured, "Helios character preset takes geometry from %s",
          helios && helios->measured ? helios->measured->name.c_str() : "(none)");

    const int W = 960, H = 540;
    std::vector<float> chart = makeChart(W, H);

    lc::FrameSettings s;
    s.width = W; s.height = H; s.rowStride = W * 4;
    s.encoding = lc::ENC_GAMMA24;
    s.aperture = 2.0f;
    lc::Slot* slots[4] = {&s.distortion, &s.ca, &s.vignetting, &s.character};
    for (lc::Slot* sl : slots) sl->a = helios;

    printf("Overdrive limits\n");
    for (float od : {1.0f, 10.0f, 100.0f, 10000.0f})
    {
        s.distortion.overdrive = od;
        s.ca.overdrive = od;
        lc::LCParams P = lc::buildParams(s);
        bool monotonic = true;
        float prev = -1.0f;
        for (int i = 0; i <= 200; i++)
        {
            float r = 1.25f * i / 200.0f;
            float d = r * lc::polyEval(P.dist, r);
            if (d <= prev) monotonic = false;
            prev = d;
        }
        float caShift = std::fabs(lc::polyEval(P.tcaR, 1.0f) - 1.0f);
        CHECK(monotonic && caShift <= 0.0401f, "overdrive %6.0f: warp one-to-one, d2 = %+.5f, CA corner shift = %.4f (budget 0.04)",
              od, P.dist[2], caShift);
    }

    printf("Render\n");
    s.distortion.overdrive = 40.0f;   // real primes are subtle; push them so the test image shows it
    s.ca.overdrive = 60.0f;
    s.vignetting.overdrive = 1.0f;
    s.character.overdrive = 1.0f;
    s.defocus = 4.0f;
    s.focusMode = lc::FOCUS_RADIAL;
    s.samples = 96;
    lc::LCParams P = lc::buildParams(s);
    printf("  params: distOn=%d tcaOn=%d vigOn=%d fit=%.4f catEye=%.2f swirl=%.2f blades=%.0f\n",
           P.distOn, P.tcaOn, P.vigOn, P.fit, P.catEye, P.swirl, P.blades);

    auto t0 = std::chrono::steady_clock::now();
    std::vector<float> cpu = renderCPU(P, chart);
    auto t1 = std::chrono::steady_clock::now();
    std::vector<float> gpu(chart.size());
    int rc = LensCharacterRunMetalTest(P, chart.data(), gpu.data());
    CHECK(rc == 0, "Metal kernel compiled and ran (rc=%d)", rc);
    if (rc == 0)
    {
        // Nearest-sample lookups can land on the other side of a pixel boundary when CPU and GPU
        // round differently (e.g. x86 without fused multiply-add), so judge by how many values
        // differ visibly rather than by the single worst value.
        double maxDiff = 0.0, sumDiff = 0.0;
        size_t visible = 0;
        for (size_t i = 0; i < cpu.size(); i++)
        {
            double d = std::fabs(cpu[i] - gpu[i]);
            maxDiff = std::max(maxDiff, d);
            sumDiff += d;
            if (d > 0.01) visible++;
        }
        double frac = (double)visible / cpu.size();
        CHECK(sumDiff / cpu.size() < 1e-4 && frac < 1e-4, "CPU vs Metal: mean |diff| %.2e, max %.2e, %zu of %zu values differ by > 0.01",
              sumDiff / cpu.size(), maxDiff, visible, cpu.size());
    }
    printf("  CPU single-thread 960x540: %.0f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());

    // GPU timing at UHD: the old approach against each quality mode
    {
        const int W4 = 3840, H4 = 2160;
        std::vector<float> big = makeChart(W4, H4), bigOut(big.size());
        lc::FrameSettings s4 = s;
        s4.width = W4; s4.height = H4; s4.rowStride = W4 * 4;
        for (lc::Slot* sl : {&s4.distortion, &s4.ca, &s4.vignetting, &s4.character}) sl->overdrive = 1.0f;
        auto timeIt = [&](const lc::LCParams& P4) {
            double best = 1e9, ms = 0;
            for (int i = 0; i < 4; i++) { LensCharacterRunMetalTest(P4, big.data(), bigOut.data(), &ms); best = std::min(best, ms); }
            return best;
        };
        printf("  GPU time per UHD frame, Helios, Sharp Centre (best of 4):\n");
        for (float defocus : {3.0f, 8.0f})
        {
            s4.defocus = defocus;
            s4.quality = lc::QUALITY_FULL;
            lc::LCParams full = lc::buildParams(s4);
            lc::LCParams old = full;
            old.decodeOnFetch = 1; old.adaptiveSamples = 0; old.glowSamples = 24;
            s4.quality = lc::QUALITY_FAST;
            lc::LCParams fast = lc::buildParams(s4);
            s4.quality = lc::QUALITY_GEOMETRY;
            lc::LCParams geo = lc::buildParams(s4);
            double tOld = timeIt(old), tFull = timeIt(full), tFast = timeIt(fast), tGeo = timeIt(geo);
            printf("    defocus %.0f:  v0.3 %.1f ms   Full %.1f ms   Fast %.1f ms   Geometry Only %.1f ms\n", defocus, tOld, tFull, tFast, tGeo);
        }
    }

    printf("Aperture\n");
    {
        lc::FrameSettings a = s;
        a.maxAperture = 2.0f;
        a.aperture = 2.0f;
        lc::LCParams wide = lc::buildParams(a);
        a.aperture = 4.0f;
        lc::LCParams f4 = lc::buildParams(a);
        a.aperture = 11.0f;
        lc::LCParams f11 = lc::buildParams(a);
        CHECK(std::fabs(f4.defocusPx / wide.defocusPx - 0.5f) < 1e-4f, "f/4 on an f/2 lens halves the blur (%.1f px -> %.1f px)", wide.defocusPx, f4.defocusPx);
        CHECK(f4.catEye < wide.catEye && f11.catEye < 0.05f, "cat-eye fades when stopping down (%.2f -> %.2f -> %.2f)", wide.catEye, f4.catEye, f11.catEye);
        CHECK(f4.curve < wide.curve, "iris blades show more when stopped down (curvature %.2f -> %.2f)", wide.curve, f4.curve);
    }

    printf("Lens browser\n");
    {
        lc::BrowserRequest req;
        req.title = "Choose Lens";
        req.aperture = 2.0f;
        bool a = lc::snapshotLensBrowser(req, "All Lenses", "", "builtin:Helios 44-2 58mm (character)", outDir + "/browser_all.png");
        bool b = lc::snapshotLensBrowser(req, "Character Presets", "", "builtin:Petzval 85mm (character)", outDir + "/browser_presets.png");
        bool c = lc::snapshotLensBrowser(req, "Sigma", "85", "", outDir + "/browser_search.png");
        CHECK(a && b && c, "browser rendered offscreen to %s/browser_*.png", outDir.c_str());
    }

    // Real-lens settings, no defocus: vintage presets should still show soft, swirly corners
    {
        lc::FrameSettings e = s;
        for (lc::Slot* sl : {&e.distortion, &e.ca, &e.vignetting, &e.character}) sl->overdrive = 1.0f;
        e.defocus = 0.0f;
        for (const char* id : {"builtin:Helios 44-2 58mm (character)", "builtin:Petzval 85mm (character)"})
        {
            lc::ProfilePtr lens = lib.findById(id);
            for (lc::Slot* sl : {&e.distortion, &e.ca, &e.vignetting, &e.character}) sl->a = lens;
            std::vector<float> out(chart.size());
            LensCharacterRunMetalTest(lc::buildParams(e), chart.data(), out.data());
            std::string name = std::string(id).find("Petzval") != std::string::npos ? "petzval" : "helios";
            writePPM(outDir + "/edges_" + name + "_defocus0.ppm", out, W, H);
        }
    }

    writePPM(outDir + "/selftest_before.ppm", chart, W, H);
    writePPM(outDir + "/selftest_after_cpu.ppm", cpu, W, H);
    if (rc == 0) writePPM(outDir + "/selftest_after_metal.ppm", gpu, W, H);

    printf(s_Failures ? "\n%d check(s) failed\n" : "\nAll checks passed\n", s_Failures);
    return s_Failures ? 1 : 0;
}
