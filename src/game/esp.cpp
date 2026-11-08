// The cheats page's ESP, each part its own row: ESP STYLE, a box round each living enemy in view and a line to it from
// the bottom of the screen; ESP HEALTH BARS, a bar over it; ESP ARROWS, an enemy out of the view as one arrowhead on a
// ring round the middle of the screen, in a colour of its own; ESP LABEL, a plate with its health and its distance,
// drawn by ImGui (EspOverlay), always or for a few seconds after the enemy was hit. All of it is drawn over the finished
// frame with the view the scene was drawn with, and none of it with every row off. AUTO HEADSHOT: while the player aims
// in the view turns to the head of the living enemy nearest the crosshair. AIM ASSIST: a small pull of the view to the
// body of an enemy near the crosshair while the player fires
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <imgui.h>

#include "common/aspect.h"
#include "common/options.h"
#include "common/filelog.h"
#include "game/layout.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace layout;

namespace touch {
bool Firing();  // touch.cpp: a fire button of the touch controls, or a controller's right trigger, is held
}

namespace {

// character: kind 1 an enemy, its position the one its update makes a velocity of. The world is Y up: that update
// zeroes Y for the speed along the ground
constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharPosition = 0xA0;
constexpr uint32_t kCharHealth = 0x2F0;  // float
constexpr uint32_t kCharDying = 0x384;
// not 0 for a character an AI drives; an ally has none, so only the AI-driven ones count as enemies
constexpr uint32_t kCharAi = 0x39C;
constexpr uint32_t kEnemyKind = 1;

// the player's controller: yaw positive left, pitch positive down, in degrees (mouse.cpp)
constexpr uint32_t kControllerYaw = 0x18;
constexpr uint32_t kControllerPitch = 0x1C;
// the player's zoom of the aim, above 1 while it aims in (hooks.cpp's ZoomStep), and the request to fire
constexpr uint32_t kPlayerZoom = 0x2A4;
constexpr uint32_t kPlayerFireRequest = 0x32C;

constexpr float kBoxWidth = 0.45f;   // of the box's height
constexpr float kNear = 0.08f;       // clip w nearer than this is behind the camera
constexpr float kThickness = 0.8f;   // lines of a 480 frame
constexpr float kBarThickness = 2.2f;
constexpr float kBarGap = 2.5f;      // lines of a 480 frame, between the box and its bar
constexpr float kArrow = 9.0f;       // the direction marker's length, lines of a 480 frame
constexpr float kRing = 0.30f;       // the ring the markers sit on, of the viewport's shorter side
constexpr float kBaseHeight = 1.8f;  // metres: what the head and body points are taken from, whatever the ESP box's height
constexpr float kChestShare = 0.66f; // of the height: the body, where the assist pulls
constexpr auto kHitShow = std::chrono::seconds(4);  // a label shown for hits stays this long after the last one
constexpr float kCone = 30.0f;       // degrees from the crosshair an enemy is taken in by the auto headshot
constexpr float kPull = 1.0f;        // of the turn to the head, a frame
constexpr float kAssistCone[] = {0.0f, 6.0f, 10.0f, 15.0f};   // degrees, by the AIM ASSIST level
constexpr float kAssistPull[] = {0.0f, 0.03f, 0.06f, 0.10f};  // of the turn to the body, a frame
constexpr float kDegrees = 180.0f / 3.14159265f;
constexpr auto kFireHold = std::chrono::milliseconds(150);  // a fire request counts this long after it was seen

// an enemy is drawn while its update ran this recently: none once the game pauses, no flicker between ticks
constexpr auto kSeen = std::chrono::milliseconds(250);
constexpr size_t kMaxEnemies = 64;

struct Seen {
    uint32_t who;
    std::chrono::steady_clock::time_point at;
    float peak;  // the most health it was seen with, the health bar's full
    float last;  // its health at the last update, a drop is a hit
    std::chrono::steady_clock::time_point hit_at;  // the last hit
};
Seen g_seen[kMaxEnemies];
size_t g_count;

// the scene's view and projection, row vectors: clip = position * view * projection
float g_view[16], g_projection[16];
bool g_camera;
uint32_t g_camera_frame;  // counts the scene's draws: the view turns once for each view it reads

// the 2D layer's untextured triangles (eax triangles, ecx their points; float x, y of the points' offset, then a
// float rgba of each point, 16 aligned): in its batch, drawn with its shader and states at the next flush
constexpr uint32_t kTriangles = 0xD5A00;
struct Vertex {
    float x, y;
};
constexpr size_t kSegments = 8;  // the line, the box's four sides, the bar's back and front; a marker takes two triangles
constexpr size_t kMaxVertices = kMaxEnemies * kSegments * 6;

float Height() { return float(std::clamp(game::Opt().esp_height, 100u, 250u)) / 100.0f; }

// something of the ESP is on: the enemies' updates are noted for it, and for the aim cheats
bool Drawing() {
    const game::Options& o = game::Opt();
    return o.esp_style || o.esp_arrows || o.esp_health || o.esp_label;
}
bool Wanted() { return Drawing() || game::Opt().aimbot || game::Opt().aim_assist; }

// a living enemy: one an AI drives, which leaves the allies out
bool Target(uint32_t who) {
    if (R32(who + kCharKind) != kEnemyKind || R32(who + kCharDying)) return false;
    return G<float>(who + kCharHealth) > 0 && R32(who + kCharAi) != 0;
}

void Remember(uint32_t who) {
    auto now = std::chrono::steady_clock::now();
    float health = G<float>(who + kCharHealth);
    for (size_t i = 0; i < g_count; i++)
        if (g_seen[i].who == who) {
            g_seen[i].at = now;
            g_seen[i].peak = std::max(g_seen[i].peak, health);
            if (health < g_seen[i].last - 0.001f) g_seen[i].hit_at = now;
            g_seen[i].last = health;
            return;
        }
    // each character once in the log, to tell allies from enemies by what they have
    filelog::Write("esp", "character %08X kind %u ai %08X health %.0f", who, R32(who + kCharKind), R32(who + kCharAi), health);
    const auto long_ago = now - std::chrono::hours(1);
    if (g_count < kMaxEnemies) {
        g_seen[g_count++] = {who, now, health, health, long_ago};
        return;
    }
    // full with the ESP off, which keeps the list: the one seen longest ago goes
    Seen* oldest = g_seen;
    for (Seen& s : g_seen)
        if (s.at < oldest->at) oldest = &s;
    *oldest = {who, now, health, health, long_ago};
}

// clip space; false behind the camera
bool Clip(const float p[3], float clip[4]) {
    float eye[4];
    for (int c = 0; c < 4; c++)
        eye[c] = p[0] * g_view[c] + p[1] * g_view[4 + c] + p[2] * g_view[8 + c] + g_view[12 + c];
    for (int c = 0; c < 4; c++)
        clip[c] = eye[0] * g_projection[c] + eye[1] * g_projection[4 + c] + eye[2] * g_projection[8 + c] +
                  eye[3] * g_projection[12 + c];
    return clip[3] > kNear;
}

// eye space: x to the right, z ahead
void Eye(const float p[3], float eye[3]) {
    for (int c = 0; c < 3; c++) eye[c] = p[0] * g_view[c] + p[1] * g_view[4 + c] + p[2] * g_view[8 + c] + g_view[12 + c];
}

// screen position in viewport pixels; false behind the camera
bool Project(const float p[3], float vx, float vy, float vw, float vh, float out[2]) {
    float clip[4];
    if (!Clip(p, clip)) return false;
    out[0] = vx + (clip[0] / clip[3] + 1) * 0.5f * vw;
    out[1] = vy + (1 - clip[1] / clip[3]) * 0.5f * vh;
    return true;
}

// a segment as a quad of two triangles, its width across it
Vertex* Segment(Vertex* v, float x0, float y0, float x1, float y1, float width) {
    float dx = x1 - x0, dy = y1 - y0;
    float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0)) return v;
    float nx = -dy / length * width * 0.5f, ny = dx / length * width * 0.5f;
    const float corner[4][2] = {{x0 + nx, y0 + ny}, {x0 - nx, y0 - ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}};
    for (int i : {0, 1, 2, 2, 1, 3}) *v++ = {corner[i][0], corner[i][1]};
    return v;
}

