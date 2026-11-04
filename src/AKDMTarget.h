#pragma once
// Target identification and IL2CPP method resolution.
//
// Arknights is a Unity/IL2CPP title whose `il2cpp_*` API is not exported, so
// symbols cannot be looked up with dlsym. This module rebuilds the mapping from
// data that ships with the game:
//
//   Data/Managed/Metadata/global-metadata.dat -> images, types, methods
//   UnityFramework  Il2CppCodeGenModule       -> per-image method pointers
//
// Access is abstracted behind ImageReader so identical code runs inside the
// game (vm_read_overwrite on live memory) and in the offline validator (pread
// on the file). Every lookup is bounds checked, and a method is only accepted
// when its declaring type matches the requested one, so an unexpected layout
// yields "not supported" rather than a wrong address.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace akdm {

using ImageReader = bool (*)(void *context, uintptr_t address, void *out,
                             size_t size);

struct SegmentInfo {
    std::string name;
    uint64_t vmaddr = 0;
    uint64_t vmsize = 0;
    uint64_t fileoff = 0;
    uint64_t filesize = 0;
};

struct ImageInfo {
    std::string name;
    int32_t typeStart = -1;
    int32_t typeCount = 0;
    int32_t methodStart = -1;      // global method index of the image's first method
    int32_t methodCount = 0;
    uintptr_t methodPointers = 0;  // image address of Il2CppCodeGenModule.methodPointers
};

struct MethodRef {
    std::string image;
    std::string typeName;
    std::string name;
    int32_t typeIndex = -1;
    int32_t methodIndex = -1;  // global method index
    int32_t paramCount = -1;
    uintptr_t code = 0;        // image address of the compiled method
    bool valid() const { return code != 0; }
};

class Target {
public:
    bool Load(const void *metadata, size_t metadataSize,
              const std::vector<SegmentInfo> &segments, ImageReader reader,
              void *context);

    const std::string &LastError() const { return error_; }
    const std::vector<ImageInfo> &Images() const { return images_; }

    // Resolves image::ns.klass.method. `method == nullptr` resolves the type
    // only. `paramCount` (-1 = any) disambiguates overloads.
    MethodRef ResolveMethod(const char *image, const char *ns, const char *klass,
                            const char *method, int paramCount = -1);

    // Finds the codegen module for `image` and stores its method pointer
    // table in the ImageInfo. Called lazily by ResolveMethod.
    bool EnsureModules();

    bool ReadBytes(uintptr_t address, void *out, size_t size) const;

    // Some pointers read out of a codegen module are still link-time addresses
    // (an offline reader sees the file before dyld rebases it), others are
    // already runtime addresses. This maps whichever it is onto the runtime
    // space the segment table uses.
    uintptr_t Normalise(uintptr_t pointer) const;

    // Load bias applied by the caller (0 when segments are already runtime).
    uintptr_t slide_ = 0;
    void SetSlide(uintptr_t slide) { slide_ = slide; }
    const SegmentInfo *SegmentFor(uintptr_t address) const;
    bool InSegment(const char *segmentName, uintptr_t address,
                   size_t size = 1) const;

private:
    bool ParseHeader();
    bool ParseImages();
    const ImageInfo *FindImage(const char *name) const;
    int32_t FindType(const ImageInfo &image, const char *ns, const char *klass,
                     int32_t *typeIndex) const;
    uint32_t U32(size_t offset) const;
    int32_t I32(size_t offset) const;
    uint16_t U16(size_t offset) const;
    std::string Str(uint32_t index) const;
    bool RangeOk(size_t offset, size_t length) const;

    ImageReader reader_ = nullptr;
    void *context_ = nullptr;
    const uint8_t *metadata_ = nullptr;
    size_t metadataSize_ = 0;
    std::string error_;
    bool modulesReady_ = false;

    std::vector<SegmentInfo> segments_;
    std::vector<ImageInfo> images_;

    uint32_t stringOffset_ = 0, stringSize_ = 0;
    uint32_t typeDefOffset_ = 0, typeDefSize_ = 0;
    uint32_t methodOffset_ = 0, methodSize_ = 0;
    uint32_t imageOffset_ = 0, imageSize_ = 0;
};

// IL2CPP metadata layout constants for the accepted versions (v29..v31).
constexpr size_t kTypeDefSize = 92;
constexpr size_t kMethodDefSize = 32;
constexpr size_t kImageDefSize = 40;

} // namespace akdm
