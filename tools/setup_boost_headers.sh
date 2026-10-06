#!/bin/sh
# Fetch/extract headers only. Never invokes a compiler, b2, make or tests.
# Optional argument: an already downloaded official boost_1_85_0.tar.bz2.
set -eu

if [ "$#" -gt 1 ]; then
    printf '%s\n' "Usage: sh tools/setup_boost_headers.sh [boost_1_85_0.tar.bz2]" >&2
    exit 1
fi

ppu_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ppu_boost_name=boost_1_85_0
ppu_boost_dest="$ppu_root/third_party/$ppu_boost_name"
ppu_boost_sha=7009fe1faa1697476bdc7027703a2badb84e849b7b0baad5086b087b971f8617

# Never merge into, delete or replace somebody else's header installation.
if [ -e "$ppu_boost_dest" ] || [ -L "$ppu_boost_dest" ]; then
    printf 'Destination already exists: %s\nUse it as-is, or move it aside manually before rerunning setup.\n' "$ppu_boost_dest" >&2
    exit 1
fi

for ppu_tool in tar bzip2 sha256sum mktemp; do
    if ! command -v "$ppu_tool" >/dev/null 2>&1; then
        printf 'Required host utility is missing: %s\n' "$ppu_tool" >&2
        exit 1
    fi
done
if [ "$#" -eq 0 ] && ! command -v curl >/dev/null 2>&1; then
    printf '%s\n' 'curl is required to download Boost; alternatively pass the official archive path.' >&2
    exit 1
fi

mkdir -p -- "$ppu_root/third_party"
ppu_boost_work=$(mktemp -d "$ppu_root/third_party/.ppu-boost-setup.XXXXXX")
# Cleanup is restricted to the directory created above, never the destination.
trap 'rm -rf -- "$ppu_boost_work"' 0
trap 'exit 1' HUP INT TERM

if [ "$#" -eq 1 ]; then
    ppu_boost_archive=$1
else
    ppu_boost_archive="$ppu_boost_work/$ppu_boost_name.tar.bz2"
    curl --fail --location --proto '=https' --proto-redir '=https' --output "$ppu_boost_archive" "https://archives.boost.io/release/1.85.0/source/$ppu_boost_name.tar.bz2"
fi

if ! printf '%s  %s\n' "$ppu_boost_sha" "$ppu_boost_archive" | sha256sum --check --status; then
    printf '%s\n' 'Boost archive checksum mismatch or unreadable archive; nothing installed.' >&2
    exit 1
fi

# Keep the complete transitive header tree and original license. No Boost
# library source, documentation, build system or host binaries are installed.
tar --extract --bzip2 --file "$ppu_boost_archive" --directory "$ppu_boost_work" "$ppu_boost_name/boost" "$ppu_boost_name/LICENSE_1_0.txt"
if [ -e "$ppu_boost_dest" ] || [ -L "$ppu_boost_dest" ]; then
    printf '%s\n' 'Destination appeared during setup; refusing to replace it.' >&2
    exit 1
fi
mv -T --no-clobber -- "$ppu_boost_work/$ppu_boost_name" "$ppu_boost_dest"
if [ -d "$ppu_boost_work/$ppu_boost_name" ]; then
    printf '%s\n' 'Destination appeared during installation; no existing files were replaced.' >&2
    exit 1
fi
printf 'Installed Boost 1.85.0 headers at %s\nNo compilation or tests were performed.\n' "$ppu_boost_dest"
