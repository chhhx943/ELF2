#include "encoder.h"
#include <stdio.h>
#include <stdlib.h>

int streamer_init(FFmpegStreamer *s, const char *filename, int width, int height, int fps)
{
    s->width = width;
    s->height = height;
    s->frame_pts = 0;
    avformat_alloc_output_context2(&s->fmt_ctx, NULL, "flv", filename);
    if (!s->fmt_ctx) {
        fprintf(stderr, "Could not allocate output context\n");
        return -1;
    }   

   AVCodec *codec = avcodec_find_encoder_by_name("h264_rkmpp");
  // const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
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
    s->enc_ctx->bit_rate = 400000; // Adjust as needed

   // av_opt_set(s->enc_ctx->priv_data, "preset", "ultrafast", 0);
   // av_opt_set(s->enc_ctx->priv_data, "tune", "zerolatency", 0);



    if (s->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        s->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(s->enc_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return -1;
    }
    avcodec_parameters_from_context(s->video_st->codecpar, s->enc_ctx);

    s->sws_ctx = NULL;

    s->yuv_frame = av_frame_alloc();
    s->yuv_frame->format = AV_PIX_FMT_NV12;
    s->yuv_frame->width = width;
    s->yuv_frame->height = height;
    
    if (av_frame_get_buffer(s->yuv_frame, 0) < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        return -1;
    }
   
    if (!(s->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&s->fmt_ctx->pb, filename, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open output file\n");
            return -1;
        }
    }

    avformat_write_header(s->fmt_ctx, NULL);      
    return 0;
}

int streamer_push(FFmpegStreamer *s, uint8_t *nv12_data)
{

    s->yuv_frame->data[0] = nv12_data;
    s->yuv_frame->linesize[0] = s->width;
    s->yuv_frame->data[1] = nv12_data + s->width * s->height;
    s->yuv_frame->linesize[1] = s->width;
    s->yuv_frame->pts = s->frame_pts++;
    AVPacket *pkt = av_packet_alloc();

    int ret = avcodec_send_frame(s->enc_ctx, s->yuv_frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending frame to encoder\n");
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(s->enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "Error encoding frame\n");
            return -1;
        }
        av_packet_rescale_ts(pkt, s->enc_ctx->time_base, s->video_st->time_base);
        pkt->stream_index = s->video_st->index;
        av_interleaved_write_frame(s->fmt_ctx, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return 0;
}

int streamer_clean(FFmpegStreamer *s)
{
    if (!s) return;
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