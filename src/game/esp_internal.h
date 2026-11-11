// What esp.cpp and esp_item.cpp share. esp.cpp keeps the enemies' ESP, the aim cheats and the walk of the world's records
// (nodes and bodies); esp_item.cpp keeps the objective markers: the tracker of the mission's objectives, the pickups, the
// targets and their drawing. The names below are esp.cpp's, declared here for esp_item.cpp to use
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace game::item {

constexpr uint32_t kNodeId = 0x1C, kNodeSolid = 0x2C, kNodeBlock = 0x2B;
constexpr uint32_t kSolidSize = 0x60, kBlockSize = 32, kIdSize = 16;  // bytes read of a node's object
constexpr uint32_t kMatchRead = 0x100;                                 // bytes read of a live object

struct LogFile {
    const char* name;
    bool first = true;
    long written = 0;
    bool off = false;   // nothing is written: the test log of the overlay is off while only the objective markers run
    bool mute = false;  // lean log: only the rows sent with force are written
};

struct Node {
    uint32_t kind, ptr, id;
    float pos[3] = {};
    uint32_t aux = 0, flag = 0;  // a transform's +0x50 and +0x54
    bool ok = false;             // its position was read and makes sense
    bool have = false;           // raw holds a read
    uint8_t changes = 0;         // changes logged
    uint8_t raw[kSolidSize] = {};
};
struct BigKind {
    uint32_t count, node, node2;  // objects of the kind, and the addresses of the first two met
};
struct ObjRef {
    uint32_t kind, addr, id;  // a live object of the kinds of 256 and up: its header's first and third words, and where it is
};
struct BodyHit {
    uint32_t addr, kind, id;  // a record that looks like a body, before it is proven by its pointer to itself
};
struct Body {
    uint32_t addr, kind, id;
    float pos[3];
    uint32_t type;   // +0xC4: 3 and 4 are the ones the aim routine tests for an objective
    uint8_t state;   // +0xD0
    bool pass;       // marked and the objective test says yes
    bool test;       // the objective test of the game, reproduced, whatever the state
};
struct NodePass {
    std::vector<Node> list;
    std::vector<ObjRef> objs;
    std::vector<BodyHit> hits;
    uint32_t hit_over = 0;  // candidates left out for the cap
    uint32_t kinds[257] = {};  // nodes met by kind, the last for 256 and up
    std::unordered_map<uint32_t, BigKind> big;  // the kinds of 256 and up
};
// a live object that matched a transform which is not a character's, its first 0x100 bytes watched for a change
struct Watch {
    uint32_t kind, addr;
    std::chrono::steady_clock::time_point armed;  // changes are logged from here, the words that changed before are noise
    uint8_t changes = 0;
    uint8_t noise[kMatchRead / 4] = {};  // times each word changed while it warmed up
    uint8_t raw[kMatchRead] = {};
};
struct Dots {
    uint32_t world = 0, tag = 0, table_tag = 0;  // table_tag: the tag the table was logged for
    std::vector<Watch> watch;
    std::vector<Body> bodies;  // the bodies of the walk before, read again every kBodyEvery
    std::string body_summary;  // their count by kind at the walk before
    bool bodies_seen = false;
    std::chrono::steady_clock::time_point next_body{};
    std::unordered_map<uint32_t, uint32_t> counts;  // objects by kind at the walk before
    bool scanning = false;
    uint32_t cursor = 0;
    NodePass cur, last;
    std::unordered_map<uint32_t, uint32_t> tags;  // tag -> records of the kinds 0x1C, 0x2B, 0x2C that carry it
    std::chrono::steady_clock::time_point next_scan{}, next_read{};
};

// the scene's camera, esp.cpp's: Clip is the clip space of a point (false behind the camera), Eye the eye space
extern bool g_camera;
bool Clip(const float p[3], float clip[4]);
void Eye(const float p[3], float eye[3]);
constexpr uint32_t kCharPosition = 0xA0;
constexpr float kFarOut = 20000.0f;  // metres: a position further than this is not one

// a level is running (the world's aim struct is there)
bool InLevel();

// guest memory as the walks read it, and the ranges the searches go through: the image, then the RAM
bool ObjCopy(uint32_t va, void* dst, size_t n);
struct ScanRange {
    uint32_t base, size;
};
constexpr ScanRange kScanRanges[] = {{0x10000, 0x3F0000}, {0x80010000, 0x03FF0000}};
constexpr uint32_t kScanChunk = 32768;  // one read

// the temporary log SaveData/logs/objdots.log
double LogTime();
void Append(LogFile& log, const std::string& text, bool force = false);
extern LogFile g_dots_log;

// the walk of the world's records: the nodes and the bodies it found, and the lines it may still write for a level
extern Dots g_dots;
extern int g_body_lines, g_open_lines;

// the seconds after the count of a destruction objective rose: the bodies and the nodes within kNearRange of where the
// player was are logged (the walk writes the bodies, esp_item.cpp the nodes)
constexpr float kNearRange = 30.0f;  // metres
constexpr auto kNearWindow = std::chrono::seconds(20);
constexpr int kNearLines = 10;  // lines for a rise
extern std::chrono::steady_clock::time_point g_near_until;
extern float g_near_at[3];
extern int g_near_left;
float NearDist(const float* p);
bool NearOpen(const float* p);
bool NearTake();

// esp_item.cpp
void ObjectivesTrack(const float* mine, std::chrono::steady_clock::time_point now);
void ObjectiveOverlay(ImDrawList* draw, ImFont* font, float size, float fx, float fy, float fw, float fh);
void DrawObjectives();

}  // namespace game::item
