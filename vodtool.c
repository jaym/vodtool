#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

static void usage(char* cmd_name) {
    fprintf(stderr, "usage: %s [options] infile\n", cmd_name);
    fprintf(stderr, "\n");
    fprintf(stderr, "Extracts and transcodes the specified segment");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t-d, --duration\tThe duration in timescale units of each segment.\tDefault Value: 5\n");
    fprintf(stderr, "\t-t, --timescale\tThe number of units in a second.\tDefault Value: 1\n");
    fprintf(stderr, "\t-s, --segment\tThe segment to fetch.\tDefault Value:0\n");

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

/**
 * Seek to the specified timestamp. This will seek to the closest key frame that is before or
 * equal to the specified timestamp.
 *
 * The timestamp is in AV_TIME_BASE units
 */
static void seek_to_timestamp(AVFormatContext* ctx, AVCodecContext* dec_ctx, int64_t max_timestamp) {
    int ret;

    if((ret = avformat_seek_file(ctx, -1, 0, max_timestamp, max_timestamp, 0)) < 0) {
        fprintf(stderr, "Could not seek\n");
        exit(1);
    }
}

/**
 * Convert frame the specified timebase to AV_TIME_BASE
 */
static inline int64_t to_av_timebase(int64_t timestamp, AVRational timebase) {
    return av_rescale_q(timestamp, timebase, (AVRational){1, AV_TIME_BASE});
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
    int duration = 5;
    int timescale = 1;
    int segment = 0;
    int64_t start_timestamp;
    int64_t end_timestamp;
    int best_video_stream = 0;
    int best_audio_stream = 0;
    int ret;

    static struct option long_options[] = {
        {"duration", required_argument, 0, 'd'},
        {"timescale", required_argument, 0, 't'},
        {"segment", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };
    int option;
    while ((option = getopt_long(argc, argv, "d:t:s:h?", long_options, NULL)) != -1) {
        switch (option) {
            case 'd':
                duration = atoi(optarg);
                break;
            case 't':
                timescale = atoi(optarg);
                break;
            case 's':
                segment = atoi(optarg);
                break;
            case 'h':
            case '?':
                usage(argv[0]);
                break;
        }
    }

    if (argc - optind != 1) {
        usage(argv[0]);
    }

    av_register_all();

    input_filename = argv[optind];

    input_ctx = open_input_file(input_filename);

    best_video_stream = find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO);
    best_audio_stream = find_best_stream(input_ctx, AVMEDIA_TYPE_AUDIO);

    av_dump_format(input_ctx, best_video_stream, input_filename, 0);
    av_dump_format(input_ctx, best_audio_stream, input_filename, 0);

    for (int i = 0; i < input_ctx->nb_streams; i++) {
        if (i != best_video_stream && i != best_audio_stream) {
            input_ctx->streams[i]->discard = AVDISCARD_ALL;
        }
    }

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

    start_timestamp = to_av_timebase(segment, (AVRational){duration, timescale});
    end_timestamp = to_av_timebase(segment+1, (AVRational){duration, timescale});

    fprintf(stderr, "start_timestamp=%lld;end_timestamp=%lld\n", start_timestamp, end_timestamp);
    seek_to_timestamp(input_ctx, dec_ctx, start_timestamp);

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

#ifdef DEBUG
        fprintf(stderr, "packet pts=%lld;dts=%lld\n", packet.pts, packet.dts);
#endif

        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN)) {
          ret = 0;
        } else if (ret < 0) {
            fprintf(stderr, "Didn't get frame\n");
            exit(1);
        } else {
          if (av_compare_ts(frame->pts, input_ctx->streams[packet.stream_index]->time_base, start_timestamp, (AVRational){1, AV_TIME_BASE}) >= 0) {

              fprintf(stderr, "saving frame av base timestamp=%lld\n", to_av_timebase(frame->pts, input_ctx->streams[packet.stream_index]->time_base));
              pgm_save(frame->data[0], frame->linesize[0],
                  frame->width, frame->height, "test.pgm");
              exit(0);
            }
        }

        av_packet_unref(&packet);
    }
}
