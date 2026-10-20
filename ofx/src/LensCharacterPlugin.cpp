// LensCharacterPlugin.cpp — OpenFX plugin: parameters, render dispatch (Metal / CPU), lens browser, custom lens.
//
// Inspector layout (simple first, detail collapsed):
//   Lens          Choose Lens… · aperture · input encoding
//   Look          one slider per trait, 1 = the real lens, higher = overdrive
//   Focus         defocus and where it applies
//   Mix & Match   take individual traits from other lenses          (collapsed)
//   Advanced      focal length, sensor size, optical centre, limits  (collapsed)
//   Custom Lens   build and save your own lens                       (collapsed)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "LensBrowser.h"
#include "LensKernel.h"
#include "Mixer.h"
#include "Profiles.h"

#define kPluginName "LensCharacter"
#define kPluginGrouping "LensCharacter"
#define kPluginDescription "Lens emulation from measured profiles: distortion, chromatic aberration, vignetting, bokeh and glow. Mix traits from different lenses and overdrive them."
#define kPluginIdentifier "com.ktosi.LensCharacter"
#define kPluginVersionMajor 0
#define kPluginVersionMinor 4

#define kDefaultLensId "builtin:Helios 44-2 58mm (character)"

using namespace OFX;

static const char* kSlots[4] = {"dist", "ca", "vig", "char"};
static const char* kSlotNames[4] = {"Distortion", "Chromatic Aberration", "Vignetting", "Bokeh Character"};

static std::string P(const char* slot, const char* name) { return std::string(slot) + name; }

#ifdef __APPLE__
extern void RunMetalKernel(void* p_CmdQ, const lc::LCParams& p_Params, const float* p_Input, float* p_Output);
#endif

////////////////////////////////////////////////////////////////////////////////

class LensProcessor : public ImageProcessor
{
public:
    explicit LensProcessor(ImageEffect& p_Instance) : ImageProcessor(p_Instance), m_Src(nullptr) {}

    void setSrcImg(Image* p_Src) { m_Src = p_Src; }
    void setParams(const lc::LCParams& p_Params) { m_Params = p_Params; }

    virtual void processImagesMetal()
    {
#ifdef __APPLE__
        lc::LCParams params = m_Params;
        params.rowStride = params.width * 4;   // GPU buffers are tightly packed
        RunMetalKernel(_pMetalCmdQ, params, static_cast<const float*>(m_Src->getPixelData()),
                       static_cast<float*>(_dstImg->getPixelData()));
#endif
    }

    virtual void multiThreadProcessImages(OfxRectI p_Window)
    {
        const OfxRectI& b = m_Src->getBounds();
        const float* src = static_cast<const float*>(m_Src->getPixelAddress(b.x1, b.y1));
        lc::LCParams params = m_Params;
        params.decodeOnFetch = 1;   // no linear pre-pass on the CPU path
        for (int y = p_Window.y1; y < p_Window.y2; ++y)
        {
            if (_effect.abort()) break;
            float* dst = static_cast<float*>(_dstImg->getPixelAddress(p_Window.x1, y));
            for (int x = p_Window.x1; x < p_Window.x2; ++x, dst += 4)
            {
                lc::LCColor c = lc::shade(params, src, x - b.x1, y - b.y1);
                dst[0] = c.r; dst[1] = c.g; dst[2] = c.b; dst[3] = c.a;
            }
        }
    }

private:
    Image* m_Src;
    lc::LCParams m_Params;
};

////////////////////////////////////////////////////////////////////////////////

class LensCharacterPlugin : public ImageEffect
{
public:
    explicit LensCharacterPlugin(OfxImageEffectHandle p_Handle);

    virtual void render(const RenderArguments& p_Args);
    virtual void changedParam(const InstanceChangedArgs& p_Args, const std::string& p_ParamName);

private:
    struct SlotParams { StringParam* id; StringParam* info; };

    lc::ProfilePtr resolveId(const std::string& p_Id, const lc::ProfilePtr& p_Base, const lc::ProfilePtr& p_Custom) const;
    lc::ProfilePtr buildCustom(double p_Time) const;
    void refreshInfo();
    void chooseLens(double p_Time, int p_Slot);   // -1 = main lens
    void copyBaseIntoCustom(double p_Time);
    void saveCustom(double p_Time);

    Clip* m_DstClip;
    Clip* m_SrcClip;

    StringParam* m_LensId; StringParam* m_LensInfo;
    DoubleParam* m_Aperture;
    ChoiceParam* m_Encoding;
    ChoiceParam* m_Quality;

