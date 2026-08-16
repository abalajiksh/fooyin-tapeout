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

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(TAPEOUT)

namespace Fooyin::Tapeout::Constants {
//! Settings page id.
constexpr auto SettingsPage = "Fooyin.Page.Integrations.Tapeout";

/*!
 * Becomes Tapedeck's `source_name` as `ingest:<client>`, so it must stay stable
 * across releases — changing it forks the listening history into two apparent
 * sources.
 */
constexpr auto SubmissionClient = "Tapeout";

//! Tapedeck refuses batches larger than this.
constexpr auto MaxListensPerRequest = 1000;

/*!
 * Tapedeck expires a now-playing entry once it runs past the track's duration,
 * so a long track needs re-reporting. Matches the bundled scrobbler's cadence.
 */
constexpr auto NowPlayingRefreshIntervalMs = 180000;
} // namespace Fooyin::Tapeout::Constants
