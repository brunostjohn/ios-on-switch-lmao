// SPDX-License-Identifier: BSD-2-Clause-Patent
// CoreMotion's fast path on the device motion service (SwitchHID.h). On an iPad, CLSensorFusionServiceSPU opens it with
// IOHIDServiceClientFastPathInit (IOHIDLib.plugin's IOHIDEventServiceFastPathClass, plug-in type 0B842ADD-..., which
// opens IOHIDEventServiceFastPathUserClient) and reads the latest sample through it; without it CoreMotion logs
// "[FastPath] openFastPathPrivate() failed" at every start and takes the pushed sample. IOKit's
// IOCreatePlugInInterfaceForService reads IOCFPlugInTypes from the service itself (IORegistryEntryCreateCFProperty), and
// IOHIDEventService's defaults refuse the user client (openForClient false, copyEventForClient NULL), so the service's
// event driver gets the plug-in type and its own vtable copy:
//  * openForClient (reserved 18) admits the client. The user client puts FastPathHasEntitlement and
//    FastPathMotionEventEntitlement in the open properties (21H461 0xfffffff005ad1d14, 0xfffffff005ad1d40); SpringBoard
//    holds neither com.apple.hid.system.user-access-fast-path nor ...fast-path-motion-event-privileged, only the
//    sandbox's IOHIDEventServiceFastPathUserClient class, so the class is the gate here, and with no restricted motion
//    state nothing else differs between clients;
//  * copyEventForClient (reserved 15) answers the latest report's event, retained; the user client copies it into the
//    shared memory its client reads. None between sessions (switch_hid_forget_latest): CoreMotion then logs a failed
//    copy and takes the pushed sample (21H461's getter, locationd 0x100528874), which beats one from the session before;
//  * closeForClient (reserved 19) has nothing to undo;
//  * dispatchEvent (reserved 8, as the touch edge wrapper's) keeps that latest event, then dispatches it.
// Both builds' user clients call the three through these slots, with the provider at +0xe0 and the context at +0xe8:
// 21H461 IOHIDFamily open 0xfffffff005ad1d64, copy 0xfffffff005ad1ef4, close 0xfffffff005ad1e8c; 24B119 open
// 0xfffffff0099265d0, copy 0xfffffff0098cc338, close 0xfffffff0098cc294 (their PAC diversifiers are the vtable's
// slots 208, 205 and 209). Slots 199 and 200 must answer the device's 0xff0c / 5 first, as the temperature hook checks.
#include "SwitchHIDFastPath.h"
#include <stdint.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include "SwitchHID.h"

