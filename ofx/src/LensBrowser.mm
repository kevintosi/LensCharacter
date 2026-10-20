// LensBrowser.mm — the "Choose Lens…" popup: makers sidebar, searchable lens list, preview.
// Compiled with ARC. Class names carry an LC prefix because every plugin shares Resolve's
// Objective-C runtime.

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "LensBrowser.h"
#include "LensKernel.h"

using namespace lc;

static NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

static NSString* const kAll = @"All Lenses";
static NSString* const kPresets = @"Character Presets";
static NSString* const kMine = @"My Lenses";
static const int kUHDHalfDiag = 2203;   // px, for "CA at UHD corner"

// ---- per-lens facts shown in the list and preview ---------------------------------------------

struct LensFacts
{
    std::string focal;     // "58mm", "24–70mm"
    std::string traits;    // "Dist · CA · Vig · Bokeh"
    float distPct = 0;     // radial displacement at the corner, %
    bool mustache = false;
    float caPx = 0;        // channel separation at a UHD corner
    bool hasVig = false;
    float vigStops = 0;    // at the requested aperture
};

static void rhoPoly(const Profile& src, const float a[5], float out[5])
{
    float s = src.calib.halfDiagMm / src.calib.halfShortMm, f = 1.0f;
    for (int k = 0; k < 5; k++) { out[k] = a[k] * f; f *= s; }
}

static LensFacts factsFor(const Profile& p, float aperture)
{
    LensFacts f;
    std::set<float> focals;
    for (const DistEntry& d : p.dist) if (d.focal > 0) focals.insert(d.focal);
    for (const TcaEntry& t : p.tca) if (t.focal > 0) focals.insert(t.focal);
    for (const VigEntry& v : p.vig) if (v.focal > 0) focals.insert(v.focal);
    if (p.measured)
    {
        for (const DistEntry& d : p.measured->dist) if (d.focal > 0) focals.insert(d.focal);
    }
    char buf[64];
    if (!focals.empty())
    {
        if (*focals.begin() == *focals.rbegin()) snprintf(buf, sizeof buf, "%gmm", *focals.begin());
        else snprintf(buf, sizeof buf, "%g–%gmm", *focals.begin(), *focals.rbegin());
        f.focal = buf;
    }
    if (p.maxAperture > 0)
    {
        snprintf(buf, sizeof buf, "%sf/%g", f.focal.empty() ? "" : "  ", p.maxAperture);
        f.focal += buf;
    }

    std::vector<std::string> t;
    if (const Profile* s = p.distSource())
    {
        t.push_back("Dist");
        float a[5], r[5];
        if (sampleDistortion(*s, 0, a))
        {
            rhoPoly(*s, a, r);
            f.distPct = (polyEval(r, 1.0f) - 1.0f) * 100.0f;
            float sign0 = 0;
            for (int i = 1; i <= 20; i++)
            {
                float v = polyEval(r, i / 20.0f) - 1.0f;
                if (std::fabs(v) < 1e-5f) continue;
                if (sign0 == 0) sign0 = v;
                else if (v * sign0 < 0) f.mustache = true;
            }
        }
    }
    if (const Profile* s = p.tcaSource())
    {
        t.push_back("CA");
        float rr[5], bb[5], r2[5], b2[5];
        if (sampleTca(*s, 0, rr, bb))
        {
            rhoPoly(*s, rr, r2);
            rhoPoly(*s, bb, b2);
            float sep = std::max(std::fabs(polyEval(r2, 1.0f) - 1.0f), std::fabs(polyEval(b2, 1.0f) - 1.0f));
            f.caPx = sep * kUHDHalfDiag;
        }
    }
    if (const Profile* s = p.vigSource())
    {
        t.push_back("Vig");
        float k[3];
        if (sampleVignetting(*s, 0, aperture, k))
        {
            float v = std::max(1.0f + k[0] + k[1] + k[2], 1e-3f);
            f.hasVig = true;
            f.vigStops = std::log2(v);
        }
    }
    if (p.hasCharacter) t.push_back("Bokeh");
    for (size_t i = 0; i < t.size(); i++) f.traits += (i ? " · " : "") + t[i];
    return f;
}

