#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR=${1:-"$SCRIPT_DIR/../extra/fonts"}
OUT_DIR=${2:-"$SCRIPT_DIR/../extra/fonts"}

mkdir -p "$OUT_DIR"

need_font() {
	if [ ! -f "$SRC_DIR/$1" ]; then
		echo "missing font: $SRC_DIR/$1" >&2
		exit 1
	fi
}

need_font "DejaVuSans.ttf"
need_font "NotoSansArabic-Regular.ttf"
need_font "NotoSansHebrew-Regular.ttf"
need_font "NotoSansDevanagari-Regular.ttf"
need_font "NotoSansBengali-Regular.ttf"
need_font "NotoSansThai-Regular.ttf"
need_font "NotoNastaliqUrdu-Regular.ttf"
need_font "NotoSansSC-Regular.otf"
need_font "NotoSansJP-Regular.otf"
need_font "NotoSansKR-Regular.otf"

# Small, broad fallback: Greek/Cyrillic/Hebrew/basic Arabic/presentation forms.
# This is intentionally not full Latin; the player's normal UI font handles Latin.
pyftsubset "$SRC_DIR/DejaVuSans.ttf" \
	--output-file="$OUT_DIR/ppa-fallback-core.ttf" \
	--unicodes="U+0370-03FF,U+0400-052F,U+0590-05FF,U+0600-06FF,U+0750-077F,U+08A0-08FF,U+200C-200F,U+202A-202E,U+2066-2069,U+FB1D-FB4F,U+FB50-FDFF,U+FE70-FEFF" \
	--layout-features='*' \
	--glyph-names \
	--symbol-cmap \
	--legacy-cmap \
	--notdef-glyph \
	--notdef-outline \
	--recommended-glyphs \
	--name-IDs='*' \
	--name-legacy \
	--name-languages='*'

# Arabic OpenType shaping fallback.
pyftsubset "$SRC_DIR/NotoSansArabic-Regular.ttf" \
	--output-file="$OUT_DIR/ppa-fallback-arabic.ttf" \
	--unicodes="U+0600-06FF,U+0750-077F,U+08A0-08FF,U+200C-200F,U+202A-202E,U+2066-2069,U+FB50-FDFF,U+FE70-FEFF" \
	--layout-features='*' \
	--glyph-names \
	--symbol-cmap \
	--legacy-cmap \
	--notdef-glyph \
	--notdef-outline \
	--recommended-glyphs \
	--name-IDs='*' \
	--name-legacy \
	--name-languages='*'

# Hebrew.
pyftsubset "$SRC_DIR/NotoSansHebrew-Regular.ttf" \
	--output-file="$OUT_DIR/ppa-fallback-hebrew.ttf" \
	--unicodes="U+0590-05FF,U+FB1D-FB4F,U+200E-200F,U+202A-202E,U+2066-2069" \
	--layout-features='*' \
	--glyph-names \
	--symbol-cmap \
	--legacy-cmap \
	--notdef-glyph \
	--notdef-outline \
	--recommended-glyphs \
	--name-IDs='*' \
	--name-legacy \
	--name-languages='*'

# Devanagari / Hindi.
pyftsubset "$SRC_DIR/NotoSansDevanagari-Regular.ttf" \
	--output-file="$OUT_DIR/ppa-fallback-devanagari.ttf" \
	--unicodes="U+0900-097F,U+1CD0-1CFF,U+A8E0-A8FF,U+200C-200D" \
	--layout-features='*' \
	--glyph-names \
	--symbol-cmap \
	--legacy-cmap \
	--notdef-glyph \
	--notdef-outline \
	--recommended-glyphs \
	--name-IDs='*' \
	--name-legacy \
	--name-languages='*'

# Bengali.
pyftsubset "$SRC_DIR/NotoSansBengali-Regular.ttf" \
	--output-file="$OUT_DIR/ppa-fallback-bengali.ttf" \
	--unicodes="U+0980-09FF,U+200C-200D" \
	--layout-features='*' \
	--glyph-names \
	--symbol-cmap \
	--legacy-cmap \
	--notdef-glyph \
	--notdef-outline \
	--recommended-glyphs \
	--name-IDs='*' \
	--name-legacy \
	--name-languages='*'

# Thai.
pyftsubset "$SRC_DIR/NotoSansThai-Regular.ttf" \
	--output-file="$OUT_DIR/ppa-fallback-thai.ttf" \
	--unicodes="U+0E00-0E7F" \
	--layout-features='*' \
	--glyph-names \
	--symbol-cmap \
	--legacy-cmap \
	--notdef-glyph \
	--notdef-outline \
	--recommended-glyphs \
	--name-IDs='*' \
	--name-legacy \
	--name-languages='*'

# Urdu/Nastaliq. Keep Arabic ranges and OpenType features.
pyftsubset "$SRC_DIR/NotoNastaliqUrdu-Regular.ttf" \
	--output-file="$OUT_DIR/ppa-fallback-urdu.ttf" \
	--unicodes="U+0600-06FF,U+0750-077F,U+08A0-08FF,U+200C-200F,U+202A-202E,U+2066-2069,U+FB50-FDFF,U+FE70-FEFF" \
	--layout-features='*' \
	--glyph-names \
	--symbol-cmap \
	--legacy-cmap \
	--notdef-glyph \
	--notdef-outline \
	--recommended-glyphs \
	--name-IDs='*' \
	--name-legacy \
	--name-languages='*'

# CJK: keep these as full-ish streamed fallback files first.
# Do not memory-load these on PSP.
cp "$SRC_DIR/NotoSansSC-Regular.otf" "$OUT_DIR/ppa-fallback-sc.otf"
cp "$SRC_DIR/NotoSansJP-Regular.otf" "$OUT_DIR/ppa-fallback-jp.otf"
cp "$SRC_DIR/NotoSansKR-Regular.otf" "$OUT_DIR/ppa-fallback-kr.otf"

# Retain original legal metadata and give modified subsets independent names.
python3 "$SCRIPT_DIR/name_font_subsets.py" "$SRC_DIR" "$OUT_DIR"

ls -lh "$OUT_DIR"
