#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

#include "app/app.h"
#include "app/settings_store.h"

int main() {
    using namespace app;
    namespace fs = std::filesystem;
    const auto tag = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::current_path() / ("settings-test-" + std::to_string(tag));
    const auto file = dir / "settings.toml";
    int result = 1;
    // language names round-trip to the console's codes; an unknown name is rejected
    for (uint32_t code = 1; code < std::size(kLanguages); code++)
        if (LanguageCode(LanguageName(code)) != code) {
            std::cerr << "language " << code << " does not round-trip\n";
            return 1;
        }
    if (LanguageCode("klingon") || LanguageCode("spanish") != 5 || std::string(LanguageName(0)) != "english") {
        std::cerr << "language lookup is wrong\n";
        return 1;
    }
    try {
        const toml::table initial{{"ao", 0}, {"external", true},
            {"controls", toml::table{{"keyboard", toml::table{{"use", toml::array{"E"}},
                                                           {"reload", toml::array{"R"}}}}}}};
        MergeSettingsTable(file, initial);
        auto edited = initial;
        edited["controls"]["keyboard"].as_table()->insert_or_assign("reload", toml::array{"Y"});
        const auto delta = ChangedSettings(initial, edited);
        auto first = std::async(std::launch::async, [&] { MergeSettingsTable(file, delta); });
        MergeSettingsTable(file, toml::table{{"ao", 2}, {"ao_method", 1},
            {"controls", toml::table{{"keyboard", toml::table{{"use", toml::array{"F"}}}}}}});
        first.get();
        const auto saved = ReadSettingsTable(file);
        const auto keyboard = saved["controls"]["keyboard"];
        if (saved["ao"].value_or(0) == 2 && saved["ao_method"].value_or(0) == 1 &&
            saved["external"].value_or(false) && keyboard["use"][0].value_or(std::string("")).compare("F") == 0 &&
            keyboard["reload"][0].value_or(std::string("")).compare("Y") == 0) result = 0;
        Settings settings;
        LoadOcclusionSettings(settings, toml::table{{"ao", true}});
        if (settings.ao != 2 || settings.ao_quality != 2) result = 1;
        LoadOcclusionSettings(settings, toml::table{{"ao", 2}, {"ao_method", 1}, {"ao_quality", 1}});
        if (settings.ao != 1 || settings.ao_method != 1 || settings.ao_quality != 1) result = 1;
        LoadOcclusionSettings(settings, toml::table{{"ao", 0}, {"ao_method", 1}, {"ao_quality", 1}});
        if (settings.ao != 0 || settings.ao_method != 1 || settings.ao_quality != 1) result = 1;
        // Legacy msaa = true means 4x; explicit counts are 2, 4 or 8, and a
        // missing key keeps the default.
        if (MsaaSamplesFromTable(toml::table{{"msaa", true}}, 8) != 4) result = 1;
        if (MsaaSamplesFromTable(toml::table{{"msaa", false}}, 8) != 0) result = 1;
        for (const auto samples : {2, 4, 8}) {
            if (MsaaSamplesFromTable(toml::table{{"msaa", samples}}, 0) != uint32_t(samples)) result = 1;
        }
        if (MsaaSamplesFromTable(toml::table{{"msaa", 3}}, 8) != 0) result = 1;
        if (MsaaSamplesFromTable(toml::table{{"msaa", 1}}, 8) != 0) result = 1;
        if (MsaaSamplesFromTable(toml::table{}, 8) != 8) result = 1;
        if (result) std::cerr << saved << '\n';

        // A file that does not parse is moved aside and rebuilt from the full settings plus the edits.
        const auto broken = dir / "broken.toml";
        { std::ofstream(broken, std::ios::binary) << "ao = [unterminated\n"; }
        const toml::table full{{"ao", 1}, {"external", true}};
        MergeSettingsTable(broken, toml::table{{"ao", 2}}, &full);
        const auto rebuilt = ReadSettingsTable(broken);
        int asides = 0;
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().filename().string().starts_with("broken.toml.corrupt-")) ++asides;
        if (rebuilt["ao"].value_or(0) != 2 || !rebuilt["external"].value_or(false) || asides != 1) {
            std::cerr << "corrupt recovery: " << rebuilt << " asides " << asides << '\n';
            result = 1;
        }

        const auto async_file = dir / "async.toml";
        const toml::table loaded{{"ao", 0}, {"fps_cap", 30}};
        MergeSettingsTable(async_file, loaded);
        const auto lock_path = dir / "async.toml.lock";
        fs::remove(lock_path);
        fs::create_directory(lock_path);  // force the worker to fail to open the lock
        auto current = loaded;
        current.insert_or_assign("ao", 2);
        PublishSettingsAsync(async_file, loaded, current);
        FlushSettings();
        if (ReadSettingsTable(async_file)["ao"].value_or(-1) != 0) result = 1;
        fs::remove(lock_path);

        // An unchanged retry must keep the failed edit and the launcher's new FPS cap.
        MergeSettingsTable(async_file, toml::table{{"fps_cap", 60}});
        PublishSettingsAsync(async_file, loaded, current);
        FlushSettings();
        const auto retried = ReadSettingsTable(async_file);
        if (retried["ao"].value_or(0) != 2 || retried["fps_cap"].value_or(0) != 60) result = 1;

        // The following save must diff against the successfully published snapshot.
        MergeSettingsTable(async_file, toml::table{{"ao", 1}});
        current.insert_or_assign("ao_method", 1);
        PublishSettingsAsync(async_file, loaded, current);
        FlushSettings();
        const auto next = ReadSettingsTable(async_file);
        if (next["ao"].value_or(0) != 1 || next["ao_method"].value_or(0) != 1) result = 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    return result;
}
