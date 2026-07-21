// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Self-contained loader for decrypted PS4 executables. It parses the ELF (or
// the ELF embedded in a fake/decrypted SELF), maps PT_LOAD segments, reads the
// PS4 dynamic table out of PT_SCE_DYNLIBDATA, applies relocations and points
// every imported symbol either at a resolved address or at a per-symbol
// trampoline used to report the first missing HLE call.
//
// No dependency on the desktop core so it links into the Android .so as-is.

#include <cstring>
#include <fstream>

#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "core/aerolib/aerolib.h"
#include "core/arm64_recompiler/guest_loader.h"

namespace Core::Recompiler {

namespace {

// ---- PS4 ELF constants (subset; mirrors core/loader/elf.h) ----------------

constexpr u32 kElfMagic = 0x464C457F; // "\x7FELF" little-endian
constexpr u32 kSelfMagic = 0x1D3D154F;

constexpr u32 PT_LOAD = 0x1;
constexpr u32 PT_TLS = 0x7;
constexpr u32 PT_SCE_DYNLIBDATA = 0x61000000;
constexpr u32 PT_SCE_PROCPARAM = 0x61000001;
constexpr u32 PT_SCE_RELRO = 0x61000010;
constexpr u32 PT_DYNAMIC = 0x2;

constexpr u16 ET_SCE_EXEC = 0xfe00;
constexpr u16 ET_SCE_DYNEXEC = 0xfe10;
constexpr u16 ET_SCE_DYNAMIC = 0xfe18;

// Standard dynamic tags used by PS4 modules.
constexpr s64 DT_NULL = 0;
constexpr s64 DT_INIT = 12;
constexpr s64 DT_FINI = 13;
// Sony-specific dynamic tags (from core/module.cpp).
constexpr s64 DT_SCE_HASH = 0x61000025;
constexpr s64 DT_SCE_PLTGOT = 0x61000027;
constexpr s64 DT_SCE_JMPREL = 0x61000029;
constexpr s64 DT_SCE_PLTRELSZ = 0x6100002d;
constexpr s64 DT_SCE_RELA = 0x6100002f;
constexpr s64 DT_SCE_RELASZ = 0x61000031;
constexpr s64 DT_SCE_RELAENT = 0x61000033;
constexpr s64 DT_SCE_STRTAB = 0x61000035;
constexpr s64 DT_SCE_STRSZ = 0x61000037;
constexpr s64 DT_SCE_SYMTAB = 0x61000039;
constexpr s64 DT_SCE_SYMTABSZ = 0x6100003d;
constexpr s64 DT_SCE_SYMENT = 0x6100003b;

constexpr u32 R_X86_64_64 = 1;
constexpr u32 R_X86_64_GLOB_DAT = 6;
constexpr u32 R_X86_64_JUMP_SLOT = 7;
constexpr u32 R_X86_64_RELATIVE = 8;

#pragma pack(push, 1)
struct ElfHeader {
    u32 magic;
    u8 elf_class;
    u8 data;
    u8 version;
    u8 osabi;
    u8 abiversion;
    u8 pad[7];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct ProgramHeader {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

struct Dynamic {
    s64 d_tag;
    u64 d_val;
};

struct SceSymbol {
    u32 st_name;
    u8 st_info;
    u8 st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
};

struct SceRela {
    u64 r_offset;
    u64 r_info;
    s64 r_addend;
    u32 Sym() const {
        return static_cast<u32>(r_info >> 32);
    }
    u32 Type() const {
        return static_cast<u32>(r_info & 0xffffffff);
    }
};

struct SelfHeader {
    u32 magic;
    u8 version;
    u8 mode;
    u8 endian;
    u8 attributes;
    u8 category;
    u8 program_type;
    u16 padding1;
    u16 header_size;
    u16 meta_size;
    u32 file_size;
    u32 padding2;
    u16 segment_count;
    u16 unknown1A;
    u32 padding3;
};

struct SelfSegment {
    u64 flags;
    u64 file_offset;
    u64 file_size;
    u64 memory_size;
};
#pragma pack(pop)

u64 AlignUp(u64 value, u64 align) {
    return align <= 1 ? value : (value + align - 1) & ~(align - 1);
}

/// Default allocator: anonymous RWX memory. Uses MAP_FIXED-free placement and
/// remembers the base so the guest's absolute p_vaddr layout is honored by a
/// single reservation covering [low, high).
void* DefaultMap(u64 vaddr, u64 size) {
#ifndef _WIN32
    void* p = mmap(reinterpret_cast<void*>(vaddr), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        // Retry without a fixed hint; the loader applies a bias to match.
        p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            return nullptr;
        }
    }
    return p;
#else
    (void)vaddr;
    return ::operator new(size, std::nothrow);
#endif
}

} // namespace

GuestLoader::GuestLoader(Allocator alloc)
    : allocator{alloc ? std::move(alloc) : Allocator{DefaultMap}} {}

const std::string* GuestLoader::LookupTrampoline(u64 value) const {
    const auto it = trampoline_names.find(value);
    return it == trampoline_names.end() ? nullptr : &it->second;
}

std::optional<GuestLoader::LoadedModule> GuestLoader::Load(const std::filesystem::path& path) {
    error.clear();

    std::ifstream in{path, std::ios::binary};
    if (!in) {
        error = "cannot open " + path.string();
        return std::nullopt;
    }
    file_bytes.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    if (file_bytes.size() < sizeof(ElfHeader)) {
        error = "file too small";
        return std::nullopt;
    }

    // A fake/decrypted SELF wraps the ELF: self_header + segment headers, then
    // the ELF header follows. Locate the ELF start and a file->offset mapping.
    const u8* base = file_bytes.data();
    u64 elf_off = 0;
    const std::vector<SelfSegment>* self_segs_ptr = nullptr;
    std::vector<SelfSegment> self_segs;
    u32 head_magic;
    std::memcpy(&head_magic, base, 4);
    if (head_magic == kSelfMagic) {
        SelfHeader sh;
        std::memcpy(&sh, base, sizeof(sh));
        elf_off = sizeof(SelfHeader) + sizeof(SelfSegment) * sh.segment_count;
        self_segs.resize(sh.segment_count);
        std::memcpy(self_segs.data(), base + sizeof(SelfHeader),
                    sizeof(SelfSegment) * sh.segment_count);
        self_segs_ptr = &self_segs;
        if (elf_off + sizeof(ElfHeader) > file_bytes.size()) {
            error = "SELF header out of range";
            return std::nullopt;
        }
    } else if (head_magic != kElfMagic) {
        error = "not an ELF or decrypted SELF (encrypted dumps are not supported)";
        return std::nullopt;
    }

    ElfHeader eh;
    std::memcpy(&eh, base + elf_off, sizeof(eh));
    if (eh.magic != kElfMagic) {
        error = "missing ELF magic";
        return std::nullopt;
    }
    if (eh.e_machine != 0x3E) { // EM_X86_64
        error = "not an x86-64 executable";
        return std::nullopt;
    }

    std::vector<ProgramHeader> phdrs(eh.e_phnum);
    std::memcpy(phdrs.data(), base + elf_off + eh.e_phoff, sizeof(ProgramHeader) * eh.e_phnum);

    // Maps a file offset (as seen in phdr.p_offset) to a pointer into the raw
    // file, honoring the fSELF segment indirection when present.
    auto file_ptr = [&](u64 file_offset, u64 size) -> const u8* {
        if (self_segs_ptr) {
            for (const auto& seg : *self_segs_ptr) {
                const bool blocked = (seg.flags & 0x800) != 0;
                if (!blocked) {
                    continue;
                }
                const u32 id = static_cast<u32>((seg.flags >> 20) & 0xFFF);
                if (id >= phdrs.size()) {
                    continue;
                }
                const auto& ph = phdrs[id];
                if (file_offset >= ph.p_offset && file_offset < ph.p_offset + ph.p_filesz) {
                    const u64 delta = file_offset - ph.p_offset;
                    if (seg.file_offset + delta + size > file_bytes.size()) {
                        return nullptr;
                    }
                    return base + seg.file_offset + delta;
                }
            }
        }
        if (file_offset + size > file_bytes.size()) {
            return nullptr;
        }
        return base + file_offset;
    };

    // Compute the guest address span across all PT_LOAD segments.
    u64 low = ~u64{0};
    u64 high = 0;
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_LOAD && ph.p_type != PT_SCE_RELRO) {
            continue;
        }
        low = std::min(low, ph.p_vaddr);
        high = std::max(high, ph.p_vaddr + ph.p_memsz);
    }
    if (low == ~u64{0}) {
        error = "no loadable segments";
        return std::nullopt;
    }
    low &= ~u64{0xFFF};
    high = AlignUp(high, 0x1000);

