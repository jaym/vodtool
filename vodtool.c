#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>

static void usage(char* cmd_name) {
    fprintf(stderr, "%s in.file\n", cmd_name);
    exit(1);
}

static AVFormatContext* open_input_file(char* filename) {
    AVFormatContext* ctx = NULL;
    int ret;
    if((ret = avformat_open_input(&ctx, filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Could not open %s: %s\n", filename, av_err2str(ret));
        exit(1);
    }

    if((ret = avformat_find_stream_info(ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find codec parameters for %s: %s\n", filename, av_err2str(ret));
        exit(1);
    }

    return ctx;
}

static int find_best_stream(AVFormatContext* ctx, enum AVMediaType type) {
    int best_stream;

    best_stream = av_find_best_stream(ctx, type, -1, -1, NULL, 0);
    if (best_stream < 0) {
        fprintf(stderr, "Could not find stream of type %s\n", av_get_media_type_string(type));
        exit(1);
    }

    return best_stream;
}

static void seek_to_frame(AVFormatContext* ctx, AVCodecContext* dec_ctx, int video_stream, int start_frame) {
    int64_t timestamp;
    int ret;

    timestamp = av_rescale_q(start_frame, dec_ctx->time_base, ctx->streams[video_stream]->time_base);
    /** timestamp = 251000; */

    if((ret = av_seek_frame(ctx, video_stream, timestamp, AVSEEK_FLAG_BACKWARD)) < 0) {
        fprintf(stderr, "Could not seek\n");
        exit(1);
    }
}


static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize,
                     char *filename)
{
    FILE *f;
    int i;

    f = fopen(filename,"w");
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    for (i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize, f);
    fclose(f);
}


int main(int argc, char** argv) {
    AVFormatContext* input_ctx;
    AVCodecContext* dec_ctx;
    AVCodec* codec;
    AVPacket packet = {0};

    char* input_filename;
    int start_frame = 264;
    int num_frames = 0;
    int best_video_stream = 0;
    int best_audio_stream = 0;
    int ret;

    if (argc != 2) {
        usage(argv[0]);
    }

    av_register_all();

    input_filename = argv[1];

    input_ctx = open_input_file(input_filename);

    best_video_stream = find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO);
    best_audio_stream = find_best_stream(input_ctx, AVMEDIA_TYPE_AUDIO);

    av_dump_format(input_ctx, best_video_stream, input_filename, 0);
    av_dump_format(input_ctx, best_audio_stream, input_filename, 0);

    codec = avcodec_find_decoder(input_ctx->streams[best_video_stream]->codecpar->codec_id);

    if (!codec) {
        exit(1);
    }

    dec_ctx = avcodec_alloc_context3(codec);

    ret = avcodec_parameters_to_context(dec_ctx, input_ctx->streams[best_video_stream]->codecpar);

    if (ret < 0) {
        exit(1);
    }

    dec_ctx->framerate = input_ctx->streams[best_video_stream]->avg_frame_rate;

    ret = avcodec_open2(dec_ctx, codec, NULL);

    if (ret < 0) {
        fprintf(stderr, "Could not open input codec\n");
        exit(1);
    }

    seek_to_frame(input_ctx, dec_ctx, best_video_stream, start_frame);

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate frame\n");
    }

    while (ret >= 0) {

        ret = av_read_frame(input_ctx, &packet);

        if (packet.stream_index != best_video_stream) {
          av_packet_unref(&packet);
          continue;
        }

        ret = avcodec_send_packet(dec_ctx, &packet);
        if (ret < 0) {
          fprintf(stderr, "Could not send packet\n");
          exit(1);
        }

        fprintf(stderr, "packet pts=%lld;dts=%lld\n", packet.pts, packet.dts);

        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN)) {
          ret = 0;
        } else if (ret < 0) {
            fprintf(stderr, "Didn't get frame\n");
            exit(1);
        } else {
            fprintf(stderr, "frame pts=%lld\n", frame->pts);
            pgm_save(frame->data[0], frame->linesize[0],
                     frame->width, frame->height, "test.pgm");
            exit(0);
        }

        av_packet_unref(&packet);
    }



}
