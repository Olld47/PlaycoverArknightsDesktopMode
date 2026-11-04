// IL2CPP metadata and codegen-module resolution. See AKDMTarget.h.

#include "AKDMTarget.h"
#include "AKDMCompat.h"

#include <cstring>

namespace akdm {
namespace {

constexpr uint32_t kMetadataMagic = 0xFAB11BAF;
constexpr int32_t kMetadataVersionMin = 29;
constexpr int32_t kMetadataVersionMax = 31;

// The header is: uint32 magic, int32 version, then 66 (offset, size) pairs.
// Pair i therefore starts at byte 8 + i * 8.
enum HeaderPair : int {
    kPairString = 2,
    kPairMethods = 5,
    kPairTypeDefinitions = 19,
    kPairImages = 20,
};

// Il2CppTypeDefinition (92 bytes).
constexpr size_t kTdName = 0;
constexpr size_t kTdNamespace = 4;
constexpr size_t kTdMethodStart = 40;
constexpr size_t kTdMethodCount = 68;

// Il2CppMethodDefinition (32 bytes).
constexpr size_t kMdName = 0;
constexpr size_t kMdDeclaringType = 4;
constexpr size_t kMdParamCount = 30;

// Il2CppImageDefinition (40 bytes).
constexpr size_t kImName = 0;
constexpr size_t kImTypeStart = 8;
constexpr size_t kImTypeCount = 12;

// Il2CppCodeGenModule: { const char *moduleName; uint32 count;
//                        const Il2CppMethodPointer *methodPointers; ... }
constexpr size_t kModName = 0;
constexpr size_t kModCount = 8;
constexpr size_t kModPointers = 16;

constexpr size_t kMaxImages = 4096;

} // namespace

bool Target::RangeOk(size_t offset, size_t length) const {
    return metadata_ != nullptr && offset <= metadataSize_ &&
           length <= metadataSize_ - offset;
}

uint32_t Target::U32(size_t offset) const {
    if (!RangeOk(offset, 4)) return 0;
    uint32_t value = 0;
    std::memcpy(&value, metadata_ + offset, 4);
    return value;
}

int32_t Target::I32(size_t offset) const { return (int32_t)U32(offset); }

uint16_t Target::U16(size_t offset) const {
    if (!RangeOk(offset, 2)) return 0;
    uint16_t value = 0;
    std::memcpy(&value, metadata_ + offset, 2);
    return value;
}

std::string Target::Str(uint32_t index) const {
    if (index >= stringSize_) return std::string();
    const size_t begin = stringOffset_ + index;
    if (!RangeOk(begin, 1)) return std::string();
    size_t end = begin;
    while (end < metadataSize_ && metadata_[end] != 0) ++end;
    return std::string((const char *)metadata_ + begin, end - begin);
}

uintptr_t Target::Normalise(uintptr_t pointer) const {
    if (pointer == 0) return 0;
    // Already inside a known segment: runtime address (or slide is zero).
    if (SegmentFor(pointer) != nullptr) return pointer;
    // Otherwise it is a link-time address from an unrebased image.
    if (slide_ != 0 && SegmentFor(pointer + slide_) != nullptr)
        return pointer + slide_;
    return pointer;
}

bool Target::ReadBytes(uintptr_t address, void *out, size_t size) const {
    return reader_ != nullptr && address != 0 && size != 0 &&
           reader_(context_, address, out, size);
}

const SegmentInfo *Target::SegmentFor(uintptr_t address) const {
    for (const SegmentInfo &segment : segments_) {
        if (address >= segment.vmaddr && address - segment.vmaddr < segment.vmsize)
            return &segment;
    }
    return nullptr;
}

bool Target::InSegment(const char *segmentName, uintptr_t address,
                       size_t size) const {
    const SegmentInfo *segment = SegmentFor(address);
    if (segment == nullptr || segment->name != segmentName) return false;
    return (address - segment->vmaddr) + size <= segment->vmsize;
}

bool Target::ParseHeader() {
    if (metadataSize_ < 8) {
        error_ = "metadata too small";
        return false;
    }
    if (U32(0) != kMetadataMagic) {
        error_ = "metadata magic mismatch";
        return false;
    }
    const int32_t version = I32(4);
    if (version < kMetadataVersionMin || version > kMetadataVersionMax) {
        error_ = "unsupported metadata version " + std::to_string(version);
        return false;
    }
    auto pair = [this](int index, uint32_t &offset, uint32_t &size) {
        const size_t at = 8 + (size_t)index * 8;
        if (!RangeOk(at, 8)) return false;
        offset = U32(at);
        size = U32(at + 4);
        return RangeOk(offset, size);
    };
    uint32_t o = 0, s = 0;
    if (!pair(kPairString, o, s)) { error_ = "string table out of range"; return false; }
    stringOffset_ = o; stringSize_ = s;
    if (!pair(kPairMethods, o, s)) { error_ = "method table out of range"; return false; }
    methodOffset_ = o; methodSize_ = s;
    if (!pair(kPairTypeDefinitions, o, s)) { error_ = "type table out of range"; return false; }
    typeDefOffset_ = o; typeDefSize_ = s;
    if (!pair(kPairImages, o, s)) { error_ = "image table out of range"; return false; }
    imageOffset_ = o; imageSize_ = s;
    return true;
}

bool Target::ParseImages() {
    const size_t count = imageSize_ / kImageDefSize;
    if (count == 0 || count > kMaxImages) {
        error_ = "implausible image count";
        return false;
    }
    const size_t typeDefCount = typeDefSize_ / kTypeDefSize;
    const size_t methodDefCount = methodSize_ / kMethodDefSize;

    images_.clear();
    images_.reserve(count);
    int64_t running = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t base = imageOffset_ + i * kImageDefSize;
        ImageInfo info;
        info.name = Str(U32(base + kImName));
        info.typeStart = I32(base + kImTypeStart);
        info.typeCount = I32(base + kImTypeCount);
        if (info.typeStart < 0 || info.typeCount < 0) {
            error_ = "negative type range in image " + info.name;
            return false;
        }
        if ((size_t)info.typeStart + (size_t)info.typeCount > typeDefCount) {
            error_ = "type range out of bounds in image " + info.name;
            return false;
        }
        int64_t methods = 0;
        for (int32_t t = 0; t < info.typeCount; ++t) {
            const size_t typeBase =
                typeDefOffset_ + (size_t)(info.typeStart + t) * kTypeDefSize;
            methods += U16(typeBase + kTdMethodCount);
        }
        info.methodStart = (int32_t)running;
        info.methodCount = (int32_t)methods;
        running += methods;
        images_.push_back(std::move(info));
    }
    if (running > (int64_t)methodDefCount) {
        error_ = "image method ranges exceed the method table";
        return false;
    }
    return true;
}

