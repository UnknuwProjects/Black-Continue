// Hooks for default.xbe
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <vector>
#include <thread>

#include "app/app.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/input.h"
#include "game/layout.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace layout;

namespace d3d {
void ViewScale(float xy);
void GunDraw(bool on, const float move[4]);
void GunScale(float& sx, float& sy);  // d3d8.cpp: the projection's scale the gun was last drawn with
}

namespace game {
void EspCamera();  // esp.cpp
}

namespace {

constexpr uint32_t kDevice = SYM(D3D_Device);
constexpr uint32_t kInitDefaultState = 0x271710;

void WriteFloat(uint32_t va, float f) { std::memcpy(HostPtr(va), &f, 4); }

// what the library's vertical blank interrupt does that the game sees: count, event, callback
void VerticalBlank(X86Context& ctx) {
    uint32_t info = mem::AllocSystem(12);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        // the film decoder paces on the count: it stands still through a focus pause
        while (game::g_focus_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            next = std::chrono::steady_clock::now();
        }
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        uint32_t count = R32(kDevice + XDK_CDevice_VBlankCount) + 1;
        W32(kDevice + XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = R32(kDevice + XDK_CDevice_VBlankCallback)) {
            W32(info, count);
            W32(info + 4, R32(kDevice + XDK_CDevice_SwapCount));
            W32(info + 8, 0);
            CallGuest(ctx, callback, {info});
        }
        // the game clears the event itself before each wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

// the viewport as D3DDevice_SetViewport stores it, for the game's own readers
void StoreViewport(X86Context&, uint8_t*) {
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    const gpu::Viewport& v = d3d::State().viewport;
    uint32_t p = kDevice + XDK_CDevice_Viewport;
    W32(p, v.x);
    W32(p + 4, v.y);
    W32(p + 8, v.w);
    W32(p + 12, v.h);
    WriteFloat(p + 16, v.min_z);
    WriteFloat(p + 20, v.max_z);
}

// D3DDevice_GetViewportOffsetAndScale
X86_EXTERN_FUNC(__imp__sub_0026E9C0);
X86_FUNC(sub_0026E9C0) {
    StoreViewport(ctx, base);
    __imp__sub_0026E9C0(ctx, base);
}

// device fields the library's init sets and the game or its inlined library code reads, then the default states
void AfterCreateDevice(X86Context& ctx, uint8_t* base) {
    if (ctx.eax) return;
    W32(kDevice + XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
    W32(kDevice + XDK_CDevice_AutoDepth, R32(kDevice + XDK_CDevice_Depth));
    WriteFloat(kDevice + XDK_CDevice_BackBufferScale, 1.0f);
    WriteFloat(kDevice + XDK_CDevice_BackBufferScale + 4, 1.0f);
    W32(kDevice + XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));  // 60 Hz, widescreen
    W32(kDevice + XDK_CDevice_VBlankEvent, 0x00040000);  // notification event
    W32(kDevice + XDK_CDevice_VBlankEvent + 4, 1);
    StoreViewport(ctx, base);
    uint32_t ecx = ctx.ecx;
    uint32_t edx = ctx.edx;
    CallGuest(ctx, kInitDefaultState, {});
    ctx.eax = 0;
    ctx.ecx = ecx;
    ctx.edx = edx;
    static bool started = false;
    if (!started) StartHostThread("vertical blank", VerticalBlank);
    started = true;
}

constexpr uint32_t kFrameCount = 0x2F4368;

// the game speed cheat: the game's steps take this share of the real time
static float Speed() { return float(std::clamp(game::Opt().game_speed, 25u, 200u)) / 100.0f; }

// seconds the last frame took, the frame time the game was made for at most, at the game speed
static float FrameTime() {
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    static uint32_t frame;
    static float dt = G<float>(kGameFrameTime);
    if (uint32_t f = R32(kFrameCount); f != frame) {
        auto now = clock::now();
        dt = std::min(std::chrono::duration<float>(now - last).count(), G<float>(kGameFrameTime));
        frame = f;
        last = now;
    }
    return dt * Speed();
}

// The game's timers step one fixed frame time a frame: they step the real one, so any frame rate plays at speed
X86_EXTERN_FUNC(__imp__sub_000BE3F0);
X86_FUNC(sub_000BE3F0) {
    uint32_t t = ctx.ecx;
    auto steps = [t] {
        double parts = double(R32(t + kTimerPartSteps)) / std::max(R32(t + kTimerDivisor), 1u);
        return R32(t + kTimerWholeSteps) + parts;
    };
    double before = steps();
    float time = G<float>(t + kTimerTime);
    __imp__sub_000BE3F0(ctx, base);
    float dt = FrameTime();
    G<float>(t + kTimerDt) = dt / std::max(R32(t + kTimerDivisor), 1u);
    G<float>(t + kTimerTime) = time + float((steps() - before) * dt);
}

// frame throttle: sleeps that gave the console's one CPU to the loader
X86_FUNC(sub_001F9130) { Return(ctx, ctx.eax, 1); }

// The Flash menus and HUD tick in milliseconds, 33 a call: the real ones, the part of one carried over
void WeaponsTick(X86Context& ctx);  // weapons.cpp: the weapons page's requests
void MenuTick(X86Context& ctx, uint8_t*) {
    WeaponsTick(ctx);
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    static float carry;
    auto now = clock::now();
    carry += std::min(std::chrono::duration<float>(now - last).count(), G<float>(kGameFrameTime)) * 1000.0f;
    last = now;
    ctx.eax = uint32_t(carry);
    carry -= float(ctx.eax);
}

// what a step made once a frame at 30 becomes over the real frame: factor to the power of this
static float FrameShare() { return FrameTime() / G<float>(kGameFrameTime); }

static uint32_t Player() { return R32(kWorld) + kWorldPlayer; }

// weapon and its data
constexpr uint32_t kWeaponData = 0xEC;
constexpr uint32_t kWeaponOwner = 0xF0;
constexpr uint32_t kDataAimZoom = 0xB4;
// player
// the view's axes: right, up, forward and eye look like the rows of one matrix at 0xD0 (forward and eye are known,
// right and up are read from the rows before them and checked at run time)
constexpr uint32_t kPlayerRight = 0xD0;
constexpr uint32_t kPlayerUp = 0xE0;
constexpr uint32_t kPlayerForward = 0xF0;
constexpr uint32_t kPlayerEye = 0x100;
constexpr uint32_t kPlayerWeapon = 0x29C;
constexpr uint32_t kPlayerZoom = 0x2A4;
constexpr uint32_t kPlayerFireRequest = 0x32C;

// The zoom of the aim grows or shrinks by a fixed share a frame
void ZoomStep(X86Context& ctx, uint8_t*) {
    float old = G<float>(R32(ctx.ecx + kWeaponOwner) + kPlayerZoom);
    float now = ctx.xmm[0].f[0];
    if (now == old || old <= 0) return;
    bool out = !G<uint8_t>(ctx.ecx + 0x105) && old > 1;  // back towards 1
    ctx.xmm[0].f[0] = out ? 1 + (old - 1) * std::pow((now - 1) / (old - 1), FrameShare())
                          : old * std::pow(now / old, FrameShare());
}

constexpr float kHalf = 3.14159265f / 360.0f;

// tangent of a setting's half angle over that of the game's 70 degrees
static float Widen(uint32_t degrees) {
    return std::tan(std::clamp(degrees, 45u, 140u) * kHalf) / std::tan(70 * kHalf);
}

static float g_gun = 1.0f;  // the gun's picture over the view's

// ---- test log (ads_test.log): lines per site are capped, so the file stays small. Only writes, changes nothing ----
static long DbgMs() {
    static const auto start = std::chrono::steady_clock::now();
    return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}
static void DbgLog(const char* fmt, ...) {
    FILE* log = std::fopen("/storage/emulated/0/Black/SaveData/ads_test.log", "a");
    if (!log) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(log, fmt, args);
    va_end(args);
    std::fclose(log);
}
// what the animation hook (sub_0017B130) last saw of the player's weapon clip, read by the shell birth log
static char g_dbg_clip[16] = "-";
static float g_dbg_t14 = 0;
static int g_dbg_slot = -1;
static int g_dbg_fire_frames = 0;

// The gun at the aim: in the middle of the view. The game builds the first person gun's matrix once a frame
// (player + 0x420, in view units: x left on the screen, y up, z ahead, the last row its position) and the gun's model, the
// locators on it (muzzle flash, sparks, smoke) and the muzzle matrix of the tracers all start from it. So the move is
// added to that matrix at its source (ViewmodelShift) and everything follows alike. The shell casings are the one
// thing born from a locator of their own (ShellShift). ads_view.txt in SaveData, lines
//   TYPE dx dy dz yaw pitch shell
// (ALL for any type not named). dx, dy and dz in view units: the matrix's x axis points to the LEFT of the screen, so
// a positive dx moves the gun left, a positive dy up, a positive dz away from the eye. yaw and pitch in degrees turn
// the gun around a point ahead of the eye: a positive yaw turns its muzzle to the left, a positive pitch up. shell
// 1 (or left out) moves the casings with the gun, 0 leaves them, in between a part of it. All after dy may be left out. E.g. "ALL 0.10 0.03",
// "ASR 0.10 0.03 0 4 0"
struct AdsMove {
    float v[9] = {0, 0, 0, 0, 0, 1, 0, 1, 1};  // dx dy dz yaw pitch shell cam recoil kick
};
static std::map<std::string, AdsMove> g_ads_shifts;
static time_t g_ads_stamp = -1;  // the file's change time when it was read
static std::chrono::steady_clock::time_point g_ads_checked;
static float g_vm[5] = {};     // the move of the gun's matrix this frame: dx dy dz yaw pitch
static float g_shell = 0;      // 1: the casings go along
static float g_cam = 0;        // 1: the camera keeps its view at the aim, the gun comes to it instead
static float g_shift[4] = {};  // the old moves on the draws and the effects: left at nothing, the matrix has it now
static float g_effect = 0;

// Every weapon has its own key: its data block is one of the game's tables, 0x1E0 apart from the first at 0x8227D330, in
// the order weapons_diag.log prints ("weapons: table N = BG1_..."). The type code in the data (+0x130) is only the
// weapon's class (PST, SMG, ASR...), shared by several weapons. The suppressed variants share the data, so they have no
// key of their own: one row serves both.
static const char* const kTableNames[] = {"PST", "SHG", "SNR", "SMG", "ASR", "AK1", "RPG", "GRL", "SM3",
                                          "P90", "HVY", "MGN", "M16", "RM1", "GK1", "MP1", "BNS"};
constexpr uint32_t kTableBase = 0x8227D330;
constexpr uint32_t kTableStride = 0x1E0;
constexpr uint32_t kTableCount = sizeof(kTableNames) / sizeof(kTableNames[0]);

static const char* TableName(uint32_t data) {
    if (data < kTableBase || (data - kTableBase) % kTableStride) return nullptr;
    uint32_t index = (data - kTableBase) / kTableStride;
    return index < kTableCount ? kTableNames[index] : nullptr;
}

static void LoadAdsShifts(uint32_t data, const char* type) {
    g_ads_shifts.clear();
    static bool tagged = false;
    if (!tagged) {
        tagged = true;
        DbgLog("build: dbg2 (shell birth, fire start/end, own aim per frame)\n");
    }
    std::ifstream f("/storage/emulated/0/Black/SaveData/ads_view.txt");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string name;
        AdsMove m;
        if (!(in >> name >> m.v[0] >> m.v[1])) continue;
        for (int i = 2; i < 9; i++) {
            float t;
            if (!(in >> t)) break;
            m.v[i] = t;
        }
        g_ads_shifts[name] = m;
    }
    if (FILE* log = std::fopen("/storage/emulated/0/Black/SaveData/ads_test.log", "a")) {
        std::fprintf(log, "aim in: data=%08X type=%s, %u lines\n", unsigned(data), type, unsigned(g_ads_shifts.size()));
        std::fclose(log);
    }
}

// The weapon's row of ads_view.txt: its own, then its class's, then ALL. The file is looked at twice a second: saved
// again, it is read again, no new level needed. Used at the aim and by the recoil
static const AdsMove* AdsRow(uint32_t data) {
    if (!data) return nullptr;
    char type[4] = {};
    std::memcpy(type, HostPtr(data + 0x130), 3);  // the type code, "ASR", "SMG", "PST"...
    auto now = std::chrono::steady_clock::now();
    if (g_ads_stamp == -1 || now - g_ads_checked > std::chrono::milliseconds(500)) {
        g_ads_checked = now;
        struct stat st;
        time_t stamp = stat("/storage/emulated/0/Black/SaveData/ads_view.txt", &st) == 0 ? st.st_mtime : 0;
        if (stamp != g_ads_stamp) {
            g_ads_stamp = stamp;
            LoadAdsShifts(data, type);
        }
    }
    const char* name = TableName(data);
    const char* used = name;
    auto it = name ? g_ads_shifts.find(name) : g_ads_shifts.end();
    if (it == g_ads_shifts.end()) {
        used = type;
        it = g_ads_shifts.find(type);
    }
    if (it == g_ads_shifts.end()) {
        used = "ALL";
        it = g_ads_shifts.find("ALL");
    }
    if (it == g_ads_shifts.end()) used = "none";
    static uint32_t logged_data = 0;
    static std::string logged_used;
    if (data != logged_data || logged_used != used) {
        logged_data = data;
        logged_used = used;
        if (FILE* log = std::fopen("/storage/emulated/0/Black/SaveData/ads_test.log", "a")) {
            std::fprintf(log, "aim row: data=%08X name=%s class=%s -> row %s\n", unsigned(data), name ? name : "?", type,
                         used);
            std::fclose(log);
        }
    }
    return it == g_ads_shifts.end() ? nullptr : &it->second;
}

static void AdsShift(uint32_t data, float hip) {
    float blend = 1 - hip;
    for (float& v : g_vm) v = 0;
    g_shell = 0;
    g_cam = 0;
    if (!data || blend <= 0) return;
    const AdsMove* row = AdsRow(data);
    if (!row) return;
    for (int i = 0; i < 5; i++) g_vm[i] = row->v[i] * blend;
    g_shell = row->v[5];
    g_cam = row->v[6];
}

// The first person gun's matrix is done (ebx the player, 0x1E6757, right before the models are placed with it): the
// aim's turn and move are added to it, which the next frame builds again from the weapon's data. The turn is about a
// point kPivot ahead of the eye, so the gun's sights stay about where they were and dx, dy only trim them
void ViewmodelShift(X86Context& ctx, uint8_t*) {
    if (ctx.ebx != Player()) return;
    bool moved = false;
    for (float v : g_vm) moved = moved || v != 0;
    if (!moved) return;
    float* m = &G<float>(ctx.ebx + 0x420);  // four rows of four, the last one the position
    constexpr float kPi = 3.14159265f;
    constexpr float kPivot = 0.6f;
    float yaw = g_vm[3] * kPi / 180, pitch = g_vm[4] * kPi / 180;
    if (yaw != 0 || pitch != 0) {
        float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        for (int r = 0; r < 4; r++) {
            float* v = m + r * 4;
            if (r == 3) v[2] -= kPivot;
            float x = v[0] * cy + v[2] * sy;
            float z = -v[0] * sy + v[2] * cy;
            float y = v[1] * cp + z * sp;
            z = -v[1] * sp + z * cp;
            v[0] = x;
            v[1] = y;
            v[2] = z;
            if (r == 3) v[2] += kPivot;
        }
    }
    m[12] += g_vm[0];
    m[13] += g_vm[1];
    m[14] += g_vm[2];
}

// A shell casing is born (0x163655, ebx its record in the pool, the function's first argument the player). It was
// made from the gun's locator before the aim's move reached it. Its position is small numbers (about -0.07 -0.08 0.25
// in the log, the eye is far off in the world), so it is in the gun's own space, not the world's: the aim's turn and
// move are added to it as they are to the gun's matrix, not along the camera's axes. shell in ads_view.txt scales
// the move and the turn: 1 whole, 0 none. The first ones are written to ads_test.log
void ShellShift(X86Context& ctx, uint8_t*) {
    uint32_t player = R32(ctx.ebp + 8);
    {   // every birth of the player's shell, before anything is changed: how many a shot makes, from where, in which clip
        static int births = 0;
        if (births < 300 && player == Player()) {
            births++;
            uint32_t weapon = R32(Player() + kPlayerWeapon);
            const char* wname = weapon ? TableName(R32(weapon + kWeaponData)) : nullptr;
            const float* p0 = &G<float>(ctx.ebx + 0x40);
            DbgLog("shell birth #%d ms=%ld rec=%08X ret=%08X weapon=%s clip=%s slot=%d t14=%.2f ffr=%d g_shell=%.2f "
                   "vm=(%.3f %.3f %.3f) pos=(%.3f %.3f %.3f)\n",
                   births, DbgMs(), unsigned(ctx.ebx), unsigned(R32(ctx.ebp + 4)), wname ? wname : "?", g_dbg_clip,
                   g_dbg_slot, g_dbg_t14, g_dbg_fire_frames, g_shell, g_vm[0], g_vm[1], g_vm[2], p0[0], p0[1], p0[2]);
        }
    }
    static int calls = 0;
    if (calls < 4) {  // does the hook run at all, and with what: to ads_test.log
        calls++;
        if (FILE* log = std::fopen("/storage/emulated/0/Black/SaveData/ads_test.log", "a")) {
            std::fprintf(log, "shell call: arg=%08X player=%08X g_shell=%.2f move=%.3f %.3f %.3f\n", unsigned(player),
                         unsigned(Player()), g_shell, g_vm[0], g_vm[1], g_vm[2]);
            std::fclose(log);
        }
    }
    if (player != Player() || g_shell == 0 || (!g_vm[0] && !g_vm[1] && !g_vm[2] && !g_vm[3] && !g_vm[4])) return;
    float* pos = &G<float>(ctx.ebx + 0x40);
    float before[3] = {pos[0], pos[1], pos[2]};
    constexpr float kPi = 3.14159265f;
    constexpr float kPivot = 0.6f;
    float yaw = g_vm[3] * g_shell * kPi / 180, pitch = g_vm[4] * g_shell * kPi / 180;
    if (yaw != 0 || pitch != 0) {
        float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        float z0 = pos[2] - kPivot;
        float x = pos[0] * cy + z0 * sy;
        float z = -pos[0] * sy + z0 * cy;
        float y = pos[1] * cp + z * sp;
        z = -pos[1] * sp + z * cp;
        pos[0] = x;
        pos[1] = y;
        pos[2] = z + kPivot;
    }
    for (int i = 0; i < 3; i++) pos[i] += g_shell * g_vm[i];
    static int logged = 0;
    if (logged < 6) {
        logged++;
        if (FILE* log = std::fopen("/storage/emulated/0/Black/SaveData/ads_test.log", "a")) {
            std::fprintf(log, "shell: pos=(%.3f %.3f %.3f) -> (%.3f %.3f %.3f) move=(%.3f %.3f %.3f) turn=(%.2f %.2f)\n",
                         before[0], before[1], before[2], pos[0], pos[1], pos[2], g_vm[0], g_vm[1], g_vm[2], g_vm[3],
                         g_vm[4]);
            std::fclose(log);
        }
    }
}

// Is the gun's picture anywhere but where the game puts it: grown by the viewmodel setting or moved by the aim
static bool GunMoved() {
    return g_gun != 1 || (g_effect != 0 && (g_shift[0] || g_shift[1] || g_shift[2] || g_shift[3]));
}

// The pad as bits: the buttons, then the analog buttons (triggers...) past a light press, any pad
static uint32_t PadBits() {
    uint32_t bits = 0;
    for (int port = 0; port < 4; port++) {
        input::Pad pad = input::Poll(port);
        bits |= uint32_t(pad.buttons) & 0xFFFF;
        for (size_t i = 0; i < sizeof(pad.analog) / sizeof(pad.analog[0]) && i < 16; i++)
            if (pad.analog[i] > 30) bits |= 1u << (16 + i);
    }
    return bits;
}

// The weapons without zoom never get the game's aim byte, so the aim is read from the pad. Which bit of it is the aim is
// learned from the weapons that have one: the bit that is down when the game's aim byte is set and up when it is not.
// Kept in ads_aim.txt, a number, delete the file to learn again
static uint32_t g_aim_bit = 0;
static const char* const kAimFile = "/storage/emulated/0/Black/SaveData/ads_aim.txt";

static void LearnAim(bool held) {
    static int held_frames = 0, free_frames = 0, held_with[32] = {}, free_with[32] = {};
    if (g_aim_bit) return;
    uint32_t bits = PadBits();
    (held ? held_frames : free_frames)++;
    for (int b = 0; b < 32; b++)
        if (bits >> b & 1) (held ? held_with : free_with)[b]++;
    if (held_frames < 30 || free_frames < 30) return;
    int best = -1;
    float best_score = 0.8f;
    for (int b = 0; b < 32; b++) {
        float score = float(held_with[b]) / held_frames - float(free_with[b]) / free_frames;
        if (score > best_score) best = b, best_score = score;
    }
    if (best < 0) return;
    g_aim_bit = 1u << best;
    if (FILE* f = std::fopen(kAimFile, "w")) {
        std::fprintf(f, "%u\n", unsigned(g_aim_bit));
        std::fclose(f);
    }
    if (FILE* log = std::fopen("/storage/emulated/0/Black/SaveData/ads_test.log", "a")) {
        std::fprintf(log, "aim learned: pad bit %d (score %.2f)\n", best, best_score);
        std::fclose(log);
    }
}

static bool AimHeld() {
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        unsigned v = 0;
        if (FILE* f = std::fopen(kAimFile, "r")) {
            if (std::fscanf(f, "%u", &v) == 1) g_aim_bit = v;
            std::fclose(f);
        }
    }
    // not learned (yet): the right stick click, the only bit that moved with the aim button on the SPAS 12 and Remington
    uint32_t bit = g_aim_bit ? g_aim_bit : 0x80;
    return (PadBits() & bit) != 0;
}

