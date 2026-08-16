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
