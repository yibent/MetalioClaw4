#!/usr/bin/env python3
"""Prepare the GIF and compressed music assets used by the stress-test screen.

The source files are intentionally kept outside the build directory so they can
be replaced without touching firmware code.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-width", type=int, default=480)
    parser.add_argument("--max-height", type=int, default=480)
    parser.add_argument("--max-fps", type=float, default=20.0)
    parser.add_argument("--gif-colors", type=int, default=128)
    parser.add_argument("--audio-bitrate", default="24k")
    parser.add_argument("--audio-rate", type=int, default=16000)
    parser.add_argument("--max-total-bytes", type=int, default=0)
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if args.max_width <= 0 or args.max_height <= 0:
        raise RuntimeError("GIF dimensions must be positive")
    if not 2 <= args.gif_colors <= 256:
        raise RuntimeError("GIF color count must be between 2 and 256")
    if args.audio_rate <= 0:
        raise RuntimeError("audio sample rate must be positive")
    if args.max_total_bytes < 0:
        raise RuntimeError("maximum total size cannot be negative")


def prepare_gif(source: Path, output: Path, args: argparse.Namespace) -> None:
    try:
        from PIL import Image, ImageSequence
    except ImportError as exc:
        raise RuntimeError(
            "Pillow is required to optimize stress_test.gif; "
            "install it with 'python -m pip install Pillow'"
        ) from exc

    with Image.open(source) as image:
        frames = []
        durations = []
        source_fps = 0.0
        requires_palette_reduction = False
        pending_duration = 0
        min_duration = (
            max(20, int(round(1000.0 / args.max_fps)))
            if args.max_fps > 0
            else 20
        )
        for frame in ImageSequence.Iterator(image):
            duration_ms = max(20, int(frame.info.get("duration", 50)))
            source_fps = max(source_fps, 1000.0 / duration_ms)
            if not requires_palette_reduction:
                requires_palette_reduction = (
                    frame.convert("RGB").getcolors(maxcolors=args.gif_colors) is None
                )
            pending_duration += duration_ms
            if frames and pending_duration < min_duration:
                continue

            rgba = frame.convert("RGBA")
            rgba.thumbnail((args.max_width, args.max_height), Image.Resampling.LANCZOS)
            # A bounded palette keeps the decoder's working set and SPIFFS image
            # small while retaining transparency from the source frame.
            has_transparency = rgba.getchannel("A").getextrema()[0] < 128
            color_count = args.gif_colors - 1 if has_transparency else args.gif_colors
            palette = rgba.convert("RGB").quantize(
                colors=max(2, color_count), method=Image.Quantize.MEDIANCUT
            )
            if has_transparency:
                transparency_index = max(2, args.gif_colors) - 1
                transparent_mask = rgba.getchannel("A").point(
                    lambda alpha: 255 if alpha < 128 else 0
                )
                palette.paste(transparency_index, mask=transparent_mask)
                palette.info["transparency"] = transparency_index
            frames.append(palette)
            durations.append(max(min_duration, pending_duration))
            pending_duration = 0

        if not frames:
            raise RuntimeError(f"GIF contains no frames: {source}")

        if pending_duration:
            durations[-1] += pending_duration

        frames[0].save(
            output,
            format="GIF",
            save_all=True,
            append_images=frames[1:],
            duration=durations,
            loop=0,
            optimize=True,
            disposal=1,
        )

        requires_resize = image.width > args.max_width or image.height > args.max_height
        requires_frame_reduction = args.max_fps > 0 and source_fps > args.max_fps
        if (
            output.stat().st_size >= source.stat().st_size
            and not requires_resize
            and not requires_frame_reduction
            and not requires_palette_reduction
        ):
            shutil.copyfile(source, output)

    print(
        f"prepared {source.name}: {source.stat().st_size} -> "
        f"{output.stat().st_size} bytes ({source_fps:.1f} source fps)"
    )


def prepare_audio(source: Path, output: Path, args: argparse.Namespace) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError(
            "ffmpeg is required to optimize stress_test_music.ogg; "
            "install ffmpeg and make it available on PATH"
        )

    command = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        str(source),
        "-map_metadata",
        "-1",
        "-vn",
        "-c:a",
        "libmp3lame",
        "-b:a",
        args.audio_bitrate,
        "-ac",
        "1",
        "-ar",
        str(args.audio_rate),
        "-id3v2_version",
        "0",
        "-write_id3v1",
        "0",
        str(output),
    ]
    subprocess.run(command, check=True)
    print(
        f"prepared {source.name}: {source.stat().st_size} -> "
        f"{output.stat().st_size} bytes ({args.audio_bitrate}, mono, "
        f"{args.audio_rate} Hz)"
    )


def main() -> int:
    args = parse_args()
    validate_args(args)
    if not args.source.is_dir():
        raise RuntimeError(f"stress asset source directory not found: {args.source}")
    args.output.mkdir(parents=True, exist_ok=True)

    gif_source = args.source / "stress_test.gif"
    audio_source = args.source / "stress_test_music.ogg"
    gif_output = args.output / gif_source.name
    audio_output = args.output / "stress_test_music.mp3"

    # Remove the former Ogg Opus build output so SPIFFS never packages a stale
    # file after upgrading an existing build directory.
    legacy_audio_output = args.output / audio_source.name
    for path in (gif_output, audio_output, legacy_audio_output):
        path.unlink(missing_ok=True)

    missing = [
        path.name
        for path in (gif_source, audio_source)
        if not path.is_file()
    ]
    if missing:
        raise RuntimeError(
            "missing stress-test media: "
            + ", ".join(missing)
            + f"; place them in {args.source}"
        )

    prepare_gif(gif_source, gif_output, args)
    prepare_audio(audio_source, audio_output, args)

    prepared_size = sum(
        path.stat().st_size for path in (gif_output, audio_output) if path.is_file()
    )
    if args.max_total_bytes > 0 and prepared_size > args.max_total_bytes:
        raise RuntimeError(
            f"prepared stress assets use {prepared_size} bytes, exceeding the "
            f"{args.max_total_bytes}-byte limit; shorten the music/GIF or lower "
            "the configured bitrate, frame rate, dimensions, or color count"
        )

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
