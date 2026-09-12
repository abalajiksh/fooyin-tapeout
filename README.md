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
- Recording, release, **release-group**, release-track, **work** and artist/album-artist
  MusicBrainz ids, plus ISRC, where the file carries them. The work id is the one that ties
  every performance of the same piece together, which generic scrobblers drop entirely.

It runs alongside your other scrobbling services rather than replacing them.

> **Status: early, but working.** Listens, skips, now-playing and the offline queue have all
> been exercised against a real fooyin 0.12.6 and a live Tapedeck — including a queue that
> survived an unreachable server and a restart and replayed with its original timestamps
> intact, a track queued twice scrobbling twice, and repeat-one scrobbling once per loop.
> See [docs/tapedeck-fields.md](docs/tapedeck-fields.md) for the MusicBrainz fields Tapedeck
> does not yet store.

## Building

Requires fooyin 0.12.6 or newer **and its development files**, Qt 6.4+, CMake 3.19+ and a
C++23 compiler.

Fedora packages those development files as `fooyin-devel` in the official repositories. No
other distribution currently packages fooyin at all — its `.deb`, AppImage, Flatpak, FreeBSD
and Windows builds come from [fooyin's own releases](https://github.com/fooyin/fooyin/releases)
and ship no headers — so everywhere else you will need to build fooyin from source first.

> **Tested only on Fedora 44 (Sway desktop), x86-64.** That is the sole configuration this
> plugin has been built and run on. fooyin itself ships BSD, Debian, Windows and ARM builds,
> with macOS expected — Tapeout should be portable to all of them, since it uses nothing
> beyond Qt and fooyin's own APIs, but none of that is verified. Reports from other platforms
> are welcome; treat them as untested rather than unsupported.

```bash
cmake -B build && cmake --build build
sudo cmake --install build
```

`sudo` is needed because fooyin's CMake config hardcodes `FOOYIN_PLUGIN_INSTALL_DIR` to an
absolute path under fooyin's *own* prefix — `/usr/lib64/fooyin/plugins` on Fedora. Setting
`CMAKE_INSTALL_PREFIX` does not move the plugin, and fooyin has no per-user plugin directory
to install into instead.

That path is Fedora's. It follows whatever prefix and library directory fooyin itself was
installed with, so a source-built fooyin under `/usr/local` puts plugins in
`/usr/local/lib/fooyin/plugins` instead. `cmake --install` always gets it right because it
reads the value from fooyin's own config; to see where it will go:

```bash
grep FOOYIN_PLUGIN_INSTALL_DIR "$(dirname "$(find / -name FooyinConfig.cmake 2>/dev/null | head -1)")/FooyinConfig.cmake"
```

When iterating, symlink the build output once and skip the install step thereafter:

```bash
sudo ln -sf "$PWD/build/fyplugin_tapeout.so" /usr/lib64/fooyin/plugins/
```

The plugin is ABI-coupled to fooyin and nothing catches a mismatch at load time, so it must be
rebuilt whenever fooyin is upgraded. `find_package` enforces the minimum at build time.

### Other distributions

Everything above assumes Fedora, where `fooyin-devel` provides the headers. Anywhere else,
fooyin has to be built from source first — its `.deb`, AppImage and Flatpak builds ship the
application only. On Ubuntu or Debian:

```bash
# fooyin's own build dependencies
sudo apt install g++ git cmake pkg-config ninja-build libglu1-mesa-dev \
  libxkbcommon-dev zlib1g-dev libasound2-dev libtag1-dev libicu-dev \
  libpipewire-0.3-dev libpulse-dev qt6-base-dev libqt6sql6-sqlite \
  libqt6svg6-dev qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools \
  libavcodec-dev libavfilter-dev libavformat-dev libavutil-dev libswresample-dev

git clone --depth 1 --branch v0.12.6 https://github.com/fooyin/fooyin
cmake -S fooyin -G Ninja -B fooyin/build
cmake --build fooyin/build
sudo cmake --install fooyin/build            # installs to /usr/local
```

Then build Tapeout exactly as above — `find_package` searches `/usr/local` by default, and the
plugin follows fooyin's prefix into `/usr/local/lib/fooyin/plugins`.

> **Run the fooyin you just built** (`/usr/local/bin/fooyin`). If fooyin's `.deb` is also
> installed, `/usr/bin/fooyin` is a different build that searches its own prefix and will not
> see the plugin. Forcing the two together is worse than it not loading: there is no ABI check
> at load time, and the same fooyin version built by a different toolchain is not guaranteed
> compatible. Either remove the `.deb` or accept that only the source build gets the plugin.

Flatpak and AppImage installs cannot load third-party plugins at all — one is sandboxed under
its own prefix, the other is a read-only image.

### Tests

```bash
cmake -B build -DTAPEOUT_TESTS=ON && cmake --build build && ctest --test-dir build
```

## Configuring

**Settings → Integrations → Tapedeck.** Tick **Enabled**, enter your Tapedeck address and an API
token with the `submit` scope, then press **Test**. Nothing is submitted until **Enabled** is
ticked, so a passing **Test** alone will not produce listens.

The token is stored in plaintext in `~/.config/fooyin/fooyin.conf`, the same way fooyin's own
scrobbler stores its credentials.

If you already have fooyin's built-in scrobbler pointing at the same Tapedeck instance, disable
that service. Both submit the same listen at the same timestamp, Tapedeck deduplicates them, and
whichever arrives second is dropped — so leaving both on risks discarding the richer payload.

## Licence

MIT — see [LICENSE](LICENSE).

Note that fooyin itself is GPL-3.0. A *distributed binary* of this plugin is a combined work and
is effectively GPL-3.0; the source in this repository is MIT.
