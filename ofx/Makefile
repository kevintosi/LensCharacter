# LensCharacter OpenFX plugin — macOS build (Apple Silicon), modelled on Resolve's GainPlugin sample.
#
#   make            build LensCharacter.ofx.bundle
#   make test       build and run the CPU + Metal self-test
#   make install    copy the bundle into /Library/OFX/Plugins (restart Resolve afterwards)

OFX_SDK ?= /Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/OpenFX
SDK      = build/sdk
OFX_INC  = $(SDK)/OpenFX-1.4/include
SUP_INC  = $(SDK)/Support/include
SUP_SRC  = $(SDK)/Support/Library

ARCH     = -arch arm64 -arch x86_64
CXXFLAGS = -std=c++17 -O2 -fvisibility=hidden $(ARCH) -I$(OFX_INC) -I$(SUP_INC) -Ibuild -Wno-deprecated-declarations
LDFLAGS  = -bundle -fvisibility=hidden $(ARCH) -framework Metal -framework Foundation -framework AppKit -framework Cocoa

BUNDLE   = build/LensCharacter.ofx.bundle
SUPPORT  = ofxsCore ofxsImageEffect ofxsInteract ofxsLog ofxsMultiThread ofxsParams ofxsProperty ofxsPropertyValidation
SUP_OBJS = $(SUPPORT:%=build/%.o)
OBJS     = build/LensCharacterPlugin.o build/Profiles.o build/Mixer.o build/MetalKernel.o build/LensBrowser.o $(SUP_OBJS)

all: $(BUNDLE)

# The SDK lives under a path with spaces, which make cannot handle; link it into build/.
build:
	mkdir -p build
	ln -sfn "$(OFX_SDK)" $(SDK)

# The Metal path compiles the same LensKernel.h source at runtime; embed it as a raw string.
build/LensKernel.metal.inc: src/LensKernel.h | build
	{ printf 'R"LCK('; cat $<; printf ')LCK"\n'; } > $@

build/MetalKernel.o: src/MetalKernel.mm build/LensKernel.metal.inc src/LensKernel.h | build
	$(CXX) -c $< -o $@ $(CXXFLAGS) -fno-objc-arc

build/LensBrowser.o: src/LensBrowser.mm src/*.h | build
	$(CXX) -c $< -o $@ $(CXXFLAGS) -fobjc-arc

build/%.o: src/%.cpp src/*.h | build
	$(CXX) -c $< -o $@ $(CXXFLAGS)

build/%.o: $(SUP_SRC)/%.cpp | build
	$(CXX) -c $< -o $@ $(CXXFLAGS)

$(BUNDLE): $(OBJS) resources/profiles/*.json resources/Info.plist
	$(CXX) $(OBJS) -o build/LensCharacter.ofx $(LDFLAGS)
	mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources/profiles
	cp build/LensCharacter.ofx $(BUNDLE)/Contents/MacOS/
	cp resources/Info.plist $(BUNDLE)/Contents/
	cp resources/profiles/*.json $(BUNDLE)/Contents/Resources/profiles/
	cp resources/ATTRIBUTION.txt $(BUNDLE)/Contents/Resources/

TEST_OBJS = build/Mixer.o build/Profiles.o build/MetalKernel.o build/LensBrowser.o

build/selftest: tests/selftest.cpp $(TEST_OBJS) src/*.h | build
	$(CXX) tests/selftest.cpp $(TEST_OBJS) -o $@ $(CXXFLAGS) -framework Metal -framework Foundation -framework Cocoa

test: build/selftest $(BUNDLE)
	LENSCHARACTER_PROFILES=$(BUNDLE)/Contents/Resources/profiles ./build/selftest build

# Deliberately not depending on $(BUNDLE): under sudo that would rebuild files as root.
install:
	@test -d $(BUNDLE) || { echo "Run make first (without sudo)"; exit 1; }
	rm -rf /Library/OFX/Plugins/LensCharacter.ofx.bundle
	cp -R $(BUNDLE) /Library/OFX/Plugins/

clean:
	rm -rf build

# Double-clickable installer for sharing: installs into /Library/OFX/Plugins, no Terminal needed.
VERSION = 0.4
PKG     = ../dist/LensCharacter-$(VERSION)-mac-universal.pkg

pkg: $(BUNDLE)
	rm -rf build/pkgroot && mkdir -p build/pkgroot ../dist
	cp -R $(BUNDLE) build/pkgroot/
	pkgbuild --analyze --root build/pkgroot build/component.plist
	plutil -replace 0.BundleIsRelocatable -bool NO build/component.plist
	pkgbuild --root build/pkgroot --component-plist build/component.plist \
	         --identifier com.ktosi.LensCharacter --version $(VERSION) \
	         --install-location /Library/OFX/Plugins build/LensCharacter-component.pkg
	productbuild --package build/LensCharacter-component.pkg --identifier com.ktosi.LensCharacter.installer \
	             --version $(VERSION) $(PKG)

.PHONY: all test install clean pkg