// 0xRRGGBB as float rgba
void Unpack(uint32_t c, float out[4]) {
    out[0] = float((c >> 16) & 255) / 255.0f;
    out[1] = float((c >> 8) & 255) / 255.0f;
    out[2] = float(c & 255) / 255.0f;
    out[3] = 1.0f;
}

// what the overlay draws, handed from the game thread (DrawEsp) to the render thread (EspOverlay): each enemy in view
// as its place over the viewport, 0 to 1, with its health, the share of its most, and its distance in metres
struct Label {
    float u, v, health, share, distance;
};
struct Labels {
    Label item[kMaxEnemies];
    size_t count = 0;
    float on[4] = {0, 1, 0, 1};
    std::chrono::steady_clock::time_point at;
};
std::mutex g_labels_lock;
Labels g_labels;

void Publish(const Label* items, size_t count, const float on[4]) {
    std::lock_guard lock(g_labels_lock);
    g_labels.count = count;
    for (size_t i = 0; i < count; i++) g_labels.item[i] = items[i];
    std::memcpy(g_labels.on, on, 16);
    g_labels.at = std::chrono::steady_clock::now();
}

// the turn to a point on the nearest enemy to the crosshair, in degrees
struct Turn {
    float yaw = 0, pitch = 0;
    bool found = false;
};

