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

#include <utils/settings/settingspage.h>

namespace Fooyin {
class SettingsManager;

namespace Tapeout {
class TapeoutController;

class TapeoutPage : public SettingsPage
{
    Q_OBJECT

public:
    /*!
     * Takes the controller rather than the client: the page needs the playhead's
     * current track for a dry run and the live output device name for the
     * binding readout, and neither of those is the client's to know.
     */
    TapeoutPage(TapeoutController* controller, SettingsManager* settings, QObject* parent = nullptr);
};
} // namespace Tapeout
} // namespace Fooyin
