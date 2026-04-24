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
  //  s->enc_ctx->time_base = (AVRational){1, 1000000};
    s->enc_ctx->framerate = (AVRational){fps, 1};
    s->enc_ctx->gop_size = 10;
    s->enc_ctx->max_b_frames = 0;
    s->enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    s->enc_ctx->bit_rate = 1500000; // Adjust as needed





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

int streamer_push_zerocopy(FFmpegStreamer *s, int dma_fd) {
    int ret;

    // =======================================================
    // 1. 硬件级数据搬运 (RGA 完全接管内存对齐)
    // =======================================================


    if (dma_fd <= 0) {
        fprintf(stderr, "Invalid dma_fd: %d\n", dma_fd);
        return -1;
    }
    if (!s->yuv_frame->data[0]) {
        fprintf(stderr, "AVFrame data[0] is NULL!\n");
        return -1;
    }
    
    s->yuv_frame->height = 768;
    // 确保 AVFrame 的内部缓存已被分配且处于可写状态
    av_frame_make_writable(s->yuv_frame);

    // ??  B：申请完内存后，立刻把高度改回 720，给后面的编码器看
    s->yuv_frame->height = 720;
   


    // 1a. 配置 RGA 源：绑定 V4L2 底层吐出的纯净物理内存 (无 Padding)
    // dma_fd: V4L2 导出的金钥匙
    // RK_FORMAT_YCbCr_420_SP: RGA 里对 NV12 格式的称呼
    rga_buffer_t src = wrapbuffer_fd(dma_fd, s->width, s->height, RK_FORMAT_YCbCr_420_SP);
    src.wstride = s->width;
    src.hstride = s->height; // V4L2 真实高度 (例如 720)

    // 1b. 配置 RGA 目标：绑定 FFmpeg 带有 64 字节补齐的虚拟内存
    rga_buffer_t dst = wrapbuffer_virtualaddr(s->yuv_frame->data[0], s->width, s->height, RK_FORMAT_YCbCr_420_SP);
    dst.wstride = s->yuv_frame->linesize[0]; 
    dst.hstride = 768; // ?? 魔法核心：强制指定目标物理高度为 768，满足 MPP 的对齐癖好
    
    im_rect src_rect = {0, 0, s->width, s->height};
    im_rect dst_rect = {0, 0, s->width, s->height};

    rga_buffer_t pat = {0};
    im_rect pat_rect = {0};
    // 1c. 呼叫 RGA 硬件执行 2D 拷贝（瞬间完成，CPU 零干预）
    IM_STATUS status = improcess(src, dst, (rga_buffer_t){0}, src_rect, dst_rect, (im_rect){0}, IM_SYNC);
 //   IM_STATUS status = imcopy(src, dst);
    if (status != IM_STATUS_SUCCESS) {
        fprintf(stderr, "RGA Copy failed: %s\n", imStrError(status));
        return -1;
    }

    s->yuv_frame->data[1] = s->yuv_frame->data[0] + (1280 * 768);
    s->yuv_frame->linesize[1] = 1280; // 确保跨距一致
    // =======================================================
    // 2. 打上理想时间戳 (抚平物理抖动，保证播放器丝滑)
    // =======================================================
    // 强行按完美等差数列递增，配合推流协议的固定帧率要求
    s->yuv_frame->pts = s->frame_pts++;

    // =======================================================
    // 3. 将组装好的完美帧，喂给 MPP 硬件编码器
    // =======================================================
    ret = avcodec_send_frame(s->enc_ctx, s->yuv_frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending frame to hardware encoder\n");
        return ret;
    }

    // =======================================================
    // 4. 循环接收压缩好的 H.264 数据包并推流
    // =======================================================
    AVPacket *pkt = av_packet_alloc();
    while (1) {
        ret = avcodec_receive_packet(s->enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break; // 硬件说：还没压好，或者结束了
        } else if (ret < 0) {
            fprintf(stderr, "Error receiving packet from hardware encoder\n");
            break;
        }

        // 时间基转换：从编码器的时钟转到 RTMP/FLV 的时钟
        av_packet_rescale_ts(pkt, s->enc_ctx->time_base, s->video_st->time_base);
        pkt->stream_index = s->video_st->index;

        // 发送给 Nginx-RTMP 服务器
        av_interleaved_write_frame(s->fmt_ctx, pkt);
        av_packet_unref(pkt); // 必须 unref 防止内存泄漏
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