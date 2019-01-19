/*
 * Test a/v transcoding pipeline
 *
 * Build:
 *
 * ELV_TOOLCHAIN_DIST_PLATFORM=...
 * make
 *
 */

#include <libavutil/log.h>

#include "avpipe_xc.h"
#include "avpipe_utils.h"
#include "elv_log.h"
#include "elv_time.h"

#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

extern int
init_filters(
    const char *filters_descr,
    coderctx_t *decoder_context,
    coderctx_t *encoder_context);

extern int
elv_io_open(
    struct AVFormatContext *s,
    AVIOContext **pb,
    const char *url,
    int flags,
    AVDictionary **options);

extern void
elv_io_close(
    struct AVFormatContext *s,
    AVIOContext *pb);

static int
prepare_input(
    avpipe_io_handler_t *in_handlers,
    ioctx_t *inctx,
    AVFormatContext *format_ctx)
{
    unsigned char *bufin;
    AVIOContext *avioctx;
    int bufin_sz = 64 * 1024;

    bufin = (unsigned char *) av_malloc(64*1024); /* Must be malloc'd - will be realloc'd by avformat */
    avioctx = avio_alloc_context(bufin, bufin_sz, 0, (void *)inctx,
        in_handlers->avpipe_reader, in_handlers->avpipe_writer, in_handlers->avpipe_seeker);

    avioctx->written = inctx->sz; /* Fake avio_size() to avoid calling seek to find size */
    format_ctx->pb = avioctx;
    return 0;
}

static int
prepare_decoder(
    coderctx_t *decoder_context,
    avpipe_io_handler_t *in_handlers,
    ioctx_t *inctx,
    txparams_t *params)
{

    decoder_context->last_dts = AV_NOPTS_VALUE;

    decoder_context->video_stream_index = -1;
    decoder_context->audio_stream_index = -1;

    decoder_context->format_context = avformat_alloc_context();
    if (!decoder_context->format_context) {
        elv_err("Could not allocate memory for Format Context");
        return -1;
    }

    /* Set our custom reader */
    prepare_input(in_handlers, inctx, decoder_context->format_context);

    /* Allocate AVFormatContext in format_context and find input file format */
    if (avformat_open_input(&decoder_context->format_context, "bogus.mp4", NULL, NULL) != 0) {
        elv_err("Could not open input file");
        return -1;
    }

    /* Retrieve stream information */
    if (avformat_find_stream_info(decoder_context->format_context,  NULL) < 0) {
        elv_err("Could not get input stream info");
        return -1;
    }

    dump_decoder(decoder_context);

    for (int i = 0; i < decoder_context->format_context->nb_streams; i++) {
        decoder_context->codec_parameters[i] = decoder_context->format_context->streams[i]->codecpar;
        decoder_context->stream[i] = decoder_context->format_context->streams[i];

        if (decoder_context->format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            /* Video */
            decoder_context->video_stream_index = i;
            elv_dbg("STREAM %d Video, codec_id=%s", i, avcodec_get_name(decoder_context->codec_parameters[i]->codec_id));
        } else if (decoder_context->format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* Audio */
            decoder_context->audio_stream_index = i;
            elv_dbg("STREAM %d Audio, codec_id=%s", i, avcodec_get_name(decoder_context->codec_parameters[i]->codec_id));
        } else {
            elv_dbg("STREAM UNKNOWN type=%d", decoder_context->format_context->streams[i]->codecpar->codec_type);
            continue;
        }

        /* Initialize codec and codec context */
        decoder_context->codec[i] = avcodec_find_decoder(decoder_context->codec_parameters[i]->codec_id);
        //decoder_context->codec[i] = avcodec_find_decoder_by_name(p->codec);

        if (!decoder_context->codec[i]) {
            elv_err("Unsupported codec");
            return -1;
        }

        decoder_context->codec_context[i] = avcodec_alloc_context3(decoder_context->codec[i]);
        if (!decoder_context->codec_context[i]) {
            elv_err("Failed to allocated memory for AVCodecContext");
            return -1;
        }

        if (avcodec_parameters_to_context(decoder_context->codec_context[i], decoder_context->codec_parameters[i]) < 0) {
            elv_err("Failed to copy codec params to codec context");
            return -1;
        }

        /* Open the decoder (initialize the decoder codec_context[i] using given codec[i]). */
        if (avcodec_open2(decoder_context->codec_context[i], decoder_context->codec[i], NULL) < 0) {
            elv_err("Failed to open codec through avcodec_open2, codec_id=%s", avcodec_get_name(decoder_context->codec_parameters[i]->codec_id));
            return -1;
        }

        /* Video - set context parameters manually */
        /* Setting the frame_rate here causes slight changes to rates - leaving it unset works perfectly
          decoder_context->codec_context[i]->framerate = av_guess_frame_rate(
            decoder_context->format_context, decoder_context->format_context->streams[i], NULL);
        */
        decoder_context->codec_context[i]->time_base = decoder_context->stream[i]->time_base;

        dump_stream(decoder_context->stream[i]);
        dump_codec_parameters(decoder_context->codec_parameters[i]);
        dump_codec_context(decoder_context->codec_context[i]);
    }

    return 0;
}

