#!/usr/bin/env python3
"""Generate the built-in character profiles (ported from LensCharacter.dctl presets).

Usage:  python3 -I make_builtin_profiles.py <out.json>

Character traits (bokeh shape, cat-eye, swirl, glow...) are hand-built.
Where Lensfun has the real lens, `measured_from` points at it and the plugin
takes distortion, CA and vignetting from the measurement instead of the estimates here.
"""
import json
import math
import sys

import numpy as np

FF_HALF_DIAG = math.hypot(36.0, 24.0) / 2.0
FF_HALF_SHORT = 12.0
S_HUGIN = FF_HALF_DIAG / FF_HALF_SHORT  # rho (half-diagonal) -> Hugin units

# name, measured_from, blades curve rot catEye swirl field rim onion lca loca k1 k2 vig glow veil squeeze tint
PRESETS = [
    ('Helios 44-2 58mm (character)', 'KMZ Helios-44 58mm 1:2',
     8, 0.9, 10, 0.75, 0.9, 0.5, 0.25, 0.0, 0.3, 0.4, 0.01, 0.0, 0.4, 0.2, 0.05, 1.0, 0.05),
    ('Petzval 85mm (character)', None,
     0, 1.0, 0, 0.9, 1.5, 1.2, 0.3, 0.0, 0.4, 0.3, 0.0, 0.0, 0.65, 0.25, 0.05, 1.0, 0.1),
    ('Trioplan 100mm Soap Bubble (character)', None,
     0, 1.0, 0, 0.35, 0.2, 0.3, 0.95, 0.1, 0.2, 0.5, 0.0, 0.0, 0.3, 0.3, 0.04, 1.0, 0.05),
    ('Super Takumar 50mm f1.4 (character)', 'Pentax Super-Takumar 50mm f/1.4',
     6, 0.6, 15, 0.45, 0.2, 0.25, 0.1, 0.0, 0.2, 0.6, 0.01, 0.0, 0.4, 0.35, 0.08, 1.0, 0.35),
    ('Cooke Speed Panchro (character)', None,
     0, 1.0, 0, 0.4, 0.1, 0.35, -0.25, 0.0, 0.2, 0.25, 0.0, 0.0, 0.4, 0.3, 0.04, 1.0, 0.12),
    ('Zeiss Super Speed (character)', None,
     7, 0.15, 90, 0.3, 0.05, 0.2, 0.1, 0.0, 0.15, 0.3, 0.005, 0.0, 0.3, 0.15, 0.03, 1.0, 0.0),
    ('Modern Clean Prime (character)', '85mm F1.4 DG DN Art 020',
     11, 0.95, 0, 0.25, 0.0, 0.05, 0.05, 0.18, 0.03, 0.08, 0.015, 0.0, 0.25, 0.0, 0.0, 1.0, 0.0),
    ('Noctilux 50mm f0.95 (character)', None,
     0, 1.0, 0, 0.8, 0.35, 0.45, -0.3, 0.0, 0.2, 0.7, 0.01, 0.0, 0.75, 0.4, 0.05, 1.0, 0.03),
    ('Anamorphic 2x (character)', None,
     11, 0.9, 0, 0.35, 0.15, 0.4, 0.2, 0.0, 0.3, 0.3, 0.08, 0.02, 0.5, 0.2, 0.04, 2.0, 0.0),
    ('Apodization STF (character)', None,
     0, 1.0, 0, 0.2, 0.0, 0.05, -1.0, 0.0, 0.03, 0.05, 0.0, 0.0, 0.2, 0.0, 0.0, 1.0, 0.0),
    ('Vintage Wide 24mm (character)', None,
     6, 0.5, 0, 0.5, 0.3, 0.8, 0.15, 0.0, 0.7, 0.3, 0.12, 0.05, 0.7, 0.15, 0.06, 1.0, 0.08),
]


def fit_distortion(k1, k2):
    """DCTL inverse map rho_u = rho_d (1 + k1 rho_d^2 + k2 rho_d^4) -> forward Hugin poly."""
    rd = np.linspace(0.01, 1.05, 200)
    ru = rd * (1 + k1 * rd ** 2 + k2 * rd ** 4)
    u, d = ru * S_HUGIN, rd * S_HUGIN
    A = np.stack([np.ones_like(u), u ** 2, u ** 4], axis=1)
    c, *_ = np.linalg.lstsq(A, d / u, rcond=None)
    return [float(c[0]), 0.0, float(c[1]), 0.0, float(c[2])]


def fit_vignette(vig):
    """DCTL V = 1/(1+t^2)^2, t = 0.9 vig r  ->  pa model 1 + k1 r^2 + k2 r^4 + k3 r^6."""
    r = np.linspace(0, 1.0, 200)
    t = 0.9 * vig * r
    v = 1.0 / (1 + t * t) ** 2
    A = np.stack([r ** 2, r ** 4, r ** 6], axis=1)
    k, *_ = np.linalg.lstsq(A, v - 1.0, rcond=None)
    return [float(x) for x in k]


MAX_APERTURE = {
    'Helios': 2.0, 'Petzval': 2.2, 'Trioplan': 2.8, 'Takumar': 1.4, 'Cooke': 2.0, 'Zeiss Super Speed': 1.3,
    'Modern Clean': 1.4, 'Noctilux': 0.95, 'Anamorphic': 2.8, 'Apodization': 2.8, 'Vintage Wide': 2.8,
}


def main():
    out_path = sys.argv[1]
    profiles = []
    for row in PRESETS:
        name, measured = row[0], row[1]
        (blades, curve, rot, cat, swirl, field, rim, onion, lca, loca,
         k1, k2, vig, glow, veil, squeeze, tint) = row[2:]
        p = {
            'id': 'builtin:' + name,
            'name': name.replace(' (character)', ''),
            'source': 'builtin',
            'maker': 'Character Presets',
            'max_aperture': next(v for k, v in MAX_APERTURE.items() if k in name),
            'calib': {'crop': 1.0, 'aspect': 1.5, 'half_diag_mm': FF_HALF_DIAG, 'half_short_mm': FF_HALF_SHORT},
            'character': {
                'blades': blades, 'curve': curve, 'rotation': rot, 'cat_eye': cat, 'swirl': swirl,
                'field_curvature': field, 'rim': rim, 'onion': onion, 'bokeh_fringe': loca,
                'glow': glow, 'veil': veil, 'squeeze': squeeze, 'tint': tint,
            },
        }
        if measured:
            p['measured_from'] = measured
        else:
            if k1 or k2:
                p['distortion'] = [{'focal': 0, 'a': fit_distortion(k1, k2)}]
            if lca:
                p['tca'] = [{'focal': 0, 'r': [1.0 / (1 + lca * 0.004), 0, 0, 0, 0],
                             'b': [1.0 / (1 - lca * 0.004), 0, 0, 0, 0]}]
            if vig:
                p['vignetting'] = [{'focal': 0, 'aperture': 0, 'k': fit_vignette(vig)}]
        profiles.append(p)

    json.dump({'format': 'lenscharacter-profiles', 'version': 1, 'profiles': profiles},
              open(out_path, 'w'), indent=1)
    print(f'{len(profiles)} built-in profiles -> {out_path}')


if __name__ == '__main__':
    main()