// The aim of the SPAS 12 / Remington as the game's other weapons take it. In MODE BIDIK NORMAL, or with HOLD TO AIM on, the button is passed as
// held and is followed as it is. In KETUK + TAHAN with HOLD TO AIM off the touch layer sends the game a short pulse per press (touch.cpp:
// LatchAim), so the aim is a toggle on each rising edge here too, or it would blink for the length of the pulse.
namespace input { bool AimPulseMode(); }  // touch.cpp
static bool g_own_prev = false, g_own_toggle = false;
static bool OwnAimHeld() {
    bool bit = AimHeld();
    if (!input::AimPulseMode()) {  // the level of the button: NORMAL, or HOLD TO AIM on
        g_own_prev = bit;
        g_own_toggle = false;
        return bit;
    }
    if (bit && !g_own_prev) g_own_toggle = !g_own_toggle;
    g_own_prev = bit;
    return g_own_toggle;
}

static float g_base_fov = 0;  // the game's own view at the hip, kept to hold the camera at the aim
static float g_own_aim = 0;   // the aim of the weapons the game gives none (SPAS 12, Remington): 0 hip .. 1 aimed
static std::atomic<float> g_own_aim_pub{0};  // the same, for gyro.cpp (another thread)
// SPAS 12 / Remington: how much the view closes in at the aim, as a weapon with a zoom of its own (the pistol's) does.
// A guess until ads_test.log shows the pistol's value ("aim zoom PST=")
constexpr float kOwnAimZoom = 1.15f;
namespace game {
// true while the SPAS 12 / Remington aim is on its way in or in: gyro.cpp's WHILE AIMING reads it
bool OwnAiming() { return g_own_aim_pub.load() > 0.5f; }
}