    // Reserve the whole image in one allocation so relative layout is exact,
    // then apply a bias if the OS could not honor the requested base.
    const u64 span = high - low;
    void* host = allocator(low, span);
    if (!host) {
        error = "failed to allocate " + std::to_string(span) + " bytes of guest memory";
        return std::nullopt;
    }
    const u64 bias = reinterpret_cast<u64>(host) - low;

    LoadedModule mod{};
    mod.base = bias;
    mod.image_low = low + bias;
    mod.image_high = high + bias;
    mod.is_shared = (eh.e_type == ET_SCE_DYNAMIC);

    // Copy PT_LOAD file contents and zero the bss tail.
    const ProgramHeader* dyn_ph = nullptr;
    const ProgramHeader* dynlib_ph = nullptr;
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_DYNAMIC) {
            dyn_ph = &ph;
        } else if (ph.p_type == PT_SCE_DYNLIBDATA) {
            dynlib_ph = &ph;
        } else if (ph.p_type == PT_SCE_PROCPARAM) {
            mod.proc_param = ph.p_vaddr + bias;
        } else if (ph.p_type == PT_TLS) {
            mod.tls_vaddr = ph.p_vaddr + bias;
            mod.tls_filesz = ph.p_filesz;
            mod.tls_memsz = ph.p_memsz;
            mod.tls_align = ph.p_align;
        }
        if (ph.p_type != PT_LOAD && ph.p_type != PT_SCE_RELRO) {
            continue;
        }
        u8* dst = reinterpret_cast<u8*>(ph.p_vaddr + bias);
        if (ph.p_filesz) {
            const u8* src = file_ptr(ph.p_offset, ph.p_filesz);
            if (!src) {
                error = "segment file range out of bounds";
                return std::nullopt;
            }
            std::memcpy(dst, src, ph.p_filesz);
        }
        if (ph.p_memsz > ph.p_filesz) {
            std::memset(dst + ph.p_filesz, 0, ph.p_memsz - ph.p_filesz);
        }
    }

    mod.entry = eh.e_entry + bias;
    mod.entry_points.push_back(mod.entry);

    // ---- Dynamic table + relocations -------------------------------------
    // PS4 stores the dynamic array in PT_DYNAMIC but the strtab/symtab/rela
    // tables it references live inside PT_SCE_DYNLIBDATA (file-relative).
    if (dyn_ph && dynlib_ph) {
        const u8* dynlib = file_ptr(dynlib_ph->p_offset, dynlib_ph->p_filesz);
        const u8* dyn_raw = file_ptr(dyn_ph->p_offset, dyn_ph->p_filesz);
        if (dynlib && dyn_raw) {
            const auto* dyn = reinterpret_cast<const Dynamic*>(dyn_raw);
            const size_t dyn_count = dyn_ph->p_filesz / sizeof(Dynamic);

            const char* strtab = nullptr;
            const SceSymbol* symtab = nullptr;
            size_t symtab_size = 0;
            const SceRela* rela = nullptr;
            size_t rela_count = 0;
            const SceRela* jmprel = nullptr;
            size_t jmprel_count = 0;

            for (size_t i = 0; i < dyn_count; ++i) {
                switch (dyn[i].d_tag) {
                case DT_SCE_STRTAB:
                    strtab = reinterpret_cast<const char*>(dynlib + dyn[i].d_val);
                    break;
                case DT_SCE_SYMTAB:
                    symtab = reinterpret_cast<const SceSymbol*>(dynlib + dyn[i].d_val);
                    break;
                case DT_SCE_SYMTABSZ:
                    symtab_size = dyn[i].d_val;
                    break;
                case DT_SCE_RELA:
                    rela = reinterpret_cast<const SceRela*>(dynlib + dyn[i].d_val);
                    break;
                case DT_SCE_RELASZ:
                    rela_count = dyn[i].d_val / sizeof(SceRela);
                    break;
                case DT_SCE_JMPREL:
                    jmprel = reinterpret_cast<const SceRela*>(dynlib + dyn[i].d_val);
                    break;
                case DT_SCE_PLTRELSZ:
                    jmprel_count = dyn[i].d_val / sizeof(SceRela);
                    break;
                default:
                    break;
                }
            }
            (void)symtab_size;

            InstallTrampoline();

            auto resolve = [&](u32 sym_index, u64 addend, u64 where) -> u64 {
                if (!symtab || !strtab) {
                    return 0;
                }
                const SceSymbol& sym = symtab[sym_index];
                std::string encoded = strtab + sym.st_name;
                // NIDs are "<nid>#<lib>#<mod>"; keep the leading NID token.
                const std::string nid = encoded.substr(0, encoded.find('#'));
                if (sym.st_value != 0 && sym.st_shndx != 0) {
                    // Defined locally in this module.
                    return sym.st_value + bias + addend;
                }
                // Imported: record it and point at a per-symbol trampoline so
                // the first call lands somewhere we can name.
                Symbol s{};
                s.nid = nid;
                s.name = ResolveNidName(nid);
                s.stub_slot = where;
                const u64 tramp = unresolved_trampoline + (mod.imports.size() + 1) * 16;
                s.trampoline = tramp;
                mod.imports.push_back(s);
                trampoline_names[tramp] = s.name;
                return tramp;
            };

            auto apply = [&](const SceRela* table, size_t count) {
                for (size_t i = 0; i < count; ++i) {
                    const SceRela& r = table[i];
                    u64* target = reinterpret_cast<u64*>(r.r_offset + bias);
                    switch (r.Type()) {
                    case R_X86_64_RELATIVE:
                        *target = bias + r.r_addend;
                        break;
                    case R_X86_64_64:
                    case R_X86_64_GLOB_DAT:
                        *target = resolve(r.Sym(), r.r_addend, r.r_offset + bias);
                        break;
                    case R_X86_64_JUMP_SLOT:
                        *target = resolve(r.Sym(), 0, r.r_offset + bias);
                        break;
                    default:
                        break;
                    }
                }
            };

            if (rela) {
                apply(rela, rela_count);
            }
            if (jmprel) {
                apply(jmprel, jmprel_count);
            }
        }
    }

    return mod;
}

void GuestLoader::InstallTrampoline() {
    if (unresolved_trampoline) {
        return;
    }
    // A page of `syscall` (0F 05) instructions with RAX preset to a sentinel
    // would need code; instead we reserve a guest-address range that the
    // dispatcher recognizes as "unresolved import" via LookupTrampoline.
    // The actual bytes are never executed as code — calls to it exit through
    // the missing-HLE path in the boot layer, which checks LookupTrampoline.
    static u64 counter = 0x4000'0000'0000ull;
    unresolved_trampoline = counter;
    counter += 0x1000'0000ull;
}

std::string ResolveNidName(const std::string& nid) {
    // shadPS4 ships the full PS4 NID -> symbol name table (aerolib); use it so
    // an unresolved import is reported by its real function name.
    if (const auto* entry = Core::AeroLib::FindByNid(nid.c_str())) {
        return entry->name;
    }
    return nid;
}

} // namespace Core::Recompiler
