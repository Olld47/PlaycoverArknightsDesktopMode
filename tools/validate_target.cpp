// Offline validator: runs the plugin's own Target resolver against the game
// files on disk. This is how the resolution logic is proven without launching
// the game. It is a developer tool; it is not shipped inside the dylib.
//
//   ./validate_target <path to .app>
//
// Exit code 0 means every expectation below resolved to real compiled code.

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
    std::string path;
    int fd = -1;
    uint64_t slide = 0; // added to vmaddr to obtain the address the reader sees
    std::vector<akdm::SegmentInfo> segments;
    uint64_t textLow = 0, textHigh = 0;
};

bool ReadAt(int fd, uint64_t offset, void *out, size_t size) {
    if (offset > (uint64_t)1 << 40) return false;
    ssize_t got = pread(fd, out, size, (off_t)offset);
    return got == (ssize_t)size;
}

// The reader receives vmaddr-space addresses and maps them to file offsets.
bool ImageRead(void *context, uintptr_t address, void *out, size_t size) {
    FileImage *image = (FileImage *)context;
    for (const akdm::SegmentInfo &segment : image->segments) {
        if (address < segment.vmaddr) continue;
        const uint64_t delta = address - segment.vmaddr;
        if (delta >= segment.vmsize) continue;
        if (delta + size > segment.filesize) return false; // zero-fill / BSS
        return ReadAt(image->fd, segment.fileoff + delta, out, size);
    }
    return false;
}

