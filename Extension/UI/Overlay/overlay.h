#pragma once
#include "Engine/Game/World/park_rotation.h"
#include "Engine/Game/World/park_editor.h"
#include "Engine/Game/World/world_layers.h"
#include "Engine/Game/World/world_controls.h"
#include "Engine/Game/Rendering/graphics_controls.h"
#include "Engine/Game/Profile/object_persistence_model.h"
#include "Engine/Game/Profile/player_card.h"
#include "Engine/Game/Profile/progression.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/Settings/named_settings.h"
#include "Engine/Game/Multiplayer/session_model.h"
#include "Engine/Game/UI/menu_scale.h"
#include "Engine/Game/Skater/first_person_spring.h"

#include "Engine/Core/Console/console_entry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dingosdk::overlay {
struct Level {
    std::string asset;
    std::string display_name;
    std::vector<std::string> start_points;
    std::string manifest_start_point;
    // False only for a Studio manifest fallback which is valid as a detached
    // LM destination, not as the native request's base/root level.
    bool native_registered = true;
    bool can_load = true;
    std::string load_block_reason;
    bool custom = false; // Declared by a mod's level manifest.

    const std::string* automatic_start_point() const noexcept {
        if (!manifest_start_point.empty()) return &manifest_start_point;
        return !native_registered && start_points.size() == 1 ? &start_points.front() : nullptr;
    }
};

enum class DebugAction {
    set_free_camera,
    set_game_ui_hidden,
    set_camera_speed,
    restore_debug,
    set_noclip,
    add_forward_velocity,
    set_forward_velocity_speed,
    set_no_bail,
    set_park_editor,
    add_up_velocity,
    set_up_velocity_speed,
    set_first_person,
    set_first_person_fov,
    set_first_person_spring,
    set_first_person_offset_x,
    set_first_person_offset_y,
    set_first_person_offset_z,
    set_first_person_pitch,
    set_first_person_yaw,
    set_first_person_roll,
    set_first_person_spring_up,
    set_first_person_spring_down,
    set_first_person_spring_left,
    set_first_person_spring_right,
    reset_first_person_arm,
    set_free_camera_fov,  // 0 = the game's own FOV
    // Keep the last action in sync with the bound in request_scheduler.h.
};

struct DebugRequest {
    DebugAction action = DebugAction::restore_debug;
    bool enabled = false;
    float value = 0.0f;
};

struct FlightInput {
    bool active = false;
    bool boost = false;
    float right = 0, up = 0, forward = 0;
    float look_x = 0, look_y = 0;
};

struct DebugModel {
    bool available = false;
    bool camera_available = false;
    bool ui_available = false;
    bool free_camera = false;
    bool first_person = false;
    // Vertical degrees applied to the first-person view; 0 keeps the camera's own.
    float first_person_fov = 0;
    float free_camera_fov = 0;  // 0 = the game's own FOV
    first_person::Settings first_person_arm;
    bool park_editor = false;
    bool camera_transform_valid = false;
    std::array<float, 16> camera_transform{};
    float camera_fov = 55;
    bool noclip = false, noclip_available = false;
    bool forward_velocity_available = false;
    bool up_velocity_available = false;
    std::string camera_unavailable = "Waiting for local controls.";
    std::string noclip_unavailable = "Waiting for local controls.";
    std::string forward_velocity_unavailable = "Waiting for local controls.";
    std::string up_velocity_unavailable = "Waiting for local controls.";
    bool no_bail = false, no_bail_available = false, no_bail_active = false;
    std::uint64_t noclip_velocity_updates = 0, noclip_motion_updates = 0;
    std::uint64_t forward_velocity_updates = 0;
    std::uint64_t up_velocity_updates = 0;
    bool game_ui_hidden = false;
    bool settings_owned = false;
    // ReSkate free-flight speed in world units per second.
    float camera_speed = 15.0f;
    float forward_velocity_speed = 20.0f;
    float up_velocity_speed = 20.0f;
    bool camera_position_valid = false;
    bool skater_position_valid = false;
    std::array<float, 3> camera_position{};
    std::array<float, 3> skater_position{};
    std::uintptr_t skater_identity = 0;
    std::string status;
};

enum class OfflineFeatureGroup {
    activities,
    fast_travel,
    progression,
    developer_menus,
    board_wear,
    player_collision,
    restore_all,
};

