// Sparkle auto-updater wrapper for macOS.
// SPUStandardUpdaterController handles the entire update flow:
// check → download → verify EdDSA signature → install → relaunch.

#import <Foundation/Foundation.h>

#ifdef SPARKLE_ENABLED
#import <Sparkle/Sparkle.h>

static SPUStandardUpdaterController* g_updaterController = nil;
#endif

void macos_updater_init()
{
#ifdef SPARKLE_ENABLED
    g_updaterController = [[SPUStandardUpdaterController alloc]
        initWithStartingUpdater:YES
        updaterDelegate:nil
        userDriverDelegate:nil];
#endif
}

// Explicit, user-initiated check. Shows Sparkle's full UI, including the
// "you're up to date" dialog. Wire this to a "Check for Updates…" menu item.
void macos_updater_check_now()
{
#ifdef SPARKLE_ENABLED
    [g_updaterController checkForUpdates:nil];
#endif
}

// Silent background check. Only surfaces UI when an update is actually
// available — ideal to call shortly after launch so an offer can appear
// promptly without waiting for the scheduled interval.
void macos_updater_check_in_background()
{
#ifdef SPARKLE_ENABLED
    [g_updaterController.updater checkForUpdatesInBackground];
#endif
}
