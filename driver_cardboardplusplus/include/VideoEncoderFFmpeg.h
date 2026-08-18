#pragma once
// FFmpeg C headers shared by the VideoEncoder translation units. They MUST be
// included inside `extern "C"` so MSVC generates correct C-linkage name
// decoration for the FFmpeg symbols.
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
#include <libavutil/hwcontext_d3d11va.h>
}