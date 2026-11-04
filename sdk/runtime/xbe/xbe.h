#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace xbe {

// the header XORs the entry point and kernel thunk address with these
constexpr uint32_t kRetailEntryKey = 0xA8FC57AB;
constexpr uint32_t kDebugEntryKey = 0x94859D4B;
constexpr uint32_t kRetailThunkKey = 0x5B6D40B6;
constexpr uint32_t kDebugThunkKey = 0xEFB1F152;

enum SectionFlags : uint32_t {
    kWritable = 1,
    kPreload = 2,
    kExecutable = 4,
    kInsertedFile = 8,
};

struct Section {
    std::string name;
    uint32_t flags = 0;
    uint32_t va = 0, vsize = 0;
    uint32_t raw = 0, rsize = 0;
};

struct Library {
    std::string name;
    uint16_t major = 0, minor = 0, build = 0, flags = 0;
};

struct Tls {
    uint32_t data_start = 0, data_end = 0, index_addr = 0, callbacks = 0, zero_fill = 0, characteristics = 0;
};

struct Image {
    std::vector<uint8_t> file;
    uint32_t base = 0, header_size = 0, image_size = 0;
    uint32_t entry = 0, kernel_thunk = 0, tls_addr = 0;
    uint32_t stack_commit = 0, heap_reserve = 0, heap_commit = 0;
    uint32_t cert = 0, title_id = 0, init_flags = 0;
    bool debug = false;
    std::vector<Section> sections;
    std::vector<Library> libraries;
    Tls tls;

    bool InFile(uint64_t off, uint64_t len) const {
        return off <= file.size() && len <= file.size() - off;
    }

    uint32_t U32(uint64_t off) const {
        uint32_t v = 0;
        if (InFile(off, 4)) std::memcpy(&v, file.data() + off, 4);
        return v;
    }

    // file offset for a virtual address, -1 if unmapped or in the zero filled tail
    int64_t Offset(uint32_t va) const {
        if (va >= base && uint64_t(va) - base < header_size && InFile(uint64_t(va) - base, 1))
            return int64_t(uint64_t(va) - base);
        for (auto& s : sections) {
            if (va >= s.va && uint64_t(va) - s.va < s.rsize) {
                const uint64_t o = uint64_t(s.raw) + (uint64_t(va) - s.va);
                return InFile(o, 1) ? int64_t(o) : -1;
            }
        }
        return -1;
    }

    const Section* SectionAt(uint32_t va) const {
        for (auto& s : sections)
            if (va >= s.va && uint64_t(va) - s.va < s.vsize) return &s;
        return nullptr;
    }

    bool Read32(uint32_t va, uint32_t& out) const {
        int64_t o = Offset(va);
        if (o < 0 || !InFile(uint64_t(o), 4)) return false;
        out = U32(uint64_t(o));
        return true;
    }

    std::string CStr(uint32_t va) const {
        int64_t o = Offset(va);
        if (o < 0) return {};
        std::string s;
        while (size_t(o) < file.size() && file[size_t(o)]) s += char(file[size_t(o++)]);
        return s;
    }

