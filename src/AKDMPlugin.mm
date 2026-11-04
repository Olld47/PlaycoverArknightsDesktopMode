// AKDM: Arknights desktop (keyboard + mouse) mode for PlayCover.
//
// Loaded as an injected dylib. The plugin
//   1. verifies it is running inside Arknights,
//   2. rebuilds the IL2CPP symbol map from the game's own metadata,
//   3. switches the game's own PC-mode flag on,
//   4. feeds real hardware input into Unity's legacy input API,
//   5. lets the game keep its original pointer capture logic.
//
// Everything is reversible in the sense that it only touches process memory;
// nothing is written to disk and no game file is modified.

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "AKDMCompat.h"
#include "AKDMConfig.h"
#include "AKDMHardware.h"
#include "AKDMHook.h"
#include "AKDMInput.h"
#include "AKDMTarget.h"

namespace {

const char kPluginVersion[] __attribute__((used)) = "AKDM 0.1.0";

// Supported bundle identifiers. The game ships several regional builds that
// share the same IL2CPP layout.
const char *const kSupportedBundles[] = {
    "com.hypergryph.arknights",
    "com.YoStarEN.Arknights",
    "com.YoStarJP.Arknights",
    "com.YoStarKR.Arknights",
    "tw.txwy.ios.arknights",
};

const char kUnityFrameworkPath[] = "UnityFramework.framework/UnityFramework";

std::atomic<bool> gActive{false};
std::atomic<bool> gPCRequested{false};
std::atomic<bool> gPCDone{false};
// Set once the plugin has finished its one-time setup (config, bundle check).
std::atomic<bool> gConfigured{false};
// Set once UnityFramework has been found and the hooks are installed.
std::atomic<bool> gActivated{false};
// Guards Activate(); Tick() and the dyld callback can race.
std::mutex gActivateLock;
akdm::Config gConfig;
akdm::Target *gTarget = nullptr;
dispatch_source_t gTimer = nullptr;
std::atomic<unsigned long> gTicks{0};
std::atomic<unsigned long> gActivateAttempts{0};
uintptr_t gUnitySlide = 0;
std::atomic<int> gHooksInstalled{0};
std::atomic<bool> gHooksComplete{false};
std::atomic<bool> gTouchProbeReady{false};

// ---------------------------------------------------------------------------
// Image helpers
// ---------------------------------------------------------------------------

struct ImageRange {
    uintptr_t header = 0;   // in-process address of the mach header
    uintptr_t slide = 0;
    std::vector<akdm::SegmentInfo> segments;
};

bool ReadLoadCommands(const mach_header_64 *header, ImageRange &range) {
    const load_command *command =
        (const load_command *)((const uint8_t *)header + sizeof(mach_header_64));
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmdsize < sizeof(load_command)) return false;
        if (command->cmd == LC_SEGMENT_64) {
            const segment_command_64 *segment =
                (const segment_command_64 *)command;
            akdm::SegmentInfo info;
            char name[17] = {};
            __builtin_memcpy(name, segment->segname, 16);
            info.name = name;
            info.vmaddr = segment->vmaddr;
            info.vmsize = segment->vmsize;
            info.fileoff = segment->fileoff;
            info.filesize = segment->filesize;
            range.segments.push_back(info);
        }
        command = (const load_command *)((const uint8_t *)command + command->cmdsize);
    }
    return !range.segments.empty();
}

// Locates UnityFramework among the loaded images.
bool FindUnityFramework(ImageRange &range) {
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = _dyld_get_image_name(i);
        if (name == nullptr) continue;
        if (strstr(name, kUnityFrameworkPath) == nullptr) continue;
        const mach_header *header = _dyld_get_image_header(i);
        if (header == nullptr || header->magic != MH_MAGIC_64) continue;
        if (header->cputype != CPU_TYPE_ARM64) continue;
        range.header = (uintptr_t)header;
        range.slide = _dyld_get_image_vmaddr_slide(i);
        return ReadLoadCommands((const mach_header_64 *)header, range);
    }
    return false;
}

// Reads from the UnityFramework image. Addresses are the image's own vmaddr
// values, so the slide has to be added; the reader context carries it.
struct ImageContext {
    uintptr_t slide = 0;
};

// Target works entirely in *runtime* addresses. That is the natural space:
//   * Il2CppCodeGenModule.methodPointers entries are rebased by dyld,
//   * adrp/ldr decoding is relative to the runtime PC,
//   * the segments we hand over are pre-shifted by the load bias.
// So this reader must NOT add the slide itself.
bool ReadImage(void *context, uintptr_t address, void *out, size_t size) {
    (void)context;
    return akdm::ReadProcessMemory(address, out, size);
}

