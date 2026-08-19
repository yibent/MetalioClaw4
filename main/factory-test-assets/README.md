# Stress-test media

Place the two source files in this directory before building. The build stops
with an explicit error if either file is missing:

- `stress_test.gif`
- `stress_test_music.ogg`

The build optimizes the GIF to a maximum of 480x480 pixels and 20 FPS. The OGG
source is re-encoded as mono MP3 at 16 kHz / 24 kbps and packaged as
`stress_test_music.mp3`. This avoids the ESP Audio Codec OGG/Opus parser path,
which can reject valid multi-packet OGG pages and crash its decoder task. The
source OGG may contain any codec that `ffmpeg` can decode.

The prepared files may use up to 1,150,000 bytes in total. If they exceed that
limit, shorten the media or lower the compression settings in
`main/CMakeLists.txt`.

The optimizer needs Pillow (available in the ESP-IDF Python environment) and
an `ffmpeg` build with `libmp3lame` on `PATH`. The firmware opens the prepared
files as `/factory_test/stress_test.gif` and
`/factory_test/stress_test_music.mp3`.

## Flashing

The `factory_test` partition was enlarged from 600 KB to 1408 KB. The first
build after this change must be flashed with the updated partition table and
the generated SPIFFS image:

```sh
idf.py flash
```

Do not use app-only flashing for that first update: it does not write the
partition table or `factory_test.bin`. Once a device already has the enlarged
partition table, media-only updates can be written with:

```sh
idf.py factory_test-flash
```
