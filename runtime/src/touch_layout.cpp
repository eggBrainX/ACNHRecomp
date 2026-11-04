// Drawn touch controls (docs/nvn/ANDROID-PORT.md A4): a fixed layout of round buttons per game, hit-tested by
// platform_android_input.cpp and drawn by the NVN present pass (nvn_present.cpp) over the picture.
//
// Super Mario Bros. Wonder, landscape: A (jump, the big one) at the bottom right with B (jump), Y (dash, action) and
// X (action) around it; ZR (roll), R, L, + and - along the top. The left half is the stick (platform_android_input.cpp):
// it appears where the thumb lands, and pushed nearly all the way it also holds Y, so running needs no second thumb.
// Games without a layout here keep their own touch handling (Hades: zones around the thumb).
//
// Elsewhere touch_overlay() draws nothing, except with NX_OVERLAY_DEMO=1 (the buttons at rest), to check the look
// on the Mac with NX_PRESENT_TEST.
#include "touch_layout.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nx/game.h>
#include <nx/runtime.h>

#include "config.h"

namespace nx::plat {

namespace {

// x, y: 0..1 across the landscape screen; r: a fraction of the screen height.
const TouchButton kWonder[] = {
    {0.905f, 0.74f, 0.105f, kA},     {0.775f, 0.86f, 0.085f, kB},    {0.775f, 0.60f, 0.085f, kY},
    {0.905f, 0.47f, 0.075f, kX},     {0.955f, 0.25f, 0.055f, kZR},   {0.955f, 0.07f, 0.045f, kR},
    {0.045f, 0.07f, 0.045f, kL},     {0.545f, 0.07f, 0.040f, kPlus}, {0.455f, 0.07f, 0.040f, kMinus},
};

const char* const kNames[] = {"A", "B", "Y", "X", "ZR", "R", "L", "PLUS", "MINUS"};  // the order of kWonder

// Animal Crossing: New Horizons, landscape: the face buttons in the Switch's diamond at the bottom right (A talk and
// pick, B run and cancel, X pockets, Y pick up), L/R (tool ring, camera) and ZL/ZR (NookPhone, reactions) along the
// top corners, + and - at the top centre with the settings gear between them, and a small d-pad (tools, camera
// zoom) at the bottom left, under where the stick usually lands. The rest of the left half is the stick.
const TouchButton kAcnh[] = {
    {0.915f, 0.700f, 0.080f, kA},     {0.835f, 0.845f, 0.080f, kB},     {0.835f, 0.555f, 0.080f, kX},
    {0.755f, 0.700f, 0.080f, kY},     {0.955f, 0.090f, 0.050f, kR},     {0.880f, 0.090f, 0.050f, kZR},
    {0.045f, 0.090f, 0.050f, kL},     {0.120f, 0.090f, 0.050f, kZL},    {0.560f, 0.070f, 0.040f, kPlus},
    {0.440f, 0.070f, 0.040f, kMinus}, {0.500f, 0.070f, 0.036f, kSettingsButton},
    {0.075f, 0.700f, 0.040f, kUp},    {0.075f, 0.900f, 0.040f, kDown},  {0.030f, 0.800f, 0.040f, kLeft},
    {0.120f, 0.800f, 0.040f, kRight},
};

// kAcnh is drawn for 16:9. On another shape each button keeps its distance from its side of the screen (in screen
// heights, as a thumb measures it) and the clusters keep their corners: on a 20:9 phone the d-pad and the face
// buttons stay under the thumbs instead of spreading toward the middle. Settings > Controls > On-screen controls
// size (input.touch_size, or NX_TOUCH_SIZE: 50..150 %) scales every button and its distance from its corner (#42).
constexpr float kAcnhAspect = 16.0f / 9.0f;
constexpr size_t kAcnhCount = sizeof kAcnh / sizeof kAcnh[0];

// ---- a layout made in the button customizer ----------------------------------------------------------------------
// <data folder>/touch_layout.txt (Home.java imports it from the clipboard; or put the file there) holds the lines the
// customizer's "Copy settings" gives, one `key = value` each:
//   input.touch_layout = A:x,y,r;B:x,y,r;...   positions (0..1 across the screen) and radii (of its height), as drawn
//                                              for 16:9; a button not named keeps its place
//   input.touch_stick = x,y,r                  where the stick's hint is, and the stick's reach
//   input.touch_hidden = X,UP                  buttons not drawn and not pressable
//   input.touch_mirror = 1                     the whole layout flipped (left-handed): the stick takes the right half
//   input.touch_size = 50..150                 scales every radius
//   input.touch_opacity = 10..100              brightness
//   input.touch_controls = true|false          on-screen controls at all
// input.touch_skin is ignored (one look). Read once, when the game starts. Without the file nothing changes.
const char* const kAcnhNames[] = {"A", "B", "X", "Y", "R", "ZR", "L", "ZL", "PLUS", "MINUS", "SETTINGS", "UP", "DOWN", "LEFT", "RIGHT"};
static_assert(sizeof kAcnhNames / sizeof kAcnhNames[0] == kAcnhCount, "one name per button of kAcnh");

struct Custom {
    bool present = false;
    std::map<std::string, std::array<float, 3>> layout;
    std::array<float, 3> stick{0.2325f, 0.66f, 0.12f};  // the customizer's default
    std::set<std::string> hidden;
    bool mirror = false;
    int size = 0, opacity = 0, controls = -1;  // 0: not set; controls -1: not set
};

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(sep, pos);
        if (end == std::string::npos) end = s.size();
        out.push_back(trim(s.substr(pos, end - pos)));
        pos = end + 1;
    }
    return out;
}

