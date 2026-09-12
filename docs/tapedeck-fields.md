# Tapedeck-side work: fields Tapeout sends that Tapedeck doesn't store

Context doc for the Tapedeck end. Tapeout (v0.2.0) submits these; Tapedeck currently
drops the ones marked **missing**. Nothing here is a Tapeout bug — the plugin sends
them and discards nothing.

Baseline for "what Tapedeck stores" is listen `id: 490239` (Charlie Puth — LA Girls,
`submission_client: "Tapeout"`, 2026-09-12), the first real Tapeout submission.

## Where the fields sit in the payload

Tapeout submits ListenBrainz-shaped JSON. `Listen::toJson()` returns the
*track_metadata* object; `tapedeckclient` wraps it as
`{listen_type, payload:[{listen_at, track_metadata}]}`.

```
track_metadata
├── track_name / artist_name / release_name
└── additional_info
    ├── submission_client          "Tapeout"
    ├── submission_client_version  plugin version, not fooyin's
    ├── media_player               / media_player_version
    ├── duration_ms, tracknumber, listened_ms, skipped
    ├── recording_mbid, release_mbid, release_group_mbid
    ├── release_track_mbid, work_mbid, isrc          <- new in 0.2.0
    ├── artist_mbids[], album_artist_mbids[]         <- album_artist_mbids new
    ├── artist_names[]             only when >1
    └── tapedeck_audio / tapedeck_device / tapedeck_session
        / tapedeck_playback / tapedeck_chain
```

The offline queue uses a *different*, flat shape (`Listen::serialise()` /
`deserialise`, `~/.local/share/fooyin/tapeout/queue.json`, `"version": 1`).
Unknown keys deserialise to empty, so adding fields does not need a version bump.

## Field inventory

| Tapeout sends | Tapedeck column | Status |
|---|---|---|
| `recording_mbid` | `mbid_recording` | stored |
| `release_mbid` | `mbid_release` | stored |
| `artist_mbids[]` | `mbid_artist` (JSON array) | stored |
| `release_group_mbid` | — | **missing** |
| `release_track_mbid` | — | **missing**, new in 0.2.0 |
| `work_mbid` | — | **missing**, new in 0.2.0 |
| `album_artist_mbids[]` | — | **missing**, new in 0.2.0 |
| `isrc` | `isrc` | column exists and is populated from other sources (Plex rows carry it), so this should land once tagged files are submitted |

Note `caa_release_mbid` (`dd68d7de…`) differs from `mbid_release` (`25854863…`).
That's Tapedeck resolving cover art via another release in the group and is correct
behaviour, not a mismatch to fix.

## One thing to check on the Tapedeck side

**`Accepted 1 duplicate 1` when both rows were new.** One batch submitted a skip
(`490246`) and a listen (`490247`); both exist, neither was a genuine duplicate. The
guess is that a skip is reported as `duplicate` because it creates no listen row — but
it makes the client's success log actively misleading, and Tapeout has no way to tell a
real duplicate from this. Worth pinning down.

(An earlier note here claimed skips never carry a chain, based on `490246` having
`chain: null`. Skip `490254` came back with `chain: "Desktop Casual"`, so that was wrong
— whatever the difference is, it resolves at least some of the time.)

## What to do Tapedeck-side

1. **Add columns**: `mbid_release_group`, `mbid_release_track`, `mbid_work`,
   `mbid_album_artist` (JSON array, mirroring `mbid_artist`).
2. **Populate `isrc`** from `additional_info.isrc` — the column is already there.
3. `mbid_release_group` is the highest-value one: it's what groups reissues,
   remasters and regional editions into one release, and the README sells it as a
   differentiator over a generic ListenBrainz client.
4. `mbid_work` matters most for classical, where one work spans many recordings and
   performers — likely the largest practical win given the size of that part of the
   history.

Tapeout omits empty MBIDs entirely rather than sending blanks, so a missing key
means "not tagged", never "tagged empty". Treat absent and null identically.

## One-off: codec casing in existing rows

Tapeout sent `codec` as fooyin reports it — `"FLAC"` — while Plex and other sources
send `"flac"`. Tapedeck groups on the literal string, so the two spellings split any
per-codec statistic. Tapeout now lowercases before submitting, matching what it
already did for `container`.

