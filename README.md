# Tapeout

A [fooyin](https://github.com/fooyin/fooyin) plugin that reports playback to a self-hosted
[Tapedeck](https://codeberg.org/abksh/tapedeck) instance.

Tapedeck speaks ListenBrainz, and fooyin's built-in scrobbler can already point at it. Tapeout
exists for the part a generic ListenBrainz client cannot express — **how** you listened:

- **Audio quality** as fooyin actually decoded it — codec, container, sample rate, bit depth,
  channels, lossless. Fooyin is the decoder, so this is measured rather than guessed at.
- **The output device**, which Tapedeck learns and binds to a signal chain. Switch from speakers
  to a headphone DAC and the chain follows, without restating it per listen.
- **Skips**, submitted as skips. Tapedeck stores them and excludes them from every count.
- Recording, release, **release-group** and **artist** MusicBrainz ids, where the file carries them.

It runs alongside your other scrobbling services rather than replacing them.

> **Status: early.** Listen submission, now-playing, an offline queue and the settings page are
> written but not yet built against a real fooyin install. Not usable yet.

## Building

Requires fooyin's development files, Qt 6.4+, CMake 3.19+ and a C++23 compiler.

```bash
cmake -B build && cmake --build build
```

The built plugin is installed into fooyin's plugin directory by `cmake --install build`.

## Configuring

**Settings → Integrations → Tapedeck.** Enter your Tapedeck address and an API token with the
`submit` scope, then press **Test**.

If you already have fooyin's built-in scrobbler pointing at the same Tapedeck instance, disable
that service. Both submit the same listen at the same timestamp, Tapedeck deduplicates them, and
whichever arrives second is dropped — so leaving both on risks discarding the richer payload.

## Licence

MIT — see [LICENSE](LICENSE).

Note that fooyin itself is GPL-3.0. A *distributed binary* of this plugin is a combined work and
is effectively GPL-3.0; the source in this repository is MIT.
