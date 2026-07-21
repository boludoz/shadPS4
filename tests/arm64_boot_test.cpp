// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Builds a minimal x86-64 ELF at runtime and boots it through the full chain
// (GuestLoader -> JitEngine -> Booter with syscall handling), proving the boot
// path end to end: an ELF is loaded, its code is translated to the host arch
// and executed, and its write()/exit() syscalls are serviced.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/arm64_recompiler/boot.h"
#include "core/arm64_recompiler/game_package.h"
#include "core/arm64_recompiler/guest_loader.h"
#include "core/arm64_recompiler/jit_engine.h"

using namespace Core::Recompiler;

namespace {

int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

#pragma pack(push, 1)
struct Ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};
#pragma pack(pop)

// Assembles a tiny program at load base `vaddr`:
//   write(1, msg, len); exit(0)
// and wraps it in a one-PT_LOAD ELF written to `path`.
std::string BuildHelloElf(const std::string& dir, u64 vaddr) {
    const char msg[] = "hello from x86 on this host\n";
    const u32 msg_len = sizeof(msg) - 1;

    // Code, followed immediately by the message. rip-relative LEA reaches it.
    std::vector<u8> code;
    auto emit = [&](std::initializer_list<u8> bytes) {
        for (u8 b : bytes) {
            code.push_back(b);
        }
    };
    // mov eax, 4              ; SYS_write
    emit({0xB8, 0x04, 0x00, 0x00, 0x00});
    // mov edi, 1              ; fd = stdout
    emit({0xBF, 0x01, 0x00, 0x00, 0x00});
    // lea rsi, [rip+disp]     ; msg (patched below)
    const size_t lea_at = code.size();
    emit({0x48, 0x8D, 0x35, 0x00, 0x00, 0x00, 0x00});
    // mov edx, msg_len
    emit({0xBA, static_cast<u8>(msg_len), 0x00, 0x00, 0x00});
    // syscall
    emit({0x0F, 0x05});
    // mov eax, 1              ; SYS_exit
    emit({0xB8, 0x01, 0x00, 0x00, 0x00});
    // xor edi, edi           ; code 0
    emit({0x31, 0xFF});
    // syscall
    emit({0x0F, 0x05});

    const size_t msg_off = code.size();
    for (u32 i = 0; i < msg_len; ++i) {
        code.push_back(static_cast<u8>(msg[i]));
    }
    // Patch the LEA displacement: target = msg, rip = addr after the LEA.
    const size_t rip_after_lea = lea_at + 7;
    const int32_t disp = static_cast<int32_t>(msg_off - rip_after_lea);
    std::memcpy(code.data() + lea_at + 3, &disp, sizeof(disp));

    const u64 code_off = sizeof(Ehdr) + sizeof(Phdr);

    Ehdr eh{};
    std::memcpy(eh.ident, "\x7F"
                          "ELF",
                4);
    eh.ident[4] = 2; // ELFCLASS64
    eh.ident[5] = 1; // little endian
    eh.ident[6] = 1; // version
    eh.type = 2;     // ET_EXEC
    eh.machine = 0x3E;
    eh.version = 1;
    eh.entry = vaddr + code_off;
    eh.phoff = sizeof(Ehdr);
    eh.ehsize = sizeof(Ehdr);
    eh.phentsize = sizeof(Phdr);
    eh.phnum = 1;

    Phdr ph{};
    ph.type = 1; // PT_LOAD
    ph.flags = 5; // R+X
    ph.offset = 0;
    ph.vaddr = vaddr;
    ph.paddr = vaddr;
    ph.filesz = code_off + code.size();
    ph.memsz = ph.filesz;
    ph.align = 0x1000;

    std::vector<u8> file;
    file.resize(code_off + code.size());
    std::memcpy(file.data(), &eh, sizeof(eh));
    std::memcpy(file.data() + sizeof(eh), &ph, sizeof(ph));
    std::memcpy(file.data() + code_off, code.data(), code.size());

    const std::string path = dir + "/hello_x86.elf";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    return path;
}

void TestBootHelloElf() {
    const std::string dir = "/tmp"; // scratch; overridden by TMPDIR if set
    const std::string path = BuildHelloElf(dir, 0x600000);

    GuestLoader loader;
    const auto module = loader.Load(path);
    CHECK(module.has_value());
    if (!module) {
        std::fprintf(stderr, "load error: %s\n", loader.GetError().c_str());
        return;
    }
    CHECK(module->entry != 0);
    CHECK(module->image_low != 0);

    JitEngine jit;
    CHECK(jit.GetError().empty());

    // Capture the guest's write() so we can assert on it.
    std::string captured;
    Booter booter{jit, loader};
    booter.SetWriteSink([&](int fd, const char* data, u64 len) {
        (void)fd;
        captured.append(data, len);
    });

    const BootResult r = booter.Boot(*module);
    CHECK(r.status == BootResult::Status::SyscallStop);
    CHECK(r.exit_code == 0);
    CHECK(captured == "hello from x86 on this host\n");
    if (captured != "hello from x86 on this host\n") {
        std::fprintf(stderr, "captured: '%s' status=%d\n", captured.c_str(),
                     static_cast<int>(r.status));
    }
}