Rows submitted before that change carry the uppercase spelling: listens `490239` and
`490247` at minimum. A one-line `UPDATE ... SET codec = lower(codec)` cleans them up,
and is worth doing across all sources rather than just Tapeout's.

## Fixed: `listened_ms` (kept here for the history)

`listened_ms` records the moment fooyin's played-threshold fires, not how much was
actually heard. `handleTrackPlayed` (`src/core/tapeoutcontroller.cpp:211`) samples
`PlayerController::currentTimeListened()` inside the `trackPlayed` signal, which
fooyin emits on threshold crossing — not at track end.

Observed twice, on unrelated tracks:

| Listen | Reported | Duration | Fraction |
|---|---|---|---|
| 490239 LA Girls | 98470 ms | 197 s | 49.98% |
| 490247 Happy Nation | 124187 ms | 248 s | 50.07% |

This is independent of Tapedeck's own scrobble threshold: that setting decides
*whether* a listen counts, while `listened_ms` claims *how much* was heard. A
fully-played track will keep reporting roughly the threshold value.

**Skips are unaffected** — they submit from a different path and report honestly.
Skip `490246` recorded 18000 ms of a 203 s track (8.9%), which is what actually
happened. So the field is trustworthy on skips and a floor on listens.

**The fix is the small one.** The open question was whether
`PlayerController::currentTimeListened()` still holds the *outgoing* track's value
when `handleTrackChanged` (`src/core/tapeoutcontroller.cpp:176`) fires, or has
already reset. Skip 490246 proves it holds: an 18-second skip reported 18000 ms from
exactly that context. So submission can move to track-end, where `handleTrackChanged`
already branches on `m_currentPlayed` — both listen and skip collapse into one place
reading the final value, and `handleTrackPlayed` shrinks to setting the flag.

Cost of the change: a listen is submitted at track end rather than at the threshold,
so a `SIGKILL` mid-track loses it where before it would already have been sent. A clean
exit is safe — `TapeoutController::shutdown()` calls `finishCurrent()` before
`ListenQueue::write()`, so a qualifying track still playing at exit is kept.

**There was a second, worse half to this.** Moving submission to track-end was not
enough, because `currentTimeListened()` cannot be read at the end at all: fooyin resets
the counter *before* emitting `currentTrackChanged`. Reading it there is a race. It
returned correct values for some skips and `0` for others, and a fully-played track
replayed immediately (repeat-one, or the same track queued twice) submitted a listen
with `listened_ms: 0`.

The counter is therefore **sampled once a second while the track plays**
(`positionChangedSeconds`), and the last sample is what gets submitted; the live counter
is still consulted and used if higher, so a play ending between samples is not
truncated. Costs up to one second of precision at the tail. The tally resets whenever a
new play is adopted, including the same title arriving again.

**Verified in production.** Sunsetz (`490255`) reported 214674 ms against 214 s.
Crépuscule queued twice gave two listens (225306 / 225290 ms, 225 s track) and
Dans l'obscurité looped three times gave three (248428 / 248554 / 248533 ms, 248 s),
each with its own timestamp — while the live counter read `0` at submission time for
the first two, which is exactly the case the sampling exists to cover.

Rows submitted before the fix under-report and cannot be recovered, since the real
figure was never recorded: listens `490239`, `490247`, `490249`–`490252` hold the
played-threshold rather than listening time, and skip `490254` holds `0` from the race
rather than a real measurement. Treat all of them as unreliable for this field only.

### Known characteristic: repeat-one restarts through a stop

fooyin loops a track as `Playing → Stopped → Playing` rather than a direct track change,
and Tapeout's stop handler clears what it is tracking. The loop iteration is only picked
up again because `currentTrackChanged` fires immediately afterwards — which it does, on
every iteration observed. Two consequences:

- Now-playing is cleared and re-announced on each loop, so the deck flickers briefly.
  Cosmetic; it self-heals on the next update.
- Tracking depends on that signal arriving. Adopting the player's track on the
  `Playing` transition instead was tried and rejected: it fires *before*
  `currentTrackChanged`, so the following change event would see a valid track with no
  listened time and submit a spurious skip on every loop.
