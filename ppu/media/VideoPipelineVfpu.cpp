#include "VideoPipelineVfpu.hpp"
#include "../common/ppu_boost.hpp"
#include <boost/static_assert.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <boost/type_traits/is_pod.hpp>

#include "../common/ppa_accel_governor.h"
#include "../common/ppa_scratchpad.h"
#include "../mod/cpu_clock.h"

#include <pspkernel.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef PPA_ENABLE_VFPU_ACCEL
#define PPA_ENABLE_VFPU_ACCEL 1
#endif
#ifndef PPA_ENABLE_VFPU_VIDEO
#define PPA_ENABLE_VFPU_VIDEO 1
#endif
#ifndef PPA_ENABLE_SCRATCHPAD_ACCEL
#define PPA_ENABLE_SCRATCHPAD_ACCEL 1
#endif
#ifndef PPA_ACCEL_VERIFY
#define PPA_ACCEL_VERIFY 0
#endif
#ifndef PPA_ACCEL_VERIFY_MAX_DELTA
#define PPA_ACCEL_VERIFY_MAX_DELTA 3U
#endif

namespace ppa { namespace video {
namespace {

/* Debug/PSPLink builds use -O0. Keep tiny pixel/classification helpers in the
 * hot loop as real inline operations there too; otherwise function-call
 * overhead can dominate sparse VFPU work and falsely pressure the clock
 * governor. */
#if defined(__GNUC__)
#define PPA_VFPU_HOT_INLINE static inline __attribute__((always_inline))
#else
#define PPA_VFPU_HOT_INLINE static inline
#endif

static const unsigned int kMaxWidth = 480U;
static const unsigned int kRowBytes = kMaxWidth * sizeof(uint32_t);
static const unsigned int kSampleWidth = 32U;
static const unsigned int kSampleHeight = 6U;
static const unsigned int kSampleRows = kSampleHeight + 2U;
static const unsigned int kCalibrationRepeats = 8U;
static const unsigned int kSampleSourceBytes =
    kSampleWidth * kSampleRows * sizeof(uint32_t);
static const unsigned int kSampleOutputBytes =
    kSampleWidth * kSampleHeight * sizeof(uint32_t);

PPA_VFPU_HOT_INLINE int clamp8(int value)
{
    return value < 0 ? 0 : (value > 255 ? 255 : value);
}
PPA_VFPU_HOT_INLINE int red(uint32_t p) { return (int)(p & 0xffU); }
PPA_VFPU_HOT_INLINE int green(uint32_t p) { return (int)((p >> 8) & 0xffU); }
PPA_VFPU_HOT_INLINE int blue(uint32_t p) { return (int)((p >> 16) & 0xffU); }
PPA_VFPU_HOT_INLINE int alpha(uint32_t p) { return (int)((p >> 24) & 0xffU); }
PPA_VFPU_HOT_INLINE uint32_t rgba(int r, int g, int b, int a)
{
    return (uint32_t)clamp8(r) |
           ((uint32_t)clamp8(g) << 8) |
           ((uint32_t)clamp8(b) << 16) |
           ((uint32_t)clamp8(a) << 24);
}
struct __attribute__((aligned(16))) VfpuBatch {
    int inR[4];
    int inG[4];
    int inB[4];
    int outR[4];
    int outG[4];
    int outB[4];
    int outY[4];
    float sixteen[4];
    float limitedScale[4];
    float center[4];
    float contrast[4];
    float lumaR[4];
    float lumaG[4];
    float lumaB[4];
    float saturation[4];
};

/* These are the existing offsets, shared by the mapper and compile-time
 * budget checks. Keep the 768-byte slot to preserve scratchpad placement. */
static const unsigned int kBatchStorageBytes = 768U;
static const unsigned int kBatchOffset = 2U * kRowBytes;
static const unsigned int kSampleSourceOffset = kBatchOffset + kBatchStorageBytes;
static const unsigned int kSampleScalarOffset = kSampleSourceOffset + kSampleSourceBytes;
static const unsigned int kSampleVfpuOffset = kSampleScalarOffset + kSampleOutputBytes;
static const unsigned int kScratchRequired = kSampleVfpuOffset + kSampleOutputBytes;

BOOST_STATIC_ASSERT_MSG(boost::is_pod<VfpuBatch>::value,
    "Scratch-mapped VFPU storage must not require C++ construction");
BOOST_STATIC_ASSERT_MSG(boost::alignment_of<VfpuBatch>::value >= 16,
    "VFPU quad loads require aligned batch storage");
BOOST_STATIC_ASSERT_MSG(sizeof(VfpuBatch) <= kBatchStorageBytes,
    "VFPU batch must fit before the sample buffers");
BOOST_STATIC_ASSERT_MSG((PPA_SCRATCHPAD_BASE_ADDRESS + PPA_SCRATCHPAD_VIDEO_OFFSET +
    kBatchOffset) % boost::alignment_of<VfpuBatch>::value == 0,
    "Scratch-mapped batch must honor its actual type alignment");
BOOST_STATIC_ASSERT_MSG(kRowBytes % 16U == 0 && kSampleSourceOffset % 16U == 0 &&
    kSampleScalarOffset % 16U == 0 && kSampleVfpuOffset % 16U == 0,
    "Video scratch rows and samples must remain quad aligned");
BOOST_STATIC_ASSERT_MSG(kScratchRequired <= PPA_SCRATCHPAD_VIDEO_BYTES,
    "Video workspaces must fit their lease without entering audio scratch");

struct ScratchLayout {
    uint32_t *previousRow;
    uint32_t *currentRow;
    VfpuBatch *batch;
    uint32_t *sampleSource;
    uint32_t *sampleScalar;
    uint32_t *sampleVfpu;
};

static bool mapScratch(void *base, unsigned int bytes, ScratchLayout &layout)
{
    unsigned char *p = (unsigned char *)base;

    if (base == 0 || bytes < kScratchRequired)
        return false;
    layout.previousRow = (uint32_t *)(p + 0U);
    layout.currentRow = (uint32_t *)(p + kRowBytes);
    layout.batch = (VfpuBatch *)(p + kBatchOffset);
    layout.sampleSource = (uint32_t *)(p + kSampleSourceOffset);
    layout.sampleScalar = (uint32_t *)(p + kSampleScalarOffset);
    layout.sampleVfpu = (uint32_t *)(p + kSampleVfpuOffset);
    return true;
}

static void fill4(float *v, float value)
{
    v[0] = value; v[1] = value; v[2] = value; v[3] = value;
}

static void prepareBatchConstants(VfpuBatch *b,
                                  const ColorTransformParameters &p)
{
    fill4(b->sixteen, 16.0f);
    fill4(b->limitedScale, 298.0f / 256.0f);
    fill4(b->center, 128.0f);
    fill4(b->contrast, (float)p.contrastQ8 / 256.0f);
    fill4(b->lumaR, 77.0f / 256.0f);
    fill4(b->lumaG, 150.0f / 256.0f);
    fill4(b->lumaB, 29.0f / 256.0f);
    fill4(b->saturation, (float)p.saturationQ8 / 256.0f);
}

static_assert(offsetof(VfpuBatch, inR) == 0, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, inG) == 16, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, inB) == 32, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, outR) == 48, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, outG) == 64, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, outB) == 80, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, outY) == 96, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, sixteen) == 112, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, limitedScale) == 128, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, center) == 144, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, contrast) == 160, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, lumaR) == 176, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, lumaG) == 192, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, lumaB) == 208, "VfpuBatch layout");
static_assert(offsetof(VfpuBatch, saturation) == 224, "VfpuBatch layout");