bool valid_xyr(float x, float y, float r) { return x >= 0 && x <= 1 && y >= 0 && y <= 1 && r > 0.01f && r < 0.3f; }

const Custom& custom() {
    static const Custom c = [] {
        Custom out;
        std::ifstream f(data_root() + "/touch_layout.txt");
        std::string line;
        while (std::getline(f, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
            if (key.rfind("input.touch_", 0) != 0) continue;
            key = key.substr(12);
            if (key == "layout") {
                for (const std::string& item : split(val, ';')) {
                    size_t colon = item.find(':');
                    float x, y, r;
                    if (colon == std::string::npos || std::sscanf(item.c_str() + colon + 1, "%f,%f,%f", &x, &y, &r) != 3) continue;
                    if (valid_xyr(x, y, r)) out.layout[item.substr(0, colon)] = {x, y, r}, out.present = true;
                }
            } else if (key == "stick") {
                float x, y, r;
                if (std::sscanf(val.c_str(), "%f,%f,%f", &x, &y, &r) == 3 && valid_xyr(x, y, r)) out.stick = {x, y, r}, out.present = true;
            } else if (key == "hidden") {
                for (const std::string& n : split(val, ','))
                    if (!n.empty()) out.hidden.insert(n);
                out.present = true;
            } else if (key == "mirror") {
                out.mirror = val == "1" || val == "true", out.present = true;
            } else if (key == "size") {
                out.size = std::clamp(std::atoi(val.c_str()), 50, 150), out.present = true;
            } else if (key == "opacity") {
                out.opacity = std::clamp(std::atoi(val.c_str()), 10, 100), out.present = true;
            } else if (key == "controls") {
                out.controls = (val == "1" || val == "true") ? 1 : 0, out.present = true;
            }
        }
        if (out.present) nx::log("touch: layout from touch_layout.txt (%zu buttons moved, %zu hidden, mirror %d)", out.layout.size(),
                                 out.hidden.size(), int(out.mirror));
        return out;
    }();
    return c;
}

// Only Animal Crossing uses it (the names are its buttons').
bool custom_on() {
    static const bool on = std::strcmp(game::kInfo.id, "acnh") == 0 && custom().present;
    return on;
}

// The buttons that are drawn: kAcnh with the customizer's positions, without the hidden ones.
const std::vector<TouchButton>& acnh_base() {
    static const std::vector<TouchButton> v = [] {
        std::vector<TouchButton> out;
        const Custom& c = custom();
        const bool on = custom_on();
        for (size_t k = 0; k < kAcnhCount; k++) {
            TouchButton b = kAcnh[k];
            if (on) {
                if (c.hidden.count(kAcnhNames[k])) continue;
                if (auto it = c.layout.find(kAcnhNames[k]); it != c.layout.end()) b.x = it->second[0], b.y = it->second[1], b.r = it->second[2];
            }
            out.push_back(b);
        }
        return out;
    }();
    return v;
}

int env_int(const char* name, int fallback);

float touch_size_scale() {
    static const int env = env_int("NX_TOUCH_SIZE", 0);
    const int pct = env ? env : (custom_on() && custom().size) ? custom().size : config_get_int("input.touch_size", 100);
    return float(std::clamp(pct, 50, 150)) / 100.0f;
}

TouchButton fit_acnh(const TouchButton& b, float aspect, float s) {
    TouchButton o = b;
    float dx;  // in heights: from the left edge, the right edge or the middle
    if (b.x < 0.3f) {
        dx = b.x * kAcnhAspect * s;
        o.x = dx / aspect;
    } else if (b.x > 0.7f) {
        dx = (1.0f - b.x) * kAcnhAspect * s;
        o.x = 1.0f - dx / aspect;
    } else {
        dx = (b.x - 0.5f) * kAcnhAspect * s;
        o.x = 0.5f + dx / aspect;
    }
    o.y = b.y < 0.5f ? b.y * s : 1.0f - (1.0f - b.y) * s;
    o.r = b.r * s;
    return o;
}

// Two buffers: a new shape or size is written into the one not handed out last (the input and present threads read
// the layout without a lock; it changes only when the setting does).
const TouchButton* acnh_buttons(float aspect) {
    static TouchButton buf[2][kAcnhCount];
    static std::atomic<int> cur{-1};
    static std::mutex lock;
    static float made_aspect = 0, made_scale = 0;
    if (!(aspect > 0.5f && aspect < 5.0f)) aspect = kAcnhAspect;
    const float s = touch_size_scale();
    // A customized layout keeps the positions as drawn in the customizer (size scales the radii only, as there).
    const float ps = custom_on() ? 1.0f : s;
    const std::vector<TouchButton>& base = acnh_base();
    int c = cur.load(std::memory_order_acquire);
    if (c >= 0 && std::fabs(made_aspect - aspect) < 1e-3f && made_scale == s) return buf[c];
    std::lock_guard lk(lock);
    c = cur.load(std::memory_order_relaxed);
    if (c >= 0 && std::fabs(made_aspect - aspect) < 1e-3f && made_scale == s) return buf[c];
    const int next = c == 0 ? 1 : 0;
    for (size_t k = 0; k < base.size(); k++) {
        buf[next][k] = fit_acnh(base[k], aspect, ps);
        if (custom_on()) {
            buf[next][k].r = base[k].r * s;
            if (custom().mirror) buf[next][k].x = 1.0f - buf[next][k].x;
        }
    }
    made_aspect = aspect, made_scale = s;
    cur.store(next, std::memory_order_release);
    return buf[next];
}

int env_int(const char* name, int fallback) {
    const char* e = std::getenv(name);
    return e && *e ? std::atoi(e) : fallback;
}

// The layout as the launcher set it (config.txt, which becomes the environment): NX_TOUCH_LAYOUT=A:x,y,r;B:... replaces
// the positions and radii of the named buttons, NX_TOUCH_SIZE (50..160, percent) scales every radius. Read once.
const std::vector<TouchButton>& wonder_buttons() {
    static const std::vector<TouchButton> v = [] {
        std::vector<TouchButton> out(std::begin(kWonder), std::end(kWonder));
        if (const char* e = std::getenv("NX_TOUCH_LAYOUT")) {
            std::string all = e;
            size_t pos = 0;
            while (pos < all.size()) {
                size_t end = all.find(';', pos);
                if (end == std::string::npos) end = all.size();
                std::string item = all.substr(pos, end - pos);
                pos = end + 1;
                size_t colon = item.find(':');
                float x, y, r;
                if (colon == std::string::npos || std::sscanf(item.c_str() + colon + 1, "%f,%f,%f", &x, &y, &r) != 3) continue;
                for (size_t k = 0; k < out.size(); k++)
                    if (item.compare(0, colon, kNames[k]) == 0 && x >= 0 && x <= 1 && y >= 0 && y <= 1 && r > 0.01f && r < 0.3f)
                        out[k].x = x, out[k].y = y, out[k].r = r;
            }
        }
        float scale = float(std::clamp(env_int("NX_TOUCH_SIZE", 100), 50, 160)) / 100.0f;
        for (auto& b : out) b.r *= scale;
        return out;
    }();
    return v;
}

}  // namespace