static int
prepare_video_encoder(
    coderctx_t *encoder_context,
    coderctx_t *decoder_context,
    txparams_t *params)
{
    int index = decoder_context->video_stream_index;

    if (index < 0) {
        elv_dbg("No video stream detected by decoder.");
        return 0;
    }

    encoder_context->video_stream_index = index;
    encoder_context->last_dts = AV_NOPTS_VALUE;
    encoder_context->stream[index] = avformat_new_stream(encoder_context->format_context, NULL);
    encoder_context->codec[index] = avcodec_find_encoder_by_name(params->codec);

    /* Custom output buffer */
    encoder_context->format_context->io_open = elv_io_open;
    encoder_context->format_context->io_close = elv_io_close;

    if (!encoder_context->codec[index]) {
        elv_dbg("could not find the proper codec");
        return -1;
    }

    encoder_context->codec_context[index] = avcodec_alloc_context3(encoder_context->codec[index]);
    if (!encoder_context->codec_context[index]) {
        elv_dbg("could not allocated memory for codec context");
        return -1;
    }

    AVCodecContext *encoder_codec_context = encoder_context->codec_context[index];

    /* Set encoder parameters */
    AVDictionary *encoder_options = NULL;
#if 0
    /* These are not necessary - keeping as an example */
    // TODO: how will this be freed?
    av_opt_set(&encoder_options, "keyint", "60", 0);
    av_opt_set(&encoder_options, "min-keyint", "60", 0);
    av_opt_set(&encoder_options, "no-scenecut", "-1", 0);

    /*
     * These are not necessary in order to force keyframes for the segments
     *
    av_opt_set(encoder_codec_context->priv_data, "keyint", "60", AV_OPT_FLAG_ENCODING_PARAM);
    av_opt_set(encoder_codec_context->priv_data, "min-keyint", "60", AV_OPT_FLAG_ENCODING_PARAM);
    av_opt_set(encoder_codec_context->priv_data, "no-scenecut", "-1", AV_OPT_FLAG_ENCODING_PARAM);

    av_opt_set(encoder_codec_context->priv_data, "gop_size", "60", AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_SEARCH_CHILDREN);
    av_opt_set(encoder_codec_context->priv_data, "forced_idr", "1", AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_SEARCH_CHILDREN);

    // SSTEST set these on the format context as well
    av_opt_set(encoder_context->format_context->priv_data, "keyint", "60", AV_OPT_FLAG_ENCODING_PARAM);
    av_opt_set(encoder_context->format_context->priv_data, "min-keyint", "60", AV_OPT_FLAG_ENCODING_PARAM);
    av_opt_set(encoder_context->format_context->priv_data, "no-scenecut", "-1", AV_OPT_FLAG_ENCODING_PARAM);
    av_opt_set(encoder_context->format_context->priv_data, "gop_size", "60", AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_SEARCH_CHILDREN);
    av_opt_set(encoder_context->format_context->priv_data, "forced_idr", "1", AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_SEARCH_CHILDREN);
    */
#endif

    /* Added to fix/improve encoding quality of the first frame - PENDING(SSS) research */
    av_opt_set(encoder_codec_context->priv_data, "crf", params->crf_str, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_SEARCH_CHILDREN);

    // DASH segment duration (in seconds) - notice it is set on the format context not codec
    av_opt_set(encoder_context->format_context->priv_data, "seg_duration", params->seg_duration_secs_str,
        AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_SEARCH_CHILDREN);
    av_opt_set(encoder_context->format_context->priv_data, "start_segment", params->start_segment_str, 0);

    //av_opt_set(encoder_context->format_context->priv_data, "use_timeline", "0",
    //    AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_SEARCH_CHILDREN);

    /* Set codec context parameters */
    encoder_codec_context->height = params->enc_height != -1 ? params->enc_height : decoder_context->codec_context[index]->height;
    encoder_codec_context->width = params->enc_width != -1 ? params->enc_width : decoder_context->codec_context[index]->width;
    encoder_codec_context->time_base = decoder_context->codec_context[index]->time_base;
    encoder_codec_context->sample_aspect_ratio = decoder_context->codec_context[index]->sample_aspect_ratio;
    encoder_codec_context->bit_rate = params->video_bitrate;

    if (encoder_context->codec[index]->pix_fmts) {
        encoder_codec_context->pix_fmt = encoder_context->codec[index]->pix_fmts[0];
    } else {
        encoder_codec_context->pix_fmt = decoder_context->codec_context[index]->pix_fmt;
    }

    // This needs to be set before open (ffmpeg samples have it wrong)
    if (encoder_context->format_context->oformat->flags & AVFMT_GLOBALHEADER) {
        encoder_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    /* Open video encoder (initialize the encoder codec_context[i] using given codec[i]). */
    if (avcodec_open2(encoder_context->codec_context[index], encoder_context->codec[index], &encoder_options) < 0) {
        elv_dbg("Could not open encoder for video");
        return -1;
    }

    /* This needs to happen after avcodec_open2() */
    if (avcodec_parameters_from_context(
            encoder_context->stream[index]->codecpar,
            encoder_context->codec_context[index]) < 0) {
        elv_dbg("could not copy encoder parameters to output stream");
        return -1;
    }

    /* Set stream parameters - after avcodec_open2 and parameters from context.
     * This is necessary for the output to preserve the timebase and framerate of the input.
     */
    encoder_context->stream[index]->time_base = decoder_context->stream[decoder_context->video_stream_index]->time_base;
    encoder_context->stream[index]->avg_frame_rate = decoder_context->stream[decoder_context->video_stream_index]->avg_frame_rate;

    return 0;
}

static int
prepare_audio_encoder(
    coderctx_t *encoder_context,
    coderctx_t *decoder_context,
    txparams_t *params)
{
    int index = decoder_context->audio_stream_index;

    if (index < 0) {
        elv_dbg("No audio stream detected by decoder.");
        return 0;
    }

    if (!decoder_context->codec_context[index]) {
        printf("Decoder codec context is NULL!\n");
        return -1;
    }

    encoder_context->audio_stream_index = index;
    encoder_context->last_dts = AV_NOPTS_VALUE;
    encoder_context->stream[index] = avformat_new_stream(encoder_context->format_context, NULL);
    encoder_context->codec[index] = avcodec_find_encoder(decoder_context->codec_context[index]->codec_id);
    if (!encoder_context->codec[index]) {
        printf("Codec not found, codec_id=%s\n", avcodec_get_name(decoder_context->codec_context[index]->codec_id));
        return -1;
    }

    encoder_context->format_context->io_open = elv_io_open;
    encoder_context->format_context->io_close = elv_io_close;

    encoder_context->codec_context[index] = avcodec_alloc_context3(encoder_context->codec[index]);
    //encoder_context->codec_context[index] = encoder_context->stream[index]->codec;  // Deprecated!
    encoder_context->codec_context[index]->sample_rate = decoder_context->codec_context[index]->sample_rate;
    encoder_context->codec_context[index]->time_base = decoder_context->codec_context[index]->time_base;
    encoder_context->stream[index]->time_base = encoder_context->codec_context[index]->time_base;
    if (decoder_context->codec_context[index]->sample_rate)
        encoder_context->codec_context[index]->sample_rate = decoder_context->codec_context[index]->sample_rate;
    else
        encoder_context->codec_context[index]->sample_rate = params->sample_rate;

    if (decoder_context->codec[index]->sample_fmts)
        encoder_context->codec_context[index]->sample_fmt = decoder_context->codec[index]->sample_fmts[0];
    encoder_context->codec_context[index]->channel_layout = decoder_context->codec_context[index]->channel_layout;
    encoder_context->codec_context[index]->bit_rate = params->audio_bitrate;
    encoder_context->codec_context[index]->channels = av_get_channel_layout_nb_channels(encoder_context->codec_context[index]->channel_layout);

    /* Open audio encoder codec */
    if (avcodec_open2(encoder_context->codec_context[index], encoder_context->codec[index], NULL) < 0) {
        elv_dbg("Could not open encoder for audio");
        return -1;
    }

    if (avcodec_parameters_from_context(
            encoder_context->stream[index]->codecpar,
            encoder_context->codec_context[index]) < 0) {
        elv_err("Failed to copy encoder parameters to output stream");
        return -1;

    }

    return 0;
}

static int
prepare_encoder(
    coderctx_t *encoder_context,
    coderctx_t *decoder_context,
    avpipe_io_handler_t *out_handlers,
    ioctx_t *inctx,
    txparams_t *params)
{
    out_tracker_t *out_tracker;

    /*
     * Allocate an AVFormatContext for output.
     * Setting 3th paramter to "dash" determines the output file format and avoids guessing
     * output file format using filename in ffmpeg library.
     */
    avformat_alloc_output_context2(&encoder_context->format_context, NULL, "dash", NULL);
    if (!encoder_context->format_context) {
        elv_dbg("could not allocate memory for output format");
        return -1;
    }

    if (prepare_video_encoder(encoder_context, decoder_context, params)) {
        elv_err("Failure in preparing video encoder");
        return -1;
    }

    if (prepare_audio_encoder(encoder_context, decoder_context, params)) {
        elv_err("Failure in preparing audio copy");
        return -1;
    }

    /* Allocate an array of 2 out_handler_t: one for video and one for audio output stream */
    out_tracker = (out_tracker_t *) calloc(2, sizeof(out_tracker_t));
    out_tracker[0].out_handlers = out_tracker[1].out_handlers = out_handlers;
    out_tracker[0].inctx = out_tracker[1].inctx = inctx;
    out_tracker[0].video_stream_index = out_tracker[1].video_stream_index = decoder_context->video_stream_index;
    out_tracker[0].audio_stream_index = out_tracker[1].audio_stream_index = decoder_context->audio_stream_index;
    encoder_context->format_context->avpipe_opaque = out_tracker;

    dump_encoder(encoder_context);
    dump_stream(encoder_context->stream[encoder_context->video_stream_index]);
    dump_codec_context(encoder_context->codec_context[encoder_context->video_stream_index]);
    dump_stream(encoder_context->stream[encoder_context->audio_stream_index]);
    dump_codec_context(encoder_context->codec_context[encoder_context->audio_stream_index]);

    return 0;
}

static int
encode_frame(
    coderctx_t *decoder_context,
    coderctx_t *encoder_context,
    AVFrame *frame,
    int stream_index)
{
    AVFormatContext *format_context = encoder_context->format_context;
    AVCodecContext *codec_context = encoder_context->codec_context[stream_index];
    AVPacket *output_packet = av_packet_alloc();
    if (!output_packet) {
        elv_dbg("could not allocate memory for output packet");
        return -1;
    }

    int ret;

    ret = avcodec_send_frame(codec_context, frame);

    while (ret >= 0) {
        /* The packet must be initialized before receiving */
        av_init_packet(output_packet);
        output_packet->data = NULL;
        output_packet->size = 0;

        ret = avcodec_receive_packet(codec_context, output_packet);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            elv_err("Failure while receiving a packet from the encoder: %s", av_err2str(ret));
            return -1;
        }

        /* prepare packet for muxing */
        output_packet->stream_index = stream_index;

        /*
         * Set packet duration manually if not set by the encoder.
         * The logic is borrowed from dashenc.c dash_write_packet.
         * The main problem not having a packet duration is the calculation of the track length which
         * always ends up one packet short and then all rate and duration calculcations are slightly skewed.
         * See get_cluster_duration()
         */
        assert(output_packet->duration == 0); /* Only to notice if this ever gets set */
        if (!output_packet->duration && encoder_context->last_dts != AV_NOPTS_VALUE) {
            output_packet->duration = output_packet->dts - encoder_context->last_dts;
        }
        dump_packet("OUT", output_packet);
        encoder_context->last_dts = output_packet->dts;
        encoder_context->pts = output_packet->dts;

        /* Rescale using the stream time_base (not the codec context) */
        av_packet_rescale_ts(output_packet,
            decoder_context->stream[stream_index]->time_base,
            encoder_context->stream[stream_index]->time_base
        );

        /* mux encoded frame */
        ret = av_interleaved_write_frame(format_context, output_packet);

        if (ret != 0) {
            elv_err("%d while receiving a packet from the decoder: %s", ret, av_err2str(ret));
        }
    }
    av_packet_unref(output_packet);
    av_packet_free(&output_packet);
    return 0;
}