// The view of play, 70 over the zoom: the setting from the hip, the game's own once the aim is in.
// ecx the weapon, xmm0 its zoom, xmm1 the view. Scripted cameras write theirs after this.
// cam in ads_view.txt: 1 holds the view at the hip's while aiming (the gun comes to the eye instead of the view going
// to the gun), in between a part of it. The SPAS 12 and the Remington have no zoom, the aim button is read here (the
// byte the zoom step reads too) and the aim made by hand, only the gun moves
void FieldOfView(X86Context& ctx, uint8_t*) {
    uint32_t data = R32(ctx.ecx + kWeaponData);
    bool mine = R32(ctx.ecx + kWeaponOwner) == Player();
    float aim = data ? G<float>(data + kDataAimZoom) : 1;
    float zoom = ctx.xmm[0].f[0];
    float& fov = ctx.xmm[1].f[0];
    float hip = aim > 1 ? std::clamp((aim - zoom) / (aim - 1), 0.0f, 1.0f) : 1.0f;
    const char* name = data ? TableName(data) : nullptr;
    bool own = mine && aim <= 1 && name && (!std::strcmp(name, "SHG") || !std::strcmp(name, "RM1"));
    if (own) {
        bool held = G<uint8_t>(ctx.ecx + 0x105) != 0 || OwnAimHeld();
        static int last = -1, logged = 0;
        static uint32_t last_bits = 0;
        uint32_t bits = PadBits();
        if ((last != int(held) || bits != last_bits) && logged < 40) {  // what the pad does at this weapon: to ads_test.log
            last = held;
            last_bits = bits;
            logged++;
            if (FILE* log = std::fopen("/storage/emulated/0/Black/SaveData/ads_test.log", "a")) {
                std::fprintf(log, "own aim: %s held=%d byte105=%u pad=%08X aim_bit=%08X\n", name, int(held),
                             unsigned(G<uint8_t>(ctx.ecx + 0x105)), unsigned(bits), unsigned(g_aim_bit));
                std::fclose(log);
            }
        }
        g_own_aim += ((held ? 1.0f : 0.0f) - g_own_aim) * (1 - std::pow(0.75f, FrameShare()));
        if (g_own_aim > 0.999f) g_own_aim = 1;
        if (g_own_aim < 0.001f) g_own_aim = 0;
        g_own_aim_pub = g_own_aim;
        hip = 1 - g_own_aim;
        static int own_frames = 0;  // each call while the aim is held or on its way: how steady the button and the aim are
        if ((held || g_own_aim > 0) && own_frames < 800) {
            own_frames++;
            DbgLog("own frame #%d ms=%ld %s held=%d pad=%08X own_aim=%.3f\n", own_frames, DbgMs(), name, int(held),
                   unsigned(PadBits()), g_own_aim);
        }
    } else if (mine) {
        if (aim > 1) LearnAim(G<uint8_t>(ctx.ecx + 0x105) != 0);
        if (name) {  // each weapon's zoom once, to compare with kOwnAimZoom
            static std::map<std::string, bool> zoom_logged;
            if (!zoom_logged[name] && zoom_logged.size() <= 24) {
                zoom_logged[name] = true;
                DbgLog("aim zoom %s=%.3f\n", name, aim);
            }
        }
        if (g_own_aim != 0) {  // something else than the SPAS 12 / Remington sends the aim back to 0
            static int resets = 0;
            if (resets < 100) {
                resets++;
                DbgLog("own aim RESET ms=%ld by weapon=%s data=%08X aim=%.2f was=%.3f\n", DbgMs(), name ? name : "?",
                       unsigned(data), aim, g_own_aim);
            }
        }
        g_own_aim = 0;
        g_own_aim_pub = 0;
        g_own_toggle = false;  // another weapon: the next SPAS 12 / Remington starts at the hip
        if (zoom <= 1.001f) g_base_fov = fov;  // not zoomed: this is the view the camera keeps
    }
    if (mine) AdsShift(data, hip);
    if (own) fov /= 1 + (kOwnAimZoom - 1) * g_own_aim;  // the view closes in by degrees, as the game's 70 over the zoom
    float keep = 0;
    if (mine && !own && g_cam != 0 && zoom > 1.001f) {
        keep = std::clamp(g_cam, 0.0f, 1.0f);
        float base = g_base_fov > 0 ? g_base_fov : fov * zoom;  // before one is kept: the zoom taken out by degrees
        fov += (base - fov) * keep;
    }
    float shown = own ? 1.0f : hip + (1 - hip) * keep;  // how much of the hip's setting stays
    float view = 1 + (Widen(game::Opt().fov) - 1) * shown;
    g_gun = view / (1 + (Widen(game::Opt().viewmodel_fov) - 1) * shown);
    if (view != 1) fov = std::atan(std::tan(fov * kHalf) * view) / kHalf;
}

