#include "mod/audio_format.h"
#include "MediaBackend.hpp"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mp4info.h"
#include "mkvinfo.h"
#include "common/mem64.h"
#include "common/ppa_playback_session.h"
#include "common/ppa_video_limits.h"
#include "mod/gu_draw.h"
#include "mod/mp4_play.h"
#include "mod/mkv_play.h"

namespace ppa {
namespace media {

namespace {

static const uint32_t FourccAvc1 = 0x61766331U;

class NullAcceleration : public AvcAcceleration {
public:
    virtual bool prepare(ContainerKind, const char *) { return true; }
    virtual void finish() {}
};

NullAcceleration gNullAcceleration;
AvcAcceleration *gAcceleration = &gNullAcceleration;

static bool validDimensions(uint32_t width,
                            uint32_t height,
                            uint8_t avcProfile,
                            bool baselineLargeAllowed)
{
    if (!PPA_VIDEO_DIMENSIONS_VALID(width, height))
        return false;

    /* MKV playback retains its legacy LCD-sized baseline restriction;
     * MP4 playback admits baseline in its existing 720x480 path. */
    if (!baselineLargeAllowed && avcProfile == 0x42 &&
        (width > PPA_VIDEO_LCD_WIDTH || height > PPA_VIDEO_LCD_HEIGHT))
        return false;

    return true;
}

struct AudioTrackSignature {
    uint32_t type;
    uint32_t rate;
    uint32_t channels;

    AudioTrackSignature() : type(0), rate(0), channels(0) {}

    void assign(uint32_t audioType, uint32_t sampleRate,
                uint32_t channelCount)
    {
        type = audioType;
        rate = sampleRate;
        channels = channelCount;
    }

    bool matches(uint32_t audioType, uint32_t sampleRate,
                 uint32_t channelCount) const
    {
        return type == audioType && rate == sampleRate &&
               channels == channelCount;
    }
};

static int countMp4AudioTracks(mp4info_t *info)
{
    AudioTrackSignature reference;
    int count = 0;
    int i;

    if (info == 0)
        return 0;

    for (i = 0; i < info->total_tracks && count < 6; ++i) {
        mp4info_track_t *track = info->tracks[i];
        ppu_audio_format format;
        if (!ppu_audio_format_mp4(info, track, &format))
            continue;

        if (count == 0) {
            reference.assign(track->audio_type, track->samplerate,
                             track->channels);
        }
        else if (!reference.matches(track->audio_type, track->samplerate,
                                    track->channels)) {
            continue;
        }
        ++count;
    }
    return count;
}

static int countMkvAudioTracks(mkvinfo_t *info)
{
    AudioTrackSignature reference;
    int count = 0;
    int i;

    if (info == 0)
        return 0;

    for (i = 0; i < info->total_tracks && count < 6; ++i) {
        mkvinfo_track_t *track = info->tracks[i];
        ppu_audio_format format;
        if (!ppu_audio_format_mkv(track, &format))
            continue;

        if (count == 0) {
            reference.assign(track->audio_type, track->samplerate,
                             track->channels);
        }
        else if (!reference.matches(track->audio_type, track->samplerate,
                                    track->channels)) {
            continue;
        }
        ++count;
    }
    return count;
}

static uint32_t countMp4Subtitles(mp4info_t *info)
{
    uint32_t count = 0;
    int i;

    for (i = 0; info != 0 && i < info->total_tracks; ++i) {
        mp4info_track_t *track = info->tracks[i];
        if (track != 0 && track->type == MP4_TRACK_SUBTITLE)
            ++count;
    }
    return count;
}

static uint32_t countMkvSubtitles(mkvinfo_t *info)
{
    uint32_t count = 0;
    int i;

    for (i = 0; info != 0 && i < info->total_tracks; ++i) {
        mkvinfo_track_t *track = info->tracks[i];
        if (track == 0 || track->type != MATROSKA_TRACK_SUBTITLE)
            continue;

        if (track->video_type == 0x74787475U || /* txtu */
            track->video_type == 0x7478746cU || /* txtl */
            track->video_type == 0x73736175U || /* ssau */
            track->video_type == 0x61737375U || /* assu */
            track->video_type == 0x76747475U)   /* vttu */
            ++count;
    }
    return count;
}

class Mp4Backend : public Backend {
public:
    virtual ContainerKind kind() const { return ContainerMp4; }
    virtual file_type_enum fileType() const { return FS_MP4_FILE; }
    virtual const char *name() const { return "mp4"; }

