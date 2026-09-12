/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#pragma once

#include <utils/settings/settingsentry.h>

namespace Fooyin {
class SettingsManager;

namespace Settings::Tapeout {
Q_NAMESPACE

enum TapeoutSettings : uint32_t
{
    Enabled = 0 | Type::Bool,
    //! Tapedeck base URL, no trailing slash and no path.
    ServerUrl = 1 | Type::String,
    //! API token. Needs `submit`; `read` unlocks the chain picker.
    Token = 2 | Type::String,
    //! Report audio quality (`tapedeck_audio`).
    SendQuality = 3 | Type::Bool,
    //! Report the output device (`tapedeck_device`), which drives chain bindings.
    SendDevice = 4 | Type::Bool,
    /*!
     * Submit skipped tracks as `skipped: true` rather than dropping them.
     * Tapedeck stores them and excludes them from every count.
     */
    SendSkips = 5 | Type::Bool,
    //! Explicit chain name, overriding every other rung of Tapedeck's ladder.
    ChainName = 6 | Type::String,
    /*!
     * Stable per-install identity, reported as `tapedeck_device.machine_id`.
     * Generated once on first run — see TapeoutSettings::machineId().
     */
    MachineId = 7 | Type::String,
    /*!
     * Send the words a file carries to Tapedeck's lyrics cache.
     *
     * Off by default, and not only out of caution: the endpoint is newer than
     * the rest of this plugin, so on an older Tapedeck this would 404 once per
     * track. Both of these also need the `write` scope, which a token minted
     * for scrobbling alone does not carry.
     */
    SendLyrics = 8 | Type::Bool,
    //! Offer the embedded cover for records Tapedeck has no artwork for.
    SendArtwork = 9 | Type::Bool,
    //! Love a recording in Tapedeck when its fooyin rating crosses LoveThreshold.
    SendLoves = 10 | Type::Bool,
    //! Whole stars, 1–5. Compared against fooyin's internal 0–10 half-star scale.
    LoveThreshold = 11 | Type::Int,
};
Q_ENUM_NS(TapeoutSettings)
} // namespace Settings::Tapeout

namespace Tapeout {
class TapeoutSettings
{
public:
    explicit TapeoutSettings(SettingsManager* settings);

private:
    SettingsManager* m_settings;
};
} // namespace Tapeout
} // namespace Fooyin
