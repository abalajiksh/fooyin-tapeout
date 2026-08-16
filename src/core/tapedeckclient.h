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

#include <QObject>
#include <QUrl>

#include <memory>
#include <vector>

class QJsonDocument;
class QNetworkReply;

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

//! What a token turned out to be able to do.
struct TokenInfo
{
    bool valid{false};
    QString userName;
    QString error;
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

Q_SIGNALS:
    void tokenValidated(const Fooyin::Tapeout::TokenInfo& info);
    void listensSubmitted(const Fooyin::Tapeout::SubmitResult& result, const std::vector<Fooyin::Tapeout::Listen>& sent);
    void nowPlayingUpdated(bool success);

private:
    [[nodiscard]] QUrl endpoint(QLatin1StringView path) const;
    QNetworkReply* post(const QUrl& url, const QJsonDocument& body);
    QNetworkReply* get(const QUrl& url);

    std::shared_ptr<NetworkAccessManager> m_network;
    QUrl m_serverUrl;
    QString m_token;
};
} // namespace Tapeout
} // namespace Fooyin

Q_DECLARE_METATYPE(Fooyin::Tapeout::TokenInfo)
Q_DECLARE_METATYPE(Fooyin::Tapeout::SubmitResult)