    DoubleParam* m_Look[4];     // distortion, CA, vignetting, bokeh character
    DoubleParam* m_LookGlow;

    DoubleParam* m_Defocus; ChoiceParam* m_FocusMode;
    DoubleParam* m_FocusRadius; DoubleParam* m_FocusFalloff;

    SlotParams m_Slots[4];

    DoubleParam* m_Focal; DoubleParam* m_SensorWidth; Double2DParam* m_Center;
    BooleanParam* m_AutoFit; BooleanParam* m_Unbounded;
    DoubleParam* m_HlBoost; DoubleParam* m_HlThresh; IntParam* m_Samples;

    StringParam* m_CustomName; StringParam* m_CustomStatus;
    DoubleParam *m_cK1, *m_cK2, *m_cCaRed, *m_cCaBlue, *m_cVigStops, *m_cVigShape;
    IntParam* m_cBlades;
    DoubleParam *m_cCurve, *m_cRotation, *m_cCatEye, *m_cSwirl, *m_cField, *m_cRim, *m_cOnion;
    DoubleParam *m_cLoca, *m_cGlow, *m_cVeil, *m_cSqueeze, *m_cTint, *m_cMaxAperture;
};

LensCharacterPlugin::LensCharacterPlugin(OfxImageEffectHandle p_Handle)
    : ImageEffect(p_Handle)
{
    m_DstClip = fetchClip(kOfxImageEffectOutputClipName);
    m_SrcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    m_LensId = fetchStringParam("lensId");
    m_LensInfo = fetchStringParam("lensInfo");
    m_Aperture = fetchDoubleParam("aperture");
    m_Encoding = fetchChoiceParam("encoding");
    m_Quality = fetchChoiceParam("quality");

    for (int i = 0; i < 4; i++) m_Look[i] = fetchDoubleParam(P("look", kSlots[i]));
    m_LookGlow = fetchDoubleParam("lookGlow");

    m_Defocus = fetchDoubleParam("defocus");
    m_FocusMode = fetchChoiceParam("focusMode");
    m_FocusRadius = fetchDoubleParam("focusRadius");
    m_FocusFalloff = fetchDoubleParam("focusFalloff");

    for (int i = 0; i < 4; i++)
    {
        m_Slots[i].id = fetchStringParam(P(kSlots[i], "Id"));
        m_Slots[i].info = fetchStringParam(P(kSlots[i], "Info"));
    }

    m_Focal = fetchDoubleParam("focal");
    m_SensorWidth = fetchDoubleParam("sensorWidth");
    m_Center = fetchDouble2DParam("center");
    m_AutoFit = fetchBooleanParam("autoFit");
    m_Unbounded = fetchBooleanParam("unbounded");
    m_HlBoost = fetchDoubleParam("hlBoost");
    m_HlThresh = fetchDoubleParam("hlThresh");
    m_Samples = fetchIntParam("samples");

    m_CustomName = fetchStringParam("customName");
    m_CustomStatus = fetchStringParam("customStatus");
    m_cK1 = fetchDoubleParam("cK1");
    m_cK2 = fetchDoubleParam("cK2");
    m_cCaRed = fetchDoubleParam("cCaRed");
    m_cCaBlue = fetchDoubleParam("cCaBlue");
    m_cVigStops = fetchDoubleParam("cVigStops");
    m_cVigShape = fetchDoubleParam("cVigShape");
    m_cBlades = fetchIntParam("cBlades");
    m_cCurve = fetchDoubleParam("cCurve");
    m_cRotation = fetchDoubleParam("cRotation");
    m_cCatEye = fetchDoubleParam("cCatEye");
    m_cSwirl = fetchDoubleParam("cSwirl");
    m_cField = fetchDoubleParam("cField");
    m_cRim = fetchDoubleParam("cRim");
    m_cOnion = fetchDoubleParam("cOnion");
    m_cLoca = fetchDoubleParam("cLoca");
    m_cGlow = fetchDoubleParam("cGlow");
    m_cVeil = fetchDoubleParam("cVeil");
    m_cSqueeze = fetchDoubleParam("cSqueeze");
    m_cTint = fetchDoubleParam("cTint");
    m_cMaxAperture = fetchDoubleParam("cMaxAperture");

    refreshInfo();
}

// ---- profiles -------------------------------------------------------------------------------

lc::ProfilePtr LensCharacterPlugin::resolveId(const std::string& p_Id, const lc::ProfilePtr& p_Base, const lc::ProfilePtr& p_Custom) const
{
    if (p_Id.empty()) return p_Base;
    if (p_Id == "custom") return p_Custom;
    if (lc::ProfilePtr p = lc::Library::get().findById(p_Id)) return p;
    return lc::Library::get().find(p_Id);   // older projects stored names
}