// The list of the first person models, gun, arms and muzzle mesh: drawn at the viewmodel setting
X86_EXTERN_FUNC(__imp__sub_00176F60);
X86_FUNC(sub_00176F60) {
    if (ctx.esi == R32(kDisplay) + kDisplayDrawLists + kAlphaDrawList * kDrawListSize) game::EspCamera();
    bool list = ctx.esi == R32(kDisplay) + kDisplayDrawLists + kGunDrawList * kDrawListSize;
    bool gun = g_gun != 1 && list;
    if (list) d3d::GunDraw(true, g_shift);
    if (gun) d3d::ViewScale(g_gun);
    __imp__sub_00176F60(ctx, base);
    if (list) d3d::GunDraw(false, g_shift);
    if (gun) d3d::ViewScale(1);
}

// What sits on the gun follows the aim's move too. depth is the point's distance ahead of the eye. The gun's own draws
// are moved in the shader by dx and dy in view units (the same at any depth) and by fx and fy of the screen
// (-1..1 across, so a point at depth d moves fx * d / scale in view units, the scale being the projection's the gun
// was drawn with). Here the same move is made in the world, along the view's right and up.
static void AdsOffset(float* v, float depth) {
    if (!g_effect || (!g_shift[0] && !g_shift[1] && !g_shift[2] && !g_shift[3])) return;
    const float* right = &G<float>(Player() + kPlayerRight);
    const float* up = &G<float>(Player() + kPlayerUp);
    const float* forward = &G<float>(Player() + kPlayerForward);
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    // the rows are read as the view's axes only if they are: unit, and square to each other and to forward
    bool axes = std::fabs(dot(right, right) - 1) < 0.02f && std::fabs(dot(up, up) - 1) < 0.02f &&
                std::fabs(dot(right, forward)) < 0.02f && std::fabs(dot(up, forward)) < 0.02f &&
                std::fabs(dot(right, up)) < 0.02f;
    static bool logged = false;
    if (!logged) {
        logged = true;
        if (FILE* log = std::fopen("/storage/emulated/0/Black/SaveData/ads_test.log", "a")) {
            std::fprintf(log, "effects: axes %s right=(%.3f %.3f %.3f) up=(%.3f %.3f %.3f) forward=(%.3f %.3f %.3f)\n",
                         axes ? "ok" : "NOT a view's, effects stay", right[0], right[1], right[2], up[0], up[1], up[2],
                         forward[0], forward[1], forward[2]);
            std::fclose(log);
        }
    }
    if (!axes) return;
    float sx = 0, sy = 0;
    d3d::GunScale(sx, sy);
    float dx = g_shift[0] + (sx > 0 ? g_shift[2] * depth / sx : 0);
    float dy = g_shift[1] + (sy > 0 ? g_shift[3] * depth / sy : 0);
    for (int i = 0; i < 3; i++) v[i] += g_effect * (dx * right[i] + dy * up[i]);
}

// What sits on the gun follows its picture: across the player's view a point or an axis grows by the same share,
// its depth stays
static void OnGun(uint32_t at, bool point) {
    float* v = &G<float>(at);
    const float* forward = &G<float>(Player() + kPlayerForward);
    const float* eye = &G<float>(Player() + kPlayerEye);
    float d[3];
    float depth = 0;
    for (int i = 0; i < 3; i++) {
        d[i] = v[i] - (point ? eye[i] : 0);
        depth += d[i] * forward[i];
    }
    for (int i = 0; i < 3; i++) v[i] += (g_gun - 1) * (d[i] - forward[i] * depth);
    if (point) AdsOffset(v, depth);
}

constexpr uint32_t kMatrixPosition = 48;  // the last of a matrix's four rows