static int
decode_packet(
    coderctx_t *decoder_context,
    coderctx_t *encoder_context,
    AVPacket *packet,
    AVFrame *frame,
    AVFrame *filt_frame,
    int stream_index,
    txparams_t *p,
    int do_instrument)
{
    int ret;
    struct timeval tv;
    u_int64_t since;
    AVCodecContext *codec_context = decoder_context->codec_context[stream_index];
    int response = avcodec_send_packet(codec_context, packet);
    elv_dbg("DECODE send_packet pts=%d dts=%d duration=%d", packet->pts, packet->dts, packet->duration);

    if (response < 0) {
        elv_err("Failure while sending a packet to the decoder: %s", av_err2str(response));
        return response;
    }

    while (response >= 0) {
        elv_get_time(&tv);
        response = avcodec_receive_frame(codec_context, frame);
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
            break;
        } else if (response < 0) {
            elv_err("Failure while receiving a frame from the decoder: %s", av_err2str(response));
            return response;
        }

        if (do_instrument) {
            elv_since(&tv, &since);
            elv_log("INSTRMNT avcodec_receive_frame time=%"PRId64, since);
        }

        if (response >= 0) {
            if (codec_context->codec_type == AVMEDIA_TYPE_VIDEO) {

                /* Force an I frame at beginning of each segment */
                if (frame->pts % p->seg_duration_ts == 0) {
                    frame->pict_type = AV_PICTURE_TYPE_I;
                    elv_dbg("FRAME SET num=%d pts=%d", frame->coded_picture_number, frame->pts);
                }

                dump_frame("IN ", codec_context->frame_number, frame);
                decoder_context->pts = frame->pts;
#if 0
                // TEST ONLY - save gray scale frame
                save_gray_frame(frame->data[0], frame->linesize[0], frame->width, frame->height,
                    "frame-in", codec_context->frame_number);
#endif

                /* push the decoded frame into the filtergraph */
                elv_get_time(&tv);
                if (av_buffersrc_add_frame_flags(decoder_context->buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    elv_err("Failure in feeding the filtergraph");
                    break;
                }
                if (do_instrument) {
                    elv_since(&tv, &since);
                    elv_log("INSTRMNT av_buffersrc_add_frame_flags time=%"PRId64, since);
                }

                /* pull filtered frames from the filtergraph */
                while (1) {
                    elv_get_time(&tv);
                    ret = av_buffersink_get_frame(decoder_context->buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    }

                    if (ret < 0) {
                        assert(0);
                        return -1;
                    }

                    if (do_instrument) {
                        elv_since(&tv, &since);
                        elv_log("INSTRMNT av_buffersink_get_frame time=%"PRId64, since);
                    }

#if 0
                    // TEST ONLY - save gray scale frame
                    save_gray_frame(filt_frame->data[0], filt_frame->linesize[0], filt_frame->width, filt_frame->height,
                        "frame-filt", codec_context->frame_number);
#endif

                    dump_frame("FILT ", codec_context->frame_number, filt_frame);

                    AVFrame *frame_to_encode = filt_frame;

                    /* To allow for packet reordering frames can come with pts past the desired duration */
                    if (p->duration_ts == -1 || frame_to_encode->pts < p->start_time_ts + p->duration_ts) {
                        elv_get_time(&tv);
                        encode_frame(decoder_context, encoder_context, frame_to_encode, stream_index);
                        if (do_instrument) {
                            elv_since(&tv, &since);
                            elv_log("INSTRMNT encode_frame time=%"PRId64, since);
                        }
                    } else {
                        elv_dbg("SS-ENCODE skip frame pts=%d filt_frame pts=%d", frame->pts, filt_frame->pts);
                    }

                    av_frame_unref(filt_frame);
                }
            }
            av_frame_unref(frame);
        }
    }
    return 0;
}

