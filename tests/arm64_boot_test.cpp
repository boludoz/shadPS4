// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Builds a minimal x86-64 ELF at runtime and boots it through the full chain
// (GuestLoader -> JitEngine -> Booter with syscall handling), proving the boot
// path end to end: an ELF is loaded, its code is translated to the host arch
// and executed, and its write()/exit() syscalls are serviced.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/arm64_recompiler/boot.h"
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
    TestLoaderRejectsGarbage();

    if (failures == 0) {
        std::printf("arm64_boot_test: all tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "arm64_boot_test: %d check(s) failed\n", failures);
    return 1;
}