// ---- preview view -------------------------------------------------------------------------------

@interface LCLensPreview : NSView
@property (nonatomic) float aperture;
- (void)showProfile:(const ProfilePtr&)p;
@end

@implementation LCLensPreview
{
    ProfilePtr _profile;
}

- (BOOL)isFlipped { return YES; }

- (void)showProfile:(const ProfilePtr&)p
{
    _profile = p;
    [self setNeedsDisplay:YES];
}

static void drawText(NSString* s, NSRect r, CGFloat size, NSColor* c, BOOL bold)
{
    NSDictionary* a = @{NSFontAttributeName: bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size],
                        NSForegroundColorAttributeName: c};
    [s drawWithRect:r options:NSStringDrawingUsesLineFragmentOrigin attributes:a context:nil];
}

- (void)drawRect:(NSRect)dirty
{
    [[NSColor colorWithWhite:0.11 alpha:1] setFill];
    NSRectFill(self.bounds);
    const CGFloat W = self.bounds.size.width;
    NSColor* text = [NSColor colorWithWhite:0.92 alpha:1];
    NSColor* dim = [NSColor colorWithWhite:0.6 alpha:1];

    if (!_profile)
    {
        drawText(@"Select a lens to preview it.", NSMakeRect(16, 16, W - 32, 40), 13, dim, NO);
        return;
    }
    const Profile& p = *_profile;
    LensFacts f = factsFor(p, self.aperture);

    CGFloat y = 14;
    drawText(NS(p.name), NSMakeRect(16, y, W - 32, 40), 14, text, YES);
    y += 40;
    std::string sub = p.source == "lensfun" ? "Measured (Lensfun)" : p.source == "builtin" ? "Character preset" : p.source == "custom" ? "Custom lens (your sliders)" : "Saved in My Lenses";
    if (!p.mount.empty()) sub += " · " + p.mount;
    if (p.measured) sub += "\nGeometry measured from " + p.measured->name;
    drawText(NS(sub), NSMakeRect(16, y, W - 32, 34), 11, dim, NO);
    y += 40;

    // Distortion grid, exaggerated so subtle lenses are visible
    NSRect box = NSMakeRect(16, y, W - 32, (W - 32) * 9.0 / 16.0);
    [[NSColor colorWithWhite:0.16 alpha:1] setFill];
    NSRectFill(box);
    float poly[5] = {1, 0, 0, 0, 0};
    float exaggerate = 1.0f;
    if (const Profile* s = p.distSource())
    {
        float a[5];
        if (sampleDistortion(*s, 0, a))
        {
            rhoPoly(*s, a, poly);
            float maxd = 0;
            for (int i = 0; i <= 20; i++) maxd = std::max(maxd, std::fabs(polyEval(poly, i / 20.0f) - 1.0f));
            if (maxd > 1e-6f) exaggerate = std::min(60.0f, std::max(1.0f, 0.06f / maxd));
            for (int k = 0; k < 5; k++) poly[k] = (k == 0 ? 1.0f : 0.0f) + (poly[k] - (k == 0 ? 1.0f : 0.0f)) * exaggerate;
        }
    }
    const CGFloat cx = NSMidX(box), cy = NSMidY(box), hd = std::hypot(box.size.width, box.size.height) / 2.0;
    auto warp = [&](CGFloat gx, CGFloat gy) {
        CGFloat dx = gx - cx, dy = gy - cy, r = std::hypot(dx, dy) / hd;
        CGFloat k = polyEval(poly, (float)r);
        return NSMakePoint(cx + dx * k, cy + dy * k);
    };
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(box);
    NSBezierPath* grid = [NSBezierPath bezierPath];
    grid.lineWidth = 1.0;
    for (int i = 0; i <= 12; i++)
    {
        CGFloat gx = box.origin.x + box.size.width * i / 12.0;
        CGFloat gy = box.origin.y + box.size.height * i / 12.0;
        for (int j = 0; j <= 40; j++)
        {
            NSPoint a = warp(gx, box.origin.y + box.size.height * j / 40.0);
            j ? [grid lineToPoint:a] : [grid moveToPoint:a];
        }
        for (int j = 0; j <= 40; j++)
        {
            NSPoint a = warp(box.origin.x + box.size.width * j / 40.0, gy);
            j ? [grid lineToPoint:a] : [grid moveToPoint:a];
        }
    }
    [[NSColor colorWithRed:0.62 green:0.66 blue:0.78 alpha:0.9] setStroke];
    [grid stroke];
    [NSGraphicsContext restoreGraphicsState];
    if (exaggerate > 1.01f)
        drawText([NSString stringWithFormat:@"shown ×%.0f", exaggerate], NSMakeRect(box.origin.x + 6, NSMaxY(box) - 18, 120, 16), 10, dim, NO);
    y = NSMaxY(box) + 12;

    // Bokeh: the kernel's own sampling, drawn at the frame centre and at an edge
    if (p.hasCharacter)
    {
        const Character& c = p.character;
        CGFloat size = (W - 48) / 2.0;
        for (int which = 0; which < 2; which++)
        {
            NSRect cell = NSMakeRect(16 + which * (size + 16), y, size, size);
            [[NSColor colorWithWhite:0.05 alpha:1] setFill];
            NSRectFill(cell);
            float r = which == 0 ? 0.0f : 0.9f;
            float rdx = 0.7071f, rdy = -0.7071f;   // toward the top-right corner
            float tdx = -rdy, tdy = rdx;
            float seg = c.blades >= 3 ? 2.0f * LC_PI / c.blades : 0.0f;
            float rot = c.rotation * LC_PI / 180.0f;
            float ceX = rdx * c.catEye * r * 1.1f, ceY = rdy * c.catEye * r * 1.1f;
            float tStretch = 1.0f + c.swirl * r * r * 0.8f, rSquash = 1.0f / (1.0f + c.swirl * r * r * 0.3f);
            float rim = std::max(-1.0f, std::min(1.0f, c.rim));
            const int N = 1400;
            CGFloat scale = size * 0.30;
            for (int i = 0; i < N; i++)
            {
                float rn = std::sqrt((i + 0.5f) / N), th = i * LC_GOLDEN, shape = 1.0f;
                if (seg > 0)
                {
                    float a = std::fmod(th - rot + 64.0f * LC_PI, seg) - 0.5f * seg;
                    shape = (std::cos(LC_PI / c.blades) / std::cos(a)) * (1.0f - c.curve) + c.curve;
                }
                float ax = std::cos(th) * rn * shape, ay = std::sin(th) * rn * shape;
                float ddx = ax + ceX, ddy = ay + ceY;
                if (std::sqrt(ddx * ddx + ddy * ddy) / shape > 1.0f) continue;
                float orad = (ax * rdx + ay * rdy) * rSquash, otan = (ax * tdx + ay * tdy) * tStretch;
                float ox = orad * rdx + otan * tdx, oy = (orad * rdy + otan * tdy) * c.squeeze;
                float w = rim >= 0 ? 1.0f + rim * 4.0f * rn * rn * rn * rn : 1.0f + rim * rn * rn;
                w *= 1.0f + c.onion * 0.6f * std::cos(rn * 38.0f);
                float alpha = std::max(0.05f, std::min(1.0f, 0.22f * w));
                [[NSColor colorWithRed:1.0 green:0.86 blue:0.62 alpha:alpha] setFill];
                NSRectFillUsingOperation(NSMakeRect(NSMidX(cell) + ox * scale - 1, NSMidY(cell) + oy * scale - 1, 2.2, 2.2), NSCompositingOperationSourceOver);
            }
            drawText(which == 0 ? @"Bokeh, centre" : @"Bokeh, edge", NSMakeRect(cell.origin.x + 6, NSMaxY(cell) - 18, size, 16), 10, dim, NO);
        }
        y += size + 14;
    }

    // Numbers
    NSMutableString* facts = [NSMutableString string];
    if (p.distSource())
        [facts appendFormat:@"Distortion: %@ %.2f%% at the corner\n",
                            f.mustache ? @"moustache," : (f.distPct < 0 ? @"barrel," : @"pincushion,"), std::fabs(f.distPct)];
    else
        [facts appendString:@"Distortion: none in profile\n"];
    if (p.tcaSource()) [facts appendFormat:@"Lateral CA: %.1f px at a UHD corner\n", f.caPx];
    else [facts appendString:@"Lateral CA: none in profile\n"];
    if (f.hasVig) [facts appendFormat:@"Vignetting at f/%g: %.1f stops at the corner\n", self.aperture, f.vigStops];
    else [facts appendString:@"Vignetting: none in profile\n"];
    if (p.hasCharacter)
    {
        const Character& c = p.character;
        NSString* iris = c.blades >= 3 ? [NSString stringWithFormat:@"%.0f-blade iris", c.blades] : @"round iris";
        [facts appendFormat:@"Bokeh: %@, cat-eye %.2f, swirl %.2f, glow %.2f", iris, c.catEye, c.swirl, c.glow];
    }
    else
        [facts appendString:@"Bokeh: measured data only, so the iris is round. Pick a character preset in Mix & Match for bokeh character."];
    drawText(facts, NSMakeRect(16, y, W - 32, 140), 11.5, text, NO);
}
@end