int
avpipe_tx(
    txctx_t *txctx,
    int do_instrument)
{
    /* Set scale filter */
    char filter_str[128];
    struct timeval tv;
    u_int64_t since;
    coderctx_t *decoder_context = &txctx->decoder_ctx;
    coderctx_t *encoder_context = &txctx->encoder_ctx;
    txparams_t *params = txctx->params;

    sprintf(filter_str, "scale=%d:%d",
        encoder_context->codec_context[encoder_context->video_stream_index]->width,
        encoder_context->codec_context[encoder_context->video_stream_index]->height);
    elv_dbg("FILTER scale=%s", filter_str);

    if (init_filters(filter_str, decoder_context, encoder_context) < 0) {
        elv_err("Failed to initialize the filter");
        return -1;
    }

    if (avformat_write_header(encoder_context->format_context, NULL) < 0) {
        elv_err("Failed to open output file");
        return -1;
    }

    AVFrame *input_frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();
    if (!input_frame || !filt_frame) {
        elv_err("Failed to allocated memory for AVFrame");
        return -1;
    }

    AVPacket *input_packet = av_packet_alloc();
    if (!input_packet) {
        elv_err("Failed to allocated memory for AVPacket");
        return -1;
    }

    int response = 0;

    elv_dbg("START TIME %d DURATION %d", params->start_time_ts, params->duration_ts);

    /* Seek to start position */
    if (params->start_time_ts != -1) {
        if (av_seek_frame(decoder_context->format_context,
                decoder_context->video_stream_index, params->start_time_ts, SEEK_SET) < 0) {
            elv_err("Failed seeking to desired start frame");
            return -1;
        }
    }

    if (params->start_time_ts != -1)
        encoder_context->format_context->start_time = params->start_time_ts;
    if (params->duration_ts != -1)
        encoder_context->format_context->duration = params->duration_ts;

    if (params->seg_duration_ts % params->seg_duration_fr != 0) {
        elv_err("Frame duration is not an integer");
        return -1;
    }
    int frame_duration = params->seg_duration_ts / params->seg_duration_fr;
    int extra_pts = 5 * frame_duration; /* decode extra frames to allow for reordering */

    while (av_read_frame(decoder_context->format_context, input_packet) >= 0) {
        if (input_packet->stream_index == decoder_context->video_stream_index) {
            // Video packet
            dump_packet("IN ", input_packet);

            // Stop when we reached the desired duration (duration -1 means 'entire input stream')
            if (params->duration_ts != -1 &&
                input_packet->pts >= params->start_time_ts + params->duration_ts) {
                elv_dbg("DURATION OVER param start_time=%d duration=%d pkt pts=%d\n",
                    params->start_time_ts, params->duration_ts, input_packet->pts);
                /* Allow up to 5 reoredered packets */
                if (input_packet->pts >= params->start_time_ts + params->duration_ts + extra_pts) {
                    elv_dbg("DURATION BREAK param start_time=%d duration=%d pkt pts=%d\n",
                        params->start_time_ts, params->duration_ts, input_packet->pts);
                    break;
                }
            }

            elv_get_time(&tv);
            response = decode_packet(
                decoder_context,
                encoder_context,
                input_packet,
                input_frame,
                filt_frame,
                input_packet->stream_index,
                params,
                do_instrument
            );

            if (do_instrument) {
                elv_since(&tv, &since);
                elv_log("INSTRMNT decode_packet time=%"PRId64, since);
            }

            av_packet_unref(input_packet);

            if (response < 0) {
                break;
            }

            dump_stats(decoder_context, encoder_context);
        } else {
            // Audio packet: just copying audio stream
            av_packet_rescale_ts(input_packet,
                decoder_context->stream[input_packet->stream_index]->time_base,
                encoder_context->stream[input_packet->stream_index]->time_base
            );

            if (av_interleaved_write_frame(encoder_context->format_context, input_packet) < 0) {
                elv_err("Failure in copying audio stream");
                return -1;
            }
            elv_dbg("\tfinish copying packets without reencoding");
        }
    }

    // flush all frames
    encode_frame(decoder_context, encoder_context, NULL, encoder_context->video_stream_index);
    // TODO: should I do it for the audio stream too?

    dump_stats(decoder_context, encoder_context);

    av_packet_free(&input_packet);
    av_frame_free(&input_frame);

    av_write_trailer(encoder_context->format_context);

    return 0;
}