// NX_TOUCH_SHOW=0 (launcher: On-screen buttons off), or Settings > Controls > On-screen controls off (read live):
// nothing is drawn and touches are ignored, except by the settings screen.
bool touch_enabled() {
    static const bool env_on = [] {
        const char* e = std::getenv("NX_TOUCH_SHOW");
        return !(e && e[0] == '0');
    }();
    return env_on && config_get_bool("input.touch_controls", true) && !(custom_on() && custom().controls == 0);
}

// NX_TOUCH_MIRROR=1 (left-handed): the stick takes the right half of the screen instead of the left.
bool touch_mirrored() {
    static const bool on = [] {
        const char* e = std::getenv("NX_TOUCH_MIRROR");
        return e && e[0] == '1';
    }();
    return on || (custom_on() && custom().mirror);
}

// NX_TOUCH_OPACITY (10..100, default 85): the drawn buttons' brightness relative to the original look.
float touch_opacity_scale() {
    static const int env = env_int("NX_TOUCH_OPACITY", 0);
    int pct = env                           ? env
              : (custom_on() && custom().opacity) ? custom().opacity
                                                  : std::atoi(config_get("input.touch_opacity", "60").c_str());
    return float(std::clamp(pct, 10, 100)) / 85.0f;
}

const TouchButton* touch_buttons(size_t& count, float aspect) {
    static const bool wonder = std::strcmp(game::kInfo.id, "wonder") == 0;
    static const bool acnh = std::strcmp(game::kInfo.id, "acnh") == 0;
    if (acnh) {
        count = acnh_base().size();
        return acnh_buttons(aspect);
    }
    count = wonder ? wonder_buttons().size() : 0;
    return wonder ? wonder_buttons().data() : nullptr;
}

