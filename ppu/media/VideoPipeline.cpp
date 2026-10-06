#include "VideoPipeline.hpp"
#include "VideoPipeline.h"
#include "VideoPipelineVfpu.hpp"

#include "../config.h"
#include "../common/mem64.h"
#include "../common/ppa_hardware_profile.h"
#include "../common/ppa_bus_manager.h"
#include "../common/ppa_video_limits.h"
#include "../common/ppa_accel_governor.h"
#include "../common/ppa_scratchpad.h"
#include "../mod/aspect_ratio.h"
#include "../mod/cpu_clock.h"

#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

/* Keep the scalar PSP-panel fallback available if VFPU work is declined. */
#ifndef PPA_ENABLE_CPU_FULL_FRAME_FILTERS
#define PPA_ENABLE_CPU_FULL_FRAME_FILTERS 1
#endif

namespace ppa { namespace video {

static int g_trickplay_frame_drop = 0;

namespace {

static int g_lcd_output = 1;

static inline int clamp8(int value)
{
    return value < 0 ? 0 : (value > 255 ? 255 : value);
}

static inline int red(uint32_t p)   { return (int)(p & 0xffU); }
static inline int green(uint32_t p) { return (int)((p >> 8) & 0xffU); }
static inline int blue(uint32_t p)  { return (int)((p >> 16) & 0xffU); }
static inline int alpha(uint32_t p) { return (int)((p >> 24) & 0xffU); }

static inline uint32_t rgba(int r, int g, int b, int a)
{
    return (uint32_t)clamp8(r) |
           ((uint32_t)clamp8(g) << 8) |
           ((uint32_t)clamp8(b) << 16) |
           ((uint32_t)clamp8(a) << 24);
}

struct ColorMetadata {
    int fullRange;
    int primaries;
    int transfer;
    int matrix;

    ColorMetadata() : fullRange(-1), primaries(-1), transfer(-1), matrix(-1) {}
};

/* Bounded RBSP reader. It strips emulation-prevention bytes while reading, so
 * VUI metadata can be extracted directly from the SPS without a temporary
 * buffer or malformed-stream overread. */
class RbspBitReader {
public:
    RbspBitReader(const uint8_t *data, unsigned int size)
        : data_(data), size_(size), index_(0), current_(0), bitsLeft_(0),
          zeroCount_(0), failed_(data == 0 || size == 0) {}

    bool readBits(unsigned int count, unsigned int &value) {
        unsigned int i;
        value = 0;
        if (count > 32U) {
            failed_ = true;
            return false;
        }
        for (i = 0; i < count; ++i) {
            unsigned int bit;
            if (!readBit(bit))
                return false;
            value = (value << 1) | bit;
        }
        return true;
    }

    bool readUe(unsigned int &value) {
        unsigned int zeros = 0;
        unsigned int bit;
        unsigned int suffix = 0;
        while (zeros < 31U) {
            if (!readBit(bit))
                return false;
            if (bit != 0)
                break;
            ++zeros;
        }
        if (zeros == 31U) {
            failed_ = true;
            return false;
        }
        if (zeros != 0U && !readBits(zeros, suffix))
            return false;
        value = ((1U << zeros) - 1U) + suffix;
        return true;
    }

    bool readSe(int &value) {
        unsigned int code;
        if (!readUe(code))
            return false;
        value = (code & 1U) ? (int)((code + 1U) >> 1) : -(int)(code >> 1);
        return true;
    }

    bool failed() const { return failed_; }

private:
    bool nextByte(uint8_t &value) {
        while (index_ < size_) {
            uint8_t next = data_[index_++];
            if (zeroCount_ >= 2U && next == 0x03U) {
                zeroCount_ = 0;
                continue;
            }
            if (next == 0)
                ++zeroCount_;
            else
                zeroCount_ = 0;
            value = next;
            return true;
        }
        failed_ = true;
        return false;
    }

    bool readBit(unsigned int &value) {
        if (bitsLeft_ == 0U) {
            if (!nextByte(current_))
                return false;
            bitsLeft_ = 8U;
        }
        value = (current_ >> (bitsLeft_ - 1U)) & 1U;
        --bitsLeft_;
        return true;
    }