int
avpipe_init(
    txctx_t **txctx,
    avpipe_io_handler_t *in_handlers,
    ioctx_t *inctx,
    avpipe_io_handler_t *out_handlers,
    txparams_t *params)
{
    txctx_t *p_txctx = (txctx_t *) calloc(1, sizeof(txctx_t));

    if (prepare_decoder(&p_txctx->decoder_ctx, in_handlers, inctx, params)) {
        elv_err("Failed prepared decoder");
        return -1;
    }

    if (prepare_encoder(&p_txctx->encoder_ctx, &p_txctx->decoder_ctx, out_handlers, inctx, params)) {
        elv_err("Failure in preparing output");
        return -1;
    }

    p_txctx->params = params;
    *txctx = p_txctx;
    return 0;
}

int
avpipe_fini(
    txctx_t **txctx)
{
    coderctx_t *decoder_context = &(*txctx)->decoder_ctx;
    coderctx_t *encoder_context = &(*txctx)->encoder_ctx;

    /* note: the internal buffer could have changed, and be != avio_ctx_buffer */
    if (decoder_context->format_context->pb) {
        AVIOContext *avioctx = (AVIOContext *) decoder_context->format_context->pb;
        av_freep(&avioctx->buffer);
        av_freep(&avioctx);
    }

    avformat_close_input(&decoder_context->format_context);
    avformat_free_context(decoder_context->format_context);
    avcodec_free_context(&decoder_context->codec_context[0]);
    avcodec_free_context(&decoder_context->codec_context[1]);

    avformat_close_input(&encoder_context->format_context);
    avformat_free_context(encoder_context->format_context);
    avcodec_free_context(&encoder_context->codec_context[0]);
    avcodec_free_context(&encoder_context->codec_context[1]);

    free(*txctx);
    *txctx = NULL;

    return 0;
}