void touch_stick_rest(float aspect, bool mirrored, float& x, float& y, float& r) {
    static const bool acnh = std::strcmp(game::kInfo.id, "acnh") == 0;
    r = 0.12f;
    if (!(aspect > 0.5f && aspect < 5.0f)) aspect = kAcnhAspect;
    if (acnh && custom_on()) {  // where the customizer put it (mirror flips it, as it flips the buttons)
        const auto& st = custom().stick;
        const TouchButton o = fit_acnh({st[0], st[1], st[2], 0}, aspect, 1.0f);
        x = custom().mirror ? 1.0f - o.x : o.x;
        y = o.y;
        r = st[2] * touch_size_scale();
        return;
    }
    float dx = 0.15f * kAcnhAspect, cy = 0.72f;  // in heights from the side edge
    if (acnh) {
        // Beside the d-pad, not over it: its right arrow's outer edge, then the stick's radius and a gap.
        const TouchButton right = fit_acnh(kAcnh[kAcnhCount - 1], aspect, touch_size_scale());
        dx = right.x * aspect + right.r + r + 0.04f;
        cy = 0.66f;
    }
    x = mirrored ? 1.0f - dx / aspect : dx / aspect;
    y = cy;
}

float touch_stick_radius() { return custom_on() ? custom().stick[2] * touch_size_scale() : 0.12f; }

uint64_t touch_hit(float x, float y, float aspect) {
    size_t n = 0;
    const TouchButton* b = touch_buttons(n, aspect);
    uint64_t best = 0;
    float best_d = 1e9f;
    for (size_t k = 0; k < n; k++) {
        float d = std::hypot((x - b[k].x) * aspect, y - b[k].y) / b[k].r;
        if (d < 1.35f && d < best_d) best = b[k].button, best_d = d;  // a little past the drawn edge still counts
    }
    return best;
}

#if !defined(__ANDROID__)
size_t touch_overlay(OverlayShape* out, size_t max) {
    static const bool demo = std::getenv("NX_OVERLAY_DEMO") != nullptr;
    if (!demo) return 0;
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    for (; k < n && k < max; k++) out[k] = {b[k].x, b[k].y, b[k].r, 0.22f};
    float sx, sy, sr;
    touch_stick_rest(16.0f / 9.0f, false, sx, sy, sr);
    if (k < max) out[k++] = {sx, sy, sr, 0.10f};  // where the stick usually goes
    return k;
}
#endif

}  // namespace nx::plat