bool Target::EnsureModules() {
    if (modulesReady_) return true;
    modulesReady_ = true;

    // Il2CppCodeGenModule layout:
    //   const char *moduleName; uint32 count; const void **methodPointers; ...
    // Modules live in __DATA, one per image. The scan copies the segment once
    // and walks it a single time: doing this per image would mean millions of
    // vm_read calls and would stall the game.
    for (const SegmentInfo &segment : segments_) {
        if (segment.name != "__DATA" && segment.name != "__DATA_CONST") continue;

        const size_t readable =
            segment.filesize > 0 ? (size_t)segment.filesize : (size_t)segment.vmsize;
        if (readable < 0x40) continue;

        std::vector<uint8_t> snapshot(readable);
        const size_t chunk = 1u << 20;
        size_t copied = 0;
        while (copied < readable) {
            const size_t amount =
                (readable - copied) < chunk ? (readable - copied) : chunk;
            if (!ReadBytes((uintptr_t)(segment.vmaddr + copied),
                           snapshot.data() + copied, amount))
                break;
            copied += amount;
        }
        if (copied < 0x40) continue;
        snapshot.resize(copied);

        for (size_t offset = 0; offset + 0x40 <= snapshot.size(); offset += 8) {
            uint64_t count = 0;
            std::memcpy(&count, snapshot.data() + offset + kModCount, sizeof(count));
            if (count == 0) continue;

            // Cheap pre-filter: the count must match some image we still need.
            ImageInfo *match = nullptr;
            for (ImageInfo &image : images_) {
                if (image.methodPointers != 0) continue;
                if (image.methodCount > 0 && (uint64_t)image.methodCount == count) {
                    match = &image;
                    break;
                }
            }
            if (match == nullptr) continue;

            uintptr_t namePointer = 0;
            uintptr_t pointers = 0;
            std::memcpy(&namePointer, snapshot.data() + offset + kModName,
                        sizeof(namePointer));
            std::memcpy(&pointers, snapshot.data() + offset + kModPointers,
                        sizeof(pointers));
            if (namePointer == 0 || pointers == 0) continue;

            // Both fields may still hold link-time addresses when read from an
            // unrebased image; Normalise maps them onto the segment space.
            namePointer = Normalise(namePointer);
            pointers = Normalise(pointers);
            if (!InSegment("__DATA", pointers, 8) &&
                !InSegment("__DATA_CONST", pointers, 8))
                continue;

            char name[160] = {};
            if (!ReadBytes(namePointer, name, sizeof(name) - 1)) continue;
            if (match->name != name) continue;

            match->methodPointers = pointers;
        }
    }
    return true;
}

