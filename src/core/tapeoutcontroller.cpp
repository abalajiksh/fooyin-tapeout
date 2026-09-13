/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#include "tapeoutcontroller.h"

#include "listenqueue.h"
#include "tapedeckclient.h"
#include "tapeoutconstants.h"
#include "tapeoutsettings.h"
#include "trackmedia.h"

#include <core/engine/audioloader.h>
#include <core/engine/enginecontroller.h>
#include <core/library/musiclibrary.h>
#include <core/player/playercontroller.h>
#include <core/playlist/playlisthandler.h>
#include <utils/settings/settingsmanager.h>

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QTimerEvent>

#include <algorithm>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {
//! How long to wait before retrying a submission that failed for a retryable reason.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
constexpr auto RetryInterval = 5min;
constexpr auto NowPlayingRefreshTimer
    = std::chrono::milliseconds{Fooyin::Tapeout::Constants::NowPlayingRefreshIntervalMs};
#else
constexpr auto RetryInterval          = 300000;
constexpr auto NowPlayingRefreshTimer = Fooyin::Tapeout::Constants::NowPlayingRefreshIntervalMs;
#endif

QString queueFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir{dir}.filePath(u"tapeout/queue.json"_s);
}

qint64 nowSecs()
{
    return QDateTime::currentSecsSinceEpoch();
}

/*!
 * Keys the "already offered" sets.
 *
 * Case-folded and separated by a unit separator, which cannot appear in either
 * half — joining on a character that can would make "A/B" + "C" collide with
 * "A" + "B/C".
 */
QString mediaKey(const QString& first, const QString& second)
{
    return first.toCaseFolded() + u'' + second.toCaseFolded();
}
} // namespace

