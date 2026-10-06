#!/usr/bin/env python3
"""Give generated, modified fonts independent internal names and a change log.

Does not touch original fonts. Copyright, trademark and license records remain
intact. Called only by the developer's subset build; not run during the audit.
"""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
from fontTools.ttLib import TTFont


SUBSETS = (
    ("DejaVuSans.ttf", "core"),
    ("NotoSansArabic-Regular.ttf", "arabic"),
    ("NotoSansHebrew-Regular.ttf", "hebrew"),
    ("NotoSansDevanagari-Regular.ttf", "devanagari"),
    ("NotoSansBengali-Regular.ttf", "bengali"),
    ("NotoSansThai-Regular.ttf", "thai"),
    ("NotoNastaliqUrdu-Regular.ttf", "urdu"),
)


def main():
    if len(sys.argv) != 3:
        raise ValueError("Usage: name_font_subsets.py ORIGINAL_FONT_DIR GENERATED_FONT_DIR")
    source, output = (Path(value).resolve() for value in sys.argv[1:])
    log = [
        "PPU subtitle font modifications — 2026-10-06 workflow",
        "Subsets generated with pyftsubset; layout features and original notices retained.",
        "Internal family/full/PostScript names changed to PPU Fallback names.",
        "These modified fonts remain under their original font licenses.",
        "See licenses/fonts/FONT-NOTICES.txt, DejaVu-LICENSE.txt and OFL-1.1.txt.",
        "CJK ppa-fallback-*.otf copies are unmodified and retain their original names.",
        "",
    ]
    for original, suffix in SUBSETS:
        original_path = source / original
        path = output / ("ppa-fallback-" + suffix + ".ttf")
        if path == original_path or path.is_symlink():
            raise ValueError("Refusing to replace an original or symlinked font")
        font = TTFont(path, recalcTimestamp=False)
        temporary = None
        try:
            # This workflow uses static TrueType outlines only. Do not leave
            # an unrenamed CFF or variable-font internal name behind silently.
            if "CFF " in font or "CFF2" in font or "fvar" in font:
                raise ValueError("Unsupported subset naming tables: " + path.name)
            family = "PPU Fallback " + suffix.title()
            psname = "PPUFallback" + suffix.title() + "-Regular"
            values = {1: family, 2: "Regular", 3: "PPU-Subset-" + psname,
                      4: family + " Regular", 6: psname, 16: family,
                      17: "Regular", 18: family + " Regular", 21: family,
                      22: "Regular", 25: "PPUFallback" + suffix.title()}
            table = font["name"]
            # Replace *every* localized variant of naming records. Preserve
            # IDs 0/7/8/9/13/14 (original attribution, trademarks and licenses).
            table.names = [entry for entry in table.names if entry.nameID not in values]
            for name_id, value in values.items():
                table.setName(value, name_id, 3, 1, 0x409)
                table.setName(value, name_id, 1, 0, 0)
            if "DSIG" in font:
                del font["DSIG"]
            fd, temporary = tempfile.mkstemp(prefix=".ppu-subset-", dir=str(output))
            os.close(fd)
            font.save(temporary)
            font.close()
            os.replace(temporary, path)
            temporary = None
        finally:
            font.close()
            if temporary is not None:
                os.unlink(temporary)  # Only this invocation's private temp file.
        log.extend([
            path.name + " <- " + original,
            "Original SHA256: " + hashlib.sha256(original_path.read_bytes()).hexdigest(),
            "Modified SHA256: " + hashlib.sha256(path.read_bytes()).hexdigest(), "",
        ])
    (output / "ppa-fallback-NOTICE.txt").write_text("\n".join(log), encoding="utf-8")


if __name__ == "__main__":
    main()
