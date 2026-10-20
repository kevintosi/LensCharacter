// Profiles.cpp — lens profile library.
#include "Profiles.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "../third_party/json.hpp"

using json = nlohmann::json;

namespace lc {

// ---- paths ------------------------------------------------------------------------------

static std::string bundleResourcesDir()
{
    // .../LensCharacter.ofx.bundle/Contents/MacOS/LensCharacter.ofx -> .../Contents/Resources
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&bundleResourcesDir), &info) && info.dli_fname)
    {
        std::string p = info.dli_fname;
        size_t slash = p.rfind('/');
        if (slash != std::string::npos)
        {
            p = p.substr(0, slash);                 // .../Contents/MacOS
            slash = p.rfind('/');
            if (slash != std::string::npos)
                return p.substr(0, slash) + "/Resources";
        }
    }
    return std::string();
}

std::string Library::userDir()
{
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/Library/Application Support/LensCharacter/Profiles";
}

static std::vector<std::string> listJson(const std::string& dir)
{
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (n.size() > 5 && n.substr(n.size() - 5) == ".json")
                out.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- search ------------------------------------------------------------------------------

std::string normalizeForSearch(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool space = true;
    for (unsigned char c : s)
    {
        if (std::isalnum(c))
        {
            out.push_back(static_cast<char>(std::tolower(c)));
            space = false;
        }
        else if (!space)
        {
            out.push_back(' ');
            space = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static std::vector<std::string> tokens(const std::string& normalized)
{
    std::vector<std::string> t;
    std::istringstream ss(normalized);
    std::string w;
    while (ss >> w) t.push_back(w);
    return t;
}

// ---- loading -----------------------------------------------------------------------------

static void readArray(const json& j, float* out, int n, float fallback)
{
    for (int i = 0; i < n; i++)
        out[i] = (j.is_array() && i < (int)j.size() && j[i].is_number()) ? j[i].get<float>() : fallback;
}

void Library::loadFile(const std::string& path, std::vector<std::shared_ptr<Profile>>& into)
{
    std::ifstream f(path);
    if (!f) return;
    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded()) return;

    // Accept a library file ({"profiles": [...]}) or a single profile object
    json list = doc.contains("profiles") ? doc["profiles"] : json::array({doc});
    for (const json& j : list)
    {
        if (!j.is_object() || !j.contains("name")) continue;
        auto p = std::make_shared<Profile>();
        p->name = j.value("name", "");
        p->id = j.value("id", "user:" + p->name);
        p->source = j.value("source", "user");
        p->measuredFrom = j.value("measured_from", "");
        p->maker = j.value("maker", p->source == "user" ? "My Lenses" : "Other");
        p->mount = j.value("mount", "");
        p->maxAperture = j.value("max_aperture", 0.0f);
        if (j.contains("calib"))
        {
            p->calib.halfDiagMm = j["calib"].value("half_diag_mm", 21.6333f);
            p->calib.halfShortMm = j["calib"].value("half_short_mm", 12.0f);
        }
        if (j.contains("distortion"))
            for (const json& e : j["distortion"])
            {
                DistEntry d{};
                d.focal = e.value("focal", 0.0f);
                readArray(e["a"], d.a, 5, 0.0f);
                p->dist.push_back(d);
            }
        if (j.contains("tca"))
            for (const json& e : j["tca"])
            {
                TcaEntry t{};
                t.focal = e.value("focal", 0.0f);
                readArray(e["r"], t.r, 5, 0.0f);
                readArray(e["b"], t.b, 5, 0.0f);
                p->tca.push_back(t);
            }
        if (j.contains("vignetting"))
            for (const json& e : j["vignetting"])
            {
                VigEntry v{};
                v.focal = e.value("focal", 0.0f);
                v.aperture = e.value("aperture", 0.0f);
                readArray(e["k"], v.k, 3, 0.0f);
                p->vig.push_back(v);
            }
        if (j.contains("character"))
        {
            const json& c = j["character"];
            Character& ch = p->character;
            ch.blades = c.value("blades", 0.0f);
            ch.curve = c.value("curve", 1.0f);
            ch.rotation = c.value("rotation", 0.0f);
            ch.catEye = c.value("cat_eye", 0.0f);
            ch.swirl = c.value("swirl", 0.0f);
            ch.field = c.value("field_curvature", 0.0f);
            ch.rim = c.value("rim", 0.0f);
            ch.onion = c.value("onion", 0.0f);
            ch.loca = c.value("bokeh_fringe", 0.0f);
            ch.glow = c.value("glow", 0.0f);
            ch.veil = c.value("veil", 0.0f);
            ch.squeeze = c.value("squeeze", 1.0f);
            ch.tint = c.value("tint", 0.0f);
            p->hasCharacter = true;
        }
        into.push_back(p);
    }
}

Library::Library()
{
    reload();
}

Library& Library::get()
{
    static Library lib;
    return lib;
}

void Library::reload()
{
    std::vector<std::shared_ptr<Profile>> loaded;
    const char* overrideDir = std::getenv("LENSCHARACTER_PROFILES");   // used by the self-test
    std::string builtinDir = overrideDir ? std::string(overrideDir) : bundleResourcesDir() + "/profiles";
    for (const std::string& path : listJson(builtinDir))
        loadFile(path, loaded);
    for (const std::string& path : listJson(userDir()))
        loadFile(path, loaded);

    // User and built-in profiles first, so they win search ties against imported data
    std::stable_sort(loaded.begin(), loaded.end(), [](const std::shared_ptr<Profile>& a, const std::shared_ptr<Profile>& b) {
        auto rank = [](const std::string& s) { return s == "user" ? 0 : s == "builtin" ? 1 : 2; };
        return rank(a->source) < rank(b->source);
    });

    std::vector<ProfilePtr> profiles(loaded.begin(), loaded.end());
    std::vector<std::string> keys;
    for (const ProfilePtr& p : profiles) keys.push_back(normalizeForSearch(p->name));

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Profiles = profiles;
        m_Normalized = keys;
    }

    // Resolve `measured_from` links once everything is indexed
    for (auto& p : loaded)
        if (!p->measuredFrom.empty())
        {
            ProfilePtr m = find(p->measuredFrom);
            if (m && m.get() != p.get()) p->measured = m;
        }
}

ProfilePtr Library::find(const std::string& query) const
{
    std::string q = normalizeForSearch(query);
    if (q.empty()) return nullptr;
    std::vector<std::string> qt = tokens(q);

    std::lock_guard<std::mutex> lock(m_Mutex);
    int best = -1;
    long bestScore = -1;
    for (size_t i = 0; i < m_Profiles.size(); i++)
    {
        const std::string& key = m_Normalized[i];
        if (key == q) return m_Profiles[i];
        std::vector<std::string> kt = tokens(key);
        long score = 0;
        bool all = true;
        for (const std::string& t : qt)
        {
            if (key.find(t) == std::string::npos) { all = false; break; }
            score += std::find(kt.begin(), kt.end(), t) != kt.end() ? 100 : 40;   // whole-word hits beat substrings
        }
        if (!all) continue;
        if (key.find(q) != std::string::npos) score += 200;                       // phrase in order
        score -= (long)key.size();                                                 // prefer the most specific name
        score -= (long)i / 1000;                                                   // then library order
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }
    return best >= 0 ? m_Profiles[best] : nullptr;
}

ProfilePtr Library::findById(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const ProfilePtr& p : m_Profiles)
        if (p->id == id) return p;
    return nullptr;
}

std::vector<ProfilePtr> Library::all() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Profiles;
}

