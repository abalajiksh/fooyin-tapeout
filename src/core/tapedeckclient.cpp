/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#include "tapedeckclient.h"

#include "tapeoutconstants.h"

#include <core/network/networkaccessmanager.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

using namespace Qt::StringLiterals;

namespace {
constexpr auto RequestTimeoutMs = 30000;

/*!
 * Whether a failed request is worth trying again.
 *
 * The distinction matters more than it looks: a retryable failure keeps the
 * listen in the queue, while a non-retryable one drops it. Getting it backwards
 * either loses listens or spins forever on a request that will never succeed.
 *
 * A 403 is deliberately *not* retryable. The token authenticated fine and simply
 * lacks a scope, so re-sending changes nothing — Tapedeck answers 403 rather
 * than 401 for exactly this reason.
 */
bool statusIsRetryable(int httpStatus, QNetworkReply::NetworkError error)
{
    if(httpStatus >= 500) {
        return true;
    }
    if(httpStatus == 429) {
        return true;
    }
    if(httpStatus > 0) {
        return false; // Any other answer from the server is a considered one.
    }

    // No HTTP status at all — the request never landed.
    switch(error) {
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::UnknownNetworkError:
        case QNetworkReply::ProxyConnectionRefusedError:
        case QNetworkReply::ProxyTimeoutError:
            return true;
        default:
            return false;
    }
}

QString errorFromBody(const QJsonObject& obj, const QString& fallback)
{
    // Tapedeck answers `{ "code": <int>, "error": "<message>" }`.
    const QString error = obj.value("error"_L1).toString();
    return error.isEmpty() ? fallback : error;
}
} // namespace

namespace Fooyin::Tapeout {
bool TokenInfo::hasScope(QLatin1StringView scope) const
{
    // Exact matching, deliberately: Tapedeck treats `rewrite` as not granting
    // `write`, and only `all` is a wildcard — which the server has already
    // expanded by the time we see this.
    return scopes.contains(QString{scope});
}

TapedeckClient::TapedeckClient(std::shared_ptr<NetworkAccessManager> network, QObject* parent)
    : QObject{parent}
    , m_network{std::move(network)}
{ }

TapedeckClient::~TapedeckClient() = default;

void TapedeckClient::setServerUrl(const QString& url)
{
    QString trimmed = url.trimmed();
    while(trimmed.endsWith(u'/')) {
        trimmed.chop(1);
    }

    if(trimmed.isEmpty()) {
        m_serverUrl = QUrl{};
        return;
    }

    // A bare host is far more likely to be typed than a full URL, and defaulting
    // to https means a plain-http instance must be spelled out rather than being
    // silently downgraded to.
    if(!trimmed.contains(u"://"_s)) {
        trimmed.prepend(u"https://"_s);
    }

    m_serverUrl = QUrl{trimmed};
}

void TapedeckClient::setToken(const QString& token)
{
    m_token = token.trimmed();
}

bool TapedeckClient::isConfigured() const
{
    return m_serverUrl.isValid() && !m_serverUrl.host().isEmpty() && !m_token.isEmpty();
}

QUrl TapedeckClient::serverUrl() const
{
    return m_serverUrl;
}

QUrl TapedeckClient::endpoint(QLatin1StringView path) const
{
    QUrl url{m_serverUrl};
    url.setPath(m_serverUrl.path() + QString{path});
    return url;
}

QNetworkReply* TapedeckClient::post(const QUrl& url, const QJsonDocument& body)
{
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(m_token).toUtf8());
    request.setTransferTimeout(RequestTimeoutMs);

    return m_network->post(request, body.toJson(QJsonDocument::Compact));
}

QNetworkReply* TapedeckClient::get(const QUrl& url)
{
    QNetworkRequest request{url};
    request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(m_token).toUtf8());
    request.setTransferTimeout(RequestTimeoutMs);

    return m_network->get(request);
}