struct OfflineFeatureRequest {
    OfflineFeatureGroup group = OfflineFeatureGroup::restore_all;
    bool enabled = false;
};

struct OfflineFeatureGroupModel {
    bool available = false;
    bool effective = false;
    bool override_active = false;
};

struct EngineVariableModel {
    std::string name;
    bool available = false;
    bool value = false;
    bool override_active = false;
};

struct OfflineFeatureModel {
    bool available = false;
    bool settings_owned = false;
    OfflineFeatureGroupModel activities;
    OfflineFeatureGroupModel fast_travel;
    OfflineFeatureGroupModel progression;
    OfflineFeatureGroupModel developer_menus;
    OfflineFeatureGroupModel board_wear;
    OfflineFeatureGroupModel player_collision;
    bool skater_slots_available = false;
    bool skater_slots_override_active = false;
    bool skater_slot_manager_available = false;
    bool skater_slot_selectors_enabled = false;
    std::uint32_t skater_slot_count = 0;
    std::uint32_t skater_slot_target = 10;
    std::string skater_slot_status;
    bool main_missions_available = false;
    bool main_missions_override_active = false;
    bool main_missions_authored_offline_route = false;
    std::uint32_t main_mission_claim_leases = 0;
    std::uint32_t main_mission_target = 20;
    std::uint32_t progression_unlock_claim_leases = 0;
    std::uint32_t progression_unlock_target = 2;
    std::uint32_t onboarding_dependency_claim_leases = 0;
    std::uint32_t onboarding_dependency_target = 2;
    std::uint32_t mission_pending_restore = 0;
    std::string main_mission_status;
    std::string status;
    // Exact-build, reflection-validated boolean settings. The console exposes
    // only this allowlist; arbitrary addresses and unvalidated fields are never
    // accepted as engine variables.
    std::vector<EngineVariableModel> variables;
};

using ConsoleLogLine = dingosdk::ConsoleLogLine;

struct MissionRow {
    std::string id, group;
    int completed = -1;
};

struct Model {
    std::string state = "Waiting for native state";
    std::string detail;
    std::vector<Level> levels;
    bool can_queue_load = false;
    std::string load_block_reason = "Native load handler is not connected.";
    DebugModel debug;
    OfflineFeatureModel offline;
    std::vector<NamedSettingModel> engine_settings;
    std::vector<ConsoleLogLine> console_log;
    bool missions_available = false;
    std::vector<MissionRow> missions;
    std::string mission_feedback;
    ParksModel parks;
    WorldLayersModel world;
    WorldControlsModel world_controls;
    ProgressionModel progression;
    PlayerCardModel player_card;
    ObjectPersistenceModel object_persistence;
    ParkEditorModel editor;
    float menu_scale = default_menu_scale;
    // Offline mode: no Steam, so every multiplayer page and control is hidden.
    bool steam_offline = false;
    ControllerBindingsModel bindings;
    GraphicsControlsModel graphics;
    MultiplayerModel multiplayer;
    // Host bookkeeping: the revision each part of this copy was taken at (zero:
    // never). A reader that hands its previous copy back to read_model has only
    // the parts that changed since copied; the rest are left as they are.
    struct Revisions {
        std::uint64_t model{}, debug{}, offline{}, engine_settings{};
    } revisions;
};

// Callbacks run on the presentation thread. They must be bounded and thread
// safe; queue callbacks must enqueue requests, never call engine code here.
struct Callbacks {
    void* user = nullptr;
    void (*read_model)(void* user, Model& output) = nullptr;
    bool (*queue_load)(void* user, const char* asset, const char* start_point,
                       const char* lm_level, const char* lm_start_point,
                       char* result, std::size_t result_size) = nullptr;
    bool (*queue_debug)(void* user, const DebugRequest& request,
                        char* result, std::size_t result_size) = nullptr;
};

// Extended callback table for hosts that expose process-local offline controls.
// Keep Callbacks and DingoSDKOverlayStart unchanged for binary compatibility with
// hosts built against the original four-field table.
struct CallbacksV2 {
    void* user = nullptr;
    void (*read_model)(void* user, Model& output) = nullptr;
    bool (*queue_load)(void* user, const char* asset, const char* start_point,
                       const char* lm_level, const char* lm_start_point,
                       char* result, std::size_t result_size) = nullptr;
    bool (*queue_debug)(void* user, const DebugRequest& request,
                        char* result, std::size_t result_size) = nullptr;
    bool (*queue_offline_feature)(void* user, const OfflineFeatureRequest& request,
                                 char* result, std::size_t result_size) = nullptr;
};

