/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * The offline queue persists listens through serialise/deserialise and the wire
 * format is built by a *separate* function. A field added to one and forgotten in
 * the other loses data silently — the queue replays a listen with the field
 * missing and nothing anywhere reports it. That is what this guards.
 */

#include "core/listen.h"
#include "core/tapedeckclient.h"
#include "core/trackmedia.h"

#include <QJsonObject>
#include <QLoggingCategory>

#include <cassert>
#include <cstdio>

// Defined in tapeoutplugin.cpp for the real build, which this binary does not
// link — the client is here only for ScrobbleRule, not for its networking.
Q_LOGGING_CATEGORY(TAPEOUT, "fy.tapeout")

using namespace Qt::StringLiterals;
using namespace Fooyin::Tapeout;

namespace {
Listen fullyPopulated()
{
    Listen listen;
    listen.title            = u"LA Girls"_s;
    listen.artist           = u"Charlie Puth"_s;
    listen.artists          = {u"Charlie Puth"_s, u"Someone Else"_s};
    listen.album            = u"Voicenotes"_s;
    listen.trackNumber      = u"3"_s;
    listen.durationMs       = 197000;
    listen.timestamp        = 1789223522;
    listen.listenedMs       = 98470;
    listen.chainName        = u"Desk"_s;
    listen.recordingMbid    = u"37d9bb43-d6ba-4bb2-9a54-b49a6ce0c8cb"_s;
    listen.releaseMbid      = u"25854863-7bf8-4dd8-ad9c-d4a7a613b332"_s;
    listen.releaseGroupMbid = u"dd68d7de-55bc-4da5-bc53-f454fc71e8bb"_s;
    listen.releaseTrackMbid = u"bbbbbbbb-0000-0000-0000-000000000002"_s;
    listen.workMbid         = u"cccccccc-0000-0000-0000-000000000003"_s;
    listen.isrc             = u"USAT21804255"_s;
    listen.artistMbids      = {u"525f1f1c-03f0-4bc8-8dfd-e7521f87631b"_s};
    listen.albumArtistMbids = {u"dddddddd-0000-0000-0000-000000000004"_s};
    return listen;
}

void queueSurvivesRoundTrip()
{
    const Listen before = fullyPopulated();
    const Listen after  = Listen::deserialise(before.serialise());

    assert(after.title == before.title);
    assert(after.artist == before.artist);
    assert(after.artists == before.artists);
    assert(after.album == before.album);
    assert(after.trackNumber == before.trackNumber);
    assert(after.durationMs == before.durationMs);
    assert(after.timestamp == before.timestamp);
    assert(after.listenedMs == before.listenedMs);
    assert(after.chainName == before.chainName);
    assert(after.recordingMbid == before.recordingMbid);
    assert(after.releaseMbid == before.releaseMbid);
    assert(after.releaseGroupMbid == before.releaseGroupMbid);
    assert(after.releaseTrackMbid == before.releaseTrackMbid);
    assert(after.workMbid == before.workMbid);
    assert(after.isrc == before.isrc);
    assert(after.artistMbids == before.artistMbids);
    assert(after.albumArtistMbids == before.albumArtistMbids);
}

// A queue written by an older build has none of the newer keys. It must replay as
// a listen missing those fields, never as a parse failure.
void olderQueueEntryStillLoads()
{
    QJsonObject old;
    old.insert("title"_L1, u"LA Girls"_s);
    old.insert("artist"_L1, u"Charlie Puth"_s);
    old.insert("timestamp"_L1, 1789223522);

    const Listen listen = Listen::deserialise(old);
    assert(listen.isValid());
    assert(listen.timestamp == 1789223522);
    assert(listen.workMbid.isEmpty());
    assert(listen.albumArtistMbids.isEmpty());
}

void wireCarriesEveryMbid()
{
    const QJsonObject info = fullyPopulated().toJson().value("additional_info"_L1).toObject();

    assert(info.contains("recording_mbid"_L1));
    assert(info.contains("release_mbid"_L1));
    assert(info.contains("release_group_mbid"_L1));
    assert(info.contains("release_track_mbid"_L1));
    assert(info.contains("work_mbid"_L1));
    assert(info.contains("isrc"_L1));
    assert(info.contains("artist_mbids"_L1));
    assert(info.contains("album_artist_mbids"_L1));
}

// An MBID that was never tagged must be absent, not sent blank — Tapedeck treats a
// present-but-empty id differently from a missing one.
void untaggedMbidsAreOmitted()
{
    Listen listen;
    listen.title     = u"Untagged"_s;
    listen.artist    = u"Nobody"_s;
    listen.timestamp = 1789223522;

    const QJsonObject info = listen.toJson().value("additional_info"_L1).toObject();

    assert(!info.contains("recording_mbid"_L1));
    assert(!info.contains("release_group_mbid"_L1));
    assert(!info.contains("release_track_mbid"_L1));
    assert(!info.contains("work_mbid"_L1));
    assert(!info.contains("isrc"_L1));
    assert(!info.contains("album_artist_mbids"_L1));
}

/*
 * Which column a lyric lands in is decided by this one predicate, and both ways
 * of getting it wrong are silent: timed words filed as plain leave the reader
 * unable to follow the deck, and plain words filed as timed make Tapedeck read
 * every leading bracket as a cue.
 */
void timedLyricsAreToldApartFromPlainOnes()
{
    assert(lyricsAreTimed(u"[00:12.30] the words\n[00:15.00] more words"_s));
    // The cue need not be on the first line — an LRC usually opens with metadata.
    assert(lyricsAreTimed(u"[ar: Somebody]\n[ti: A Song]\n[00:12.30] the words"_s));
    assert(lyricsAreTimed(u"\t[1:04] indented, but still a cue"_s));

    assert(!lyricsAreTimed(u"the words\nmore words"_s));
    // The case the anchor exists for: a section marker is lyrics in a great many
    // songs, and it is not a timestamp.
    assert(!lyricsAreTimed(u"[Chorus]\nthe words"_s));
    // A bracket mid-line is part of the words, wherever it looks like a cue.
    assert(!lyricsAreTimed(u"she said [12:30] and left"_s));
    assert(!lyricsAreTimed(u""_s));
}
/*
 * The branch that decides listen versus skip. Tapedeck applies the same
 * threshold in reverse, so getting this wrong does not merely mislabel one
 * play: too late banks a listen *and* a skip for it, too early writes a listen
 * the listener's own setting says never happened.
 */
void theScrobbleRuleIsWhicheverComesFirst()
{
    ScrobbleRule rule;
    rule.known = true;
    rule.fraction = 0.5;
    rule.afterSecs = 240;
    rule.noDurationAfterSecs = 240;

    // A three-minute song: the fraction is what it can ever reach.
    assert(!rule.qualifies(180000, 89000));
    assert(rule.qualifies(180000, 90000));

    // A forty-minute raga: four minutes arrives long before half of it, and
    // reading the two as an AND would hold the listen back for twenty.
    assert(rule.qualifies(2400000, 240000));
    assert(!rule.qualifies(2400000, 239000));

    // No length: the fraction is unanswerable and the flat number is all there is.
    assert(!rule.qualifies(0, 239000));
    assert(rule.qualifies(0, 240000));

    // Nothing heard is never a listen, whatever the numbers say.
    assert(!rule.qualifies(180000, 0));
    assert(!rule.qualifies(0, 0));

    // `percent` is the server's own wording and is shown verbatim; `fraction`
    // is what the arithmetic uses. A client that recomputed one from the other
    // would round differently from the screen the listener set it on.
    rule.percent = 50.0;
    assert(rule.describe().startsWith(u"50% or 4:00"_s));

    // A listener who set 100% gets no early pass from the fraction.
    ScrobbleRule strict{
        .known = true, .fraction = 1.0, .percent = 100.0, .afterSecs = 3600, .noDurationAfterSecs = 3600};
    assert(!strict.qualifies(180000, 179000));
    assert(strict.qualifies(180000, 180000));
}
} // namespace

int main()
{
    queueSurvivesRoundTrip();
    olderQueueEntryStillLoads();
    wireCarriesEveryMbid();
    untaggedMbidsAreOmitted();
    timedLyricsAreToldApartFromPlainOnes();
    theScrobbleRuleIsWhicheverComesFirst();

    printf("ok\n");
    return 0;
}
