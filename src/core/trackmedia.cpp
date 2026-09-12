/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#include "trackmedia.h"

#include <core/engine/audioloader.h>
#include <core/track.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace {
//! Well past any real lyric. A file this big is something else wearing a .lrc.
constexpr qint64 MaxLyricBytes = 256 * 1024;

/*!
 * Tapedeck's own upload ceiling. Dropping an oversized cover here rather than
 * sending it turns a 413 nobody sees into a debug line that says which album.
 */
constexpr qint64 MaxCoverBytes = 8 * 1024 * 1024;

/*!
 * In preference order. A file that carries both a timed and an untimed tag
 * fills both slots; one that carries only `LYRICS` fills whichever slot its
 * content turns out to be.
 */
const QStringList LyricTags{u"SYNCEDLYRICS"_s, u"LYRICS"_s, u"UNSYNCEDLYRICS"_s};

bool isLocalFile(const Fooyin::Track& track)
{
    const QString path = track.filepath();
    return !path.isEmpty() && !Fooyin::Track::isRemotePath(path) && !Fooyin::Track::isArchivePath(path);
}

QByteArray readCapped(const QString& path, qint64 max)
{
    QFile file{path};
    if(!file.exists() || file.size() > max || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

/*!
 * A `.lrc` beside the track.
 *
 * Skipped for a CUE or chapter track: those share one audio file, so a single
 * sidecar beside it belongs to the whole side rather than to this track, and
 * sending it would file the same words under every title on the record.
 */
QString sidecarLyrics(const Fooyin::Track& track)
{
    if(!isLocalFile(track) || track.isBoundedSegment()) {
        return {};
    }

    const QFileInfo info{track.filepath()};
    const QByteArray data = readCapped(info.dir().filePath(info.completeBaseName() + u".lrc"_s), MaxLyricBytes);
    return QString::fromUtf8(data).trimmed();
}

/*!
 * A cover sitting beside the file.
 *
 * ponytail: the conventional names only, not fooyin's own artwork search —
 * that one follows patterns configured in settings this plugin cannot see.
 * Embedded art is the common case and needs none of it; widen the list, or
 * reach for CoverProvider, if a real library turns out to be missed.
 */
QByteArray folderCover(const Fooyin::Track& track)
{
    if(!isLocalFile(track)) {
        return {};
    }

    static const QStringList names{u"cover"_s, u"folder"_s, u"front"_s, u"album"_s, u"artwork"_s};
    static const QStringList extensions{u"jpg"_s, u"jpeg"_s, u"png"_s, u"webp"_s};

    // Listed and compared rather than passed as name filters: QDir's filters are
    // only case-insensitive on Windows, and `Cover.jpg` is as common as `cover.jpg`.
    const QFileInfoList entries = QFileInfo{track.filepath()}.dir().entryInfoList(QDir::Files);
    for(const QFileInfo& entry : entries) {
        if(names.contains(entry.completeBaseName().toLower())
           && extensions.contains(entry.suffix().toLower())) {
            if(const QByteArray data = readCapped(entry.absoluteFilePath(), MaxCoverBytes); !data.isEmpty()) {
                return data;
            }
        }
    }

    return {};
}
} // namespace

namespace Fooyin::Tapeout {
bool lyricsAreTimed(const QString& text)
{
    // Anchored to the start of a line, because `[Chorus]` is a lyric in a great
    // many songs and Tapedeck's parser deliberately keeps it. An unanchored
    // match would call such a lyric timed and hand the reader cues it cannot use.
    static const QRegularExpression cue{uR"((?:\A|\n)[ \t]*\[\d{1,3}:\d{2})"_s};
    return cue.match(text).hasMatch();
}

TrackLyrics lyricsFromTrack(const Track& track)
{
    TrackLyrics lyrics;

    for(const QString& tag : LyricTags) {
        for(const QString& value : track.extraTag(tag)) {
            const QString text = value.trimmed();
            if(text.isEmpty()) {
                continue;
            }
            // First writer wins per slot, which is what makes LyricTags an order
            // rather than a list.
            QString& slot = lyricsAreTimed(text) ? lyrics.synced : lyrics.plain;
            if(slot.isEmpty()) {
                slot = text;
            }
        }
    }

    // A sidecar outranks the tag it collides with. Somebody put that file there
    // for this pressing, by hand, after the tags were written.
    if(const QString lrc = sidecarLyrics(track); !lrc.isEmpty()) {
        (lyricsAreTimed(lrc) ? lyrics.synced : lyrics.plain) = lrc;
    }

    return lyrics;
}

TrackCover coverFromTrack(const Track& track, AudioLoader& loader)
{
    QByteArray data = loader.readTrackCover(track, Track::Cover::Front);
    if(data.isEmpty()) {
        data = folderCover(track);
    }

    if(data.isEmpty() || data.size() > MaxCoverBytes) {
        return {};
    }

    // From the bytes, because an extension is a claim and Tapedeck stores the
    // file under an extension taken from the declared type. A mislabelled cover
    // would be served as something it is not.
    const QString type = QMimeDatabase{}.mimeTypeForData(data).name();
    if(!type.startsWith("image/"_L1)) {
        return {};
    }

    return {.data = data, .contentType = type};
}
} // namespace Fooyin::Tapeout