    const uint8_t *data_;
    unsigned int size_;
    unsigned int index_;
    uint8_t current_;
    unsigned int bitsLeft_;
    unsigned int zeroCount_;
    bool failed_;
};

static bool skipScalingList(RbspBitReader &reader, unsigned int count)
{
    int lastScale = 8;
    int nextScale = 8;
    unsigned int j;
    for (j = 0; j < count; ++j) {
        if (nextScale != 0) {
            int deltaScale;
            if (!reader.readSe(deltaScale))
                return false;
            nextScale = (lastScale + deltaScale + 256) & 255;
        }
        if (nextScale != 0)
            lastScale = nextScale;
    }
    return true;
}

static bool parseAvcVui(const uint8_t *sps, unsigned int size,
                        ColorMetadata &metadata)
{
    unsigned int offset = 0;
    unsigned int profile;
    unsigned int value;
    unsigned int pocType;
    unsigned int i;

    metadata = ColorMetadata();
    if (sps == 0 || size < 4U)
        return false;
    if ((sps[0] & 0x1fU) == 7U)
        offset = 1U;

    RbspBitReader reader(sps + offset, size - offset);
    if (!reader.readBits(8, profile) ||
        !reader.readBits(8, value) ||
        !reader.readBits(8, value) ||
        !reader.readUe(value))
        return false;

    if (profile == 100U || profile == 110U || profile == 122U ||
        profile == 244U || profile == 44U || profile == 83U ||
        profile == 86U || profile == 118U || profile == 128U ||
        profile == 138U || profile == 139U || profile == 134U ||
        profile == 135U) {
        unsigned int chromaFormat;
        if (!reader.readUe(chromaFormat))
            return false;
        if (chromaFormat == 3U && !reader.readBits(1, value))
            return false;
        if (!reader.readUe(value) || !reader.readUe(value) ||
            !reader.readBits(1, value) || !reader.readBits(1, value))
            return false;
        if (value != 0U) {
            const unsigned int lists = chromaFormat == 3U ? 12U : 8U;
            for (i = 0; i < lists; ++i) {
                if (!reader.readBits(1, value))
                    return false;
                if (value != 0U &&
                    !skipScalingList(reader, i < 6U ? 16U : 64U))
                    return false;
            }
        }
    }

    if (!reader.readUe(value) || !reader.readUe(pocType))
        return false;
    if (pocType == 0U) {
        if (!reader.readUe(value))
            return false;
    } else if (pocType == 1U) {
        int signedValue;
        unsigned int cycleCount;
        if (!reader.readBits(1, value) || !reader.readSe(signedValue) ||
            !reader.readSe(signedValue) || !reader.readUe(cycleCount))
            return false;
        if (cycleCount > 255U)
            return false;
        for (i = 0; i < cycleCount; ++i) {
            if (!reader.readSe(signedValue))
                return false;
        }
    } else if (pocType > 2U) {
        return false;
    }

    if (!reader.readUe(value) || !reader.readBits(1, value) ||
        !reader.readUe(value) || !reader.readUe(value) ||
        !reader.readBits(1, value))
        return false;
    if (value == 0U && !reader.readBits(1, value))
        return false;
    if (!reader.readBits(1, value))
        return false; /* direct_8x8_inference_flag */
    if (!reader.readBits(1, value))
        return false; /* frame_cropping_flag */
    if (value != 0U) {
        for (i = 0; i < 4U; ++i) {
            if (!reader.readUe(value))
                return false;
        }
    }
    if (!reader.readBits(1, value) || value == 0U)
        return false; /* vui_parameters_present_flag */

    if (!reader.readBits(1, value))
        return false; /* aspect_ratio_info_present_flag */
    if (value != 0U) {
        unsigned int aspectIdc;
        if (!reader.readBits(8, aspectIdc))
            return false;
        if (aspectIdc == 255U &&
            (!reader.readBits(16, value) || !reader.readBits(16, value)))
            return false;
    }
    if (!reader.readBits(1, value))
        return false; /* overscan_info_present_flag */
    if (value != 0U && !reader.readBits(1, value))
        return false;
    if (!reader.readBits(1, value))
        return false; /* video_signal_type_present_flag */
    if (value == 0U)
        return true;

    if (!reader.readBits(3, value))
        return false; /* video_format */
    if (!reader.readBits(1, value))
        return false;
    metadata.fullRange = value != 0U ? 1 : 0;
    if (!reader.readBits(1, value))
        return false; /* colour_description_present_flag */
    if (value != 0U) {
        if (!reader.readBits(8, value))
            return false;
        metadata.primaries = (int)value;
        if (!reader.readBits(8, value))
            return false;
        metadata.transfer = (int)value;
        if (!reader.readBits(8, value))
            return false;
        metadata.matrix = (int)value;
    }
    return !reader.failed();
}

struct Region {
    int x0;
    int y0;
    int x1;
    int y1;
};

static Region clippedRegion(const FrameView &frame)
{
    Region r;
    r.x0 = frame.viewportX < 0 ? 0 : frame.viewportX;
    r.y0 = frame.viewportY < 0 ? 0 : frame.viewportY;
    r.x1 = frame.viewportX + frame.viewportWidth;
    r.y1 = frame.viewportY + frame.viewportHeight;
    if (r.x1 > (int)frame.width) r.x1 = (int)frame.width;
    if (r.y1 > (int)frame.height) r.y1 = (int)frame.height;
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    return r;
}

static void copyRegion(const uint32_t *src, unsigned int srcPitch,
                       uint32_t *dst, unsigned int dstPitch,
                       const Region &r)
{
    int y;
    for (y = r.y0; y < r.y1; ++y) {
        memcpy(dst + y * dstPitch + r.x0,
               src + y * srcPitch + r.x0,
               (unsigned int)(r.x1 - r.x0) * sizeof(uint32_t));
    }
}

class PspScreenStage {
public:
    PspScreenStage() : limitedEvidence_(0),
                       fullEvidence_(0), preparedValid_(false),
                       preparedFrame_(0), cachedExpandLimited_(false),
                       cachedWidePanel_(false), cachedHdrTransfer_(false),
                       cachedSaturationQ8_(256), cachedContrastQ8_(256) {}

