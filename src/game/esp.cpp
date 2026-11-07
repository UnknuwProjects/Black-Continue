// The cheats page's ESP LINES: each living enemy gets a box round it, a health bar over it and a line to it from the
// bottom of the screen, drawn over the finished frame with the view the scene was drawn with. An enemy out of the
// view is an arrow at the screen's edge towards it, in a colour of its own. AUTO HEADSHOT: while the player aims in the
// view turns to the head of the living enemy nearest the crosshair. AIM ASSIST: a small pull of the view to the body
// of an enemy near the crosshair while the player fires or aims in
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "common/options.h"
#include "core/log.h"
#include "game/layout.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace layout;

namespace {

// character: kind 1 an enemy, its position the one its update makes a velocity of. The world is Y up: that update
// zeroes Y for the speed along the ground
constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharPosition = 0xA0;
constexpr uint32_t kCharHealth = 0x2F0;  // float
constexpr uint32_t kCharDying = 0x384;
// not 0 for a character an AI drives; an ally is expected to have none (checked in the log, IGNORE ALLIES row)
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
constexpr float kArrow = 12.0f;      // the edge arrow's head, lines of a 480 frame
constexpr float kEdge = 24.0f;       // the arrow's distance from the screen's edge, pixels
constexpr float kChestShare = 0.66f; // of the height: the body, where the assist pulls
constexpr float kHeadShare = 0.92f;  // of the height: the head's middle, where the auto headshot turns
constexpr float kCone = 30.0f;       // degrees from the crosshair an enemy is taken in by the auto headshot
constexpr float kPull = 1.0f;        // of the turn to the head, a frame
constexpr float kAssistCone[] = {0.0f, 6.0f, 10.0f, 15.0f};   // degrees, by the AIM ASSIST level
constexpr float kAssistPull[] = {0.0f, 0.03f, 0.06f, 0.10f};  // of the turn to the body, a frame
constexpr float kDegrees = 180.0f / 3.14159265f;
constexpr auto kFireHold = std::chrono::milliseconds(300);  // a fire request counts this long after it was seen

// an enemy is drawn while its update ran this recently: none once the game pauses, no flicker between ticks
constexpr auto kSeen = std::chrono::milliseconds(250);
constexpr size_t kMaxEnemies = 64;

struct Seen {
    uint32_t who;
    std::chrono::steady_clock::time_point at;
    float peak;  // the most health it was seen with, the health bar's full
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
constexpr size_t kSegments = 8;  // the line, the box's four sides, the bar's back and front; an arrow takes three
constexpr size_t kMaxVertices = kMaxEnemies * kSegments * 6;

float Height() { return float(std::clamp(game::Opt().esp_height, 100u, 250u)) / 100.0f; }

// a living enemy; with IGNORE ALLIES only the ones an AI drives
bool Target(uint32_t who) {
    if (R32(who + kCharKind) != kEnemyKind || R32(who + kCharDying)) return false;
    if (!(G<float>(who + kCharHealth) > 0)) return false;
    return !game::Opt().ignore_allies || R32(who + kCharAi) != 0;
}

void Remember(uint32_t who) {
    auto now = std::chrono::steady_clock::now();
    float health = G<float>(who + kCharHealth);
    for (size_t i = 0; i < g_count; i++)
        if (g_seen[i].who == who) {
            g_seen[i].at = now;
            g_seen[i].peak = std::max(g_seen[i].peak, health);
            return;
        }
    // each character once in the log, to tell allies from enemies by what they have
    LOGR("esp: character %08X kind %u ai %08X health %.0f", who, R32(who + kCharKind), R32(who + kCharAi), health);
    if (g_count < kMaxEnemies) {
        g_seen[g_count++] = {who, now, health};
        return;
    }
    // full with the ESP off, which keeps the list: the one seen longest ago goes
    Seen* oldest = g_seen;
    for (Seen& s : g_seen)
        if (s.at < oldest->at) oldest = &s;
    *oldest = {who, now, health};
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

// the turn to a point on the nearest enemy to the crosshair, in degrees
struct Turn {
    float yaw = 0, pitch = 0;
    bool found = false;
};

Turn Nearest(float cone, float share) {
    Turn turn;
    auto now = std::chrono::steady_clock::now();
    float best = cone;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || !Target(who)) continue;
        const float* at = &G<float>(who + kCharPosition);
        float point[3] = {at[0], at[1] + Height() * share, at[2]}, clip[4];
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
    bool firing = now - fired < kFireHold;
    Turn turn;
    float pull = 0;
    if (Opt().aimbot && aiming) {
        turn = Nearest(kCone, kHeadShare);
        pull = kPull;
    } else if (level && (aiming || firing)) {
        turn = Nearest(kAssistCone[level], kChestShare);
        pull = kAssistPull[level];
    }
    if (!turn.found) return;
    G<float>(c + kControllerYaw) += turn.yaw * pull;
    G<float>(c + kControllerPitch) += turn.pitch * pull;
}

// D3DVIEWPORT8: x, y, width, height. Called between the 2D layer's begin and end
void DrawEsp(X86Context& ctx, float unit) {
    if (!Opt().esp || !g_camera || !R32(kWorld)) return;
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
    float on[4], off[4];
    Unpack(Opt().esp_on_color, on);
    Unpack(Opt().esp_off_color, off);
    const float back[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    float width = kThickness * unit, bar = kBarThickness * unit;
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
            // out of the view: an arrow at the edge towards it, flipped when it is behind the camera
            float dx = front ? clip[0] : -clip[0], dy = front ? -clip[1] : clip[1];
            if (!std::isfinite(dx) || !std::isfinite(dy)) continue;
            if (std::fabs(dx) + std::fabs(dy) < 1e-4f) dx = 0, dy = 1;
            float cx = vx + vw * 0.5f, cy = vy + vh * 0.5f;
            float rx = std::max(vw * 0.5f - kEdge, 1.0f), ry = std::max(vh * 0.5f - kEdge, 1.0f);
            float scale = 1.0f / std::max(std::fabs(dx) / rx, std::fabs(dy) / ry);
            float ex = cx + dx * scale, ey = cy + dy * scale;
            float length = std::sqrt(dx * dx + dy * dy), ux = dx / length, uy = dy / length;
            float head = kArrow * unit;
            add(cx + dx * scale * 0.78f, cy + dy * scale * 0.78f, ex, ey, width, off);
            float bx = ex - ux * head, by = ey - uy * head;
            add(ex, ey, bx - uy * head * 0.5f, by + ux * head * 0.5f, width, off);
            add(ex, ey, bx + uy * head * 0.5f, by - ux * head * 0.5f, width, off);
            continue;
        }
        float feet[3] = {at[0], at[1], at[2]};
        float top[3] = {at[0], at[1] + height, at[2]};
        float f[2], h[2];
        if (!Project(feet, vx, vy, vw, vh, f) || !Project(top, vx, vy, vw, vh, h)) continue;
        float half = (f[1] - h[1]) * kBoxWidth * 0.5f;
        float left = f[0] - half, right = f[0] + half;
        add(vx + vw * 0.5f, vy + vh, f[0], f[1], width, on);
        add(left, h[1], right, h[1], width, on);
        add(left, f[1], right, f[1], width, on);
        add(left, h[1], left, f[1], width, on);
        add(right, h[1], right, f[1], width, on);
        if (Opt().esp_health) {
            float frac = entry.peak > 0 ? std::clamp(G<float>(who + kCharHealth) / entry.peak, 0.0f, 1.0f) : 1.0f;
            float y = h[1] - kBarGap * unit - bar * 0.5f;
            const float fill[4] = {std::min(1.0f, 2 * (1 - frac)), std::min(1.0f, 2 * frac), 0.0f, 1.0f};
            add(left, y, right, y, bar, back);
            if (frac > 0) add(left, y, left + (right - left) * frac, y, bar, fill);
        }
    }
    g_count = kept;
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
    if ((game::Opt().esp || game::Opt().aimbot || game::Opt().aim_assist) && who && R32(who + kCharKind) == kEnemyKind &&
        !R32(who + kCharDying))
        Remember(who);
    __imp__sub_001DFD90(ctx, base);
}