static inline void vfpuColorCore(VfpuBatch *b, int expandLimited)
{
#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_VFPU_VIDEO && PPA_ENABLE_SCRATCHPAD_ACCEL
#define PPA_VFPU_COLOR_TAIL \
        "lv.q C030, 144(%0)\n" \
        "lv.q C100, 160(%0)\n" \
        "vsub.q C000, C000, C030\n" \
        "vsub.q C010, C010, C030\n" \
        "vsub.q C020, C020, C030\n" \
        "vmul.q C000, C000, C100\n" \
        "vmul.q C010, C010, C100\n" \
        "vmul.q C020, C020, C100\n" \
        "vadd.q C000, C000, C030\n" \
        "vadd.q C010, C010, C030\n" \
        "vadd.q C020, C020, C030\n" \
        "vf2in.q C000, C000, 0\n" \
        "vf2in.q C010, C010, 0\n" \
        "vf2in.q C020, C020, 0\n" \
        "vi2f.q C000, C000, 0\n" \
        "vi2f.q C010, C010, 0\n" \
        "vi2f.q C020, C020, 0\n" \
        "lv.q C100, 176(%0)\n" \
        "lv.q C110, 192(%0)\n" \
        "lv.q C120, 208(%0)\n" \
        "vmul.q C200, C000, C100\n" \
        "vmul.q C210, C010, C110\n" \
        "vmul.q C220, C020, C120\n" \
        "vadd.q C200, C200, C210\n" \
        "vadd.q C200, C200, C220\n" \
        "vf2in.q C200, C200, 0\n" \
        "vi2f.q C200, C200, 0\n" \
        "lv.q C230, 224(%0)\n" \
        "vsub.q C000, C000, C200\n" \
        "vsub.q C010, C010, C200\n" \
        "vsub.q C020, C020, C200\n" \
        "vmul.q C000, C000, C230\n" \
        "vmul.q C010, C010, C230\n" \
        "vmul.q C020, C020, C230\n" \
        "vadd.q C000, C000, C200\n" \
        "vadd.q C010, C010, C200\n" \
        "vadd.q C020, C020, C200\n" \
        "vf2in.q C000, C000, 0\n" \
        "vf2in.q C010, C010, 0\n" \
        "vf2in.q C020, C020, 0\n" \
        "vf2in.q C200, C200, 0\n" \
        "sv.q C000, 48(%0)\n" \
        "sv.q C010, 64(%0)\n" \
        "sv.q C020, 80(%0)\n" \
        "sv.q C200, 96(%0)\n"

    if (expandLimited) {
        __asm__ volatile(
            "lv.q C000, 0(%0)\n"
            "lv.q C010, 16(%0)\n"
            "lv.q C020, 32(%0)\n"
            "vi2f.q C000, C000, 0\n"
            "vi2f.q C010, C010, 0\n"
            "vi2f.q C020, C020, 0\n"
            "lv.q C120, 112(%0)\n"
            "lv.q C130, 128(%0)\n"
            "vsub.q C000, C000, C120\n"
            "vsub.q C010, C010, C120\n"
            "vsub.q C020, C020, C120\n"
            "vmul.q C000, C000, C130\n"
            "vmul.q C010, C010, C130\n"
            "vmul.q C020, C020, C130\n"
            "vf2in.q C000, C000, 0\n"
            "vf2in.q C010, C010, 0\n"
            "vf2in.q C020, C020, 0\n"
            "vi2f.q C000, C000, 0\n"
            "vi2f.q C010, C010, 0\n"
            "vi2f.q C020, C020, 0\n"
            PPA_VFPU_COLOR_TAIL
            :
            : "r"(b)
            : "memory");
    } else {
        __asm__ volatile(
            "lv.q C000, 0(%0)\n"
            "lv.q C010, 16(%0)\n"
            "lv.q C020, 32(%0)\n"
            "vi2f.q C000, C000, 0\n"
            "vi2f.q C010, C010, 0\n"
            "vi2f.q C020, C020, 0\n"
            PPA_VFPU_COLOR_TAIL
            :
            : "r"(b)
            : "memory");
    }
#undef PPA_VFPU_COLOR_TAIL
#else
    (void)b;
    (void)expandLimited;
#endif
}