// A locator's matrix moved onto the gun's picture, as a copy: the game's own stays where bullets start
static uint32_t Follow(uint32_t matrix, uint32_t owner, bool axes) {
    if (!GunMoved() || matrix == 0x5655C0 || owner != Player()) return matrix;
    static uint32_t copy = mem::AllocSystem(64, 16);
    std::memcpy(HostPtr(copy), HostPtr(matrix), 64);
    for (uint32_t row = axes ? 0 : kMatrixPosition; row < 64; row += 16) OnGun(copy + row, row == kMatrixPosition);
    return copy;
}

constexpr uint32_t kEffectOwner = 0x44;

// the muzzle flash sprite, drawn with the matrix
void FlashSprite(X86Context& ctx, uint8_t*) { ctx.eax = Follow(ctx.eax, Player(), true); }
// effects that follow a locator of their owner (edi)
void GunEffect(X86Context& ctx, uint8_t*) { ctx.eax = Follow(ctx.eax, ctx.edi, false); }
// effects that start at one
void GunEffectStart(X86Context& ctx, uint8_t*) { ctx.edx = Follow(ctx.edx, R32(ctx.ebx + kEffectOwner), false); }

// bullet and its shot
constexpr uint32_t kBulletShot = 0x70;
constexpr uint32_t kShotWeapon = 4;

// esi a bullet that missed, eax where its tracer starts
void TracerStart(X86Context& ctx, uint8_t*) {
    uint32_t shot = R32(ctx.esi + kBulletShot);
    uint32_t weapon = shot ? R32(shot + kShotWeapon) : 0;
    if (!GunMoved() || !weapon || R32(weapon + kWeaponOwner) != Player()) return;
    static uint32_t copy = mem::AllocSystem(16, 16);
    std::memcpy(HostPtr(copy), HostPtr(ctx.eax), 16);
    ctx.eax = copy;
    OnGun(copy, true);
}

constexpr uint32_t kFromTracerA = 0x1DBAAB;  // return addresses of the two tracers
constexpr uint32_t kFromTracerB = 0x1DC4D4;

// The player's muzzle matrix: moved for the two tracers that start from it
X86_EXTERN_FUNC(__imp__sub_001D2860);
X86_FUNC(sub_001D2860) {
    uint32_t from = R32(ctx.esp);
    uint32_t out = R32(ctx.esp + 4);
    __imp__sub_001D2860(ctx, base);
    if (GunMoved() && (from == kFromTracerA || from == kFromTracerB)) OnGun(out + kMatrixPosition, true);
}

// Recoil dies away by a factor a frame. ebx the player: the kick a shot leaves (0x1EFE58) is spent here before
// it reaches the view, the cheat makes it nothing
void RecoilDecay(X86Context& ctx, uint8_t*) {
    // recoil in ads_view.txt, a weapon's own: 1 the game's, 0 none, in between a part of it
    float kick = 1;
    if (ctx.ebx == Player()) {
        uint32_t weapon = R32(ctx.ebx + kPlayerWeapon);
        if (const AdsMove* row = weapon ? AdsRow(R32(weapon + kWeaponData)) : nullptr) kick = std::clamp(row->v[7], 0.0f, 1.0f);
    }
    bool none = game::Opt().no_recoil && ctx.ebx == Player();
    ctx.xmm[0].f[0] = none ? 0.0f : std::pow(ctx.xmm[0].f[0], FrameShare()) * kick;
}

constexpr uint32_t kDataSwayShares = 0x104;

// The gun trails the stick by two shares a frame, read from the weapon's data: scaled for the four blends
static uint32_t g_sway;
static float g_sway_share[2];
void SwayBegin(X86Context& ctx, uint8_t*) {
    g_sway = ctx.eax + kDataSwayShares;
    for (int i = 0; i < 2; i++) {
        float& k = G<float>(g_sway + i * 4);
        g_sway_share[i] = k;
        k = 1 - std::pow(1 - k, FrameShare());
    }
}
void SwayEnd(X86Context&, uint8_t*) {
    if (!g_sway) return;
    for (int i = 0; i < 2; i++) G<float>(g_sway + i * 4) = g_sway_share[i];
    g_sway = 0;
}

static float Dt() { return G<float>(R32(kWorld) + kTimerDt); }

// A wait set and tested once a frame lasts whole frames of the game's frame time: as many at any frame rate.
// rest is added, what the last wait ran over by
static float WholeFrames(float wait, float rest) {
    float step = G<float>(kGameFrameTime) / std::max(R32(R32(kWorld) + kTimerDivisor), 1u);
    if (!(Dt() < step)) return wait;
    int frames = 0;
    for (float left = wait; left > 0 && frames < 4096; left -= step) frames++;
    return std::max(frames, 1) * step + rest;
}

static bool g_idle_early;

// weapon
constexpr uint32_t kWeaponState = 0xD8;
constexpr uint32_t kWeaponWait = 0xDC;
constexpr uint32_t kWeaponAmmo = 0xF4;
constexpr uint32_t kWeaponAltAmmo = 0xF8;  // the grenades under the barrel
// ammo: a shot takes one from each (0x1EFBBB)
constexpr uint32_t kAmmoTotal = 0x14;
constexpr uint32_t kAmmoClip = 0x18;

// the cheat: what the player's weapon had before its update, put back where it went down. A reload moves rounds
// from the reserve to the magazine, the reserve is put back too
struct KeptAmmo {
    uint32_t ammo = 0;
    uint32_t total = 0;
    uint16_t clip = 0;
};

static KeptAmmo KeepAmmo(uint32_t weapon) {
    uint32_t ammo = R32(weapon + kWeaponAmmo);
    if (!game::Opt().infinite_ammo || !ammo || R32(weapon + kWeaponOwner) != Player()) return {};
    return {ammo, R32(ammo + kAmmoTotal), G<uint16_t>(ammo + kAmmoClip)};
}

// the rounds a weapon has left in all: a shot of either barrel takes one (0x1EFBC5, 0x1EFEED)
static uint32_t Rounds(uint32_t weapon) {
    uint32_t rounds = 0;
    for (uint32_t at : {kWeaponAmmo, kWeaponAltAmmo})
        if (uint32_t ammo = R32(weapon + at)) rounds += R32(ammo + kAmmoTotal);
    return rounds;
}

static void RestoreAmmo(uint32_t weapon, const KeptAmmo& kept) {
    if (!kept.ammo || R32(weapon + kWeaponAmmo) != kept.ammo) return;
    if (R32(kept.ammo + kAmmoTotal) < kept.total) W32(kept.ammo + kAmmoTotal, kept.total);
    if (G<uint16_t>(kept.ammo + kAmmoClip) < kept.clip) G<uint16_t>(kept.ammo + kAmmoClip) = kept.clip;
}

// the wait between a weapon's shots. The frame of a first shot is part of it: the fire clip runs that frame too
// and holds its next shot where the wait ends
X86_EXTERN_FUNC(__imp__sub_001EEFD0);
X86_FUNC(sub_001EEFD0) {
    uint32_t weapon = R32(ctx.esp + 4);
    float& wait = G<float>(weapon + kWeaponWait);
    float rest = wait;
    float dt = Dt();
    KeptAmmo kept = KeepAmmo(weapon);
    uint32_t rounds = Rounds(weapon);
    __imp__sub_001EEFD0(ctx, base);
    bool shot = Rounds(weapon) < rounds;
    RestoreAmmo(weapon, kept);
    uint32_t me = R32(weapon + kWeaponOwner);
    // the rapid fire cheat: a shot's wait, its fire mode's +0x20, is that many times shorter
    uint32_t faster = std::clamp(game::Opt().rapid_fire, 1u, 4u);
    if (shot && me == Player() && wait > 0) wait /= float(faster);
    if (me == Player()) {
        if (R32(weapon + kWeaponState) != 1 || wait != rest) g_idle_early = false;
        else if (g_idle_early) W32(me + kPlayerFireRequest, 0);  // the fire request is spent, as on the console
    }
    if (wait == rest) return;
    wait = WholeFrames(wait, rest <= 0 && rest > -dt ? rest : -dt);
}

