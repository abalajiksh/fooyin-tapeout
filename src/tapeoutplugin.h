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

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

#include <memory>

namespace Fooyin::Tapeout {
class TapeoutController;
class TapeoutSettings;

class TapeoutPlugin : public QObject,
                      public Plugin,
                      public CorePlugin,
                      public GuiPlugin
{
    Q_OBJECT
    // Generated from tapeout.json.in into the build dir, which create_fooyin_plugin
    // puts on the include path.
    Q_PLUGIN_METADATA(IID "org.fooyin.fooyin.plugin/1.0" FILE "tapeout.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin)

public:
    void initialise(const CorePluginContext& context) override;
    void initialise(const GuiPluginContext& context) override;
    void shutdown() override;

private:
    SettingsManager* m_settings{nullptr};

    std::unique_ptr<TapeoutSettings> m_tapeoutSettings;
    std::unique_ptr<TapeoutController> m_controller;
};
} // namespace Fooyin::Tapeout