    bool applies(const FrameView &) const { return g_lcd_output != 0; }

    void reset() {
        limitedEvidence_ = 0;
        fullEvidence_ = 0;
        preparedValid_ = false;
        preparedFrame_ = 0;
    }

    void prepare(const uint32_t *src, unsigned int sp,
                 const FrameView &f, const Region &r,
                 bool &expandLimited, bool &widePanel,
                 bool &hdrTransfer, int &saturationQ8,
                 int &contrastQ8) {
        int minY = 255;
        int maxY = 0;
        unsigned int samples = 0;
        unsigned int limitedSamples = 0;
        unsigned int darkEndpointSamples = 0;
        unsigned int brightEndpointSamples = 0;
        unsigned int outsideLimitedSamples = 0;
        int y;
        int x;

        if (preparedValid_ && preparedFrame_ == f.frameNumber) {
            expandLimited = cachedExpandLimited_;
            widePanel = cachedWidePanel_;
            hdrTransfer = cachedHdrTransfer_;
            saturationQ8 = cachedSaturationQ8_;
            contrastQ8 = cachedContrastQ8_;
            return;
        }

        widePanel =
            (f.pspModel == 2 || f.pspModel == 3 || f.pspModel == 4 ||
             f.pspModel == 6 || f.pspModel == 7 || f.pspModel == 8);
        hdrTransfer =
            (f.transferCharacteristics == 16 ||
             f.transferCharacteristics == 18);

        for (y = r.y0; y < r.y1; y += 8) {
            for (x = r.x0; x < r.x1; x += 8) {
                uint32_t pixel = src[y * sp + x];
                int rr = red(pixel);
                int gg = green(pixel);
                int bb = blue(pixel);
                int value;
                value = (77 * rr + 150 * gg + 29 * bb + 128) >> 8;
                if (value < minY) minY = value;
                if (value > maxY) maxY = value;
                if (value >= 14 && value <= 238)
                    ++limitedSamples;
                if (value >= 14 && value <= 28)
                    ++darkEndpointSamples;
                if (value >= 227 && value <= 240)
                    ++brightEndpointSamples;
                if (value <= 8 || value >= 247)
                    ++outsideLimitedSamples;
                ++samples;
            }
        }

        {
            bool endpointEvidence = false;
            bool boundedEvidence = false;
            if (samples != 0) {
                const unsigned int endpointMinimum = samples / 200U + 1U;
                const bool darkPresent = darkEndpointSamples >= endpointMinimum;
                const bool brightPresent = brightEndpointSamples >= endpointMinimum;
                endpointEvidence = f.fullRange == 0 ?
                    (darkPresent || brightPresent) :
                    (darkPresent && brightPresent);
                boundedEvidence = limitedSamples * 100U / samples >= 98U &&
                    outsideLimitedSamples * 200U <= samples &&
                    minY >= 10 && maxY <= 242;
            }
            if (endpointEvidence && boundedEvidence) {
                if (limitedEvidence_ < 16) ++limitedEvidence_;
                if (fullEvidence_ > 0) --fullEvidence_;
            }
            else {
                if (fullEvidence_ < 16) ++fullEvidence_;
                if (limitedEvidence_ > 0) --limitedEvidence_;
            }
        }

        if (f.fullRange == 1) {
            expandLimited = false;
        }
        else {
            const int evidenceNeeded = f.fullRange == 0 ? 6 : 10;
            expandLimited = limitedEvidence_ >= evidenceNeeded &&
                            fullEvidence_ < 4;
        }

        if (widePanel) {
            saturationQ8 = 250;
            contrastQ8 = 258;
        }
        else {
            saturationQ8 = 274;
            contrastQ8 = 266;
        }
        if (f.colorPrimaries == 9 ||
            f.matrixCoefficients == 9 || f.matrixCoefficients == 10) {
            saturationQ8 = widePanel ? 232 : 244;
        }
        else if (f.colorPrimaries == 1 || f.matrixCoefficients == 1) {
            saturationQ8 = widePanel ? 250 : 270;
        }
        else if (f.colorPrimaries == 5 || f.colorPrimaries == 6 ||
                 f.matrixCoefficients == 5 || f.matrixCoefficients == 6 ||
                 f.matrixCoefficients == 7) {
            saturationQ8 += widePanel ? 2 : 0;
        }

        preparedValid_ = true;
        preparedFrame_ = f.frameNumber;
        cachedExpandLimited_ = expandLimited;
        cachedWidePanel_ = widePanel;
        cachedHdrTransfer_ = hdrTransfer;
        cachedSaturationQ8_ = saturationQ8;
        cachedContrastQ8_ = contrastQ8;
    }

