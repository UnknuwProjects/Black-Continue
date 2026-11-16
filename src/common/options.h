#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

// the game's own settings, in the settings file through app::kGame.options
namespace game {

// the languages, saved by these codes: English, French, German, Italian, Spanish, Indonesian. Indonesian is the
// recomp's own text only: the game files have none, so the game itself stays in English
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp", "ind"};

struct Options {
    bool skip_intros = false;  // logos at boot and the film after pressing start
    bool fps_counter = false;
    // the overlay (src/game/fps_counter.cpp): where the FPS line goes, 0 top left, 1 top middle, 2 top right, 3 bottom
    // left, 4 bottom middle, 5 bottom right; the CPU, RAM and battery readings on it, each shown or not; the MOD BY UNKNUW mark, shown with the counter: 1 to 3
    // bottom left, middle, right
    uint32_t fps_position = 0;
    bool show_cpu = true;  // each reading of the overlay's line on its own
    bool show_ram = true;
    bool show_bat = true;
    uint32_t watermark_pos = 1;
    uint32_t fmv_scale = 0;  // 4:3 films on a wider screen: 0 fill it, top and bottom cut; 1 kept 4:3
    // a look over the frame: 0 none, 1 and 2 a CRT (light, strong), 3 sharpen, 4 vibrant, 5 noir, 6 sepia,
    // 7 night vision, 8 VHS
    uint32_t screen_filter = 0;
    bool reload_blur = true;
    bool bloom = true;  // the glow added over bright parts of the frame (native/vulkan.cpp)
    bool fog = true;
    uint32_t anti_aliasing = 0;  // 0 off, 1 FXAA as the frame goes onto the screen, 2 SSAA: twice the resolution
    // ARM Mali GPUs (MediaTek too) draw the glow passes saturated: 0 off, 1 auto (the shader fix on Mali only),
    // 2 shader fix, 3 glow passes dropped, 4 both, 5 full precision shaders, 6 every glow-like pass dropped, 7 the shader
    // fix, full precision and the wide drop; a change applies at once (native/vulkan.cpp)
    uint32_t mali_fix = 1;
    bool hdr = false;  // a tone map that brings back the colours the console's overbright look burns out
    uint32_t draw_distance = 1;  // times the game's LOD and draw distances
    // the render resolution follows the frame rate (native/dynamic_resolution.cpp): down when it falls under the
    // target, up again once it holds the target this many seconds; within min and max, percent of the screen's
    bool dynamic_resolution = false;
    uint32_t dynamic_target_fps = 60;
    uint32_t dynamic_min_percent = 40;
    uint32_t dynamic_max_percent = 100;
    uint32_t dynamic_hold = 5;
    float mouse_sensitivity = 1.0f;
    float touch_sensitivity = 1.0f;  // the touch screen's look, apart from the mouse's
    bool invert_mouse = false;
    bool hold_aim = true;  // scopes too, keyboard and mouse
    uint32_t fov = 70;  // horizontal degrees on a 4:3 frame, 70 is the game's
    uint32_t viewmodel_fov = 70;  // the gun and arms, the same measure
    uint32_t walk_speed = 50;  // percent of running, while the walk key is held
    uint32_t sprint_speed = 200;  // percent of running while sprint is held, 200 to 500 (src/game/movement.cpp)
    bool sprint_fov = true;  // the view widens a little while sprinting
    uint32_t sprint_pose = 1;  // the gun's pose while sprinting (ViewmodelShift in src/game/hooks.cpp): 0 off, 1 the built in pose, 2 SaveData/Sprint_Custom.txt
    bool noclip = false;  // cheat, NOCLIP: the player walks through walls and objects; gravity, floors and jump stay the game's (src/game/movement.cpp)
    bool infinite_stamina = false;  // the sprint never runs out and the stamina bar is not shown (src/game/movement.cpp)
    bool sprint_lines = true;  // speed lines at the edges of the screen while sprinting (SprintLines, src/game/movement.cpp)
    uint32_t jump_height = 80;  // centimetres the feet rise, 30 to 5000; the game's gravity measured 13.2 m/s squared
    uint32_t jump_mode = 0;  // 0 NORMAL, 1 FLY: jump rises while held, nothing pulls down, crouch goes down
    bool double_jump = false;  // one more jump in the air
    // a controller's sticks: the radial dead zone as percent of the travel, 0 the game's own, and what the travel
    // past it is scaled by; the left stick moves, the right looks
    uint32_t left_deadzone = 0;
    uint32_t right_deadzone = 0;
    float left_sensitivity = 1.0f;
    float right_sensitivity = 1.0f;
    uint32_t audio_output = 0;  // audio::kOutputAuto, kOutputStereo, kOutputSurround or kOutputMono
    std::string language = "int";  // one of kLanguages
    bool infinite_ammo = false;  // cheat: the player's magazine and reserve never go down
    uint32_t rapid_fire = 1;  // cheat: times the player's rate of fire, 1 the game's
    bool no_recoil = false;  // cheat: the player's shots do not kick the view
    bool steady_aim = true;  // the gun's idle sway is held still at the aim, every weapon (src/game/hooks.cpp); off: the game's own sway
    uint32_t aim_view = 1;  // the aim's look: 0 ZOOM (the game's own, no move of the gun), 1 ADS (the gun moves to the middle, src/game/hooks.cpp)
    bool no_kick = false;  // the fire clip's pose is held at its start: no kick animation, the shell casings still come (src/game/hooks.cpp)
    uint32_t ads_custom = 0;  // the ADS rows: 0 the built in table (ads_view.txt as it was), 1 ADS_Custom.txt in SaveData
    uint32_t game_speed = 100;  // cheat: percent of the game's speed, menus and films keep theirs
    bool god_mode = false;  // cheat: no hit takes the player's health
    bool one_hit_kill = false;  // cheat: any hit of the player's bullets kills
    bool infinite_grenades = false;  // cheat: throwing a grenade keeps the count
    bool infinite_medkits = false;  // cheat: using a medkit keeps the count (src/game/weapons.cpp)
    bool unlock_levels = false;  // cheat: every mission open on every difficulty, the save is left as it is
    bool unlock_black_ops = false;  // cheat: Black Ops open on the missions open on another difficulty, the same
    uint32_t esp_style = 0;  // cheat: what is drawn on each living enemy in view: 0 off, 1 box, 2 line from the bottom of the screen, 3 both
    bool esp_arrows = false;  // cheat: an arrow on a ring round the middle towards each enemy out of the view
    bool aimbot = false;  // cheat, AIM LOCK: aiming in turns the view to the head of the nearest enemy in view with nothing in the way
    // cheat, AIM LOCK, while aiming in: 0 off, 1 ONLY ASSIST (the view turns to an enemy and is let go on arrival), 2 AUTO LOCK (it stays on the enemy until it dies)
    uint32_t aim_lock = 0;
    uint32_t aim_assist = 0;  // cheat: 0 off, 1 to 3 a pull of the view to an enemy near the crosshair while firing
    uint32_t head_height = 90;  // percent: 90 is the middle of the head as measured, a step moves the point 1.8 cm; 1.80 m times it when no body was measured
    uint32_t aim_target = 0;  // cheat: where AIM LOCK turns to: 0 head, 1 body, 2 auto (the head while it is seen, else the body)
    uint32_t head_offset = 30;  // centimetres plus 30: 0 is 30 left on the screen, 30 none, 60 is 30 right
    uint32_t esp_label = 0;  // the plate over each enemy in view, drawn by ImGui: 0 off, 1 health, 2 distance, 3 both
    uint32_t esp_label_show = 0;  // when the plate shows: 0 auto (always with a box or line, else after a hit), 1 always, 2 after a hit
    bool esp_visible = false;  // the box, line, bar and plate only for an enemy with nothing in the way, not for every one in view; the arrows are as they were
    bool esp_health = false;  // the health bar over each enemy in view
    uint32_t esp_width = 8;  // tenths of a line of the 480 frame: the thickness of the ESP's box and line, 0 to 30
    uint32_t esp_height = 180;  // centimetres: the ESP box's height
    bool objective_esp = false;  // cheat, OBJECTIVE ESP: a marker, its distance and an edge arrow for each mission objective not yet done (src/game/esp.cpp)
    bool esp_fit = false;  // the box's top follows the enemy's stance (standing, crouching) as the hitboxes show it
    uint32_t esp_on_color = 0xFFFFFF;   // 0xRRGGBB, the lines and boxes of the enemies in the view
    uint32_t esp_off_color = 0xFF0000;  // the arrows to the enemies out of it
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};
Options& Opt();
// the language setting as this run started, the language the game boots in
const std::string& BootLanguage();
bool HasLanguages();  // language.cpp
bool LanguageAvailable(const std::string& code);  // language.cpp: the LANGUAGE row offers it
inline std::atomic<float> g_fps{0};  // from the host, for the game's own counter
// the view's widening while sprinting (src/game/movement.cpp): a part of the tangent of the half view, 0 none; hooks.cpp
inline std::atomic<float> g_sprint_view{0};
// from BlackActivity's thread (nativeSetStats in input.cpp), below 0 while unread or when the phone gives none: the
// temperatures in degrees C, the CPU's load and the RAM in use in percent
inline std::atomic<float> g_cpu_temp{-1.0f};
inline std::atomic<float> g_bat_temp{-1.0f};
inline std::atomic<int> g_cpu_load{-1};
inline std::atomic<int> g_ram_used{-1};
inline std::atomic<int> g_bat_level{-1};  // the battery's charge, percent
// the crouch button of the pad (the touch crouch, bit 6 of the buttons), false while the panel is open: FLY goes down
// with it (src/game/movement.cpp), written in game_info.cpp
inline std::atomic<bool> g_crouch_held{false};
// the pad's white (sprint) and black (jump) buttons, held: the game has no use for them, game_info.cpp takes them off the
// pad and movement.cpp reads them with the touch controls and the keys
inline std::atomic<bool> g_pad_sprint{false};
inline std::atomic<bool> g_pad_jump{false};
// the sprint's speed lines, 0 none .. 1 full (written by movement.cpp's hook, drawn by game::SprintLines from the UI)
inline std::atomic<float> g_sprint_lines{0};
// the sprint's gun pose, 0 hip .. 1 fully sprinting (movement.cpp's hook writes it, hooks.cpp's ViewmodelShift reads it)
inline std::atomic<float> g_sprint_pose{0};
// the pad's left stick pushed ahead (the angle within 45 degrees of straight forward), false in the panel: sprint needs it
inline std::atomic<bool> g_move_forward{false};
// the pad's left stick, -1 .. 1 (x to the right, y ahead), 0 in the panel: NOCLIP walks by it
// a crouch press to give the game once (the player is crouched and a jump or fly wants him standing: the crouch is a
// toggle, so one press stands him) and the time up to which the real crouch button is held back (fly and noclip use it
// for 'down'): both are done to the pad in game_info.cpp, from movement.cpp
inline std::atomic<int> g_stand_request{0};
inline std::atomic<int64_t> g_crouch_block_until{0};
inline std::atomic<float> g_move_x{0};
inline std::atomic<float> g_move_y{0};
// steady_clock milliseconds up to which the player is sprinting (movement.cpp renews it each frame): the fire button is
// held back until then, written to the pad in game_info.cpp
inline std::atomic<int64_t> g_sprint_until{0};
// the stamina bar: 0 empty .. 1 full, shown or not, and whether the sprint is locked out until it has come back a part
inline std::atomic<float> g_stamina{1};
inline std::atomic<bool> g_stamina_visible{false};
inline std::atomic<bool> g_stamina_low{false};
inline std::atomic<bool> g_hide_hud{false};  // the toggle HUD key: the gameplay HUD is not drawn
// the view's zoom as the look update scales the turn by (src/game/mouse.cpp): 1 none, about 0.74 a rifle's sights,
// a scope's levels under that (a half at X2, a quarter at X5)
inline std::atomic<float> g_zoom{1.0f};
inline std::atomic<bool> g_focus_paused{false};  // the game thread waits while the app is in the background
inline std::atomic<uint32_t> g_focus_pauses{0};  // focus pauses ended so far
void WaitWhileUnfocused();  // native/game_host.cpp: game thread, before the present
bool HudShown();  // src/game/hud.cpp: the player's HUD drew lately, a hint's pause too

}  // namespace game
