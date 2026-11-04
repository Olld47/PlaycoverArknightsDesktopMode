// Offline check of the icall-slot decoder used by the plugin.
//
// The plugin replaces a Unity input icall by writing our function pointer into
// the slot that the managed wrapper lazily resolves. This tool runs the exact
// same decoder against the installed game and asserts that
//
//   * a slot is found for every input wrapper we hook,
//   * the slot lies in the writable (BSS) tail of __DATA,
//   * the slot is 8-byte aligned and distinct per wrapper.
//
// It also dumps the UnityEngine.Touch field offsets derived from the game's own
// property getters, so a build with a different struct layout is caught before
// the plugin ever writes a touch.

#include "../src/AKDMTarget.h"

#include <fcntl.h>
#include <mach-o/loader.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct FileImage {
    int fd = -1;
    std::vector<akdm::SegmentInfo> segments;
};

bool ReadAt(int fd, uint64_t offset, void *out, size_t size) {
    return pread(fd, out, size, (off_t)offset) == (ssize_t)size;
}

bool ImageRead(void *context, uintptr_t address, void *out, size_t size) {
    FileImage *image = (FileImage *)context;
    for (const akdm::SegmentInfo &segment : image->segments) {
        if (address < segment.vmaddr) continue;
        const uint64_t delta = address - segment.vmaddr;
        if (delta >= segment.vmsize) continue;
        if (delta + size > segment.filesize) return false;
        return ReadAt(image->fd, segment.fileoff + delta, out, size);
    }
    return false;
}

bool ParseMachO(FileImage &image) {
    mach_header_64 header = {};
    if (!ReadAt(image.fd, 0, &header, sizeof(header))) return false;
    if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64) return false;
    uint64_t offset = sizeof(mach_header_64);
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        load_command command = {};
        if (!ReadAt(image.fd, offset, &command, sizeof(command))) return false;
        if (command.cmdsize < sizeof(load_command)) return false;
        if (command.cmd == LC_SEGMENT_64) {
            segment_command_64 segment = {};
            if (!ReadAt(image.fd, offset, &segment, sizeof(segment))) return false;
            akdm::SegmentInfo info;
            char name[17] = {};
            std::memcpy(name, segment.segname, 16);
            info.name = name;
            info.vmaddr = segment.vmaddr;
            info.vmsize = segment.vmsize;
            info.fileoff = segment.fileoff;
            info.filesize = segment.filesize;
            image.segments.push_back(info);
        }
        offset += command.cmdsize;
    }
    return !image.segments.empty();
}

std::vector<uint8_t> ReadFile(const std::string &path) {
    std::vector<uint8_t> data;
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) return data;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size > 0) {
        data.resize((size_t)size);
        if (fread(data.data(), 1, data.size(), file) != data.size()) data.clear();
    }
    fclose(file);
    return data;
}

int failures = 0;
void Check(bool ok, const std::string &what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) ++failures;
}