    virtual bool probe(const char *path, Metadata &out) const
    {
        mp4info_t *info;
        mp4info_track_t *video = 0;
        int audioTracks;
        int i;
        uint32_t sampleIndex;

        out = Metadata();
        if (path == 0)
            return false;

        info = mp4info_open_metadata(path);
        if (info == 0)
            return false;

        for (i = 0; i < info->total_tracks; ++i) {
            mp4info_track_t *track = info->tracks[i];
            if (track == 0 || track->type != MP4_TRACK_VIDEO ||
                track->video_type != FourccAvc1 ||
                !validDimensions(track->width, track->height, track->avc_profile, true))
                continue;
            video = track;
            break;
        }

        audioTracks = countMp4AudioTracks(info);
        if (video == 0 || audioTracks == 0) {
            mp4info_close(info);
            return false;
        }

        for (sampleIndex = 0; sampleIndex < video->stts_entry_count; ++sampleIndex)
            out.totalFrames += video->stts_sample_count[sampleIndex];

        out.width = video->width;
        out.height = video->height;
        out.scale = (video->stts_entry_count > 0 &&
                     video->stts_sample_duration[0] != 0) ?
                    video->stts_sample_duration[0] : 1;
        out.rate = video->time_scale != 0 ? video->time_scale : 1;
        out.audioStreams = (uint32_t)audioTracks;
        out.embeddedSubtitles = countMp4Subtitles(info);
        out.valid = true;

        mp4info_close(info);
        return true;
    }

    virtual char *play(const PlaybackRequest &request) const
    {
        mp4_play_struct *session =
            static_cast<mp4_play_struct *>(malloc_64(sizeof(mp4_play_struct)));
        char *result;

        if (session == 0)
            return const_cast<char *>("mp4: insufficient memory for playback session");

        if (!ppa_session_audio_only() && !acceleration().prepare(ContainerMp4,
                                    request.movie != 0 ? request.movie->movie_file : 0)) {
            free_64(session);
            return const_cast<char *>("mp4: AVC acceleration preparation failed");
        }

        if (ppa_session_open() < 0) {
            if (!ppa_session_audio_only()) acceleration().finish();
            free_64(session);
            return const_cast<char *>("playback: timeline allocation failed");
        }
        ppa_gu_start(request.pspType,
                     request.tvAspectRatio,
                     request.overscanLeft,
                     request.overscanTop,
                     request.overscanRight,
                     request.overscanBottom,
                     request.videoMode);

        result = mp4_play_open(session,
                               request.movie,
                               request.resume ? 1 : 0,
                               request.pspType,
                               request.tvAspectRatio,
                               720 - request.overscanLeft - request.overscanRight,
                               480 - request.overscanTop - request.overscanBottom,
                               request.videoMode);
        if (result == 0) {
            result = mp4_play_start(session);
            /* start returns only after worker joins. Restore a borrowed blanked
             * scanout before decoder close resets/releases its frame pool. */
            ppa_session_close();
            mp4_play_close(session, request.resume ? 1 : 0, request.pspType);
        }

        ppa_session_close();
        ppa_gu_end();
        if (!ppa_session_audio_only()) acceleration().finish();
        free_64(session);
        return result;
    }
};

class MkvBackend : public Backend {
public:
    virtual ContainerKind kind() const { return ContainerMkv; }
    virtual file_type_enum fileType() const { return FS_MKV_FILE; }
    virtual const char *name() const { return "mkv"; }