namespace Fooyin::Tapeout {
TapeoutController::TapeoutController(PlayerController* playerController, EngineController* engine,
                                     PlaylistHandler* playlistHandler, MusicLibrary* library,
                                     std::shared_ptr<NetworkAccessManager> network,
                                     std::shared_ptr<AudioLoader> audioLoader, SettingsManager* settings,
                                     QObject* parent)
    : QObject{parent}
    , m_playerController{playerController}
    , m_engine{engine}
    , m_playlistHandler{playlistHandler}
    , m_library{library}
    , m_audioLoader{std::move(audioLoader)}
    , m_settings{settings}
    , m_client{std::make_unique<TapedeckClient>(std::move(network))}
    , m_queue{std::make_unique<ListenQueue>(queueFilePath())}
{
    reloadSettings();

    using namespace Settings::Tapeout;
    m_settings->subscribe<ServerUrl>(this, &TapeoutController::reloadSettings);
    m_settings->subscribe<Token>(this, &TapeoutController::reloadSettings);
    m_settings->subscribe<Enabled>(this, &TapeoutController::reloadSettings);

    QObject::connect(m_playerController, &PlayerController::currentTrackChanged, this,
                     &TapeoutController::handleTrackChanged);
    QObject::connect(m_playerController, &PlayerController::trackPlayed, this, &TapeoutController::handleTrackPlayed);
    QObject::connect(m_playerController, &PlayerController::playStateChanged, this,
                     &TapeoutController::handlePlayStateChanged);

    // The output device is what Tapedeck hangs its signal-chain bindings on, so
    // it is tracked continuously rather than read at submission time — by then
    // the engine may already have moved on to the next track's output.
    QObject::connect(m_engine, &EngineController::outputChanged, this,
                     [this](const QString& output, const QString& device) {
                         m_outputBackend = output;
                         m_outputDevice  = device;
                     });
    QObject::connect(m_engine, &EngineController::deviceChanged, this,
                     [this](const QString& device) { m_outputDevice = device; });

    // Sampled at 1 Hz rather than read at the end, because fooyin resets the
    // counter before announcing the change. Paused time is already excluded by
    // fooyin, and a pause simply stops the samples, so the last one stands.
    QObject::connect(m_playerController, &PlayerController::positionChangedSeconds, this, [this](uint64_t /*secs*/) {
        const auto listened = static_cast<qint64>(m_playerController->currentTimeListened());
        // Monotonic within a play: a reset means the next play has already begun,
        // and that is the successor's tally, not this one's.
        if(listened > m_lastListenedMs) {
            m_lastListenedMs = listened;
        }
    });

    // A seek is the one thing a heartbeat cannot cover: between beats Tapedeck
    // counts forward from the last position it was told, which is right until
    // the listener jumps. Reporting immediately is cheap and Tapedeck believes a
    // backward step from a pushing client outright, so nothing needs debouncing.
    QObject::connect(m_playerController, &PlayerController::positionMoved, this,
                     [this](uint64_t /*ms*/) { updateNowPlaying(m_playerController->currentTrack()); });

    QObject::connect(m_client.get(), &TapedeckClient::listensSubmitted, this, &TapeoutController::handleSubmitResult);
    QObject::connect(m_client.get(), &TapedeckClient::scrobbleRuleFetched, this,
                     [this](const ScrobbleRule& rule) { m_rule = rule; });

    // Both, because fooyin routes a rating written to the file and a rating held
    // in the library through different signals, and a love should not depend on
    // which one the user's tag-writing settings produce.
    if(m_library) {
        QObject::connect(m_library, &MusicLibrary::tracksMetadataChanged, this,
                         &TapeoutController::handleTracksChanged);
        QObject::connect(m_library, &MusicLibrary::tracksUpdated, this, &TapeoutController::handleTracksChanged);
    }

    // Anything left from the last session goes out as soon as we are configured.
    if(isEnabled() && !m_queue->isEmpty()) {
        flush();
    }
}

TapeoutController::~TapeoutController() = default;

TapedeckClient* TapeoutController::client() const
{
    return m_client.get();
}

bool TapeoutController::isEnabled() const
{
    return m_settings->value<Settings::Tapeout::Enabled>() && m_client->isConfigured();
}

void TapeoutController::reloadSettings()
{
    using namespace Settings::Tapeout;

    m_client->setServerUrl(m_settings->value<ServerUrl>());
    m_client->setToken(m_settings->value<Token>());

    if(isEnabled()) {
        // Re-read rather than cached from setup: it is a setting, changed from a
        // settings screen at any time, and a value kept from a handshake goes
        // quietly stale — which is the failure the endpoint exists to prevent.
        // ponytail: startup and reconfiguration only. Add a periodic re-read if
        // a fooyin left running for days is seen following a superseded rule.
        m_client->fetchScrobbleRule();

        if(!m_queue->isEmpty()) {
            flush();
        }
    }
}

SessionInfo TapeoutController::currentSession() const
{
    SessionInfo session;

    const Playlist::PlayModes mode = m_playerController->playMode();
    session.shuffle = mode.testFlag(Playlist::ShuffleTracks) || mode.testFlag(Playlist::ShuffleAlbums);

    // Free text, and Tapedeck says so — this is our vocabulary for our own
    // containers, with nothing shared to normalise against. The playlist the
    // track is actually playing from is the honest answer.
    const PlaylistTrack plTrack = m_playerController->currentPlaylistTrack();
    if(plTrack.isInPlaylist()) {
        if(const Playlist* playlist = m_playlistHandler->playlistById(plTrack.playlistId)) {
            session.queueSource = playlist->name();
        }
    }

    return session;
}

Listen TapeoutController::buildListen(const Track& track) const
{
    using namespace Settings::Tapeout;

    Listen listen = listenFromTrack(track);
    listen.session = currentSession();

    if(!m_settings->value<SendQuality>()) {
        listen.quality.reset();
    }

    if(m_settings->value<SendDevice>()) {
        DeviceInfo device;
        device.machineId    = m_settings->value<MachineId>();
        device.outputDevice = m_outputDevice;
        device.outputType   = m_outputBackend;
        listen.device       = device;
    }

    // Left empty unless the user picked one. Tapedeck resolves an unknown chain
    // name to nothing at all rather than falling through to the output binding,
    // so a guess here is worse than saying nothing.
    listen.chainName = m_settings->value<ChainName>();

    return listen;
}

void TapeoutController::handleTrackChanged(const Track& track)
{
    // The outgoing track ends here, played or skipped.
    finishCurrent();

    m_currentTrack     = track;
    m_currentStartedAt = nowSecs();
    m_currentPlayed    = false;
    // Same title arriving again is a *new* play — repeat-one, or the track queued
    // twice — so the tally starts from zero rather than carrying over.
    m_lastListenedMs   = 0;

    if(!isEnabled() || !track.isValid()) {
        return;
    }

    updateNowPlaying(track);
    offerMedia(track);
}

void TapeoutController::offerMedia(const Track& track)
{
    using namespace Settings::Tapeout;

    const QString artist = track.artist();
    if(artist.isEmpty()) {
        return;
    }

    if(m_settings->value<SendLyrics>() && !track.title().isEmpty()) {
        // Marked before the request, not after. The set exists to stop a
        // repeat-one play asking twice, and a send that never comes back is
        // exactly the case where asking again would not help.
        if(const QString key = mediaKey(artist, track.title()); !m_lyricsOffered.contains(key)) {
            m_lyricsOffered.insert(key);
            if(const TrackLyrics lyrics = lyricsFromTrack(track); !lyrics.isEmpty()) {
                m_client->sendLyrics(artist, track.title(), track.album(), lyrics);
            }
        }
    }

    // Artwork is deliberately *not* offered here. Tapedeck attaches a cover by
    // filling `artwork_url` on the listens of that record, so an upload sent
    // before the listen exists updates nothing and the file is stored orphaned
    // — and a record with no listens yet is exactly the one whose artwork is
    // missing. It is held until the listen has actually landed; see
    // rememberArtwork and handleSubmitResult.
}

void TapeoutController::rememberArtwork(const Track& track)
{
    using namespace Settings::Tapeout;

    if(!m_settings->value<SendArtwork>() || track.artist().isEmpty() || track.album().isEmpty()
       || !m_audioLoader) {
        return;
    }

    // The *track* artist, not the album artist, because Tapedeck identifies a
    // record by the pair its listens carry and a listen carries the track
    // artist. On a compilation that means offering the sleeve once per
    // contributor — which is what Tapedeck's own album keying asks for, and the
    // repeats cost a question each rather than an upload.
    const QString key = mediaKey(track.artist(), track.album());
    if(!m_artworkOffered.contains(key)) {
        m_artworkPending.insert(key, track);
    }
}

void TapeoutController::offerPendingArtwork(const std::vector<Listen>& stored)
{
    for(const Listen& listen : stored) {
        // A skip stores no listen row for the cover to attach to, so there is
        // nothing for an upload to cover.
        if(listen.skipped || listen.album.isEmpty()) {
            continue;
        }

        const QString key = mediaKey(listen.artist, listen.album);
        const auto pending = m_artworkPending.constFind(key);
        if(pending == m_artworkPending.constEnd() || m_artworkOffered.contains(key)) {
            continue;
        }

        const Track track = *pending;
        m_artworkPending.erase(pending);
        m_artworkOffered.insert(key);

        m_client->offerArtwork(listen.artist, listen.album,
                               normaliseMbid(firstExtraTag(track, u"MUSICBRAINZ_ALBUMID"_s)),
                               [track, loader = m_audioLoader] { return coverFromTrack(track, *loader); });
    }
}

void TapeoutController::handleTrackPlayed(const Track& track)
{
    Q_UNUSED(track)

    // Only records that the threshold was crossed. Submitting from here would
    // freeze `listened_ms` at the threshold itself — fooyin emits this the moment
    // the track qualifies, not when it ends, so a track played to the last second
    // would still report whatever the threshold happened to be.
    m_currentPlayed = true;
}

void TapeoutController::finishCurrent()
{
    if(!m_currentTrack.isValid() || !m_settings->value<Settings::Tapeout::Enabled>()) {
        m_currentTrack   = {};
        m_currentPlayed  = false;
        m_lastListenedMs = 0;
        return;
    }

    // The sampled value, not a fresh read — see m_lastListenedMs. The live counter
    // is still consulted in case this play ended between two samples, but it is
    // only ever an improvement, never a replacement.
    const auto listenedMs = std::max(m_lastListenedMs, static_cast<qint64>(m_playerController->currentTimeListened()));

    // Tapedeck's rule wins wherever it is known, because Tapedeck applies the
    // same threshold in reverse: a track announced as now-playing and never
    // scrobbled is written off as a skip. Disagreeing in one direction banks the
    // listen *and* leaves a skip behind it; disagreeing in the other puts a
    // listen on a permanent public record the listener's own setting says was
    // never heard. fooyin's own threshold stands in only until the server has
    // answered — never as a second opinion beside it.
    const bool qualified = m_rule.known
                             ? m_rule.qualifies(static_cast<qint64>(m_currentTrack.duration()), listenedMs)
                             : m_currentPlayed;

    if(qualified) {
        queueListen(m_currentTrack, m_currentStartedAt, listenedMs);
    }
    // A track leaving before it crossed the threshold was skipped. fooyin has no
    // skip signal, so falling short of the rule is the signal — and it is a
    // real one worth keeping: Tapedeck stores skips, excludes them from every
    // count, and has had no live source for them.
    else if(m_settings->value<Settings::Tapeout::SendSkips>()) {
        queueSkip(m_currentTrack, m_currentStartedAt, listenedMs);
    }

    m_currentTrack   = {};
    m_currentPlayed  = false;
    m_lastListenedMs = 0;

    flush();
}

void TapeoutController::queueListen(const Track& track, qint64 startedAt, qint64 listenedMs)
{
    Listen listen = buildListen(track);
    // Always the *start* of play, which is what Tapedeck stores and what its
    // duplicate window is measured against.
    listen.timestamp  = startedAt > 0 ? startedAt : nowSecs();
    listen.listenedMs = listenedMs;

    if(!listen.isValid()) {
        qCDebug(TAPEOUT) << "Not submitting a track with no title or artist";
        return;
    }

    // Held, not sent: the cover can only attach to a listen that exists.
    rememberArtwork(track);
    m_queue->add(listen);
}

void TapeoutController::queueSkip(const Track& track, qint64 startedAt, qint64 listenedMs)
{
    Listen listen    = buildListen(track);
    listen.timestamp = startedAt > 0 ? startedAt : nowSecs();
    listen.skipped   = true;
    // The whole point of pairing this with `skipped`: how far in the listener
    // gave up is the difference between disliking a track and mis-clicking.
    listen.listenedMs = listenedMs;

    if(listen.isValid()) {
        m_queue->add(listen);
    }
}

void TapeoutController::handlePlayStateChanged(Player::PlayState state, Player::PlayState previous)
{
    Q_UNUSED(previous)

    if(state == Player::PlayState::Stopped) {
        m_nowPlayingTimer.stop();

        // Playback ending is as much an end of play as the next track starting,
        // and the last track of a queue only ever ends this way — before, it was
        // submitted at the threshold and this path only had skips to worry about.
        finishCurrent();

        if(isEnabled()) {
            m_client->clearNowPlaying();
        }
        return;
    }

    // Playing and paused are the same call. `tapedeck_playback.state` carries
    // the difference, and the heartbeat keeps running through a pause on
    // purpose: a paused entry holds its position and expires only after ten
    // minutes of silence, so stopping the beat would drop the track off the deck
    // rather than showing it paused.
    if(isEnabled()) {
        updateNowPlaying(m_playerController->currentTrack());
    }
}

void TapeoutController::updateNowPlaying(const Track& track)
{
    if(!isEnabled() || !track.isValid()) {
        return;
    }

    Listen listen = buildListen(track);

    // The playhead, measured now. This is what makes Tapedeck treat the position
    // as `Exact` rather than counting forward from when we last spoke — and the
    // paused flag is what stops that becoming a confident lie the moment the
    // listener hits pause.
    PlaybackState playback;
    playback.positionMs = static_cast<qint64>(m_playerController->currentPosition());
    playback.paused     = m_playerController->playState() == Player::PlayState::Paused;
    listen.playback     = playback;

    m_client->updateNowPlaying(listen);

    // Heartbeating is safe: Tapedeck forwards a now-playing to Last.fm and
    // ListenBrainz only on a real change of track, so repeats cost nothing
    // beyond our own instance.
    m_nowPlayingTimer.start(NowPlayingRefreshTimer, this);
}

void TapeoutController::handleTracksChanged(const TrackList& tracks)
{
    using namespace Settings::Tapeout;

    if(!isEnabled() || !m_settings->value<SendLoves>()) {
        // The ratings map is still not updated here, deliberately. Turning the
        // setting on mid-session then seeds from whatever is current, so
        // enabling it does not replay every edit made while it was off.
        return;
    }

    // fooyin's internal scale is 0–10 half-stars; the setting is whole stars.
    const int threshold = std::clamp(m_settings->value<LoveThreshold>(), 1, 5) * 2;

    for(const Track& track : tracks) {
        if(!track.isValid() || track.title().isEmpty() || track.artist().isEmpty()) {
            continue;
        }

        const QString key   = track.uniqueFilepath();
        const int rating    = track.ratingStars();
        const auto previous = m_ratings.constFind(key);

        // First sighting is not an edit — see m_ratings.
        if(previous == m_ratings.constEnd()) {
            m_ratings.insert(key, rating);
            continue;
        }
        if(*previous == rating) {
            continue;
        }

        const bool wasLoved = *previous >= threshold;
        const bool isLoved  = rating >= threshold;
        m_ratings.insert(key, rating);

        // Only a crossing is news. Going from three stars to four is a rating
        // change and not a love, and sending one would overwrite a love the
        // user set in Tapedeck itself.
        if(wasLoved != isLoved) {
            m_client->setLove(track.artist(), track.title(), isLoved);
        }
    }
}

ScrobbleRule TapeoutController::scrobbleRule() const
{
    return m_rule;
}

QString TapeoutController::currentOutputDevice() const
{
    return m_outputDevice;
}

void TapeoutController::previewCurrentTrack()
{
    const Track track = m_playerController->currentTrack();
    if(!track.isValid()) {
        m_client->dryRun({});
        return;
    }

    Listen listen = buildListen(track);
    // A real timestamp, because the dry run reports whether dedup *would* have
    // swallowed this — and against a made-up time that answer means nothing.
    listen.timestamp  = m_currentStartedAt > 0 ? m_currentStartedAt : nowSecs();
    listen.listenedMs = std::max(m_lastListenedMs, static_cast<qint64>(m_playerController->currentTimeListened()));

    m_client->dryRun(listen);
}

void TapeoutController::flush()
{
    if(m_submitting || !isEnabled() || m_queue->isEmpty()) {
        return;
    }

    const std::vector<Listen> batch = m_queue->take(Constants::MaxListensPerRequest);
    if(batch.empty()) {
        return;
    }

    m_submitting = true;
    m_client->submitListens(batch);
}

void TapeoutController::handleSubmitResult(const SubmitResult& result, const std::vector<Listen>& sent)
{
    m_submitting = false;

    if(!result.ok) {
        if(result.retryable) {
            // Kept in the queue. Retrying is safe: Tapedeck dedups on its own
            // source id and again on a fuzzy title/timestamp window, so a listen
            // that did land is recognised rather than duplicated.
            qCInfo(TAPEOUT) << "Keeping" << sent.size() << "listens queued for retry:" << result.error;
            if(!m_retryTimer.isActive()) {
                m_retryTimer.start(RetryInterval, this);
            }
        }
        else {
            // Nothing about re-sending would change the answer.
            qCWarning(TAPEOUT) << "Dropping" << sent.size() << "listens Tapedeck refused:" << result.error;
            m_queue->remove(sent);
        }
        return;
    }

    // Accepted and duplicate both mean Tapedeck has it. Rejected listens are bad
    // data and would be refused identically forever, so they go too.
    m_queue->remove(sent);

    // Only now are there rows for a cover to attach to.
    offerPendingArtwork(sent);

    if(!m_queue->isEmpty()) {
        flush(); // More than one batch was waiting.
    }
}

void TapeoutController::timerEvent(QTimerEvent* event)
{
    if(event->timerId() == m_nowPlayingTimer.timerId()) {
        // Paused counts as still on the deck — the beat is what keeps a paused
        // track from expiring, and it is how Tapedeck learns the pause is still
        // in effect. Only a stop ends it.
        const bool onDeck = m_playerController->playState() != Player::PlayState::Stopped;
        if(isEnabled() && onDeck) {
            updateNowPlaying(m_playerController->currentTrack());
        }
        else {
            m_nowPlayingTimer.stop();
        }
        return;
    }

    if(event->timerId() == m_retryTimer.timerId()) {
        m_retryTimer.stop();
        flush();
        return;
    }

    QObject::timerEvent(event);
}

void TapeoutController::shutdown()
{
    m_nowPlayingTimer.stop();
    m_retryTimer.stop();

    // Exiting mid-track ends the play like anything else: a listen if it crossed
    // the threshold, a skip if not. This has to happen before the queue is
    // written — since submission moved to track-end, a track that qualified but
    // was still playing has not been sent anywhere yet, and a clean exit is the
    // last chance to keep it.
    finishCurrent();

    // The queue is written synchronously here — its own timer will not get
    // another chance to fire.
    m_queue->write();
}
} // namespace Fooyin::Tapeout
