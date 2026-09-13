# Tapedeck-side work: two endpoints Tapeout needs

Context doc for the Tapedeck end, the companion to [tapedeck-fields.md](tapedeck-fields.md).
That one is about columns Tapedeck already has; this one is about **two routes that do
not exist yet**. Tapeout 0.4.0 speaks to them, both switches are **off by default**, and
until the server half lands turning one on costs a 404 per track and nothing else.

Line numbers are against the Tapedeck tree as of 2026-09-12.

## Why not the routes that already exist

| Existing | Why it does not serve |
|---|---|
| `PUT /api/v1/lyrics` | Session-only, and it calls `set_words` — which sets `edited` and stamps `source = 'You'`. That is a *correction*, a claim only a person typing gets to make. A player reporting what its files contain is not correcting anything. |
| `POST /api/v1/lyrics/fetch` | Session-only, and rightly so: it spends a shared politeness budget at LRCLIB. Tapeout is offering words, not asking for them. |
| `PUT /api/v1/albums/{id}/cover`, `POST /api/v1/albums/{id}/cover/upload` | Session-only, and they take an **entity id**. A scrobbling client knows names, never ids. |
| `GET /api/v1/resolve` | Session-only, and it *creates the entity on first visit* — a write on a read path. A player asking "do you have a cover" must not mint entities as a side effect. |
| `additional_info` on `/1/submit-listens` | Considered and rejected: a lyric is a few KB on every play of a track Tapedeck already has words for, and a cover is a megabyte of JPEG through the ingest path on every album change. Ingest stays about the listening. |

Both new routes take a **token with the `write` scope** — `scoped_token_user(parts, state,
"write", |t| t.can_write())` in `src/server/auth.rs:179`. Not `read`: the precondition of a
write should not force a scrobbling token to carry read access to the whole history.

---

## 1. `POST /api/v1/lyrics/library`

The words a file carries, from a client that is playing that file.

```http
POST /api/v1/lyrics/library
Authorization: Token <write>
Content-Type: application/json

{
  "artist": "Charlie Puth",
  "title":  "LA Girls",
  "album":  "Voicenotes",          // optional, advisory
  "plain":  "…",                   // either or both
  "synced": "[00:12.30] …",
  "source": "Tapeout"
}
```

**The whole handler is one existing call**: `LyricsClient::remember_library(artist, title,
plain, synced, source)` — `src/lyrics.rs:362`. It already does everything that makes this
safe:

- refuses to write over a row holding `edited`, so a correction typed in Tapedeck survives;
- records provenance rather than assuming it, which is the reason `source` is in the body
  at all — "from your own files" and "from LRCLIB" are different statements about whose
  text the reader is looking at;
- re-reads the language from the new words;
- upserts, so a repeat send is harmless.

Responses:

| Code | Body | When |
|---|---|---|
| 200 | `{"status": "saved"}` | Stored, or silently left alone because the row is `edited`. |
| 400 | error | Blank artist or title, or no words in the body. |
| 401 | error | No token, or no `write` scope. |

Two calls worth making that `remember_library` does not: report whether the row was
`edited` (`is_edited`, `src/lyrics.rs:422`) so the answer distinguishes *stored* from
*left alone*, and prefer the client's source string only if you do not want to derive it
from the token's connection name instead. Tapeout sends `"Tapeout"`; the server owning
that decision is fine, it just needs to be one of the two.

**Not in scope.** No `instrumental` — `remember_library` hardcodes it to 0 and a player
cannot tell "no words tagged" from "no words exist". Tapeout never sends an empty lyric.

### What Tapeout sends

Once per track per run, on track change rather than on the scrobble, so the words are
there while the song is playing. `synced` is an `.lrc` sidecar beside the file or a timed
tag; `plain` is `LYRICS`/`UNSYNCEDLYRICS`. A sidecar outranks a tag it collides with. A
CUE or chapter track is skipped for sidecars — those share one audio file, so a single
`.lrc` beside it belongs to the whole side rather than to one track.

---

## 2. `/api/v1/art/library`

Two methods on one route, because the question only exists to decide the write.

### `GET /api/v1/art/library?artist=&album=`

```json
{ "held": true }
```

