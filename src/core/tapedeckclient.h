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
#include "trackmedia.h"

#include <QBasicTimer>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QJsonDocument;
class QNetworkReply;
class QNetworkRequest;

namespace Fooyin {
class NetworkAccessManager;

namespace Tapeout {
//! Outcome of a submission, which decides whether the queue keeps the listens.
struct SubmitResult
{
    bool ok{false};
    //! True when retrying could plausibly work — network down, 5xx, timeout.
    bool retryable{false};
    int accepted{0};
    int duplicate{0};
    //! Indices Tapedeck refused. These are dropped, never retried.
    QList<int> rejectedIndices;
    QString error;
};

/*!
 * What a token turned out to be able to do.
 *
 * Everything past @a userName is only sent when the token is valid — the
 * endpoint answers whoever asks, so Tapedeck will not report its build beside
 * `valid: false`. Read these only inside a `valid` branch.
 */
struct TokenInfo
{
    bool valid{false};
    QString userName;
    QString error;

    /*!
     * As granted, with `all` already expanded. Matching is exact and **none
     * implies another** — `submit` does not carry `read`.
     */
    QStringList scopes;
    //! Empty against a Tapedeck older than 0.66.0, which did not report it.
    QString serverVersion;
    //! Whether this token's `import` submissions are relayed. Tapeout sends `single`, so it is informational.
    bool importsAreLive{false};
    //! Rung 3 of the chain ladder, when the token carries one.
    std::optional<int> defaultChainId;

    [[nodiscard]] bool hasScope(QLatin1StringView scope) const;
};

/*!
 * When a listen counts, as the server computes it.
 *
 * Not cosmetic agreement with the server: Tapedeck uses this same threshold to
 * decide whether a track a client announced as now-playing and never scrobbled
 * gets written off as a **skip**. Submitting later than the rule banks the
 * listen *and* leaves a skip behind it; submitting earlier puts a listen on a
 * permanent public record that the listener's own setting says was never heard.
 *
 * Half the track or four minutes is only the default — it is a per-user setting,
 * because that convention fits a three-minute pop song far better than a
 * forty-minute raga or a ninety-second hardcore track.
 */
struct ScrobbleRule
{
    /*!
     * False until the server has answered, and fooyin's own played threshold
     * stands in until it has.
     *
     * A guess is worse than a stand-in here: the defaults below are this
     * plugin's idea of the convention, and the whole point of the endpoint is
     * that the listener may have chosen otherwise.
     */
    bool known{false};

    //! Share of the track that must have played, 0.05–1.0, already clamped by the server.
    //! **This is the number to do arithmetic with**; `percent` is for showing a person.
    double fraction{0.5};
    //! The same share worded as a percentage. Shown rather than recomputed, so a
    //! settings screen says exactly what Tapedeck's own does.
    double percent{50.0};
    //! Seconds after which it counts regardless of the fraction.
    int afterSecs{240};
    //! The only rule left when the track's length is unknown — the fraction is unanswerable.
    int noDurationAfterSecs{240};

    //! `user` or `default` — whether the listener chose these numbers or inherited them.
    QString source;
    double defaultPercent{50.0};
    int defaultAfterSecs{240};

    /*!
     * Whether a play of @a listenedMs out of @a durationMs counts.
     *
     * **Whichever comes first.** The server states `rule: either` rather than
     * leaving it implied, because read as an AND it scrobbles a long track
     * hours late and a short one never.
     */
    [[nodiscard]] bool qualifies(qint64 durationMs, qint64 listenedMs) const;

    //! One line for a settings screen, e.g. "50% or 4:00, whichever comes first".
    [[nodiscard]] QString describe() const;
};

//! One signal chain, for the picker. Tapedeck resolves a chain by *name*, so that is what is stored.
struct ChainInfo
{
    int id{0};
    QString name;
    /*!
     * Whether this is the chain the *token* falls back to — rung 3 of the
     * ladder.
     *
     * **A chain is not default for anything by itself.** Being the default is a
     * property of the thing pointing *at* a chain, and the token is one such
     * thing. There has never been an `is_default` in either direction — the
     * schema documented one for a long time and it was fiction, corrected in
     * Tapedeck 0.115.1. Filled in from `default_chain_id` on the last token
     * validation, so it is simply absent until there has been one.
     */
    bool isDefault{false};
};

/*!
 * One output-device binding — rung 2 of Tapedeck's chain ladder.
 *
 * @a identifier is matched against `tapedeck_device.output_device`, which is
 * the value Tapeout already sends, so the two line up without translation.
 */
struct BindingInfo
{
    QString identifier;
    //! Unset for an output Tapedeck has seen but nobody has assigned a chain to.
    std::optional<int> chainId;
};

/*!
 * What Tapedeck *would* have done with a listen, from `?dry_run`.
 *
 * Every field here is invisible from an ordinary successful submit, which is
 * the point: a chain resolved from the wrong rung and one resolved from the
 * right rung produce identical answers on the way in.
 */
struct DryRunInfo
{
    bool ok{false};
    QString error;

