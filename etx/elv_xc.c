/*
 * Test a/v transcoding pipeline
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <errno.h>
#include <pthread.h>

#include "avpipe_xc.h"
#include "avpipe_utils.h"
#include "elv_log.h"
#include "url_parser.h"
#include "elv_sock.h"

static int opened_inputs = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

extern int
do_mux(
    txparams_t *params,
    char *out_filename
);

typedef struct udp_thread_params_t {
    int             fd;             /* Socket fd to read UDP datagrams */
    elv_channel_t   *udp_channel;   /* udp channel to keep incomming UDP packets */
    socklen_t       salen;
} udp_thread_params_t;

void *
udp_thread_func(
    void *thread_params)
{
    udp_thread_params_t *params = (udp_thread_params_t *) thread_params;
    struct sockaddr     ca;
    socklen_t len;
    udp_packet_t *udp_packet;

    int pkt_num = 0;
    for ( ; ; ) {
        if (readable_timeout(params->fd, UDP_PIPE_TIMEOUT) <= 0) {
            elv_log("UDP recv timeout");
            break;
        }

        len = params->salen;
        udp_packet = (udp_packet_t *) calloc(1, sizeof(udp_packet_t));
        
        udp_packet->len = recvfrom(params->fd, udp_packet->buf, MAX_UDP_PKT_LEN, 0, &ca, &len);
        pkt_num++;
        elv_channel_send(params->udp_channel, udp_packet);
        elv_log("Received UDP packet=%d, len=%d", pkt_num, udp_packet->len);
    }

    return NULL;
}

int
in_opener(
    const char *url,
    ioctx_t *inctx)
{
    struct stat stb;
    int rc;
    int fd;
    url_parser_t url_parser;

    rc = parse_url((char *) url, &url_parser);
    if (rc) {
        elv_err("Failed to parse input url=%s", url);
        inctx->opaque = NULL;
        return -1;
    }

    /* If input url is a UDP */
    if (!strcmp(url_parser.protocol, "udp")) {
        const int           on = 1;
        socklen_t           salen;
        struct sockaddr     *sa;
        udp_thread_params_t *params;

        fd = udp_socket(url_parser.host, url_parser.port, &sa, &salen);
        if (fd < 0) {
            elv_err("Failed to open input udp url=%s error=%d", url, errno);
            inctx->opaque = NULL;
            return -1;
        }

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if ((rc = bind(fd, sa, salen)) < 0) {
            /* Can not bind, fail and exit */
            elv_err("Failed to bind UDP socket, rc=%d", rc);
            return -1;
        }

        struct timeval tv;
        tv.tv_sec = UDP_PIPE_TIMEOUT;
        tv.tv_usec = 0;
        if ((rc = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) < 0) {
            elv_err("Failed to set UDP socket timeout, rc=%d", rc);
            return -1;
        }

        size_t bufsz = UDP_PIPE_BUFSIZE;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const void *)&bufsz, (socklen_t)sizeof(bufsz)) == -1) {
            elv_err("Failed to set UDP socket buf size to=%"PRId64, bufsz);
            return -1;
        }

        elv_channel_init(&inctx->udp_channel, MAX_UDP_CHANNEL);
        inctx->opaque = (int *) calloc(1, 2*sizeof(int));
        *((int *)(inctx->opaque)) = fd;

        pthread_mutex_lock(&lock);
        opened_inputs++;
        *((int *)(inctx->opaque)+1) = opened_inputs;
        pthread_mutex_unlock(&lock);

        /* Start a thread to read into UDP channel */
        params = (udp_thread_params_t *) calloc(1, sizeof(udp_thread_params_t));
        params->fd = fd;
        params->salen = salen;
        params->udp_channel = inctx->udp_channel;

        pthread_create(&inctx->utid, NULL, udp_thread_func, params);

        elv_dbg("IN OPEN UDP fd=%d url=%s", fd, url);
        return 0;
    }

    /* If input is not file */
    if (strcmp(url_parser.protocol, "file")) {
        elv_err("Invalid input url=%s, can be only udp or file", url);
        inctx->opaque = NULL;
        return -1;
    }
    
    fd = open(url, O_RDONLY);
    if (fd < 0) {
        elv_err("Failed to open input url=%s error=%d", url, errno);
        inctx->opaque = NULL;
        return -1;
    }

    inctx->opaque = (int *) calloc(1, 2*sizeof(int));
    *((int *)(inctx->opaque)) = fd;
    if (fstat(fd, &stb) < 0) {
        free(inctx->opaque);
        return -1;
    }

    if (url != NULL)
        inctx->url = strdup(url);
    else
        /* Default file input would be assumed to be mp4 */
        inctx->url = "bogus.mp4";

    pthread_mutex_lock(&lock);
    opened_inputs++;
    *((int *)(inctx->opaque)+1) = opened_inputs;
    pthread_mutex_unlock(&lock);

    inctx->sz = stb.st_size;
    elv_dbg("IN OPEN fd=%d url=%s", fd, url);
    return 0;
}

int
in_closer(
    ioctx_t *inctx)
{
    if (!inctx->opaque)
        return 0;

    int fd = *((int *)(inctx->opaque));
    elv_dbg("IN io_close custom writer fd=%d\n", fd);
    free(inctx->opaque);
    close(fd);
    return 0;
}

