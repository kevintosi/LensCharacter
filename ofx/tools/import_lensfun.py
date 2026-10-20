#!/usr/bin/env python3
"""Convert the Lensfun XML database into LensCharacter profile JSON.

Usage:  python3 -I import_lensfun.py <lensfun/data/db> <out.json>

Every lens becomes one profile with measured traits in a common form:
  distortion  rho_d = rho_u * (a0 + a1 r + a2 r^2 + a3 r^3 + a4 r^4)   Hugin units
  tca         same polynomial, per channel (red, blue) relative to green
  vignetting  V = 1 + k1 r^2 + k2 r^4 + k3 r^6                         r=1 at half diagonal
Hugin units: r = 1 at half the short side of the calibration sensor.
The plugin rescales these to the frame using the stored calibration geometry.

Lensfun database: CC BY-SA 3.0, https://github.com/lensfun/lensfun
"""
import json
import math
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

FF_DIAG = math.hypot(36.0, 24.0)

# Group spelling variants under one maker in the lens browser
MAKER_ALIASES = {
    'carl zeiss': 'Zeiss', 'zeiss': 'Zeiss',
    'leica camera ag': 'Leica', 'leica': 'Leica',
    'schneider': 'Schneider-Kreuznach', 'schneider-kreuznach': 'Schneider-Kreuznach',
    'om digital solutions': 'OM System', 'om system': 'OM System',
    'kmz': 'KMZ (Zenit, Helios)',
    'nikon corporation': 'Nikon',
    'ricoh imaging company': 'Ricoh', 'ricoh imaging company, ltd.': 'Ricoh',
}


def browser_maker(maker):
    return MAKER_ALIASES.get(maker.lower(), maker) if maker else 'Other'


def text(el, tag):
    # Prefer the untranslated element (no lang attribute)
    for c in el.findall(tag):
        if 'lang' not in c.attrib:
            return (c.text or '').strip()
    c = el.find(tag)
    return (c.text or '').strip() if c is not None else ''


APERTURE_PATTERNS = [
    re.compile(r'(?:^|[^A-Za-z]|mm)[fF]\s*/?\s*(\d+(?:\.\d+)?)'),   # f/1.4, F1.4, 35mmF1.4
    re.compile(r'1\s*:\s*(\d+(?:\.\d+)?)'),                          # 1:2, 1:1.4
    re.compile(r'(?:^|\s)(\d+(?:\.\d+)?)/\d+'),                      # Zeiss style 2/35
]


def max_aperture(model, vig_apertures):
    """Wide-open f-number from the model name, else the widest calibrated aperture."""
    for pat in APERTURE_PATTERNS:
        m = pat.search(model)
        if m:
            v = float(m.group(1))
            if 0.7 <= v <= 32:
                return v
    return min(vig_apertures) if vig_apertures else 0.0


def f(attr, key, default=0.0):
    try:
        return float(attr.get(key, default))
    except ValueError:
        return default


def dist_poly(d):
    m = d.get('model')
    if m == 'poly3':
        k1 = f(d, 'k1')
        return [1.0 - k1, 0.0, k1, 0.0, 0.0]
    if m == 'poly5':
        return [1.0, 0.0, f(d, 'k1'), 0.0, f(d, 'k2')]
    if m == 'ptlens':
        a, b, c = f(d, 'a'), f(d, 'b'), f(d, 'c')
        return [1.0 - a - b - c, c, b, a, 0.0]
    return None


def tca_poly(t):
    m = t.get('model')
    if m == 'linear':
        return [f(t, 'kr', 1.0), 0, 0, 0, 0], [f(t, 'kb', 1.0), 0, 0, 0, 0]
    if m == 'poly3':
        r = [f(t, 'vr', 1.0), f(t, 'cr'), f(t, 'br'), 0.0, 0.0]
        b = [f(t, 'vb', 1.0), f(t, 'cb'), f(t, 'bb'), 0.0, 0.0]
        return r, b
    return None


