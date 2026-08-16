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

#include <core/engine/enginecontroller.h>
#include <core/player/playercontroller.h>
#include <utils/settings/settingsmanager.h>

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QTimerEvent>

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
} // namespace

namespace Fooyin::Tapeout {
TapeoutController::TapeoutController(PlayerController* playerController, EngineController* engine,
                                     std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                                     QObject* parent)
    : QObject{parent}
    , m_playerController{playerController}
    , m_engine{engine}
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

    QObject::connect(m_client.get(), &TapedeckClient::listensSubmitted, this, &TapeoutController::handleSubmitResult);

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

    if(isEnabled() && !m_queue->isEmpty()) {
        flush();
    }
}

Listen TapeoutController::buildListen(const Track& track) const
{
    using namespace Settings::Tapeout;

    Listen listen = listenFromTrack(track);

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
    // A track leaving before it crossed the threshold was skipped. fooyin has no
    // skip signal, so the absence of `trackPlayed` for the outgoing track is the
    // signal — and it is a real one worth keeping: Tapedeck stores skips,
    // excludes them from every count, and has had no live source for them.
    if(m_currentTrack.isValid() && !m_currentPlayed && m_settings->value<Settings::Tapeout::SendSkips>()) {
        queueSkip(m_currentTrack, m_currentStartedAt);
    }

    m_currentTrack     = track;
    m_currentStartedAt = nowSecs();
    m_currentPlayed    = false;

    if(!isEnabled() || !track.isValid()) {
        return;
    }

    updateNowPlaying(track);
}

void TapeoutController::handleTrackPlayed(const Track& track)
{
    if(!m_settings->value<Settings::Tapeout::Enabled>()) {
        return;
    }

    m_currentPlayed = true;

    Listen listen  = buildListen(track);
    // Always the *start* of play, which is what Tapedeck stores and what its
    // duplicate window is measured against.
    listen.timestamp = m_currentStartedAt > 0 ? m_currentStartedAt : nowSecs();

    if(!listen.isValid()) {
        qCDebug(TAPEOUT) << "Not submitting a track with no title or artist";
        return;
    }

    m_queue->add(listen);
    flush();
}

void TapeoutController::queueSkip(const Track& track, qint64 startedAt)
{
    if(!m_settings->value<Settings::Tapeout::Enabled>()) {
        return;
    }

    Listen listen    = buildListen(track);
    listen.timestamp = startedAt > 0 ? startedAt : nowSecs();
    listen.skipped   = true;

    if(listen.isValid()) {
        m_queue->add(listen);
    }
}

void TapeoutController::handlePlayStateChanged(Player::PlayState state, Player::PlayState previous)
{
    Q_UNUSED(previous)

    if(state == Player::PlayState::Stopped) {
        m_nowPlayingTimer.stop();

        // A skip is a skip whether the next track follows or playback simply
        // stops, so the same rule applies here.
        if(m_currentTrack.isValid() && !m_currentPlayed && m_settings->value<Settings::Tapeout::SendSkips>()) {
            queueSkip(m_currentTrack, m_currentStartedAt);
            flush();
        }

        m_currentTrack = {};
        m_currentPlayed = false;

        if(isEnabled()) {
            m_client->clearNowPlaying();
        }
        return;
    }

    if(state == Player::PlayState::Playing && isEnabled()) {
        updateNowPlaying(m_playerController->currentTrack());
    }
    else if(state == Player::PlayState::Paused) {
        // Tapedeck extrapolates a now-playing position from wall-clock, so a
        // paused track would keep advancing on the deck. Nothing in the
        // ListenBrainz format can say "paused" — this is what N1 in
        // TAPEDECK-CHANGES.md fixes.
        m_nowPlayingTimer.stop();
    }
}

void TapeoutController::updateNowPlaying(const Track& track)
{
    if(!isEnabled() || !track.isValid()) {
        return;
    }

    m_client->updateNowPlaying(buildListen(track));

    // Tapedeck expires an entry once its own arithmetic runs past the track's
    // duration, so a long track has to be re-reported to stay on the deck.
    m_nowPlayingTimer.start(NowPlayingRefreshTimer, this);
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

    if(!m_queue->isEmpty()) {
        flush(); // More than one batch was waiting.
    }
}

void TapeoutController::timerEvent(QTimerEvent* event)
{
    if(event->timerId() == m_nowPlayingTimer.timerId()) {
        if(isEnabled() && m_playerController->playState() == Player::PlayState::Playing) {
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

    // A track playing at exit was skipped as far as the threshold is concerned.
    if(m_currentTrack.isValid() && !m_currentPlayed && m_settings->value<Settings::Tapeout::SendSkips>()) {
        queueSkip(m_currentTrack, m_currentStartedAt);
    }

    // The queue is written synchronously here — its own timer will not get
    // another chance to fire.
    m_queue->write();
}
} // namespace Fooyin::Tapeout