    QString chainName;
    //! `explicit`, `output_binding`, `token_default`, `device_default` or `none`.
    QString chainSource;
    //! 0–100, computed from the `tapedeck_audio` fields. Absent when none were sent.
    std::optional<double> qualityScore;
    //! `pending` is forwarded onward; `imported` is stored only.
    QString statusIfStored;
    bool duplicate{false};
};

/*!
 * Talks to one Tapedeck instance.
 *
 * Holds no state beyond its configuration — the queue owns what is pending, so a
 * failed submission is the queue's problem to retry rather than something this
 * class remembers.
 */
class TapedeckClient : public QObject
{
    Q_OBJECT

public:
    TapedeckClient(std::shared_ptr<NetworkAccessManager> network, QObject* parent = nullptr);
    ~TapedeckClient() override;

    void setServerUrl(const QString& url);
    void setToken(const QString& token);

    [[nodiscard]] bool isConfigured() const;
    [[nodiscard]] QUrl serverUrl() const;

    /*!
     * `GET /1/validate-token`.
     *
     * An invalid token answers `200 {"valid": false}`, never 401 — a 401 would
     * send a client into a token refresh loop it cannot win. The reply is read
     * for the field, not the status.
     */
    void validateToken();

    /*!
     * `POST /1/submit-listens` with `listen_type: single`.
     *
     * Deliberately not `import`, which fooyin's built-in scrobbler sends and
     * which Tapedeck stores without forwarding unless the token carries an
     * `imports_are_live` flag the client cannot see.
     */
    void submitListens(const std::vector<Listen>& listens);

    //! `POST /1/submit-listens` with `listen_type: playing_now`, which carries no timestamp.
    void updateNowPlaying(const Listen& listen);

    //! `POST /1/playing-now/delete`, so a stopped player does not leave a ghost on the deck.
    void clearNowPlaying();

    /*!
     * `POST /api/v1/lyrics/library` — the words this file carries.
     *
     * Not `PUT /api/v1/lyrics`, which is a *correction*: it marks the row edited
     * and stops every later lookup touching it, which is a claim only a person
     * typing gets to make. These are fetched words like any other, from a source
     * that happens to be the listener's own tagging.
     *
     * Fire and forget. Nothing downstream depends on the answer and there is no
     * queue behind it — the words are still in the file, so the next play of the
     * track offers them again.
     *
     * @note Needs the `write` scope. See docs/tapedeck-media.md.
     */
    void sendLyrics(const QString& artist, const QString& title, const QString& album, const TrackLyrics& lyrics);

    /*!
     * Offer this album's cover, if Tapedeck has none.
     *
     * Asks `GET /api/v1/art/library` first and uploads only on a no. The cover is
     * a lazily-read megabyte, so @a cover is a producer rather than the bytes:
     * an album Tapedeck already has never touches the disk at all.
     *
     * @note Needs the `write` scope. See docs/tapedeck-media.md.
     */
    void offerArtwork(const QString& artist, const QString& album, const QString& releaseMbid,
                      std::function<TrackCover()> cover);

    /*!
     * `GET /api/v1/chains` — the list behind the picker.
     *
     * The reason a picker beats a text field here is that Tapedeck resolves an
     * explicit chain by name and an unknown name resolves to *nothing* rather
     * than falling through to the next rung. A typo is silent.
     *
     * @note Needs the `read` scope.
     */
    void fetchChains();

    //! `GET /api/v1/bindings` — which outputs are bound to a chain. Needs `read`.
    void fetchBindings();