lc::ProfilePtr LensCharacterPlugin::buildCustom(double t) const
{
    auto p = std::make_shared<lc::Profile>();
    std::string name;
    m_CustomName->getValueAtTime(t, name);
    p->name = name.empty() ? "Custom Lens (your sliders)" : name + " (Custom Lens sliders)";
    p->id = "custom";
    p->source = "custom";
    p->maker = "My Lenses";
    p->maxAperture = (float)m_cMaxAperture->getValueAtTime(t);

    lc::DistEntry d{};
    d.a[0] = 1.0f; d.a[2] = (float)m_cK1->getValueAtTime(t); d.a[4] = (float)m_cK2->getValueAtTime(t);
    p->dist.push_back(d);

    lc::TcaEntry c{};
    c.r[0] = 1.0f + (float)m_cCaRed->getValueAtTime(t) / 1000.0f;
    c.b[0] = 1.0f + (float)m_cCaBlue->getValueAtTime(t) / 1000.0f;
    p->tca.push_back(c);

    lc::VigEntry v{};
    lc::fitVignette((float)m_cVigStops->getValueAtTime(t), (float)m_cVigShape->getValueAtTime(t), v.k);
    p->vig.push_back(v);

    lc::Character& ch = p->character;
    ch.blades = (float)m_cBlades->getValueAtTime(t);
    ch.curve = (float)m_cCurve->getValueAtTime(t);
    ch.rotation = (float)m_cRotation->getValueAtTime(t);
    ch.catEye = (float)m_cCatEye->getValueAtTime(t);
    ch.swirl = (float)m_cSwirl->getValueAtTime(t);
    ch.field = (float)m_cField->getValueAtTime(t);
    ch.rim = (float)m_cRim->getValueAtTime(t);
    ch.onion = (float)m_cOnion->getValueAtTime(t);
    ch.loca = (float)m_cLoca->getValueAtTime(t);
    ch.glow = (float)m_cGlow->getValueAtTime(t);
    ch.veil = (float)m_cVeil->getValueAtTime(t);
    ch.squeeze = (float)m_cSqueeze->getValueAtTime(t);
    ch.tint = (float)m_cTint->getValueAtTime(t);
    p->hasCharacter = true;
    return p;
}

void LensCharacterPlugin::refreshInfo()
{
    std::string lensId;
    m_LensId->getValue(lensId);
    lc::ProfilePtr custom = buildCustom(0);
    lc::ProfilePtr base = resolveId(lensId, nullptr, custom);
    m_LensInfo->setValue(base ? base->name : "No lens chosen: an ideal lens");

    for (int i = 0; i < 4; i++)
    {
        std::string id;
        m_Slots[i].id->getValue(id);
        lc::ProfilePtr p = id.empty() ? nullptr : resolveId(id, base, custom);
        m_Slots[i].info->setValue(id.empty() ? std::string("Same as main lens") : p ? p->name : "Lens not found: " + id);
    }
}

void LensCharacterPlugin::chooseLens(double t, int p_Slot)
{
    lc::BrowserRequest req;
    req.custom = buildCustom(t);
    req.aperture = (float)m_Aperture->getValueAtTime(t);
    req.allowNone = p_Slot >= 0;
    req.title = p_Slot < 0 ? "Choose Lens" : std::string("Choose Lens for ") + kSlotNames[p_Slot];
    StringParam* target = p_Slot < 0 ? m_LensId : m_Slots[p_Slot].id;
    target->getValue(req.currentId);

    std::string picked;
    if (lc::showLensBrowser(req, picked))
    {
        target->setValue(picked);
        // A newly chosen main lens starts wide open, the way its character is defined
        if (p_Slot < 0)
            if (lc::ProfilePtr lens = resolveId(picked, nullptr, buildCustom(t)))
                if (lens->maxAperture > 0.0f) m_Aperture->setValue(lens->maxAperture);
        refreshInfo();
    }
}

