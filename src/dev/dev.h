// Developer-only tools; only built with TCNYC_DEV_TOOLS.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cpu/x86_context.h"

namespace dev {

// once per frame on the game thread, before the simulation step (timing.cpp)
void OnFrame(X86Context &ctx);

// the dev menu (dev_menu.cpp), from GameInfo::ui inside the ImGui frame
void DrawMenu();

// game state for the menu; safe to call from the UI thread (reads only)
struct Mission {
    int case_index, mission_index;
    uint32_t hash;
    std::string id;    // internal name from LevelInfo.def, or the hash in hex
    std::string title; // the game's English title, or a readable one made from the id
};
struct Case {
    uint32_t hash;
    std::string id, title;
    std::vector<Mission> missions;
};
std::vector<Case> StoryTable();
bool InGame();                                   // game object ready
bool InGameplay();                               // a gameplay camera follows the player
bool CurrentMission(int &case_index, int &mission_index);
int ClockMinutes();                              // time of day, minutes since midnight, -1 if unknown
std::string IdOf(uint32_t hash);    // internal name, or the hash in hex
std::string TitleOf(uint32_t hash); // display title, or IdOf

// live script instances (dev_scripts.cpp), for jumping to a stage of any mission or character script
struct Script {
    uint32_t instance, owner;     // owner: the character running it, 0 for mission-level scripts
    uint32_t owner_name;          // the character's name hash, 0 if unknown
    uint32_t state;               // current state (name hash)
    std::vector<uint32_t> states; // every state it has (name hashes)
};
std::vector<Script> LiveScripts();
std::string ScriptName(uint32_t hash);
void RequestState(uint32_t instance, uint32_t state_hash); // applied at that script's next update
void ScriptsFrame();                                       // once a frame, from OnFrame

// player cheats and game speed (dev_player.cpp)
bool PlayerStatus(float &health, float &max_health); // false when there is no player
void SetGodMode(bool on);
bool GodMode();
void SetInfiniteAmmo(bool on);
bool InfiniteAmmo();
void SetAllowWeaponSwitch(bool on); // undo missions' DisableSwitchItems
bool AllowWeaponSwitch();
void RequestHeal();
void RequestMoney(int amount);
void RequestGoodCopPoints(int points);
void RequestBadCopPoints(int points);
void RequestCareerPoints(int points);
bool CopPoints(int &good, int &bad);
void RequestItem(const char *item_name, bool left_hand = false); // e.g. "Pistol_DE": into Nick's hand
void SetGameSpeed(int quarters); // 0 paused, 4 normal, 1 quarter speed, 16 four times
int GameSpeed();
void StepFrame();                 // while paused: run one simulation tick
uint32_t AdjustTicks(uint32_t ticks); // timing.cpp: the simulation's ticks this frame
void PlayerFrame(X86Context &ctx);    // once a frame, from OnFrame

// requests from the menu, applied on the game thread at the next frame
void RequestMission(int case_index, int mission_index);
void RequestClock(int hour, int minute);

} // namespace dev
