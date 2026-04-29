#include "encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <libavutil/hwcontext_drm.h>
#include <drm/drm_fourcc.h>
#include <libavutil/imgutils.h>
#include <sys/mman.h>
#include <time.h>
#include <RgaApi.h>
#include <im2d.h>

int streamer_init(FFmpegStreamer *s, const char *filename, int width, int height, int fps)
{
    const AVCodec *codec;
    int ret;

    s->width = width;
    s->height = height;
    s->frame_pts = 0;

    avformat_alloc_output_context2(&s->fmt_ctx, NULL, "flv", filename);
    if (!s->fmt_ctx) {
        fprintf(stderr, "Could not allocate output context\n");
        return -1;
    }

    codec = avcodec_find_encoder_by_name("h264_rkmpp");
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        return -1;
    }

    s->video_st = avformat_new_stream(s->fmt_ctx, codec);
    if (!s->video_st) {
        fprintf(stderr, "Could not create stream\n");
        return -1;
    }

    s->enc_ctx = avcodec_alloc_context3(codec);
    if (!s->enc_ctx) {
        fprintf(stderr, "Could not allocate codec context\n");
        return -1;
    }

    s->enc_ctx->width = width;
    s->enc_ctx->height = height;
    s->enc_ctx->time_base = (AVRational){1, fps};
    s->enc_ctx->framerate = (AVRational){fps, 1};
    s->enc_ctx->gop_size = 10;
    s->enc_ctx->max_b_frames = 0;
    s->enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    s->enc_ctx->bit_rate = 1500000;

    if (s->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        s->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(s->enc_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return -1;
    }
    avcodec_parameters_from_context(s->video_st->codecpar, s->enc_ctx);

    s->sws_ctx = NULL;

    s->yuv_frame = av_frame_alloc();
    if (!s->yuv_frame) {
        fprintf(stderr, "Could not allocate frame\n");
        return -1;
    }

    /*
     * MPP/RGA 这一链对高度对齐比较敏感，这里先按 768 分配带对齐的缓冲，
     * 后面真正编码时再把 frame height 设回 720。
     */
    s->yuv_frame->format = AV_PIX_FMT_NV12;
    s->yuv_frame->width = width;
    s->yuv_frame->height = 768;

    if (av_frame_get_buffer(s->yuv_frame, 64) < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        return -1;
    }

    s->yuv_frame->height = height;

    if (!(s->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&s->fmt_ctx->pb, filename, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open output file\n");
            return -1;
        }
    }

    ret = avformat_write_header(s->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not write stream header: %d\n", ret);
        return -1;
    }

    return 0;
}

int streamer_push(FFmpegStreamer *s, uint8_t *nv12_data)
{
    int ret;
    AVPacket *pkt = av_packet_alloc();

    if (!pkt) {
        return -1;
    }

    s->yuv_frame->data[0] = nv12_data;
    s->yuv_frame->linesize[0] = s->width;
    s->yuv_frame->data[1] = nv12_data + s->width * s->height;
    s->yuv_frame->linesize[1] = s->width;
    s->yuv_frame->pts = s->frame_pts++;

    ret = avcodec_send_frame(s->enc_ctx, s->yuv_frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending frame to encoder\n");
        av_packet_free(&pkt);
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(s->enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error encoding frame\n");
            av_packet_free(&pkt);
            return -1;
        }

        av_packet_rescale_ts(pkt, s->enc_ctx->time_base, s->video_st->time_base);
        pkt->stream_index = s->video_st->index;

        ret = av_interleaved_write_frame(s->fmt_ctx, pkt);
        if (ret < 0) {
            fprintf(stderr, "RTMP write failed: %d\n", ret);
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            return -1;
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return 0;
}

int streamer_push_zerocopy(FFmpegStreamer *s, int dma_fd) {
    int ret;
    AVPacket *pkt;
    rga_buffer_t src;
    rga_buffer_t dst;
    im_rect src_rect;
    im_rect dst_rect;
    IM_STATUS status;

    /* 先用 RGA 把 V4L2 导出的 DMA-BUF 拷进 FFmpeg 的对齐缓冲，再送给编码器。 */
    if (dma_fd <= 0) {
        fprintf(stderr, "Invalid dma_fd: %d\n", dma_fd);
        return -1;
    }
    if (!s->yuv_frame->data[0]) {
        fprintf(stderr, "AVFrame data[0] is NULL!\n");
        return -1;
    }

    s->yuv_frame->height = 768;
    ret = av_frame_make_writable(s->yuv_frame);
    if (ret < 0) {
        fprintf(stderr, "Frame is not writable: %d\n", ret);
        return -1;
    }
    s->yuv_frame->height = 720;

    src = wrapbuffer_fd(dma_fd, s->width, s->height, RK_FORMAT_YCbCr_420_SP);
    src.wstride = s->width;
    src.hstride = s->height;

    dst = wrapbuffer_virtualaddr(s->yuv_frame->data[0], s->width, s->height, RK_FORMAT_YCbCr_420_SP);
    dst.wstride = s->yuv_frame->linesize[0];
    dst.hstride = 768;

    src_rect = (im_rect){0, 0, s->width, s->height};
    dst_rect = (im_rect){0, 0, s->width, s->height};

    status = improcess(src, dst, (rga_buffer_t){0}, src_rect, dst_rect, (im_rect){0}, IM_SYNC);
    if (status != IM_STATUS_SUCCESS) {
        fprintf(stderr, "RGA copy failed: %s\n", imStrError(status));
        return -1;
    }

    s->yuv_frame->data[1] = s->yuv_frame->data[0] + (s->width * 768);
    s->yuv_frame->linesize[1] = s->width;
    s->yuv_frame->pts = s->frame_pts++;

    ret = avcodec_send_frame(s->enc_ctx, s->yuv_frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending frame to hardware encoder\n");
        return ret;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        return -1;
    }

    while (1) {
        ret = avcodec_receive_packet(s->enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error receiving packet from hardware encoder\n");
            av_packet_free(&pkt);
            return -1;
        }

        av_packet_rescale_ts(pkt, s->enc_ctx->time_base, s->video_st->time_base);
        pkt->stream_index = s->video_st->index;

        ret = av_interleaved_write_frame(s->fmt_ctx, pkt);
        if (ret < 0) {
            fprintf(stderr, "RTMP write failed: %d\n", ret);
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            return -1;
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return 0;
}

int streamer_clean(FFmpegStreamer *s)
{
    if (!s) {
        return -1;
    }
    if (s->fmt_ctx) {
        av_write_trailer(s->fmt_ctx);
    }
    if (s->yuv_frame) {
        av_frame_free(&s->yuv_frame);
    }
    if (s->enc_ctx) {
        avcodec_free_context(&s->enc_ctx);
    }
    if (s->fmt_ctx && !(s->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&s->fmt_ctx->pb);
    }
    if (s->fmt_ctx) {
        avformat_free_context(s->fmt_ctx);
        s->fmt_ctx = NULL;
    }

    return 0;
}
