#ifndef PPA_BUILD_CONTRACT_H
#define PPA_BUILD_CONTRACT_H

/*
 * Release builds must retain the complete deep-B compatibility layer.  These
 * checks intentionally fail compilation if a future build-file cleanup drops
 * one of the pieces required for B-frames > 1 or B-pyramid presentation.
 */
#if defined(PPA_RELEASE_BUILD) && PPA_RELEASE_BUILD
# if !defined(PPA_H264X_PRE_REWRITE) || !(PPA_H264X_PRE_REWRITE)
#  error "Release builds require H.264X access-unit pre-rewrite"
# endif
# if !defined(PPA_H264X_REWRITE_RETRY) || !(PPA_H264X_REWRITE_RETRY)
#  error "Release builds require the H.264X decoder retry bridge"
# endif
# if !defined(PPA_H264X_DEEP_PRESERVE_RPLM) || !(PPA_H264X_DEEP_PRESERVE_RPLM)
#  error "Release builds require deep-B reference-list preservation"
# endif
#endif

#endif