// Extended callback table for the in-game command console. Keep V1 and V2
// unchanged so previously built hosts remain binary compatible.
// Internal private action queue, separate from the stable V3 callback ABI and console history.
using MultiplayerQueue = bool (*)(const char *action, const char *argument, const char *password,
                                 char *result, std::size_t size);
void set_multiplayer_queue(MultiplayerQueue) noexcept;
// The session's text chat, read by the chat panel every frame (thread-safe, cheap).
using ChatFeed = MultiplayerChat (*)();
void set_chat_feed(ChatFeed) noexcept;
// Text the game draws with its debug text natives, where retail draws nothing (the
// S.K.A.T.E. throwdown HUD). One frame of lines, positioned in a width x height screen;
// color is R, G, B, A bytes (ImGui's IM_COL32 layout). Read every presented frame
// (thread-safe, cheap; empty = nothing).
struct GameTextLine {
    float x{}, y{}, scale{1};
    std::uint32_t color{0xffffffffU};
    bool centered{}; // x is the middle of the line
    std::string text;
};
struct GameText {
    float width{}, height{};
    std::vector<GameTextLine> lines;
};
using GameTextFeed = GameText (*)();
void set_game_text_feed(GameTextFeed) noexcept;
// The S.K.A.T.E. throwdown's HUD as ReSkate draws it (skate_hud_overlay.cpp), read from what
// the game's own debug HUD lays out each frame: the messages at the top (what to do, the
// trick to copy, how an attempt went) and every player's letters. Read every presented
// frame; no messages and no players = nothing to draw.
struct SkateHudMessage {
    enum class Kind : std::uint8_t { prompt, trick, success, failure } kind{};
    std::string text;
};
struct SkateHudPlayer {
    std::string name; // empty when the game has none
    int letters{};    // 0-5 of S.K.A.T.E.
    bool up{};        // whose turn it is
    bool out{};       // eliminated
};
struct SkateHud {
    std::vector<SkateHudMessage> messages; // top to bottom
    std::vector<SkateHudPlayer> players;   // in the game's order
    std::vector<std::string> done_tricks;  // set so far this game, oldest first (none repeat)
};
using SkateHudFeed = SkateHud (*)();
void set_skate_hud_feed(SkateHudFeed) noexcept;
// ReSkate's game modes (Extension/Modes): the scoreboard, clock and callouts, and what the
// game's leader marked out drawn over the world (the area, checkpoints or spots, Graffiti's
// zones in their taggers' colours). Read every presented frame; inactive = nothing to draw.
struct ModesHudRow {
    std::string name, value;
    std::uint32_t color{0xffffffffU}; // IM_COL32
    bool self{}, up{}, out{};
};
// A Graffiti tag: the path a board slid along (a grind) or a gap's takeoff and landing, in the
// colour of whoever holds it.
struct ModesHudTag {
    std::vector<std::array<float, 3>> path;
    bool gap{};
    std::uint32_t color{};
};
// Another player's game the local player is not in, shown like a throwdown drop: a banner over
// its spot (the start gate, the area's centre or where it was set up) and a row in the menu.
struct ModesHudOffer {
    std::string mode, host, detail;  // "HALL OF MEAT", "Huntredbanzzz", "3 players - join now"
    std::array<float, 3> at{};
    bool has_at{};
    bool open{};   // still taking players (setting up or counting down)
    bool target{}; // the one the join button acts on now
    std::uint64_t id{}; // `mode join <id>`
};
// Skate Tag: a player where they last said they were. The one who is it wears a neon crown; arrows at
// the screen's edge point to whoever matters off screen (it, or everyone else for the one who is).
struct ModesHudPlayer {
    std::string name;
    std::array<float, 3> at{};
    std::uint32_t color{}; // IM_COL32
    bool it{}, self{};
};
struct ModesHud {
    bool active{};        // in a game (the panel, the area and the rest); offers draw either way
    std::vector<ModesHudPlayer> players; // Skate Tag, while it is played
    std::vector<ModesHudOffer> offers;
    std::string invite;   // "Huntredbanzzz is starting Hall of Meat", shown for a while after it appears
    float invite_fade{};  // 0..1
    bool can_join{};      // a join button press would join `target` now
    std::string title;    // "Graffiti"
    std::string clock;    // "2:31", the countdown's "3", or empty
    std::string status;   // what to do now
    std::string line;     // the local player's line in progress
    std::string banner;   // the latest callout (big, for a moment)
    std::uint64_t banner_serial{};
    std::string warning;  // out of the area
    // The results at the end: the winner, their score and colour, and how long until it closes.
    bool results{};
    std::string winner, winner_value;
    std::uint32_t winner_color{0xffffffffU};
    std::uint32_t closing_ms{};
    std::vector<ModesHudRow> rows;
    std::vector<std::array<float, 3>> corners; // the area, in order around it (a circle's centre when area_radius > 0)
    float area_radius{};
    bool placing{};                            // the area or points are being placed: drawn as a preview
    bool aiming{}, aim_ok{};                   // placing from the free camera: a reticle; aim_ok when it finds ground
    std::array<float, 3> cursor{};             // placing: where the next thing goes
    std::string hint;                          // what to do while placing ("Skate to the centre.")
    // The controls while placing, as button prompts: a D-pad direction ('U', 'D', 'L', 'R', or 0
    // for none), the keyboard key and what it does.
    struct Prompt {
        char dpad{};
        std::string key, label;
    };
    std::vector<Prompt> prompts;
    std::array<float, 3> me{};                 // the local skater, for the walls fading in near them
    bool have_me{};
    std::vector<std::array<float, 3>> points;  // checkpoints or spots
    std::vector<std::uint32_t> point_colors;   // one per point (0: plain)
    int next_point{-1};                        // Deathrace: the local player's next checkpoint
    bool route{};                              // the points are a Deathrace route: start, checkpoints, finish
    std::vector<float> point_yaws;             // each gate's facing, degrees (the trainer's heading); empty: along the route
    std::vector<float> point_widths;           // each gate's half width in metres; empty: `radius`
    float radius{};
    std::vector<ModesHudTag> tags;
    std::array<float, 16> camera{}; // world matrix: right, up, back, position rows
    float vertical_fov{};
};
using ModesHudFeed = ModesHud (*)();
void set_modes_hud_feed(ModesHudFeed) noexcept;
// The Bone Cam (Extension/Modes/bone_cam.h): during a big bail the screen goes X-ray and every
// bone of the local skater is drawn between its real joints, the ones that took a hard hit in red,
// with a Skate 2 style injury list. Inactive = nothing to draw.
struct BoneCamBone {
    std::uint8_t sprite{};         // modes::BoneSprite
    std::array<float, 3> a{}, b{}; // world: the bone's first joint, and a point along it
    std::uint8_t hurt{};           // 0 sound, 1 cracked, 2 broken
};
struct BoneCamInjury {
    std::string bone, what; // "LEFT FEMUR", "FRACTURED"
    bool severe{};
};
// What a slam leaves behind: marks on the ground where the body slid (a skid for the torso, thin
// scrapes for hands, knees and head), and the hit itself.
struct BoneCamMark {
    std::vector<std::array<float, 3>> path; // world, along the ground
    float alpha{};                          // 0..1, fading with age
    bool skid{};
};
// The skater's pose for the 3D X-ray (bone_cam_3d.cpp), in world space. Sides are [0] right,
// [1] left. `valid` once a pose has been read this bail.
struct BoneCamPose {
    using Point = std::array<float, 3>;
    bool valid{};
    std::vector<Point> spine; // the joints from the hips up to the head, in order
    Point head{}, head_up{}; // the head joint and its up axis (unit)
    std::array<Point, 2> clavicle{}, shoulder{}, elbow{}, wrist{}, fingers{}, hip{}, knee{}, ankle{}, toe{};
    // Injuries by region (modes::BoneSprite order): 0 sound, 1 cracked, 2 broken.
    std::vector<std::uint8_t> hurt;
};
struct BoneCam {
    bool active{};         // the X-ray itself
    BoneCamPose pose;
    bool effects{};        // marks or a hit to draw (with or without the X-ray)
    float fade{};          // 0..1: fades in when the bail starts, out at the end
    std::vector<BoneCamMark> marks;
    float hit{};                    // 0..1: the flash of an impact, fading fast
    float daze{};                   // 0..1: a concussion's after-effects (kept faint), fading
    std::array<float, 3> hit_at{};  // world: where it landed
    bool preview{};        // the menu's preview: the injuries are examples
    std::uint64_t serial{}; // one per bail
    std::vector<BoneCamBone> bones;
    std::vector<BoneCamInjury> injuries;
    std::array<float, 3> left{}; // world: from the skater's right shoulder toward the left one (which way the art faces)
    std::array<float, 16> camera{}; // world matrix: right, up, back, position rows
    float vertical_fov{};
};
using BoneCamFeed = BoneCam (*)();
void set_bone_cam_feed(BoneCamFeed) noexcept;
// ModesHud's menu needs (the Game Modes page of the ReSkate menu): what the local player can do.
struct ModesMenu {
    bool in_game{}, leading{};
    int phase{};                  // modes::Phase, 0 when no game
    std::string mode;             // "graffiti"
    std::size_t corners{}, points{}, players{};
    std::uint32_t duration{}, turn{};
    int strikes{};
    float radius{};
    std::string missing;          // what `mode start` still needs
    std::string bone_cam;         // "meat", "on" or "off"
    bool bone_cam_ringing = true; // a concussion's ringing is on (not muted)
    float area_radius{};          // > 0: the area is a circle
    std::string placing;          // "circle", "corners", "points" while placing on the skater; else empty
    std::vector<ModesHudOffer> offers; // other players' games, to join from the menu
};
using ModesMenuFeed = ModesMenu (*)();
void set_modes_menu_feed(ModesMenuFeed) noexcept;
ModesMenu modes_menu() noexcept;
// True while ReSkate's menu, console, chat or park editor takes the player's input (any thread).
bool interface_open() noexcept;
// XInput buttons (XINPUT_GAMEPAD_* bits) the game stops seeing while ReSkate uses them itself, as
// game modes do with the D-pad while placing; 0 gives them all back (any thread).
void hide_game_buttons(std::uint16_t buttons) noexcept;
// All of the player's input kept from the game (keyboard, mouse and pad, Steam Input included),
// as ReSkate's menu does, without opening anything: game modes' free-camera placing flies on it.
// ReSkate's own reads still see it (key_down, the free camera, the controller reads). Publish every
// tick while wanted: a pause expires after 500 ms.
void pause_game_input(bool paused) noexcept;
// A key as the player holds it, past the gates above (GetAsyncKeyState's high bit).
bool key_down(int virtual_key) noexcept;
// Mouse wheel movement since the last call while the game's input was paused (WHEEL_DELTA units).
int take_mouse_wheel() noexcept;
// The pad buttons physically held right now, while some are hidden: with Steam Input the game hears
// actions, not buttons, so its actions wait while a hidden button is held (steam_input_block.cpp).
void hold_game_buttons(std::uint16_t held) noexcept;
// ReSkate's own nametags: one per other player, placed over the world with the camera the
// client last used. Close ones show a name and distance, far ones a dot, and players off
// screen a dot at the screen's edge. Empty = nothing to draw (off, or the game hides its UI).
// One line in a player's bubble stack: the text (masked when the chat filter is on), the line
// as sent when the filter changed it (same length; emote names are taken back from it), how
// far its pop-in has come (0 just arrived, 1 settled) and how opaque it still is (1 down to 0).
struct NametagBubble {
    std::string text, raw;
    float appear{1}, fade{1};
};
struct Nametag {
    std::array<float, 3> position{}; // above the skater's head, world space
    std::string name;
    std::uint32_t color{0xffffffffU}; // R, G, B, A bytes (IM_COL32)
    std::string tag;                  // role badge before the name ("Dev", "Staff", "Creator", "Centrix", "Homie", "Admin", "Host", "Friend")
    float distance{};                 // metres from the local skater
    bool talking{};
    // Recent chat lines to show as bubbles above the head, oldest first ("" = none).
    std::vector<NametagBubble> bubbles;
    bool self{};                      // the local player: bubbles only, never a name or dot
    bool nameless{};                  // another player whose name is not shown: bubbles only too
};
struct Nametags {
    std::array<float, 16> camera{}; // world matrix: right, up, back, position rows
    float vertical_fov{};
    bool show_names{true};          // draw the name, distance and role badge
    bool show_bubbles{};            // draw chat bubbles above the heads
    float bubble_distance{40.f};    // furthest a player may be and still show a bubble (metres)
    float name_distance{120.f};     // furthest a player's name shows; past it they are a dot
    bool dots{true};                // draw those dots, and the ones at the edge for off-screen players
    std::vector<Nametag> tags;
};
using NametagFeed = Nametags (*)();
void set_nametag_feed(NametagFeed) noexcept;
using ParkSurfaceQueue = bool (*)(const EditorSurfaceRequest &);
void set_park_surface_queue(ParkSurfaceQueue) noexcept;
using ParkPreviewQueue = bool (*)(const EditorPreviewRequest &);
void set_park_preview_queue(ParkPreviewQueue) noexcept;
using ParkSelectionQueue = bool (*)(const EditorSelectionRequest &);
void set_park_selection_queue(ParkSelectionQueue) noexcept;
using ParkPasteQueue = bool (*)(const EditorPasteRequest &);
void set_park_paste_queue(ParkPasteQueue) noexcept;
struct CallbacksV3 {
    void* user = nullptr;
    void (*read_model)(void* user, Model& output) = nullptr;
    bool (*queue_load)(void* user, const char* asset, const char* start_point,
                       const char* lm_level, const char* lm_start_point,
                       char* result, std::size_t result_size) = nullptr;
    bool (*queue_debug)(void* user, const DebugRequest& request,
                        char* result, std::size_t result_size) = nullptr;
    bool (*queue_offline_feature)(void* user, const OfflineFeatureRequest& request,
                                 char* result, std::size_t result_size) = nullptr;
    bool (*queue_console_command)(void* user, const char* command,
                                  char* result, std::size_t result_size) = nullptr;
};