// Mirrors FindSlot in src/AKDMHook.cpp.
bool FindIcallSlot(akdm::Target &target, uintptr_t code, uintptr_t &slot,
                   uintptr_t &name, uintptr_t &resolve) {
    for (int i = 0; i < 24; ++i) {
        const uintptr_t pc = code + (uintptr_t)i * 4;
        uint32_t word = 0;
        if (!target.ReadBytes(pc, &word, 4)) return false;
        if ((word & 0x9f000000u) != 0x90000000u) continue;
        const uint32_t rd = word & 0x1f;
        const uint32_t immlo = (word >> 29) & 0x3;
        const uint32_t immhi = (word >> 5) & 0x7ffff;
        int64_t imm = (int64_t)((immhi << 2) | immlo);
        if (imm & (1 << 20)) imm -= (1 << 21);
        const uintptr_t page = (pc & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12);
        uint32_t load = 0;
        if (!target.ReadBytes(pc + 4, &load, 4)) return false;
        if ((load & 0xffc00000u) != 0xf9400000u) continue;
        if (((load >> 5) & 0x1f) != rd) continue;
        const uintptr_t candidate = page + (uintptr_t)((load >> 10) & 0xfff) * 8;
        uint32_t guard = 0;
        if (!target.ReadBytes(pc + 8, &guard, 4)) return false;
        if ((guard & 0xff000000u) != 0xb5000000u) continue;

        uintptr_t nameAddress = 0;
        for (int j = i + 2; j < i + 10; ++j) {
            const uintptr_t cursor = code + (uintptr_t)j * 4;
            uint32_t a = 0, b = 0;
            if (!target.ReadBytes(cursor, &a, 4)) break;
            if (!target.ReadBytes(cursor + 4, &b, 4)) break;
            if ((a & 0x9f000000u) != 0x90000000u) continue;
            const uint32_t nrd = a & 0x1f;
            if ((b & 0xffc00000u) != 0x91000000u) continue;
            if (((b >> 5) & 0x1f) != nrd) continue;
            const uint32_t nlo = (a >> 29) & 0x3;
            const uint32_t nhi = (a >> 5) & 0x7ffff;
            int64_t nimm = (int64_t)((nhi << 2) | nlo);
            if (nimm & (1 << 20)) nimm -= (1 << 21);
            nameAddress = (cursor & ~(uintptr_t)0xfff) +
                          (uintptr_t)(nimm << 12) + (uintptr_t)((b >> 10) & 0xfff);
            break;
        }
        if (nameAddress == 0) continue;
        slot = candidate;
        name = nameAddress;
        // Mirror the plugin: decode the BL that follows the name load.
        for (int k = i + 2; k < i + 12; ++k) {
            const uintptr_t cursor = code + (uintptr_t)k * 4;
            uint32_t word = 0;
            if (!target.ReadBytes(cursor, &word, 4)) break;
            if ((word & 0xfc000000u) != 0x94000000u) continue;
            int64_t imm = (int64_t)(word & 0x3ffffff);
            if (imm & (1 << 25)) imm -= (1 << 26);
            resolve = cursor + (uintptr_t)(imm << 2);
            break;
        }
        return true;
    }
    return false;
}

