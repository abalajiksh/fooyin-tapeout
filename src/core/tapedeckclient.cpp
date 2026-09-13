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

#include <QCoreApplication>
#include <QDateTime>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimerEvent>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>

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
bool ScrobbleRule::qualifies(qint64 durationMs, qint64 listenedMs) const
{
    if(listenedMs <= 0) {
        return false;
    }

    // Whichever comes first, never both — see the note on the struct.
    if(listenedMs >= static_cast<qint64>(afterSecs) * 1000) {
        return true;
    }

    // No length means the fraction is unanswerable, so the flat number is the
    // only rule left. The server names it separately rather than leaving the
    // client to invent an answer for the case.
    if(durationMs <= 0) {
        return listenedMs >= static_cast<qint64>(noDurationAfterSecs) * 1000;
    }

    return static_cast<double>(listenedMs) >= static_cast<double>(durationMs) * fraction;
}

QString ScrobbleRule::describe() const
{
    const QString after = QStringLiteral("%1:%2").arg(afterSecs / 60).arg(afterSecs % 60, 2, 10, QChar{u'0'});
    // `percent` rather than fraction × 100: the server sends both, and the
    // percentage is the one worded for a person to read.
    return QCoreApplication::translate("ScrobbleRule", "%1% or %2, whichever comes first")
        .arg(percent, 0, 'g', 3)
        .arg(after);
}

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

QNetworkRequest TapedeckClient::request(const QUrl& url, bool authorised) const
{
    QNetworkRequest request{url};
    if(authorised) {
        request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(m_token).toUtf8());
    }
    request.setTransferTimeout(RequestTimeoutMs);

    return request;
}

QNetworkReply* TapedeckClient::post(const QUrl& url, const QJsonDocument& body, bool authorised)
{
    QNetworkRequest req = request(url, authorised);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);

    return m_network->post(req, body.toJson(QJsonDocument::Compact));
}

QNetworkReply* TapedeckClient::get(const QUrl& url)
{
    return m_network->get(request(url));
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
        // Kept, because it is the only place the default is ever stated — the
        // chain list carries no such flag.
        m_defaultChainId = info.defaultChainId;

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

void TapedeckClient::sendLyrics(const QString& artist, const QString& title, const QString& album,
                                const TrackLyrics& lyrics)
{
    if(!isConfigured() || artist.isEmpty() || title.isEmpty() || lyrics.isEmpty()) {
        return;
    }

    QJsonObject body;
    body.insert("artist"_L1, artist);
    body.insert("title"_L1, title);
    if(!album.isEmpty()) {
        body.insert("album"_L1, album);
    }
    if(!lyrics.plain.isEmpty()) {
        body.insert("plain"_L1, lyrics.plain);
    }
    if(!lyrics.synced.isEmpty()) {
        body.insert("synced"_L1, lyrics.synced);
    }
    // Provenance is recorded rather than assumed, which is the whole reason
    // Tapedeck keeps this column: "from your files" and "from LRCLIB" are
    // different statements about whose text the reader is looking at.
    body.insert("source"_L1, QLatin1StringView{Constants::SubmissionClient});

    QNetworkReply* reply = post(endpoint("/api/v1/lyrics/library"_L1), QJsonDocument{body});

    QObject::connect(reply, &QNetworkReply::finished, this, [reply, title]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(status != 200) {
            // Debug, not warning: a Tapedeck without the endpoint answers 404 on
            // every track, and a log line per song is worse than the missing
            // feature it is reporting.
            qCDebug(TAPEOUT) << "Lyrics for" << title << "were not stored:" << status << reply->errorString();
        }
    });
}

void TapedeckClient::offerArtwork(const QString& artist, const QString& album, const QString& releaseMbid,
                                  std::function<TrackCover()> cover)
{
    if(!isConfigured() || artist.isEmpty() || album.isEmpty() || !cover) {
        return;
    }

    QUrl url = endpoint("/api/v1/art/library"_L1);
    QUrlQuery query;
    query.addQueryItem(u"artist"_s, artist);
    query.addQueryItem(u"album"_s, album);
    url.setQuery(query);

    QNetworkReply* reply = get(url);

    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, artist, album, releaseMbid, cover = std::move(cover)]() {
                         reply->deleteLater();

                         const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                         if(status != 200) {
                             qCDebug(TAPEOUT) << "Could not ask about artwork for" << album << ":" << status;
                             return;
                         }

                         const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
                         if(obj.value("held"_L1).toBool()) {
                             return; // Tapedeck has a cover for this record already.
                         }

                         // Only now is the disk touched — see the note on offerArtwork.
                         const TrackCover image = cover();
                         if(image.isEmpty()) {
                             qCDebug(TAPEOUT) << "No usable cover to send for" << album;
                             return;
                         }

                         uploadArtwork(artist, album, releaseMbid, image);
                     });
}

