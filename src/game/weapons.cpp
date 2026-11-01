// The weapons page of the F10 panel: any of the game's player weapons in any level.
//
// Every level's weapon table holds all seventeen player weapons, but a level ships only the first person banks
// (Levels\Level_NN\fpguns\<weapon>.db and .wdd: the gun, arms, animations and sounds) and the world models
// (Guns.bin, the "<weapon>_LOD" models a dropped gun shows) of the guns it uses. So a weapon is given the game's way:
// created from the table, put in the slot not in hand, and switched to as the d-pad does; a bank the level lacks is
// read from another level's folder, and a world model it lacks from another level's Guns.bin.
#include "game/weapons.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/layout.h"
#include "kernel/disc.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;
namespace fs = std::filesystem;

namespace {

// the weapon manager: +0 the weapon table, +4 the weapon objects (0x110 each), +8 their in use flags
constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kTableCount = 1;  // byte
constexpr uint32_t kTableEntries = 4;
constexpr uint32_t kEntrySize = 0x20;  // name key (8 bytes), weapon data, ...
// resource manager: the TEXTURE and MODELS dictionaries looked up by name, 11 of each
constexpr uint32_t kResources = 0x2D45D8;
constexpr uint32_t kTextureDicts = 0xC64;
constexpr uint32_t kModelDicts = 0xC90;
constexpr int kDictSlots = 11;
constexpr int kDictSlotsKept = 3;  // left free for the level's own later loads
// guest functions
constexpr uint32_t kCreateWeapon = 0x1CEE10;  // edi the manager; (owner, type, 0) -> the weapon
constexpr uint32_t kFreeWeapon = 0x1C9B90;    // edi the manager, edx the weapon
constexpr uint32_t kFindModel = 0x1F83E0;     // (resources, key low, key high) -> the model or 0
constexpr uint32_t kFixGuns = 0xB5F20;        // eax a Guns.bin in memory: relocated and its dictionaries registered
// world
constexpr uint32_t kWorldState = 0x5AE0;  // the level loader's step
constexpr uint32_t kStatePlaying = 0x1C;
constexpr uint32_t kWorldSilver = 0x5CB0;  // byte: the level loaded Guns_s.bin
// player
constexpr uint32_t kPlayerKind = 0xC4;
constexpr uint32_t kKindPlayer = 2;
constexpr uint32_t kPlayerInventory = 0x278;
// inventory: the ammunition of each type (words) first
constexpr uint32_t kInvOwner = 0x1C;
constexpr uint32_t kInvSlots = 0x20;     // the weapons, one a slot
constexpr uint32_t kInvHeld = 0x24;      // the weapon in hand
constexpr uint32_t kInvSwitchTo = 0x28;  // the weapon a switch brings out
constexpr uint32_t kInvRequest = 0x38;   // 2 asks to switch to the other slot, as the d-pad does
constexpr uint32_t kInvBusy = 0xB4;       // 0x13 while the suppressor animation runs
constexpr uint32_t kInvSlotCount = 0x42;  // byte
constexpr uint32_t kInvSlotHeld = 0x43;   // byte
constexpr uint32_t kPickupTable = 0x5844;      // its pickups, 0x160 bytes each: +0x140 kind, +0x148 type, +0x154 collected
constexpr int kAmmoTypes = 10;
constexpr uint32_t kPlayerMedkits = 0x8E1;  // byte in the player: the medkits held (found by the pickup tracker), the game holds 3
constexpr int kMedkitMax = 3;
constexpr uint32_t kAmmoMax = 999;
// weapon and its data
constexpr uint32_t kWeaponType = 0;  // byte: index in the table
constexpr uint32_t kWeaponAnim = 0xD8;        // the first person animation id, 0 when idle (0x19 attach, 0x1A detach)
constexpr uint32_t kWeaponSuppressed = 0x108;  // 1 while the suppressor is on (found by diffing snapshots)
constexpr uint32_t kWeaponData = 0xEC;
constexpr uint32_t kWeaponMagazine = 0xF4;  // the main fire's magazine, +0xF8 the alternate fire's
constexpr uint32_t kMagazineData = 8;
constexpr uint32_t kDataClip = 0x60;
constexpr uint32_t kDataAmmoType = 0x64;

// the game's files under D:, the disc image when it runs from one, else the game's folder
const std::string kDiscRoot = "D:\\";

// names as the game keeps them: twelve characters of " -/0-9A-Z_" in base 40
std::string Decode(uint64_t key) {
    char s[13] = {};
    for (int i = 11; i >= 0; i--) {
        int c = int(key % 40);
        key /= 40;
        s[i] = c == 0 ? ' ' : c == 1 ? '-' : c == 2 ? '/' : c < 13 ? char('0' + c - 3) : c < 39 ? char('A' + c - 13) : '_';
    }
    std::string out = s;
    out.erase(out.find_last_not_of(' ') + 1);
    return out;
}

uint64_t Encode(const std::string& name) {
    uint64_t key = 0;
    for (size_t i = 0; i < 12; i++) {
        char ch = i < name.size() ? char(std::toupper(uint8_t(name[i]))) : ' ';
        int c = ch == '-' ? 1 : ch == '/' ? 2 : ch >= '0' && ch <= '9' ? 3 + ch - '0' : ch >= 'A' && ch <= 'Z' ? 13 + ch - 'A' : ch == '_' ? 39 : 0;
        key = key * 40 + uint64_t(c);
    }
    return key;
}

// the names the game's own text gives them, told apart by clip and ammunition; the silver ones end in 5
const std::map<std::string, std::string> kNames = {
    {"PST", "Pistol"},         {"PS5", "Pistol (Silenced)"},  {"GK1", "Glock"},        {"GK5", "Glock (Silenced)"},
    {"MGN", "Magnum"},         {"MG5", "Magnum"},             {"SMG", "Uzi"},          {"SM5", "Uzi"},
    {"SM3", "MAC-10"},         {"MC5", "MAC-10"},             {"MP1", "MP5"},          {"MP5", "MP5 (Suppressed)"},
    {"P90", "P90"},            {"P95", "P90"},                {"AK1", "AK-47"},        {"AK5", "AK-47"},
    {"ASR", "G36C"},           {"AS5", "G36C"},               {"M16", "M16"},          {"M45", "M16"},
    {"BNS", "M16A2 + M203"},   {"SHG", "SPAS 12"},            {"SH5", "SPAS 12"},      {"RM1", "Remington 870"},
    {"RM5", "Remington 870"},  {"SNR", "Walther 2000"},       {"SN5", "Walther 2000 (Silenced)"}, {"HVY", "M249"},
    {"HV5", "M249"},           {"RPG", "RPG"},                {"GRL", "Grenade launcher"},
};

std::string Label(const std::string& key_name) {
    std::string code = key_name.size() > 4 && key_name.compare(0, 4, "BG1_") == 0 ? key_name.substr(4) : key_name;
    auto it = kNames.find(code);
    return it == kNames.end() ? code : it->second;
}

// ---- automatic suppressor: weapon+0x108 is 1 while the suppressor is on ----------------------------------------
// Weapons whose table code is listed get it set once when they are in hand and idle. The default list is what the
// tests proved (Glock, AK-47, Pistol, MP5, Walther 2000, Uzi, G36C, MAC-10, P90, M16); suppressor_codes.txt beside the log (codes separated by spaces, e.g. "GK1 AK1 MP1")
// replaces it, to try other weapons.
std::atomic<bool> g_auto_suppressor{false};  // the panel offers both versions of each weapon; true also forces it on pickups
std::set<std::string> g_supp_codes;
std::map<uint32_t, int> g_applied;  // weapon object -> type it was handled as
uint32_t g_swap_back = 0;           // a weapon to bring back out once the other one is in hand

std::string CodeOf(const std::string& key_name) {
    return key_name.size() > 4 && key_name.compare(0, 4, "BG1_") == 0 ? key_name.substr(4) : key_name;
}

// black.log takes none of the LOGI lines, so the weapons' own diagnostics also go to a file beside it
void DiagLog(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    LOGI("%s", line);
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    static const char* path = "/storage/emulated/0/Black/SaveData/weapons_diag.log";
    static bool first = true;
    static int lines = 0;
    if (first) {  // every run starts a fresh file; the run before is kept as weapons_diag_prev.log
        first = false;
        std::rename(path, "/storage/emulated/0/Black/SaveData/weapons_diag_prev.log");
    }
    if (lines >= 3000) return;  // a runaway probe cannot fill the storage
    lines++;
    if (FILE* f = std::fopen(path, "a")) {
        std::fputs(line, f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

void LoadSuppressorCodes() {
    static bool done;
    if (done) return;
    done = true;
    g_supp_codes = {"GK1", "AK1", "PST", "MP1", "SMG", "ASR", "SM3", "P90", "M16", "BNS"};  // checked in game; SNR (Walther 2000) has no suppressor to put on or take off
    std::ifstream f("/storage/emulated/0/Black/SaveData/suppressor_codes.txt");
    std::set<std::string> codes;
    std::string tok;
    while (f >> tok) {
        for (char& c : tok) c = char(std::toupper(uint8_t(c)));
        codes.insert(CodeOf(tok));
    }
    if (!codes.empty()) g_supp_codes = codes;
    std::string all;
    for (const std::string& c : g_supp_codes) all += c + " ";
    DiagLog("weapons: auto suppressor for %s", all.c_str());
}

bool SuppSupportedCode(const std::string& code) {
    LoadSuppressorCodes();
    return g_supp_codes.count(code) != 0;
}

// Every type the game has no suppressor for (no Y) gets a "+ Suppressor" entry in the second group, a permanent one, never
// forced on a pickup or a drop (g_supp_codes is the only list those use). Tested in game: the sound is a suppressed one
// for all of them, the muzzle effect changes too except for these two, where only the sound changes.
bool SoundOnlyCode(const std::string& code) {
    static const std::set<std::string> only = {"RPG", "GRL"};
    return only.count(code) != 0;
}

// does `type` of this manager's table take a suppressor?
bool SuppSupportedType(uint32_t manager, int type) {
    uint32_t table = manager ? R32(manager) : 0;
    if (!table || type < 0) return false;
    int count = G<uint8_t>(table + kTableCount);
    uint32_t entries = R32(table + kTableEntries);
    if (!entries || type >= count || count > 64) return false;
    uint32_t e = entries + uint32_t(type) * kEntrySize;
    return SuppSupportedCode(CodeOf(Decode(R32(e) | uint64_t(R32(e + 4)) << 32)));
}

// what the game thread found and the panel shows
std::mutex g_lock;
weapons::View g_view;
std::atomic<int> g_give{-1};
std::atomic<bool> g_fill{false};
std::atomic<bool> g_medkits{false};

void SetStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_view.status = text;
    DiagLog("weapons: %s", text.c_str());
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// a file of the game, by its path under D:
bool GameFileExists(const std::string& path) {
    if (const Disc* disc = MountedDisc()) {
        const Disc::Entry* e = disc->Find(path);
        return e && !e->dir;
    }
    std::string host = ResolvePath(kDiscRoot + path, 0);
    std::error_code ec;
    return !host.empty() && fs::is_regular_file(host, ec);
}

// a file of the game, read whole: from the disc image when it runs from one, else from the game's folder
bool LoadGameFile(const std::string& path, std::vector<uint8_t>& out) { return ReadGameFile(kDiscRoot + path, out); }

int LevelNumber(std::string name) {
    name = Lower(name);
    bool ok = name.size() == 8 && name.compare(0, 6, "level_") == 0 && std::isdigit(uint8_t(name[6])) &&
              std::isdigit(uint8_t(name[7]));
    return ok ? (name[6] - '0') * 10 + (name[7] - '0') : -1;
}

// the level folders the game has, by number
const std::vector<int>& Levels() {
    static const std::vector<int> levels = [] {
        std::vector<int> out;
        if (const Disc* disc = MountedDisc()) {
            if (const Disc::Entry* dir = disc->Find("Levels"))
                for (const Disc::Entry& e : dir->children)
                    if (e.dir && LevelNumber(e.name) >= 0) out.push_back(LevelNumber(e.name));
        } else {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(ResolvePath(kDiscRoot + "Levels", 0), ec))
                if (e.is_directory(ec) && LevelNumber(e.path().filename().string()) >= 0)
                    out.push_back(LevelNumber(e.path().filename().string()));
        }
        std::sort(out.begin(), out.end());
        return out;
    }();
    return levels;
}

// ---- first person banks: Level_NN\fpguns\ a level lacks is read from another level's -----------------------------

// in place, the same length: the level number of a bank path to one that has the file
void RedirectBank(uint32_t path_va) {
    std::string path = ReadCString(path_va, 260);
    std::string low = Lower(path);
    size_t at = low.find("level_");
    if (at == std::string::npos || low.find("\\fpguns\\", at) != at + 8 || !std::isdigit(uint8_t(low[at + 6])) ||
        !std::isdigit(uint8_t(low[at + 7])))
        return;
    static std::mutex lock;
    static std::map<std::string, int> chosen;  // lowercase path -> the level to read it from, -1 as it is
    std::lock_guard<std::mutex> guard(lock);
    auto it = chosen.find(low);
    if (it == chosen.end()) {
        size_t root = path.find(":\\");
        size_t skip = root == std::string::npos ? 0 : root + 2;  // a D:\ in front
        std::string rel = path.substr(skip);
        int level = -1;
        if (!GameFileExists(rel)) {
            for (int n : Levels()) {
                char digits[3];
                std::snprintf(digits, sizeof(digits), "%02d", n);
                std::string other = rel;
                other.replace(at - skip + 6, 2, digits);
                if (GameFileExists(other)) {
                    level = n;
                    break;
                }
            }
            if (level < 0) LOGW("weapons: no level has %s", path.c_str());
            else LOGI("weapons: %s from Level_%02d", path.c_str(), level);
        }
        it = chosen.emplace(low, level).first;
    }
    if (it->second < 0) return;
    char* s = static_cast<char*>(HostPtr(path_va));
    s[at + 6] = char('0' + it->second / 10);
    s[at + 7] = char('0' + it->second % 10);
}

// ---- world models: another level's Guns.bin for the "_LOD" models this one lacks ----------------------------------

struct GunsFile {
    std::string path;  // Levels\Level_NN\Stg_0001\Guns.bin
    bool silver;       // Guns_s.bin
    std::vector<uint64_t> models;
    uint32_t buffer = 0;     // guest copy, made the first time it is needed
    uint32_t dicts[2] = {};  // its TEXTURE and MODELS dictionaries while registered, this level
};

std::vector<GunsFile>& GunsFiles() {
    static std::vector<GunsFile> files = [] {
        std::vector<GunsFile> out;
        for (int n : Levels()) {
            for (const char* name : {"Guns.bin", "Guns_s.bin"}) {
                char path[64];
                std::snprintf(path, sizeof(path), "Levels\\Level_%02d\\Stg_0001\\%s", n, name);
                std::vector<uint8_t> d;
                if (!GameFileExists(path) || !LoadGameFile(path, d)) continue;
                auto u32 = [&](size_t at) {
                    uint32_t v = 0;
                    if (at + 4 <= d.size()) std::memcpy(&v, &d[at], 4);
                    return v;
                };
                // +4 the MODELS dictionary: +8 its count, +0xC its entries, 16 bytes each from the name key
                uint32_t dict = u32(4);
                uint32_t count = u32(dict + 8), entries = dict + u32(dict + 0xC);
                GunsFile g{path, name[4] == '_', {}};
                for (uint32_t i = 0; i < count && i < 64; i++) {
                    uint64_t key = u32(entries + i * 16) | uint64_t(u32(entries + i * 16 + 4)) << 32;
                    if (key) g.models.push_back(key);
                }
                out.push_back(std::move(g));
            }
        }
        return out;
    }();
    return files;
}

int FreeSlots(uint32_t list) {
    int free = 0;
    for (int i = 0; i < kDictSlots; i++) free += R32(list + i * 4) == 0;
    return free;
}

void Unregister(uint32_t list, uint32_t dict) {
    for (int i = 0; i < kDictSlots; i++)
        if (dict && R32(list + i * 4) == dict) W32(list + i * 4, 0);
}

// a level is loading: what this one registered goes, the game may have cleared it already
void ForgetExtraGuns() {
    uint32_t res = R32(kResources);
    for (GunsFile& g : GunsFiles()) {
        if (res) {
            Unregister(res + kTextureDicts, g.dicts[0]);
            Unregister(res + kModelDicts, g.dicts[1]);
        }
        g.dicts[0] = g.dicts[1] = 0;
    }
}

// a world model, from another level's Guns.bin when this level lacks it; false when there is no room
bool EnsureModel(X86Context& ctx, uint64_t key, bool silver) {
    uint32_t res = R32(kResources);
    if (!res) return false;
    if (CallGuest(ctx, kFindModel, {res, uint32_t(key), uint32_t(key >> 32)})) return true;
    GunsFile* pick = nullptr;
    for (GunsFile& g : GunsFiles()) {
        if (g.dicts[1] || std::find(g.models.begin(), g.models.end(), key) == g.models.end()) continue;
        if (!pick || (g.silver == silver && pick->silver != silver)) pick = &g;
    }
    if (!pick) return false;
    if (FreeSlots(res + kTextureDicts) <= kDictSlotsKept || FreeSlots(res + kModelDicts) <= kDictSlotsKept) return false;
    std::vector<uint8_t> d;
    if (!LoadGameFile(pick->path, d) || d.size() < 16) return false;
    if (!pick->buffer) pick->buffer = mem::AllocContiguous(uint32_t(d.size()), 0, 0xFFFFFFFF, 0x1000);
    if (!pick->buffer) return false;
    std::memcpy(HostPtr(pick->buffer), d.data(), d.size());  // as on disc again: the game relocates it in place
    // known before the fix up: its models look their textures up while it runs (sub_001FBCE0 below)
    pick->dicts[0] = pick->buffer + R32(pick->buffer);
    pick->dicts[1] = pick->buffer + R32(pick->buffer + 4);
    uint32_t eax = ctx.eax;
    ctx.eax = pick->buffer;
    CallGuest(ctx, kFixGuns, {});
    ctx.eax = eax;
    LOGI("weapons: world models from %s", pick->path.c_str());
    return CallGuest(ctx, kFindModel, {res, uint32_t(key), uint32_t(key >> 32)}) != 0;
}

bool EnsureWorldModel(X86Context& ctx, const std::string& weapon_name, bool silver) {
    return EnsureModel(ctx, Encode(weapon_name + "_LOD"), silver);
}

// A dropped weapon becomes a pickup with its world model, a null one crashes: it is loaded now if it can be, else
// the drop makes none, as when the game's pickups are all in use. pm the pickup manager: +0x5844 the pickup table
// (count byte, +8 entries of 0x18: weapon key, model key, weapon type byte at +0x10), +0x5800 its models found
constexpr uint32_t kPickupModels = 0x5800;
constexpr uint32_t kDropEntrySize = 0x18;
bool DropHasModel(X86Context& ctx, uint32_t pm, uint8_t type) {
    uint32_t table = pm ? R32(pm + kPickupTable) : 0;
    if (!table) return true;
    uint32_t entries = R32(table + 8);
    int count = G<uint8_t>(table);
    int found = -1;
    for (int i = 0; i < count && found < 0; i++)
        if (G<uint8_t>(entries + uint32_t(i) * kDropEntrySize + 0x10) == type) found = i;
    if (found < 0) return false;
    uint32_t models = R32(pm + kPickupModels);
    if (models && R32(models + uint32_t(found) * 4)) return true;
    uint32_t e = entries + uint32_t(found) * kDropEntrySize;
    uint64_t key = R32(e + 8) | uint64_t(R32(e + 12)) << 32;
    uint32_t world = R32(kWorld);
    return EnsureModel(ctx, key, world && G<uint8_t>(world + kWorldSilver) != 0);
}

// ---- the player and the level's weapon table ---------------------------------------------------------------------

struct Level {
    uint32_t world, player, inventory, manager, entries;
    int count;
};

bool ReadLevel(Level& l) {
    l.world = R32(kWorld);
    if (!l.world || R32(l.world + kWorldState) != kStatePlaying) return false;
    l.player = l.world + kWorldPlayer;
    l.inventory = l.player + kPlayerInventory;
    if (R32(l.player + kPlayerKind) != kKindPlayer || R32(l.inventory + kInvOwner) != l.player ||
        !R32(l.inventory + kInvSlots))
        return false;
    l.manager = R32(kWeaponManager);
    uint32_t table = l.manager ? R32(l.manager) : 0;
    if (!table || !R32(l.manager + 4)) return false;
    l.count = G<uint8_t>(table + kTableCount);
  