int
in_read_packet(
    void *opaque,
    uint8_t *buf,
    int buf_size)
{
    ioctx_t *c = (ioctx_t *)opaque;
    int r = 0;

    if (c->udp_channel) {
        udp_packet_t *udp_packet;
        int rc;

        if (c->cur_packet) {
            r = buf_size > (c->cur_packet->len - c->cur_pread) ? (c->cur_packet->len - c->cur_pread) : buf_size;
            memcpy(buf, &c->cur_packet->buf[c->cur_pread], r);
            c->cur_pread += r;
            if (c->cur_pread == c->cur_packet->len) {
                free(c->cur_packet);
                c->cur_packet = NULL;
                c->cur_pread = 0;
            }
            c->read_bytes += r;
            c->read_pos += r;
            elv_dbg("IN READ UDP partial read=%d pos=%"PRId64" total=%"PRId64, r, c->read_pos, c->read_bytes);
            return r;        
        }

        rc = elv_channel_timed_receive(c->udp_channel, UDP_PIPE_TIMEOUT*1000000, (void **)&udp_packet);
        if (rc == ETIMEDOUT) {
            elv_dbg("TIMEDOUT in UDP rcv channel");
            return -1;
        }

        r = buf_size > udp_packet->len ? udp_packet->len : buf_size;
        c->read_bytes += r;
        c->read_pos += r;
        memcpy(buf, udp_packet->buf, r);
        if (r < udp_packet->len) {
            c->cur_packet = udp_packet;
            c->cur_pread = r;
        } else {
            free(udp_packet);
        }
        elv_dbg("IN READ UDP read=%d pos=%"PRId64" total=%"PRId64, r, c->read_pos, c->read_bytes);
        return r;
    } else {
        int fd = *((int *)(c->opaque));
        elv_dbg("IN READ buf=%p buf_size=%d fd=%d", buf, buf_size, fd);

        r = read(fd, buf, buf_size);
        if (r >= 0) {
            c->read_bytes += r;
            c->read_pos += r;
        }
        elv_dbg("IN READ read=%d pos=%"PRId64" total=%"PRId64, r, c->read_pos, c->read_bytes);
    }

    return r > 0 ? r : -1;
}

int
in_write_packet(
    void *opaque,
    uint8_t *buf,
    int buf_size)
{
    elv_dbg("IN WRITE");
    return 0;
}

int64_t
in_seek(
    void *opaque,
    int64_t offset,
    int whence)
{
    ioctx_t *c = (ioctx_t *)opaque;
    int fd = *((int *)(c->opaque));
    int64_t rc = lseek(fd, offset, whence);
    whence = whence & 0xFFFF; /* Mask out AVSEEK_SIZE and AVSEEK_FORCE */
    switch (whence) {
    case SEEK_SET:
        c->read_pos = offset; break;
    case SEEK_CUR:
        c->read_pos += offset; break;
    case SEEK_END:
        c->read_pos = c->sz - offset; break;
    default:
        elv_dbg("IN SEEK - weird seek\n");
    }

    elv_dbg("IN SEEK offset=%"PRId64" whence=%d rc=%"PRId64, offset, whence, rc);
    return rc;
}

int
in_stat(
    void *opaque,
    avp_stat_t stat_type)
{
    int64_t fd;
    ioctx_t *c = (ioctx_t *)opaque;

    fd = *((int64_t *)(c->opaque));
    elv_log("IN STAT fd=%d, read offset=%"PRId64, fd, c->read_bytes);
    return 0;
}

int
out_opener(
    const char *url,
    ioctx_t *outctx)
{
    char segname[128];
    char dir[256];
    int fd;
    ioctx_t *inctx = outctx->inctx;
    int gfd = *((int *)(inctx->opaque)+1);
    struct stat st = {0};

    sprintf(dir, "./O/O%d", gfd);
    if (stat(dir, &st) == -1) {
        mkdir(dir, 0700);
    }

    /* If there is no url, just allocate the buffers. The data will be copied to the buffers */
    switch (outctx->type) {
    case avpipe_manifest:
        /* Manifest */
        sprintf(segname, "%s/%s", dir, "dash.mpd");
        break;

    case avpipe_master_m3u:
        /* HLS master mt38 */
        sprintf(segname, "%s/%s", dir, "master.m3u8");
        break;

    case avpipe_video_init_stream:
    case avpipe_audio_init_stream:
    case avpipe_video_m3u:
    case avpipe_audio_m3u:
    case avpipe_aes_128_key:
    case avpipe_mp4_stream:
    case avpipe_fmp4_stream:
        /* Init segments, or m3u files */
        sprintf(segname, "%s/%s", dir, url);
        break;

    case avpipe_video_segment:
    case avpipe_audio_segment:
        {
            const char *segbase = "chunk-stream";

            sprintf(segname, "./%s/%s%d-%05d.m4s",
                dir, segbase, outctx->stream_index, outctx->seg_index);
        }
        break;

    case avpipe_mp4_segment:
        {
            const char *segbase = "segment";

            sprintf(segname, "./%s/%s%d-%05d.mp4",
                dir, segbase, outctx->stream_index, outctx->seg_index);
        }
        break;

    case avpipe_fmp4_segment:
        {
            const char *segbase = "fsegment";

            sprintf(segname, "./%s/%s%d-%05d.mp4",
                dir, segbase, outctx->stream_index, outctx->seg_index);
        }
        break;

    default:
        return -1;
    }

    fd = open(segname, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        elv_err("Failed to open segment file %s (%d)", segname, errno);
        return -1;
    }

    outctx->opaque = (int *) malloc(sizeof(int));
    *((int *)(outctx->opaque)) = fd;

    outctx->bufsz = 1 * 1024 * 1024;
    outctx->buf = (unsigned char *)malloc(outctx->bufsz); /* Must be malloc'd - will be realloc'd by avformat */
    elv_dbg("OUT OPEN outctx=%p, path=%s, type=%d, fd=%d, seg_index=%d\n", outctx, segname, outctx->type, fd, outctx->seg_index);
    return 0;
}

int
out_read_packet(
    void *opaque,
    uint8_t *buf,
    int buf_size)
{
    ioctx_t *outctx = (ioctx_t *)opaque;
    int fd = *(int *)outctx->opaque;
    int bread;

    elv_dbg("OUT READ buf_size=%d fd=%d", buf_size, fd);

    bread = read(fd, buf, buf_size);
    if (bread >= 0) {
        outctx->read_bytes += bread;
        outctx->read_pos += bread;
    }

    elv_dbg("OUT READ read=%d pos=%d total=%"PRId64, bread, outctx->read_pos, outctx->read_bytes);

    return bread;
}

