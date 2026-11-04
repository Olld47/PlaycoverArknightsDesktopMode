// Runtime primitives: logging and bounded in-process memory reads.

#include "AKDMCompat.h"

#include <cstring>

#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <unistd.h>

namespace akdm {
namespace {

std::mutex &LogLock() {
    static std::mutex lock;
    return lock;
}

FILE *&LogFile() {
    static FILE *file = nullptr;
    return file;
}

} // namespace

void OpenLog() {
    if (AKDM_ENABLE_LOGGING == 0) return;
    std::lock_guard<std::mutex> lock(LogLock());
    if (LogFile()) return;
    @autoreleasepool {
        NSString *dir = [NSHomeDirectory()
            stringByAppendingPathComponent:@"Documents/AKDM"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:nil];
        NSString *path = [dir stringByAppendingPathComponent:@"plugin.log"];
        NSDictionary *attributes =
            [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
        if (attributes && [attributes fileSize] > 4u * 1024u * 1024u) {
            NSString *previous =
                [dir stringByAppendingPathComponent:@"plugin.previous.log"];
            [[NSFileManager defaultManager] removeItemAtPath:previous error:nil];
            [[NSFileManager defaultManager] moveItemAtPath:path
                                                    toPath:previous
                                                     error:nil];
        }
        LogFile() = fopen(path.UTF8String, "a");
    }
}

void CloseLog() {
    std::lock_guard<std::mutex> lock(LogLock());
    if (LogFile()) {
        fclose(LogFile());
        LogFile() = nullptr;
    }
}

void WriteLog(const char *format, ...) {
    std::lock_guard<std::mutex> lock(LogLock());
    if (!LogFile()) return;
    fprintf(LogFile(), "[%.3f] ", CFAbsoluteTimeGetCurrent());
    va_list ap;
    va_start(ap, format);
    vfprintf(LogFile(), format, ap);
    va_end(ap);
    fputc('\n', LogFile());
    fflush(LogFile());
}

bool ReadProcessMemory(uintptr_t address, void *out, size_t size) {
    if (address == 0 || out == nullptr || size == 0) return false;
    vm_size_t copied = 0;
    kern_return_t status = vm_read_overwrite(mach_task_self(), address,
                                             (vm_size_t)size,
                                             (vm_address_t)out, &copied);
    return status == KERN_SUCCESS && copied == size;
}

bool RegionIsReadable(uintptr_t address, size_t size) {
    if (address == 0 || size == 0) return false;
    mach_vm_address_t region = address;
    mach_vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t status = mach_vm_region(
        mach_task_self(), &region, &regionSize, VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (status != KERN_SUCCESS) return false;
    if (region > address || address - region > regionSize) return false;
    if (size > regionSize - (address - region)) return false;
    return (info.protection & VM_PROT_READ) != 0;
}

bool WriteProcessMemory(uintptr_t address, const void *data, size_t size) {
    if (address == 0 || data == nullptr || size == 0) return false;
    if (!RegionIsReadable(address, size)) {
        AKDM_LOG("write refused: 0x%lx is not mapped", (unsigned long)address);
        return false;
    }

    // arm64 uses 16KiB pages; protect exactly the pages the write touches and
    // remember what the mapping looked like so it can be restored.
    const uintptr_t pageSize = 0x4000;
    const uintptr_t pageStart = address & ~(pageSize - 1);
    const uintptr_t pageEnd = (address + size + pageSize - 1) & ~(pageSize - 1);
    const size_t length = pageEnd - pageStart;

    vm_region_basic_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_vm_address_t region = pageStart;
    mach_vm_size_t regionSize = 0;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t status = mach_vm_region(
        mach_task_self(), &region, &regionSize, VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (status != KERN_SUCCESS) return false;
    const vm_prot_t original = info.protection;

    if ((original & VM_PROT_WRITE) == 0) {
        status = mach_vm_protect(mach_task_self(), pageStart, length, false,
                                 original | VM_PROT_WRITE);
        if (status != KERN_SUCCESS) {
            AKDM_LOG("write failed: cannot make 0x%lx writable",
                     (unsigned long)pageStart);
            return false;
        }
    }

    std::memcpy((void *)address, data, size);

    if ((original & VM_PROT_WRITE) == 0) {
        status = mach_vm_protect(mach_task_self(), pageStart, length, false,
                                 original);
        if (status != KERN_SUCCESS) {
            AKDM_LOG("write warning: cannot restore protection on 0x%lx",
                     (unsigned long)pageStart);
        }
    }
    return true;
}

bool RegionIsExecutable(uintptr_t address, size_t length) {
    if (address == 0 || length == 0) return false;
    mach_vm_address_t region = address;
    mach_vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t status = mach_vm_region(
        mach_task_self(), &region, &regionSize, VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (status != KERN_SUCCESS) return false;
    if (region > address || address - region > regionSize) return false;
    if (length > regionSize - (address - region)) return false;
    const vm_prot_t required = VM_PROT_READ | VM_PROT_EXECUTE;
    return (info.protection & required) == required;
}

} // namespace akdm
