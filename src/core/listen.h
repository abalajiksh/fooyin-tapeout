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

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace Fooyin {
class Track;

namespace Tapeout {
/*!
 * How the audio actually sounded, as `tapedeck_audio`.
 *
 * The delivery fields describe a *genuine* conversion only. Fooyin decoding FLAC
 * to PCM for a DAC is not one; fooyin resampling or re-quantising because the
 * output profile demanded it is. Tapedeck's quality score docks points whenever
 * a delivery field is set, so filling them on a clean passthrough quietly
 * penalises the best listens on the system.
 */
struct AudioQuality
{
    QString codec;
    QString container;
    QString formatType{QStringLiteral("pcm")};
    std::optional<int> bitrate;      // kbps
    std::optional<int> sampleRate;   // Hz
    std::optional<int> bitDepth;
    std::optional<int> channels;
    std::optional<bool> lossless;

    std::optional<int> deliverySampleRate;
    std::optional<int> deliveryBitDepth;
    std::optional<bool> transcoded;
    QString transcodeReason;

    [[nodiscard]] QJsonObject toJson() const;
};

/*!
 * Where the audio came out, as `tapedeck_device`.
 *
 * `outputDevice` is the load-bearing field: Tapedeck records every distinct
 * value it sees, surfaces unmapped ones for one-tap assignment, and then
 * resolves the binding on every later listen. Sending it is what makes the
 * signal chain follow the hardware without the user restating it.
 */
struct DeviceInfo
{
    QString machineId;
    QString outputDevice;
    QString outputType; // fooyin's backend: alsa, pipewire, wasapi, …

    [[nodiscard]] QJsonObject toJson() const;
};

//! One listen, in the shape Tapedeck's ingest handler reads.
struct Listen
{
    QString title;
    QString artist;
    QStringList artists;
    QString album;
    QString trackNumber;
    std::optional<qint64> durationMs;

    QString recordingMbid;
    QString releaseMbid;
    QString releaseGroupMbid;
    QStringList artistMbids;

    //! Unix seconds, UTC, and always the *start* of play.
    qint64 timestamp{0};
    bool skipped{false};

    //! Explicit chain name. Empty means "let Tapedeck resolve it", which is the norm.
    QString chainName;

    std::optional<AudioQuality> quality;
    std::optional<DeviceInfo> device;

    [[nodiscard]] bool isValid() const;

    /*!
     * The `track_metadata` object. `listened_at` sits beside it rather than
     * inside it, so the caller adds that — and must not for `playing_now`,
     * which Tapedeck rejects if it carries one.
     */
    [[nodiscard]] QJsonObject toJson() const;

    //! Round-trips through the on-disk queue.
    [[nodiscard]] QJsonObject serialise() const;
    static Listen deserialise(const QJsonObject& obj);
};

//! Reads everything Tapedeck can use out of a fooyin track.
Listen listenFromTrack(const Track& track);

/*!
 * MusicBrainz ids as fooyin hands them over, normalised the way Tapedeck's own
 * clients do: brace-stripped, lowercased, and rejected outright unless they are
 * a well-formed UUID.
 *
 * Returning empty for junk matters more than it looks. An empty string sent to
 * Tapedeck outranks a real id from its MBID mapping *and* satisfies every
 * `IS NOT NULL` the enrichment backfill queues on, so the row is filed as
 * identified and never looked up again. Callers must omit the key, not send "".
 */
QString normaliseMbid(const QString& value);
} // namespace Tapeout
} // namespace Fooyin