namespace {
// IOHIDEventService's reserved slot n is vtable slot 190 + n in both builds' IOHIDFamily: 21H461's from its
// IOHIDFamily-2031.120.4 header with slot 198 (reserved 8) proven on hardware, 24B119's read from its kernelcache's
// IOHIDEventDriver vtable (205-209 IOHIDEventService's defaults, 198 and 201 IOHIDEventDriver's own).
#if SWITCH_IPSW_KIND_24B119 || SWITCH_IPSW_KIND_21H461
enum : unsigned {
    kSlots = 256,
    kDispatch = 198,
    kPrimaryUsagePage = 199,
    kPrimaryUsage = 200,
    kCopyEventForClient = 205,
    kOpenForClient = 208,
    kCloseForClient = 209,
};

// IOHIDLib.plugin's CFPlugInTypes maps the type to its factory; the path is the one IOHIDFamily's embedded
// IOHIDEventDriver personality gives its other IOHIDLib types.
constexpr const char *kPlugInType = "0B842ADD-C395-4352-95F6-6585EB515623";  // kIOHIDServiceFastPathPlugInTypeID
constexpr const char *kPlugIn = "IOHIDFamily.kext/PlugIns/IOHIDLib.plugin";
#else
#error "SwitchHIDFastPath: no IOHIDEventService slots for this IPSW kind"
#endif

IOService *motionDevice;
IONotifier *notifier;
bool installed;  // the notification thread's
uintptr_t originalDispatch;
// copyEventForClient runs on the user client's thread, not the event driver's work loop (IOHIDEventService.h), while
// dispatchEvent replaces the event on the work loop: taking a reference and dropping the slot's must not interleave.
// A spin lock around a pointer swap and a retain.
IOSimpleLock *lock;
OSObject *latest;

template <typename R, typename... A> R call(void *object, unsigned slot, A... args)
{
    return reinterpret_cast<R (*)(void *, A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object, args...);
}

void keep(OSObject *event)
{
    if (event)
        event->retain();
    IOSimpleLockLock(lock);
    OSObject *replaced = latest;
    latest = event;
    IOSimpleLockUnlock(lock);
    OSSafeReleaseNULL(replaced);
}

// NOLINTBEGIN(misc-const-correctness): IOHIDEventService's slots, their parameters as declared
void dispatch(IOService *driver, OSObject *event, IOOptionBits options)
{
    keep(event);
    reinterpret_cast<void (*)(IOService *, OSObject *, IOOptionBits)>(originalDispatch)(driver, event, options);
}

OSObject *copyEventForClient(IOService *, OSObject *, IOOptionBits, void *)
{
    IOSimpleLockLock(lock);
    OSObject *event = latest;
    if (event)
        event->retain();
    IOSimpleLockUnlock(lock);
    return event;
}

bool openForClient(IOService *, IOService *, IOOptionBits, OSDictionary *, void **context)
{
    if (context)
        *context = nullptr;
    return true;
}

void closeForClient(IOService *, IOService *, void *, IOOptionBits)
{}
// NOLINTEND(misc-const-correctness)

void publishPlugIn(IOService *driver)
{
    const auto *types = OSDynamicCast(OSDictionary, driver->getProperty("IOCFPlugInTypes"));
    OSDictionary *merged =
        types ? OSDictionary::withDictionary(types, types->getCount() + 1) : OSDictionary::withCapacity(1);
    const OSString *plugIn = OSString::withCString(kPlugIn);
    if (merged && plugIn && merged->setObject(kPlugInType, plugIn))
        driver->setProperty("IOCFPlugInTypes", merged);
    OSSafeReleaseNULL(plugIn);
    OSSafeReleaseNULL(merged);
}

bool install(IOService *driver)
{
    const uint32_t page = call<uint32_t>(driver, kPrimaryUsagePage);
    const uint32_t usage = call<uint32_t>(driver, kPrimaryUsage);
    if (page != 0xff0c || usage != 5) {
        IOLog("SwitchHID: device motion fast path off: event driver slots 199/200 answer %#x/%#x, not 0xff0c/0x5 "
              "(IOHIDEventService's reserved slots moved)\n",
              page, usage);
        return false;
    }
    IOSimpleLock *created = IOSimpleLockAlloc();
    auto *table = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));
    if (!created || !table) {
        IOLog("SwitchHID: device motion fast path off: no memory\n");
        return false;
    }
    memcpy(table, *reinterpret_cast<uintptr_t **>(driver), kSlots * sizeof(uintptr_t));
    originalDispatch = table[kDispatch];
    table[kDispatch] = reinterpret_cast<uintptr_t>(&dispatch);
    table[kCopyEventForClient] = reinterpret_cast<uintptr_t>(&copyEventForClient);
    table[kOpenForClient] = reinterpret_cast<uintptr_t>(&openForClient);
    table[kCloseForClient] = reinterpret_cast<uintptr_t>(&closeForClient);
    publishPlugIn(driver);
    __atomic_store_n(&lock, created, __ATOMIC_RELEASE);
    __asm__ volatile("dmb ish" ::: "memory");
    *reinterpret_cast<uintptr_t **>(driver) = table;
#if DEVELOPMENT || DEBUG
    IOLog("SwitchHID: device motion fast path on (event driver slots 199/200 answer 0xff0c/0x5)\n");
#endif
    return true;
}

// IOKit's first publish of each IOHIDEventDriver, on the matching thread before the event system learns of the
// service, so CoreMotion never sees the service without the plug-in type.
bool published(void *, void *, IOService *driver, IONotifier *)
{
    const IOService *interface = driver ? driver->getProvider() : nullptr;
    if (!installed && interface && interface->getProvider() == motionDevice)
        installed = install(driver);
    return true;
}
}  // namespace

void switch_hid_fastpath_watch(IOService *device)
{
    motionDevice = device;
    OSDictionary *matching = IOService::serviceMatching("IOHIDEventDriver");
    if (matching)
        notifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, &published, nullptr);
    OSSafeReleaseNULL(matching);
    if (!notifier)
        IOLog("SwitchHID: device motion fast path off: no publish notification for its event driver\n");
}

void switch_hid_forget_latest(IOService *device)
{
    if (device == motionDevice && __atomic_load_n(&lock, __ATOMIC_ACQUIRE))
        keep(nullptr);
}
