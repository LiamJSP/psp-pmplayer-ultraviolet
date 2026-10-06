#ifndef PPA_VIDEO_PIPELINE_VFPU_HPP
#define PPA_VIDEO_PIPELINE_VFPU_HPP

#include "VideoPipeline.hpp"

namespace ppa { namespace video {

class VfpuStageAccelerator : public StageAccelerator {
public:
    VfpuStageAccelerator();
    virtual ~VfpuStageAccelerator();

    bool supports(StageId id) const;
    bool supportsColorGroup(unsigned int stageMask) const;
    bool processColorGroup(const FrameView &frame,
                           const ColorTransformParameters &parameters);
    void resetSession(unsigned int stageMask);
    void shutdown();

private:
    bool sessionActive_;
    unsigned int sessionStageMask_;
    VfpuStageAccelerator(const VfpuStageAccelerator &);
    VfpuStageAccelerator &operator=(const VfpuStageAccelerator &);
};

} }

#endif
