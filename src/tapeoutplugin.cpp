/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#include "tapeoutplugin.h"

#include "core/tapeoutcontroller.h"
#include "settings/tapeoutpage.h"
#include "tapeoutconstants.h"
#include "tapeoutsettings.h"

#include <utils/settings/settingsmanager.h>

Q_LOGGING_CATEGORY(TAPEOUT, "fy.tapeout")

namespace Fooyin::Tapeout {
void TapeoutPlugin::initialise(const CorePluginContext& context)
{
    m_settings = context.settingsManager;

    m_tapeoutSettings = std::make_unique<TapeoutSettings>(m_settings);
    m_controller = std::make_unique<TapeoutController>(context.playerController, context.engine,
                                                       context.playlistHandler, context.networkAccess, m_settings);
}

void TapeoutPlugin::initialise(const GuiPluginContext& context)
{
    Q_UNUSED(context)

    new TapeoutPage(m_controller->client(), m_settings, this);
}

void TapeoutPlugin::shutdown()
{
    if(m_controller) {
        m_controller->shutdown();
    }
}
} // namespace Fooyin::Tapeout

#include "moc_tapeoutplugin.cpp"