int
out_write_packet(
    void *opaque,
    uint8_t *buf,
    int buf_size)
{
    ioctx_t *outctx = (ioctx_t *)opaque;
    int fd = *(int *)outctx->opaque;
    int bwritten;

    if (fd < 0) {
        /* If there is no space in outctx->buf, reallocate the buffer */
        if (outctx->bufsz-outctx->written_bytes < buf_size) {
            unsigned char *tmp = (unsigned char *) calloc(1, outctx->bufsz*2);
            memcpy(tmp, outctx->buf, outctx->written_bytes);
            outctx->bufsz = outctx->bufsz*2;
            free(outctx->buf);
            outctx->buf = tmp;
            elv_dbg("OUT WRITE growing the buffer to %d", outctx->bufsz);
        }

        elv_dbg("OUT WRITE MEMORY write sz=%d", buf_size);
        memcpy(outctx->buf+outctx->written_bytes, buf, buf_size);
        outctx->written_bytes += buf_size;
        outctx->write_pos += buf_size;
        bwritten = buf_size;
    }
    else {
        bwritten = write(fd, buf, buf_size);
        if (bwritten >= 0) {
            outctx->written_bytes += bwritten;
            outctx->write_pos += bwritten;
        }
    }

    elv_dbg("OUT WRITE fd=%d size=%d written=%d pos=%"PRId64" total=%"PRId64, fd, buf_size, bwritten, outctx->write_pos, outctx->written_bytes);
    return bwritten;
}

int64_t
out_seek(
    void *opaque,
    int64_t offset,
    int whence)
{
    ioctx_t *outctx = (ioctx_t *)opaque;
    int fd = *(int *)outctx->opaque;

    int rc = lseek(fd, offset, whence);
    whence = whence & 0xFFFF; /* Mask out AVSEEK_SIZE and AVSEEK_FORCE */
    switch (whence) {
    case SEEK_SET:
        outctx->read_pos = offset; break;
    case SEEK_CUR:
        outctx->read_pos += offset; break;
    case SEEK_END:
        outctx->read_pos = -1;
        elv_dbg("OUT SEEK - SEEK_END not yet implemented\n");
        break;
    default:
        elv_err("OUT SEEK - weird seek\n");
    }

    elv_dbg("OUT SEEK offset=%"PRId64" whence=%d rc=%d", offset, whence, rc);
    return rc;
}

int
out_closer(
    ioctx_t *outctx)
{
    int fd = *((int *)(outctx->opaque));
    elv_dbg("OUT CLOSE custom writer fd=%d\n", fd);
    close(fd);
    free(outctx->opaque);
    free(outctx->buf);
    return 0;
}

int
out_stat(
    void *opaque,
    avp_stat_t stat_type)
{
    ioctx_t *outctx = (ioctx_t *)opaque;
    int64_t fd = *(int64_t *)outctx->opaque;

    if (outctx->type != avpipe_video_segment &&
        outctx->type != avpipe_audio_segment &&
        outctx->type != avpipe_mp4_stream &&
        outctx->type != avpipe_fmp4_stream &&
        outctx->type != avpipe_mp4_segment &&
        outctx->type != avpipe_fmp4_segment)
        return 0;

    switch (stat_type) {
    case out_stat_bytes_written:
        elv_log("OUT STAT fd=%d, write offset=%"PRId64, fd, outctx->written_bytes);
        break;
    case out_stat_decoding_start_pts:
        elv_log("OUT STAT fd=%d, start PTS=%"PRId64, fd, outctx->decoding_start_pts);
        break;
    case out_stat_encoding_end_pts:
        elv_log("OUT STAT fd=%d, end PTS=%"PRId64, fd, outctx->encoder_ctx->input_last_pts_sent_encode);
        break;
    default:
        break;
    }
    return 0;
}

typedef struct tx_thread_params_t {
    int thread_number;
    char *filename;
    int repeats;
    txparams_t *txparams;
    avpipe_io_handler_t *in_handlers;
    avpipe_io_handler_t *out_handlers;
} tx_thread_params_t;

void *
tx_thread_func(
    void *thread_params)
{
    tx_thread_params_t *params = (tx_thread_params_t *) thread_params;
    txctx_t *txctx;
    int i;

    elv_log("TRANSCODER THREAD %d STARTS", params->thread_number);

    for (i=0; i<params->repeats; i++) {
        ioctx_t *inctx = (ioctx_t *)calloc(1, sizeof(ioctx_t));

        if (params->in_handlers->avpipe_opener(params->filename, inctx) < 0) {
            elv_err("THREAD %d, iteration %d failed to open avpipe output", params->thread_number, i+1);
            continue;
        }

        if (avpipe_init(&txctx, params->in_handlers, inctx, params->out_handlers, params->txparams, params->filename) < 0) {
            elv_err("THREAD %d, iteration %d, failed to initialize avpipe", params->thread_number, i+1);
            continue;
        }

        if (avpipe_tx(txctx, 0, 1) < 0) {
            elv_err("THREAD %d, iteration %d error in transcoding", params->thread_number, i+1);
            continue;
        }

        /* If url is UDP, then wait for UDP thread to be finished */
        if (inctx->utid) {
            pthread_join(inctx->utid, NULL);
        }

        /* Close input handler resources */
        params->in_handlers->avpipe_closer(inctx);

        elv_dbg("Releasing all the resources");
        avpipe_fini(&txctx);
    }

    elv_log("TRANSCODER THREAD %d ENDS", params->thread_number);

    return 0;
}

static tx_type_t
tx_type_from_string(
    char *tx_type_str
)
{
    if (!strcmp(tx_type_str, "all"))
        return tx_all;

    if (!strcmp(tx_type_str, "video"))
        return tx_video;

    if (!strcmp(tx_type_str, "audio"))
        return tx_audio;

    return tx_none;
}