size_t Library::size() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Profiles.size();
}

// ---- saving --------------------------------------------------------------------------------

static void mkdirs(const std::string& path)
{
    std::string cur;
    std::istringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/'))
    {
        if (part.empty()) { cur = "/"; continue; }
        cur += (cur.empty() || cur.back() == '/' ? "" : "/") + part;
        mkdir(cur.c_str(), 0755);
    }
}

bool Library::saveUserProfile(const Profile& p, std::string& outPath, std::string& outError)
{
    std::string fname = normalizeForSearch(p.name);
    std::replace(fname.begin(), fname.end(), ' ', '-');
    if (fname.empty()) { outError = "Give the custom lens a name before saving."; return false; }

    json j;
    j["id"] = "user:" + p.name;
    j["name"] = p.name;
    j["source"] = "user";
    j["maker"] = "My Lenses";
    if (p.maxAperture > 0) j["max_aperture"] = p.maxAperture;
    j["calib"] = {{"half_diag_mm", p.calib.halfDiagMm}, {"half_short_mm", p.calib.halfShortMm}};
    if (!p.measuredFrom.empty()) j["measured_from"] = p.measuredFrom;
    for (const DistEntry& d : p.dist)
        j["distortion"].push_back({{"focal", d.focal}, {"a", std::vector<float>(d.a, d.a + 5)}});
    for (const TcaEntry& t : p.tca)
        j["tca"].push_back({{"focal", t.focal}, {"r", std::vector<float>(t.r, t.r + 5)}, {"b", std::vector<float>(t.b, t.b + 5)}});
    for (const VigEntry& v : p.vig)
        j["vignetting"].push_back({{"focal", v.focal}, {"aperture", v.aperture}, {"k", std::vector<float>(v.k, v.k + 3)}});
    const Character& c = p.character;
    j["character"] = {{"blades", c.blades}, {"curve", c.curve}, {"rotation", c.rotation}, {"cat_eye", c.catEye},
                      {"swirl", c.swirl}, {"field_curvature", c.field}, {"rim", c.rim}, {"onion", c.onion},
                      {"bokeh_fringe", c.loca}, {"glow", c.glow}, {"veil", c.veil}, {"squeeze", c.squeeze}, {"tint", c.tint}};

    std::string dir = userDir();
    mkdirs(dir);
    outPath = dir + "/" + fname + ".json";
    std::ofstream f(outPath);
    if (!f) { outError = "Could not write " + outPath; return false; }
    f << j.dump(1);
    f.close();
    reload();
    return true;
}

