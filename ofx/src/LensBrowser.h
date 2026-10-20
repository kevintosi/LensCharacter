// LensBrowser.h — native popup for browsing and picking lens profiles.
#pragma once

#include <string>

#include "Profiles.h"

namespace lc {

struct BrowserRequest
{
    std::string title;        // e.g. "Choose Lens" or "Choose Distortion Lens"
    std::string currentId;    // preselected profile id ("" = none, "custom" = Custom lens)
    ProfilePtr custom;        // the instance's Custom lens, listed under My Lenses
    float aperture = 2.0f;    // for the vignetting readout
    bool allowNone = false;   // show "Use Base Lens" (for per-trait overrides)
};

// Opens the browser as a modal window. Returns true when the user picked a lens;
// outId is the profile id, "custom" for the Custom lens, or "" for "Use Base Lens".
bool showLensBrowser(const BrowserRequest& p_Request, std::string& outId);

// Renders the browser offscreen to a PNG (self-test and documentation).
bool snapshotLensBrowser(const BrowserRequest& p_Request, const std::string& p_Category,
                         const std::string& p_Search, const std::string& p_SelectId, const std::string& p_PngPath);

} // namespace lc
