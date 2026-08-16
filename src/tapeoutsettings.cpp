/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#include "tapeoutsettings.h"

#include <utils/settings/settingsmanager.h>

#include <QUuid>

using namespace Qt::StringLiterals;

namespace Fooyin::Tapeout {
TapeoutSettings::TapeoutSettings(SettingsManager* settings)
    : m_settings{settings}
{
    using namespace Settings::Tapeout;

    m_settings->createSetting<Enabled>(false, u"Tapeout/Enabled"_s);
    m_settings->createSetting<ServerUrl>(u""_s, u"Tapeout/ServerUrl"_s);
    m_settings->createSetting<Token>(u""_s, u"Tapeout/Token"_s);
    m_settings->createSetting<SendQuality>(true, u"Tapeout/SendQuality"_s);
    m_settings->createSetting<SendDevice>(true, u"Tapeout/SendDevice"_s);
    m_settings->createSetting<SendSkips>(true, u"Tapeout/SendSkips"_s);
    m_settings->createSetting<ChainName>(u""_s, u"Tapeout/ChainName"_s);
    m_settings->createSetting<MachineId>(u""_s, u"Tapeout/MachineId"_s);

    // Tapedeck keys devices on this, and a value that changed between runs would
    // file every session as a new device. Minted once, then left alone.
    if(m_settings->value<MachineId>().isEmpty()) {
        m_settings->set<MachineId>(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
}
} // namespace Fooyin::Tapeout
