// <switch/boot_sync.h>: which of the changes the policy step makes on Data before launchd (bsd/kern/kern_exec.c) go to
// the card synchronously, over the changes each kind of boot makes there, in the step's order.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <switch/boot_sync.h>

namespace {

constexpr switch_boot_change kDurable = SWITCH_BOOT_DURABLE, kRedone = SWITCH_BOOT_REDONE;

const char kLaunchd[] = "/private/var/db/com.apple.xpc.launchd";
const char kPolicy[] = "/private/var/db/com.apple.xpc.launchd/disabled.plist";
const char kMigrated[] = "/private/var/db/com.apple.xpc.launchd/disabled.migrated";
const char kManaged[] = "/private/var/Managed Preferences/mobile/com.apple.springboard.plist";
const char kPurpleBuddy[] = "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist";
const char kSeed[] = "/private/var/mobile/Library/Preferences/com.apple.migration.plist";
const char kTimedCache[] = "/private/var/db/timed/com.apple.timed.plist";
const char kTimedMarker[] = "/private/var/Switch/monotonic-clock-boot";
const char kIconInfo[] = "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache/Library/"
                         "Caches/com.apple.IconsCache/28A6C7EA-97A4-3002-8473-901196984E6C/__cache_info__";
const char kIconMarker[] = "/private/var/Switch/icon-cache-reset";
const char kPanic[] = "/private/var/Switch/panic-2026-11-23-061500.log";
const char kPanicCopy[] = "/private/var/mobile/Library/Logs/CrashReporter/Switch-panic-2026-11-23-061500.log";

struct Change {
    switch_boot_change kind;
    const char *path;
};

// Each change's answer: whether its write goes out synchronously.
std::vector<bool> Make(switch_boot_sync &sync, const std::vector<Change> &changes)
{
    std::vector<bool> synced;
    for (const Change &change : changes) {
        synced.push_back(switch_boot_sync_change(&sync, change.kind, change.path));
    }
    return synced;
}

TEST(SwitchBootSync, AnOrdinaryBootSyncsNothing)
{
    switch_boot_sync sync = {};
    EXPECT_FALSE(switch_boot_sync_due(&sync)) << "the directory syncs before the policy file";
    EXPECT_EQ(Make(sync, {{kRedone, kTimedMarker}}), std::vector<bool>({false}));
    EXPECT_FALSE(switch_boot_sync_due(&sync)) << "the directory and volume syncs after it";
    EXPECT_EQ(sync.durable, 0u);
}

TEST(SwitchBootSync, ADeployBootDropsIconServicesCacheUnsynced)
{
    switch_boot_sync sync = {};
    const std::vector<Change> deploy = {{kRedone, kIconInfo},
                                        {kRedone, kIconInfo},
                                        {kRedone, kIconMarker},
                                        {kRedone, kTimedCache},
                                        {kRedone, kTimedMarker}};
    EXPECT_EQ(Make(sync, deploy), std::vector<bool>(deploy.size(), false));
    EXPECT_FALSE(switch_boot_sync_due(&sync));
}

TEST(SwitchBootSync, AChangedOverrideIsSyncedWithEverySyncAfterIt)
{
    switch_boot_sync sync = {};
    EXPECT_FALSE(switch_boot_sync_due(&sync)) << "/private/var/db and the launchd directory before the policy";
    EXPECT_TRUE(switch_boot_sync_change(&sync, kDurable, kPolicy)) << "the policy file itself";
    EXPECT_TRUE(switch_boot_sync_due(&sync)) << "the launchd directory before its migration marker";
    EXPECT_EQ(Make(sync, {{kRedone, kTimedMarker}}), std::vector<bool>({true})) << "a durable boot's later writes";
    EXPECT_TRUE(switch_boot_sync_due(&sync)) << "the volume";
    EXPECT_EQ(sync.durable, 1u);
    EXPECT_STREQ(sync.first, kPolicy);
}

TEST(SwitchBootSync, AFreshDataVolumeIsSyncedFromItsFirstDirectory)
{
    switch_boot_sync sync = {};
    EXPECT_TRUE(switch_boot_sync_change(&sync, kDurable, kLaunchd)) << "the new directory itself";
    EXPECT_TRUE(switch_boot_sync_due(&sync)) << "the new directory's own sync";
    const std::vector<Change> fresh = {{kDurable, kPolicy},      {kDurable, kMigrated}, {kDurable, kManaged},
                                       {kDurable, kPurpleBuddy}, {kDurable, kSeed},     {kRedone, kTimedMarker}};
    EXPECT_EQ(Make(sync, fresh), std::vector<bool>(fresh.size(), true));
    EXPECT_STREQ(sync.first, kLaunchd);
}

TEST(SwitchBootSync, APanicSaveSyncsTheRecordAndTheVolumeNotTheWritesBefore)
{
    switch_boot_sync sync = {};
    const std::vector<Change> panicked = {{kRedone, kTimedMarker}, {kDurable, kPanic}, {kDurable, kPanicCopy}};
    EXPECT_EQ(Make(sync, panicked), std::vector<bool>({false, true, true}));
    EXPECT_TRUE(switch_boot_sync_due(&sync));
    EXPECT_EQ(sync.durable, 2u);
    EXPECT_STREQ(sync.first, kPanic);
}

TEST(SwitchBootSync, TheFirstDurablePathIsKeptCutToFit)
{
    switch_boot_sync sync = {};
    const std::string deep = "/private/var/" + std::string(200, 'd');
    (void)switch_boot_sync_change(&sync, kRedone, kTimedMarker);
    (void)switch_boot_sync_change(&sync, kDurable, deep.c_str());
    (void)switch_boot_sync_change(&sync, kDurable, kPolicy);
    EXPECT_EQ(std::string(sync.first), deep.substr(0, sizeof(sync.first) - 1));
}

}  // namespace
