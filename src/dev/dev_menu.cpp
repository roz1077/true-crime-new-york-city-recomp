// The developer debug menu: F9 in dev builds (TCNYC_DEV_TOOLS) only.
// Missions: every case and mission in the story table, with a jump button.
// World: time of day. Info: game state.
// It reads game memory directly and sends changes through dev.h's requests,
// which the game thread applies at its next frame.

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

#include "app/app.h"
#include "common/guest_values.h"
#include "core/input.h"
#include "core/log.h"
#include "dev/dev.h"

namespace dev {
namespace {

bool g_open = false;
char g_filter[64] = "";
int g_hour = 12, g_minute = 0;
bool g_clock_seeded = false;

void Open() {
    g_open = true;
    g_clock_seeded = false;
    input::SetUiActive(true);
    app::CaptureMouse(false); // the menu needs the pointer
}

void Close() {
    g_open = false;
    input::SetUiActive(false);
    app::CaptureMouse(true);
}

bool Matches(const std::string &name) {
    if (!g_filter[0]) return true;
    for (size_t i = 0; i + std::char_traits<char>::length(g_filter) <= name.size(); ++i) {
        size_t j = 0;
        while (g_filter[j] && std::tolower(uint8_t(name[i + j])) == std::tolower(uint8_t(g_filter[j]))) ++j;
        if (!g_filter[j]) return true;
    }
    return false;
}

void MissionsTab() {
    if (!InGame()) {
        ImGui::TextDisabled("The game has not started yet.");
        return;
    }
    const bool gameplay = InGameplay();
    int current_case = -1, current_mission = -1;
    CurrentMission(current_case, current_mission);
    if (!gameplay) ImGui::TextColored({1.0f, 0.7f, 0.3f, 1.0f}, "Load a save first: jumps only work in gameplay.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter (title or ID, e.g. Vertigo, BC2, Race)", g_filter, sizeof(g_filter));
    ImGui::BeginChild("missions", {0, 0}, ImGuiChildFlags_Border);
    for (const Case &c : StoryTable()) {
        const bool case_matches = Matches(c.title) || Matches(c.id);
        bool any = case_matches;
        for (const Mission &m : c.missions) any = any || Matches(m.title) || Matches(m.id);
        if (!any) continue;
        ImGui::PushID(int(c.hash));
        const bool here = c.missions.size() && c.missions[0].case_index == current_case;
        if (g_filter[0]) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        else if (here) ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
        if (ImGui::TreeNode("case", "%s  (%zu)", c.title.c_str(), c.missions.size())) {
            for (const Mission &m : c.missions) {
                if (!case_matches && !Matches(m.title) && !Matches(m.id)) continue;
                ImGui::PushID(m.mission_index);
                const bool current = m.case_index == current_case && m.mission_index == current_mission;
                ImGui::BeginDisabled(!gameplay);
                if (ImGui::SmallButton("Start")) {
                    RequestMission(m.case_index, m.mission_index);
                    Close();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (current)
                    ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.0f}, "%s  (current)", m.title.c_str());
                else
                    ImGui::TextUnformatted(m.title.c_str());
                if (m.title != m.id) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", m.id.c_str());
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

char g_stage_filter[64] = "";

bool StageMatches(const std::string &name) {
    if (!g_stage_filter[0]) return true;
    const size_t n = std::char_traits<char>::length(g_stage_filter);
    for (size_t i = 0; i + n <= name.size(); ++i) {
        size_t j = 0;
        while (j < n && std::tolower(uint8_t(name[i + j])) == std::tolower(uint8_t(g_stage_filter[j]))) ++j;
        if (j == n) return true;
    }
    return false;
}

// every live mission and character script, with its stages
void StagesTab() {
    if (!InGameplay()) {
        ImGui::TextDisabled("Load a save (and start a mission) first.");
        return;
    }
    ImGui::TextWrapped("Live scripts in the current level. \"Go\" switches a script to that stage, as if the "
                       "script had called SetState itself. Start the mission first, then jump.");
    bool trace = TraceScripts();
    if (ImGui::Checkbox("Trace script events to the log (stage changes, SetGrabbable, SetTimer)", &trace))
        SetTraceScripts(trace);
    const std::vector<Script> scripts = LiveScripts();
    // fast-forward: every running script with a "Die" stage goes to it, except bosses
    if (ImGui::Button("Defeat all enemies")) {
        constexpr uint32_t die = guest::Crc32("Die");
        auto bossy = [](const std::string &name) {
            std::string lower = name;
            for (char &ch : lower) ch = char(std::tolower(uint8_t(ch)));
            return lower.find("boss") != std::string::npos || lower.find("tommy") != std::string::npos;
        };
        for (const Script &s : scripts) {
            bool has_die = false, boss = s.owner_name && bossy(ScriptName(s.owner_name));
            for (uint32_t state : s.states) {
                has_die = has_die || state == die;
                boss = boss || bossy(ScriptName(state));
            }
            if (has_die && !boss && s.state != die) RequestState(s.instance, die);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(skips scripts that mention a boss or Tommy)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##stagefilter", "Filter stages (e.g. Tommy, Fight, Boss)", g_stage_filter,
                             sizeof(g_stage_filter));
    ImGui::BeginChild("scripts", {0, 0}, ImGuiChildFlags_Border);
    int shown = 0;
    for (const Script &s : scripts) {
        std::vector<std::string> names;
        bool any = false;
        for (uint32_t state : s.states) {
            names.push_back(ScriptName(state));
            any = any || StageMatches(names.back());
        }
        if (!any) continue;
        ++shown;
        ImGui::PushID(int(s.instance));
        const std::string current = ScriptName(s.state);
        if (g_stage_filter[0]) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        const bool open = s.owner ? ImGui::TreeNode("s", "%s  -  %s",
                                                    s.owner_name ? ScriptName(s.owner_name).c_str() : "Character",
                                                    current.c_str())
                                  : ImGui::TreeNode("s", "Mission script %08X  -  %s", s.instance, current.c_str());
        if (open) {
            for (size_t i = 0; i < s.states.size(); ++i) {
                if (!StageMatches(names[i])) continue;
                ImGui::PushID(int(i));
                if (ImGui::SmallButton("Go")) RequestState(s.instance, s.states[i]);
                ImGui::SameLine();
                if (s.states[i] == s.state)
                    ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.0f}, "%s  (current)", names[i].c_str());
                else
                    ImGui::TextUnformatted(names[i].c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (!shown) ImGui::TextDisabled(scripts.empty() ? "No scripts running." : "No stage matches the filter.");
    ImGui::EndChild();
}

void PlayerTab() {
    float health = 0, max_health = 0;
    if (!InGameplay() || !PlayerStatus(health, max_health)) {
        ImGui::TextDisabled("Load a save first.");
        return;
    }
    ImGui::Text("Health: %.0f / %.0f", health, max_health);
    bool god = GodMode(), ammo = InfiniteAmmo();
    if (ImGui::Checkbox("God mode (ignore damage)", &god)) SetGodMode(god);
    if (ImGui::Checkbox("Infinite ammo", &ammo)) SetInfiniteAmmo(ammo);
    if (ImGui::Button("Full health")) RequestHeal();
    ImGui::Separator();
    ImGui::TextUnformatted("Money");
    if (ImGui::Button("+$1,000")) RequestMoney(1000);
    ImGui::SameLine();
    if (ImGui::Button("+$10,000")) RequestMoney(10000);
    ImGui::SameLine();
    if (ImGui::Button("+$100,000")) RequestMoney(100000);
    int good = 0, bad = 0;
    if (CopPoints(good, bad)) ImGui::Text("Good Cop points: %d   Bad Cop points: %d", good, bad);
    if (ImGui::Button("+10 Good##cop")) RequestGoodCopPoints(10);
    ImGui::SameLine();
    if (ImGui::Button("+100 Good##cop")) RequestGoodCopPoints(100);
    ImGui::SameLine();
    if (ImGui::Button("+10 Bad##bad")) RequestBadCopPoints(10);
    ImGui::SameLine();
    if (ImGui::Button("+100 Bad##bad")) RequestBadCopPoints(100);
    ImGui::SameLine();
    if (ImGui::Button("Clear Bad##bad")) RequestBadCopPoints(-bad);
    ImGui::TextUnformatted("Career points (rank)");
    if (ImGui::Button("+100##career")) RequestCareerPoints(100);
    ImGui::SameLine();
    if (ImGui::Button("+1,000##career")) RequestCareerPoints(1000);
    ImGui::Separator();

    // weapon item names from the game's data (ScriptDev .def files); mission-only variants left out
    struct Group {
        const char *label;
        std::vector<const char *> items;
        bool left; // offer the left hand too (dual wielding)
    };
    static const Group kGroups[] = {
        {"Special", {"Special_RPG7", "Special_FThrower", "Special_ChainGun", "Special_M1", "Special_PepSpray",
                     "Special_Holdout"},
         false},
        {"Pistols", {"Pistol_1911", "Pistol_DE", "Pistol_P22", "Pistol_PT100", "Pistol_SOCOM", "Pistol_Taser"}, true},
        {"Shotguns",
         {"Shotgun_Beanbag", "Shotgun_M3", "Shotgun_Mariner", "Shotgun_Protecta", "Shotgun_Sawedoff", "Shotgun_Spas12"},
         false},
        {"SMGs", {"Smg_MP5", "Smg_MP7", "Smg_MiniUzi", "Smg_SAF", "Smg_Skorpion"}, true},
        {"Rifles", {"Arifle_AK47", "Arifle_AUG", "Arifle_CAR15", "Arifle_M4", "Arifle_OC14", "Arifle_Vektor"}, false},
        {"Melee",
         {"Melee_BballBat", "Melee_Blackjack", "Melee_BoStaff", "Melee_Crowbar", "Melee_Dagger", "Melee_Fireaxe",
          "Melee_Hatchet", "Melee_IcePick", "Melee_Kama", "Melee_Katana", "Melee_Machete", "Melee_MagLight",
          "Melee_MeatCleaver", "Melee_Naginata", "Melee_Nightstick", "Melee_Sai", "Melee_Screwdriver",
          "Melee_SlgHammer", "Melee_StunBaton", "Melee_Switchblade", "Melee_WoodSword", "Melee_Wrench"},
         false},
        {"Throwables", {"Throw_Caltrops", "Throw_Cocktail", "Throw_FlashBang", "Throw_Frag", "Throw_LandMine"}, false},
    };
    ImGui::TextUnformatted("Weapons");
    ImGui::TextDisabled("Equip clears Nick's weapons, then puts it in his hand. Left swaps only the left hand.");
    bool allow = AllowWeaponSwitch();
    if (ImGui::Checkbox("Allow guns in missions that block them", &allow)) SetAllowWeaponSwitch(allow);
    ImGui::TextDisabled("Some missions (e.g. Human Cargo) take Nick's guns at the start; equip after it loads.");
    for (const Group &g : kGroups) {
        if (!ImGui::TreeNode(g.label)) continue;
        for (const char *item : g.items) {
            ImGui::PushID(item);
            if (ImGui::SmallButton("Equip")) RequestItem(item);
            if (g.left) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Left")) RequestItem(item, true);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(std::strchr(item, '_') + 1);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void SpeedTab() {
    if (!InGame()) {
        ImGui::TextDisabled("The game has not started yet.");
        return;
    }
    const int speed = GameSpeed();
    ImGui::Text("Game speed: %s", speed == 0 ? "paused" : speed == 4 ? "normal" : "changed");
    struct Choice {
        const char *label;
        int quarters;
    };
    static constexpr Choice kChoices[] = {{"Pause", 0}, {"1/4x", 1}, {"1/2x", 2}, {"1x", 4}, {"2x", 8}, {"4x", 16}};
    for (const Choice &c : kChoices) {
        if (&c != kChoices) ImGui::SameLine();
        const bool active = speed == c.quarters;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(c.label)) SetGameSpeed(c.quarters);
        if (active) ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(speed != 0);
    if (ImGui::Button("Step one frame")) StepFrame();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Pause freezes the game but leaves this menu working. Fast speeds may skip frames.");
}

char g_log_filter[64] = "";
bool g_log_dev_only = false;

void LogTab() {
    static std::vector<std::string> lines;
    static double last_read = -10;
    const double now = ImGui::GetTime();
    if (now - last_read > 0.5) {
        last_read = now;
        lines.clear();
        if (FILE *f = std::fopen(logx::Path().c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            const long start = size > 65536 ? size - 65536 : 0;
            std::fseek(f, start, SEEK_SET);
            std::string text(size_t(size - start), '\0');
            text.resize(std::fread(text.data(), 1, text.size(), f));
            std::fclose(f);
            size_t pos = start ? text.find('\n') + 1 : 0;
            while (pos < text.size()) {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string line = text.substr(pos, end - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
                pos = end + 1;
            }
        }
    }
    ImGui::Checkbox("Debug menu lines only", &g_log_dev_only);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##logfilter", "Filter log", g_log_filter, sizeof(g_log_filter));
    ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string &line : lines) {
        if (g_log_dev_only && line.find("dev:") == std::string::npos) continue;
        if (g_log_filter[0] && line.find(g_log_filter) == std::string::npos) continue;
        const bool error = line.find("error") != std::string::npos;
        if (error) ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f}, "%s", line.c_str());
        else ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void WorldTab() {
    const int now = ClockMinutes();
    if (now < 0) {
        ImGui::TextDisabled("The game has not started yet.");
        return;
    }
    if (!g_clock_seeded) {
        g_hour = now / 60;
        g_minute = now % 60;
        g_clock_seeded = true;
    }
    ImGui::Text("Time of day: %02d:%02d", now / 60, now % 60);
    ImGui::SliderInt("Hour", &g_hour, 0, 23);
    ImGui::SliderInt("Minute", &g_minute, 0, 59);
    if (ImGui::Button("Set time")) RequestClock(g_hour, g_minute);
    ImGui::SameLine();
    if (ImGui::Button("Dawn")) RequestClock(6, 0);
    ImGui::SameLine();
    if (ImGui::Button("Noon")) RequestClock(12, 0);
    ImGui::SameLine();
    if (ImGui::Button("Dusk")) RequestClock(19, 0);
    ImGui::SameLine();
    if (ImGui::Button("Midnight")) RequestClock(0, 0);
}

void InfoTab() {
    ImGui::Text("Game ready: %s", InGame() ? "yes" : "no");
    ImGui::Text("In gameplay: %s", InGameplay() ? "yes" : "no");
    int c = -1, m = -1;
    if (CurrentMission(c, m)) {
        std::string case_name = "?", mission_name = "?";
        for (const Case &entry : StoryTable())
            for (const Mission &mission : entry.missions)
                if (mission.case_index == c && mission.mission_index == m) {
                    case_name = entry.title;
                    mission_name = mission.title;
                }
        ImGui::Text("Mission: %s / %s  (case %d, mission %d)", case_name.c_str(), mission_name.c_str(), c, m);
    }
    const int now = ClockMinutes();
    if (now >= 0) ImGui::Text("Time of day: %02d:%02d", now / 60, now % 60);
}

} // namespace

// Release packaging (scripts/package-release.ps1, scripts/test_no_dev_tools.py) refuses an
// executable that contains this string. It is logged once so it always stays in the binary.
constexpr char kDevBuildMarker[] = "TCNYC-DEV-TOOLS-BUILD";

void DrawMenu() {
    static bool announced = false;
    if (!announced) {
        announced = true;
        LOGR("dev: %s, debug menu on F9", kDevBuildMarker);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        if (g_open) Close();
        else Open();
    }
    // always visible, so screenshots and bug reports show which build they came from
    ImGui::GetForegroundDrawList()->AddText({8.0f, ImGui::GetIO().DisplaySize.y - 22.0f},
                                            IM_COL32(255, 200, 80, 200), "DEV BUILD  (F9: debug menu)");
    if (!g_open) return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize({520, size.y * 0.7f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({size.x * 0.5f, size.y * 0.5f}, ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    bool open = true;
    if (ImGui::Begin("Debug menu (F9)", &open, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Missions")) {
                MissionsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Stages")) {
                StagesTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Player")) {
                PlayerTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Speed")) {
                SpeedTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("World")) {
                WorldTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Info")) {
                InfoTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Log")) {
                LogTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) Close();
}

} // namespace dev