static uint32_t scalarPixel(uint32_t pixel,
                            uint32_t up,
                            uint32_t down,
                            int verticalNeighbors,
                            const ColorTransformParameters &p)
{
    int rr = red(pixel);
    int gg = green(pixel);
    int bb = blue(pixel);
    int yy = 0;

    if (p.expandLimited) {
        rr = ((rr - 16) * 298 + 128) >> 8;
        gg = ((gg - 16) * 298 + 128) >> 8;
        bb = ((bb - 16) * 298 + 128) >> 8;
    }
    rr = ((rr - 128) * p.contrastQ8 + 128 * 256 + 128) >> 8;
    gg = ((gg - 128) * p.contrastQ8 + 128 * 256 + 128) >> 8;
    bb = ((bb - 128) * p.contrastQ8 + 128 * 256 + 128) >> 8;
    yy = (77 * rr + 150 * gg + 29 * bb + 128) >> 8;
    rr = yy + (((rr - yy) * p.saturationQ8 + 128) >> 8);
    gg = yy + (((gg - yy) * p.saturationQ8 + 128) >> 8);
    bb = yy + (((bb - yy) * p.saturationQ8 + 128) >> 8);

    if (p.hdrTransfer) {
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
    if (!p.widePanel && yy < 96) {
        int lift = (96 - yy) / 12;
        rr += lift; gg += lift; bb += lift;
    }
    if (p.widePanel && verticalNeighbors) {
        int ur = red(up), ub = blue(up);
        int dr = red(down), db = blue(down);
        rr = (6 * rr + ur + dr + 4) >> 3;
        bb = (6 * bb + ub + db + 4) >> 3;
    }
    return rgba(rr, gg, bb, alpha(pixel));
}

static void processBatch(const uint32_t *src,
                         const uint32_t *up,
                         const uint32_t *down,
                         uint32_t *dst,
                         unsigned int count,
                         int verticalNeighbors,
                         const ColorTransformParameters &p,
                         VfpuBatch *b)
{
    unsigned int i;
    for (i = 0; i < 4U; ++i) {
        uint32_t pixel = i < count ? src[i] : src[count - 1U];
        b->inR[i] = red(pixel);
        b->inG[i] = green(pixel);
        b->inB[i] = blue(pixel);
    }

    vfpuColorCore(b, p.expandLimited);

    for (i = 0; i < count; ++i) {
        int rr = b->outR[i];
        int gg = b->outG[i];
        int bb = b->outB[i];
        int yy = b->outY[i];

        if (p.hdrTransfer) {
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
        if (!p.widePanel && yy < 96) {
            int lift = (96 - yy) / 12;
            rr += lift; gg += lift; bb += lift;
        }
        if (p.widePanel &&
            verticalNeighbors) {
            int ur = red(up[i]), ub = blue(up[i]);
            int dr = red(down[i]), db = blue(down[i]);
            rr = (6 * rr + ur + dr + 4) >> 3;
            bb = (6 * bb + ub + db + 4) >> 3;
        }
        dst[i] = rgba(rr, gg, bb, alpha(src[i]));
    }
}

static unsigned int channelDelta(uint32_t a, uint32_t b)
{
    unsigned int maximum = 0;
    unsigned int d;
    d = (unsigned int)(red(a) > red(b) ? red(a) - red(b) : red(b) - red(a));
    if (d > maximum) maximum = d;
    d = (unsigned int)(green(a) > green(b) ? green(a) - green(b) : green(b) - green(a));
    if (d > maximum) maximum = d;
    d = (unsigned int)(blue(a) > blue(b) ? blue(a) - blue(b) : blue(b) - blue(a));
    if (d > maximum) maximum = d;
    return maximum;
}

static void captureSample(const FrameView &frame,
                          const ColorTransformParameters &p,
                          uint32_t *sample,
                          unsigned int &sampleWidth,
                          unsigned int &sampleHeight)
{
    unsigned int regionWidth = (unsigned int)(p.x1 - p.x0);
    unsigned int regionHeight = (unsigned int)(p.y1 - p.y0);
    unsigned int sampleX;
    unsigned int sampleY;
    unsigned int row;

    sampleWidth = regionWidth < kSampleWidth ? regionWidth : kSampleWidth;
    sampleWidth &= ~3U;
    sampleHeight = regionHeight > 2U ? regionHeight - 2U : 0U;
    if (sampleHeight > kSampleHeight)
        sampleHeight = kSampleHeight;
    if (sampleWidth < 4U || sampleHeight == 0U) {
        sampleWidth = sampleHeight = 0U;
        return;
    }
    sampleX = (unsigned int)p.x0 + (regionWidth - sampleWidth) / 2U;
    sampleY = (unsigned int)p.y0 + 1U +
              (regionHeight - sampleHeight - 2U) / 2U;
    for (row = 0; row < sampleHeight + 2U; ++row) {
        const uint32_t *src = frame.pixels +
            (sampleY - 1U + row) * frame.pitch + sampleX;
        memcpy(sample + row * sampleWidth, src,
               sampleWidth * sizeof(uint32_t));
    }
}

static void runCalibration(const ColorTransformParameters &parameters,
                           ScratchLayout &layout,
                           unsigned int sampleWidth,
                           unsigned int sampleHeight)
{
    ColorTransformParameters sampleParameters = parameters;
    uint64_t started;
    unsigned int scalarUs;
    unsigned int vfpuUs;
    unsigned int count = sampleWidth * sampleHeight;
    unsigned int i;
    unsigned int maxDelta = 0;
    unsigned long long sumDelta = 0;
    int verifyFailed = 0;

    if (sampleWidth == 0U || sampleHeight == 0U)
        return;
    sampleParameters.x0 = 0;
    sampleParameters.y0 = 0;
    sampleParameters.x1 = (int)sampleWidth;
    sampleParameters.y1 = (int)sampleHeight;

    started = (uint64_t)sceKernelGetSystemTimeWide();
    {
        unsigned int repeat;
        for (repeat = 0; repeat < kCalibrationRepeats; ++repeat) {
            for (i = 0; i < sampleHeight; ++i) {
                unsigned int x;
                const uint32_t *row = layout.sampleSource +
                    (i + 1U) * sampleWidth;
                const uint32_t *up = row - sampleWidth;
                const uint32_t *down = row + sampleWidth;
                uint32_t *out = layout.sampleScalar + i * sampleWidth;
                for (x = 0; x < sampleWidth; ++x)
                    out[x] = scalarPixel(row[x], up[x], down[x], 1,
                                         sampleParameters);
            }
            __asm__ volatile("" : : : "memory");
        }
    }
    scalarUs = (unsigned int)((uint64_t)sceKernelGetSystemTimeWide() - started);

    started = (uint64_t)sceKernelGetSystemTimeWide();
    {
        unsigned int repeat;
        for (repeat = 0; repeat < kCalibrationRepeats; ++repeat) {
            for (i = 0; i < sampleHeight; ++i) {
                unsigned int x;
                const uint32_t *row = layout.sampleSource +
                    (i + 1U) * sampleWidth;
                const uint32_t *up = row - sampleWidth;
                const uint32_t *down = row + sampleWidth;
                uint32_t *out = layout.sampleVfpu + i * sampleWidth;
                for (x = 0; x < sampleWidth; x += 4U)
                    processBatch(row + x, up + x, down + x, out + x, 4U, 1,
                                 sampleParameters, layout.batch);
            }
            __asm__ volatile("" : : : "memory");
        }
    }
    vfpuUs = (unsigned int)((uint64_t)sceKernelGetSystemTimeWide() - started);

    for (i = 0; i < count; ++i) {
        unsigned int d = channelDelta(layout.sampleScalar[i],
                                      layout.sampleVfpu[i]);
        if (d > maxDelta) maxDelta = d;
        sumDelta += d;
    }
    /* Correctness sampling is always active; PPA_ACCEL_VERIFY only enables
     * the heavier CRC/mean-delta diagnostics.  A failed sample immediately
     * selects the authoritative scalar path. */
    verifyFailed = maxDelta > PPA_ACCEL_VERIFY_MAX_DELTA;
#if PPA_ACCEL_VERIFY
    ;
#else
    (void)sumDelta;
#endif
    ppa_accel_governor_observe_sample(scalarUs ? scalarUs : 1U,
                                      vfpuUs ? vfpuUs : 1U,
                                      maxDelta,
                                      verifyFailed);
}

} // namespace

VfpuStageAccelerator::VfpuStageAccelerator()
    : sessionActive_(false), sessionStageMask_(0U) {}

VfpuStageAccelerator::~VfpuStageAccelerator() {}

bool VfpuStageAccelerator::supports(StageId id) const
{
#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_VFPU_VIDEO && PPA_ENABLE_SCRATCHPAD_ACCEL
    return id == StagePspScreen;
#else
    (void)id;
    return false;
#endif
}

bool VfpuStageAccelerator::supportsColorGroup(unsigned int stageMask) const
{
#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_VFPU_VIDEO && PPA_ENABLE_SCRATCHPAD_ACCEL
    return stageMask == StageMaskPspScreen;
#else
    (void)stageMask;
    return false;
#endif
}

bool VfpuStageAccelerator::processColorGroup(
    const FrameView &frame,
    const ColorTransformParameters &parameters)
{
#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_VFPU_VIDEO && PPA_ENABLE_SCRATCHPAD_ACCEL
    struct ppa_scratchpad_lease lease;
    ScratchLayout layout;
    enum ppa_accel_plan plan;
    unsigned int width;
    unsigned int height;
    unsigned int y;
    unsigned int sampleWidth = 0;
    unsigned int sampleHeight = 0;
    int calibrationDue;
    uint64_t started;
    uint64_t copySampleStarted = 0;
    unsigned int copySampleUs = 0;
    unsigned int copySampleRows = 0;
    unsigned int estimatedCopyUs = 0;

    if (!sessionActive_ ||
        (sessionStageMask_ & parameters.stageMask) != parameters.stageMask ||
        !supportsColorGroup(parameters.stageMask) || frame.pixels == 0)
        return false;
    if (parameters.x0 < 0 || parameters.y0 < 0 ||
        parameters.x1 <= parameters.x0 || parameters.y1 <= parameters.y0 ||
        (unsigned int)parameters.x1 > frame.width ||
        (unsigned int)parameters.y1 > frame.height)
        return false;
    width = (unsigned int)(parameters.x1 - parameters.x0);
    height = (unsigned int)(parameters.y1 - parameters.y0);
    if (width == 0U || height == 0U || width > kMaxWidth ||
        frame.pitch < frame.width)
        return false;

    plan = ppa_accel_governor_plan(parameters.stageMask,
                                    width * height,
                                    frame.frameNumber);
    if (plan != PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR)
        return false;
    if (!ppa_scratchpad_acquire(PPA_SCRATCHPAD_PARTITION_VIDEO, &lease))
        return false;
    if (!mapScratch(lease.base, lease.bytes, layout)) {
        ppa_scratchpad_release(&lease);
        return false;
    }

    prepareBatchConstants(layout.batch, parameters);
    calibrationDue = ppa_accel_governor_calibration_due(frame.frameNumber);
    if (calibrationDue)
        captureSample(frame, parameters, layout.sampleSource,
                      sampleWidth, sampleHeight);

    started = (uint64_t)sceKernelGetSystemTimeWide();
    for (y = 0; y < height; ++y) {
        const unsigned int absoluteY = (unsigned int)parameters.y0 + y;
        const uint32_t *sourceRow = frame.pixels +
            absoluteY * frame.pitch + (unsigned int)parameters.x0;
        const uint32_t *upRow;
        const uint32_t *downRow;
        uint32_t *destinationRow = frame.pixels +
            absoluteY * frame.pitch + (unsigned int)parameters.x0;
        unsigned int x;

        if (calibrationDue && (y & 15U) == 0U) {
            copySampleStarted = (uint64_t)sceKernelGetSystemTimeWide();
            memcpy(layout.currentRow, sourceRow, width * sizeof(uint32_t));
            copySampleUs += (unsigned int)(
                (uint64_t)sceKernelGetSystemTimeWide() - copySampleStarted);
            ++copySampleRows;
        } else {
            memcpy(layout.currentRow, sourceRow, width * sizeof(uint32_t));
        }

        upRow = y > 0U ? layout.previousRow : layout.currentRow;
        downRow = y + 1U < height ? sourceRow + frame.pitch : layout.currentRow;
        for (x = 0; x < width; x += 4U) {
            unsigned int count = width - x;
            if (count > 4U) count = 4U;
            processBatch(layout.currentRow + x,
                         upRow + x,
                         downRow + x,
                         destinationRow + x,
                         count,
                         y > 0U && y + 1U < height,
                         parameters,
                         layout.batch);
        }
        {
            uint32_t *temp = layout.previousRow;
            layout.previousRow = layout.currentRow;
            layout.currentRow = temp;
        }
    }
    {
        unsigned int elapsedUs = (unsigned int)(
            (uint64_t)sceKernelGetSystemTimeWide() - started);
        if (copySampleRows != 0U)
            estimatedCopyUs = (copySampleUs * height +
                               copySampleRows / 2U) / copySampleRows;
        ppa_accel_governor_observe_vfpu_frame(elapsedUs,
                                              estimatedCopyUs,
                                              width * height);
    }

    if (calibrationDue && sampleWidth != 0U && sampleHeight != 0U)
        runCalibration(parameters, layout, sampleWidth, sampleHeight);

    ppa_scratchpad_release(&lease);
    return true;
#else
    (void)frame;
    (void)parameters;
    return false;
#endif
}

void VfpuStageAccelerator::resetSession(unsigned int stageMask)
{
#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_VFPU_VIDEO && PPA_ENABLE_SCRATCHPAD_ACCEL
    if (sessionActive_) shutdown();
    sessionStageMask_ = 0U;
    if (stageMask != StageMaskPspScreen) return;
    sessionStageMask_ = stageMask;
    ppa_scratchpad_init();
    ppa_scratchpad_set_phase(PPA_SCRATCHPAD_PHASE_PLAYBACK);
    ppa_accel_governor_init();
    ppa_accel_governor_reset();
    sessionActive_ = true;
#else
    (void)stageMask;
#endif
}

void VfpuStageAccelerator::shutdown()
{
#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_VFPU_VIDEO && PPA_ENABLE_SCRATCHPAD_ACCEL
    if (!sessionActive_) {
        sessionStageMask_ = 0U;
        return;
    }
    /* Persist panel calibration only after presentation has stopped. */
    ppa_accel_governor_flush_profile();
    ppa_scratchpad_set_phase(PPA_SCRATCHPAD_PHASE_MENU);
    sessionActive_ = false;
    sessionStageMask_ = 0U;
#endif
}

} }
