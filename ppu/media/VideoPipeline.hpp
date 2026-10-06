#ifndef PPA_VIDEO_PIPELINE_HPP
#define PPA_VIDEO_PIPELINE_HPP

#include <stdint.h>

namespace ppa { namespace video {

/* Keep the panel mask stable for existing accelerator calibration profiles. */
enum StageId { StagePspScreen = 5 };
enum StageMask { StageMaskPspScreen = 1U << StagePspScreen };

struct FrameView {
    uint32_t *pixels;
    unsigned int pitch;
    unsigned int width;
    unsigned int height;
    unsigned int sourceWidth;
    unsigned int sourceHeight;
    int viewportX;
    int viewportY;
    int viewportWidth;
    int viewportHeight;
    unsigned int frameNumber;
    int pspModel;
    int fullRange;
    int colorPrimaries;
    int transferCharacteristics;
    int matrixCoefficients;
};

/* Immutable panel parameters shared by the scalar and VFPU paths. */
struct ColorTransformParameters {
    unsigned int stageMask;
    int x0;
    int y0;
    int x1;
    int y1;
    int expandLimited;
    int widePanel;
    int hdrTransfer;
    int saturationQ8;
    int contrastQ8;
};

/* Accelerators claim whole planned groups rather than forcing an intermediate
 * full-frame copy for every logical stage.  The CPU pipeline remains the
 * authoritative fallback and no virtual dispatch occurs per pixel. */
class StageAccelerator {
public:
    virtual ~StageAccelerator() {}
    virtual bool supports(StageId id) const = 0;
    virtual bool supportsColorGroup(unsigned int stageMask) const = 0;
    virtual bool processColorGroup(const FrameView &frame,
                                   const ColorTransformParameters &parameters) = 0;
    virtual void resetSession(unsigned int stageMask) = 0;
    virtual void shutdown() = 0;
};

void setLcdOutput(bool enabled);

class VideoPipeline {
public:
    static VideoPipeline &instance();
    void reloadConfiguration();
    void resetSession();
    void leavePlayback();
    void shutdown();
    void setAvcSps(const uint8_t *sps, unsigned int size);
    bool configured() const;
    bool enabled() const;
    bool vfpuRequired() const;
    bool extremeBatterySaverEnabled() const;
    bool process(const FrameView &frame);
    void setAccelerator(StageAccelerator *accelerator);

private:
    VideoPipeline();
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline &);
    VideoPipeline &operator=(const VideoPipeline &);

    struct Impl;
    Impl *impl_;
};

} }

#endif