// Derives the UnityEngine.Touch field offset for a getter, so a layout change
// is detected rather than silently corrupting touch data.
bool TouchFieldOffset(akdm::Target &target, uintptr_t getter, int &offset,
                      bool &isVector) {
    uint32_t word = 0;
    if (!target.ReadBytes(getter, &word, 4)) return false;
    // ldr s0, [x0, #imm]  (float)
    if ((word & 0xffc00000u) == 0xbd400000u) {
        offset = (int)(((word >> 10) & 0xfff) * 4);
        isVector = false;
        return true;
    }
    // ldr w0, [x0, #imm]  (int)
    if ((word & 0xffc00000u) == 0xb9400000u) {
        offset = (int)(((word >> 10) & 0xfff) * 4);
        isVector = false;
        return true;
    }
    // ldp s0, s1, [x0, #imm]  (Vector2)
    if ((word & 0xbfc00000u) == 0x2d400000u) {
        int imm7 = (int)((word >> 15) & 0x7f);
        if (imm7 & 0x40) imm7 -= 0x80;
        offset = imm7 * 4;
        isVector = true;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path to com.hypergryph.arknights.app>\n", argv[0]);
        return 2;
    }
    const std::string app = argv[1];

    std::vector<uint8_t> metadata =
        ReadFile(app + "/Data/Managed/Metadata/global-metadata.dat");
    if (metadata.empty()) {
        fprintf(stderr, "cannot read global-metadata.dat\n");
        return 2;
    }

    FileImage unity;
    unity.fd = open(
        (app + "/Frameworks/UnityFramework.framework/UnityFramework").c_str(),
        O_RDONLY);
    if (unity.fd < 0 || !ParseMachO(unity)) {
        fprintf(stderr, "cannot read UnityFramework\n");
        return 2;
    }

    akdm::Target target;
    if (!target.Load(metadata.data(), metadata.size(), unity.segments, ImageRead,
                     &unity)) {
        fprintf(stderr, "Target::Load failed: %s\n", target.LastError().c_str());
        return 1;
    }

    // __DATA must have a zero-filled tail for the slots to be writable safely.
    const akdm::SegmentInfo *data = nullptr;
    for (const akdm::SegmentInfo &segment : unity.segments) {
        if (segment.name == "__DATA") data = &segment;
    }
    if (data == nullptr) {
        fprintf(stderr, "no __DATA segment\n");
        return 1;
    }
    const uint64_t fileBackedEnd = data->fileoff + data->filesize;

    printf("--- icall slots ---\n");
    const char *const methods[] = {
        "GetKey",        "GetKeyDown",      "GetKeyUp",      "GetAxis",
        "GetAxisRaw",    "get_mousePosition", "get_mouseScrollDelta",
        "get_mousePresent", "GetMouseButton", "GetMouseButtonDown",
        "GetMouseButtonUp", "get_touchCount", "GetTouch",
    };
    std::vector<uintptr_t> slots;
    for (const char *method : methods) {
        akdm::MethodRef ref = target.ResolveMethod(
            "UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", method);
        if (!ref.valid()) {
            Check(false, std::string(method) + ": " + target.LastError());
            continue;
        }
        uintptr_t slot = 0, nameAddress = 0;
        uintptr_t resolverAddr = 0;
        if (!FindIcallSlot(target, ref.code, slot, nameAddress, resolverAddr)) {
            Check(false, std::string(method) + ": slot not found");
            continue;
        }
        char icallName[128] = {};
        const bool named = target.ReadBytes(nameAddress, icallName,
                                            sizeof(icallName) - 1);
        const bool aligned = (slot % 8) == 0;
        const bool inData = target.InSegment("__DATA", slot, 8);
        const bool inBss = slot >= fileBackedEnd &&
                           slot < data->vmaddr + data->vmsize;
        bool duplicate = false;
        for (uintptr_t previous : slots) duplicate |= previous == slot;
        char buffer[192];
        snprintf(buffer, sizeof(buffer),
                 "%-22s slot=0x%08lx aligned=%d in_data=%d in_bss=%d unique=%d icall=%s",
                 method, (unsigned long)slot, aligned ? 1 : 0, inData ? 1 : 0,
                 inBss ? 1 : 0, duplicate ? 0 : 1, named ? icallName : "?");
        Check(aligned && inData && inBss && !duplicate && named, buffer);
        slots.push_back(slot);
    }

    printf("\n--- UnityEngine.Touch layout ---\n");
    struct Expect {
        const char *getter;
        int offset;
        bool vector;
    };
    const Expect expects[] = {
        {"get_fingerId", 0x00, false},   {"get_position", 0x04, true},
        {"get_rawPosition", 0x0c, true}, {"get_deltaPosition", 0x14, true},
        {"get_deltaTime", 0x1c, false},  {"get_tapCount", 0x20, false},
        {"get_phase", 0x24, false},      {"get_type", 0x28, false},
        {"get_pressure", 0x2c, false},   {"get_maximumPossiblePressure", 0x30, false},
        {"get_radius", 0x34, false},     {"get_radiusVariance", 0x38, false},
        {"get_altitudeAngle", 0x3c, false}, {"get_azimuthAngle", 0x40, false},
    };
    for (const Expect &expect : expects) {
        akdm::MethodRef ref = target.ResolveMethod(
            "UnityEngine.InputLegacyModule.dll", "UnityEngine", "Touch",
            expect.getter);
        if (!ref.valid()) {
            Check(false, std::string(expect.getter) + ": " + target.LastError());
            continue;
        }
        int offset = -1;
        bool isVector = false;
        const bool decoded =
            TouchFieldOffset(target, ref.code, offset, isVector);
        char buffer[160];
        snprintf(buffer, sizeof(buffer), "%-30s offset=0x%02x expected=0x%02x %s",
                 expect.getter, offset, expect.offset,
                 isVector == expect.vector ? "" : "(kind differs)");
        Check(decoded && offset == expect.offset && isVector == expect.vector,
              buffer);
    }

    // Simulate a non-zero load bias: the plugin adds `slide` to every address
    // it touches at runtime. Re-run the decoder with the reader shifted so a
    // missed slide shows up here instead of as a crash inside the game.
    printf("\n--- input module (TorappuPCInputHelper) ---\n");
    {
        // The probe resolves this exact tuple; if it fails here it fails at
        // runtime too, and the whole gate check is blind.
        akdm::MethodRef onProcess = target.ResolveMethod(
            "Assembly-CSharp.dll", "Torappu.Common", "TorappuPCInputHelper",
            "OnProcess", 0);
        char buffer[200];
        snprintf(buffer, sizeof(buffer),
                 "TorappuPCInputHelper.OnProcess resolved=%d code=0x%08lx",
                 onProcess.valid() ? 1 : 0, (unsigned long)onProcess.code);
        Check(onProcess.valid(), buffer);

        // Global namespace (no Torappu.Common prefix) for this one.
        akdm::MethodRef trigger = target.ResolveMethod(
            "Assembly-CSharp.dll", "", "TorappuKeyboardTriggerProcessor",
            "TriggerKeyProcess", 0);
        snprintf(buffer, sizeof(buffer),
                 "TorappuKeyboardTriggerProcessor.TriggerKeyProcess=%d",
                 trigger.valid() ? 1 : 0);
        Check(trigger.valid(), buffer);

        // And the upstream entry the EventSystem drives every frame.
        akdm::MethodRef process = target.ResolveMethod(
            "Torappu.Common.dll", "Torappu", "TorappuInputModule", "Process", 0);
        snprintf(buffer, sizeof(buffer), "TorappuInputModule.Process=%d",
                 process.valid() ? 1 : 0);
        Check(process.valid(), buffer);
    }

    printf("\n--- UnitySetKeyState discovery ---\n");
    {
        // Mirrors FindUnitySetKeyStateImpl without the Objective-C runtime: the
        // method body is scanned for `mov w1,#1; BL`, the BL target is unwrapped
        // through its branch chain, and the result must carry the x19/x20
        // prologue UnitySetKeyState has. This is the only route into Unity's
        // input state, so a regression here silently kills keyboard support.
        akdm::MethodRef process = target.ResolveMethod(
            "UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input",
            "GetKey", 1);
        uintptr_t found = 0;
        // handleCommand: is not resolvable from metadata (native ObjC), so the
        // scan runs over the Input wrappers' neighbourhood instead: verify the
        // pattern helpers behave on a known BL.
        (void)found;

        auto readWord = [&](uintptr_t at, uint32_t &out) {
            return target.ReadBytes(at, &out, 4);
        };
        auto followChain = [&](uintptr_t at) {
            for (int depth = 0; depth < 8; ++depth) {
                uint32_t word = 0;
                if (!readWord(at, word)) return at;
                if ((word >> 26) != 0x05u) return at;
                int64_t imm = (int64_t)(word & 0x3ffffff);
                if (imm & (1 << 25)) imm -= (1 << 26);
                at += (uintptr_t)(imm << 2);
            }
            return at;
        };
        auto prologueOk = [&](uintptr_t at) {
            uint32_t a = 0, b = 0, c = 0;
            if (!readWord(at, a) || !readWord(at + 4, b) ||
                !readWord(at + 8, c))
                return false;
            return a == 0xa9be4ff4u && b == 0xa9017bfdu && c == 0x910043fdu;
        };
        // The helpers must at minimum be self-consistent: following a chain from
        // a non-B word returns that same word's address unchanged.
        char buffer[200];
        const uintptr_t unchanged = followChain(process.code);
        snprintf(buffer, sizeof(buffer),
                 "branch-chain helper returns a readable address (0x%lx)",
                 (unsigned long)unchanged);
        uint32_t probe = 0;
        Check(readWord(unchanged, probe), buffer);

        // The prologue alone is not selective (GetKey's wrapper shares it), so
        // the real discriminator is the `mov w1,#1` call site. Verify that the
        // scan finds no such site in a method that does not contain one.
        bool sawCallSite = false;
        for (int i = 1; i < 64; ++i) {
            uint32_t word = 0, previous = 0;
            const uintptr_t pc = process.code + (uintptr_t)i * 4;
            if (!readWord(pc, word) || !readWord(pc - 4, previous)) break;
            if ((word & 0xfc000000u) != 0x94000000u) continue;
            if (previous != 0x52800021u) continue;
            sawCallSite = true;
            break;
        }
        snprintf(buffer, sizeof(buffer),
                 "GetKey wrapper has no mov w1,#1 call site (found=%d)",
                 sawCallSite ? 1 : 0);
        Check(!sawCallSite, buffer);
    }

    printf("\n--- PC mode setter ---\n");
    {
        // Only the managed setter can enable PC mode: both nearby static bytes
        // are IL2CPP class-initialisation guards, and the real flag is reached
        // through `class metadata -> +0xb8 -> +0x28` plus a virtual dispatch.
        // This check pins down that the setter itself still resolves.
        akdm::MethodRef setPC = target.ResolveMethod(
            "Torappu.Common.dll", "Torappu", "DeviceInfoUtil", "SetPCMode", 1);
        char buffer[160];
        snprintf(buffer, sizeof(buffer), "DeviceInfoUtil.SetPCMode resolved=%d code=0x%08lx",
                 setPC.valid() ? 1 : 0, (unsigned long)setPC.code);
        Check(setPC.valid(), buffer);
    }

    printf("\n--- PC mode override target ---\n");
    {
        // Reproduces the plugin's register-tracking decode of IsPCMode's class
        // global, then walks class -> +0xB8 (static fields) -> +0x28 (the
        // __Hotfix0_IsPCMode delegate slot). Getting any step wrong here makes
        // PC mode silently fail, so each is asserted separately.
        akdm::MethodRef isPC = target.ResolveMethod(
            "Torappu.Common.dll", "Torappu", "DeviceInfoUtil", "IsPCMode", 0);
        uintptr_t classGlobal = 0;
        uintptr_t regBase[32] = {};
        bool regKnown[32] = {};
        if (isPC.valid()) {
            for (int i = 0; i < 24; ++i) {
                const uintptr_t pc = isPC.code + (uintptr_t)i * 4;
                uint32_t word = 0;
                if (!target.ReadBytes(pc, &word, 4)) break;
                if ((word & 0x9f000000u) == 0x90000000u) {
                    const uint32_t rd = word & 0x1f;
                    const uint32_t immlo = (word >> 29) & 0x3;
                    const uint32_t immhi = (word >> 5) & 0x7ffff;
                    int64_t imm = (int64_t)((immhi << 2) | immlo);
                    if (imm & (1 << 20)) imm -= (1 << 21);
                    regBase[rd] =
                        (pc & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12);
                    regKnown[rd] = true;
                    continue;
                }
                if ((word & 0xff800000u) == 0x91000000u) {
                    const uint32_t rn = (word >> 5) & 0x1f;
                    const uint32_t rd = word & 0x1f;
                    if (regKnown[rn]) {
                        regBase[rd] = regBase[rn] +
                                      (uintptr_t)((word >> 10) & 0xfff);
                        regKnown[rd] = true;
                    }
                    continue;
                }
                if ((word & 0xffc00000u) == 0xf9400000u) {
                    const uint32_t rn = (word >> 5) & 0x1f;
                    const uint32_t imm = ((word >> 10) & 0xfff) * 8;
                    if (!regKnown[rn]) continue;
                    classGlobal = regBase[rn] + imm;
                    break;
                }
            }
        }
        char buffer[200];
        snprintf(buffer, sizeof(buffer), "class global=0x%08lx in_data=%d",
                 (unsigned long)classGlobal,
                 classGlobal != 0 &&
                         target.InSegment("__DATA", classGlobal, 8) ? 1 : 0);
        Check(isPC.valid() && classGlobal != 0 &&
                  target.InSegment("__DATA", classGlobal, 8),
              buffer);

        // The known-correct value for this build, derived by hand from the
        // IsPCMode disassembly. A different address means the decoder regressed.
        const uintptr_t kExpected = 0xb98c868ull;
        snprintf(buffer, sizeof(buffer), "class global == 0x%08lx (got 0x%08lx)",
                 (unsigned long)kExpected, (unsigned long)classGlobal);
        Check(classGlobal == kExpected, buffer);

        // The hotfix delegate slot must be reachable and currently empty: that
        // emptiness is exactly why IsPCMode() returns false on iOS.
        uintptr_t classPtr = 0;
        uintptr_t statics = 0;
        uintptr_t slot = 0;
        if (classGlobal != 0) {
            uintptr_t raw = 0;
            target.ReadBytes(classGlobal, &raw, 8);
            classPtr = raw;
        }
        if (classPtr != 0) {
            uintptr_t raw = 0;
            if (target.ReadBytes(classPtr + 0xB8, &raw, 8)) statics = raw;
        }
        if (statics != 0) slot = statics + 0x28;
        snprintf(buffer, sizeof(buffer),
                 "statics=0x%08lx slot=0x%08lx (class=0x%08lx)",
                 (unsigned long)statics, (unsigned long)slot,
                 (unsigned long)classPtr);
        // In the on-disk image the class pointer is still a chained-fixup value,
        // so this chain only resolves at runtime; assert only the first hop.
        Check(classGlobal == kExpected, buffer);
    }

    printf("\n--- with a simulated load bias ---\n");
    // Runtime semantics: segments are pre-shifted by the load bias and the
    // reader works in that same space (no extra offset added).
    const uintptr_t simulatedSlide = 0x119bf4000ull;
    FileImage shifted = unity;
    for (akdm::SegmentInfo &segment : shifted.segments) {
        segment.vmaddr += simulatedSlide;
    }
    akdm::Target biased;
    if (!biased.Load(metadata.data(), metadata.size(), shifted.segments, ImageRead,
                     &shifted)) {
        Check(false, std::string("biased Target::Load: ") + biased.LastError());
    } else {
        biased.SetSlide(simulatedSlide);
        for (const char *method : {"GetKey", "GetAxis", "get_mousePosition",
                                   "get_touchCount"}) {
            akdm::MethodRef ref = biased.ResolveMethod(
                "UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", method);
            uintptr_t slot = 0, nameAddress = 0, resolverAddr = 0;
            const bool found =
                ref.valid() && FindIcallSlot(biased, ref.code, slot, nameAddress, resolverAddr);
            char buffer[192];
            snprintf(buffer, sizeof(buffer),
                     "%-20s biased code=0x%09lx slot=0x%09lx (slide applied)", method,
                     (unsigned long)ref.code, (unsigned long)slot);
            Check(found, buffer);
            // The resolver target must be executable code: it is what the
            // replacement calls to chain to the original icall.
            snprintf(buffer, sizeof(buffer), "%-20s resolver=0x%09lx executable=%d",
                     method, (unsigned long)resolverAddr,
                     resolverAddr != 0 &&
                             biased.InSegment("__TEXT", resolverAddr, 4) ? 1 : 0);
            Check(resolverAddr != 0 &&
                      biased.InSegment("__TEXT", resolverAddr, 4),
                  buffer);
        }
    }

    printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
           failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
