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
class TapedeckClient;

class TapeoutPage : public SettingsPage
{
    Q_OBJECT

public:
    TapeoutPage(TapedeckClient* client, SettingsManager* settings, QObject* parent = nullptr);
};
} // namespace Tapeout
} // namespace Fooyin