// ---- controller ---------------------------------------------------------------------------------

@interface LCLensBrowserController : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate, NSWindowDelegate>
@property (strong) NSPanel* panel;
@property (strong) NSSearchField* search;
@property (strong) NSTableView* sidebar;
@property (strong) NSTableView* table;
@property (strong) LCLensPreview* preview;
@property (strong) NSButton* useButton;
@property (strong) NSTextField* countLabel;
@property (nonatomic) BOOL chose;
@property (nonatomic) BOOL modal;
- (instancetype)initWithRequest:(const BrowserRequest&)req;
- (std::string)selectedId;
- (void)selectCategory:(NSString*)cat search:(NSString*)text selectId:(const std::string&)pid;
@end

@implementation LCLensBrowserController
{
    std::vector<ProfilePtr> _all;
    std::vector<std::string> _keys;        // normalized search text per profile
    std::vector<LensFacts> _facts;
    std::vector<int> _rows;                // indices into _all for the current filter
    NSMutableArray<NSString*>* _categories;
    std::map<std::string, int> _counts;
    BOOL _allowNone;
    float _aperture;
}

- (instancetype)initWithRequest:(const BrowserRequest&)req
{
    if (!(self = [super init])) return nil;
    _allowNone = req.allowNone;
    _aperture = req.aperture;

    if (req.custom) _all.push_back(req.custom);
    for (const ProfilePtr& p : Library::get().all()) _all.push_back(p);
    for (const ProfilePtr& p : _all)
    {
        _keys.push_back(normalizeForSearch(p->name + " " + p->maker + " " + p->mount));
        _facts.push_back(factsFor(*p, _aperture));
        _counts[p->maker]++;
    }

    _categories = [NSMutableArray arrayWithObjects:kAll, kPresets, kMine, nil];
    std::vector<std::string> makers;
    for (auto& kv : _counts)
        if (kv.first != "Character Presets" && kv.first != "My Lenses") makers.push_back(kv.first);
    std::sort(makers.begin(), makers.end(), [](const std::string& a, const std::string& b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    for (const std::string& m : makers) [_categories addObject:NS(m)];

    [self buildWindow:NS(req.title)];
    return self;
}

- (void)buildWindow:(NSString*)title
{
    const CGFloat W = 1220, H = 700;
    self.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered defer:NO];
    self.panel.title = title.length ? title : @"Choose Lens";
    self.panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.panel.minSize = NSMakeSize(1000, 560);
    self.panel.delegate = self;
    self.panel.releasedWhenClosed = NO;
    NSView* root = self.panel.contentView;

    const CGFloat side = 210, preview = 300, pad = 14, bottom = 56, top = 52;

    self.search = [[NSSearchField alloc] initWithFrame:NSMakeRect(pad + side + pad, H - top + 12, 360, 26)];
    self.search.placeholderString = @"Search lenses, makers, mounts…";
    self.search.delegate = self;
    self.search.autoresizingMask = NSViewMinYMargin;
    [root addSubview:self.search];

    self.countLabel = [NSTextField labelWithString:@""];
    self.countLabel.frame = NSMakeRect(pad + side + pad + 372, H - top + 16, 260, 18);
    self.countLabel.textColor = NSColor.secondaryLabelColor;
    self.countLabel.autoresizingMask = NSViewMinYMargin;
    [root addSubview:self.countLabel];

    // Sidebar
    NSScrollView* sideScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(pad, bottom, side, H - bottom - top + 38)];
    sideScroll.hasVerticalScroller = YES;
    sideScroll.autoresizingMask = NSViewHeightSizable;
    self.sidebar = [[NSTableView alloc] initWithFrame:sideScroll.bounds];
    NSTableColumn* sc = [[NSTableColumn alloc] initWithIdentifier:@"cat"];
    sc.width = side - 4;
    [self.sidebar addTableColumn:sc];
    self.sidebar.headerView = nil;
    self.sidebar.rowHeight = 22;
    self.sidebar.style = NSTableViewStyleSourceList;
    self.sidebar.dataSource = self;
    self.sidebar.delegate = self;
    sideScroll.documentView = self.sidebar;
    [root addSubview:sideScroll];

    // Lens list
    CGFloat listX = pad + side + pad, listW = W - listX - pad - preview - pad;
    NSScrollView* listScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(listX, bottom, listW, H - bottom - top)];
    listScroll.hasVerticalScroller = YES;
    listScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.table = [[NSTableView alloc] initWithFrame:listScroll.bounds];
    struct { NSString* ident; NSString* title; CGFloat width; } cols[] = {
        {@"name", @"Lens", listW - 380}, {@"focal", @"Focal", 118}, {@"mount", @"Mount", 92}, {@"traits", @"Data", 170}};
    for (auto& c : cols)
    {
        NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:c.ident];
        col.title = c.title;
        col.width = c.width;
        col.resizingMask = [c.ident isEqualToString:@"name"] ? NSTableColumnAutoresizingMask : NSTableColumnUserResizingMask;
        [self.table addTableColumn:col];
    }
    self.table.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    self.table.rowHeight = 22;
    self.table.usesAlternatingRowBackgroundColors = YES;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.target = self;
    self.table.doubleAction = @selector(useLens:);
    listScroll.documentView = self.table;
    [root addSubview:listScroll];

    // Preview
    self.preview = [[LCLensPreview alloc] initWithFrame:NSMakeRect(W - pad - preview, bottom, preview, H - bottom - top + 38)];
    self.preview.aperture = _aperture;
    self.preview.autoresizingMask = NSViewMinXMargin | NSViewHeightSizable;
    self.preview.wantsLayer = YES;
    self.preview.layer.cornerRadius = 6;
    [root addSubview:self.preview];

    // Buttons
    self.useButton = [NSButton buttonWithTitle:@"Use Lens" target:self action:@selector(useLens:)];
    self.useButton.frame = NSMakeRect(W - pad - 120, 14, 120, 30);
    self.useButton.keyEquivalent = @"\r";
    self.useButton.autoresizingMask = NSViewMinXMargin;
    [root addSubview:self.useButton];

    NSButton* cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    cancel.frame = NSMakeRect(W - pad - 120 - 10 - 100, 14, 100, 30);
    cancel.keyEquivalent = @"\033";
    cancel.autoresizingMask = NSViewMinXMargin;
    [root addSubview:cancel];

    if (_allowNone)
    {
        NSButton* none = [NSButton buttonWithTitle:@"Use Base Lens" target:self action:@selector(useBase:)];
        none.frame = NSMakeRect(pad, 14, 140, 30);
        [root addSubview:none];
    }
}