def convert_lens(lens):
    maker = text(lens, 'maker')
    model = text(lens, 'model')
    if not model:
        return None
    lens_type = text(lens, 'type') or 'rectilinear'
    if lens_type != 'rectilinear':
        return None  # fisheye projections need a different model
    try:
        crop = float(text(lens, 'cropfactor') or 1.0)
    except ValueError:
        crop = 1.0
    aspect_txt = text(lens, 'aspect-ratio')
    aspect = 1.5
    if aspect_txt:
        try:
            if ':' in aspect_txt:
                a, b = aspect_txt.split(':')
                aspect = float(a) / float(b)
            else:
                aspect = float(aspect_txt)
        except ValueError:
            pass
    aspect = max(aspect, 1.0 / aspect)

    half_diag_mm = FF_DIAG / crop / 2.0
    half_short_mm = FF_DIAG / crop / math.hypot(aspect, 1.0) / 2.0

    dist, tca, vig = [], [], {}
    for cal in lens.findall('calibration'):
        for d in cal.findall('distortion'):
            p = dist_poly(d)
            if p:
                dist.append({'focal': f(d, 'focal'), 'a': [round(x, 7) for x in p]})
        for t in cal.findall('tca'):
            p = tca_poly(t)
            if p:
                tca.append({'focal': f(t, 'focal'),
                            'r': [round(x, 8) for x in p[0]],
                            'b': [round(x, 8) for x in p[1]]})
        for v in cal.findall('vignetting'):
            if v.get('model') != 'pa':
                continue
            key = (f(v, 'focal'), f(v, 'aperture'))
            dist_m = f(v, 'distance', 1000.0)
            # keep the farthest focus distance per (focal, aperture)
            if key not in vig or dist_m > vig[key][0]:
                vig[key] = (dist_m, [f(v, 'k1'), f(v, 'k2'), f(v, 'k3')])

    if not (dist or tca or vig):
        return None

    name = f'{maker} {model}'.strip() if maker and not model.lower().startswith(maker.lower()) else model
    return {
        'id': 'lensfun:' + name,
        'name': name,
        'source': 'lensfun',
        'maker': browser_maker(maker),
        'mount': text(lens, 'mount'),
        'max_aperture': max_aperture(model, [k[1] for k in vig if k[1] > 0]),
        'calib': {'crop': crop, 'aspect': round(aspect, 5),
                  'half_diag_mm': round(half_diag_mm, 5), 'half_short_mm': round(half_short_mm, 5)},
        'distortion': sorted(dist, key=lambda e: e['focal']),
        'tca': sorted(tca, key=lambda e: e['focal']),
        'vignetting': [{'focal': k[0], 'aperture': k[1], 'k': [round(x, 6) for x in val[1]]}
                       for k, val in sorted(vig.items())],
    }


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    db_dir, out_path = Path(sys.argv[1]), Path(sys.argv[2])
    profiles, seen = [], set()
    for xml_file in sorted(db_dir.glob('*.xml')):
        root = ET.parse(xml_file).getroot()
        for lens in root.findall('lens'):
            p = convert_lens(lens)
            if not p:
                continue
            # the same model can appear once per calibration crop factor; keep them apart
            pid = p['id'] if p['id'] not in seen else f"{p['id']} (crop {p['calib']['crop']})"
            p['id'] = pid
            if pid != p['id'].split(' (crop')[0]:
                p['name'] = f"{p['name']} (crop {p['calib']['crop']})"
            seen.add(pid)
            profiles.append(p)
    out = {'format': 'lenscharacter-profiles', 'version': 1,
           'attribution': 'Lens data from the Lensfun database, CC BY-SA 3.0, https://github.com/lensfun/lensfun',
           'profiles': profiles}
    out_path.write_text(json.dumps(out, separators=(',', ':'), ensure_ascii=False))
    print(f'{len(profiles)} profiles -> {out_path} ({out_path.stat().st_size // 1024} KB)')


if __name__ == '__main__':
    main()
