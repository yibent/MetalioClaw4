#!/usr/bin/env python3
"""Prepare image assets before esp_mmap_assets converts them to split images."""

import argparse
import fnmatch
import shutil
from pathlib import Path

from PIL import Image

LANCZOS = getattr(Image, "Resampling", Image).LANCZOS


def resize_square_icon(path: Path, target_size: int) -> None:
    with Image.open(path) as image:
        width, height = image.size
        if width != height:
            raise ValueError(
                f"desktop icon must be square: {path.name} is {width}x{height}"
            )
        if width == target_size:
            return

        rgba = image.convert("RGBA")
        resized = rgba.resize(
            (target_size, target_size), LANCZOS
        )
        resized.save(path, format="PNG", optimize=True)

    print(f"Prepared desktop icon: {path.name} {width}x{height} -> "
          f"{target_size}x{target_size}")


def prepare_assets(source_dir: Path, output_dir: Path, icon_glob: str,
                   icon_size: int, native_copy_glob: str,
                   native_copy_suffix: str) -> None:
    source_dir = source_dir.resolve()
    output_dir = output_dir.resolve()

    if not source_dir.is_dir():
        raise ValueError(f"asset source directory does not exist: {source_dir}")
    if source_dir == output_dir:
        raise ValueError("asset source and output directories must be different")
    if icon_size < 1 or icon_size > 65535:
        raise ValueError("icon size must be in the SPNG range 1..65535")

    if output_dir.exists():
        shutil.rmtree(output_dir)
    shutil.copytree(source_dir, output_dir)

    matched = 0
    native_copies = 0
    for path in sorted(output_dir.rglob("*.png")):
        if fnmatch.fnmatch(path.name, icon_glob):
            if native_copy_glob and fnmatch.fnmatch(path.name, native_copy_glob):
                native_copy = path.with_name(
                    f"{path.stem}{native_copy_suffix}{path.suffix}"
                )
                shutil.copy2(path, native_copy)
                native_copies += 1
            resize_square_icon(path, icon_size)
            matched += 1

    if matched == 0:
        raise ValueError(f"no PNG assets matched desktop icon glob: {icon_glob}")

    print(f"Prepared {matched} desktop icons for {icon_size}x{icon_size} SPNG output")
    if native_copies:
        print(f"Preserved {native_copies} native-size preview icons")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Copy assets and normalize square desktop icons before SPNG conversion"
    )
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--icon-glob", default="ic_app_home_theme*.png")
    parser.add_argument("--icon-size", required=True, type=int)
    parser.add_argument("--native-copy-glob", default="")
    parser.add_argument("--native-copy-suffix", default="_preview")
    args = parser.parse_args()

    prepare_assets(
        args.source,
        args.output,
        args.icon_glob,
        args.icon_size,
        args.native_copy_glob,
        args.native_copy_suffix,
    )


if __name__ == "__main__":
    main()