// ---------------------------------------------------------------------------
// Game metadata
// ---------------------------------------------------------------------------

// global-metadata.dat ships unencrypted next to the managed assemblies.
std::vector<uint8_t> LoadMetadata() {
    @autoreleasepool {
        NSString *bundle = [[NSBundle mainBundle] bundlePath];
        NSString *path =
            [bundle stringByAppendingPathComponent:
                        @"Data/Managed/Metadata/global-metadata.dat"];
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (data == nil || data.length == 0) return {};
        std::vector<uint8_t> bytes((size_t)data.length);
        __builtin_memcpy(bytes.data(), data.bytes, (size_t)data.length);
        return bytes;
    }
}

// ---------------------------------------------------------------------------
// PC mode
// ---------------------------------------------------------------------------

// DeviceInfoUtil.SetPCMode(true) compiles down to a single store of 1 into a
// static byte, guarded by a class-initialisation check. Calling the managed
// method from a native input hook is unsafe (the IL2CPP runtime may not be
// ready for that class yet, and it faults with a pointer-authentication error).
// Writing the byte directly has the same effect with none of that risk.

// Invoked on the IL2CPP thread (from a hook) so the managed call is legal.
// PC mode must be forced on: it is what routes input to the real key pipeline.
//
// TorappuPCInputHelper::OnProcess:
//     +64  BL   DeviceInfoUtil.IsPCMode()
//     +68  TBZ  w0, #0 -> 0x3050990      ; IsPCMode() == false
//     +6c  B    TriggerStandAloneProcess ; IsPCMode() == true  <- real keyboard
//     0x3050990: B _TriggerKeyProcessOnMobile ; only polls GetKeyDown(Escape)
//
// TriggerStandAloneProcess tail-calls TorappuKeyboardTriggerProcessor::
// TriggerKeyProcess, which is the actual key-binding pipeline. So IsPCMode()
// must return true, and __Hotfix0_IsPCMode is the only switch that makes it so
// in this build (the managed setter routes through an unassembled strategy).
void RequestPCMode() {
    if (gPCDone.load()) return;
    if (gTarget == nullptr) return;
    if (akdm::InstallPCModeOverride(*gTarget, gUnitySlide)) {
        gPCDone.store(true);
        // IsPCMode() is now forced true, so the next question is whether
        // OnProcess even reaches that branch: it tests [statics+0x50] first.
        akdm::ProbeInputModule(*gTarget);
    }
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

bool IsSupportedBundle() {
    @autoreleasepool {
        NSString *identifier = [[NSBundle mainBundle] bundleIdentifier];
        if (identifier == nil) return false;
        for (const char *supported : kSupportedBundles) {
            if ([identifier isEqualToString:@(supported)]) return true;
        }
        return false;
    }
}

void StartTimer();

// PC mode is deliberately NOT enabled by writing memory.
//
// Both bytes near DeviceInfoUtil's statics are IL2CPP class-initialisation
// guards, not the PC flag: IsPCMode reads the real value through
// `class metadata -> +0xb8 -> +0x28`. Setting a guard byte makes IL2CPP skip
// class initialisation, and the first field access then dereferences an
// uninitialised pointer (observed as a crash inside IsPCMode itself).
//
// The only safe way to flip PC mode is to call the managed setter, which needs
// a valid IL2CPP thread. That is handled below.

// One-time setup: logging, config, and the bundle check. Runs in the dylib
// constructor, which is the only moment early enough to read the config.
void Configure() {
    if (gConfigured.exchange(true)) return;

    akdm::OpenLog();
    AKDM_LOG("%s starting", kPluginVersion);

    gConfig = akdm::LoadConfig();

    if (!IsSupportedBundle()) {
        AKDM_LOG("not Arknights; plugin inactive");
        return;
    }
    AKDM_LOG("bundle ok");
}

// Installs the hooks. Safe to call repeatedly: it returns immediately once it
// has succeeded, and is retried while UnityFramework is still absent.
//
// UnityFramework is NOT linked by the Arknights executable; the game dlopen()s
// it from GameLoader after launch. So at constructor time it is usually not
// loaded yet, and looking for it once would always fail.
bool Activate() {
    if (gActivated.load()) return true;
    std::lock_guard<std::mutex> lock(gActivateLock);
    if (gActivated.load()) return true;
    if (!gConfigured.load()) return false;

    const unsigned long attempt = gActivateAttempts.fetch_add(1) + 1;

    ImageRange unity;
    if (!FindUnityFramework(unity)) {
        // Log what is actually loaded: the framework is dlopen()ed by the game,
        // so "not found" usually just means "not yet".
        const uint32_t count = _dyld_image_count();
        AKDM_LOG("activate #%lu: UnityFramework not loaded yet (%u images)", attempt,
                 count);
        for (uint32_t i = 0; i < count; ++i) {
            const char *name = _dyld_get_image_name(i);
            if (name != nullptr && strstr(name, "Unity") != nullptr)
                AKDM_LOG("  loaded: %s", name);
        }
        return false;
    }
    AKDM_LOG("activate #%lu: UnityFramework at %p slide 0x%lx segments %zu", attempt,
             (void *)unity.header, (unsigned long)unity.slide, unity.segments.size());

    std::vector<uint8_t> metadata = LoadMetadata();
    if (metadata.empty()) {
        AKDM_LOG("global-metadata.dat unreadable; will retry");
        return false;
    }

    // The reader keeps a pointer to its context, so both must outlive the
    // Target. They are function-local statics for exactly that reason.
    static ImageContext context;
    static std::vector<uint8_t> metadataStore;
    metadataStore = std::move(metadata);

    // Convert the link-time segment table into runtime addresses once, so every
    // address Target reports or consumes lives in the same space.
    for (akdm::SegmentInfo &segment : unity.segments) {
        segment.vmaddr += unity.slide;
    }

    akdm::Target *target = new akdm::Target();
    if (!target->Load(metadataStore.data(), metadataStore.size(), unity.segments,
                      ReadImage, &context)) {
        AKDM_LOG("target resolution failed: %s", target->LastError().c_str());
        delete target;
        return false;
    }
    gUnitySlide = unity.slide;
    target->SetSlide(unity.slide);
    AKDM_LOG("metadata parsed: %zu images", target->Images().size());

    if (gConfig.enablePCMode) {
        // PC mode is enabled by populating __Hotfix0_IsPCMode (see
        // InstallPCModeOverride); the managed SetPCMode setter is a no-op in
        // this build. Its resolution is logged only as a sanity check.
        akdm::MethodRef setPC = target->ResolveMethod(
            "Torappu.Common.dll", "Torappu", "DeviceInfoUtil", "SetPCMode", 1);
        AKDM_LOG("DeviceInfoUtil.SetPCMode resolved=%d (unused; hotfix slot is "
                 "the real switch)",
                 setPC.valid() ? 1 : 0);
    } else {
        // Without PC mode the game never queries Input.GetKey, so the keyboard
        // hooks cannot install and the plugin cannot do its job. Say so loudly.
        AKDM_LOG("pc mode disabled by config: keyboard hooks will never install");
    }

    akdm::InputHookReport hooks;
    if (gConfig.enableInputHooks) {
        hooks = akdm::InstallInputHooks(*target, unity.slide);
    } else {
        AKDM_LOG("input hooks disabled by config");
    }
    AKDM_LOG("input hooks: %d/%d installed", hooks.installed, hooks.attempted);
    AKDM_LOG("  keys=%d/%d/%d axis=%d/%d mouse=%d/%d/%d/%d/%d touch=%d/%d",
             hooks.getKey ? 1 : 0, hooks.getKeyDown ? 1 : 0, hooks.getKeyUp ? 1 : 0,
             hooks.getAxis ? 1 : 0, hooks.getAxisRaw ? 1 : 0,
             hooks.mousePosition ? 1 : 0, hooks.mousePresent ? 1 : 0,
             hooks.mouseButton ? 1 : 0, hooks.mouseButtonDown ? 1 : 0,
             hooks.mouseButtonUp ? 1 : 0, hooks.touchCount ? 1 : 0,
             hooks.getTouch ? 1 : 0);

    // A first pass that installs nothing is normal: Unity resolves each icall
    // lazily, so most slots are still zero until the game has used that
    // wrapper. Keep the Target and let the maintenance pass fill them in.
    if (hooks.installed == 0) {
        AKDM_LOG("no hooks installable yet; will retry once slots resolve");
    }

    gTarget = target;

    // The override needs DeviceInfoUtil's class and static fields to exist, and
    // writing them is only safe on the IL2CPP thread. The touch probe fires on
    // that thread every frame, so it is the trigger; the maintenance loop also
    // retries in case the class is still initialising.
    if (gConfig.enablePCMode) {
        // Every-call action: the static fields may not exist on the first few
        // queries, so this must keep trying rather than fire once.
        akdm::SetProbeAction(&RequestPCMode);
        AKDM_LOG("pc mode: override armed on the touch probe (retrying)");
    }

    akdm::ProbeCursorHook(*gTarget, gUnitySlide);
    // Locate Unity's internal key-state setter. The game never polls
    // Input.GetKey on iOS (its PC input module is never assembled), so the
    // hooks alone cannot deliver anything; this is the path that actually
    // reaches Unity's own input state.
    akdm::FindUnitySetKeyState(*gTarget);
    if (!akdm::InstallTouchProbe(*gTarget, gUnitySlide)) {
        AKDM_LOG("touch probe deferred (slot not resolved yet)");
    }
    akdm::AttachHardwareObservers();

    gHooksInstalled.store(hooks.installed);
    gHooksComplete.store(hooks.installed >= 3);
    gActive.store(true);
    gActivated.store(true);
    AKDM_LOG("plugin active=1 after %lu attempt(s)", attempt);
    return true;
}

// dyld calls this for every image that is added to the process, including
// UnityFramework when the game loads it.
void OnImageAdded(const mach_header *header, intptr_t slide) {
    (void)slide;
    if (header == nullptr || gActivated.load()) return;
    const uint32_t index = _dyld_image_count();
    for (uint32_t i = 0; i < index; ++i) {
        if (_dyld_get_image_header(i) != header) continue;
        const char *name = _dyld_get_image_name(i);
        if (name == nullptr || strstr(name, kUnityFrameworkPath) == nullptr) break;
        AKDM_LOG("dyld: UnityFramework appeared");
        // Do not activate inline: this runs under dyld's lock while the image
        // is still being registered, and Activate() reads tens of megabytes and
        // allocates. Hop to the main queue so dyld finishes first.
        dispatch_async(dispatch_get_main_queue(), ^{
            @autoreleasepool {
                Activate();
            }
        });
        break;
    }
}

// The 16ms pointer-polling timer is gone: PlayTools owns the mouse-to-touch
// path end to end, so the plugin has no cursor position of its own to publish.


// Runs every 1.5s. Maintenance, retries and diagnostics.
void SlowTick() {
    gTicks.fetch_add(1);
    if (!gActivated.load()) {
        if (gConfigured.load() && IsSupportedBundle()) Activate();
        return;
    }
    // Retry hook installation: a slot only becomes installable once the game
    // has used that Unity wrapper at least once, so the first pass usually
    // defers most of them.
    if (gTarget != nullptr && !gTouchProbeReady.load()) {
        if (akdm::InstallTouchProbe(*gTarget, gUnitySlide)) {
            gTouchProbeReady.store(true);
        }
    }
    if (gTarget != nullptr && !gHooksComplete.load()) {
        akdm::InputHookReport again = akdm::InstallInputHooks(*gTarget, gUnitySlide);
        if (again.installed > 0) {
            const int total = gHooksInstalled.fetch_add(again.installed) +
                              again.installed;
            AKDM_LOG("hook retry: +%d installed (total %d)", again.installed, total);
            if (total >= 3) gHooksComplete.store(true);
        }
    }
    akdm::AttachHardwareObservers();
    // Keep sampling: if the game assembles its PC input module at any point the
    // gate flips and the log records it, with no hook installed.
    if (gTarget != nullptr && gPCDone.load()) akdm::ProbeInputModule(*gTarget);

    akdm::InputCounters counters = akdm::SnapshotCounters();
    AKDM_LOG("tick=%lu keys=%llu pc=%d",
             gTicks.load(), (unsigned long long)counters.keys,
             gPCDone.load() ? 1 : 0);
}

// A single slow maintenance timer; the former fast pointer-polling timer is
// gone because PlayTools owns the mouse-to-touch path end to end.
void StartTimer() {
    if (gTimer == nullptr) {
        gTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                        dispatch_get_main_queue());
        if (gTimer != nullptr) {
            const uint64_t interval = 1500ull * NSEC_PER_MSEC;
            dispatch_source_set_timer(gTimer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                      interval, 200ull * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(gTimer, ^{
                @autoreleasepool {
                    SlowTick();
                }
            });
            dispatch_resume(gTimer);
        } else {
            AKDM_LOG("slow timer unavailable");
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

__attribute__((constructor)) static void AKDMStart(void) {
    @autoreleasepool {
        // Arknights does not link UnityFramework; the game loads it later with
        // dlopen. So this only does the one-time setup and then arranges for
        // activation to happen as soon as the framework appears:
        //   * a dyld add-image callback (immediate), and
        //   * a periodic retry (covers a framework loaded before we were).
        Configure();
        if (gConfigured.load() && IsSupportedBundle()) {
            _dyld_register_func_for_add_image(OnImageAdded);
            Activate();
            StartTimer();
        }
    }
}

__attribute__((destructor)) static void AKDMStop(void) {
    akdm::CloseLog();
}
