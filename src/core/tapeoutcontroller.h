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
#include <QHash>
#include <QObject>
#include <QSet>

#include <memory>

namespace Fooyin {
class AudioLoader;
class EngineController;
class MusicLibrary;
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
                      PlaylistHandler* playlistHandler, MusicLibrary* library,
                      std::shared_ptr<NetworkAccessManager> network, std::shared_ptr<AudioLoader> audioLoader,
                      SettingsManager* settings, QObject* parent = nullptr);
    ~TapeoutController() override;

    [[nodiscard]] TapedeckClient* client() const;

    /*!
     * The output fooyin is playing through, as Tapedeck knows it.
     *
     * This is the string a chain binding is keyed on, so a settings screen can
     * say whether *this* output resolves to a chain rather than only listing
     * the bindings and leaving the reader to match them up.
     */
    [[nodiscard]] QString currentOutputDevice() const;

    //! Asks Tapedeck what it would do with what is playing, storing nothing.
    void previewCurrentTrack();

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
    /*!
     * Offers Tapedeck the words and the cover this file carries.
     *
     * Once per track and once per record per run, not once per play: both are
     * properties of the file rather than of the listening, and the queue behind
     * listens deliberately has no counterpart here — a send that fails is
     * offered again the next time the track comes round.
     */
    void offerMedia(const Track& track);
    /*!
     * Pushes a love when a rating crosses the threshold, and withdraws one when
     * it falls back under.
     *
     * Only ever reacts to a rating *changing* under fooyin's hands. It does not
     * reconcile the library against Tapedeck at startup, which is the difference
     * between a feature and an accident: loves also arrive from Tapedeck's own
     * UI and from a Last.fm pull, and a bulk pass would read every one of those
     * as "not starred here" and take it away.
     */
    void handleTracksChanged(const TrackList& tracks);
    /*!
     * Holds a track's cover back until its listen has actually been stored.
     *
     * Tapedeck attaches a cover by filling `artwork_url` on the listens of that
     * record, so an upload sent while the track is still playing updates
     * nothing and leaves the file orphaned — and a record with no listens yet
     * is precisely the one whose artwork is missing.
     */
    void rememberArtwork(const Track& track);
    //! Offers the covers whose listens Tapedeck has just confirmed.
    void offerPendingArtwork(const std::vector<Listen>& stored);

    PlayerController* m_playerController;
    EngineController* m_engine;
    PlaylistHandler* m_playlistHandler;
    MusicLibrary* m_library;
    std::shared_ptr<AudioLoader> m_audioLoader;
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

    /*!
     * What has already been offered this run.
     *
     * In memory on purpose. Persisting it would save a handful of requests a
     * session against a cache that can be cleared, a file that can be retagged
     * and a cover that can be replaced — and would then be wrong about all
     * three until somebody deleted it.
     */
    QSet<QString> m_lyricsOffered;
    QSet<QString> m_artworkOffered;

    /*!
     * Last rating seen for a track, on fooyin's 0–10 half-star scale.
     *
     * Seeded silently on first sighting: the first time a track is seen is not
     * the user rating it, and treating it as one would push the whole library
     * the first time fooyin rescans.
     */
    QHash<QString, int> m_ratings;

    //! Album key to one track of it, awaiting its listen landing. See rememberArtwork.
    QHash<QString, Track> m_artworkPending;
};
} // namespace Tapeout
} // namespace Fooyin