static int
do_probe(
    char *filename,
    int seekable
)
{
    ioctx_t inctx;
    avpipe_io_handler_t in_handlers;
    txprobe_t *probe;
    int rc;

    in_handlers.avpipe_opener = in_opener;
    in_handlers.avpipe_closer = in_closer;
    in_handlers.avpipe_reader = in_read_packet;
    in_handlers.avpipe_writer = in_write_packet;
    in_handlers.avpipe_seeker = in_seek;

    memset(&inctx, 0, sizeof(ioctx_t));

    if (in_handlers.avpipe_opener(filename, &inctx) < 0) {
        rc = -1;
        goto end_probe;
    }

    rc = avpipe_probe(&in_handlers, &inctx, seekable, &probe);
    if (rc < 0) {
        printf("Error: avpipe probe failed on file %s with no valid stream.\n", filename);
        goto end_probe;
    }

    for (int i=0; i<rc; i++) {
        const char *channel_name = avpipe_channel_name(probe->stream_info[i].channels, probe->stream_info[i].channel_layout);

        if (probe->stream_info[i].codec_type != AVMEDIA_TYPE_AUDIO)
            channel_name = NULL;

        const char *profile_name = avcodec_profile_name(probe->stream_info[i].codec_id, probe->stream_info[i].profile);

        printf("Stream[%d]\n"
                "\tstream_id: %d\n"
                "\tcodec_type: %s\n"
                "\tcodec_id: %d\n"
                "\tcodec_name: %s\n"
                "\tprofile: %s\n"
                "\tlevel: %d\n"
                "\tduration_ts: %"PRId64"\n"
                "\ttime_base: %d/%d\n"
                "\tnb_frames: %"PRId64"\n"
                "\tstart_time: %"PRId64"\n"
                "\tavg_frame_rate: %d/%d\n"
                "\tframe_rate: %d/%d\n"
                "\tsample_rate: %d\n"
                "\tchannels: %d\n"
                "\tchannel_layout: %s\n"
                "\tticks_per_frame: %d\n"
                "\tbit_rate: %"PRId64"\n"
                "\twidth: %d\n"
                "\theight: %d\n"
                "\tpix_fmt: %s\n"
                "\thas_b_frames: %d\n"
                "\tfield_order: %d\n"
                "\tsample_aspect_ratio: %d:%d\n"
                "\tdisplay_aspect_ratio: %d:%d\n",
                probe->stream_info[i].stream_index,
                probe->stream_info[i].stream_id,
                av_get_media_type_string(probe->stream_info[i].codec_type),
                probe->stream_info[i].codec_id,
                probe->stream_info[i].codec_name,
                profile_name != NULL ? profile_name : "-",
                probe->stream_info[i].level,
                probe->stream_info[i].duration_ts != AV_NOPTS_VALUE ? probe->stream_info[i].duration_ts : 0,
                probe->stream_info[i].time_base.num,probe->stream_info[i].time_base.den,
                probe->stream_info[i].nb_frames,
                probe->stream_info[i].start_time != AV_NOPTS_VALUE ? probe->stream_info[i].start_time : 0,
                probe->stream_info[i].avg_frame_rate.num, probe->stream_info[i].avg_frame_rate.den,
                probe->stream_info[i].frame_rate.num, probe->stream_info[i].frame_rate.den,
                probe->stream_info[i].sample_rate,
                probe->stream_info[i].channels,
                channel_name != NULL ? channel_name : "-",
                probe->stream_info[i].ticks_per_frame,
                probe->stream_info[i].bit_rate,
                probe->stream_info[i].width,
                probe->stream_info[i].height,
                av_get_pix_fmt_name(probe->stream_info[i].pix_fmt) != NULL ? av_get_pix_fmt_name(probe->stream_info[i].pix_fmt) : "-",
                probe->stream_info[i].has_b_frames,
                probe->stream_info[i].field_order,
                probe->stream_info[i].sample_aspect_ratio.num, probe->stream_info[i].sample_aspect_ratio.den,
                probe->stream_info[i].display_aspect_ratio.num, probe->stream_info[i].display_aspect_ratio.den
                );
    }
    printf("Container\n"
        "\tformat_name: %s\n"
        "\tduration: %.5f\n",
        probe->container_info.format_name,
        probe->container_info.duration);

end_probe:
    elv_dbg("Releasing probe resources");
    /* Close input handler resources */
    in_handlers.avpipe_closer(&inctx);
    return rc;
}

static int
read_file(
    char *filename,
    char **buf
)
{
    char *lbuf;
    struct stat st;

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
        return -1;

    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    lbuf = (char *) malloc(st.st_size+1);
    int nread = read(fd, lbuf, st.st_size);
    if (nread != st.st_size) {
        close(fd);
        free(lbuf);
        return -1;
    }

    close(fd);
    lbuf[nread] = '\0';
    *buf = lbuf;
    return nread;
}

static int
read_muxing_spec(
    char *spec_filename,
    txparams_t *params
)
{
    char *buf;
    int nread = read_file(spec_filename, &buf);
    if (nread < 0)
        return -1;

    params->mux_spec = buf;
    return nread;
}

static int
read_image(
    char *overlay_filename,
    txparams_t *params
)
{
    char *buf;
    int nread = read_file(overlay_filename, &buf);
    if (nread < 0)
        return -1;

    params->watermark_overlay = buf;
    params->watermark_overlay_len = nread;
    return nread;
}

static image_type
get_image_type(
    char * image_type_str)
{
    if (strncmp(image_type_str, "png", 3) == 0 || strncmp(image_type_str, "PNG", 3) == 0)
        return png_image;

    if (strncmp(image_type_str, "jpg", 3) == 0 || strncmp(image_type_str, "JPG", 3) == 0)
        return jpg_image;
    
    if (strncmp(image_type_str, "gif", 3) == 0 || strncmp(image_type_str, "GIF", 3) == 0)
        return gif_image;

    return unknown_image;
}