void LensCharacterPlugin::copyBaseIntoCustom(double t)
{
    std::string lensId;
    m_LensId->getValueAtTime(t, lensId);
    lc::ProfilePtr base = lensId == "custom" ? nullptr : resolveId(lensId, nullptr, nullptr);
    if (!base)
    {
        m_CustomStatus->setValue("Choose a lens first (Lens > Choose Lens…), then copy it here.");
        return;
    }
    const float focal = (float)m_Focal->getValueAtTime(t);
    const float aperture = (float)m_Aperture->getValueAtTime(t);

    if (const lc::Profile* src = base->distSource())
    {
        float a[5];
        if (lc::sampleDistortion(*src, focal, a) && a[0] != 0.0f)
        {
            m_cK1->setValue(a[2] / a[0]);
            m_cK2->setValue(a[4] / a[0]);
        }
    }
    if (const lc::Profile* src = base->tcaSource())
    {
        float r[5], b[5];
        if (lc::sampleTca(*src, focal, r, b))
        {
            m_cCaRed->setValue((r[0] - 1.0f) * 1000.0f);
            m_cCaBlue->setValue((b[0] - 1.0f) * 1000.0f);
        }
    }
    if (const lc::Profile* src = base->vigSource())
    {
        float k[3];
        if (lc::sampleVignetting(*src, focal, aperture, k))
        {
            auto V = [&](float r) { float r2 = r * r; return std::max(1.0f + k[0] * r2 + k[1] * r2 * r2 + k[2] * r2 * r2 * r2, 1e-3f); };
            float stops = -std::log2(V(1.0f));
            float half = -std::log2(V(0.5f));
            float shape = 1.0f;
            if (stops > 0.01f && half > 0.0f) shape = std::log(half / stops) / (2.0f * std::log(0.5f));
            m_cVigStops->setValue(std::max(0.0f, stops));
            m_cVigShape->setValue(std::min(4.0f, std::max(0.5f, shape)));
        }
    }
    if (base->maxAperture > 0.0f) m_cMaxAperture->setValue(base->maxAperture);
    if (base->hasCharacter)
    {
        const lc::Character& c = base->character;
        m_cBlades->setValue((int)c.blades);
        m_cCurve->setValue(c.curve);
        m_cRotation->setValue(c.rotation);
        m_cCatEye->setValue(c.catEye);
        m_cSwirl->setValue(c.swirl);
        m_cField->setValue(c.field);
        m_cRim->setValue(c.rim);
        m_cOnion->setValue(c.onion);
        m_cLoca->setValue(c.loca);
        m_cGlow->setValue(c.glow);
        m_cVeil->setValue(c.veil);
        m_cSqueeze->setValue(c.squeeze);
        m_cTint->setValue(c.tint);
    }
    m_CustomName->setValue(base->name + " custom");
    m_LensId->setValue("custom");
    m_CustomStatus->setValue("Copied " + base->name + " into the Custom Lens, which is now the main lens. Adjust the sliders, then save.");
    refreshInfo();
}

void LensCharacterPlugin::saveCustom(double t)
{
    std::string name;
    m_CustomName->getValueAtTime(t, name);
    std::string key = lc::normalizeForSearch(name);
    if (key.empty() || key == "custom")
    {
        m_CustomStatus->setValue("Type a name for this lens first.");
        return;
    }
    lc::Profile copy = *buildCustom(t);
    copy.name = name;
    copy.source = "user";
    copy.maker = "My Lenses";
    std::string path, error;
    if (lc::Library::get().saveUserProfile(copy, path, error))
    {
        m_LensId->setValue("user:" + name);
        m_CustomStatus->setValue("Saved \"" + name + "\" to My Lenses and set it as the main lens.");
    }
    else
        m_CustomStatus->setValue(error);
    refreshInfo();
}

void LensCharacterPlugin::changedParam(const InstanceChangedArgs& p_Args, const std::string& p_ParamName)
{
    if (p_Args.reason != eChangeUserEdit) return;

    if (p_ParamName == "chooseLens") { chooseLens(p_Args.time, -1); return; }
    for (int i = 0; i < 4; i++)
        if (p_ParamName == P(kSlots[i], "Choose")) { chooseLens(p_Args.time, i); return; }
    if (p_ParamName == "customUse") { m_LensId->setValue("custom"); refreshInfo(); return; }
    if (p_ParamName == "customCopy") { copyBaseIntoCustom(p_Args.time); return; }
    if (p_ParamName == "customSave") { saveCustom(p_Args.time); return; }
    if (p_ParamName == "reloadLibrary")
    {
        lc::Library::get().reload();
        m_CustomStatus->setValue("Reloaded " + std::to_string(lc::Library::get().size()) + " lens profiles.");
        refreshInfo();
        return;
    }
    if (p_ParamName == "customName") refreshInfo();
}

// ---- render ----------------------------------------------------------------------------------