void TapedeckClient::validateToken()
{
    if(m_serverUrl.isEmpty() || m_token.isEmpty()) {
        Q_EMIT tokenValidated({.valid = false, .userName = {}, .error = tr("Enter a server address and a token")});
        return;
    }

    QNetworkReply* reply = get(endpoint("/1/validate-token"_L1));

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();

        if(status != 200) {
            const QString fallback = reply->errorString().isEmpty()
                                       ? tr("The server answered %1").arg(status)
                                       : reply->errorString();
            Q_EMIT tokenValidated({.valid = false, .userName = {}, .error = errorFromBody(obj, fallback)});
            return;
        }

        // 200 does not mean the token is good — the field does.
        TokenInfo info;
        info.valid    = obj.value("valid"_L1).toBool();
        info.userName = obj.value("user_name"_L1).toString();

        if(!info.valid) {
            // Tapedeck sends nothing else in this case, on purpose: the endpoint
            // answers whoever asks, and reporting the build beside `valid:false`
            // would hand it to anyone guessing at tokens.
            info.error = tr("Tapedeck does not recognise this token");
            Q_EMIT tokenValidated(info);
            return;
        }

        // Absent on a server older than 0.66.0, which leaves the defaults —
        // no scopes known, no version. Both read as "cannot tell", not "no".
        const QJsonArray scopes = obj.value("scopes"_L1).toArray();
        for(const auto& scope : scopes) {
            info.scopes.append(scope.toString());
        }
        info.serverVersion  = obj.value("server_version"_L1).toString();
        info.importsAreLive = obj.value("imports_are_live"_L1).toBool();
        if(const QJsonValue chain = obj.value("default_chain_id"_L1); chain.isDouble()) {
            info.defaultChainId = chain.toInt();
        }

        Q_EMIT tokenValidated(info);
    });
}

void TapedeckClient::submitListens(const std::vector<Listen>& listens)
{
    if(listens.empty()) {
        return;
    }
    if(!isConfigured()) {
        Q_EMIT listensSubmitted({.ok = false, .retryable = true, .error = tr("Tapeout is not configured")}, listens);
        return;
    }

    QJsonArray payload;
    for(const Listen& listen : listens) {
        QJsonObject obj;
        obj.insert("listened_at"_L1, listen.timestamp);
        obj.insert("track_metadata"_L1, listen.toJson());
        payload.append(obj);
    }

    QJsonObject body;
    body.insert("listen_type"_L1, "single"_L1);
    body.insert("payload"_L1, payload);

    qCDebug(TAPEOUT) << "Submitting" << listens.size() << "listens";

    QNetworkReply* reply = post(endpoint("/1/submit-listens"_L1), QJsonDocument{body});

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, listens]() {
        reply->deleteLater();

        const int status      = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();

        SubmitResult result;

        if(status != 200) {
            result.ok        = false;
            result.retryable = statusIsRetryable(status, reply->error());
            result.error     = errorFromBody(obj, reply->errorString());
            qCWarning(TAPEOUT) << "Submission failed:" << result.error << "retryable:" << result.retryable;
            Q_EMIT listensSubmitted(result, listens);
            return;
        }

        result.ok = true;
        result.accepted = obj.value("accepted"_L1).toInt();
        // Tapedeck names this `duplicate`, singular, whatever openapi.yaml says.
        result.duplicate = obj.value("duplicate"_L1).toInt();

        // A 200 can still carry per-listen refusals. These are bad data rather
        // than bad luck, so they are dropped from the queue with the accepted
        // ones — retrying them would spin forever.
        const QJsonArray rejected = obj.value("rejected"_L1).toArray();
        for(const auto& value : rejected) {
            const QJsonObject entry = value.toObject();
            result.rejectedIndices.append(entry.value("index"_L1).toInt());
            qCWarning(TAPEOUT) << "Tapedeck rejected a listen:" << entry.value("reason"_L1).toString();
        }

        qCDebug(TAPEOUT) << "Accepted" << result.accepted << "duplicate" << result.duplicate << "rejected"
                         << result.rejectedIndices.size();

        Q_EMIT listensSubmitted(result, listens);
    });
}

void TapedeckClient::updateNowPlaying(const Listen& listen)
{
    if(!isConfigured() || !listen.isValid()) {
        return;
    }

    QJsonObject entry;
    // No `listened_at` — Tapedeck rejects a `playing_now` that carries one.
    entry.insert("track_metadata"_L1, listen.toJson());

    QJsonObject body;
    body.insert("listen_type"_L1, "playing_now"_L1);
    body.insert("payload"_L1, QJsonArray{entry});

    QNetworkReply* reply = post(endpoint("/1/submit-listens"_L1), QJsonDocument{body});

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok    = status == 200;
        if(!ok) {
            // Now-playing is ephemeral, so a failure is logged and dropped
            // rather than queued — by the time it could be retried it would be
            // describing a track that is no longer on.
            qCDebug(TAPEOUT) << "Now-playing update failed:" << reply->errorString();
        }
        Q_EMIT nowPlayingUpdated(ok);
    });
}

void TapedeckClient::clearNowPlaying()
{
    if(!isConfigured()) {
        return;
    }

    QNetworkReply* reply = post(endpoint("/1/playing-now/delete"_L1), QJsonDocument{QJsonObject{}});
    QObject::connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}
} // namespace Fooyin::Tapeout