bool ParseMachO(FileImage &image) {
    uint8_t header[sizeof(mach_header_64)];
    if (!ReadAt(image.fd, 0, header, sizeof(header))) return false;
    const mach_header_64 *mh = (const mach_header_64 *)header;
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64) return false;

    uint64_t offset = sizeof(mach_header_64);
    for (uint32_t i = 0; i < mh->ncmds; ++i) {
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
    for (const akdm::SegmentInfo &segment : image.segments) {
        if (segment.name == "__TEXT") {
            image.textLow = segment.vmaddr;
            image.textHigh = segment.vmaddr + segment.vmsize;
        }
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

// Decodes the first `adrp` + `ldr` pair, returning the addressed cell.
bool DecodeIcallCell(akdm::Target &target, uintptr_t code, uintptr_t &cell) {
    for (int i = 0; i < 24; ++i) {
        uint32_t word = 0;
        if (!target.ReadBytes(code + i * 4, &word, 4)) return false;
        if ((word & 0x9f000000u) != 0x90000000u) continue; // adrp
        const uint32_t rd = word & 0x1f;
        const uint32_t immlo = (word >> 29) & 0x3;
        const uint32_t immhi = (word >> 5) & 0x7ffff;
        int64_t imm = (int64_t)((immhi << 2) | immlo);
        if (imm & (1 << 20)) imm -= (1 << 21);
        const uintptr_t pc = code + (uintptr_t)i * 4;
        const uintptr_t page = (pc & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12);
        uint32_t next = 0;
        if (!target.ReadBytes(pc + 4, &next, 4)) return false;
        if ((next & 0xffc00000u) != 0xf9400000u) continue; // ldr x,[x,#imm]
        if (((next >> 5) & 0x1f) != rd) continue;
        cell = page + ((uintptr_t)((next >> 10) & 0xfff) * 8);
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path to com.hypergryph.arknights.app>\n",
                argv[0]);
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
    unity.path = app + "/Frameworks/UnityFramework.framework/UnityFramework";
    unity.fd = open(unity.path.c_str(), O_RDONLY);
    if (unity.fd < 0 || !ParseMachO(unity)) {
        fprintf(stderr, "cannot read UnityFramework\n");
        return 2;
    }
    printf("UnityFramework: %zu segments\n", unity.segments.size());

    akdm::Target target;
    if (!target.Load(metadata.data(), metadata.size(), unity.segments,
                     ImageRead, &unity)) {
        fprintf(stderr, "Target::Load failed: %s\n", target.LastError().c_str());
        return 1;
    }
    printf("metadata loaded: %zu images\n\n", target.Images().size());

    struct Expectation {
        const char *image;
        const char *ns;
        const char *klass;
        const char *method;
        int params;
    };
    // Everything the plugin needs, with the parameter counts observed in this
    // build. A wrong count simply fails to resolve, which is the intent.
    const Expectation expectations[] = {
        {"Torappu.Common.dll", "Torappu", "DeviceInfoUtil", "IsPCMode", 0},
        {"Torappu.Common.dll", "Torappu", "DeviceInfoUtil", "SetPCMode", 1},
        {"Torappu.Common.dll", "Torappu.SDK", "SDKHelper", "IsU8ViewOpen", 0},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "GetKey", 1},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "GetKeyDown", 1},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "GetKeyUp", 1},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "GetAxis", 1},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "get_mousePosition", 0},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "get_mouseScrollDelta", 0},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "get_mousePresent", 0},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "GetMouseButton", 1},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "GetMouseButtonDown", 1},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "GetMouseButtonUp", 1},
        {"UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input", "get_touchCount", 0},
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Cursor", "set_visible", 1},
        // Cursor.get_visible is stripped by the IL2CPP linker in this build;
        // the plugin mirrors visibility itself instead of reading it.
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Cursor", "get_lockState", 0},
        {"Assembly-CSharp.dll", "Torappu", "TorappuKeyBoardLogic", "TriggerIfAnyKeyDown", 2},
        {"Assembly-CSharp.dll", "Torappu.UI", "TorappuTouchInputController", "_UpdateTouchInfo", 0},
    };

    for (const Expectation &e : expectations) {
        akdm::MethodRef ref = target.ResolveMethod(e.image, e.ns, e.klass,
                                                   e.method, e.params);
        const std::string label = std::string(e.image) + " " + e.ns + "." +
                                  e.klass + "::" + e.method;
        if (!ref.valid()) {
            Check(false, label + "  (" + target.LastError() + ")");
            continue;
        }
        // The address must be executable code inside __TEXT.
        const bool inText = ref.code >= unity.textLow && ref.code < unity.textHigh;
        uint32_t first = 0, second = 0;
        const bool readable = target.ReadBytes(ref.code, &first, 4) &&
                              target.ReadBytes(ref.code + 4, &second, 4);
        const bool prologue = readable && ((first & 0xff000000u) == 0xa9000000u ||
                                           (first & 0xff800000u) == 0xa9800000u ||
                                           (first & 0xffc00000u) == 0xa9800000u ||
                                           (first & 0xff000000u) == 0xd1000000u ||
                                           (first & 0xff000000u) == 0xd1010000u ||
                                           (first & 0xff000000u) == 0xd1020000u ||
                                           (first & 0xff000000u) == 0xd1030000u ||
                                           (first & 0xfffffc1fu) == 0xa9bf7bfdu);
        char buffer[256];
        snprintf(buffer, sizeof(buffer),
                 "%-58s code=0x%08lx in_text=%d prologue=%d", label.c_str(),
                 (unsigned long)ref.code, inText ? 1 : 0, prologue ? 1 : 0);
        Check(inText && prologue, buffer);
    }

    printf("\n--- icall cells (populated at runtime; decoded from the wrapper) ---\n");
    for (const Expectation &e : expectations) {
        if (std::strcmp(e.image, "UnityEngine.InputLegacyModule.dll") != 0) continue;
        akdm::MethodRef ref = target.ResolveMethod(e.image, e.ns, e.klass,
                                                   e.method, e.params);
        if (!ref.valid()) continue;
        uintptr_t cell = 0;
        const bool decoded = DecodeIcallCell(target, ref.code, cell);
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "%-24s cell=0x%08lx in_data=%d",
                 e.method, (unsigned long)cell,
                 decoded && target.InSegment("__DATA", cell, 8) ? 1 : 0);
        Check(decoded && target.InSegment("__DATA", cell, 8), buffer);
    }

    printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
           failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