static void
usage(
    char *progname,
    char *bad_flag,
    int status
)
{
    printf(
        "Invalid parameter: %s\n\n"
        "Usage: %s <params>\n"
        "\t-audio-bitrate :         (optional) Default: 128000\n"
        "\t-audio-fill-gap :        (optional) Default: 0, must be 0 or 1. It only effects if encoder is aac.\n"
        "\t-audio-index :           (optional) Default: the index of last audio stream\n"
        "\t-bitdepth :              (optional) Bitdepth of color space. Default is 8, can be 8, 10, or 12.\n"
        "\t-bypass :                (optional) Bypass transcoding. Default is 0, must be 0 or 1\n"
        "\t-command :               (optional) Directing command of etx, can be \"transcode\", \"probe\" or \"mux\" (default is transcode).\n"
        "\t-crf :                   (optional) Mutually exclusive with video-bitrate. Default: 23\n"
        "\t-crypt-iv :              (optional) 128-bit AES IV, as hex\n"
        "\t-crypt-key :             (optional) 128-bit AES key, as hex\n"
        "\t-crypt-kid :             (optional) 16-byte key ID, as hex\n"
        "\t-crypt-scheme :          (optional) Encryption scheme. Default is \"none\", can be: \"aes-128\", \"cenc\", \"cbc1\", \"cens\", \"cbcs\"\n"
        "\t-crypt-url :             (optional) Specify a key URL in the HLS manifest\n"
        "\t-d :                     (optional) Decoder name. For video default is \"h264\", can be: \"h264\", \"h264_cuvid\", \"jpeg2000\", \"hevc\"\n"
        "\t                                    For audio default is \"aac\", but for ts files should be set to \"ac3\"\n"
        "\t-duration-ts :           (optional) Default: -1 (entire stream)\n"
        "\t-e :                     (optional) Encoder name. Default is \"libx264\", can be: \"libx264\", \"libx265\", \"h264_nvenc\", \"h264_videotoolbox\"\n"
        "\t                                    For audio default is \"aac\", but for ts files should be set to \"ac3\"\n"
        "\t-enc-height :            (optional) Default: -1 (use source height)\n"
        "\t-enc-width :             (optional) Default: -1 (use source width)\n"
        "\t-equal-fduration :       (optional) Force equal frame duration. Must be 0 or 1 and only valid for \"fmp4-segment\" format\n"
        "\t-f :                     (mandatory) Input filename for transcoding. Valid formats are: a filename that points to a valid file, or udp://127.0.0.1:<port>.\n"
        "\t                                     Output goes to directory ./O\n"
        "\t-format :                (optional) Package format. Default is \"dash\", can be: \"dash\", \"hls\", \"mp4\", \"fmp4\", \"segment\", or \"fmp4-segment\"\n"
        "\t                                    Using \"segment\" format produces self contained mp4 segments with start pts from 0 for each segment\n"
        "\t                                    Using \"fmp4-segment\" format produces self contained mp4 segments with continious pts.\n"
        "\t                                    Using \"fmp4-segment\" generates segments that are appropriate for live streaming.\n"
        "\t-force-keyint :          (optional) Force IDR key frame in this interval.\n"
        "\t-master-display :        (optional) Master display, only valid if encoder is libx265.\n"
        "\t-max-cll :               (optional) Maximum Content Light Level and Maximum Frame Average Light Level, only valid if encoder is libx265.\n"
        "\t                                    This parameter is a comma separated of max-cll and max-fall (i.e \"1514,172\").\n"
        "\t-mux-spec :              (optional) Muxing spec file.\n"
        "\t-preset :                (optional) Preset string to determine compression speed. Default is \"medium\". Valid values are: \"ultrafast\", \"superfast\",\n"
        "\t                                    \"veryfast\", \"faster\", \"fast\", \"medium\", \"slow\", \"slower\", \"veryslow\".\n"
        "\t-r :                     (optional) number of repeats. Default is 1 repeat, must be bigger than 1\n"
        "\t-rc-buffer-size :        (optional)\n"
        "\t-rc-max-rate :           (optional)\n"
        "\t-sample-rate :           (optional) Default: -1. For aac output sample rate is set to input sample rate and this parameter is ignored.\n"
        "\t-seekable :              (optional) Seekable stream. Default is 0, must be 0 or 1\n"
        "\t-seg-duration-ts :       (mandatory If format is not \"segment\") segment duration time base (positive integer).\n"
        "\t-seg-duration :          (mandatory If format is \"segment\") segment duration secs (positive integer). It is used for making mp4 segments.\n"
        "\t-start-pts :             (optional) Starting PTS for output. Default is 0\n"
        "\t-start-frag-index :      (optional) Start fragment index of first segment. Default is 0\n"
        "\t-start-segment :         (optional) Start segment number >= 1, Default is 1\n"
        "\t-start-time-ts :         (optional) Default: 0\n"
        "\t-stream-id :             (optional) Default: -1, if it is valid it will be used to transcode elementary stream with that stream-id.\n"
        "\t-sync-audio-to-iframe:   (optional) Default 0, must be 0 or 1. Sync audio to first video iframe when input stream is mpegts.\n"
        "\t-t :                     (optional) Transcoding threads. Default is 1 thread, must be bigger than 1\n"
        "\t-tx-type :               (optional) Transcoding type. Default is \"all\", can be \"video\", \"audio\", or \"all\" \n"
        "\t-video-bitrate :         (optional) Mutually exclusive with crf. Default: -1 (unused)\n"
        "\t-wm-text :               (optional) Watermark text that will be presented in every video frame if it exist. It has higher priority than overlay watermark.\n"
        "\t-wm-xloc :               (optional) Watermark X location\n"
        "\t-wm-yloc :               (optional) Watermark Y location\n"
        "\t-wm-color :              (optional) Watermark font color\n"
        "\t-wm-overlay :            (optional) Watermark overlay image file. It has less priority than text watermark.\n"
        "\t-wm-overlay-type :       (optional) Watermark overlay image file type, can be \"png\", \"gif\", \"jpg\". Default is png.\n"
        "\t-wm-relative-size :      (optional) Watermark relative font/shadow size\n"
        "\t-wm-shadow :             (optional) Watermarking with shadow. Default is 1, means with shadow.\n"
        "\t-wm-shadow-color :       (optional) Watermark shadow color. Default is white.\n",
        bad_flag, progname);
    printf("\n%s version=%s\n", progname, avpipe_version());
    exit(status);
}