// share of kBaseHeight above the feet, side metres to the right of the aim point on the screen (left when negative)
Turn Nearest(float cone, float share, float side) {
    Turn turn;
    auto now = std::chrono::steady_clock::now();
    float best = cone;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || !Target(who)) continue;
        const float* at = &G<float>(who + kCharPosition);
        float point[3] = {at[0] + g_view[0] * side, at[1] + kBaseHeight * share + g_view[4] * side, at[2] + g_view[8] * side};
        float clip[4];
        if (!Clip(point, clip)) continue;
        float y = -std::atan(clip[0] / clip[3] / g_projection[0]) * kDegrees;
        float p = -std::atan(clip[1] / clip[3] / g_projection[5]) * kDegrees;
        float off = std::sqrt(y * y + p * p);
        if (off < best) best = off, turn = {y, p, true};
    }
    return turn;
}

}  // namespace

namespace game {

// the device's view and projection as the scene draws them (D3DDevice_SetTransform, view 0 and projection 1)
void EspCamera() {
    uint32_t transforms = SYM(D3D_Device) + XDK_CDevice_Transforms;
    std::memcpy(g_view, HostPtr(transforms), sizeof(g_view));
    std::memcpy(g_projection, HostPtr(transforms + 64), sizeof(g_projection));
    g_camera = true;
    g_camera_frame++;
}

// esi the player's controller in its look update, before the game clamps the pitch and wraps the yaw. The turn is
// read off the projection, the angles of the target from the middle of the view
void Aimbot(X86Context& ctx) {
    static uint32_t turned;
    static std::chrono::steady_clock::time_point fired;
    uint32_t c = ctx.esi;
    uint32_t me = R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0;
    uint32_t level = std::min(Opt().aim_assist, 3u);
    if ((!Opt().aimbot && !level) || !g_camera || !me || turned == g_camera_frame) return;
    turned = g_camera_frame;
    auto now = std::chrono::steady_clock::now();
    if (R32(me + kPlayerFireRequest)) fired = now;
    bool aiming = G<float>(me + kPlayerZoom) > 1.01f;
    bool firing = touch::Firing() || now - fired < kFireHold;
    Turn turn;
    float pull = 0;
    if (Opt().aimbot && aiming) {
        turn = Nearest(kCone, float(std::clamp(Opt().head_height, 40u, 100u)) / 100.0f,
                       float(int(std::clamp(Opt().head_offset, 0u, 60u)) - 30) / 100.0f);
        pull = kPull;
    } else if (level && firing) {  // not for aiming in: only a shot that is fired is helped
        turn = Nearest(kAssistCone[level], kChestShare, 0.0f);
        pull = kAssistPull[level];
    }
    if (!turn.found) return;
    G<float>(c + kControllerYaw) += turn.yaw * pull;
    G<float>(c + kControllerPitch) += turn.pitch * pull;
}

// render thread, inside the ImGui frame (menu.cpp's DrawUi): ESP LABEL. The frame is shown letterboxed to the
// output's aspect, in the middle of the screen (vulkan.cpp's DrawFrame), so a place over the viewport maps onto that
// rectangle. Each enemy gets a small dark plate, edged in the ESP colour: its health in the colour of what is left,
// then its distance
void EspOverlay() {
    uint32_t mode = std::min(Opt().esp_label, 3u);
    if (!mode) return;
    Labels l;
    {
        std::lock_guard lock(g_labels_lock);
        l = g_labels;
    }
    // the game thread publishes every frame: older than this and it paused, nothing is shown
    if (std::chrono::steady_clock::now() - l.at > std::chrono::milliseconds(150)) return;
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x, h = io.DisplaySize.y, aspect = Aspect();
    float fw = std::min(w, h * aspect), fh = fw / aspect;
    float fx = (w - fw) * 0.5f, fy = (h - fh) * 0.5f;
    float line = fh / 480.0f;  // a line of the 480 frame, in screen pixels
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    float size = ImGui::GetFontSize() * 0.5f;
    float scale = size / ImGui::GetFontSize();
    float pad = size * 0.45f, gap = size * 0.7f;
    // over the box, and over its health bar when there is one
    float lift = ((Opt().esp_health ? kBarGap + kBarThickness : 0.0f) + 2.5f) * line;
    ImU32 edge = ImGui::ColorConvertFloat4ToU32(ImVec4(l.on[0], l.on[1], l.on[2], 0.85f));
    ImU32 ground = IM_COL32(10, 12, 14, 175);
    ImU32 dim = IM_COL32(215, 222, 228, 255);
    char hp[16], dist[16];
    for (size_t i = 0; i < l.count; i++) {
        const Label& e = l.item[i];
        bool show_hp = mode & 1, show_dist = (mode & 2) && e.distance >= 0;
        if (!show_hp && !show_dist) continue;
        std::snprintf(hp, sizeof(hp), "%d", int(std::lround(e.health)));
        std::snprintf(dist, sizeof(dist), "%d m", int(std::lround(e.distance)));
        ImVec2 hp_size = ImGui::CalcTextSize(hp), dist_size = ImGui::CalcTextSize(dist);
        hp_size.x *= scale, hp_size.y *= scale, dist_size.x *= scale, dist_size.y *= scale;
        float inner = (show_hp ? hp_size.x : 0) + (show_dist ? dist_size.x : 0) + (show_hp && show_dist ? gap : 0);
        float text_h = std::max(hp_size.y, dist_size.y);
        float pw = inner + pad * 2, ph = text_h + pad;
        float cx = fx + e.u * fw, bottom = fy + e.v * fh - lift;
        float left = std::clamp(cx - pw * 0.5f, fx, std::max(fx, fx + fw - pw));
        float top = std::max(fy, bottom - ph);
        ImVec2 a(left, top), b(left + pw, top + ph);
        draw->AddRectFilled(a, b, ground, size * 0.35f);
        draw->AddRect(a, b, edge, size * 0.35f, 0, std::max(1.0f, fh / 540.0f));
        float x = left + pad, y = top + (ph - text_h) * 0.5f;
        if (show_hp) {
            // red as it empties, green while it holds
            ImU32 colour = ImGui::ColorConvertFloat4ToU32(
                ImVec4(std::min(1.0f, 2 * (1 - e.share)), std::min(1.0f, 2 * e.share), 0.2f, 1));
            draw->AddText(font, size, ImVec2(x, y), colour, hp);
            x += hp_size.x;
        }
        if (show_hp && show_dist) {
            float mid = x + gap * 0.5f;
            draw->AddLine(ImVec2(mid, top + ph * 0.25f), ImVec2(mid, top + ph * 0.75f), IM_COL32(255, 255, 255, 70));
            x += gap;
        }
        if (show_dist) draw->AddText(font, size, ImVec2(x, y), dim, dist);
    }
}

// D3DVIEWPORT8: x, y, width, height. Called between the 2D layer's begin and end
void DrawEsp(X86Context& ctx, float unit) {
    float on[4], off[4];
    Unpack(Opt().esp_on_color, on);
    Unpack(Opt().esp_off_color, off);
    if (!Drawing() || !g_camera || !R32(kWorld)) {
        Publish(nullptr, 0, on);
        return;
    }
    const uint32_t me = R32(kWorld) + kWorldPlayer;
    const float* mine = &G<float>(me + kCharPosition);
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    static const uint32_t buffer = mem::AllocSystem(kMaxVertices * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(kMaxVertices * 16, 16);
    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    float(*colour)[4] = static_cast<float(*)[4]>(HostPtr(colours));
    Vertex* v = first;
    // a segment with a colour for each of its points
    auto add = [&](float x0, float y0, float x1, float y1, float width, const float rgba[4]) {
        size_t before = size_t(v - first);
        v = Segment(v, x0, y0, x1, y1, width);
        for (size_t i = before; i < size_t(v - first); i++) std::memcpy(colour[i], rgba, 16);
    };
    Label labels[kMaxEnemies];
    size_t labelled = 0;
    const float back[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    const uint32_t style = std::min(Opt().esp_style, 3u), label_mode = std::min(Opt().esp_label, 3u);
    const float width = float(std::min(Opt().esp_width, 30u)) / 10.0f * unit;  // the box and the line
    const float tail = kThickness * unit * 1.6f, bar = kBarThickness * unit;
    float height = Height();
    auto now = std::chrono::steady_clock::now();
    size_t kept = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || !Target(who)) continue;
        Seen entry = g_seen[kept++] = g_seen[i];
        const float* at = &G<float>(who + kCharPosition);
        float chest[3] = {at[0], at[1] + height * kChestShare, at[2]}, clip[4];
        bool front = Clip(chest, clip);
        bool inside = front && std::fabs(clip[0]) <= clip[3] && std::fabs(clip[1]) <= clip[3];
        if (!inside) {
            if (!Opt().esp_arrows) continue;
            // out of the view: one arrowhead on a ring round the middle, as the direction cues of other shooters. In
            // front of the camera (beside the picture, above or below) it sits where the enemy is on the screen and
            // is the head alone; behind the player it sits on the lower half by the enemy's bearing, with a short
            // tail towards the middle
            float dir[2];
            if (front) {
                dir[0] = clip[0] / clip[3];
                dir[1] = -clip[1] / clip[3];
            } else {
                float eye[3];
                Eye(chest, eye);
                dir[0] = eye[0];
                dir[1] = -eye[2];
            }
            float length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
            if (!std::isfinite(length)) continue;
            if (length < 1e-4f) dir[0] = 0, dir[1] = front ? -1.0f : 1.0f, length = 1;
            float ux = dir[0] / length, uy = dir[1] / length;
            float ring = kRing * std::min(vw, vh), h = kArrow * unit;
            float mx = vx + vw * 0.5f + ux * ring, my = vy + vh * 0.5f + uy * ring;
            // the head points out from the middle, its base a little wider than long
            const float pts[3][2] = {{mx + ux * h, my + uy * h},
                                     {mx - ux * h * 0.5f - uy * h * 0.8f, my - uy * h * 0.5f + ux * h * 0.8f},
                                     {mx - ux * h * 0.5f + uy * h * 0.8f, my - uy * h * 0.5f - ux * h * 0.8f}};
            size_t before = size_t(v - first);
            for (const auto& q : pts) *v++ = {q[0], q[1]};
            for (size_t i = before; i < size_t(v - first); i++) std::memcpy(colour[i], off, 16);
            if (!front) add(mx - ux * h * 0.5f, my - uy * h * 0.5f, mx - ux * h * 2.2f, my - uy * h * 2.2f, tail, off);
            continue;
        }
        float feet[3] = {at[0], at[1], at[2]};
        float top[3] = {at[0], at[1] + height, at[2]};
        float f[2], h[2];
        if (!Project(feet, vx, vy, vw, vh, f) || !Project(top, vx, vy, vw, vh, h)) continue;
        float hp = G<float>(who + kCharHealth);
        float dx = at[0] - mine[0], dy = at[1] - mine[1], dz = at[2] - mine[2];
        // the plate: always, or only for a few seconds after a hit; AUTO is always with a box or a line, else after a hit
        bool hit = now - entry.hit_at < kHitShow;
        uint32_t show = std::min(Opt().esp_label_show, 2u);
        if (label_mode && (show == 1 || (show == 0 && style) || hit))
            labels[labelled++] = {(f[0] - vx) / vw, (h[1] - vy) / vh, hp,
                                  entry.peak > 0 ? std::clamp(hp / entry.peak, 0.0f, 1.0f) : 1.0f,
                                  std::sqrt(dx * dx + dy * dy + dz * dz)};
        float half = (f[1] - h[1]) * kBoxWidth * 0.5f;
        float left = f[0] - half, right = f[0] + half;
        if (width > 0) {
            if (style & 2) add(vx + vw * 0.5f, vy + vh, f[0], f[1], width, on);
            if (style & 1) {
                add(left, h[1], right, h[1], width, on);
                add(left, f[1], right, f[1], width, on);
                add(left, h[1], left, f[1], width, on);
                add(right, h[1], right, f[1], width, on);
            }
        }
        if (Opt().esp_health) {
            float frac = entry.peak > 0 ? std::clamp(G<float>(who + kCharHealth) / entry.peak, 0.0f, 1.0f) : 1.0f;
            float y = h[1] - kBarGap * unit - bar * 0.5f;
            const float fill[4] = {std::min(1.0f, 2 * (1 - frac)), std::min(1.0f, 2 * frac), 0.0f, 1.0f};
            add(left, y, right, y, bar, back);
            if (frac > 0) add(left, y, left + (right - left) * frac, y, bar, fill);
        }
    }
    g_count = kept;
    Publish(labels, labelled, on);
    if (v == first) return;
    W32(offset, 0);
    W32(offset + 4, 0);
    uint32_t eax = ctx.eax, ecx = ctx.ecx;
    ctx.eax = uint32_t(v - first) / 3;
    ctx.ecx = buffer;
    CallGuest(ctx, kTriangles, {offset, colours});
    ctx.eax = eax;
    ctx.ecx = ecx;
}

}  // namespace game

// The characters' update, a frame (ecx the character, its class's own; the player's is 0x1E5FA0): the enemies
// that ran it are the ones drawn
X86_EXTERN_FUNC(__imp__sub_001DFD90);
X86_FUNC(sub_001DFD90) {
    uint32_t who = ctx.ecx;
    if (Wanted() && who && R32(who + kCharKind) == kEnemyKind && !R32(who + kCharDying))
        Remember(who);
    __imp__sub_001DFD90(ctx, base);
}
