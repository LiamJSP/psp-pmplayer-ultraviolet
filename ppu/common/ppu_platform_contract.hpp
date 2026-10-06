#ifndef PPU_PLATFORM_CONTRACT_HPP
#define PPU_PLATFORM_CONTRACT_HPP

#include "ppu_boost.hpp"
#include <boost/static_assert.hpp>
#include <boost/type_traits/is_signed.hpp>
#include <boost/type_traits/is_unsigned.hpp>
#include <boost/type_traits/is_pod.hpp>
#include <limits.h>
#include <float.h>
#include <stdint.h>
#include <psptypes.h>

#include "graphics.h"
#include "ppa_cache.h"
#include "ppa_memory.h"
#include "ppa_scratchpad.h"
#include "../mod/audio_format.h"

#if PPU_ENABLE_OPUS
#include <opus/opus.h>
#include <boost/type_traits/is_same.hpp>
BOOST_STATIC_ASSERT_MSG((boost::is_same<opus_int16, int16_t>::value),
    "Opus and the PCM stream must agree on sample pointer types");
BOOST_STATIC_ASSERT_MSG(sizeof(opus_int32) == 4,
    "Opus packet length/rate ABI requires 32-bit integers");
#endif

/* Compile-time requirements of the existing C/assembly paths. This header
 * emits no objects, constructors or runtime checks. Keep it in main.cpp so
 * C libraries and kernel PRXs retain their original language and flags. */
namespace ppu_platform_contract {
BOOST_STATIC_ASSERT_MSG(CHAR_BIT == 8, "Media parsers require 8-bit bytes");
BOOST_STATIC_ASSERT_MSG(sizeof(void *) == 4, "PSP address aliases require 32-bit pointers");
BOOST_STATIC_ASSERT_MSG(sizeof(uintptr_t) == sizeof(void *), "Pointer arithmetic must not truncate");
BOOST_STATIC_ASSERT_MSG(sizeof(int) == 4, "PSP C and assembly integer ABI");
BOOST_STATIC_ASSERT_MSG(sizeof(float) == 4, "VFPU lanes require 32-bit floats");
BOOST_STATIC_ASSERT_MSG(FLT_RADIX == 2 && FLT_MANT_DIG >= 24,
    "PCM conversion requires exact binary32 representation of s16 samples");
BOOST_STATIC_ASSERT_MSG(sizeof(short) == 2, "libsamplerate PCM API requires 16-bit shorts");
BOOST_STATIC_ASSERT_MSG(PPU_AUDIO_RATE == 44100U,
    "PSP linear SRC phase denominator must match the output sample rate");
BOOST_STATIC_ASSERT_MSG(PPU_AUDIO_BLOCK >= 64U && PPU_AUDIO_BLOCK <= 65472U &&
    (PPU_AUDIO_BLOCK % 64U) == 0, "PSP audio channel block size contract");
BOOST_STATIC_ASSERT_MSG(PPU_AUDIO_MAX_DECODE_FRAMES >= 5760U,
    "Opus output workspace must hold a 120-ms packet at 48 kHz");
BOOST_STATIC_ASSERT_MSG(boost::is_pod<ppu_audio_format>::value,
    "Container audio configuration must remain a C value type");
BOOST_STATIC_ASSERT_MSG(sizeof(Color) == 4, "RGBA surfaces require four-byte pixels");
BOOST_STATIC_ASSERT_MSG(boost::is_unsigned<Color>::value, "RGBA bit operations require unsigned pixels");
BOOST_STATIC_ASSERT_MSG(sizeof(SceOff) == 8, "Large-file seeks require 64-bit offsets");
BOOST_STATIC_ASSERT_MSG(boost::is_signed<SceOff>::value, "Seek failures use negative offsets");

BOOST_STATIC_ASSERT_MSG(PPA_CACHE_LINE != 0 &&
    (PPA_CACHE_LINE & (PPA_CACHE_LINE - 1U)) == 0, "Cache rounding needs a power of two");
BOOST_STATIC_ASSERT_MSG((PPA_SCRATCHPAD_BASE_ADDRESS % PPA_CACHE_LINE) == 0,
    "Scratchpad base must retain cache-line alignment");
BOOST_STATIC_ASSERT_MSG(PPA_SCRATCHPAD_VIDEO_OFFSET + PPA_SCRATCHPAD_VIDEO_BYTES <=
    PPA_SCRATCHPAD_AUDIO_OFFSET, "Video scratch must not overlap audio");
BOOST_STATIC_ASSERT_MSG(PPA_SCRATCHPAD_AUDIO_OFFSET + PPA_SCRATCHPAD_AUDIO_BYTES <=
    PPA_SCRATCHPAD_CONTROL_OFFSET, "Audio scratch must not overlap control");
BOOST_STATIC_ASSERT_MSG(PPA_SCRATCHPAD_CONTROL_OFFSET + PPA_SCRATCHPAD_CONTROL_BYTES <=
    PPA_SCRATCHPAD_TOTAL_BYTES, "Scratch partitions must fit physical SRAM");
BOOST_STATIC_ASSERT_MSG(PPA_MEMORY_EXTENDED_SURFACE_BASE +
    PPA_MEMORY_EXTENDED_RESERVED_BYTES == PPA_MEMORY_EXTENDED_ARENA_SAFE_BASE,
    "Large-object arena must begin after the fixed surface reservation");
/* One RGB workspace precedes the eight display surfaces. */
BOOST_STATIC_ASSERT_MSG((PPA_MEMORY_EXTENDED_FRAME_COUNT + 1U) *
    PPA_MEMORY_EXTENDED_FRAME_BYTES <= PPA_MEMORY_EXTENDED_RESERVED_BYTES,
    "Fixed video surfaces must fit their reservation");
}

#endif
