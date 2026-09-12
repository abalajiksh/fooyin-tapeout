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

//! One signal chain, for the picker. Tapedeck resolves a chain by *name*, so that is what is stored.
struct ChainInfo
{
    int id{0};
    QString name;
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
Q_DECLARE_METATYPE(Fooyin::Tapeout::ChainInfo)
Q_DECLARE_METATYPE(Fooyin::Tapeout::BindingInfo)
Q_DECLARE_METATYPE(Fooyin::Tapeout::DryRunInfo)
