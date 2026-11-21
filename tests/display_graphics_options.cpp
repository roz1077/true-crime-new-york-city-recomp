#include "game/display_rows.h"

#include <cstring>

int main() {
    using namespace game::display_rows;
    using namespace game::choices;
    constexpr auto main = Page::Main;
    if (RowOf(main, Option::CityOuterDetail) != 0 ||
        RowCount(main, true) != RowCount(main) + 1 ||
        OptionAt(main, RowOf(main, Option::CityOuterDetail, true), true) != Option::CityOuterDetail)
        return 1;

    Values values{};
    auto check_choice = [&](Option option, std::span<const uint32_t> choices, uint32_t Values::*field, bool outer = false) {
        const auto row = RowOf(main, option, outer);
        if (row == 0 || OptionAt(main, row, outer) != option) return false;
        values.*field = choices.front();
        for (const auto expected : choices) {
            if (values.*field != expected) return false;
            char text[kTextBytes];
            Text(values, option, text);
            if (!*text || std::strlen(text) > kMaxTextChars || std::strchr(text, '%')) return false;
            const auto step = Input(main, row, kRight, outer);
            if (!step.handled || step.change != 1) return false;
            Change(values, option, step.change, {});
        }
        return values.*field == choices.back();
    };
    if (!check_choice(Option::Gamma, kGammaPercents, &Values::gamma) ||
        !check_choice(Option::Brightness, kBrightnessPercents, &Values::brightness) ||
        !check_choice(Option::Contrast, kContrastPercents, &Values::contrast) ||
        !check_choice(Option::Saturation, kSaturationPercents, &Values::saturation) ||
        !check_choice(Option::PedDetail, kPedDetails, &Values::ped_detail) ||
        !check_choice(Option::PedLimit, kPedLimits, &Values::ped_limit) ||
        !check_choice(Option::PedDensity, kPedDensities, &Values::ped_density) ||
        !check_choice(Option::TrafficDensity, kTrafficDensities, &Values::traffic_density) ||
        !check_choice(Option::TrafficDistance, kTrafficDistances, &Values::traffic_distance) ||
        !check_choice(Option::CityInnerDetail, kCityInnerDetail, &Values::city_inner_detail) ||
        !check_choice(Option::CityOuterDetail, kCityOuterDetail, &Values::city_outer_detail, true))
        return 2;

    Change(values, Option::DumpFormat, 1, {});
    if (Enabled(values, Option::DumpFormat) || values.texture_dump_format) return 3;
    Change(values, Option::TextureDumping, 1, {});
    Change(values, Option::DumpFormat, 1, {});
    Change(values, Option::TextureReplacement, 1, {});
    if (!Enabled(values, Option::DumpFormat) || !values.dump_textures || values.texture_dump_format != 1 ||
        values.custom_textures)
        return 4;
    Change(values, Option::DumpFormat, -1, {});
    if (values.texture_dump_format != 0) return 5;

    for (bool outer : {false, true}) {
        const auto reset = RowOf(main, Option::Defaults, outer);
        if (!Input(main, reset, kAccept, outer).reset || Input(main, reset, kRight, outer).change ||
            Scroll(reset, 0) + kShownRows != RowCount(main, outer))
            return 6;
        const auto hdr = RowOf(main, Option::HdrPage, outer);
        if (Input(Page::Hdr, 0, kBack, outer).row != hdr) return 7;
    }
    for (bool enabled : {false, true}) {
        values.custom_textures = values.dump_textures = enabled;
        values.texture_dump_format = uint32_t(enabled);
        for (const auto option : {Option::TextureReplacement, Option::TextureDumping, Option::DumpFormat, Option::Defaults}) {
            if (!RowOf(main, option)) return 8;
            char text[kTextBytes];
            Text(values, option, text);
            if (!*text || std::strlen(text) > kMaxTextChars || std::strchr(text, '%')) return 9;
        }
    }
    if (GradeActive(100, 100, 100, 100) || !GradeActive(150, 100, 100, 100) ||
        !GradeActive(100, 150, 100, 100) || !GradeActive(100, 100, 150, 100) ||
        !GradeActive(100, 100, 100, 150))
        return 10;
    if (GradeGamma(200) != 2.0f || GradeGamma(50) != 0.5f || GradeBrightness(150) != 1.5f ||
        GradeContrast(50) != 0.5f || GradeSaturation(0) != 0.0f || GradeSaturation(200) != 2.0f)
        return 11;
}
