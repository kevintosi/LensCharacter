# LensCharacter

A free lens-emulation plugin for DaVinci Resolve on macOS. Pick from 1,500+ measured lenses and character presets, then add their distortion, chromatic aberration, vignetting, bokeh, swirl and glow to your footage. You can take individual traits from different lenses and push them past what the real glass does.

- **Measured lenses**: real distortion, colour fringing and vignetting for 1,540 lenses from the [Lensfun](https://github.com/lensfun/lensfun) database
- **Character presets**: hand-built bokeh, cat-eye, swirl and glow for lenses like the Helios 44-2, Petzval, Trioplan and Super Takumar, linked to measured geometry where it exists
- **Lens browser**: search by name, maker or mount, with a preview of each lens's distortion and bokeh shape
- **Mix & Match**: take distortion from one lens and bokeh from another
- **Overdrive**: push any trait past the real lens without breaking the image
- **Real aperture behaviour**: stopping down shrinks the blur, fades cat-eye and swirl, and shows the iris blades
- **Custom lenses**: build your own and save it to your library
- **GPU accelerated** with Metal, plus Fast and Geometry Only modes for smooth playback

## Install

Requires a Mac (Apple Silicon or Intel) and DaVinci Resolve. Tested with Resolve Studio 21.1.

1. Download the latest `LensCharacter-…-mac-universal.pkg` from [Releases](../../releases).
2. Quit DaVinci Resolve.
3. Double-click the installer. macOS will say it can't verify the developer, because the installer isn't signed with an Apple Developer ID. Click **Done**.
4. Open **System Settings → Privacy & Security**, scroll down, click **Open Anyway** next to the LensCharacter message, and enter your password.
5. Click through the installer.
6. Open Resolve. The effect is under **Effects → OpenFX → LensCharacter** on the Edit and Color pages.

To check the download, compare its SHA-256 with the one in the release notes (`shasum -a 256 <file>` in Terminal).

To uninstall, open `/Library/OFX/Plugins` in Finder (⌘⇧G) and move `LensCharacter.ofx.bundle` to the Bin.

## Quick start

1. Add **LensCharacter** to a clip and set **Input Encoding** to match your timeline: *Gamma 2.4* for a standard Rec.709 project, *DaVinci Intermediate* if you're colour managed.
2. Click **Choose Lens…** and pick a lens. Choosing a lens sets **Aperture** wide open.
3. Use the **Look** sliders to scale each trait: 1 is the real lens, 0 turns it off, higher values overdrive it. Measured primes are subtle, so try 5–20 on Distortion and Chromatic Aberration to see them clearly.
4. Use **Focus** to set how much blur there is and where. Bokeh shape and swirl appear in the blurred areas and toward the corners.
5. For playback, set **Quality** to *Fast* or *Geometry Only*. Set it back to *Full* before you deliver.

The plugin has no depth information. Blur goes by position (sharp centre, soft edges) or by the clip's alpha channel.

## Build from source

Needs the Xcode Command Line Tools and DaVinci Resolve, which ships the OpenFX SDK.

```bash
cd ofx
make            # build/LensCharacter.ofx.bundle
make test       # CPU/GPU agreement, overdrive limits, lens browser snapshots
sudo make install
make pkg        # installer in dist/
```

See [`ofx/README.md`](ofx/README.md) for the code layout and how to regenerate the lens data, and [`docs/architecture.html`](docs/architecture.html) for the design. [`dctl/`](dctl/) holds the original single-file DCTL prototype.

## Credits and licences

- Code: MIT, see [LICENSE](LICENSE).
- Measured lens data: converted from the [Lensfun](https://github.com/lensfun/lensfun) database, CC BY-SA 3.0.
- [nlohmann/json](https://github.com/nlohmann/json), MIT.
- OpenFX Support library, BSD, The Open Effects Association.

Lens and brand names are used only to describe the lenses being emulated. LensCharacter is not affiliated with or endorsed by any lens manufacturer. The character presets are artistic approximations, not measurements.