// Records how Resolve flags renders (playback, scrubbing, Deliver) the first time each
// combination appears, so Fast quality can later switch itself off for final renders.
static void logRenderFlags(const RenderArguments& a)
{
    static std::mutex mutex;
    static int seen = 0;
    int key = 1 | (a.renderQualityDraft ? 2 : 0) | (a.interactiveRenderStatus ? 4 : 0) | (a.sequentialRenderStatus ? 8 : 0) |
              (a.isEnabledMetalRender ? 16 : 0) | (a.renderScale.x < 0.99 ? 32 : 0);
    std::lock_guard<std::mutex> lock(mutex);
    if (seen & (1 << (key & 31))) return;
    seen |= 1 << (key & 31);
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/Library/Logs/LensCharacter.log";
    if (FILE* f = fopen(path.c_str(), "a"))
    {
        fprintf(f, "render flags: draft=%d interactive=%d sequential=%d metal=%d scale=%.3f\n",
                (int)a.renderQualityDraft, (int)a.interactiveRenderStatus, (int)a.sequentialRenderStatus,
                (int)a.isEnabledMetalRender, a.renderScale.x);
        fclose(f);
    }
}

void LensCharacterPlugin::render(const RenderArguments& p_Args)
{
    if (m_DstClip->getPixelDepth() != eBitDepthFloat || m_DstClip->getPixelComponents() != ePixelComponentRGBA)
        throwSuiteStatusException(kOfxStatErrUnsupported);

    std::unique_ptr<Image> dst(m_DstClip->fetchImage(p_Args.time));
    std::unique_ptr<Image> src(m_SrcClip->fetchImage(p_Args.time));
    if (!dst || !src) throwSuiteStatusException(kOfxStatFailed);

    const double t = p_Args.time;
    const OfxRectI& b = src->getBounds();

    lc::FrameSettings s;
    s.width = b.x2 - b.x1;
    s.height = b.y2 - b.y1;
    s.rowStride = src->getRowBytes() / (int)sizeof(float);
    m_Encoding->getValueAtTime(t, s.encoding);
    s.aperture = (float)m_Aperture->getValueAtTime(t);
    s.focal = (float)m_Focal->getValueAtTime(t);
    s.sensorWidthMm = (float)m_SensorWidth->getValueAtTime(t);
    double cx, cy;
    m_Center->getValueAtTime(t, cx, cy);
    s.centerX = (float)cx;
    s.centerY = (float)cy;
    s.autoFit = m_AutoFit->getValueAtTime(t);
    s.unbounded = m_Unbounded->getValueAtTime(t);

    std::string lensId;
    m_LensId->getValueAtTime(t, lensId);
    lc::ProfilePtr custom = buildCustom(t);
    lc::ProfilePtr base = resolveId(lensId, nullptr, custom);

    lc::Slot* slots[4] = {&s.distortion, &s.ca, &s.vignetting, &s.character};
    for (int i = 0; i < 4; i++)
    {
        std::string id;
        m_Slots[i].id->getValueAtTime(t, id);
        slots[i]->a = resolveId(id, base, custom);
        slots[i]->amount = 1.0f;
        slots[i]->overdrive = (float)m_Look[i]->getValueAtTime(t);
    }
    s.glowScale = (float)m_LookGlow->getValueAtTime(t);
    m_Quality->getValueAtTime(t, s.quality);
    {
        // Blur scales from the wide-open aperture of the lens that supplies the bokeh
        const lc::ProfilePtr& bokehLens = s.character.a ? s.character.a : base;
        s.maxAperture = bokehLens && bokehLens->maxAperture > 0.0f ? bokehLens->maxAperture : 2.0f;
    }
    logRenderFlags(p_Args);

    s.defocus = (float)m_Defocus->getValueAtTime(t);
    m_FocusMode->getValueAtTime(t, s.focusMode);
    s.focusRadius = (float)m_FocusRadius->getValueAtTime(t);
    s.focusFalloff = (float)m_FocusFalloff->getValueAtTime(t);
    s.hlBoost = (float)m_HlBoost->getValueAtTime(t);
    s.hlThresh = (float)m_HlThresh->getValueAtTime(t);
    s.samples = m_Samples->getValueAtTime(t);

    LensProcessor proc(*this);
    proc.setDstImg(dst.get());
    proc.setSrcImg(src.get());
    proc.setGPURenderArgs(p_Args);
    proc.setRenderWindow(p_Args.renderWindow);
    proc.setParams(lc::buildParams(s));
    proc.process();
}

////////////////////////////////////////////////////////////////////////////////
// Factory

class LensCharacterFactory : public PluginFactoryHelper<LensCharacterFactory>
{
public:
    LensCharacterFactory() : PluginFactoryHelper<LensCharacterFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor) {}
    virtual void load() {}
    virtual void unload() {}
    virtual void describe(ImageEffectDescriptor& p_Desc);
    virtual void describeInContext(ImageEffectDescriptor& p_Desc, ContextEnum p_Context);
    virtual ImageEffect* createInstance(OfxImageEffectHandle p_Handle, ContextEnum) { return new LensCharacterPlugin(p_Handle); }
};

