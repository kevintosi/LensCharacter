// LensKernel.h — the per-pixel lens simulation, shared by the CPU path and the Metal path.
//
// The CPU build #includes this file. The Metal build embeds this same text as a string
// (see Makefile) and compiles it at runtime with LC_METAL defined, so both paths run
// identical math. Keep to the subset both languages accept: scalar floats/ints, plain
// structs, no recursion, no std:: containers.

#ifndef LC_KERNEL_H
#define LC_KERNEL_H

#ifdef LC_METAL
  #define LC_CONST constant
  #define LC_SRC const device float*
  #define LC_FN inline
#else
  #include <cmath>
  #define LC_CONST const
  #define LC_SRC const float*
  #define LC_FN static inline
#endif

namespace lc {

#ifndef LC_METAL
using std::sqrt; using std::cos; using std::sin; using std::floor; using std::fabs;
using std::fmax; using std::fmin; using std::pow; using std::fmod; using std::log2;
using std::exp2; using std::round;
#endif

enum { ENC_LINEAR = 0, ENC_DAVINCI_INTERMEDIATE = 1, ENC_GAMMA24 = 2 };
enum { FOCUS_UNIFORM = 0, FOCUS_RADIAL = 1, FOCUS_ALPHA = 2, FOCUS_ALPHA_INV = 3 };

// Everything the kernel needs for one frame. Layout must stay identical in C++ and MSL:
// only 4-byte scalars and float arrays.
struct LCParams
{
    int   width, height, rowStride;   // rowStride in floats
    int   encoding;
    int   decodeOnFetch;               // 1: source is still encoded (CPU path); 0: already linear (Metal pre-pass)

    float cx, cy;                      // optical centre, pixels
    float pxToRho;                     // 1 / half-diagonal in pixels

    // Distortion, forward model in rho units: rho_d = rho_u * (d0 + d1 r + d2 r^2 + d3 r^3 + d4 r^4)
    int   distOn;
    float dist[5];
    float fit;                         // scale on source coordinates (auto-zoom for barrel)

    // Lateral CA, red/blue relative to green, same polynomial form
    int   tcaOn;
    float tcaR[5];
    float tcaB[5];

    // Relative illumination, two blended sources: gain = VA^expA * VB^expB, V = 1 + k1 r^2 + k2 r^4 + k3 r^6
    int   vigOn;
    float vigA[3]; float vigAExp;
    float vigB[3]; float vigBExp;

    // Character (bokeh shape and field behaviour)
    float blades, curve, rotation, catEye, swirl, field, rim, onion, loca;
    float glow, veil, squeeze, tint;

