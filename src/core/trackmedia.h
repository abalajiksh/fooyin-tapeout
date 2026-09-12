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

#include <QByteArray>
#include <QString>

namespace Fooyin {
class AudioLoader;
class Track;

namespace Tapeout {
/*!
 * The words as the file carries them.
 *
 * Both forms travel when both exist. Tapedeck prefers the timed one for the
 * reader and falls back to the plain one, so sending only what we have is
 * enough — it derives the untimed text from the cues itself rather than needing
 * us to strip them.
 */
struct TrackLyrics
{
    QString plain;
    //! LRC, `[mm:ss.xx] words` per line. This is the form that can follow the deck.
    QString synced;

    [[nodiscard]] bool isEmpty() const
    {
        return plain.isEmpty() && synced.isEmpty();
    }
};

/*!
 * Lyrics out of a track's tags and its `.lrc` sidecar, if either is there.
 *
 * Touches the disk for the sidecar, so it is called once per track rather than
 * per now-playing beat.
 */
[[nodiscard]] TrackLyrics lyricsFromTrack(const Track& track);

//! Cover bytes, exactly as they are stored — never re-encoded.
struct TrackCover
{
    QByteArray data;
    //! Sniffed from the bytes, not from a file extension.
    QString contentType;

    [[nodiscard]] bool isEmpty() const
    {
        return data.isEmpty() || contentType.isEmpty();
    }
};

/*!
 * The front cover for a track: the embedded picture where there is one, a
 * conventionally named image beside the file where there is not.
 *
 * Reads the original bytes rather than a decoded pixmap, so a JPEG arrives at
 * Tapedeck as the JPEG that is in the file — re-encoding a cover to upload it
 * would quietly cost a generation of quality for nothing.
 */
[[nodiscard]] TrackCover coverFromTrack(const Track& track, AudioLoader& loader);

/*!
 * Whether a lyric carries LRC cues.
 *
 * The one branch that decides which column the words land in, and getting it
 * wrong is not cosmetic: timed words filed as plain leave the reader unable to
 * follow the deck, and plain words filed as timed make Tapedeck parse every
 * line's leading bracket as a cue.
 */
[[nodiscard]] bool lyricsAreTimed(const QString& text);
} // namespace Tapeout
} // namespace Fooyin