void LensCharacterFactory::describe(ImageEffectDescriptor& p_Desc)
{
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);
    p_Desc.addSupportedContext(eContextFilter);
    p_Desc.addSupportedContext(eContextGeneral);
    p_Desc.addSupportedBitDepth(eBitDepthFloat);
    p_Desc.setSingleInstance(false);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setSupportsMultiResolution(false);
    p_Desc.setSupportsTiles(false);
    p_Desc.setTemporalClipAccess(false);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(false);
#ifdef __APPLE__
    p_Desc.setSupportsMetalRender(true);
#endif
}

// ---- descriptor helpers ----------------------------------------------------------------------

struct Ui
{
    ImageEffectDescriptor& d;
    PageParamDescriptor* page;
    GroupParamDescriptor* group = nullptr;

    template <typename T> T* place(T* p)
    {
        if (group) p->setParent(*group);
        page->addChild(*p);
        return p;
    }

    void beginGroup(const std::string& name, const std::string& label, bool open)
    {
        group = d.defineGroupParam(name);
        group->setLabels(label, label, label);
        group->setOpen(open);
    }

    DoubleParamDescriptor* dbl(const std::string& name, const std::string& label, const std::string& hint,
                               double def, double lo, double hi, double dispLo, double dispHi)
    {
        DoubleParamDescriptor* p = d.defineDoubleParam(name);
        p->setLabels(label, label, label);
        p->setScriptName(name);
        p->setHint(hint);
        p->setDefault(def);
        p->setRange(lo, hi);
        p->setDisplayRange(dispLo, dispHi);
        p->setIncrement((dispHi - dispLo) / 200.0);
        return place(p);
    }

    IntParamDescriptor* integer(const std::string& name, const std::string& label, const std::string& hint, int def, int lo, int hi)
    {
        IntParamDescriptor* p = d.defineIntParam(name);
        p->setLabels(label, label, label);
        p->setHint(hint);
        p->setDefault(def);
        p->setRange(lo, hi);
        p->setDisplayRange(lo, hi);
        return place(p);
    }

    BooleanParamDescriptor* boolean(const std::string& name, const std::string& label, const std::string& hint, bool def)
    {
        BooleanParamDescriptor* p = d.defineBooleanParam(name);
        p->setLabels(label, label, label);
        p->setHint(hint);
        p->setDefault(def);
        return place(p);
    }

    PushButtonParamDescriptor* button(const std::string& name, const std::string& label, const std::string& hint)
    {
        PushButtonParamDescriptor* p = d.definePushButtonParam(name);
        p->setLabels(label, label, label);
        p->setHint(hint);
        return place(p);
    }

    // Visible read-only text
    StringParamDescriptor* label(const std::string& name, const std::string& label, const std::string& hint)
    {
        StringParamDescriptor* p = d.defineStringParam(name);
        p->setLabels(label, label, label);
        p->setHint(hint);
        p->setStringType(eStringTypeLabel);
        p->setAnimates(false);
        p->setEvaluateOnChange(false);
        p->setIsPersistant(false);
        return place(p);
    }

    // Stored value the user never edits directly (set by the lens browser)
    StringParamDescriptor* hidden(const std::string& name, const std::string& def)
    {
        StringParamDescriptor* p = d.defineStringParam(name);
        p->setLabels(name, name, name);
        p->setDefault(def);
        p->setAnimates(false);
        p->setIsSecret(true);
        return place(p);
    }

    StringParamDescriptor* text(const std::string& name, const std::string& label, const std::string& hint)
    {
        StringParamDescriptor* p = d.defineStringParam(name);
        p->setLabels(label, label, label);
        p->setHint(hint);
        p->setStringType(eStringTypeSingleLine);
        p->setAnimates(false);
        return place(p);
    }
};