struct Status {
    bool started = false;
    bool bound = false;
    bool ready = false;
    bool visible = false;
    bool stopping = false;
    bool failed = false;
    std::uint64_t rendered_frames = 0;
};
bool keyboard_shortcuts_allowed() noexcept;

// A short message in the top-left corner of the game window: a title line and
// optional detail, stacked under earlier ones and fading out on their own
// (info a few seconds, warnings and errors longer). Safe from any thread and
// before the overlay has started: queued notices show once the first frame
// draws. They take no input and never open the menu.
enum class NoticeLevel { info, warning, error };
void notify(NoticeLevel level, std::string title, std::string text = {}) noexcept;
}

// Call outside DllMain, before the game's first DXGI factory is created. No
// remote-process access and no image-specific patching happen in this component.
extern "C" __declspec(dllexport) bool DingoSDKOverlayStart(
    const dingosdk::overlay::Callbacks* callbacks);
extern "C" __declspec(dllexport) bool DingoSDKOverlayStartV2(
    const dingosdk::overlay::CallbacksV2* callbacks);
extern "C" __declspec(dllexport) bool DingoSDKOverlayStartV3(
    const dingosdk::overlay::CallbacksV3* callbacks);

// Nonblocking. A later Present releases GPU resources after its fence completes.
// The DLL stays pinned until process exit so no thread can return into unloaded
// detours. Input is released immediately, even if rendering has stopped.
extern "C" __declspec(dllexport) void DingoSDKOverlayRequestStop();
extern "C" __declspec(dllexport) void DingoSDKOverlayGetStatus(dingosdk::overlay::Status* status);
// Poll only on the client update thread. Returns no input while either overlay
// surface is open, the game is unfocused, or the overlay has stopped.
extern "C" void DingoSDKOverlayReadFlightInput(dingosdk::overlay::FlightInput* input, bool flight_active, bool player_flight = false);
// allow_menu is used only by the Binds page while recording. Focus and lifetime
// gates still apply. Gameplay callers leave it false.
extern "C" void DingoSDKOverlayReadControllerInput(dingosdk::ControllerInput* input, bool allow_menu = false);

// Optional direct integration for a host that already knows its exact pair.
struct IDXGISwapChain;
struct ID3D12CommandQueue;
extern "C" __declspec(dllexport) bool DingoSDKOverlayBindDx12(
    IDXGISwapChain* swapchain, ID3D12CommandQueue* command_queue);