`held` is exactly `Database::album_artwork(user_id, artist, album).is_some()` —
`src/db/library.rs:299`, which already asks the right question (`artwork_url` non-empty
**or** `caa_id > 0`, tombstones excluded) against the right key. Add the entity's
`cover_override` to it if a hand-set cover should also count as held; it should.

This exists so the common case costs one small GET instead of a megabyte. Tapeout does not
read the cover off disk at all until this answers `false`.

### `POST /api/v1/art/library`

`multipart/form-data`:

| Field | |
|---|---|
| `artist` | text, required — the **track** artist, matching `scrobbles.artist` |
| `album` | text, required |
| `release_mbid` | text, optional, normalised — advisory only |
| `file` | the image, original bytes, content type declared |

**Write `scrobbles.artwork_url`, not a cover override.** The column is the artwork
backfill's queue and `GET /api/v1/albums/{id}` already falls back to "the artwork the
newest listen of this album carries" — so a cover that arrived with the listens is
exactly what a Plex-sourced listen carries, and reads back through the path that already
exists. A `cover_override` would mean somebody set this by hand, and nobody did.

Reuse from `src/server/artwork.rs`: `MAX_ART_BYTES` (8 MB), `physical::image_ext` for the
type check, and `store_upload(state, bytes, ext)` (line 431) for the write and the
returned `/api/v1/art/upload/<random>.<ext>` URL. Then `UPDATE scrobbles SET artwork_url =
? WHERE user_id = ? AND LOWER(TRIM(artist)) = ? AND LOWER(TRIM(album)) = ? AND (artwork_url
IS NULL OR artwork_url = '')` — the same key `album_artwork` uses, and only over rows the
backfill has not already filled.

⚠️ **`read_one_image` cannot be reused as-is** (`src/server/artwork.rs:387`). It takes the
first field that yields bytes, and this body has text fields before the file. Read the
fields by name; keep its size and type checks.

Responses:

| Code | Body | When |
|---|---|---|
| 200 | `{"artwork_url": "/api/v1/art/upload/….jpg", "listens_covered": N}` | Stored, and attached to N listens. |
| 200 | `{"already_held": true}` | Something filled the cover between the GET and this. Not an error. |
| 400 | error | No image, or a blank artist or album. |
| 401 | error | No token, or no `write` scope. |
| 413 | error | Larger than 8 MB. Tapeout drops these client-side, so this should be rare. |
| 415 | error | Not an image type this server stores. |

⚠️ **`listens_covered: 0` means the cover was stored and orphaned.** Nothing references
a file that covered no listens. It is reported rather than refused because the upload did
succeed and the caller is the one that can tell whether that was expected — but a client
that sees it has got its ordering wrong, which is exactly what happened the first time
Tapeout ran this path (below).

### What Tapeout sends

**After the listen has been stored, never on track change.** This is the ordering the
`UPDATE` above forces and it is not obvious: a record with no listens yet is precisely the
record whose artwork is missing, so an upload sent while the track is still playing
matches no rows and leaves the file orphaned. Tapeout holds the cover from the moment the
listen is queued and offers it only once Tapedeck has confirmed the submission — which
also means a skipped track never uploads one, since a skip stores no row for it to attach
to.

Once per `(artist, album)` per run. The embedded front cover as the file stores it —
never re-encoded, read through fooyin's own `AudioLoader::readTrackCover` — or, where
there is none embedded, a conventionally named image beside the file (`cover`, `folder`,
`front`, `album`, `artwork` × jpg/jpeg/png/webp). Content type is sniffed from the bytes,
not from the extension. Oversized and non-image files are dropped before the request.

On a compilation the same sleeve is offered once per contributing artist, because that is
how Tapedeck keys an album. Each repeat is a GET that answers `held` after the first
upload — except the very first per artist, which uploads again, since the rows for that
artist genuinely have no artwork yet. That is the model behaving as designed, not a bug
to work around in the client.

---

## Checklist

- [ ] `POST /api/v1/lyrics/library`, `write` scope → `remember_library`
- [ ] `GET /api/v1/art/library` → `album_artwork(...).is_some()` (+ `cover_override`)
- [ ] `POST /api/v1/art/library` → `store_upload` + `UPDATE scrobbles SET artwork_url`
- [x] Multipart read by field name, not `read_one_image`
- [ ] `openapi.yaml` entries under the Statistics and Library tags
- [ ] Flip Tapeout's two switches on and drop the "off by default" note from its README