    // Defocus
    float defocusPx;
    float edgeBlurPx;                  // field-curvature blur at the corner per unit of `field`, independent of defocus
    int   focusMode;
    float focusRadius, focusFalloff;
    float hlBoost, hlThresh;
    int   samples;                     // maximum bokeh samples; fewer are used for small blur radii
    int   adaptiveSamples;             // 1: scale sample count with blur area
    int   glowSamples;
};

struct LCColor { float r, g, b, a; };

#define LC_PI 3.14159265f
#define LC_GOLDEN 2.39996323f

LC_FN float sat(float x) { return fmin(fmax(x, 0.0f), 1.0f); }
LC_FN float mixf(float a, float b, float t) { return a + (b - a) * t; }
LC_FN float sstep(float a, float b, float x)
{
    float t = sat((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

// ---- transfer functions -------------------------------------------------------------
LC_FN float decode(int enc, float v)
{
    if (enc == ENC_DAVINCI_INTERMEDIATE)
        return v <= 0.02740668f ? v / 10.44426855f : exp2(v / 0.07329248f - 7.0f) - 0.0075f;
    if (enc == ENC_GAMMA24)
        return v > 0.0f ? pow(v, 2.4f) : v;
    return v;
}

LC_FN float encode(int enc, float v)
{
    if (enc == ENC_DAVINCI_INTERMEDIATE)
        return v <= 0.00262409f ? v * 10.44426855f : (log2(v + 0.0075f) + 7.0f) * 0.07329248f;
    if (enc == ENC_GAMMA24)
        return v > 0.0f ? pow(v, 1.0f / 2.4f) : v;
    return v;
}

// ---- image access (decoded to linear light) ---------------------------------------------
LC_FN float fetchN(LC_SRC src, LC_CONST LCParams& P, float x, float y, int c)
{
    int ix = (int)fmin(fmax(round(x), 0.0f), (float)(P.width - 1));
    int iy = (int)fmin(fmax(round(y), 0.0f), (float)(P.height - 1));
    float v = src[iy * P.rowStride + ix * 4 + c];
    return P.decodeOnFetch != 0 ? decode(P.encoding, v) : v;
}

LC_FN float fetchB(LC_SRC src, LC_CONST LCParams& P, float x, float y, int c)
{
    x = fmin(fmax(x, 0.0f), (float)(P.width - 1));
    y = fmin(fmax(y, 0.0f), (float)(P.height - 1));
    int x0 = (int)floor(x);
    int y0 = (int)floor(y);
    int x1 = x0 + 1 < P.width ? x0 + 1 : x0;
    int y1 = y0 + 1 < P.height ? y0 + 1 : y0;
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    float a = src[y0 * P.rowStride + x0 * 4 + c];
    float b = src[y0 * P.rowStride + x1 * 4 + c];
    float d = src[y1 * P.rowStride + x0 * 4 + c];
    float e = src[y1 * P.rowStride + x1 * 4 + c];
    if (P.decodeOnFetch != 0)
    {
        a = decode(P.encoding, a); b = decode(P.encoding, b);
        d = decode(P.encoding, d); e = decode(P.encoding, e);
    }
    return mixf(mixf(a, b, fx), mixf(d, e, fx), fy);
}

// ---- radial polynomials ----------------------------------------------------------------
LC_FN float polyEval(LC_CONST float* a, float r)
{
    return a[0] + r * (a[1] + r * (a[2] + r * (a[3] + r * a[4])));
}

LC_FN float polyDeriv(LC_CONST float* a, float r)
{
    return a[1] + r * (2.0f * a[2] + r * (3.0f * a[3] + r * 4.0f * a[4]));
}

// Solve u * P(u) = t for u (Newton). Used to turn the lens's forward model into a lookup.
LC_FN float polyInverse(LC_CONST float* a, float t)
{
    float u = t / fmax(a[0], 0.05f);
    for (int i = 0; i < 8; i++)
    {
        float p = polyEval(a, u);
        float g = u * p - t;
        float dg = p + u * polyDeriv(a, u);
        if (fabs(dg) < 1e-4f) break;
        u = fmax(u - g / dg, 0.0f);
    }
    return u;
}

LC_FN float luma(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// ---- the whole lens, one output pixel -----------------------------------------------------
LC_FN LCColor shade(LC_CONST LCParams& P, LC_SRC src, int px, int py)
{
    float H = (float)P.height;

    // Field position of this output pixel
    float dx = (float)px - P.cx;
    float dy = (float)py - P.cy;
    float rpx = sqrt(dx * dx + dy * dy);
    float r = rpx * P.pxToRho;           // 1.0 at the frame corner
    float r2 = r * r;
    float rdx = 1.0f, rdy = 0.0f;
    if (rpx > 1e-4f) { rdx = dx / rpx; rdy = dy / rpx; }
    float tdx = -rdy, tdy = rdx;

    // Geometry: undo lateral CA per channel, then undo distortion, to find where each
    // channel's light came from in the source plate.
    float rhoR = r, rhoG = r, rhoB = r;
    if (P.tcaOn != 0)
    {
        rhoR = polyInverse(P.tcaR, r);
        rhoB = polyInverse(P.tcaB, r);
    }
    if (P.distOn != 0)
    {
        rhoR = polyInverse(P.dist, rhoR);
        rhoG = polyInverse(P.dist, rhoG);
        rhoB = polyInverse(P.dist, rhoB);
    }
    float k = P.fit / P.pxToRho;
    float xR = P.cx + rdx * rhoR * k, yR = P.cy + rdy * rhoR * k;
    float xG = P.cx + rdx * rhoG * k, yG = P.cy + rdy * rhoG * k;
    float xB = P.cx + rdx * rhoB * k, yB = P.cy + rdy * rhoB * k;

    float alpha = src[py * P.rowStride + px * 4 + 3];

    // Defocus amount
    float m;
    if (P.focusMode == FOCUS_UNIFORM)     m = 1.0f;
    else if (P.focusMode == FOCUS_RADIAL) m = sstep(P.focusRadius, P.focusRadius + P.focusFalloff, r);
    else if (P.focusMode == FOCUS_ALPHA)  m = sat(alpha);
    else                                  m = 1.0f - sat(alpha);
    m += P.field * r2 * (P.focusMode == FOCUS_UNIFORM ? 1.0f : 0.5f);
    // Vintage lenses go soft toward the corners even when the subject is in focus,
    // so field curvature adds its own blur (and with it cat-eye and swirl) at any defocus.
    float Rpx = P.defocusPx * m + P.edgeBlurPx * P.field * r2;

    // Sharp path
    float shR = fetchB(src, P, xR, yR, 0);
    float shG = fetchB(src, P, xG, yG, 1);
    float shB = fetchB(src, P, xB, yB, 2);
    float outR = shR, outG = shG, outB = shB;

    // Bokeh gather: shaped aperture kernel that changes across the field
    float blurMix = sstep(0.6f, 2.0f, Rpx);
    if (blurMix > 0.0f)
    {
        // A disc of radius R px needs roughly R² samples to cover it; small blurs need far
        // fewer than the maximum, which is where most of the frame sits in Sharp Centre mode.
        int N = P.samples;
        if (P.adaptiveSamples != 0)
            N = (int)fmin((float)P.samples, fmax(12.0f, Rpx * Rpx * 0.9f));
        float loca = P.loca;
        float extent = 1.0f + fabs(loca) * 0.35f;
        float eR = 1.0f + loca * 0.35f;
        float eG = 1.0f;
        float eB = 1.0f - loca * 0.35f;
        float edgeSoft = 0.05f;
        float seg = P.blades >= 3.0f ? 2.0f * LC_PI / P.blades : 0.0f;
        float rotRad = P.rotation * LC_PI / 180.0f;
        float ceShift = P.catEye * r * 1.1f;
        float ceX = rdx * ceShift, ceY = rdy * ceShift;
        float tStretch = 1.0f + P.swirl * r2 * 0.8f;
        float rSquash = 1.0f / (1.0f + P.swirl * r2 * 0.3f);
        float rim = fmin(fmax(P.rim, -1.0f), 1.0f);

        float aR = 0.0f, aG = 0.0f, aB = 0.0f;
        float wR = 0.0f, wG = 0.0f, wB = 0.0f;

        for (int i = 0; i < N; i++)
        {
            float fi = (float)i + 0.5f;
            float rn = sqrt(fi / (float)N) * extent;
            float th = (float)i * LC_GOLDEN;

            float shape = 1.0f;
            if (seg > 0.0f)
            {
                float a = fmod(th - rotRad + 64.0f * LC_PI, seg) - 0.5f * seg;
                float poly = cos(LC_PI / P.blades) / cos(a);
                shape = mixf(poly, 1.0f, P.curve);
            }
            float ax = cos(th) * rn * shape;
            float ay = sin(th) * rn * shape;

            // mechanical vignetting (cat eye)
            float ddx = ax + ceX, ddy = ay + ceY;
            float ce = sstep(1.0f + edgeSoft, 1.0f - edgeSoft, sqrt(ddx * ddx + ddy * ddy) / shape);
            if (ce <= 0.0f) continue;

            // swirl: stretch tangentially
            float orad = (ax * rdx + ay * rdy) * rSquash;
            float otan = (ax * tdx + ay * tdy) * tStretch;
            float ox = (orad * rdx + otan * tdx) * Rpx;
            float oy = (orad * rdy + otan * tdy) * Rpx * P.squeeze;

            float sR = fetchN(src, P, xR + ox, yR + oy, 0);
            float sG = fetchN(src, P, xG + ox, yG + oy, 1);
            float sB = fetchN(src, P, xB + ox, yB + oy, 2);

            float hl = fmax(luma(sR, sG, sB) - P.hlThresh, 0.0f) * 4.0f;
            float hw = 1.0f + P.hlBoost * hl * hl;

            float onionW = 1.0f + P.onion * 0.6f * cos(rn * 38.0f);
            float base = ce * hw * onionW;

            float pR = sat(rn / eR), pG = sat(rn / eG), pB = sat(rn / eB);
            float rimR = rim >= 0.0f ? 1.0f + rim * 4.0f * pR * pR * pR * pR : 1.0f + rim * pR * pR;
            float rimG = rim >= 0.0f ? 1.0f + rim * 4.0f * pG * pG * pG * pG : 1.0f + rim * pG * pG;
            float rimB = rim >= 0.0f ? 1.0f + rim * 4.0f * pB * pB * pB * pB : 1.0f + rim * pB * pB;
            float kR = sstep(eR + edgeSoft, eR - edgeSoft, rn) * rimR * base;
            float kG = sstep(eG + edgeSoft, eG - edgeSoft, rn) * rimG * base;
            float kB = sstep(eB + edgeSoft, eB - edgeSoft, rn) * rimB * base;

            aR += sR * kR; wR += kR;
            aG += sG * kG; wG += kG;
            aB += sB * kB; wB += kB;
        }
        if (wR > 0.0f) outR = mixf(shR, aR / wR, blurMix);
        if (wG > 0.0f) outG = mixf(shG, aG / wG, blurMix);
        if (wB > 0.0f) outB = mixf(shB, aB / wB, blurMix);
    }

    // Glow (spherical aberration) and veiling glare
    if (P.glow > 0.0f || P.veil > 0.0f)
    {
        float gR = 0.0f, gG = 0.0f, gB = 0.0f, gw = 0.0f;
        float gRad = 0.012f * H;
        int G = P.glowSamples;
        for (int i = 0; i < G; i++)
        {
            float fi = (float)i + 0.5f;
            float rn = sqrt(fi / (float)G);
            float th = (float)i * LC_GOLDEN;
            float ox = cos(th) * rn * gRad;
            float oy = sin(th) * rn * gRad;
            float sR = fetchB(src, P, xR + ox, yR + oy, 0);
            float sG = fetchB(src, P, xG + ox, yG + oy, 1);
            float sB = fetchB(src, P, xB + ox, yB + oy, 2);
            float hl = fmax(luma(sR, sG, sB) - 0.5f, 0.0f) * 3.0f;
            float kk = (1.0f - rn * 0.7f) * (1.0f + hl * hl * 2.0f);
            gR += sR * kk; gG += sG * kk; gB += sB * kk; gw += kk;
        }
        gR /= gw; gG /= gw; gB /= gw;
        float glow = sat(P.glow);
        outR = mixf(outR, fmax(outR, gR), glow);
        outG = mixf(outG, fmax(outG, gG), glow);
        outB = mixf(outB, fmax(outB, gB), glow);
        outR += P.veil * gR; outG += P.veil * gG; outB += P.veil * gB;
        float norm = 1.0f / (1.0f + P.veil * 0.5f);
        outR *= norm; outG *= norm; outB *= norm;
    }

    // Relative illumination (measured vignetting), blended in log space
    if (P.vigOn != 0)
    {
        float r4 = r2 * r2, r6 = r4 * r2;
        float vA = fmax(1.0f + P.vigA[0] * r2 + P.vigA[1] * r4 + P.vigA[2] * r6, 1e-3f);
        float vB = fmax(1.0f + P.vigB[0] * r2 + P.vigB[1] * r4 + P.vigB[2] * r6, 1e-3f);
        float v = pow(vA, P.vigAExp) * pow(vB, P.vigBExp);
        outR *= v; outG *= v; outB *= v;
    }

    // Coating tint
    outR *= 1.0f + P.tint * 0.06f;
    outG *= 1.0f + P.tint * 0.01f;
    outB *= 1.0f - P.tint * 0.14f;

    LCColor c;
    c.r = encode(P.encoding, outR);
    c.g = encode(P.encoding, outG);
    c.b = encode(P.encoding, outB);
    c.a = alpha;
    // NaN / Inf scrub
    if (!(c.r == c.r) || fabs(c.r) > 1e20f) c.r = 0.0f;
    if (!(c.g == c.g) || fabs(c.g) > 1e20f) c.g = 0.0f;
    if (!(c.b == c.b) || fabs(c.b) > 1e20f) c.b = 0.0f;
    return c;
}

} // namespace lc

#endif
