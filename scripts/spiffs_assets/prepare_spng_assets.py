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


def resize_image_to_fit(path: Path, max_width: int, max_height: int) -> None:
    with Image.open(path) as image:
        width, height = image.size
        scale = min(max_width / width, max_height / height)
        target_width = max(1, round(width * scale))
        target_height = max(1, round(height * scale))
        if (width, height) == (target_width, target_height):
            return

        if image.mode not in ("RGB", "RGBA"):
            image = image.convert("RGBA")
        resized = image.resize((target_width, target_height), LANCZOS)
        resized.save(path, format="PNG", optimize=True)

    print(f"Prepared boot image: {path.name} {width}x{height} -> "
          f"{target_width}x{target_height}")


def prepare_assets(source_dir: Path, output_dir: Path, icon_glob: str,
                   icon_size: int, native_copy_glob: str,
                   native_copy_suffix: str, boot_image: str,
                   boot_image_max_width: int,
                   boot_image_max_height: int) -> None:
    source_dir = source_dir.resolve()
    output_dir = output_dir.resolve()

    if not source_dir.is_dir():
        raise ValueError(f"asset source directory does not exist: {source_dir}")
    if source_dir == output_dir:
        raise ValueError("asset source and output directories must be different")
    if icon_size < 1 or icon_size > 65535:
        raise ValueError("icon size must be in the SPNG range 1..65535")
    if boot_image_max_width < 1 or boot_image_max_width > 65535:
        raise ValueError("boot image width must be in the SPNG range 1..65535")
    if boot_image_max_height < 1 or boot_image_max_height > 65535:
        raise ValueError("boot image height must be in the SPNG range 1..65535")

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

    boot_image_path = output_dir / boot_image
    if not boot_image_path.is_file():
        raise ValueError(f"boot image does not exist: {boot_image_path}")
    resize_image_to_fit(
        boot_image_path, boot_image_max_width, boot_image_max_height
    )

    if matched == 0:
        print(f"No desktop icons matched {icon_glob}; skipped icon resize")
    else:
        print(f"Prepared {matched} desktop icons for {icon_size}x{icon_size} SPNG output")
    if native_copies:
        print(f"Preserved {native_copies} native-size preview icons")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Copy and resize selected assets before SPNG conversion"
    )
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--icon-glob", default="ic_app_home_theme*.png")
    parser.add_argument("--icon-size", required=True, type=int)
    parser.add_argument("--native-copy-glob", default="")
    parser.add_argument("--native-copy-suffix", default="_preview")
    parser.add_argument("--boot-image", required=True)
    parser.add_argument("--boot-image-max-width", required=True, type=int)
    parser.add_argument("--boot-image-max-height", required=True, type=int)
    args = parser.parse_args()

    prepare_assets(
        args.source,
        args.output,
        args.icon_glob,
        args.icon_size,
        args.native_copy_glob,
        args.native_copy_suffix,
        args.boot_image,
        args.boot_image_max_width,
        args.boot_image_max_height,
    )


if __name__ == "__main__":
    main()
