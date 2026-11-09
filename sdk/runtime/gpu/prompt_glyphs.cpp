// Device-matched button prompts: the game's font atlas glyphs redrawn for the
// active controller family or for keyboard and mouse. Shared by every renderer.
#include "gpu/prompt_glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace gpu {

// The game's inline button glyphs share a linear font atlas with regular text.
bool IsPromptAtlas(const TextureDesc& d) {
    return d.format == 0x1D && d.width == 128 && d.height == 448 && d.levels == 1 && !d.cube && d.depth == 1;
}

void ReplaceControllerGlyphs(std::vector<uint8_t>& px, uint32_t pitch, input::PromptStyle style) {
    auto pixel = [&](int x, int y) { return px.data() + y * pitch + x * 4; };
    // Match the four colored glyphs before editing a texture with this layout.
    const uint8_t colors[][3] = {{102, 238, 119}, {51, 153, 238}, {51, 51, 238}, {238, 102, 68}};
    const int centers[] = {8, 26, 44, 62};
    for (int i = 0; i < 4; ++i) {
        if (!std::equal(std::begin(colors[i]), std::end(colors[i]), pixel(centers[i] - 6, 240))) return;
    }
    // Compact 3x5 keycap labels, drawn at double height, fit the original glyph advance and UVs.
    struct Label { int x, y, w, h; char text[8]; };
    auto draw_labels = [&](const Label* labels, size_t n) {
        constexpr struct { char key; uint8_t rows[5]; } alphabet[] = {
            {'A', {2, 5, 7, 5, 5}}, {'B', {6, 5, 6, 5, 6}}, {'C', {7, 4, 4, 4, 7}}, {'E', {7, 4, 6, 4, 7}}, {'F', {7, 4, 6, 4, 4}},
            {'G', {7, 4, 5, 5, 7}}, {'L', {4, 4, 4, 4, 7}}, {'M', {5, 7, 7, 5, 5}},
            {'P', {6, 5, 6, 4, 4}}, {'Q', {7, 5, 5, 7, 1}}, {'R', {6, 5, 6, 5, 5}},
            {'S', {7, 4, 7, 1, 7}}, {'T', {7, 2, 2, 2, 2}},
            {'D', {6, 5, 5, 5, 6}}, {'H', {5, 5, 7, 5, 5}}, {'I', {7, 2, 2, 2, 7}},
            {'J', {1, 1, 1, 5, 7}}, {'K', {5, 5, 6, 5, 5}}, {'N', {5, 7, 7, 7, 5}},
            {'O', {7, 5, 5, 5, 7}}, {'U', {5, 5, 5, 5, 7}}, {'V', {5, 5, 5, 5, 2}},
            {'W', {5, 5, 7, 7, 5}}, {'X', {5, 5, 2, 5, 5}}, {'Y', {5, 5, 2, 2, 2}},
            {'Z', {7, 1, 2, 4, 7}}, {'7', {7, 1, 2, 2, 2}}, {'9', {7, 5, 7, 1, 7}}, {'+', {0, 2, 7, 2, 0}},
            {'-', {0, 0, 7, 0, 0}},
        };
        for (size_t k = 0; k < n; ++k) {
            const Label& key = labels[k];
            for (int y = 0; y < key.h; ++y)
                for (int x = 0; x < key.w; ++x)
                    std::fill_n(pixel(key.x + x, key.y + y), 4, 0);
            const int count = int(std::strlen(key.text));
            const int left = key.x + (key.w - (4 * count - 1)) / 2;
            const int top = key.y + (key.h - 10) / 2;
            for (int i = 0; i < count; ++i) {
                for (const auto& glyph : alphabet) {
                    if (glyph.key != key.text[i]) continue;
                    for (int y = 0; y < 10; ++y)
                        for (int x = 0; x < 3; ++x)
                            if (glyph.rows[y / 2] & (4 >> x))
                                std::fill_n(pixel(left + 4 * i + x, top + y), 4, 255);
                }
            }
        }
    };
    // BACK and START artwork reaches past the keycap slots; these cover all of it
    constexpr int kBackX = 99, kBackY = 278, kBackW = 25, kBackH = 20;
    constexpr int kStartX = 0, kStartY = 301, kStartW = 27, kStartH = 20;  // the "¡" glyph starts at x 27
    // the three-line Menu / Options icon in the cleared START slot
    auto menu_icon = [&] {
        for (int line = 0; line < 3; ++line)
            for (int y = 0; y < 2; ++y)
                for (int x = 7; x < 20; ++x) std::fill_n(pixel(kStartX + x, kStartY + 4 + line * 5 + y), 4, 255);
    };
    if (style == input::PromptStyle::KeyboardMouse || style == input::PromptStyle::KeyboardDriving) {
        // Face buttons follow input.cpp: on foot A Ctrl, B V, X Alt (or the heavy-attack key),
        // Y Space; in a vehicle A is W (accelerate), B Space (handbrake), X S (brake), Y unbound.
        const bool driving = style == input::PromptStyle::KeyboardDriving;
        Label heavy{55, 234, 15, 14, "ALT"};
        if (char key = input::LetterKey(input::g_heavy_attack_key, 0)) heavy = {55, 234, 15, 14, {key}};
        const Label keys[] = {
            driving ? Label{1, 234, 15, 14, "W"} : Label{1, 234, 15, 14, "CTL"},
            driving ? Label{19, 234, 15, 14, "-"} : Label{19, 234, 15, 14, "SPC"},
            driving ? Label{37, 234, 15, 14, "SPC"} : Label{37, 234, 15, 14, "V"},
            driving ? Label{55, 234, 15, 14, "S"} : heavy,
            {1, 279, 14, 15, {input::LetterKey(input::g_use_key, 'E')}}, {37, 279, 13, 15, {input::LetterKey(input::g_reload_key, 'R')}},
            {101, 256, 13, 18, "RMB"}, {19, 279, 13, 18, "LMB"},
            {kBackX, kBackY, kBackW, kBackH, "Q"}, {kStartX, kStartY, kStartW, kStartH, "ESC"},
            {53, 279, 26, 19, {input::kLeftStickClickKey}}, {73, 233, 28, 19, "SHF"},
            // the left stick glyph: movement (steering in a vehicle)
            {84, 301, 21, 20, "WASD"},
        };
        draw_labels(keys, std::size(keys));
        // The individual D-pad slots become keyboard arrows.
        for (int direction = 0; direction < 4; ++direction) {
            const int center = 9 + direction * 20;
            for (int y = -8; y <= 9; ++y) {
                for (int x = -8; x <= 8; ++x) {
                    const bool vertical = direction < 2;
                    const int along = vertical ? y : x;
                    const int across = std::abs(vertical ? x : y);
                    const int forward = direction % 2 == 0 ? -along : along;
                    const bool arrow = (std::abs(along) <= 5 && across <= 1) ||
                                       (forward >= 0 && forward <= 5 && std::abs(forward + across - 5) <= 1);
                    std::fill_n(pixel(center + x, 264 + y), 4, arrow ? 255 : 0);
                }
            }
        }
        // Right-stick movement uses a mouse outline in keyboard mode.
        constexpr int kMouseCenter = 111;
        for (int y = -7; y <= 8; ++y) {
            for (int x = -7; x <= 7; ++x) {
                const int dx = std::abs(x);
                const bool outline = (dx == 4 && y >= -4 && y <= 4) ||
                                     (dx <= 3 && (y == -5 || y == 5)) ||
                                     (x == 0 && y >= -4 && y <= 0);
                std::fill_n(pixel(kMouseCenter + x, 241 + y), 4, outline ? 255 : 0);
            }
        }
        // The menu legend uses the neutral D-pad glyph for directional selection.
        for (int y = 256; y < 274; ++y) {
            for (int x = 81; x < 98; ++x) {
                const int dx = std::abs(x - 89);
                const int dy = std::abs(y - 264);
                const bool arrow = (dx <= 1 && dy <= 6) || (dy <= 1 && dx <= 6) ||
                                   (dx >= 4 && dx + dy <= 7) || (dy >= 4 && dx + dy <= 7);
                std::fill_n(pixel(x, y), 4, arrow ? 255 : 0);
            }
        }
        return;
    }
    constexpr const char* glyphs[][9] = {
        {"##.....##", "###...###", ".###.###.", "..#####..", "...###...", "..#####..", ".###.###.", "###...###", "##.....##"},
        {"....#....", "...###...", "...#.#...", "..##.##..", "..#...#..", ".##...##.", ".#.....#.", "#########", "#########"},
        {"..#####..", ".##...##.", "##.....##", "#.......#", "#.......#", "#.......#", "##.....##", ".##...##.", "..#####.."},
        {"#########", "#########", "##.....##", "##.....##", "##.....##", "##.....##", "##.....##", "#########", "#########"},
    };
    constexpr uint8_t ink[][3] = {{255, 195, 162}, {183, 243, 125}, {157, 161, 255}, {255, 176, 240}};
    if (style == input::PromptStyle::PlayStation) {
        for (int i = 0; i < 4; ++i) {
            for (int y = 234; y <= 247; ++y) {
                for (int x = -7; x <= 7; ++x) {
                    auto* p = pixel(centers[i] + x, y);
                    const int dy = y - 240;
                    const bool symbol = dy >= -4 && dy <= 4 && x >= -4 && x <= 4 &&
                                        glyphs[i][dy + 4][x + 4] == '#';
                    if (symbol) {
                        std::copy_n(ink[i], 3, p);
                        p[3] = 255;
                    } else {
                        std::fill_n(p, 3, 28);
                    }
                }
            }
        }
        // DualShock 4 / DualSense names for the Back and Start buttons
        const Label menu[] = {{kBackX, kBackY, kBackW, kBackH, "SHARE"}, {kStartX, kStartY, kStartW, kStartH, ""}};
        draw_labels(menu, std::size(menu));
        menu_icon();
    }
    if (style == input::PromptStyle::SteamDeck) {
        // The Deck's face buttons sit where the Xbox ones do, as monochrome lettered buttons:
        // a dark disc with a light rim and a bold 3x5 letter drawn at double size
        constexpr struct { uint8_t rows[5]; } face[] = {
            {{2, 5, 7, 5, 5}}, {{5, 5, 2, 2, 2}}, {{6, 5, 6, 5, 6}}, {{5, 5, 2, 5, 5}},  // A Y B X
        };
        for (int i = 0; i < 4; ++i) {
            for (int y = 234; y <= 247; ++y) {
                for (int x = -7; x <= 7; ++x) {
                    auto* p = pixel(centers[i] + x, y);
                    const float dy = float(y) - 240.5f;
                    const float r = std::sqrt(float(x * x) + dy * dy);
                    const int lx = x + 3, ly = y - 236;  // 6x10 letter box
                    const bool letter = lx >= 0 && lx < 6 && ly >= 0 && ly < 10 &&
                                        (face[i].rows[ly / 2] & (4 >> (lx / 2)));
                    const uint8_t shade = letter ? 255 : r > 6.0f ? 200 : 50;
                    std::fill_n(p, 3, shade);
                    p[3] = r > 7.0f ? 0 : 255;
                }
            }
        }
        // View (two overlapping windows) and Menu (three lines)
        const Label menu[] = {{kBackX, kBackY, kBackW, kBackH, ""}, {kStartX, kStartY, kStartW, kStartH, ""}};
        draw_labels(menu, std::size(menu));
        auto frame = [&](int x0, int y0) {
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 9; ++x)
                    if (y == 0 || y == 6 || x == 0 || x == 8) std::fill_n(pixel(x0 + x, y0 + y), 4, 255);
        };
        frame(kBackX + 6, kBackY + 4);
        frame(kBackX + 10, kBackY + 8);
        menu_icon();
    }
    // Xbox White/Black are the left/right shoulders. The boxed L/R are triggers.
    constexpr const char* letters[][7] = {
        {"#..", "#..", "#..", "#..", "#..", "#..", "###"},
        {"##.", "#.#", "#.#", "##.", "#.#", "#.#", "#.#"},
    };
    constexpr const char* suffixes[][7] = {
        {".#.", "##.", ".#.", ".#.", ".#.", ".#.", "###"},
        {"###", "..#", "..#", "###", "#..", "#..", "###"},
        {"##.", "#.#", "#.#", "##.", "#.#", "#.#", "##."},
        {"###", ".#.", ".#.", ".#.", ".#.", ".#.", ".#."},
    };
    constexpr struct { int x, y, w, h, side, trigger; } labels[] = {
        {1, 279, 14, 15, 0, 0}, {37, 279, 13, 15, 1, 0},
        {101, 256, 13, 18, 0, 1}, {19, 279, 13, 18, 1, 1},
    };
    const int suffix_offset = style == input::PromptStyle::Xbox ? 2 : 0;
    for (const auto& label : labels) {
        for (int y = 0; y < label.h; ++y) {
            for (int x = 0; x < label.w; ++x) {
                const int row = y - (label.h - 7) / 2;
                const int column = x - (label.w - 7) / 2;
                const bool in_text = row >= 0 && row < 7 && column >= 0 && column < 7;
                const bool ink = in_text &&
                                 (column < 3 ? letters[label.side][row][column] == '#' :
                                  column > 3 && suffixes[suffix_offset + label.trigger][row][column - 4] == '#');
                auto* p = pixel(label.x + x, label.y + y);
                std::fill_n(p, 4, ink ? 255 : 0);
            }
        }
    }
}

} // namespace gpu