    virtual bool probe(const char *path, Metadata &out) const
    {
        mkvinfo_t *info;
        mkvinfo_track_t *video = 0;
        int audioTracks;
        int i;

        out = Metadata();
        if (path == 0)
            return false;

        info = mkvinfo_open_metadata(path);
        if (info == 0)
            return false;

        for (i = 0; i < info->total_tracks; ++i) {
            mkvinfo_track_t *track = info->tracks[i];
            uint8_t profile;

            if (track == 0 || track->type != MATROSKA_TRACK_VIDEO ||
                track->video_type != FourccAvc1 ||
                track->private_data == 0 || track->private_size < 2)
                continue;

            profile = track->private_data[1];
            if (!validDimensions(track->width, track->height, profile, false))
                continue;

            video = track;
            break;
        }

        audioTracks = countMkvAudioTracks(info);
        if (video == 0 || audioTracks == 0) {
            mkvinfo_close(info);
            return false;
        }

        out.width = video->width;
        out.height = video->height;
        out.scale = video->duration != 0 ? video->duration : 1;
        out.rate = video->time_scale != 0 ? video->time_scale : 1;
        if (info->duration > 0 && out.scale > 0 && out.rate > 0) {
            uint64_t frames = (static_cast<uint64_t>(info->duration) * out.rate) /
                              (static_cast<uint64_t>(out.scale) * 1000ULL);
            out.totalFrames = frames > 0xffffffffULL ? 0xffffffffU :
                              static_cast<uint32_t>(frames);
        }
        out.audioStreams = (uint32_t)audioTracks;
        out.embeddedSubtitles = countMkvSubtitles(info);
        out.valid = true;

        mkvinfo_close(info);
        return true;
    }

    virtual char *play(const PlaybackRequest &request) const
    {
        mkv_play_struct *session =
            static_cast<mkv_play_struct *>(malloc_64(sizeof(mkv_play_struct)));
        char *result;

        if (session == 0)
            return const_cast<char *>("mkv: insufficient memory for playback session");

        if (!ppa_session_audio_only() && !acceleration().prepare(ContainerMkv,
                                    request.movie != 0 ? request.movie->movie_file : 0)) {
            free_64(session);
            return const_cast<char *>("mkv: AVC acceleration preparation failed");
        }

        if (ppa_session_open() < 0) {
            if (!ppa_session_audio_only()) acceleration().finish();
            free_64(session);
            return const_cast<char *>("playback: timeline allocation failed");
        }
        ppa_gu_start(request.pspType,
                     request.tvAspectRatio,
                     request.overscanLeft,
                     request.overscanTop,
                     request.overscanRight,
                     request.overscanBottom,
                     request.videoMode);

        result = mkv_play_open(session,
                               request.movie,
                               request.resume ? 1 : 0,
                               request.pspType,
                               request.tvAspectRatio,
                               720 - request.overscanLeft - request.overscanRight,
                               480 - request.overscanTop - request.overscanBottom,
                               request.videoMode);
        if (result == 0) {
            result = mkv_play_start(session);
            /* Keep display-power ownership ahead of decoder/frame teardown;
             * the outer close also covers opens that failed before workers. */
            ppa_session_close();
            mkv_play_close(session, request.resume ? 1 : 0, request.pspType);
        }

        ppa_session_close();
        ppa_gu_end();
        if (!ppa_session_audio_only()) acceleration().finish();
        free_64(session);
        return result;
    }
};

Mp4Backend gMp4Backend;
MkvBackend gMkvBackend;

} // anonymous namespace

Metadata::Metadata()
    : valid(false), totalFrames(0), width(0), height(0), scale(1), rate(1),
      audioStreams(0), embeddedSubtitles(0)
{
}

const Backend *backendForFileType(file_type_enum fileType)
{
    switch (fileType) {
    case FS_MP4_FILE:
        return &gMp4Backend;
    case FS_MKV_FILE:
        return &gMkvBackend;
    default:
        return 0;
    }
}

const Backend *backendForExtension(const char *extension)
{
    if (extension == 0)
        return 0;
    if (strcasecmp(extension, "mp4") == 0)
        return &gMp4Backend;
    if (strcasecmp(extension, "mkv") == 0)
        return &gMkvBackend;
    return 0;
}

bool isSupportedFileType(file_type_enum fileType)
{
    return backendForFileType(fileType) != 0;
}

const char *fileTypeName(file_type_enum fileType)
{
    const Backend *backend = backendForFileType(fileType);
    return backend != 0 ? backend->name() : "unknown";
}

void installAcceleration(AvcAcceleration *value)
{
    gAcceleration = value != 0 ? value : &gNullAcceleration;
}

AvcAcceleration &acceleration()
{
    return *gAcceleration;
}

} // namespace media
} // namespace ppa
