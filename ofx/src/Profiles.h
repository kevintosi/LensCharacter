// Profiles.h — lens profile library: loading, search, trait sampling, saving.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lc {

struct DistEntry { float focal; float a[5]; };
struct TcaEntry  { float focal; float r[5]; float b[5]; };
struct VigEntry  { float focal; float aperture; float k[3]; };

// Hand-built traits that measurement databases don't carry. Defaults are the ideal lens.
struct Character
{
    float blades = 0, curve = 1, rotation = 0, catEye = 0, swirl = 0, field = 0;
    float rim = 0, onion = 0, loca = 0, glow = 0, veil = 0, squeeze = 1, tint = 0;
};

struct Calibration
{
    float halfDiagMm = 21.6333f;   // vignetting: r = 1 here
    float halfShortMm = 12.0f;     // distortion / CA (Hugin units): r = 1 here
};

struct Profile
{
    std::string id, name, source, measuredFrom, maker, mount;
    Calibration calib;
    float maxAperture = 0.0f;   // wide-open f-number, 0 if unknown
    std::vector<DistEntry> dist;
    std::vector<TcaEntry> tca;
    std::vector<VigEntry> vig;
    bool hasCharacter = false;
    Character character;
    std::shared_ptr<const Profile> measured;   // resolved `measured_from` lens

    // Trait lookups fall back to the measured lens when this profile has none of its own.
    const Profile* distSource() const { return !dist.empty() ? this : (measured && !measured->dist.empty() ? measured.get() : nullptr); }
    const Profile* tcaSource()  const { return !tca.empty()  ? this : (measured && !measured->tca.empty()  ? measured.get() : nullptr); }
    const Profile* vigSource()  const { return !vig.empty()  ? this : (measured && !measured->vig.empty()  ? measured.get() : nullptr); }

    std::string summary() const;   // e.g. "Helios 44-2 58mm (character) · built-in · Distortion CA ..."
};

using ProfilePtr = std::shared_ptr<const Profile>;

// Trait sampling, in the profile's own calibration units.
bool sampleDistortion(const Profile& p, float focal, float out[5]);
bool sampleTca(const Profile& p, float focal, float outR[5], float outB[5]);
bool sampleVignetting(const Profile& p, float focal, float aperture, float out[3]);

class Library
{
public:
    static Library& get();

    void reload();
    ProfilePtr find(const std::string& query) const;   // fuzzy name search
    ProfilePtr findById(const std::string& id) const;  // exact, as stored by the lens browser
    std::vector<ProfilePtr> all() const;
    size_t size() const;

    // ~/Library/Application Support/LensCharacter/Profiles
    static std::string userDir();
    bool saveUserProfile(const Profile& p, std::string& outPath, std::string& outError);

private:
    Library();
    void loadFile(const std::string& path, std::vector<std::shared_ptr<Profile>>& into);

    mutable std::mutex m_Mutex;
    std::vector<ProfilePtr> m_Profiles;
    std::vector<std::string> m_Normalized;   // search keys, parallel to m_Profiles
};

std::string normalizeForSearch(const std::string& s);

} // namespace lc
