// Mixer.cpp — trait mixing and overdrive (architecture §08–§09).
//
// Rule: blend lens *parameters*, never rendered pixels. Every trait is expressed as a
// deviation Δ from the ideal lens, blended A↔B, then scaled by amount × overdrive through
// a soft limiter whose budget keeps the image valid (no fold-over warps, bounded CA,
// bounded vignetting) unless the user turns on Unbounded.

#include "Mixer.h"

#include <algorithm>
#include <cmath>

namespace lc {

static const float kIdeal[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

float softLimit(float x, float budget)
{
    if (budget <= 0.0f) return 0.0f;
    float t = std::fabs(x) / budget;
    float s = t / std::pow(1.0f + t * t * t * t, 0.25f);
    return std::copysign(budget * s, x);
}

// Value-space limiter for a single parameter: ideal + sign(Δ)·B·S(x|Δ|/B)
static float driveValue(float ideal, float delta, float x, float budget, bool unbounded)
{
    if (unbounded) return ideal + delta * x;
    return ideal + std::copysign(softLimit(x * std::fabs(delta), budget), delta);
}

static float plateHalfDiagMm(const FrameSettings& s, const Profile& src)
{
    if (s.sensorWidthMm > 0.0f && s.width > 0)
        return 0.5f * s.sensorWidthMm * std::hypot(1.0f, (float)s.height / (float)s.width);
    return src.calib.halfDiagMm;
}

// Rescale a Hugin-unit polynomial to rho units (rho = 1 at the frame corner).
static void toRho(float a[5], float scale)
{
    float f = 1.0f;
    for (int k = 0; k < 5; k++) { a[k] *= f; f *= scale; }
}

// ---- distortion / CA ------------------------------------------------------------------------

static bool sampleDistRho(const FrameSettings& s, const ProfilePtr& p, float out[5])
{
    const Profile* src = p ? p->distSource() : nullptr;
    if (!src || !sampleDistortion(*src, s.focal, out)) return false;
    toRho(out, plateHalfDiagMm(s, *src) / src->calib.halfShortMm);
    return true;
}

static bool sampleTcaRho(const FrameSettings& s, const ProfilePtr& p, float r[5], float b[5])
{
    const Profile* src = p ? p->tcaSource() : nullptr;
    if (!src || !sampleTca(*src, s.focal, r, b)) return false;
    float scale = plateHalfDiagMm(s, *src) / src->calib.halfShortMm;
    toRho(r, scale);
    toRho(b, scale);
    return true;
}

static void blendDelta(const Slot& slot, const float* a, bool haveA, const float* b, bool haveB, float delta[5])
{
    for (int k = 0; k < 5; k++)
    {
        float dA = haveA ? a[k] - kIdeal[k] : 0.0f;
        float dB = haveB ? b[k] - kIdeal[k] : 0.0f;
        delta[k] = dA * (1.0f - slot.mix) + dB * slot.mix;
    }
}

static bool isZero(const float d[5])
{
    for (int k = 0; k < 5; k++) if (d[k] != 0.0f) return false;
    return true;
}

// The warp r → r·P(r) must stay one-to-one with a sane slope out past the corner.
static bool warpValid(const float delta[5], float s)
{
    float a[5];
    for (int k = 0; k < 5; k++) a[k] = kIdeal[k] + s * delta[k];
    for (int i = 0; i <= 64; i++)
    {
        float r = 1.25f * (float)i / 64.0f;
        float p = polyEval(a, r);
        float dp = p + r * polyDeriv(a, r);
        if (p < 0.05f || dp < 0.1f) return false;
    }
    return true;
}

// Largest scale on Δ that keeps the warp valid. Measured primes are very subtle, so the
// search runs geometrically up to 1e5× before giving up on finding a limit.
static float warpBudget(const float delta[5])
{
    const float maxS = 1e5f;
    if (warpValid(delta, maxS)) return maxS;
    float lo = 1e-4f, hi = maxS;
    if (!warpValid(delta, lo)) return 0.0f;
    for (int i = 0; i < 40; i++)
    {
        float mid = std::sqrt(lo * hi);
        (warpValid(delta, mid) ? lo : hi) = mid;
    }
    return lo;
}

static float maxRadialShift(const float delta[5])
{
    float m = 0.0f;
    for (int i = 0; i <= 16; i++)
    {
        float r = (float)i / 16.0f;
        m = std::max(m, std::fabs(r * polyEval(delta, r)));
    }
    return m;
}

// ---- vignetting --------------------------------------------------------------------------------

static bool sampleVigRho(const FrameSettings& s, const ProfilePtr& p, float out[3])
{
    const Profile* src = p ? p->vigSource() : nullptr;
    if (!src || !sampleVignetting(*src, s.focal, s.aperture, out)) return false;
    float sv = plateHalfDiagMm(s, *src) / src->calib.halfDiagMm;
    float f = sv * sv;
    out[0] *= f; out[1] *= f * f; out[2] *= f * f * f;
    return true;
}

static float vigLog2(const float k[3], float r)
{
    float r2 = r * r;
    float v = 1.0f + k[0] * r2 + k[1] * r2 * r2 + k[2] * r2 * r2 * r2;
    return std::log2(std::max(v, 1e-3f));
}

void fitVignette(float stops, float shape, float k[3])
{
    // Least squares fit of V(r) = 2^(-stops · r^(2·shape)) on r ∈ [0, 1]
    double ata[3][3] = {}, atb[3] = {};
    for (int i = 0; i <= 64; i++)
    {
        double r = i / 64.0, r2 = r * r;
        double basis[3] = {r2, r2 * r2, r2 * r2 * r2};
        double y = std::pow(2.0, -stops * std::pow(r, 2.0 * shape)) - 1.0;
        for (int a = 0; a < 3; a++)
        {
            atb[a] += basis[a] * y;
            for (int b = 0; b < 3; b++) ata[a][b] += basis[a] * basis[b];
        }
    }
    // Gaussian elimination on the 3×3 normal equations
    for (int c = 0; c < 3; c++)
    {
        int piv = c;
        for (int r = c + 1; r < 3; r++) if (std::fabs(ata[r][c]) > std::fabs(ata[piv][c])) piv = r;
        std::swap(ata[c], ata[piv]); std::swap(atb[c], atb[piv]);
        for (int r = c + 1; r < 3; r++)
        {
            double f = ata[r][c] / ata[c][c];
            for (int cc = c; cc < 3; cc++) ata[r][cc] -= f * ata[c][cc];
            atb[r] -= f * atb[c];
        }
    }
    double x[3];
    for (int r = 2; r >= 0; r--)
    {
        double sum = atb[r];
        for (int c = r + 1; c < 3; c++) sum -= ata[r][c] * x[c];
        x[r] = sum / ata[r][r];
    }
    for (int i = 0; i < 3; i++) k[i] = (float)x[i];
}

// ---- assemble --------------------------------------------------------------------------------

LCParams buildParams(const FrameSettings& s)
{
    LCParams P{};
    P.width = s.width;
    P.height = s.height;
    P.rowStride = s.rowStride;
    P.encoding = s.encoding;

    const float W = (float)s.width, H = (float)s.height;
    P.cx = 0.5f * W * (1.0f + s.centerX);
    P.cy = 0.5f * H * (1.0f + s.centerY);
    P.pxToRho = 1.0f / (0.5f * std::sqrt(W * W + H * H));
    P.fit = 1.0f;

    // Distortion
    {
        const Slot& sl = s.distortion;
        float a[5], b[5], delta[5];
        bool hA = sampleDistRho(s, sl.a, a);
        bool hB = sampleDistRho(s, sl.b, b);
        blendDelta(sl, a, hA, b, hB, delta);
        float x = sl.amount * sl.overdrive;
        float sc = s.unbounded ? x : softLimit(x, warpBudget(delta));
        for (int k = 0; k < 5; k++) P.dist[k] = kIdeal[k] + sc * delta[k];
        P.distOn = (!isZero(delta) && sc != 0.0f) ? 1 : 0;
        if (P.distOn && s.autoFit)
        {
            float srcCorner = polyInverse(P.dist, 1.0f);
            if (srcCorner > 1.0f) P.fit = 1.0f / srcCorner;
        }
    }

    // Lateral chromatic aberration
    {
        const Slot& sl = s.ca;
        float aR[5], aB[5], bR[5], bB[5], dR[5], dB[5];
        bool hA = sampleTcaRho(s, sl.a, aR, aB);
        bool hB = sampleTcaRho(s, sl.b, bR, bB);
        blendDelta(sl, aR, hA, bR, hB, dR);
        blendDelta(sl, aB, hA, bB, hB, dB);
        float x = sl.amount * sl.overdrive;
        float sc = x;
        if (!s.unbounded)
        {
            // Budget: channel separation ≤ 2% of the diagonal (0.04 in rho), and a valid warp
            float shift = std::max(maxRadialShift(dR), maxRadialShift(dB));
            float budget = shift > 0.0f ? 0.04f / shift : 1e5f;
            budget = std::min(budget, std::min(warpBudget(dR), warpBudget(dB)));
            sc = softLimit(x, budget);
        }
        for (int k = 0; k < 5; k++)
        {
            P.tcaR[k] = kIdeal[k] + sc * dR[k];
            P.tcaB[k] = kIdeal[k] + sc * dB[k];
        }
        P.tcaOn = ((!isZero(dR) || !isZero(dB)) && sc != 0.0f) ? 1 : 0;
    }

    // Vignetting (log-space blend)
    {
        const Slot& sl = s.vignetting;
        float kA[3] = {0, 0, 0}, kB[3] = {0, 0, 0};
        bool hA = sampleVigRho(s, sl.a, kA);
        bool hB = sampleVigRho(s, sl.b, kB);
        float mA = hA ? 1.0f - sl.mix : 0.0f;
        float mB = hB ? sl.mix : 0.0f;
        float worst = 0.0f;
        for (int i = 0; i <= 32; i++)
        {
            float r = (float)i / 32.0f;
            worst = std::max(worst, std::fabs(mA * vigLog2(kA, r) + mB * vigLog2(kB, r)));
        }
        float x = sl.amount * sl.overdrive;
        float sc = s.unbounded ? x : softLimit(x, worst > 0.0f ? 6.0f / worst : 1e5f);   // floor: −6 stops
        for (int k = 0; k < 3; k++) { P.vigA[k] = kA[k]; P.vigB[k] = kB[k]; }
        P.vigAExp = mA * sc;
        P.vigBExp = mB * sc;
        P.vigOn = (worst > 0.0f && sc != 0.0f) ? 1 : 0;
    }

    // Character
    {
        const Slot& sl = s.character;
        Character ideal;
        const Character& A = (sl.a && sl.a->hasCharacter) ? sl.a->character : ideal;
        const Character& B = (sl.b && sl.b->hasCharacter) ? sl.b->character : ideal;
        float m = sl.mix;
        float x = sl.amount * sl.overdrive;
        bool u = s.unbounded;
        auto D = [&](float va, float vb, float id) { return (va - id) * (1.0f - m) + (vb - id) * m; };

        const Character& dominant = m < 0.5f ? A : B;
        P.blades = dominant.blades;
        P.rotation = dominant.rotation;
        P.curve = A.curve * (1.0f - m) + B.curve * m;
        P.catEye = driveValue(0.0f, D(A.catEye, B.catEye, 0.0f), x, 1.5f, u);
        P.swirl = driveValue(0.0f, D(A.swirl, B.swirl, 0.0f), x, 3.0f, u);
        P.field = driveValue(0.0f, D(A.field, B.field, 0.0f), x, 3.0f, u);
        P.rim = driveValue(0.0f, D(A.rim, B.rim, 0.0f), x, 1.0f, u);
        P.onion = driveValue(0.0f, D(A.onion, B.onion, 0.0f), x, 1.0f, u);
        P.loca = driveValue(0.0f, D(A.loca, B.loca, 0.0f), x, 1.0f, u);
        P.glow = driveValue(0.0f, D(A.glow, B.glow, 0.0f), x, 1.0f, u);
        P.veil = driveValue(0.0f, D(A.veil, B.veil, 0.0f), x, 0.5f, u);
        P.squeeze = driveValue(1.0f, D(A.squeeze, B.squeeze, 1.0f), x, 1.5f, u);
        P.tint = driveValue(0.0f, D(A.tint, B.tint, 0.0f), x, 1.0f, u);
        float gx = std::max(0.0f, s.glowScale);
        P.glow = driveValue(0.0f, P.glow, gx, 1.0f, u);
        P.veil = driveValue(0.0f, P.veil, gx, 0.5f, u);
    }

    // Aperture. Defocus is the blur wide open; the circle of confusion shrinks as 1/N when
    // stopping down. Cat-eye (and the swirl it causes) comes from the barrel clipping a wide
    // pupil, so it fades over the first couple of stops, as do spherical-aberration glow and
    // haze; the iris blades also start to show as straight edges.
    const float N0 = s.maxAperture > 0.0f ? s.maxAperture : 2.0f;
    const float N = std::max(s.aperture, 0.5f);
    const float blurScale = N0 / std::max(N, N0 * 0.5f);
    const float stopsDown = std::max(0.0f, std::log2(N / N0));
    const float open = std::max(0.0f, std::min(1.0f, 1.0f - stopsDown / 2.5f));
    P.catEye *= open;
    P.swirl *= open;
    P.glow *= 0.35f + 0.65f * open;
    P.veil *= 0.5f + 0.5f * open;
    if (P.blades >= 3.0f) P.curve *= 0.4f + 0.6f * open;

    // Defocus
    P.defocusPx = s.defocus * 0.005f * H * blurScale;
    P.edgeBlurPx = 0.012f * H * blurScale;   // field 1.0 → 1.2% of frame height at the corner (≈26 px at UHD)
    P.focusMode = s.focusMode;
    P.focusRadius = s.focusRadius;
    P.focusFalloff = s.focusFalloff;
    P.hlBoost = s.hlBoost;
    P.hlThresh = s.hlThresh;
    P.samples = std::max(8, std::min(512, s.samples));
    P.adaptiveSamples = 1;
    P.glowSamples = 16;
    P.decodeOnFetch = 0;   // the Metal path linearizes first; the CPU path turns this on

    // Playback quality
    if (s.quality == QUALITY_FAST)
    {
        P.samples = std::max(16, P.samples / 3);
        P.glowSamples = 8;
    }
    else if (s.quality == QUALITY_GEOMETRY)
    {
        P.defocusPx = 0.0f;
        P.edgeBlurPx = 0.0f;
        P.glow = 0.0f;
        P.veil = 0.0f;
    }
    return P;
}

} // namespace lc
