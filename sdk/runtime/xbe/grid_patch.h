#pragma once
// City-cell grid remainder fix, shared by the CMake generation step (tools/grid_patch) and the
// first-launch compiler (runtime/app/game_code.cpp), so both paths produce identical code.
//
// The grid routines divide (coordinate + 1000) with signed idiv and use the remainder as a 3x3
// bucket. A negative coordinate gives a negative remainder; the index becomes -1 and the 16-byte
// record is written over the end of cell slot 8 (+0x70/+0x74), corrupting its pointers and crashing
// the renderer. Only the divisions at the guest addresses below feed that index; make their
// remainder the mathematical modulo by adding the divisor when it is negative. Non-negative
// dividends are unchanged and the quotient is discarded in these routines.
//
// The sites are explicit. Before changing anything the patcher verifies, for every occurrence, the
// guest address, the idiv operand, the divisor load and the divide-error trap in the generated
// block. It also requires the exact number of occurrences, so a recompiler change that moves or
// splits these routines fails the build instead of patching an unrelated division.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace xbe {

// Guest idiv instructions whose remainder feeds the 3x3 cell index. The recompiler emits
// continuation entries for some functions, so the same address appears in more than one host
// function; `occurrences` is the count across the configured partitions for this title.
struct GridSite {
    uint32_t address;
    const char* operand;  // idiv operand, which must also be the divisor loaded into d
    uint32_t occurrences;
};

inline constexpr GridSite kGridSites[] = {
    {0x00138BCD, "ecx", 1},
    {0x00138BE4, "edi", 1},
    {0x0013F212, "ecx", 1},
    {0x0013F229, "edi", 1},
    {0x00146D30, "esi", 3},
    {0x00146D7D, "ecx", 3},
    {0x00147053, "esi", 2},
    {0x0014709D, "ecx", 2},
    {0x00147373, "edi", 2},
    {0x001473BD, "ecx", 2},
    {0x0014A5D1, "ecx", 1},
    {0x0014A5F8, "esi", 1},
};
inline constexpr uint32_t kGridSiteCount = sizeof(kGridSites) / sizeof(kGridSites[0]);
inline constexpr uint32_t kGridExpectedOccurrences = 20;

struct GridPatchResult {
    uint32_t files = 0;
    uint32_t occurrences = 0;
    std::string error;
    bool ok() const { return error.empty(); }
};

inline std::string ToHex(uint32_t value, int digits = 8) {
    char text[16];
    std::snprintf(text, sizeof(text), "%0*X", digits, value);
    return text;
}

inline bool ParseHex(const std::string& text, size_t at, size_t count, uint32_t& out) {
    if (at + count > text.size()) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < count; i++) {
        const char ch = text[at + i];
        const int digit = ch >= '0' && ch <= '9' ? ch - '0'
                            : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                            : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                                     : -1;
        if (digit < 0) return false;
        value = value * 16 + uint32_t(digit);
    }
    out = value;
    return true;
}

inline const GridSite* GridSiteAt(uint32_t address) {
    for (const GridSite& site : kGridSites)
        if (site.address == address) return &site;
    return nullptr;
}