    void run(const uint32_t *src, unsigned int sp,
             uint32_t *dst, unsigned int dp,
             const FrameView &f, const Region &r) {
        bool expandLimited;
        bool widePanel;
        bool hdrTransfer;
        int saturationQ8;
        int contrastQ8;
        int y;
        int x;

        prepare(src, sp, f, r,
                expandLimited, widePanel, hdrTransfer,
                saturationQ8, contrastQ8);

        for (y = r.y0; y < r.y1; ++y) {
            for (x = r.x0; x < r.x1; ++x) {
                uint32_t pixel = src[y * sp + x];
                int rr = red(pixel);
                int gg = green(pixel);
                int bb = blue(pixel);
                int yy;

                if (expandLimited) {
                    rr = ((rr - 16) * 298 + 128) >> 8;
                    gg = ((gg - 16) * 298 + 128) >> 8;
                    bb = ((bb - 16) * 298 + 128) >> 8;
                }
                rr = ((rr - 128) * contrastQ8 + 128 * 256 + 128) >> 8;
                gg = ((gg - 128) * contrastQ8 + 128 * 256 + 128) >> 8;
                bb = ((bb - 128) * contrastQ8 + 128 * 256 + 128) >> 8;
                yy = (77 * rr + 150 * gg + 29 * bb + 128) >> 8;
                rr = yy + (((rr - yy) * saturationQ8 + 128) >> 8);
                gg = yy + (((gg - yy) * saturationQ8 + 128) >> 8);
                bb = yy + (((bb - yy) * saturationQ8 + 128) >> 8);

                if (hdrTransfer) {
                    int mappedY = yy;
                    if (yy < 96)
                        mappedY += (96 - yy) >> 4;
                    else if (yy > 192)
                        mappedY = 192 + ((yy - 192) * 3 >> 2);
                    rr += mappedY - yy;
                    gg += mappedY - yy;
                    bb += mappedY - yy;
                    yy = mappedY;
                }
                if (!widePanel && yy < 96) {
                    int lift = (96 - yy) / 12;
                    rr += lift;
                    gg += lift;
                    bb += lift;
                }
                if (widePanel && y > r.y0 && y + 1 < r.y1) {
                    uint32_t up = src[(y - 1) * sp + x];
                    uint32_t down = src[(y + 1) * sp + x];
                    rr = (6 * rr + red(up) + red(down) + 4) >> 3;
                    bb = (6 * bb + blue(up) + blue(down) + 4) >> 3;
                }
                dst[y * dp + x] = rgba(rr, gg, bb, alpha(pixel));
            }
        }
    }

private:
    int limitedEvidence_;
    int fullEvidence_;
    bool preparedValid_;
    unsigned int preparedFrame_;
    bool cachedExpandLimited_;
    bool cachedWidePanel_;
    bool cachedHdrTransfer_;
    int cachedSaturationQ8_;
    int cachedContrastQ8_;
};

} // namespace

struct VideoPipeline::Impl {
    bool pspScreenEnabled;
    bool extremeBatterySaver;
    uint32_t *scratch;
    unsigned int scratchPixels;
    VfpuStageAccelerator vfpuAccelerator;
    StageAccelerator *accelerator;
    ColorMetadata color;
    PspScreenStage pspScreen;

