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

#include "listen.h"

#include <core/player/playerdefs.h>
#include <core/track.h>

#include <QBasicTimer>
#include <QObject>

#include <memory>

namespace Fooyin {
class EngineController;
class NetworkAccessManager;
class PlayerController;
class PlaylistHandler;
class SettingsManager;

namespace Tapeout {
class ListenQueue;
class TapedeckClient;
struct SubmitResult;

/*!
 * Watches fooyin's player and reports to Tapedeck.
 *
 * The played threshold is fooyin's own — `PlayerController::trackPlayed` fires
 * when a track crosses it, which keeps Tapeout consistent with every other
 * scrobbling service the user has enabled rather than inventing a second policy.
 */
class TapeoutController : public QObject
{
    Q_OBJECT

public:
    TapeoutController(PlayerController* playerController, EngineController* engine,
                      PlaylistHandler* playlistHandler, std::shared_ptr<NetworkAccessManager> network,
                      SettingsManager* settings, QObject* parent = nullptr);
    ~TapeoutController() override;

    [[nodiscard]] TapedeckClient* client() const;

    //! Flushes anything held and writes the queue out.
    void shutdown();

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    void reloadSettings();
    void handleTrackChanged(const Track& track);
    void handleTrackPlayed(const Track& track);
    void handlePlayStateChanged(Player::PlayState state, Player::PlayState previous);
    void handleSubmitResult(const SubmitResult& result, const std::vector<Listen>& sent);

    [[nodiscard]] bool isEnabled() const;
    [[nodiscard]] Listen buildListen(const Track& track) const;
    [[nodiscard]] SessionInfo currentSession() const;
    /*!
     * Submits whatever is on the deck — as a listen if it crossed fooyin's played
     * threshold, as a skip otherwise — and clears it so it cannot go twice. Every
     * path that ends a play routes through here, which is the only point at which
     * fooyin has counted the whole play and not yet reset for what follows.
     */
    void finishCurrent();
    void queueListen(const Track& track, qint64 startedAt, qint64 listenedMs);
    void queueSkip(const Track& track, qint64 startedAt, qint64 listenedMs);
    void flush();
    void updateNowPlaying(const Track& track);

    PlayerController* m_playerController;
    EngineController* m_engine;
    PlaylistHandler* m_playlistHandler;
    SettingsManager* m_settings;

    std::unique_ptr<TapedeckClient> m_client;
    std::unique_ptr<ListenQueue> m_queue;

    QBasicTimer m_nowPlayingTimer;
    QBasicTimer m_retryTimer;

    //! The output fooyin is currently playing through, for `tapedeck_device`.
    QString m_outputDevice;
    QString m_outputBackend;

    Track m_currentTrack;
    //! Start of the current play, Unix seconds — what Tapedeck stores as the timestamp.
    qint64 m_currentStartedAt{0};
    //! Whether the current track crossed fooyin's played threshold.
    bool m_currentPlayed{false};
    /*!
     * Last sampled value of PlayerController::currentTimeListened(), taken once a
     * second while the track plays.
     *
     * It cannot be read when the play ends: fooyin resets the counter *before*
     * emitting currentTrackChanged, so by the time any end-of-play handler runs it
     * may already be 0. That is not hypothetical — replaying the same track
     * submitted a full listen with listened_ms of 0. Sampling forward is the only
     * way to hold a value that is still true at the end.
     */
    qint64 m_lastListenedMs{0};
    //! True while a submission is in flight, so the queue is not sent twice.
    bool m_submitting{false};
};
} // namespace Tapeout
} // namespace Fooyin