void TapedeckClient::uploadArtwork(const QString& artist, const QString& album, const QString& releaseMbid,
                                   const TrackCover& cover)
{
    auto* multiPart = new QHttpMultiPart{QHttpMultiPart::FormDataType};

    const auto addText = [multiPart](QLatin1StringView name, const QString& value) {
        if(value.isEmpty()) {
            return;
        }
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral(R"(form-data; name="%1")").arg(QString{name}));
        part.setBody(value.toUtf8());
        multiPart->append(part);
    };

    addText("artist"_L1, artist);
    addText("album"_L1, album);
    // Optional, and only ever a normalised one. It is how Tapedeck tells two
    // records with the same name apart without guessing from the title.
    addText("release_mbid"_L1, releaseMbid);

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, cover.contentType);
    // Tapedeck takes the extension from the content type and never from the
    // name, so the filename here is a formality the format requires.
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader, u"form-data; name=\"file\"; filename=\"cover\""_s);
    filePart.setBody(cover.data);
    multiPart->append(filePart);

    QNetworkReply* reply = m_network->post(request(endpoint("/api/v1/art/library"_L1)), multiPart);
    multiPart->setParent(reply);

    QObject::connect(reply, &QNetworkReply::finished, this, [reply, album]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(status == 200) {
            qCDebug(TAPEOUT) << "Sent a cover for" << album;
            return;
        }
        // A warning here, unlike the check: we were told there was no cover and
        // then failed to supply one, which is a thing that went wrong rather
        // than a server that is simply older than the feature.
        qCWarning(TAPEOUT) << "Cover upload for" << album << "failed:" << status << reply->errorString();
    });
}

void TapedeckClient::fetchChains()
{
    if(!isConfigured()) {
        return;
    }

    QNetworkReply* reply = get(endpoint("/api/v1/chains"_L1));

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(status != 200) {
            // A 403 here means the token has `submit` but not `read`, which is
            // the ordinary case rather than a fault. The picker simply stays a
            // text field.
            qCDebug(TAPEOUT) << "Could not list chains:" << status;
            Q_EMIT chainsFetched({});
            return;
        }

        QList<ChainInfo> chains;
        const QJsonArray array = QJsonDocument::fromJson(reply->readAll()).object().value("chains"_L1).toArray();
        for(const auto& value : array) {
            const QJsonObject obj = value.toObject();
            const int id = obj.value("id"_L1).toInt();
            chains.append({.id        = id,
                           .name      = obj.value("name"_L1).toString(),
                           .isDefault = m_defaultChainId && *m_defaultChainId == id});
        }

        Q_EMIT chainsFetched(chains);
    });
}

void TapedeckClient::fetchBindings()
{
    if(!isConfigured()) {
        return;
    }

    QNetworkReply* reply = get(endpoint("/api/v1/bindings"_L1));

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(status != 200) {
            qCDebug(TAPEOUT) << "Could not list bindings:" << status;
            Q_EMIT bindingsFetched({});
            return;
        }

        QList<BindingInfo> bindings;
        const QJsonArray array = QJsonDocument::fromJson(reply->readAll()).object().value("bindings"_L1).toArray();
        for(const auto& value : array) {
            const QJsonObject obj = value.toObject();
            BindingInfo binding;
            // `identifier`, not `output_name` — the PUT names it the other way,
            // but a row that comes back is spelled like the column.
            binding.identifier = obj.value("identifier"_L1).toString();
            if(const QJsonValue chain = obj.value("chain_id"_L1); chain.isDouble()) {
                binding.chainId = chain.toInt();
            }
            bindings.append(binding);
        }

        Q_EMIT bindingsFetched(bindings);
    });
}