// ---- trait sampling ---------------------------------------------------------------------------

template <typename E>
static void bracket(const std::vector<E>& v, float focal, int& i0, int& i1, float& t)
{
    i0 = i1 = 0; t = 0.0f;
    if (v.size() <= 1 || focal <= 0.0f) return;
    if (focal <= v.front().focal) return;
    if (focal >= v.back().focal) { i0 = i1 = (int)v.size() - 1; return; }
    for (size_t i = 0; i + 1 < v.size(); i++)
        if (focal >= v[i].focal && focal <= v[i + 1].focal)
        {
            i0 = (int)i; i1 = (int)i + 1;
            float span = v[i + 1].focal - v[i].focal;
            t = span > 0 ? (focal - v[i].focal) / span : 0.0f;
            return;
        }
}

bool sampleDistortion(const Profile& p, float focal, float out[5])
{
    if (p.dist.empty()) return false;
    int i0, i1; float t;
    bracket(p.dist, focal, i0, i1, t);
    for (int k = 0; k < 5; k++) out[k] = p.dist[i0].a[k] + (p.dist[i1].a[k] - p.dist[i0].a[k]) * t;
    return true;
}

bool sampleTca(const Profile& p, float focal, float outR[5], float outB[5])
{
    if (p.tca.empty()) return false;
    int i0, i1; float t;
    bracket(p.tca, focal, i0, i1, t);
    for (int k = 0; k < 5; k++)
    {
        outR[k] = p.tca[i0].r[k] + (p.tca[i1].r[k] - p.tca[i0].r[k]) * t;
        outB[k] = p.tca[i0].b[k] + (p.tca[i1].b[k] - p.tca[i0].b[k]) * t;
    }
    return true;
}

// Vignetting depends on focal length and aperture: pick the nearest calibrated focal length,
// then interpolate between the bracketing apertures in stops.
bool sampleVignetting(const Profile& p, float focal, float aperture, float out[3])
{
    if (p.vig.empty()) return false;
    float bestFocal = p.vig.front().focal;
    if (focal > 0)
        for (const VigEntry& v : p.vig)
            if (std::fabs(v.focal - focal) < std::fabs(bestFocal - focal)) bestFocal = v.focal;

    std::vector<const VigEntry*> atFocal;
    for (const VigEntry& v : p.vig)
        if (v.focal == bestFocal) atFocal.push_back(&v);
    std::sort(atFocal.begin(), atFocal.end(), [](const VigEntry* a, const VigEntry* b) { return a->aperture < b->aperture; });

    const VigEntry* lo = atFocal.front();
    const VigEntry* hi = atFocal.front();
    if (aperture <= 0 || aperture <= lo->aperture) hi = lo;
    else if (aperture >= atFocal.back()->aperture) lo = hi = atFocal.back();
    else
        for (size_t i = 0; i + 1 < atFocal.size(); i++)
            if (aperture >= atFocal[i]->aperture && aperture <= atFocal[i + 1]->aperture) { lo = atFocal[i]; hi = atFocal[i + 1]; break; }

    float t = 0.0f;
    if (hi != lo && lo->aperture > 0 && hi->aperture > lo->aperture)
        t = std::log2(aperture / lo->aperture) / std::log2(hi->aperture / lo->aperture);
    for (int k = 0; k < 3; k++) out[k] = lo->k[k] + (hi->k[k] - lo->k[k]) * t;
    return true;
}

std::string Profile::summary() const
{
    std::string src = source == "lensfun" ? "Lensfun measured" : source == "builtin" ? "built-in" : source == "custom" ? "custom" : "your profile";
    std::string traits;
    if (distSource()) traits += " Distortion";
    if (tcaSource()) traits += " CA";
    if (vigSource()) traits += " Vignetting";
    if (hasCharacter) traits += " Character";
    std::string s = name + " · " + src + " ·" + (traits.empty() ? " no traits" : traits);
    if (measured) s += " · geometry from " + measured->name;
    return s;
}

} // namespace lc