void LensCharacterFactory::describeInContext(ImageEffectDescriptor& p_Desc, ContextEnum)
{
    ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(false);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(false);

    Ui ui{p_Desc, p_Desc.definePageParam("Controls")};

    // Lens
    ui.beginGroup("grpLens", "Lens", true);
    ui.button("chooseLens", "Choose Lens…", "Browse 1,500+ measured lenses and character presets");
    ui.label("lensInfo", "Lens", "The lens currently applied");
    ui.hidden("lensId", kDefaultLensId);
    ui.dbl("aperture", "Aperture (f-stop)", "Like a real lens: stopping down shrinks the blur, fades cat-eye and swirl, reduces glow and shows the iris blades. Also selects the measured vignetting. Choosing a lens sets it wide open.", 2.0, 0.7, 32, 0.95, 16);
    {
        ChoiceParamDescriptor* c = p_Desc.defineChoiceParam("encoding");
        c->setLabels("Input Encoding", "Input Encoding", "Input Encoding");
        c->setHint("How the clip is encoded on this node. The optics are simulated in linear light, so set this to match your timeline.");
        c->appendOption("Linear");
        c->appendOption("DaVinci Intermediate");
        c->appendOption("Gamma 2.4 (Rec.709)");
        c->setDefault(2);
        ui.place(c);
    }
    {
        ChoiceParamDescriptor* c = p_Desc.defineChoiceParam("quality");
        c->setLabels("Quality", "Quality", "Quality");
        c->setHint("Fast uses a third of the bokeh samples for smoother playback. Geometry Only skips blur and glow and keeps distortion, CA and vignetting. Set Full before delivering.");
        c->appendOption("Full");
        c->appendOption("Fast (playback)");
        c->appendOption("Geometry Only (no blur)");
        c->setDefault(0);
        ui.place(c);
    }

    // Look
    ui.beginGroup("grpLook", "Look", true);
    const char* lookHints[4] = {
        "Geometric distortion. 1 = the real lens, 0 = none, higher pushes it further.",
        "Colour fringing toward the edges. 1 = the real lens. Real primes are subtle; try 5–20.",
        "Corner darkening. 1 = the real lens at this aperture.",
        "Bokeh shape, cat-eye, swirl, soft corners and coating tint. Shows in out-of-focus areas and toward the corners. 1 = the lens as profiled."};
    const char* lookLabels[4] = {"Distortion", "Chromatic Aberration", "Vignetting", "Bokeh Character"};
    const double lookMax[4] = {10, 30, 3, 3};
    for (int i = 0; i < 4; i++)
        ui.dbl(P("look", kSlots[i]), lookLabels[i], lookHints[i], 1.0, 0, 1000, 0, lookMax[i]);
    ui.dbl("lookGlow", "Glow and Haze", "Halo around highlights and low-contrast veil. 1 = the lens as profiled.", 1.0, 0, 100, 0, 3);

    // Focus
    ui.beginGroup("grpFocus", "Focus", true);
    ui.dbl("defocus", "Defocus", "How out of focus the image is outside the sharp area. Bokeh shape and swirl show up in the blurred areas.", 3, 0, 40, 0, 20);
    {
        ChoiceParamDescriptor* c = p_Desc.defineChoiceParam("focusMode");
        c->setLabels("Focus Area", "Focus Area", "Focus Area");
        c->setHint("Where the blur goes. Alpha modes read the clip's alpha: 1 = fully defocused.");
        c->appendOption("Whole Frame");
        c->appendOption("Sharp Centre");
        c->appendOption("From Alpha");
        c->appendOption("From Inverted Alpha");
        c->setDefault(1);
        ui.place(c);
    }
    ui.dbl("focusRadius", "Sharp Area Size", "Size of the sharp centre (Sharp Centre mode)", 0.25, 0, 1.5, 0, 1.5);
    ui.dbl("focusFalloff", "Focus Falloff", "How gradually blur builds outside the sharp area", 0.5, 0.01, 2, 0.01, 2);

    // Mix & Match
    ui.beginGroup("grpMix", "Mix & Match", false);
    for (int i = 0; i < 4; i++)
    {
        std::string n = kSlotNames[i];
        ui.button(P(kSlots[i], "Choose"), n + " From…", "Take " + n + " from a different lens. Choose \"Use Base Lens\" in the browser to go back.");
        ui.label(P(kSlots[i], "Info"), n, "Lens this trait comes from");
        ui.hidden(P(kSlots[i], "Id"), "");
    }

    // Advanced
    ui.beginGroup("grpAdvanced", "Advanced", false);
    ui.dbl("focal", "Focal Length (zooms)", "For zoom lenses: the focal length to sample. 0 uses the widest calibration.", 0, 0, 2000, 0, 300);
    ui.dbl("sensorWidth", "Sensor Width (mm)", "Width of the sensor area this plate was shot on. 0 assumes it matches each lens profile's own format.", 0, 0, 80, 0, 60);
    {
        Double2DParamDescriptor* c = p_Desc.defineDouble2DParam("center");
        c->setLabels("Optical Centre", "Optical Centre", "Optical Centre");
        c->setHint("Offset of the optical axis from frame centre, -1..1 per axis");
        c->setDefault(0, 0);
        c->setRange(-1, -1, 1, 1);
        c->setDisplayRange(-1, -1, 1, 1);
        ui.place(c);
    }
    ui.boolean("autoFit", "Auto Zoom For Distortion", "Scale up slightly so barrel distortion never pulls in edges from outside the frame", true);
    ui.boolean("unbounded", "Unbounded Overdrive", "Removes the overdrive safety limits. Warps can fold over and colour channels can separate into ghosts.", false);
    ui.dbl("hlBoost", "Highlight Boost", "Makes bright points dominate the blur so they bloom into bokeh balls", 3, 0, 40, 0, 20);
    ui.dbl("hlThresh", "Highlight Threshold", "Brightness (linear) where highlight boost begins", 0.7, 0, 10, 0, 2);
    ui.integer("samples", "Bokeh Quality", "Samples per pixel. Raise for large defocus, lower for faster playback.", 96, 8, 512);

    // Custom Lens
    ui.beginGroup("grpCustom", "Custom Lens", false);
    ui.button("customCopy", "Start From Current Lens", "Copies the current lens into these sliders and switches to the Custom Lens");
    ui.button("customUse", "Use Custom Lens", "Makes these sliders the main lens");
    ui.text("customName", "Name", "Name for saving this lens to My Lenses");
    ui.dbl("cK1", "Distortion k1", "Negative = barrel, positive = pincushion", 0, -1, 1, -0.2, 0.2);
    ui.dbl("cK2", "Distortion k2", "Opposite sign to k1 gives moustache distortion", 0, -1, 1, -0.05, 0.05);
    ui.dbl("cCaRed", "CA Red (‰)", "Red size relative to green, per mille. Positive gives red/cyan fringes.", 0.3, -20, 20, -3, 3);
    ui.dbl("cCaBlue", "CA Blue (‰)", "Blue size relative to green, per mille. Negative gives blue/yellow fringes.", -0.3, -20, 20, -3, 3);
    ui.dbl("cVigStops", "Vignetting (stops)", "How many stops darker the corners are", 1, 0, 6, 0, 4);
    ui.dbl("cVigShape", "Vignetting Shape", "1 = gradual, higher = flat centre with steep corners", 1.5, 0.5, 4, 0.5, 4);
    ui.integer("cBlades", "Aperture Blades", "0 = perfectly round iris", 0, 0, 18);
    ui.dbl("cCurve", "Blade Curvature", "0 = straight blades (polygon), 1 = round", 1, 0, 1, 0, 1);
    ui.dbl("cRotation", "Aperture Rotation", "Degrees", 0, -180, 180, -180, 180);
    ui.dbl("cCatEye", "Cat Eye", "Bokeh is clipped into cat-eye shapes toward the edges", 0.4, 0, 1.5, 0, 1.5);
    ui.dbl("cSwirl", "Swirl", "Bokeh stretches around the centre toward the edges", 0.3, 0, 3, 0, 2);
    ui.dbl("cField", "Edge Softness", "Edges fall out of focus faster than the centre", 0.3, 0, 3, 0, 2);
    ui.dbl("cRim", "Bokeh Edge", "-1 = soft discs, 0 = even, 1 = bright soap-bubble rims", 0, -1, 1, -1, 1);
    ui.dbl("cOnion", "Onion Rings", "Concentric rings inside bokeh", 0, 0, 1, 0, 1);
    ui.dbl("cLoca", "Bokeh Fringing", "Coloured rims on bokeh and contrasty edges", 0.3, -1, 1, -1, 1);
    ui.dbl("cGlow", "Glow", "Halo around highlights", 0.1, 0, 1, 0, 1);
    ui.dbl("cVeil", "Haze", "Low-contrast lift from scattered light", 0.02, 0, 0.5, 0, 0.5);
    ui.dbl("cSqueeze", "Anamorphic Squeeze", "Oval bokeh, 2 = classic 2x anamorphic", 1, 1, 2, 1, 2);
    ui.dbl("cTint", "Coating Tint", "Negative = cool, positive = warm", 0, -1, 1, -1, 1);
    ui.dbl("cMaxAperture", "Wide Open (f-stop)", "This lens's maximum aperture. Blur is defined wide open and shrinks as you stop down.", 2.0, 0.7, 22, 0.95, 8);
    ui.button("customSave", "Save To My Lenses", "Saves this lens under the name above so it appears in the lens browser");
    ui.button("reloadLibrary", "Reload Lens Library", "Re-reads lens files from disk");
    ui.label("customStatus", "", "Result of the last action");
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& p_FactoryArray)
{
    static LensCharacterFactory factory;
    p_FactoryArray.push_back(&factory);
}