// animation controller
constexpr uint32_t kAnimOwner = 0;
constexpr uint32_t kAnimClip = 0x54;
constexpr uint32_t kAnimName = 0x5C;
constexpr uint32_t kAnimFrom = 0x90;
// animation clip
constexpr uint32_t kAnimClipHash = 0x18;
constexpr uint32_t kAnimClipFades = 0x88;
constexpr uint32_t kFadeSize = 0x28;
constexpr uint32_t kAnimClipFadeSlot = 0x1C8;  // the current fade
constexpr uint32_t kFireLoopHash = 0x6D9D2AC0;

static bool IsFireClip(const std::string& name) {
    static const char* fire[] = {"S_100", "S_110", "S_114", "S_130", "S_170", "S_176"};
    bool fired = false;
    for (auto f : fire) fired |= name.size() >= 5 && name.compare(name.size() - 5, 5, f) == 0;
    return fired;
}

// the player's weapon has kick 0 in ads_view.txt (not the RPG and the Walther, left as the game has them)
static bool KickOff() {
    uint32_t weapon = R32(Player() + kPlayerWeapon);
    if (!weapon) return false;
    uint32_t data = R32(weapon + kWeaponData);
    const char* name = TableName(data);
    if (name && (!std::strcmp(name, "RPG") || !std::strcmp(name, "SNR"))) return false;
    const AdsMove* row = AdsRow(data);
    return row && row->v[8] <= 0;
}

// SPAS 12 / Remington at the aim: the idle clip stands where it was when the aim came in (the other weapons have none at
// the aim). EXPERIMENT: weapon state 1 is taken for ready, +0x14 / +0x18 for the clip's time: both unverified, see
// the "idle hold" lines of ads_test.log
static bool OwnIdleHold() {
    uint32_t weapon = R32(Player() + kPlayerWeapon);
    if (!weapon || R32(weapon + kWeaponState) != 0 || g_own_aim_pub.load() <= 0.5f) return false;
    const char* name = TableName(R32(weapon + kWeaponData));
    return name && (!std::strcmp(name, "SHG") || !std::strcmp(name, "RM1"));
}

// EXPERIMENT: which times of a fire clip kick 0 holds, read from kick_probe.txt beside ads_view.txt (one number, a sum:
// 1 = +0x14, 2 = +0x18, 4 = +0x20, 8 = +0x24; none or unreadable: 15, since the shells came one a frame with +0x14 held alone). Read again every 2 seconds or so
static float g_kick_pin = 0;  // the frame +0x14 and +0x18 are held at: the second number of kick_probe.txt
static uint32_t KickProbeMask() {
    static uint32_t mask = 15;
    static int tick = 0;
    if (tick++ % 120 == 0) {
        uint32_t now = 15;
        float pin = 0;
        if (FILE* f = std::fopen("/storage/emulated/0/Black/SaveData/kick_probe.txt", "r")) {
            unsigned v = 0;
            float p = 0;
            int n = std::fscanf(f, "%u %f", &v, &p);
            if (n >= 1) now = v & 15;
            if (n >= 2 && p >= 0 && p < 60) pin = p;
            std::fclose(f);
        }
        if (now != mask || pin != g_kick_pin) {
            mask = now;
            g_kick_pin = pin;
            DbgLog("kick probe mask=%u pin=%.2f\n", mask, pin);
        }
    }
    return mask;
}

// kick 0, v2 (UNVERIFIED hook point until tested): the pose of a clip is sampled by sub_00097E00 (VA 0x97E00), which reads the
// clip's time at +0x14 / +0x18. The animation events (shell, sound) are tested elsewhere, when the time advances (sub_00097C60 ->
// 0x97150), so only the sampler sees the held time and the events keep the real one. Times put back right after the sampler.
// kick_probe.txt: first number & 3 != 0 turns it on (15 = on, 12 = off), second number = the clip time the pose stands at.
static uint32_t g_kick_ac = 0;  // the player's animation controller, noted by sub_0017B130
X86_EXTERN_FUNC(__imp__sub_00097E00);
X86_FUNC(sub_00097E00) {
    uint32_t clip = R32(ctx.esp + 4);
    uint32_t ac = g_kick_ac;
    bool hold = clip && ac && R32(ac + kAnimClip) == clip && R32(ac + kAnimOwner) == Player() &&
                IsFireClip(ReadCString(ac + kAnimName, 13)) && KickOff() && (KickProbeMask() & 3);
    if (!hold) return __imp__sub_00097E00(ctx, base);
    float real14 = G<float>(clip + 0x14), real18 = G<float>(clip + 0x18);
    G<float>(clip + 0x14) = g_kick_pin;
    G<float>(clip + 0x18) = g_kick_pin;
    __imp__sub_00097E00(ctx, base);
    // what the sampler changed itself (a loop wrap) goes onto the real times
    G<float>(clip + 0x14) = real14 + (G<float>(clip + 0x14) - g_kick_pin);
    G<float>(clip + 0x18) = real18 + (G<float>(clip + 0x18) - g_kick_pin);
    static int logs = 0;
    if (logs < 30) {
        logs++;
        DbgLog("pose hold %s real t14=%.2f held at %.2f\n", ReadCString(ac + kAnimName, 13).c_str(), real14, g_kick_pin);
    }
}