// Assembles a program that reads its own dump through the boot VFS:
//   fd = open("/app0/data.txt"); n = read(fd, buf, 64); write(1, buf, n);
//   exit(0)
// This is the same access pattern a real game uses for its assets.
std::string BuildReaderElf(const std::string& dir, u64 vaddr) {
    std::vector<u8> code;
    auto emit = [&](std::initializer_list<u8> bytes) {
        for (u8 b : bytes) {
            code.push_back(b);
        }
    };
    auto emit_lea = [&](u8 modrm) {
        // lea r64, [rip+disp32]; displacement patched afterwards.
        const size_t at = code.size();
        emit({0x48, 0x8D, modrm, 0x00, 0x00, 0x00, 0x00});
        return at;
    };

    const size_t lea_path_at = emit_lea(0x3D); // lea rdi, [rip+path]
    emit({0x31, 0xF6});                        // xor esi, esi (O_RDONLY)
    emit({0x31, 0xD2});                        // xor edx, edx
    emit({0xB8, 0x05, 0x00, 0x00, 0x00});      // mov eax, 5 (SYS_open)
    emit({0x0F, 0x05});                        // syscall -> fd
    emit({0x89, 0xC7});                        // mov edi, eax
    const size_t lea_buf_at = emit_lea(0x35);  // lea rsi, [rip+buf]
    emit({0xBA, 0x40, 0x00, 0x00, 0x00});      // mov edx, 64
    emit({0xB8, 0x03, 0x00, 0x00, 0x00});      // mov eax, 3 (SYS_read)
    emit({0x0F, 0x05});                        // syscall -> bytes read
    emit({0x89, 0xC2});                        // mov edx, eax
    emit({0xBF, 0x01, 0x00, 0x00, 0x00});      // mov edi, 1
    emit({0xB8, 0x04, 0x00, 0x00, 0x00});      // mov eax, 4 (SYS_write)
    emit({0x0F, 0x05});                        // syscall
    emit({0xB8, 0x01, 0x00, 0x00, 0x00});      // mov eax, 1 (SYS_exit)
    emit({0x31, 0xFF});                        // xor edi, edi
    emit({0x0F, 0x05});                        // syscall

    const char path_str[] = "/app0/data.txt";
    const size_t path_off = code.size();
    for (char c : path_str) {
        code.push_back(static_cast<u8>(c)); // includes the NUL
    }
    const size_t buf_off = code.size();
    code.resize(code.size() + 64, 0);

    auto patch = [&](size_t lea_at, size_t target) {
        const int32_t disp = static_cast<int32_t>(target - (lea_at + 7));
        std::memcpy(code.data() + lea_at + 3, &disp, sizeof(disp));
    };
    patch(lea_path_at, path_off);
    patch(lea_buf_at, buf_off);

    const u64 code_off = sizeof(Ehdr) + sizeof(Phdr);
    Ehdr eh{};
    std::memcpy(eh.ident, "\x7F"
                          "ELF",
                4);
    eh.ident[4] = 2;
    eh.ident[5] = 1;
    eh.ident[6] = 1;
    eh.type = 2;
    eh.machine = 0x3E;
    eh.version = 1;
    eh.entry = vaddr + code_off;
    eh.phoff = sizeof(Ehdr);
    eh.ehsize = sizeof(Ehdr);
    eh.phentsize = sizeof(Phdr);
    eh.phnum = 1;

    Phdr ph{};
    ph.type = 1;
    ph.flags = 7; // R+W+X: the read buffer lives in the image
    ph.offset = 0;
    ph.vaddr = vaddr;
    ph.paddr = vaddr;
    ph.filesz = code_off + code.size();
    ph.memsz = ph.filesz;
    ph.align = 0x1000;

    std::vector<u8> file(code_off + code.size());
    std::memcpy(file.data(), &eh, sizeof(eh));
    std::memcpy(file.data() + sizeof(eh), &ph, sizeof(ph));
    std::memcpy(file.data() + code_off, code.data(), code.size());

    const std::string path = dir + "/eboot.bin";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    return path;
}

