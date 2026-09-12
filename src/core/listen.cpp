/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#include "listen.h"

#include "tapeoutconstants.h"
#include "tapeoutversion.h"

#include <core/track.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSysInfo>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {
/*!
 * Lossless-ness is a property of the codec, and fooyin does not report it, so it
 * is derived from the name. Listed rather than inferred: guessing wrong writes a
 * false claim about the listen into a permanent record.
 */
bool codecIsLossless(const QString& codec)
{
    static const QStringList lossless{u"flac"_s,   u"alac"_s,      u"wav"_s,  u"pcm"_s,
                                      u"ape"_s,    u"wavpack"_s,   u"wv"_s,   u"tta"_s,
                                      u"tak"_s,    u"shorten"_s,   u"shn"_s,  u"dsd"_s,
                                      u"dsf"_s,    u"dff"_s,       u"aiff"_s, u"mlp"_s,
                                      u"truehd"_s, u"pcm_s16le"_s, u"pcm_s24le"_s};

    const QString name = codec.trimmed().toLower();
    return std::ranges::any_of(lossless, [&name](const QString& l) { return name.contains(l); });
}

bool codecIsDsd(const QString& codec)
{
    const QString name = codec.trimmed().toLower();
    return name.contains(u"dsd"_s) || name.contains(u"dsf"_s) || name.contains(u"dff"_s);
}

void insertIfSet(QJsonObject& obj, QLatin1StringView key, const QString& value)
{
    if(!value.isEmpty()) {
        obj.insert(key, value);
    }
}

template <typename T>
void insertIfSet(QJsonObject& obj, QLatin1StringView key, const std::optional<T>& value)
{
    if(value.has_value()) {
        obj.insert(key, *value);
    }
}
} // namespace

