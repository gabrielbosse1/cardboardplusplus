// FFmpeg/libav C API surface consumed by VideoEncoder (codec, bitstream filter, scale, D3D11VA interop).
#pragma once
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