// Writes a minimal param.sfo with TITLE and TITLE_ID, byte-compatible with
// core/file_format/psf.h.
void WriteParamSfo(const std::filesystem::path& path) {
    struct Entry {
        const char* key;
        const char* value;
    };
    const Entry items[] = {{"TITLE", "Boot VFS Test Game"}, {"TITLE_ID", "CUSA00042"}};
    const u32 count = 2;

    std::vector<u8> keys;
    std::vector<u8> data;
    std::vector<u8> index;
    auto push32 = [](std::vector<u8>& v, u32 x) {
        v.push_back(x & 0xFF);
        v.push_back((x >> 8) & 0xFF);
        v.push_back((x >> 16) & 0xFF);
        v.push_back((x >> 24) & 0xFF);
    };
    auto push16 = [](std::vector<u8>& v, u16 x) {
        v.push_back(x & 0xFF);
        v.push_back((x >> 8) & 0xFF);
    };
    for (const auto& item : items) {
        const u16 key_off = static_cast<u16>(keys.size());
        for (const char* p = item.key; *p; ++p) {
            keys.push_back(static_cast<u8>(*p));
        }
        keys.push_back(0);
        const u32 data_off = static_cast<u32>(data.size());
        const u32 len = static_cast<u32>(std::strlen(item.value)) + 1;
        const u32 max_len = (len + 3) & ~3u;
        for (const char* p = item.value; *p; ++p) {
            data.push_back(static_cast<u8>(*p));
        }
        data.resize(data_off + max_len, 0);

        push16(index, key_off);
        push16(index, 0x0402); // Text (0x0204 stored big-endian)
        push32(index, len);
        push32(index, max_len);
        push32(index, data_off);
    }

    const u32 header_size = 20;
    const u32 key_table_off = header_size + static_cast<u32>(index.size());
    const u32 data_table_off = key_table_off + static_cast<u32>(keys.size());

    std::vector<u8> file;
    file.push_back(0x00);
    file.push_back('P');
    file.push_back('S');
    file.push_back('F');
    push32(file, 0x00000101); // version 1.1
    push32(file, key_table_off);
    push32(file, data_table_off);
    push32(file, count);
    file.insert(file.end(), index.begin(), index.end());
    file.insert(file.end(), keys.begin(), keys.end());
    file.insert(file.end(), data.begin(), data.end());

    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(file.data()), file.size());
}

// A dump folder loaded end to end, the way the Android host does it: folder
// in, param.sfo metadata out, /app0 mounted, and the guest reading its own
// asset file through the file syscalls.
void TestGameDumpBoot() {
    namespace fs = std::filesystem;
    const fs::path dump = fs::temp_directory_path() / "shadps4_dump_test";
    fs::create_directories(dump / "sce_sys");
    fs::create_directories(dump / "sce_module");
    BuildReaderElf(dump.string(), 0x700000);
    WriteParamSfo(dump / "sce_sys" / "param.sfo");
    {
        std::ofstream out{dump / "data.txt", std::ios::binary};
        out << "asset served from /app0\n";
    }

    std::string error;
    const auto pkg = GamePackage::Resolve(dump, &error);
    CHECK(pkg.has_value());
    if (!pkg) {
        std::fprintf(stderr, "resolve error: %s\n", error.c_str());
        return;
    }
    CHECK(pkg->eboot_path == dump / "eboot.bin");
    CHECK(pkg->title == "Boot VFS Test Game");
    CHECK(pkg->title_id == "CUSA00042");
    CHECK(pkg->sce_modules.empty());

    // Resolving via the eboot path directly gives the same package.
    const auto pkg2 = GamePackage::Resolve(dump / "eboot.bin", &error);
    CHECK(pkg2.has_value() && pkg2->title_id == "CUSA00042");

    GuestLoader loader;
    const auto module = loader.Load(pkg->eboot_path);
    CHECK(module.has_value());
    if (!module) {
        std::fprintf(stderr, "load error: %s\n", loader.GetError().c_str());
        return;
    }

    JitEngine jit;
    std::string captured;
    Booter booter{jit, loader};
    booter.AddMount("/app0", pkg->game_dir);
    booter.SetWriteSink([&](int fd, const char* data, u64 len) {
        (void)fd;
        captured.append(data, len);
    });

    const BootResult r = booter.Boot(*module);
    CHECK(r.status == BootResult::Status::SyscallStop);
    CHECK(r.exit_code == 0);
    CHECK(captured == "asset served from /app0\n");
    if (captured != "asset served from /app0\n") {
        std::fprintf(stderr, "captured: '%s' status=%d (%s)\n", captured.c_str(),
                     static_cast<int>(r.status), r.message.c_str());
    }
}

void TestLoaderRejectsGarbage() {
    const std::string path = std::string("/tmp/not_an_elf.bin");
    FILE* f = std::fopen(path.c_str(), "wb");
    const char junk[] = "this is not an executable at all, just some bytes here";
    std::fwrite(junk, 1, sizeof(junk), f);
    std::fclose(f);

    GuestLoader loader;
    const auto module = loader.Load(path);
    CHECK(!module.has_value());
    CHECK(!loader.GetError().empty());
}

} // namespace

int main() {
    TestBootHelloElf();
    TestGameDumpBoot();
    TestLoaderRejectsGarbage();

    if (failures == 0) {
        std::printf("arm64_boot_test: all tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "arm64_boot_test: %d check(s) failed\n", failures);
    return 1;
}