void TapedeckClient::fetchScrobbleRule()
{
    if(!isConfigured()) {
        return;
    }

    QNetworkReply* reply = get(endpoint("/api/v1/scrobble-settings"_L1));

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(status != 200) {
            // Nothing is emitted either way, so whatever rule was already in hand
            // keeps standing. A 500 explicitly does *not* mean "they have not set
            // one" — the default is an answer and is withheld after a failed look
            // — so inventing 50/240 here would have us follow a rule the server
            // is not applying, and report it to the user as theirs.
            if(status == 404) {
                qCDebug(TAPEOUT) << "This Tapedeck has no scrobble-settings endpoint; using fooyin's threshold";
            }
            else {
                qCWarning(TAPEOUT) << "Could not read the scrobble rule:" << status
                                   << "- keeping the rule already in force";
            }
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();

        ScrobbleRule rule;
        rule.known    = true;
        rule.fraction = obj.value("fraction"_L1).toDouble(rule.fraction);
        rule.percent  = obj.value("percent"_L1).toDouble(rule.fraction * 100.0);
        rule.afterSecs = obj.value("after_secs"_L1).toInt(rule.afterSecs);
        // Equal to after_secs today, but read rather than assumed: it is named
        // separately precisely so a client does not decide that for itself.
        rule.noDurationAfterSecs = obj.value("no_duration_after_secs"_L1).toInt(rule.afterSecs);
        rule.source              = obj.value("source"_L1).toString();
        rule.defaultPercent      = obj.value("default_percent"_L1).toDouble(rule.defaultPercent);
        rule.defaultAfterSecs    = obj.value("default_after_secs"_L1).toInt(rule.defaultAfterSecs);

        // Always `either` today, and qualifies() implements exactly that. Read
        // and checked rather than ignored: if this ever gains another value the
        // failure is silent double-counting, and a line in the log is the only
        // warning anyone would get.
        if(const QString kind = obj.value("rule"_L1).toString(); !kind.isEmpty() && kind != "either"_L1) {
            qCWarning(TAPEOUT) << "Tapedeck reports an unfamiliar scrobble rule" << kind
                               << "- still reading it as whichever-comes-first";
        }

        // The server clamps before answering, so a sane value here is the one it
        // actually uses. This guards only against a reply that omitted the field.
        if(rule.fraction <= 0.0 || rule.fraction > 1.0) {
            qCWarning(TAPEOUT) << "Ignoring an out-of-range scrobble fraction:" << rule.fraction;
            return;
        }

        qCDebug(TAPEOUT) << "Scrobble rule:" << rule.describe() << "(" << rule.source << ")";
        Q_EMIT scrobbleRuleFetched(rule);
    });
}

void TapedeckClient::dryRun(const Listen& listen)
{
    if(!isConfigured() || !listen.isValid()) {
        Q_EMIT dryRunFinished({.ok = false, .error = tr("Nothing is playing")});
        return;
    }

    QJsonObject entry;
    entry.insert("listened_at"_L1, listen.timestamp);
    entry.insert("track_metadata"_L1, listen.toJson());

    QJsonObject body;
    body.insert("listen_type"_L1, "single"_L1);
    body.insert("payload"_L1, QJsonArray{entry});

    QUrl url = endpoint("/1/submit-listens"_L1);
    url.setQuery(u"dry_run=1"_s);

    QNetworkReply* reply = post(url, QJsonDocument{body});

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status      = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();

        if(status != 200) {
            Q_EMIT dryRunFinished({.ok = false, .error = errorFromBody(obj, reply->errorString())});
            return;
        }

        const QJsonArray listens = obj.value("listens"_L1).toArray();
        if(listens.isEmpty()) {
            // A listen Tapedeck would refuse outright lands in `rejected` rather
            // than `listens`, and the reason there is the useful half.
            const QJsonArray rejected = obj.value("rejected"_L1).toArray();
            const QString reason      = rejected.isEmpty() ? tr("Tapedeck resolved nothing")
                                                           : rejected.first().toObject().value("reason"_L1).toString();
            Q_EMIT dryRunFinished({.ok = false, .error = reason});
            return;
        }

        const QJsonObject first = listens.first().toObject();

        DryRunInfo info;
        info.ok             = true;
        info.chainName      = first.value("chain_name"_L1).toString();
        info.chainSource    = first.value("chain_source"_L1).toString();
        info.statusIfStored = first.value("status_if_stored"_L1).toString();
        info.duplicate      = first.value("duplicate"_L1).toBool();
        if(const QJsonValue score = first.value("quality_score"_L1); score.isDouble()) {
            info.qualityScore = score.toDouble();
        }

        Q_EMIT dryRunFinished(info);
    });
}

void TapedeckClient::setLove(const QString& artist, const QString& title, bool loved)
{
    if(!isConfigured() || artist.isEmpty() || title.isEmpty()) {
        return;
    }

    QJsonObject body;
    // Always a recording. A love of an album or an artist is a different
    // gesture, and a star on one track is not a statement about either.
    body.insert("kind"_L1, "recording"_L1);
    body.insert("name"_L1, title);
    body.insert("artist"_L1, artist);
    body.insert("loved"_L1, loved);

    QNetworkReply* reply = post(endpoint("/api/v1/loves"_L1), QJsonDocument{body});

    QObject::connect(reply, &QNetworkReply::finished, this, [reply, title, loved]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(status == 200) {
            qCDebug(TAPEOUT) << (loved ? "Loved" : "Un-loved") << title;
            return;
        }
        qCWarning(TAPEOUT) << "Could not set love on" << title << ":" << status << reply->errorString();
    });
}

