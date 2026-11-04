#pragma once
// Minimal, dependency-free C++ primitives shared by every translation unit.
//
// Nothing here touches the game. It provides bounded in-process memory reads
// and a logging facility that compiles out of release builds.
//
// Design rule for the whole plugin: every address is derived from the game's
// own metadata and cross-checked before use; anything unexpected is refused
// rather than guessed at.

#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>

#ifndef AKDM_ENABLE_LOGGING
#define AKDM_ENABLE_LOGGING 0
#endif
static_assert(AKDM_ENABLE_LOGGING == 0 || AKDM_ENABLE_LOGGING == 1,
              "AKDM_ENABLE_LOGGING must be 0 or 1");

namespace akdm {

// ---------------------------------------------------------------------------
// Logging (implemented in AKDMCompat.mm)
// ---------------------------------------------------------------------------

void OpenLog();
void CloseLog();
void WriteLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Arguments are not evaluated at all when logging is compiled out.
#define AKDM_LOG(...)                       \
    do {                                    \
        if (AKDM_ENABLE_LOGGING != 0) {     \
            ::akdm::WriteLog(__VA_ARGS__);  \
        }                                   \
    } while (false)

// ---------------------------------------------------------------------------
// Memory access
// ---------------------------------------------------------------------------

// Reads live process memory via vm_read_overwrite (implemented in .mm).
bool ReadProcessMemory(uintptr_t address, void *out, size_t size);

inline bool ReadMemory(uintptr_t address, void *out, size_t size) {
    if (address == 0 || size == 0) return false;
    return ReadProcessMemory(address, out, size);
}

template <typename T>
inline bool ReadValue(uintptr_t address, T &out) {
    return ReadMemory(address, &out, sizeof(T));
}

// Writes `size` bytes at `address`. If the page is read-only the protection is
// temporarily raised to writable and restored afterwards, so a wrong address
// fails cleanly instead of taking the process down.
bool WriteProcessMemory(uintptr_t address, const void *data, size_t size);

// Convenience wrapper for a single pointer-sized write.
inline bool WritePointer(uintptr_t address, uintptr_t value) {
    return WriteProcessMemory(address, &value, sizeof(value));
}

// True when [address, address+size) is currently mapped and readable.
bool RegionIsReadable(uintptr_t address, size_t size);

inline bool BytesMatch(uintptr_t address, const void *expected, size_t size) {
    if (size == 0 || size > 256) return false;
    unsigned char actual[256];
    return ReadProcessMemory(address, actual, size) &&
           std::memcmp(actual, expected, size) == 0;
}

// True when [address, address+length) sits inside one RX mapping.
bool RegionIsExecutable(uintptr_t address, size_t length);

} // namespace akdm
