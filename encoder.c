#include "encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <libavutil/hwcontext_drm.h>
#include <drm/drm_fourcc.h>
#include <libavutil/imgutils.h>
#include <sys/mman.h>
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
    s->enc_ctx->bit_rate = 1500000; // Adjust as needed

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
    
    if (av_frame_get_buffer(s->yuv_frame, 64) < 0) {
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

/*int streamer_push_zerocopy(FFmpegStreamer *s, int dma_fd) {
    int ret;

    av_frame_unref(s->yuv_frame);
    // 1. 设置帧格式为特殊的 DRM_PRIME，告诉 FFmpeg "这里面不是真实数据，是硬件句柄"
    s->yuv_frame->format = AV_PIX_FMT_DRM_PRIME;
    s->yuv_frame->width = s->enc_ctx->width;
    s->yuv_frame->height = s->enc_ctx->height;

    // 2. 动态分配并填写 DRM 硬件描述符 (快递单)
    AVDRMFrameDescriptor *desc = av_mallocz(sizeof(AVDRMFrameDescriptor));
  
    if (!desc) return -1;

    desc->nb_objects = 1;
    desc->objects[0].fd = dma_fd;
    desc->objects[0].size = s->yuv_frame->width * s->yuv_frame->height * 3 / 2; // NV12 总大小
    desc->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR; // 线性内存排布
    
    desc->nb_layers = 1;
    desc->layers[0].format = DRM_FORMAT_NV12; // 明确告知是 NV12 格式
    desc->layers[0].nb_planes = 2;            // NV12 包含 Y 和 UV 两个平面
    
    // 配置 Y 平面 (亮度)
    desc->layers[0].planes[0].object_index = 0;
    desc->layers[0].planes[0].offset = 0;
    desc->layers[0].planes[0].pitch = s->yuv_frame->width; // 每一行的跨度
    
    // 配置 UV 平面 (色度)
    desc->layers[0].planes[1].object_index = 0;
    desc->layers[0].planes[1].offset = s->yuv_frame->width * s->yuv_frame->height;
    desc->layers[0].planes[1].pitch = s->yuv_frame->width;

    // 3. 将描述符挂载到 AVFrame 的 data[0] 指针上
    s->yuv_frame->data[0] = (uint8_t *)desc;
    s->yuv_frame->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc), av_buffer_default_free, NULL, 0);
    s->yuv_frame->pts = s->frame_pts++;

    // 4. 将帧发送给硬件编码器
    ret = avcodec_send_frame(s->enc_ctx, s->yuv_frame);
    

    if (ret < 0) {
        fprintf(stderr, "Error sending frame to hardware encoder\n");
        return ret;
    }

    // 5. 循环接收压缩好的 H.264 数据包并推流
    AVPacket *pkt = av_packet_alloc();
    while (1) {
        ret = avcodec_receive_packet(s->enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break; // 硬件说：这帧还没完全压好，或者结束了，跳出循环
        } else if (ret < 0) {
            fprintf(stderr, "Error receiving packet from hardware encoder\n");
            break;
        }

        // 时间基转换：从编码器的时钟转到 RTMP/FLV 封装器的时钟
        av_packet_rescale_ts(pkt, s->enc_ctx->time_base, s->video_st->time_base);
        pkt->stream_index = s->video_st->index;

        // 发送给 Nginx 服务器
        av_interleaved_write_frame(s->fmt_ctx, pkt);
        av_packet_unref(pkt); // 用完清理当前包
    }
    av_packet_free(&pkt);

    return 0;
}
*/
int streamer_push_zerocopy(FFmpegStreamer *s, int dma_fd) {
    int ret;
    size_t frame_size = s->width * s->height * 3 / 2; // NV12 紧凑大小

    // 1. 将底层的 DMA-BUF 映射到子进程的虚拟内存中
    uint8_t *mapped_ptr = mmap(NULL, frame_size, PROT_READ, MAP_SHARED, dma_fd, 0);
    if (mapped_ptr == MAP_FAILED) {
        perror("[Child] mmap dma_fd failed");
        return -1;
    }

    // 2. 构造源数据的信息卡：告诉 FFmpeg 我们的 V4L2 数据是紧紧挨着的 (stride = width)
    uint8_t *src_data[4] = { mapped_ptr, mapped_ptr + (s->width * s->height), NULL, NULL };
    int src_linesize[4] = { s->width, s->width, 0, 0 };

    // 3. 【解除绿屏的魔法核心】
    // 确保 AVFrame 可写，然后使用 av_image_copy 智能拷贝。
    // 它会自动把紧凑的 src_data，对齐到拥有 768 Padding 的 s->yuv_frame 内存中！
    av_frame_make_writable(s->yuv_frame);
    av_image_copy(s->yuv_frame->data, s->yuv_frame->linesize,
                  (const uint8_t **)src_data, src_linesize,
                  s->yuv_frame->format, s->width, s->height);

    s->yuv_frame->pts = s->frame_pts++;

    // 4. 解除映射 (数据已经安全抵达带 Padding 的 AVFrame)
    munmap(mapped_ptr, frame_size);

    // 5. 把完美的帧喂给 MPP 硬件编码器
    ret = avcodec_send_frame(s->enc_ctx, s->yuv_frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending frame to encoder\n");
        return ret;
    }

    // 6. 接收 H.264 数据包并推流
    AVPacket *pkt = av_packet_alloc();
    while (1) {
        ret = avcodec_receive_packet(s->enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

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
    if (!s) return -1;
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