// A fire clip that ends before the wait does hands over to the idle early: the request would start it again.
// An idle fading in over a looping fire clip holds it still instead of running it into its next shot
X86_EXTERN_FUNC(__imp__sub_0017B130);
X86_FUNC(sub_0017B130) {
    uint32_t ac = R32(ctx.esp + 4);
    uint32_t from = R32(ac + kAnimFrom);
    uint32_t clip = R32(ac + kAnimClip);
    if (R32(ac + kAnimOwner) != Player() || !clip) return __imp__sub_0017B130(ctx, base);
    int16_t slot = G<int16_t>(clip + kAnimClipFadeSlot);
    uint32_t fade = clip + kAnimClipFades + slot * kFadeSize;
    g_kick_ac = ac;  // kick 0 is held in sub_00097E00 now, the times of the clip are left alone here
    __imp__sub_0017B130(ctx, base);
    {
        uint32_t now_clip = R32(ac + kAnimClip);
        std::string clip_name = ReadCString(ac + kAnimName, 13);
        {   // every change of the player's clip, with the weapon's state and the aim: to find the idle clip
            static std::string last_name;
            static int change_logs = 0;
            bool spam = clip_name.size() >= 5 && (clip_name.compare(clip_name.size() - 5, 5, "S_100") == 0 ||
                                                  clip_name.compare(clip_name.size() - 5, 5, "S_190") == 0 ||
                                                  clip_name.compare(clip_name.size() - 5, 5, "S_110") == 0);
            if (clip_name != last_name && !spam && change_logs < 1500) {
                change_logs++;
                last_name = clip_name;
                uint32_t w = R32(Player() + kPlayerWeapon);
                const char* wn = w ? TableName(R32(w + kWeaponData)) : nullptr;
                DbgLog("clip %s weapon=%s state=%u own_aim=%.2f t14=%.2f len=%.1f\n", clip_name.c_str(), wn ? wn : "-",
                       w ? R32(w + kWeaponState) : 0u, g_own_aim_pub.load(), now_clip ? G<float>(now_clip + 0x14) : 0.f,
                       now_clip ? G<float>(now_clip + 0x1C) : 0.f);
            }
        }
        {   // one line when a fire clip starts and one when it ends (frames it lasted): a single shot too
            static bool in_fire = false;
            static uint32_t fire_clip = 0;
            static int fire_slot = -1, fire_frames = 0, fire_logs = 0;
            static float fire_last_t14 = 0;
            bool is_fire = now_clip && IsFireClip(clip_name);
            int now_slot = now_clip ? int(G<int16_t>(now_clip + kAnimClipFadeSlot)) : -1;
            if (in_fire && (!is_fire || now_clip != fire_clip || now_slot != fire_slot)) {
                if (fire_logs < 300) {
                    fire_logs++;
                    DbgLog("fire end ms=%ld frames=%d last_t14=%.2f\n", DbgMs(), fire_frames, fire_last_t14);
                }
                in_fire = false;
            }
            if (is_fire) {
                if (!in_fire) {
                    in_fire = true;
                    fire_clip = now_clip;
                    fire_slot = now_slot;
                    fire_frames = 0;
                    if (fire_logs < 300) {
                        fire_logs++;
                        DbgLog("fire start ms=%ld %s slot=%d len=%.1f kick0=%d aim=%d\n", DbgMs(), clip_name.c_str(),
                               now_slot, G<float>(now_clip + 0x1C), int(KickOff()),
                               int(g_vm[0] != 0 || g_vm[1] != 0 || g_vm[2] != 0));
                    }
                }
                fire_frames++;
                fire_last_t14 = G<float>(now_clip + 0x14);
            }
            std::snprintf(g_dbg_clip, sizeof(g_dbg_clip), "%s", is_fire ? clip_name.c_str() : "-");
            g_dbg_t14 = now_clip ? G<float>(now_clip + 0x14) : 0;
            g_dbg_slot = now_slot;
            g_dbg_fire_frames = in_fire ? fire_frames : 0;
        }
        if (now_clip && IsFireClip(clip_name)) {
            static std::map<std::string, int> seen;  // the first frames of each fire clip to ads_test.log
            int& n = seen[clip_name];
            if (n < 12 && seen.size() <= 24) {
                n++;
                if (FILE* log = std::fopen("/storage/emulated/0/Black/SaveData/ads_test.log", "a")) {
                    std::fprintf(log, "fire clip %s #%d slot=%d t14=%.2f t18=%.2f len=%.1f t20=%.3f t24=%.3f\n",
                                 clip_name.c_str(), n, int(G<int16_t>(now_clip + kAnimClipFadeSlot)),
                                 G<float>(now_clip + 0x14), G<float>(now_clip + 0x18), G<float>(now_clip + 0x1C),
                                 G<float>(now_clip + 0x20), G<float>(now_clip + 0x24));
                    std::fclose(log);
                }
            }
        }
        {   // idle hold at the aim of the SPAS 12 / Remington
            static uint32_t idle_clip = 0;
            static float idle_time[2];
            static int idle_logs = 0;
            bool idle_name = clip_name.size() >= 5 && clip_name.compare(clip_name.size() - 5, 5, "S_001") == 0;
            if (now_clip && idle_name && OwnIdleHold()) {
                if (idle_clip != now_clip) {
                    idle_clip = now_clip;
                    idle_time[0] = 0;  // the first frame: the hand on the gun
                    idle_time[1] = 0;
                    if (idle_logs < 40) {
                        idle_logs++;
                        DbgLog("idle hold %s t14=%.2f t18=%.2f len=%.1f\n", clip_name.c_str(), idle_time[0], idle_time[1],
                               G<float>(now_clip + 0x1C));
                    }
                }
                G<float>(now_clip + 0x14) = idle_time[0];
                G<float>(now_clip + 0x18) = idle_time[1];
            } else {
                idle_clip = 0;
            }
        }
    }
    if (G<int16_t>(clip + kAnimClipFadeSlot) == slot || FrameShare() >= 1) return;
    uint32_t weapon = R32(R32(ac + kAnimOwner) + kPlayerWeapon);
    if (weapon && R32(weapon + kWeaponState) == 1) {
        if (!IsFireClip(ReadCString(ac + kAnimName, 13))) g_idle_early = true;
    }
    // from +0x10 3 and its hash: the looping fire clip
    bool fire = from && R32(from + 0x10) == 3 && R32(from + kAnimClipHash) == kFireLoopHash;
    if (fire && G<uint16_t>(fade + 0x24)) W32(fade + 0x18, 2);
}

// how long a shot holds an enemy's gun sound: past the wait
X86_EXTERN_FUNC(__imp__sub_0013FC20);
X86_FUNC(sub_0013FC20) {
    float& hold = G<float>(ctx.esp + 8);
    hold = WholeFrames(hold, Dt());
    __imp__sub_0013FC20(ctx, base);
}

// physics [kPhysics]: +0 the solver
constexpr uint32_t kPhysics = 0x2D45F4;
constexpr uint32_t kSolverWorld = 0x18;
// physics world
constexpr uint32_t kPhysicsBodyEnd = 0x270;
constexpr uint32_t kPhysicsBodies = 0x29C;
constexpr uint32_t kPhysicsGravity = 0x360;
constexpr uint32_t kPhysicsGravityStep = 0x370;  // gravity a step squared
constexpr uint32_t kPhysicsStep = 0x380;
constexpr uint32_t kPhysicsRate = 0x384;
constexpr uint32_t kPhysicsRestLimit = 0x390;
// body
constexpr uint32_t kBodyNext = 0x2C;
constexpr uint32_t kBodyImpulse = 0x90;

// bodies stepped by the old step move by the new one
static void ScaleBodies(uint32_t world, float by) {
    static const uint32_t kMove[] = {0x20, 0x24, 0x28, 0x30, 0x34, 0x38, 0xA0, 0xA4, 0xA8};
    uint32_t body = R32(world + kPhysicsBodies);
    for (int left = 4096; body && body != world + kPhysicsBodyEnd && left; left--, body = R32(body + kBodyNext)) {
        for (uint32_t at : kMove) G<float>(body + at) *= by;
        // gravity a step squared, an impulse on top of it a step
        for (int i = 0; i < 12; i += 4) {
            float g = G<float>(world + kPhysicsGravityStep + i);
            float& impulse = G<float>(body + kBodyImpulse + i);
            impulse = (impulse - g) * by + g * by * by;
        }
        G<float>(body + 0x9C) *= by * by;
    }
}

// The physics world steps once a frame by the frame time of the level's start and bodies hold their move a step:
// both follow the real frame time, before the characters write theirs
static bool g_rest_tick;
void PhysicsTime(X86Context&, uint8_t*) {
    uint32_t physics = R32(kPhysics);
    uint32_t solver = physics ? R32(physics) : 0;
    uint32_t world = solver ? R32(solver + kSolverWorld) : 0;
    float dt = FrameTime();
    float share = FrameShare();
    if (!world || !(dt > 0)) return;
    float& step = G<float>(world + kPhysicsStep);
    if (step > 0 && step != dt) ScaleBodies(world, dt / step);
    step = dt;
    G<float>(world + kPhysicsRate) = 1 / dt;
    for (int i = 0; i < 16; i += 4) {
        G<float>(world + kPhysicsGravityStep + i) = G<float>(world + kPhysicsGravity + i) * dt * dt;
    }
    // a body rests once its move a step stays under a limit for a count of steps: the limit shrinks with the
    // step, the count runs at the game's frame rate
    static float limit;
    static float written;
    static float ticks;
    float& now = G<float>(world + kPhysicsRestLimit);
    if (now != written) limit = now;
    now = written = limit * share * share;
    ticks += share;
    g_rest_tick = ticks >= 1;
    if (g_rest_tick) ticks -= 1;
}
bool BodyRestTick(X86Context&, uint8_t*) { return g_rest_tick; }

// a body's damping is a factor a step, its top speed a move a step
void BodyDamping(X86Context& ctx, uint8_t*) {
    for (int i = 0; i < 2; i++)
        if (float& keep = ctx.xmm[i].f[0]; keep > 0) keep = std::pow(keep, FrameShare());
}
void BodyTopSpeed(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] *= FrameShare(); }

// a joint lets its bodies turn apart by at most a set angle a step
void JointTurnCap(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] *= FrameShare(); }
void JointTurnCapNext(X86Context& ctx, uint8_t*) { ctx.xmm[6].f[0] *= FrameShare(); }

// contacts are searched this far ahead at least: twice the fall of a step at the game's frame rate
void ContactReach(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] = 1 / G<float>(kGameFrameTime); }

// a ragdoll brakes for a wall within 8 frames of its speed: frames of the game's frame rate
void RagdollLookAhead(X86Context& ctx, uint8_t*) {
    if (float share = FrameShare(); share > 0) ctx.xmm[0].f[0] /= share;
}