// ---- filtering ----

- (void)refilter
{
    NSInteger catRow = self.sidebar.selectedRow;
    NSString* cat = catRow >= 0 && catRow < (NSInteger)_categories.count ? _categories[catRow] : kAll;
    std::string catS = cat.UTF8String;
    std::string q = normalizeForSearch(self.search.stringValue.UTF8String ?: "");
    std::vector<std::string> qt;
    {
        size_t start = 0;
        while (start < q.size())
        {
            size_t sp = q.find(' ', start);
            if (sp == std::string::npos) sp = q.size();
            qt.push_back(q.substr(start, sp - start));
            start = sp + 1;
        }
    }

    _rows.clear();
    for (size_t i = 0; i < _all.size(); i++)
    {
        const Profile& p = *_all[i];
        if ([cat isEqualToString:kPresets] && p.source != "builtin") continue;
        if ([cat isEqualToString:kMine] && p.source != "user" && p.source != "custom") continue;
        if (![cat isEqualToString:kAll] && ![cat isEqualToString:kPresets] && ![cat isEqualToString:kMine] && p.maker != catS) continue;
        bool ok = true;
        for (const std::string& t : qt)
            if (_keys[i].find(t) == std::string::npos) { ok = false; break; }
        if (ok) _rows.push_back((int)i);
    }
    // Presets and your own lenses first, then alphabetical
    std::stable_sort(_rows.begin(), _rows.end(), [&](int a, int b) {
        auto rank = [&](int i) { const std::string& s = _all[i]->source; return s == "custom" ? 0 : s == "user" ? 1 : s == "builtin" ? 2 : 3; };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return strcasecmp(_all[a]->name.c_str(), _all[b]->name.c_str()) < 0;
    });
    [self.table reloadData];
    self.countLabel.stringValue = [NSString stringWithFormat:@"%zu lenses", _rows.size()];
    if (!_rows.empty() && self.table.selectedRow < 0)
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [self updatePreview];
}

