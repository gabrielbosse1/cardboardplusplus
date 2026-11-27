// FFmpeg/libav C API surface consumed by VideoEncoder (codec, bitstream filter, scale,
// hardware device contexts). Windows pulls in the d3d11va hw context; Linux uses VAAPI,
// whose header is C++ safe and therefore not listed here.
#pragma once
#ifdef _WIN32
// Must precede the extern "C" block below: libavutil/hwcontext_d3d11va.h includes
// <d3d11.h>, and parsing a C++ header inside a linkage block is an error.
#include <d3d11.h>
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
}