    Impl() : pspScreenEnabled(false), extremeBatterySaver(false),
             scratch(0), scratchPixels(0), accelerator(&vfpuAccelerator) {}
    ~Impl() { if (scratch) free_64(scratch); }
};

VideoPipeline &VideoPipeline::instance()
{
    static VideoPipeline pipeline;
    return pipeline;
}

VideoPipeline::VideoPipeline() : impl_(0)
{
    static Impl storage;
    impl_ = &storage;
}

VideoPipeline::~VideoPipeline() {}

void VideoPipeline::reloadConfiguration()
{
    Config *config = Config::getInstance();
    impl_->extremeBatterySaver = config ?
        config->getBooleanValue("config/player/extreme_battery_saver", false) : false;
    impl_->pspScreenEnabled = config ?
        config->getBooleanValue("config/video_pipeline/optimize_psp_screen", false) : false;
}

void VideoPipeline::shutdown()
{
    g_trickplay_frame_drop = 0;
    if (impl_->accelerator)
        impl_->accelerator->shutdown();
    impl_->pspScreen.reset();
    impl_->color = ColorMetadata();
    if (impl_->scratch) {
        free_64(impl_->scratch);
        impl_->scratch = 0;
        impl_->scratchPixels = 0;
    }
}

void VideoPipeline::resetSession()
{
    g_trickplay_frame_drop = 0;
    if (impl_->accelerator) {
        /* Output mode is finalized later by ppa_gu_start(). */
        if (!impl_->extremeBatterySaver && impl_->pspScreenEnabled &&
            impl_->accelerator->supports(StagePspScreen))
            impl_->accelerator->resetSession(StageMaskPspScreen);
        else
            impl_->accelerator->shutdown();
    }
    impl_->pspScreen.reset();
    impl_->color = ColorMetadata();
}

void VideoPipeline::leavePlayback()
{
    if (impl_->accelerator)
        impl_->accelerator->shutdown();
    else
        ppa_scratchpad_set_phase(PPA_SCRATCHPAD_PHASE_MENU);
}

void VideoPipeline::setAvcSps(const uint8_t *sps, unsigned int size)
{
    ColorMetadata parsed;
    if (size > 4096U || !parseAvcVui(sps, size, parsed))
        parsed = ColorMetadata();
    impl_->color = parsed;
}

bool VideoPipeline::extremeBatterySaverEnabled() const
{
    return impl_->extremeBatterySaver;
}

bool VideoPipeline::configured() const
{
    if (impl_->extremeBatterySaver || !impl_->pspScreenEnabled || !g_lcd_output)
        return false;
    if (impl_->accelerator && impl_->accelerator->supports(StagePspScreen))
        return true;
#if PPA_ENABLE_CPU_FULL_FRAME_FILTERS
    return true;
#else
    return false;
#endif
}

bool VideoPipeline::enabled() const
{
    return configured() && cpu_clock_auto_optional_work_allowed();
}

bool VideoPipeline::vfpuRequired() const
{
    return !impl_->extremeBatterySaver && g_lcd_output &&
           impl_->pspScreenEnabled && impl_->accelerator &&
           impl_->accelerator->supports(StagePspScreen);
}

void VideoPipeline::setAccelerator(StageAccelerator *a) { impl_->accelerator=a; }

void setLcdOutput(bool enabled) { g_lcd_output = enabled ? 1 : 0; }
void setTrickplayFrameDrop(bool enabled) { g_trickplay_frame_drop = enabled ? 1 : 0; }
bool trickplayFrameDropAllowed() { return g_trickplay_frame_drop != 0; }
bool shouldDropTrickplayFrame(unsigned int level, unsigned int sequence)
{
    if (!g_trickplay_frame_drop || level <= 1U)
        return false;
    if (level > 4U)
        level = 4U;
    return (sequence % level) != 0U;
}

bool VideoPipeline::process(const FrameView &frame)
{
    FrameView working = frame;
    working.fullRange = impl_->color.fullRange;
    working.colorPrimaries = impl_->color.primaries;
    working.transferCharacteristics = impl_->color.transfer;
    working.matrixCoefficients = impl_->color.matrix;
    Region region = clippedRegion(working);

    if (!enabled() || !working.pixels ||
        working.width == 0 || working.height == 0 ||
        working.width > PPA_VIDEO_LCD_WIDTH ||
        working.height > PPA_VIDEO_LCD_HEIGHT ||
        working.pitch < working.width || region.x1 - region.x0 < 3 ||
        region.y1 - region.y0 < 3)
        return false;

    struct ppa_bus_token busToken = ppa_bus_begin(PPA_BUS_POSTPROCESS);
    const uint64_t startedUs = cpu_clock_auto_now_us();
    bool completed = false;
    if (impl_->accelerator &&
        impl_->accelerator->supportsColorGroup(StageMaskPspScreen)) {
        ColorTransformParameters parameters;
        bool expandLimited, widePanel, hdrTransfer;
        int saturationQ8, contrastQ8;
        impl_->pspScreen.prepare(working.pixels, working.pitch, working, region,
                                expandLimited, widePanel, hdrTransfer,
                                saturationQ8, contrastQ8);
        memset(&parameters, 0, sizeof(parameters));
        parameters.stageMask = StageMaskPspScreen;
        parameters.x0 = region.x0;
        parameters.y0 = region.y0;
        parameters.x1 = region.x1;
        parameters.y1 = region.y1;
        parameters.expandLimited = expandLimited ? 1 : 0;
        parameters.widePanel = widePanel ? 1 : 0;
        parameters.hdrTransfer = hdrTransfer ? 1 : 0;
        parameters.saturationQ8 = saturationQ8;
        parameters.contrastQ8 = contrastQ8;
        completed = impl_->accelerator->processColorGroup(working, parameters);
    }
#if PPA_ENABLE_CPU_FULL_FRAME_FILTERS
    if (!completed) {
        const unsigned int requiredPixels = working.width * working.height;
        if (!impl_->scratch || impl_->scratchPixels < requiredPixels) {
            uint32_t *replacement = (uint32_t *)malloc_64(
                requiredPixels * sizeof(uint32_t));
            if (!replacement) {
                ppa_bus_end(&busToken);
                return false;
            }
            if (impl_->scratch) free_64(impl_->scratch);
            impl_->scratch = replacement;
            impl_->scratchPixels = requiredPixels;
        }
        const uint64_t scalarStarted = cpu_clock_auto_now_us();
        /* Panel row mixing needs the original neighboring pixels. Preserve the
         * source until this single pass is complete, then publish its region. */
        impl_->pspScreen.run(working.pixels, working.pitch, impl_->scratch,
                            working.width, working, region);
        copyRegion(impl_->scratch, working.width, working.pixels,
                   working.pitch, region);
        ppa_accel_governor_observe_scalar_fallback(
            (unsigned int)(cpu_clock_auto_now_us() - scalarStarted),
            (unsigned int)(region.x1 - region.x0) *
            (unsigned int)(region.y1 - region.y0));
        completed = true;
    }
#endif
    if (completed) {
        const uint64_t endedUs = cpu_clock_auto_now_us();
        if (endedUs >= startedUs)
            cpu_clock_auto_on_postprocess_us((unsigned int)(endedUs - startedUs));
    }
    ppa_bus_end(&busToken);
    return completed;
}

} }

