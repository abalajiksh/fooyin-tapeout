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
 * How often to re-report the playhead while a track is on.
 *
 * Much shorter than the bundled scrobbler's three minutes, because this now
 * carries a position rather than only "a track started". Tapedeck forwards a
 * now-playing outward solely on a real change of track, so a heartbeat costs
 * nothing past our own instance — and between beats the deck counts forward
 * from the last position it was given, so the interval is the drift a pause
 * or an untracked jump can accumulate.
 */
constexpr auto NowPlayingRefreshIntervalMs = 30000;
} // namespace Fooyin::Tapeout::Constants
