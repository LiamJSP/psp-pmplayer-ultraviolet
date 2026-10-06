#ifndef PPA_MEDIA_BACKEND_HPP
#define PPA_MEDIA_BACKEND_HPP

#include <stdint.h>
#include "common/directory.h"
#include "mod/movie_file.h"

namespace ppa {
namespace media {

enum ContainerKind {
    ContainerMp4,
    ContainerMkv,
    ContainerUnknown
};

struct Metadata {
    bool valid;
    uint32_t totalFrames;
    uint32_t width;
    uint32_t height;
    uint32_t scale;
    uint32_t rate;
    uint32_t audioStreams;
    uint32_t embeddedSubtitles;

    Metadata();
};

struct PlaybackRequest {
    movie_file_struct *movie;
    bool resume;
    int pspType;
    int tvAspectRatio;
    int overscanLeft;
    int overscanTop;
    int overscanRight;
    int overscanBottom;
    int videoMode;
};

/*
 * Phase-2 seam for ME/VME-backed work. The default implementation is a
 * zero-cost null object. A future kernel-module bridge can be installed once
 * and receive session boundaries without changing browser/container code.
 */
class AvcAcceleration {
public:
    virtual ~AvcAcceleration() {}
    virtual bool prepare(ContainerKind kind, const char *path) = 0;
    virtual void finish() = 0;
};

class Backend {
public:
    virtual ~Backend() {}
    virtual ContainerKind kind() const = 0;
    virtual file_type_enum fileType() const = 0;
    virtual const char *name() const = 0;
    virtual bool probe(const char *path, Metadata &metadata) const = 0;
    virtual char *play(const PlaybackRequest &request) const = 0;
};

const Backend *backendForFileType(file_type_enum fileType);
const Backend *backendForExtension(const char *extension);
bool isSupportedFileType(file_type_enum fileType);
const char *fileTypeName(file_type_enum fileType);

void installAcceleration(AvcAcceleration *acceleration);
AvcAcceleration &acceleration();

} // namespace media
} // namespace ppa

#endif