/*
 * Test basic decoding and encoding
 *
 * Usage: <FILE-IN> <FILE-OUT>
 */
int
main(
    int argc,
    char *argv[])
{
    pthread_t *tids;
    tx_thread_params_t thread_params;
    avpipe_io_handler_t in_handlers;
    avpipe_io_handler_t out_handlers;
    struct stat st = {0};
    int repeats = 1;
    int n_threads = 1;
    char *filename = NULL;
    int bypass_transcoding = 0;
    int seekable = 0;
    int start_segment = -1;
    char *command = "transcode";
    int i;
    int wm_shadow = 0;
    url_parser_t url_parser;

    /* Parameters */
    txparams_t p = {
        .stream_id = -1,
        .audio_bitrate = 128000,            /* Default bitrate */
        .audio_index = -1,                  /* Source audio index */
        .audio_fill_gap = 0,                /* Don't fill gap if there is JUMP */
        .bitdepth = 8,
        .crf_str = strdup("23"),            /* 1 best -> 23 standard middle -> 52 poor */
        .crypt_iv = NULL,
        .crypt_key = NULL,
        .crypt_key_url = NULL,
        .crypt_kid = NULL,
        .crypt_scheme = crypt_none,
        .dcodec = strdup(""),
        .duration_ts = -1,                  /* -1 means entire input stream, same units as input stream */
        .ecodec = strdup("libx264"),
        .enc_height = -1,                   /* -1 means use source height, other values 2160, 1080, 720 */
        .enc_width = -1,                    /* -1 means use source width, other values 3840, 1920, 1280 */
        .force_equal_fduration = 0,
        .force_keyint = 0,
        .format = strdup("dash"),
        .max_cll = NULL,
        .master_display = NULL,
        .preset = strdup("medium"),
        .rc_buffer_size = 4500000,          /* TODO - default? */
        .rc_max_rate = 6700000,             /* TODO - default? */
        .sample_rate = -1,                  /* Audio sampling rate 44100 */
        .seekable = 0,
        .seg_duration_ts = -1,              /* input argument, same units as input stream PTS */
        .start_pts = 0,
        .start_segment_str = strdup("1"),   /* 1-based */
        .start_time_ts = 0,                 /* same units as input stream PTS */
        .start_fragment_index = 0,          /* Default is zero */
        .sync_audio_to_iframe = 0,
        .tx_type = tx_none,
        .video_bitrate = -1,                /* not used if using CRF */
        .watermark_text = NULL,
        .watermark_shadow = 0,
        .overlay_filename = NULL,
        .watermark_overlay = NULL,
        .watermark_overlay_len = 0,
        .watermark_overlay_type = png_image,
    };

    i = 1;
    while (i < argc) {
        if ((int) argv[i][0] != '-' || i+1 >= argc) {
            usage(argv[0], argv[i], EXIT_FAILURE);
        }
        switch ((int) argv[i][1]) {
        case 'a':
            if (!strcmp(argv[i], "-audio-index")) {
                if (sscanf(argv[i+1], "%d", &p.audio_index) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-audio-fill-gap")) {
                if (sscanf(argv[i+1], "%d", &p.audio_fill_gap) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                if (p.audio_fill_gap != 0 && p.audio_fill_gap != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-audio-bitrate")) {
                if (sscanf(argv[i+1], "%d", &p.audio_bitrate) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else {
                usage(argv[0], argv[i], EXIT_FAILURE);
            }
            break;
        case 'b':
            if (!strcmp(argv[i], "-bypass") || !strcmp(argv[i], "-b")) {
                if (sscanf(argv[i+1], "%d", &bypass_transcoding) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                if (bypass_transcoding != 0 && bypass_transcoding != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                } else {
                    p.bypass_transcoding = bypass_transcoding;
                }
            } else if (!strcmp(argv[i], "-bitdepth")) { 
                if (sscanf(argv[i+1], "%d", &p.bitdepth) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else {
                usage(argv[0], argv[i], EXIT_FAILURE);
            }
            break;
        case 'c':
            if (!strcmp(argv[i], "-command")) {
                command = argv[i+1];
                if (strcmp(command, "transcode") && strcmp(command, "probe") && strcmp(command, "mux")) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-crf")) {
                p.crf_str = strdup(argv[i+1]);
            } else if (strcmp(argv[i], "-crypt-iv") == 0) {
                p.crypt_iv = strdup(argv[i+1]);
            } else if (strcmp(argv[i], "-crypt-key") == 0) {
                p.crypt_key = strdup(argv[i+1]);
            } else if (strcmp(argv[i], "-crypt-kid") == 0) {
                p.crypt_kid = strdup(argv[i+1]);
            } else if (strcmp(argv[i], "-crypt-scheme") == 0) {
                if (strcmp(argv[i+1], "aes-128") == 0) {
                    p.crypt_scheme = crypt_aes128;
                } else if (strcmp(argv[i+1], "cenc") == 0) {
                    p.crypt_scheme = crypt_cenc;
                } else if (strcmp(argv[i+1], "cbc1") == 0) {
                    p.crypt_scheme = crypt_cbc1;
                } else if (strcmp(argv[i+1], "cens") == 0) {
                    p.crypt_scheme = crypt_cens;
                } else if (strcmp(argv[i+1], "cbcs") == 0) {
                    p.crypt_scheme = crypt_cbcs;
                } else {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (strcmp(argv[i], "-crypt-url") == 0) {
                p.crypt_key_url = strdup(argv[i+1]);
            } else {
                usage(argv[0], argv[i], EXIT_FAILURE);
            }
            break;
        case 'd':
            if (!strcmp(argv[i], "-duration-ts")) {
                if (sscanf(argv[i+1], "%"PRId64, &p.duration_ts) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (strlen(argv[i]) > 2) {
                usage(argv[0], argv[i], EXIT_FAILURE);
            } else {
                p.dcodec = strdup(argv[i+1]);
            }
            break;
        case 'e':
            if (!strcmp(argv[i], "-enc-height")) {
                if (sscanf(argv[i+1], "%d", &p.enc_height) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-enc-width")) {
                if (sscanf(argv[i+1], "%d", &p.enc_width) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-equal-fduration")) {
                if (sscanf(argv[i+1], "%d", &p.force_equal_fduration) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                if (p.force_equal_fduration != 0 && p.force_equal_fduration != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (strlen(argv[i]) > 2) {
                usage(argv[0], argv[i], EXIT_FAILURE);
            } else {
                p.ecodec = strdup(argv[i+1]);
            }
            break;
        case 'f':
            if (!strcmp(argv[i], "-force-keyint")) {
                if (sscanf(argv[i+1], "%d", &p.force_keyint) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-format")) {
                if (strcmp(argv[i+1], "dash") == 0) {
                    p.format = strdup("dash");
                } else if (strcmp(argv[i+1], "hls") == 0) {
                    p.format = strdup("hls");
                } else if (strcmp(argv[i+1], "mp4") == 0) {
                    p.format = strdup("mp4");
                } else if (strcmp(argv[i+1], "fmp4") == 0) {
                    p.format = strdup("fmp4");
                } else if (strcmp(argv[i+1], "segment") == 0) {
                    p.format = strdup("segment");
                } else if (strcmp(argv[i+1], "fmp4-segment") == 0) {
                    p.format = strdup("fmp4-segment");
                } else {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (strlen(argv[i]) > 2) {
                usage(argv[0], argv[i], EXIT_FAILURE);
            } else {
                filename = argv[i+1];
            }
            break;
        case 'm':
            if (!strcmp(argv[i], "-mux-spec")) {
                if (read_muxing_spec(argv[i+1], &p) < 0) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-master-display")) {
                p.master_display = strdup(argv[i+1]);
            } else if (!strcmp(argv[i], "-max-cll")) {
                p.max_cll = strdup(argv[i+1]);
            } else {
                usage(argv[0], argv[i], EXIT_FAILURE);
            }
            break;
        case 'p':
            if (!strcmp(argv[i], "-preset")) {
                p.preset = strdup(argv[i+1]);
            } else {
                usage(argv[0], argv[i], EXIT_FAILURE);
            }
            break;
        case 'r':
            if (!strcmp(argv[i], "-rc-buffer-size")) {
                if (sscanf(argv[i+1], "%d", &p.rc_buffer_size) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-rc-max-rate")) {
                if (sscanf(argv[i+1], "%d", &p.rc_max_rate) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (strlen(argv[i]) > 2) {
                usage(argv[0], argv[i], EXIT_FAILURE);
            } else {
                if (sscanf(argv[i+1], "%d", &repeats) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                if (repeats < 1) usage(argv[0], argv[i], EXIT_FAILURE);
            }
            break;
        case 's':
            if (!strcmp(argv[i], "-stream-id")) {
                if (sscanf(argv[i+1], "%d", &p.stream_id) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                if (p.stream_id < 0) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-seekable")) {
                if (sscanf(argv[i+1], "%d", &seekable) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                if (seekable != 0 && seekable != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-sample-rate")) {
                if (sscanf(argv[i+1], "%d", &p.sample_rate) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-seg-duration-ts")) {
                if (sscanf(argv[i+1], "%"PRId64, &p.seg_duration_ts) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-seg-duration")) {
                int64_t seg_duration;
                if (sscanf(argv[i+1], "%"PRId64, &seg_duration) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                p.seg_duration = strdup(argv[i+1]);
            } else if (!strcmp(argv[i], "-start-pts")) {
                if (sscanf(argv[i+1], "%"PRId64, &p.start_pts) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-start-segment")) {
                p.start_segment_str = strdup(argv[i+1]);
            } else if (!strcmp(argv[i], "-start-frag-index")) {
                if (sscanf(argv[i+1], "%d", &p.start_fragment_index) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-start-time-ts")) {
                if (sscanf(argv[i+1], "%"PRId64, &p.start_time_ts) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-sync-audio-to-iframe")) {
                if (sscanf(argv[i+1], "%d", &p.sync_audio_to_iframe) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                if (p.sync_audio_to_iframe != 0 && p.sync_audio_to_iframe != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else {
                usage(argv[0], argv[i], EXIT_FAILURE);
            }
            break;
        case 't':
            if (!strcmp(argv[i], "-tx-type")) {
                if (strcmp(argv[i+1], "all") && strcmp(argv[i+1], "video") && strcmp(argv[i+1], "audio")) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                p.tx_type = tx_type_from_string(argv[i+1]);
                if (!strcmp(argv[i+1], "audio")) {
                    // If audio encoder is not "a3c" then set the default to "aac"
                    if (strcmp(p.ecodec, "ac3"))
                        p.ecodec = strdup("aac");
                }
            } else if (sscanf(argv[i+1], "%d", &n_threads) != 1) {
                usage(argv[0], argv[i], EXIT_FAILURE);
            }
            if ( n_threads < 1 ) usage(argv[0], argv[i], EXIT_FAILURE);
            break;
        case 'v':
            if (!strcmp(argv[i], "-video-bitrate")) {
                if (sscanf(argv[i+1], "%d", &p.video_bitrate) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else {
                usage(argv[0], argv[i], EXIT_FAILURE);
            }
            break;
        case 'w':
            if (!strcmp(argv[i], "-wm-text")) {
                p.watermark_text = strdup(argv[i+1]);
                p.watermark_shadow = 1;
                p.watermark_shadow_color = strdup("white"); /* Default shadow color */
            } else if (!strcmp(argv[i], "-wm-xloc")) {
                p.watermark_xloc = strdup(argv[i+1]);
            } else if (!strcmp(argv[i], "-wm-yloc")) {
                p.watermark_yloc = strdup(argv[i+1]);
            } else if (!strcmp(argv[i], "-wm-color")) {
                p.watermark_font_color = strdup(argv[i+1]);
            } else if (!strcmp(argv[i], "-wm-overlay")) {
                p.overlay_filename = strdup(argv[i+1]);
            } else if (!strcmp(argv[i], "-wm-overlay-type")) {
                p.watermark_overlay_type = get_image_type(argv[i+1]);
                if (p.watermark_overlay_type == unknown_image) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-wm-relative-size")) {
                if (sscanf(argv[i+1], "%f", &p.watermark_relative_sz) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
            } else if (!strcmp(argv[i], "-wm-shadow")) {
                if (sscanf(argv[i+1], "%d", &wm_shadow) != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                }
                if (wm_shadow != 0 && wm_shadow != 1) {
                    usage(argv[0], argv[i], EXIT_FAILURE);
                } else {
                    p.watermark_shadow = wm_shadow;
                }
            } else if (!strcmp(argv[i], "-wm-shadow-color")) {
                p.watermark_shadow_color = strdup(argv[i+1]);
            } else {
                usage(argv[0], argv[i], EXIT_FAILURE);
            }
            break;
        default:
            usage(argv[0], argv[i], EXIT_FAILURE);
        }
        i += 2;
    }

    if (filename == NULL) {
        usage(argv[0], "-f", EXIT_FAILURE);
    }

    // Set AV libs log level and handle using elv_log
    av_log_set_level(AV_LOG_DEBUG);
    connect_ffmpeg_log();

    elv_logger_open(NULL, "etx", 10, 100*1024*1024, elv_log_file);
    elv_set_log_level(elv_log_debug);

    if (!strcmp(command, "probe")) {
        return do_probe(filename, seekable);
    } else if (!strcmp(command, "mux")) {
        return do_mux(&p, filename);
    }

    if (sscanf(p.start_segment_str, "%d", &start_segment) != 1) {
        usage(argv[0], "-start_segment", EXIT_FAILURE);
    }
    if (strcmp(p.format, "segment") &&
        strcmp(p.format, "fmp4-segment") &&
        (p.seg_duration_ts <= 0 || start_segment < 1)) {
        usage(argv[0], "seg_duration_ts, start_segment", EXIT_FAILURE);
    }
    if ((!strcmp(p.format, "segment") ||
        !strcmp(p.format, "fmp4-segment")) &&
        (p.seg_duration == NULL || start_segment < 1)) {
        usage(argv[0], "seg_duration, start_segment", EXIT_FAILURE);
    }

    if (p.overlay_filename) {
        read_image(p.overlay_filename, &p);
        if (!p.watermark_overlay_len)
            usage(argv[0], "wm-overlay", EXIT_FAILURE);
    }

    /* Create O dir if doesn't exist */
    if (stat("./O", &st) == -1)
        mkdir("./O", 0700);

    elv_log("txparams:\n"
            "  audio_bitrate=%d\n"
            "  crf_str=%s\n"
            "  crypt_iv=%s\n"
            "  crypt_key=%s\n"
            "  crypt_key_url=%s\n"
            "  crypt_kid=%s\n"
            "  crypt_scheme=%d\n"
            "  dcodec=%s\n"
            "  duration_ts=%d\n"
            "  ecodec=%s\n"
            "  enc_height=%d\n"
            "  enc_width=%d\n"
            "  format=%s\n"
            "  rc_buffer_size=%d\n"
            "  rc_max_rate=%d\n"
            "  sample_rate=%d\n"
            "  seg_duration_ts=%"PRId64"\n"
            "  seg_duration=%"PRId64"\n"
            "  start_pts=%d\n"
            "  start_segment_str=%s\n"
            "  start_time_ts=%d\n"
            "  video_bitrate=%d",
        p.audio_bitrate, p.crf_str, p.crypt_iv, p.crypt_key, p.crypt_key_url,
        p.crypt_kid, p.crypt_scheme, p.dcodec, p.duration_ts, p.ecodec,
        p.enc_height, p.enc_width, p.format, p.rc_buffer_size, p.rc_max_rate,
        p.sample_rate, p.seg_duration_ts, p.seg_duration, p.start_pts,
        p.start_segment_str, p.start_time_ts, p.video_bitrate);

    in_handlers.avpipe_opener = in_opener;
    in_handlers.avpipe_closer = in_closer;
    in_handlers.avpipe_reader = in_read_packet;
    in_handlers.avpipe_writer = in_write_packet;
    in_handlers.avpipe_seeker = in_seek;
    in_handlers.avpipe_stater = in_stat;

    out_handlers.avpipe_opener = out_opener;
    out_handlers.avpipe_closer = out_closer;
    out_handlers.avpipe_reader = out_read_packet;
    out_handlers.avpipe_writer = out_write_packet;
    out_handlers.avpipe_seeker = out_seek;
    out_handlers.avpipe_stater = out_stat;

    thread_params.filename = strdup(filename);
    thread_params.repeats = repeats;
    thread_params.txparams = &p;
    thread_params.in_handlers = &in_handlers;
    thread_params.out_handlers = &out_handlers;

    if (parse_url(filename, &url_parser) != 0) {
        usage(argv[0], "-f", EXIT_FAILURE);
    }

    tids = (pthread_t *) calloc(1, n_threads*sizeof(pthread_t));

    /* If it is UDP, only run one thread */
    if (!strcmp(url_parser.protocol, "udp")) {
        tx_thread_params_t *tp = (tx_thread_params_t *) malloc(sizeof(tx_thread_params_t));
        thread_params.repeats = 1;
        *tp = thread_params;
        tp->thread_number = 1;
        pthread_create(&tids[0], NULL, tx_thread_func, tp);
        pthread_join(tids[0], NULL);
        return 0;
    }

    for (i=0; i<n_threads; i++) {
        tx_thread_params_t *tp = (tx_thread_params_t *) malloc(sizeof(tx_thread_params_t));
        *tp = thread_params;
        tp->thread_number = i+1;
        pthread_create(&tids[i], NULL, tx_thread_func, tp);
    }

    for (i=0; i<n_threads; i++) {
        pthread_join(tids[i], NULL);
    }

    free(tids);

    return 0;
}