void TapedeckClient::beginPairing(const QString& clientName, const QString& scopes)
{
    cancelPairing();

    // Deliberately not isConfigured(): pairing is how the token is obtained, so
    // requiring one would be a loop with no way in.
    if(!m_serverUrl.isValid() || m_serverUrl.host().isEmpty()) {
        Q_EMIT pairingFinished({}, {}, tr("Enter a server address first"));
        return;
    }

    QJsonObject body;
    body.insert("client_name"_L1, clientName);
    body.insert("scopes"_L1, scopes);

    QNetworkReply* reply = post(endpoint("/1/pair/start"_L1), QJsonDocument{body}, false);

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status      = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();

        if(status == 429) {
            Q_EMIT pairingFinished({}, {}, tr("Tapedeck is rate limiting pairing. Try again in a minute."));
            return;
        }
        if(status != 200) {
            const QString fallback = status > 0 ? tr("The server answered %1").arg(status) : reply->errorString();
            Q_EMIT pairingFinished({}, {}, errorFromBody(obj, fallback));
            return;
        }

        m_deviceCode = obj.value("device_code"_L1).toString();
        const QString userCode = obj.value("user_code"_L1).toString();
        if(m_deviceCode.isEmpty() || userCode.isEmpty()) {
            Q_EMIT pairingFinished({}, {}, tr("Tapedeck did not return a pairing code"));
            return;
        }

        // Both defaulted rather than assumed: an older build that omits them
        // should still pair, just on our own conservative schedule.
        const int expiresIn  = obj.value("expires_in"_L1).toInt(300);
        m_pairingIntervalSecs = std::max(1, obj.value("interval"_L1).toInt(5));
        m_pairingExpiresAt    = QDateTime::currentSecsSinceEpoch() + expiresIn;

        Q_EMIT pairingCode(userCode, expiresIn);
        m_pairingTimer.start(std::chrono::seconds{m_pairingIntervalSecs}, this);
    });
}

void TapedeckClient::cancelPairing()
{
    m_pairingTimer.stop();
    m_deviceCode.clear();
    m_pairingExpiresAt = 0;
}

void TapedeckClient::endPairing(const QString& token, const QString& userName, const QString& error)
{
    cancelPairing();
    Q_EMIT pairingFinished(token, userName, error);
}

void TapedeckClient::pollPairing()
{
    if(m_deviceCode.isEmpty()) {
        return;
    }
    if(QDateTime::currentSecsSinceEpoch() > m_pairingExpiresAt) {
        // Our own clock, because an expired code is answered exactly like an
        // unknown one — the server will not tell us which this was.
        endPairing({}, {}, tr("The pairing code expired. Start again."));
        return;
    }

    QJsonObject body;
    body.insert("device_code"_L1, m_deviceCode);

    QNetworkReply* reply = post(endpoint("/1/pair/poll"_L1), QJsonDocument{body}, false);

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status      = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();

        if(status == 200) {
            const QString token = obj.value("token"_L1).toString();
            if(token.isEmpty()) {
                endPairing({}, {}, tr("Tapedeck approved the pairing but sent no token"));
                return;
            }
            // The token is in this body and nowhere else — it is deleted from
            // the request the moment it is collected, so a dropped reply here
            // means starting over.
            endPairing(token, obj.value("user_name"_L1).toString(), {});
            return;
        }

        if(status == 429) {
            // Being asked to wait is not a failure. Back off and keep the
            // pairing alive; the deadline above is what ends it.
            m_pairingIntervalSecs = std::min(m_pairingIntervalSecs * 2, 30);
            m_pairingTimer.start(std::chrono::seconds{m_pairingIntervalSecs}, this);
            return;
        }

        const QString error = obj.value("error"_L1).toString();
        if(error == "authorization_pending"_L1) {
            return; // Nobody has approved it yet. Keep waiting.
        }
        if(error == "expired_token"_L1) {
            endPairing({}, {}, tr("The pairing code expired. Start again."));
            return;
        }

        endPairing({}, {}, error.isEmpty() ? reply->errorString() : error);
    });
}

void TapedeckClient::timerEvent(QTimerEvent* event)
{
    if(event->timerId() == m_pairingTimer.timerId()) {
        pollPairing();
        return;
    }

    QObject::timerEvent(event);
}
} // namespace Fooyin::Tapeout
