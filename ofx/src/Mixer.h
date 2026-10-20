// Mixer.h — turns per-trait slot choices (source lenses, weights, overdrive) into kernel parameters.
#pragma once

#include "LensKernel.h"
#include "Profiles.h"

namespace lc {

// One trait slot: lens A, optional lens B, A↔B mix, amount toward the ideal lens, overdrive.
struct Slot
{
    ProfilePtr a, b;
    float mix = 0.0f;
    float amount = 1.0f;
    float overdrive = 1.0f;
};

struct FrameSettings
{
    int width = 0, height = 0, rowStride = 0, encoding = ENC_GAMMA24;
    float sensorWidthMm = 0.0f;     // 0 = assume the plate matches each profile's calibration format
    float focal = 0.0f;             // 0 = shortest calibrated focal length
    float aperture = 2.0f;          // f-stop the user dialled in
    float maxAperture = 2.0f;       // lens wide open; blur and bokeh character are relative to this
    float centerX = 0.0f, centerY = 0.0f;
    bool autoFit = true;
    bool unbounded = false;

    Slot distortion, ca, vignetting, character;
    float glowScale = 1.0f;         // extra multiplier on glow and veiling glare (after overdrive)

    float defocus = 0.0f;
    int focusMode = FOCUS_RADIAL;
    float focusRadius = 0.25f, focusFalloff = 0.5f;
    float hlBoost = 3.0f, hlThresh = 0.7f;
    int samples = 96;
    int quality = 0;                // 0 Full, 1 Fast (playback), 2 Geometry Only
};

enum { QUALITY_FULL = 0, QUALITY_FAST = 1, QUALITY_GEOMETRY = 2 };

LCParams buildParams(const FrameSettings& s);

// Overdrive soft limiter: ≈ x for x ≪ budget, approaches budget asymptotically.
float softLimit(float x, float budget);

// Fits relative illumination "stops darker at the corner" with a falloff shape to the pa polynomial.
void fitVignette(float stops, float shape, float k[3]);

} // namespace lc