extern "C" {

void ppa_video_pipeline_reload_config(void)
{
    ppa::video::VideoPipeline::instance().reloadConfiguration();
}

void ppa_video_pipeline_reset(void)
{
    ppa::video::VideoPipeline::instance().resetSession();
}

void ppa_video_pipeline_leave_playback(void)
{
    ppa::video::VideoPipeline::instance().leavePlayback();
}

void ppa_video_pipeline_shutdown(void)
{
    ppa::video::VideoPipeline::instance().shutdown();
}

void ppa_video_pipeline_set_lcd_output(int lcd_output)
{
    ppa::video::setLcdOutput(lcd_output != 0);
}

void ppa_video_pipeline_set_avc_sps(const unsigned char *sps,
                                    unsigned int size)
{
    ppa::video::VideoPipeline::instance().setAvcSps(sps, size);
}

int ppa_video_pipeline_configured(void)
{
    return ppa::video::VideoPipeline::instance().configured() ? 1 : 0;
}

int ppa_video_pipeline_enabled(void)
{
    return ppa::video::VideoPipeline::instance().enabled() ? 1 : 0;
}

int ppa_video_pipeline_vfpu_required(void)
{
    int required =
        ppa::video::VideoPipeline::instance().vfpuRequired() ? 1 : 0;
    return required;
}

int ppa_video_pipeline_extreme_battery_saver_enabled(void)
{
    return ppa::video::VideoPipeline::instance().extremeBatterySaverEnabled() ? 1 : 0;
}

void ppa_video_pipeline_set_trickplay_frame_drop(int enabled)
{
    ppa::video::setTrickplayFrameDrop(enabled != 0);
}

int ppa_video_pipeline_trickplay_frame_drop_allowed(void)
{
    return ppa::video::trickplayFrameDropAllowed() ? 1 : 0;
}

int ppa_video_pipeline_should_drop_trickplay_frame(unsigned int level,
                                                    unsigned int sequence)
{
    return ppa::video::shouldDropTrickplayFrame(level, sequence) ? 1 : 0;
}

int ppa_video_pipeline_process(void *frame_buffer,
                               unsigned int output_pitch,
                               unsigned int source_width,
                               unsigned int source_height,
                               unsigned int aspect_ratio,
                               unsigned int zoom,
                               unsigned int frame_number)
{
    ppa::video::FrameView frame;
    const unsigned int ar = aspect_ratio < 4 ? aspect_ratio : 0;
    int viewportWidth = (int)(aspect_ratios[ar].psp_width * zoom / 100U);
    int viewportHeight = (int)(aspect_ratios[ar].psp_height * zoom / 100U);
    if (viewportWidth <= 0) viewportWidth = (int)PPA_VIDEO_LCD_WIDTH;
    if (viewportHeight <= 0) viewportHeight = (int)PPA_VIDEO_LCD_HEIGHT;
    frame.pixels = (uint32_t *)frame_buffer;
    frame.pitch = output_pitch;
    frame.width = PPA_VIDEO_LCD_WIDTH;
    frame.height = PPA_VIDEO_LCD_HEIGHT;
    frame.sourceWidth = source_width;
    frame.sourceHeight = source_height;
    frame.viewportWidth = viewportWidth;
    frame.viewportHeight = viewportHeight;
    frame.viewportX = ((int)PPA_VIDEO_LCD_WIDTH - viewportWidth) / 2;
    frame.viewportY = ((int)PPA_VIDEO_LCD_HEIGHT - viewportHeight) / 2;
    frame.frameNumber = frame_number;
    frame.pspModel = ppa_hardware_model();
    frame.fullRange = -1;
    frame.colorPrimaries = -1;
    frame.transferCharacteristics = -1;
    frame.matrixCoefficients = -1;
    return ppa::video::VideoPipeline::instance().process(frame) ? 1 : 0;
}

}