- (void)updatePreview
{
    NSInteger r = self.table.selectedRow;
    [self.preview showProfile:(r >= 0 && r < (NSInteger)_rows.size()) ? _all[_rows[r]] : ProfilePtr()];
    self.useButton.enabled = r >= 0;
}

- (void)selectCategory:(NSString*)cat search:(NSString*)text selectId:(const std::string&)pid
{
    NSUInteger ci = [_categories indexOfObject:cat];
    [self.sidebar reloadData];
    [self.sidebar selectRowIndexes:[NSIndexSet indexSetWithIndex:ci == NSNotFound ? 0 : ci] byExtendingSelection:NO];
    self.search.stringValue = text ?: @"";
    [self refilter];
    for (size_t i = 0; i < _rows.size(); i++)
        if (_all[_rows[i]]->id == pid)
        {
            [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
            [self.table scrollRowToVisible:i];
            break;
        }
    [self updatePreview];
}

- (std::string)selectedId
{
    NSInteger r = self.table.selectedRow;
    return (r >= 0 && r < (NSInteger)_rows.size()) ? _all[_rows[r]]->id : std::string();
}

// ---- actions ----

- (void)finish:(BOOL)chose
{
    self.chose = chose;
    if (self.modal)
    {
        self.modal = NO;
        [NSApp stopModal];
    }
    [self.panel orderOut:nil];
}

- (void)useLens:(id)sender { if (self.table.selectedRow >= 0) [self finish:YES]; }
- (void)cancel:(id)sender { [self finish:NO]; }
- (void)useBase:(id)sender
{
    [self.table deselectAll:nil];
    _rows.clear();
    [self finish:YES];
}
- (void)windowWillClose:(NSNotification*)n { if (self.modal) { self.modal = NO; self.chose = NO; [NSApp stopModal]; } }

- (void)controlTextDidChange:(NSNotification*)n { [self refilter]; }

// ---- table data ----

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv
{
    return tv == self.sidebar ? (NSInteger)_categories.count : (NSInteger)_rows.size();
}

- (id)tableView:(NSTableView*)tv objectValueForTableColumn:(NSTableColumn*)col row:(NSInteger)row
{
    if (tv == self.sidebar)
    {
        NSString* cat = _categories[row];
        size_t n = [cat isEqualToString:kAll] ? _all.size()
                 : [cat isEqualToString:kPresets] ? (size_t)_counts["Character Presets"]
                 : [cat isEqualToString:kMine] ? (size_t)(_counts["My Lenses"] + (_all.empty() || _all[0]->source != "custom" ? 0 : 1))
                 : (size_t)_counts[cat.UTF8String];
        return [NSString stringWithFormat:@"%@  (%zu)", cat, n];
    }
    const int i = _rows[row];
    const Profile& p = *_all[i];
    NSString* ident = col.identifier;
    if ([ident isEqualToString:@"name"]) return NS(p.name);
    if ([ident isEqualToString:@"focal"]) return NS(_facts[i].focal);
    if ([ident isEqualToString:@"mount"]) return NS(p.mount);
    return NS(_facts[i].traits);
}

- (BOOL)tableView:(NSTableView*)tv shouldEditTableColumn:(NSTableColumn*)col row:(NSInteger)row { return NO; }

- (void)tableViewSelectionDidChange:(NSNotification*)n
{
    if (n.object == self.sidebar) [self refilter];
    else [self updatePreview];
}
@end

// ---- entry points ---------------------------------------------------------------------------------

namespace lc {

static bool runOnMain(bool (^block)(void))
{
    if ([NSThread isMainThread]) return block();
    __block bool result = false;
    dispatch_sync(dispatch_get_main_queue(), ^{ result = block(); });
    return result;
}

bool showLensBrowser(const BrowserRequest& p_Request, std::string& outId)
{
    BrowserRequest req = p_Request;
    __block std::string picked;
    bool ok = runOnMain(^bool {
        @autoreleasepool
        {
            [NSApplication sharedApplication];
            LCLensBrowserController* c = [[LCLensBrowserController alloc] initWithRequest:req];
            NSString* startCat = kAll;
            [c selectCategory:startCat search:@"" selectId:req.currentId];
            [c.panel center];
            c.modal = YES;
            [c.panel makeKeyAndOrderFront:nil];
            [c.panel makeFirstResponder:c.search];
            [NSApp runModalForWindow:c.panel];
            if (c.chose) picked = [c selectedId];
            return c.chose;
        }
    });
    if (ok) outId = picked;
    return ok;
}

bool snapshotLensBrowser(const BrowserRequest& p_Request, const std::string& p_Category, const std::string& p_Search,
                         const std::string& p_SelectId, const std::string& p_PngPath)
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        LCLensBrowserController* c = [[LCLensBrowserController alloc] initWithRequest:p_Request];
        [c selectCategory:NS(p_Category) search:NS(p_Search) selectId:p_SelectId];
        NSView* v = c.panel.contentView;
        [v layoutSubtreeIfNeeded];
        [v displayIfNeeded];
        NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
        if (!rep) return false;
        [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return [png writeToFile:NS(p_PngPath) atomically:YES];
    }
}

} // namespace lc