    /*!
     * `GET /api/v1/scrobble-settings` — the listener's own "when it counts" rule.
     *
     * Takes a plain `submit` token rather than `read`, deliberately: it exists
     * only to decide a submission, and gating it on `read` would make every
     * scrobble client carry read access to the whole history to learn one
     * number it is obliged to obey.
     *
     * Re-read when a session starts rather than cached from setup. It is a
     * setting, changed from a settings screen at any time, so a value kept from
     * a setup handshake goes quietly stale — which is the failure the endpoint
     * exists to prevent.
     */
    void fetchScrobbleRule();

    /*!
     * `POST /1/submit-listens?dry_run=1` — resolve a listen and throw it away.
     *
     * Genuinely read-only: it looks a device up rather than upserting one,
     * because asking what would happen must not itself be a listen.
     */
    void dryRun(const Listen& listen);

    /*!
     * `POST /api/v1/loves` — love or un-love a recording.
     *
     * Loves attach to entities, never to a listen; Tapedeck resolves the names
     * itself, which is why nothing here needs an id. A recording love is
     * mirrored outward to Last.fm and ListenBrainz.
     *
     * @note Needs the `write` scope.
     */
    void setLove(const QString& artist, const QString& title, bool loved);

    /*!
     * Begin device-code pairing, and poll it through to a token.
     *
     * Unauthenticated on purpose — the client has no credential yet. The
     * vocabulary echoes RFC 8628, but this is not OAuth: no registration, no
     * client id, and what comes back is an ordinary Tapedeck token.
     *
     * Only the server address needs to be set. Emits @a pairingCode as soon as
     * there is something for the user to type, then @a pairingFinished exactly
     * once — with a token, or with the reason there is none.
     */
    void beginPairing(const QString& clientName, const QString& scopes);
    //! Stops polling. No signal follows; the caller asked for this.
    void cancelPairing();

Q_SIGNALS:
    void tokenValidated(const Fooyin::Tapeout::TokenInfo& info);
    void listensSubmitted(const Fooyin::Tapeout::SubmitResult& result, const std::vector<Fooyin::Tapeout::Listen>& sent);
    void nowPlayingUpdated(bool success);
    void chainsFetched(const QList<Fooyin::Tapeout::ChainInfo>& chains);
    void bindingsFetched(const QList<Fooyin::Tapeout::BindingInfo>& bindings);
    void dryRunFinished(const Fooyin::Tapeout::DryRunInfo& info);
    //! Only ever emitted with a rule the server actually answered with.
    void scrobbleRuleFetched(const Fooyin::Tapeout::ScrobbleRule& rule);
    //! Something for the human to type into Tapedeck, and how long they have.
    void pairingCode(const QString& userCode, int expiresInSecs);
    //! Exactly once per pairing. An empty @a token means @a error says why.
    void pairingFinished(const QString& token, const QString& userName, const QString& error);

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    [[nodiscard]] QUrl endpoint(QLatin1StringView path) const;
    /*!
     * @param authorised false for the pairing routes, which are unauthenticated
     * by design — we have no token yet, and sending an empty one would turn a
     * pending pairing into a rejected request.
     */
    [[nodiscard]] QNetworkRequest request(const QUrl& url, bool authorised = true) const;
    QNetworkReply* post(const QUrl& url, const QJsonDocument& body, bool authorised = true);
    QNetworkReply* get(const QUrl& url);
    void uploadArtwork(const QString& artist, const QString& album, const QString& releaseMbid,
                       const TrackCover& cover);
    void pollPairing();
    void endPairing(const QString& token, const QString& userName, const QString& error);

    std::shared_ptr<NetworkAccessManager> m_network;
    QUrl m_serverUrl;
    QString m_token;

    //! Last seen on a valid token, for marking that chain in the picker.
    std::optional<int> m_defaultChainId;

    QBasicTimer m_pairingTimer;
    QString m_deviceCode;
    //! Wall clock, seconds. Pairing windows are short and the server forgets first.
    qint64 m_pairingExpiresAt{0};
    int m_pairingIntervalSecs{5};
};
} // namespace Tapeout
} // namespace Fooyin

Q_DECLARE_METATYPE(Fooyin::Tapeout::TokenInfo)
Q_DECLARE_METATYPE(Fooyin::Tapeout::SubmitResult)
Q_DECLARE_METATYPE(Fooyin::Tapeout::ScrobbleRule)
Q_DECLARE_METATYPE(Fooyin::Tapeout::ChainInfo)
Q_DECLARE_METATYPE(Fooyin::Tapeout::BindingInfo)
Q_DECLARE_METATYPE(Fooyin::Tapeout::DryRunInfo)