    bool Parse(std::vector<uint8_t> data, std::string& error) {
        file = std::move(data);
        sections.clear();
        libraries.clear();
        tls = {};
        debug = false;
        title_id = 0;
        if (file.size() < 0x180 || std::memcmp(file.data(), "XBEH", 4)) {
            error = "not an XBE";
            return false;
        }
        base = U32(0x104);
        header_size = U32(0x108);
        image_size = U32(0x10C);
        cert = U32(0x118);
        uint32_t nsec = U32(0x11C), sec_addr = U32(0x120);
        init_flags = U32(0x124);
        uint32_t ep = U32(0x128), kt = U32(0x158);
        tls_addr = U32(0x12C);
        stack_commit = U32(0x130);
        heap_reserve = U32(0x134);
        heap_commit = U32(0x138);
        // The image addresses are 32 bit and every span below is checked before it is read.
        const uint64_t image_end = uint64_t(base) + image_size;
        if (!base || header_size < 0x178 || header_size > file.size() || image_size < header_size ||
            image_end > 0x100000000ull) {
            error = "header sizes are invalid";
            return false;
        }
        auto inside = [&](uint32_t v) { return v >= base && uint64_t(v) < image_end; };
        if (inside(ep ^ kRetailEntryKey) && inside(kt ^ kRetailThunkKey)) {
            entry = ep ^ kRetailEntryKey;
            kernel_thunk = kt ^ kRetailThunkKey;
        } else if (inside(ep ^ kDebugEntryKey) && inside(kt ^ kDebugThunkKey)) {
            entry = ep ^ kDebugEntryKey;
            kernel_thunk = kt ^ kDebugThunkKey;
            debug = true;
        } else {
            error = "entry point key not recognised";
            return false;
        }
        if (nsec > 1024 || (nsec && sec_addr < base)) {
            error = "section table is invalid";
            return false;
        }
        const uint64_t sec_off = uint64_t(sec_addr) - base;
        if (nsec && (!InFile(sec_off, uint64_t(nsec) * 56) || sec_off + uint64_t(nsec) * 56 > header_size)) {
            error = "section table is outside the header";
            return false;
        }
        for (uint32_t i = 0; i < nsec; i++) {
            const uint64_t o = sec_off + uint64_t(i) * 56;
            Section s;
            s.flags = U32(o);
            s.va = U32(o + 4);
            s.vsize = U32(o + 8);
            s.raw = U32(o + 12);
            s.rsize = U32(o + 16);
            if (!InFile(s.raw, s.rsize) ||
                (s.rsize && (uint64_t(s.va) + s.rsize > image_end || s.va < base))) {
                error = "section data is outside the image";
                return false;
            }
            if (s.vsize && (s.va < base || uint64_t(s.va) + s.vsize > image_end)) {
                error = "section range is outside the image";
                return false;
            }
            uint32_t name = U32(o + 20);
            if (name >= base) {
                const uint64_t no = uint64_t(name) - base;
                if (no < header_size && InFile(no, 1)) {
                    const size_t max = size_t(std::min<uint64_t>(header_size - no, file.size() - no));
                    size_t n = 0;
                    while (n < max && file[size_t(no) + n]) ++n;
                    if (n == max) {
                        error = "section name is not terminated";
                        return false;
                    }
                    s.name.assign(reinterpret_cast<const char*>(file.data() + no), n);
                }
            }
            sections.push_back(std::move(s));
        }
        uint32_t nlib = U32(0x160), lib_addr = U32(0x164);
        if (nlib) {
            if (nlib > 1024 || lib_addr < base) {
                error = "library table is invalid";
                return false;
            }
            const uint64_t lib_off = uint64_t(lib_addr) - base;
            if (!InFile(lib_off, uint64_t(nlib) * 16)) {
                error = "library table is outside the file";
                return false;
            }
            for (uint32_t i = 0; i < nlib; i++) {
                const uint64_t o = lib_off + uint64_t(i) * 16;
                Library l;
                char n[9] = {};
                std::memcpy(n, file.data() + o, 8);
                l.name = n;
                std::memcpy(&l.major, file.data() + o + 8, 8);  // major, minor, build and flags, four uint16 in order
                libraries.push_back(l);
            }
        }
        if (cert >= base) {
            const uint64_t co = uint64_t(cert) - base;
            if (InFile(co, 12)) title_id = U32(co + 8);  // certificate +8 is the title id
        }
        if (tls_addr) {
            const int64_t o = Offset(tls_addr);
            if (o < 0 || !InFile(uint64_t(o), 24) || uint64_t(tls_addr) + 24 > image_end ||
                Offset(tls_addr + 23) != o + 23) {
                error = "TLS record is outside the image";
                return false;
            }
            tls.data_start = U32(uint64_t(o));
            tls.data_end = U32(uint64_t(o) + 4);
            tls.index_addr = U32(uint64_t(o) + 8);
            tls.callbacks = U32(uint64_t(o) + 12);
            tls.zero_fill = U32(uint64_t(o) + 16);
            tls.characteristics = U32(uint64_t(o) + 20);
            if (tls.data_start > tls.data_end || (tls.data_end && (tls.data_start < base || tls.data_end > image_end)) ||
                (tls.index_addr && (tls.index_addr < base || tls.index_addr >= image_end))) {
                error = "TLS record is invalid";
                return false;
            }
        }
        return true;
    }
};

}  // namespace xbe