// Applies the fix to dir/x86_recomp.0..partitions-1.cpp. Every expected partition, site and
// occurrence is required; files are replaced through a temporary file, and any mismatch is an
// error. Calling it again on already-corrected sources verifies and accepts them.
inline GridPatchResult PatchGridSources(const std::filesystem::path& dir, uint32_t partitions) {
    GridPatchResult result;
    if (!partitions) {
        result.error = "the image has no recompiled partitions";
        return result;
    }
    const std::string plain = "ctx.edx = (uint32_t)(n % d); }";
    const std::string fixed =
        "ctx.edx = (uint32_t)(n % d); if ((int32_t)ctx.edx < 0) ctx.edx += (uint32_t)d; }";
    std::vector<uint32_t> seen(kGridSiteCount, 0);
    for (uint32_t part = 0; part < partitions; part++) {
        const std::filesystem::path file = dir / ("x86_recomp." + std::to_string(part) + ".cpp");
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            result.error = "cannot read " + file.string();
            return result;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const bool trailing_newline = !text.empty() && text.back() == '\n';
        std::vector<std::string> lines;
        {
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line)) lines.push_back(line);
        }
        bool changed = false;
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string& line = lines[i];
            const size_t comment = line.find("// ");
            if (comment == std::string::npos) continue;
            uint32_t address = 0;
            if (!ParseHex(line, comment + 3, 8, address)) continue;
            if (line.compare(comment + 12, 4, "idiv") != 0) continue;
            const GridSite* site = GridSiteAt(address);
            if (!site) continue;
            const size_t operand_at = comment + 16;
            if (operand_at >= line.size() || line[operand_at] != ' ') {
                result.error = "malformed idiv comment at guest " + ToHex(address);
                return result;
            }
            const size_t operand_end = line.find_first_of(" \t\r", operand_at + 1);
            const std::string operand =
                line.substr(operand_at + 1, operand_end == std::string::npos ? std::string::npos
                                                                             : operand_end - operand_at - 1);
            if (operand != site->operand) {
                result.error = "guest " + ToHex(address) + " divides by " + operand + ", expected " +
                               site->operand;
                return result;
            }
            size_t target = i + 1;
            while (target < lines.size() && lines[target].find_first_not_of(" \t\r") == std::string::npos) target++;
            if (target == lines.size()) {
                result.error = "the idiv block after guest " + ToHex(address) + " is missing";
                return result;
            }
            const std::string& block = lines[target];
            const std::string divisor = "int32_t d = (int32_t)ctx." + operand + ";";
            const std::string trap = "x86_trap(ctx, 0x" + ToHex(address, 1) + ", \"divide error\");";
            if (block.find("int64_t n = (int64_t)") == std::string::npos ||
                block.find(divisor) == std::string::npos || block.find(trap) == std::string::npos) {
                result.error = "guest " + ToHex(address) + " does not match the expected idiv block";
                return result;
            }
            const size_t at = block.find(plain);
            const size_t already = block.find(fixed);
            if (at != std::string::npos) {
                lines[target].replace(at, plain.size(), fixed);
                changed = true;
            } else if (already == std::string::npos) {
                result.error = "unexpected idiv remainder after guest " + ToHex(address);
                return result;
            }
            const size_t index = size_t(site - kGridSites);
            seen[index]++;
            result.occurrences++;
        }
        if (changed) {
            std::filesystem::path temp = file;
            temp += ".gridtmp";
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                result.error = "cannot write " + temp.string();
                return result;
            }
            for (size_t i = 0; i < lines.size(); i++) {
                out << lines[i];
                if (i + 1 < lines.size() || trailing_newline) out << '\n';
            }
            out.close();
            if (!out) {
                result.error = "write failed for " + temp.string();
                std::error_code ignored;
                std::filesystem::remove(temp, ignored);
                return result;
            }
            std::error_code ec;
            std::filesystem::remove(file, ec);
            if (ec) {
                result.error = "removing " + file.string() + " failed: " + ec.message();
                std::filesystem::remove(temp, ec);
                return result;
            }
            std::filesystem::rename(temp, file, ec);
            if (ec) {
                result.error = "replacing " + file.string() + " failed: " + ec.message();
                std::filesystem::remove(temp, ec);
                return result;
            }
        }
        result.files++;
    }
    for (uint32_t i = 0; i < kGridSiteCount; i++) {
        if (seen[i] != kGridSites[i].occurrences) {
            result.error = "guest " + ToHex(kGridSites[i].address) + " appears " + std::to_string(seen[i]) +
                           " time(s), expected " + std::to_string(kGridSites[i].occurrences);
            return result;
        }
    }
    if (result.occurrences != kGridExpectedOccurrences) {
        result.error = "patched " + std::to_string(result.occurrences) + " sites, expected " +
                       std::to_string(kGridExpectedOccurrences);
        return result;
    }
    return result;
}

}  // namespace xbe