namespace Fooyin::Tapeout {
QString firstExtraTag(const Track& track, const QString& tag)
{
    if(!track.hasExtraTag(tag)) {
        return {};
    }
    const QStringList values = track.extraTag(tag);
    return values.empty() ? QString{} : values.front();
}

QString normaliseMbid(const QString& value)
{
    const QString trimmed = value.trimmed();
    if(trimmed.isEmpty()) {
        return {};
    }

    static const QRegularExpression mbidRegex{
        uR"(^\{?([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})\}?$)"_s};

    const QRegularExpressionMatch match = mbidRegex.match(trimmed);
    return match.hasMatch() ? match.captured(1).toLower() : QString{};
}

QJsonObject AudioQuality::toJson() const
{
    QJsonObject obj;

    insertIfSet(obj, "codec"_L1, codec);
    insertIfSet(obj, "container"_L1, container);
    insertIfSet(obj, "format_type"_L1, formatType);
    insertIfSet(obj, "bitrate"_L1, bitrate);
    insertIfSet(obj, "sample_rate"_L1, sampleRate);
    insertIfSet(obj, "bit_depth"_L1, bitDepth);
    insertIfSet(obj, "channels"_L1, channels);
    insertIfSet(obj, "is_lossless"_L1, lossless);

    // Only ever present on a real conversion — see the note on AudioQuality.
    insertIfSet(obj, "delivery_sample_rate"_L1, deliverySampleRate);
    insertIfSet(obj, "delivery_bit_depth"_L1, deliveryBitDepth);
    insertIfSet(obj, "is_transcoded"_L1, transcoded);
    insertIfSet(obj, "transcode_reason"_L1, transcodeReason);

    return obj;
}

QJsonObject DeviceInfo::toJson() const
{
    QJsonObject obj;

    insertIfSet(obj, "machine_id"_L1, machineId);
    insertIfSet(obj, "output_device"_L1, outputDevice);
    insertIfSet(obj, "output_type"_L1, outputType);
    obj.insert("player_name"_L1, QCoreApplication::applicationName());
    obj.insert("player_version"_L1, QCoreApplication::applicationVersion());
    obj.insert("platform"_L1, QSysInfo::prettyProductName());

    return obj;
}

QJsonObject SessionInfo::toJson() const
{
    QJsonObject obj;

    obj.insert("is_shuffle"_L1, shuffle);
    insertIfSet(obj, "queue_source"_L1, queueSource);

    return obj;
}

QJsonObject PlaybackState::toJson() const
{
    QJsonObject obj;

    obj.insert("position_ms"_L1, positionMs);
    // Always stated. Tapedeck reads anything other than the literal "paused" as
    // playing, but leaving it out on a pause would be the one failure this
    // object exists to prevent.
    obj.insert("state"_L1, paused ? "paused"_L1 : "playing"_L1);

    return obj;
}

bool Listen::isValid() const
{
    // No minimum duration. The bundled scrobbler refuses anything under 30
    // seconds, but Tapedeck does not, and an album intro is a thing that was
    // genuinely listened to.
    return !title.isEmpty() && !artist.isEmpty();
}

QJsonObject Listen::toJson() const
{
    QJsonObject meta;
    meta.insert("track_name"_L1, title);
    meta.insert("artist_name"_L1, artist);
    insertIfSet(meta, "release_name"_L1, album);

    QJsonObject info;
    info.insert("submission_client"_L1, QLatin1StringView{Constants::SubmissionClient});
    // Tapeout's own version, not fooyin's — these are two different things and
    // ListenBrainz has a separate pair of keys for the player precisely because
    // a plugin and its host version independently.
    info.insert("submission_client_version"_L1, QLatin1StringView{TAPEOUT_VERSION});
    info.insert("media_player"_L1, QCoreApplication::applicationName());
    info.insert("media_player_version"_L1, QCoreApplication::applicationVersion());

    insertIfSet(info, "duration_ms"_L1, durationMs);
    insertIfSet(info, "tracknumber"_L1, trackNumber);

    // Every MBID is normalised on the way in, so an empty one here means "not
    // present" and the key must be omitted rather than sent blank.
    insertIfSet(info, "recording_mbid"_L1, recordingMbid);
    insertIfSet(info, "release_mbid"_L1, releaseMbid);
    insertIfSet(info, "release_group_mbid"_L1, releaseGroupMbid);
    insertIfSet(info, "release_track_mbid"_L1, releaseTrackMbid);
    insertIfSet(info, "work_mbid"_L1, workMbid);
    insertIfSet(info, "isrc"_L1, isrc);
    if(!artistMbids.empty()) {
        info.insert("artist_mbids"_L1, QJsonArray::fromStringList(artistMbids));
    }
    if(!albumArtistMbids.empty()) {
        info.insert("album_artist_mbids"_L1, QJsonArray::fromStringList(albumArtistMbids));
    }

    // Tapedeck believes this outright; a single name is not a credit list and is
    // left for its own `;` splitting to handle.
    if(artists.size() > 1) {
        info.insert("artist_names"_L1, QJsonArray::fromStringList(artists));
    }

    if(skipped) {
        info.insert("skipped"_L1, true);
    }
    insertIfSet(info, "listened_ms"_L1, listenedMs);

    if(quality) {
        info.insert("tapedeck_audio"_L1, quality->toJson());
    }
    if(device) {
        info.insert("tapedeck_device"_L1, device->toJson());
    }
    if(session) {
        info.insert("tapedeck_session"_L1, session->toJson());
    }
    if(playback) {
        info.insert("tapedeck_playback"_L1, playback->toJson());
    }
    if(!chainName.isEmpty()) {
        // Despite the key, Tapedeck resolves this by *name*. An unknown one
        // resolves to nothing rather than falling through to the next rung of
        // the ladder, so it is only ever sent when the user picked it.
        QJsonObject chain;
        chain.insert("chain_id"_L1, chainName);
        info.insert("tapedeck_chain"_L1, chain);
    }

    meta.insert("additional_info"_L1, info);
    return meta;
}

QJsonObject Listen::serialise() const
{
    QJsonObject obj;

    obj.insert("title"_L1, title);
    obj.insert("artist"_L1, artist);
    obj.insert("timestamp"_L1, timestamp);
    insertIfSet(obj, "album"_L1, album);
    insertIfSet(obj, "track_number"_L1, trackNumber);
    insertIfSet(obj, "duration_ms"_L1, durationMs);
    insertIfSet(obj, "recording_mbid"_L1, recordingMbid);
    insertIfSet(obj, "release_mbid"_L1, releaseMbid);
    insertIfSet(obj, "release_group_mbid"_L1, releaseGroupMbid);
    insertIfSet(obj, "release_track_mbid"_L1, releaseTrackMbid);
    insertIfSet(obj, "work_mbid"_L1, workMbid);
    insertIfSet(obj, "isrc"_L1, isrc);
    insertIfSet(obj, "chain_name"_L1, chainName);

    if(!artists.empty()) {
        obj.insert("artists"_L1, QJsonArray::fromStringList(artists));
    }
    if(!artistMbids.empty()) {
        obj.insert("artist_mbids"_L1, QJsonArray::fromStringList(artistMbids));
    }
    if(!albumArtistMbids.empty()) {
        obj.insert("album_artist_mbids"_L1, QJsonArray::fromStringList(albumArtistMbids));
    }
    if(skipped) {
        obj.insert("skipped"_L1, true);
    }
    insertIfSet(obj, "listened_ms"_L1, listenedMs);
    if(quality) {
        obj.insert("quality"_L1, quality->toJson());
    }
    if(device) {
        obj.insert("device"_L1, device->toJson());
    }
    if(session) {
        obj.insert("session"_L1, session->toJson());
    }

    // `playback` is deliberately absent: it describes where the playhead was at
    // the moment of submission, which means nothing for a listen replayed out of
    // the queue an hour later.

    return obj;
}

Listen Listen::deserialise(const QJsonObject& obj)
{
    Listen listen;

    listen.title            = obj.value("title"_L1).toString();
    listen.artist           = obj.value("artist"_L1).toString();
    listen.album            = obj.value("album"_L1).toString();
    listen.trackNumber      = obj.value("track_number"_L1).toString();
    listen.timestamp        = obj.value("timestamp"_L1).toInteger();
    listen.recordingMbid    = obj.value("recording_mbid"_L1).toString();
    listen.releaseMbid      = obj.value("release_mbid"_L1).toString();
    listen.releaseGroupMbid = obj.value("release_group_mbid"_L1).toString();
    listen.releaseTrackMbid = obj.value("release_track_mbid"_L1).toString();
    listen.workMbid         = obj.value("work_mbid"_L1).toString();
    listen.isrc             = obj.value("isrc"_L1).toString();
    listen.chainName        = obj.value("chain_name"_L1).toString();
    listen.skipped          = obj.value("skipped"_L1).toBool();

    if(obj.contains("duration_ms"_L1)) {
        listen.durationMs = obj.value("duration_ms"_L1).toInteger();
    }
    if(obj.contains("listened_ms"_L1)) {
        listen.listenedMs = obj.value("listened_ms"_L1).toInteger();
    }
    if(obj.contains("session"_L1)) {
        SessionInfo session;
        const QJsonObject s = obj.value("session"_L1).toObject();
        session.shuffle     = s.value("is_shuffle"_L1).toBool();
        session.queueSource = s.value("queue_source"_L1).toString();
        listen.session      = session;
    }

    const auto readStrings = [&obj](QLatin1StringView key) {
        QStringList out;
        const QJsonArray arr = obj.value(key).toArray();
        for(const auto& v : arr) {
            out.append(v.toString());
        }
        return out;
    };
    listen.artists          = readStrings("artists"_L1);
    listen.artistMbids      = readStrings("artist_mbids"_L1);
    listen.albumArtistMbids = readStrings("album_artist_mbids"_L1);

    // Quality and device are stored already in wire shape, so they are replayed
    // as-is rather than round-tripped through the structs.
    if(obj.contains("quality"_L1)) {
        AudioQuality quality;
        const QJsonObject q = obj.value("quality"_L1).toObject();
        quality.codec       = q.value("codec"_L1).toString();
        quality.container   = q.value("container"_L1).toString();
        quality.formatType  = q.value("format_type"_L1).toString();
        if(q.contains("bitrate"_L1)) {
            quality.bitrate = q.value("bitrate"_L1).toInt();
        }
        if(q.contains("sample_rate"_L1)) {
            quality.sampleRate = q.value("sample_rate"_L1).toInt();
        }
        if(q.contains("bit_depth"_L1)) {
            quality.bitDepth = q.value("bit_depth"_L1).toInt();
        }
        if(q.contains("channels"_L1)) {
            quality.channels = q.value("channels"_L1).toInt();
        }
        if(q.contains("is_lossless"_L1)) {
            quality.lossless = q.value("is_lossless"_L1).toBool();
        }
        if(q.contains("delivery_sample_rate"_L1)) {
            quality.deliverySampleRate = q.value("delivery_sample_rate"_L1).toInt();
        }
        if(q.contains("delivery_bit_depth"_L1)) {
            quality.deliveryBitDepth = q.value("delivery_bit_depth"_L1).toInt();
        }
        if(q.contains("is_transcoded"_L1)) {
            quality.transcoded = q.value("is_transcoded"_L1).toBool();
        }
        quality.transcodeReason = q.value("transcode_reason"_L1).toString();
        listen.quality          = quality;
    }

    if(obj.contains("device"_L1)) {
        DeviceInfo device;
        const QJsonObject d = obj.value("device"_L1).toObject();
        device.machineId    = d.value("machine_id"_L1).toString();
        device.outputDevice = d.value("output_device"_L1).toString();
        device.outputType   = d.value("output_type"_L1).toString();
        listen.device       = device;
    }

    return listen;
}

Listen listenFromTrack(const Track& track)
{
    Listen listen;

    listen.title   = track.title();
    listen.artist  = track.artist();
    listen.artists = track.artists();
    listen.album   = track.album();

    // Sent as the tag's own string. Tapedeck parses "7" and "7/12" alike, and an
    // unparseable one costs the track number rather than the listen.
    listen.trackNumber = track.trackNumber();

    if(track.duration() > 0) {
        listen.durationMs = static_cast<qint64>(track.duration());
    }

    // fooyin normalises tag names across Vorbis, ID3 and MP4, so these work
    // whatever the container. The built-in scrobbler reads only the first two.
    listen.recordingMbid    = normaliseMbid(firstExtraTag(track, u"MUSICBRAINZ_TRACKID"_s));
    listen.releaseMbid      = normaliseMbid(firstExtraTag(track, u"MUSICBRAINZ_ALBUMID"_s));
    listen.releaseGroupMbid = normaliseMbid(firstExtraTag(track, u"MUSICBRAINZ_RELEASEGROUPID"_s));
    listen.releaseTrackMbid = normaliseMbid(firstExtraTag(track, u"MUSICBRAINZ_RELEASETRACKID"_s));
    listen.workMbid         = normaliseMbid(firstExtraTag(track, u"MUSICBRAINZ_WORKID"_s));
    // ISRC is not a UUID, so it skips normaliseMbid and is taken as tagged.
    listen.isrc = firstExtraTag(track, u"ISRC"_s).trimmed();

    const auto readMbids = [&track](const QString& tag) {
        QStringList out;
        for(const QString& value : track.extraTag(tag)) {
            if(const QString mbid = normaliseMbid(value); !mbid.isEmpty()) {
                out.append(mbid);
            }
        }
        return out;
    };
    listen.artistMbids      = readMbids(u"MUSICBRAINZ_ARTISTID"_s);
    listen.albumArtistMbids = readMbids(u"MUSICBRAINZ_ALBUMARTISTID"_s);

    AudioQuality quality;
    // Lowercased to match `container` and every other scrobbling source. fooyin
    // reports "FLAC" where Plex and friends report "flac", and Tapedeck groups on
    // the literal string — two spellings of one codec split every stat built on it.
    quality.codec     = track.codec().trimmed().toLower();
    quality.container = QFileInfo{track.filepath()}.suffix().toLower();
    if(track.bitrate() > 0) {
        quality.bitrate = track.bitrate();
    }
    if(track.sampleRate() > 0) {
        quality.sampleRate = track.sampleRate();
    }
    if(track.bitDepth() > 0) {
        quality.bitDepth = track.bitDepth();
    }
    if(track.channels() > 0) {
        quality.channels = track.channels();
    }
    if(!quality.codec.isEmpty()) {
        quality.lossless   = codecIsLossless(quality.codec);
        quality.formatType = codecIsDsd(quality.codec) ? u"dsd"_s : u"pcm"_s;
    }
    listen.quality = quality;

    return listen;
}
} // namespace Fooyin::Tapeout