// A joint's error, its anchors apart or a turn past its limit or lock, is put right within one step and that push
// stays on as speed: put right within a frame of the game's frame rate instead.
// xmm1 the gap of the anchors after the step, xmm2 the part of it the bodies' own motion makes
void JointAnchorGap(X86Context& ctx, uint8_t*) {
    float share = FrameShare();
    for (int i = 0; i < 4; i++) {
        float motion = ctx.xmm[2].f[i];
        ctx.xmm[1].f[i] = motion + (ctx.xmm[1].f[i] - motion) * share;
    }
}

// the turn a step may make on each of three axes, from [esp+0x60] to [esp+0x80]: a lock, or a limit already
// passed, asks for a turn back
constexpr uint32_t kJointTurnLow = 0x60;
constexpr uint32_t kJointTurnHigh = 0x80;
void JointTurnBack(X86Context& ctx, uint8_t*) {
    float share = FrameShare();
    for (uint32_t axis = 0; axis < 12; axis += 4) {
        float& low = G<float>(ctx.esp + kJointTurnLow + axis);
        float& high = G<float>(ctx.esp + kJointTurnHigh + axis);
        bool locked = low == high;
        if (locked || high < 0) high *= share;
        if (locked || low > 0) low *= share;
    }
}

constexpr uint32_t kShardAngle = 0xF4;

// Glass shards (0x1690D0, 0x110 bytes each) move, turn and count down once a frame of the game's frame rate
static bool g_shard_tick;
X86_EXTERN_FUNC(__imp__sub_001690D0);
X86_FUNC(sub_001690D0) {
    static float ticks;
    ticks += FrameShare();
    g_shard_tick = ticks >= 1;
    if (g_shard_tick) ticks -= 1;
    __imp__sub_001690D0(ctx, base);
}
bool ShardTick(X86Context&, uint8_t*) { return g_shard_tick; }

// xmm1 the gain of a shard's turn a frame
void ShardSpinGain(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] *= FrameShare(); }

// xmm1 the new turn a frame: the angle takes a share of it
void ShardTurn(X86Context& ctx, uint8_t*) {
    ctx.xmm[0].f[0] = G<float>(ctx.ebx + kShardAngle) + ctx.xmm[1].f[0] * FrameShare();
}

// xmm0 the gain of a shard's move a frame, then the new move
void ShardStep(X86Context& ctx, uint8_t*) {
    float share = FrameShare();
    for (int i = 0; i < 4; i++) ctx.xmm[0].f[i] *= share;
}

// LOD and draw distances reach this many times farther: each compare sees a distance that much shorter or a
// limit that much longer. Characters, props, pickups, ground cover, particles and bullet holes
static float Reach() { return float(std::clamp(game::Opt().draw_distance, 1u, 8u)); }
void DistanceShorter(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] /= Reach(); }
void DistanceShorterSquared(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] /= Reach() * Reach(); }
void LimitLonger(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] *= Reach(); }
void CharacterDetail(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] /= Reach(); }
void PropLimit(X86Context& ctx, uint8_t*) { ctx.xmm[2].f[0] *= Reach() * Reach(); }
constexpr uint32_t kParticleFarCut = 0xC4;
constexpr uint32_t kCameraFarCut = 0x88;

// edi a particle type, xmm1 the view's scale, xmm2 its far cut under the camera's (edx): the fade before
// it starts as much farther as the cut got
void ParticleFade(X86Context& ctx, uint8_t*) {
    float cut = std::min(G<float>(ctx.edi + kParticleFarCut) * ctx.xmm[1].f[0], G<float>(ctx.edx + kCameraFarCut));
    if (Reach() > 1 && cut > 0) ctx.xmm[3].f[0] *= ctx.xmm[2].f[0] / cut;
}

// ground cover and its patches
constexpr uint32_t kCoverPatches = 0x28;
constexpr uint32_t kPatchCount = 63;
constexpr uint32_t kPatchSize = 16;
constexpr uint32_t kPatchDistance = 4;

// ebx the ground cover, 63 patches a frame: full, a nearer patch replaces the farthest
void GroundCoverFull(X86Context& ctx, uint8_t*) {
    if (ctx.eax < kPatchCount || game::Opt().draw_distance <= 1) return;
    uint32_t list = ctx.ebx + kCoverPatches;
    uint32_t last = list;
    for (uint32_t at = list + kPatchSize; at < list + kPatchCount * kPatchSize; at += kPatchSize)
        if (G<float>(at + kPatchDistance) > G<float>(last + kPatchDistance)) last = at;
    if (!(ctx.xmm[2].f[0] < G<float>(last + kPatchDistance))) return;
    W32(last, R32(ctx.ebp + 8));
    G<float>(last + kPatchDistance) = ctx.xmm[2].f[0];
    G<float>(last + 8) = ctx.xmm[1].f[0];
    G<uint8_t>(last + 0xC) = uint8_t(ctx.ecx);
    G<uint8_t>(last + 0xD) = ctx.edx == 2;
}

// draw lists 0, 2 and 3 hold every mesh in reach: a full list drops the rest
void DrawListRoom(X86Context&, uint8_t*) {
    static const uint32_t caps[] = {4096, 0, 2048, 512};
    static uint32_t room[4];
    uint32_t list = R32(kDisplay) + kDisplayDrawLists;
    for (int i = 0; i < 4; i++, list += kDrawListSize) {
        if (!caps[i]) continue;
        if (!room[i]) room[i] = mem::AllocSystem(caps[i] * 28);
        W32(list + kDrawListRoom, room[i]);
        if (R32(list + 4)) W32(list + 4, room[i] + caps[i] * 20);
        W32(list + kDrawListCap, caps[i]);
    }
}

// reload animations start the screen blur
bool ReloadBlur(X86Context&, uint8_t*) { return game::Opt().reload_blur; }

// the blur holds for the animation's frames times the last frame's time: frames of the game's frame rate instead
constexpr uint32_t kAnimationFrames = 0x1C;
void ReloadBlurHold(X86Context& ctx, uint8_t*) {
    float frames = G<float>(ctx.ecx + kAnimationFrames);
    ctx.xmm[0].f[0] = frames * G<float>(kGameFrameTime);
}

// A or Start on any pad, or the left mouse button, on the press
static bool SkipPressed() {
    static bool held = true;
    bool down = input::LeftButton();
    for (int port = 0; port < 4; port++) {
        input::Pad pad = input::Poll(port);
        down |= pad.analog[0] || (pad.buttons & 0x10);
    }
    bool pressed = down && !held;
    held = down;
    return pressed;
}

// film player: +0 the movie
constexpr uint32_t kFilmName = 0x84;
constexpr uint32_t kFilmFlags = 0x198;
constexpr uint32_t kFilmState = 0x1A8;
constexpr uint32_t kFilmStop = 0x1B9;
constexpr uint32_t kFilmPlaying = 0x1C;  // the last state
// movie
constexpr uint32_t kMovieFrameLength = 0x54;
constexpr uint32_t kMovieClockStep = 0x5C;

// The film player, one for every film: a film ends on a press unless the film table marks it as menu background,
// the logos (LO_) and the film after pressing start (02_) as they begin when intros are skipped
void SkipVideo(X86Context&, uint8_t*) {
    bool pressed = SkipPressed();
    uint32_t game_object = R32(kGame);
    uint32_t player = game_object + kFrontEnd + kFilmPlayer;
    uint32_t film = R32(game_object + kCurrentFilm);
    uint32_t state = R32(player + kFilmState);
    bool background = film < kFilms && G<uint8_t>(kFilmTable + film * kFilmEntry + kFilmBehindMenu);
    if (pressed) LOGI("film %u: press in state %X, flags %X", film, state, R32(player + kFilmFlags));
    // a film keeps the frame time of its start: its frame length, and its clock's step
    if (uint32_t movie = R32(player); movie && state >= 4 && state <= kFilmPlaying) {
        G<float>(movie + kMovieFrameLength) = G<float>(kGameFrameTime);
        G<float>(movie + kMovieClockStep) = G<float>(game_object + kGameClockStep);
    }
    if (background || state < 2 || state > kFilmPlaying) return;
    std::string name = ReadCString(player + kFilmName, 8);
    bool intro = name.rfind("LO_", 0) == 0 || name.rfind("02_", 0) == 0;
    if (pressed || (game::Opt().skip_intros && intro)) G<uint8_t>(player + kFilmStop) = 1;
}