bool Target::Load(const void *metadata, size_t metadataSize,
                  const std::vector<SegmentInfo> &segments, ImageReader reader,
                  void *context) {
    metadata_ = (const uint8_t *)metadata;
    metadataSize_ = metadataSize;
    segments_ = segments;
    reader_ = reader;
    context_ = context;
    error_.clear();
    images_.clear();
    modulesReady_ = false;

    if (metadata_ == nullptr || metadataSize_ == 0) {
        error_ = "no metadata";
        return false;
    }
    if (reader_ == nullptr) {
        error_ = "no image reader";
        return false;
    }
    if (!ParseHeader()) return false;
    if (!ParseImages()) return false;
    return true;
}

const ImageInfo *Target::FindImage(const char *name) const {
    if (name == nullptr) return nullptr;
    for (const ImageInfo &image : images_) {
        if (image.name == name) return &image;
    }
    return nullptr;
}

int32_t Target::FindType(const ImageInfo &image, const char *ns,
                         const char *klass, int32_t *typeIndex) const {
    if (klass == nullptr) return -1;
    for (int32_t t = 0; t < image.typeCount; ++t) {
        const int32_t index = image.typeStart + t;
        const size_t base = typeDefOffset_ + (size_t)index * kTypeDefSize;
        if (Str(U32(base + kTdName)) != klass) continue;
        if (ns != nullptr && Str(U32(base + kTdNamespace)) != ns) continue;
        if (typeIndex) *typeIndex = index;
        return index;
    }
    return -1;
}

MethodRef Target::ResolveMethod(const char *image, const char *ns,
                                const char *klass, const char *method,
                                int paramCount) {
    MethodRef ref;
    const ImageInfo *info = FindImage(image);
    if (info == nullptr) {
        error_ = std::string("image not found: ") + (image ? image : "(null)");
        return ref;
    }
    int32_t typeIndex = -1;
    if (FindType(*info, ns, klass, &typeIndex) < 0) {
        error_ = std::string("type not found: ") + (ns ? ns : "") + "." +
                 (klass ? klass : "");
        return ref;
    }
    ref.image = image;
    ref.typeName = std::string(ns ? ns : "") + "." + (klass ? klass : "");
    ref.typeIndex = typeIndex;
    if (method == nullptr) {
        ref.name = klass;
        return ref;
    }

    const size_t methodDefCount = methodSize_ / kMethodDefSize;
    auto consider = [&](size_t index) {
        const size_t base = methodOffset_ + index * kMethodDefSize;
        if (I32(base + kMdDeclaringType) != typeIndex) return false;
        if (Str(U32(base + kMdName)) != method) return false;
        const int32_t count = U16(base + kMdParamCount);
        if (paramCount >= 0 && count != paramCount) return false;
        ref.name = method;
        ref.methodIndex = (int32_t)index;
        ref.paramCount = count;
        return true;
    };

    // Prefer the declared contiguous range, then fall back to a full scan for
    // builds where methodStart is not recorded.
    const size_t typeBase = typeDefOffset_ + (size_t)typeIndex * kTypeDefSize;
    const int32_t declaredStart = I32(typeBase + kTdMethodStart);
    const int32_t declaredCount = U16(typeBase + kTdMethodCount);
    if (declaredStart >= 0 && declaredCount > 0 &&
        (size_t)declaredStart + (size_t)declaredCount <= methodDefCount) {
        for (size_t i = 0; i < (size_t)declaredCount; ++i) {
            if (consider((size_t)declaredStart + i)) break;
        }
    }
    if (ref.methodIndex < 0) {
        for (size_t i = 0; i < methodDefCount; ++i) {
            if (consider(i)) break;
        }
    }
    if (ref.methodIndex < 0) {
        error_ = std::string("method not found: ") + ref.typeName + "::" + method;
        return ref;
    }

    if (!EnsureModules()) {
        error_ = "codegen modules unavailable";
        ref.methodIndex = -1;
        return ref;
    }
    info = FindImage(image);
    if (info == nullptr || info->methodPointers == 0) {
        error_ = std::string("no method pointer table for ") + image;
        ref.methodIndex = -1;
        return ref;
    }
    const int32_t relative = ref.methodIndex - info->methodStart;
    if (relative < 0 || relative >= info->methodCount) {
        error_ = std::string("method outside image range: ") + ref.typeName +
                 "::" + method;
        ref.methodIndex = -1;
        return ref;
    }
    uintptr_t code = 0;
    if (!ReadBytes(info->methodPointers + (uintptr_t)relative * sizeof(uintptr_t),
                   &code, sizeof(code)) ||
        code == 0) {
        error_ = std::string("method has no compiled code: ") + ref.typeName +
                 "::" + method;
        ref.methodIndex = -1;
        return ref;
    }
    ref.code = Normalise(code);
    return ref;
}

} // namespace akdm
