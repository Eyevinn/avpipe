/*
 * avpipe.c
 *
 * Implements generic C-GO handlers needed by avpipe GO.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <libavutil/log.h>

#include "avpipe_utils.h"
#include "avpipe_xc.h"
#include "elv_log.h"
#include "elv_channel.h"
#include "avpipe.h"

extern const char *
av_get_pix_fmt_name(
    enum AVPixelFormat pix_fmt);

extern const char *
avcodec_profile_name(
    enum AVCodecID codec_id,
    int profile);

int64_t NewIOHandler(char*, int64_t *);
int AVPipeReadInput(int64_t, uint8_t *, int);
int64_t AVPipeSeekInput(int64_t, int64_t offset, int whence);
int AVPipeCloseInput(int64_t);
int AVPipeStatInput(int64_t, avp_stat_t, void *);
int64_t AVPipeOpenOutput(int64_t, int, int, int);
int AVPipeWriteOutput(int64_t, int64_t, uint8_t *, int);
int AVPipeSeekOutput(int64_t, int64_t, int64_t offset, int whence);
int AVPipeCloseOutput(int64_t, int64_t);
int AVPipeStatOutput(int64_t, int64_t, avp_stat_t, void *);
int CLog(char *);
int CDebug(char *);
int CInfo(char *);
int CWarn(char *);
int CError(char *);

#define MIN_VALID_FD      (-4)

#define MAX_TX  128    /* Maximum transcodings per system */

typedef struct txctx_entry_t {
    int32_t         handle;
    txctx_t         *txctx;
    int             done;
} txctx_entry_t;

txctx_entry_t          *tx_table[MAX_TX];
static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;

int
in_stat(
    void *opaque,
    avp_stat_t stat_type);

int
in_opener(
    const char *url,
    ioctx_t *inctx)
{
    int64_t size;

#ifdef CHECK_C_READ
    struct stat stb;
    int fd = open(url, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    /* Allocate space for both the AVPipeHandler and fd */
    inctx->opaque = (int *) calloc(1, sizeof(int)+sizeof(int64_t));
    *((int *)(((int64_t)inctx->opaque)+1)) = fd;
    if (fstat(fd, &stb) < 0)
        return -1;

    inctx->sz = stb.st_size;
    elv_dbg("IN OPEN fd=%d", fd);
#else
    inctx->opaque = (void *) calloc(1, sizeof(int64_t));
#endif

    if (url != NULL)
        inctx->url = strdup(url);
    else
        /* Default file input would be assumed to be mp4 */
        inctx->url = "bogus.mp4";

    int64_t h = NewIOHandler((char *) url, &size);
    if (h <= 0 )
        return -1;

    if (size > 0)
        inctx->sz = size;
    elv_dbg("IN OPEN h=%"PRId64", size=%"PRId64, h, size);

    *((int64_t *)(inctx->opaque)) = h;
    return 0;
}

int
in_closer(
    ioctx_t *inctx)
{
#ifdef CHECK_C_READ
    int fd = *((int *)(((int64_t *)inctx->opaque)+1));
    elv_dbg("IN io_close custom reader fd=%d", fd);
    free(inctx->opaque);
    close(fd);
#endif

    int64_t h = *((int64_t *)(inctx->opaque));
    elv_dbg("IN io_close custom reader h=%d", h);
    AVPipeCloseInput(h);
    return 0;
}

int
in_read_packet(
    void *opaque,
    uint8_t *buf,
    int buf_size)
{
    ioctx_t *c = (ioctx_t *)opaque;
    int r;
    int64_t fd;

#if TRACE_IO
    elv_dbg("IN READ buf=%p, size=%d", buf, buf_size);
#endif

#ifdef CHECK_C_READ
    char *buf2 = (char *) calloc(1, buf_size);
    int fd2 = *((int *)(c->opaque+1));
    elv_dbg("IN READ buf_size=%d fd=%d", buf_size, fd);
    int n = read(fd2, buf2, buf_size);
#endif

    fd = *((int64_t *)(c->opaque));
    r = AVPipeReadInput(fd, buf, buf_size);
    if (r > 0) {
        c->read_bytes += r;
        c->read_pos += r;
    }

    if (c->read_bytes - c->read_reported > BYTES_READ_REPORT) {
        in_stat(opaque, in_stat_bytes_read);
        c->read_reported = c->read_bytes;
    }

#ifdef CHECK_C_READ
    if ( r == n) {
        for (int i=0; i<r; i++) {
            if ( i< 10)
                elv_log("i=%d, buf=%d, buf2=%d", i, buf[i], buf2[i]);
            if ( buf[i] != buf2[i]) {
                elv_log("NOOOO! buffers don't match");
                break;
            }
        }
    }
    free(buf2);
#endif

#if TRACE_IO
    elv_dbg("IN READ read=%d pos=%"PRId64" total=%"PRId64, r, c->read_pos, c->read_bytes);
#endif
    return r > 0 ? r : -1;
}

int
in_write_packet(
    void *opaque,
    uint8_t *buf,
    int buf_size)
{
    elv_err("IN WRITE");
    return 0;
}

int64_t
in_seek(
    void *opaque,
    int64_t offset,
    int whence)
{
    int64_t fd;
    ioctx_t *c = (ioctx_t *)opaque;
    int64_t rc;

    fd = *((int64_t *)(c->opaque));
    rc = AVPipeSeekInput(fd, offset, whence);
    if (rc < 0)
        return rc;

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

    elv_dbg("IN SEEK offset=%"PRId64", whence=%d, rc=%"PRId64, offset, whence, rc);

    return rc;
}

int
in_stat(
    void *opaque,
    avp_stat_t stat_type)
{
    int64_t fd;
    ioctx_t *c = (ioctx_t *)opaque;
    int64_t rc;

    fd = *((int64_t *)(c->opaque));
    rc = AVPipeStatInput(fd, stat_type, &c->read_bytes);
    return rc;
}

int
out_opener(
    const char *url,
    ioctx_t *outctx)
{
    ioctx_t *inctx = outctx->inctx;
    int64_t fd;
    int64_t h;

    h = *((int64_t *)(inctx->opaque));

    /* Allocate the buffers. The data will be copied to the buffers */
    outctx->bufsz = 1 * 1024 * 1024;
    outctx->buf = (unsigned char *)malloc(outctx->bufsz); /* Must be malloc'd - will be realloc'd by avformat */

    fd = AVPipeOpenOutput(h, outctx->stream_index, outctx->seg_index, outctx->type);
    elv_dbg("OUT out_opener outctx=%p, fd=%"PRId64, outctx, fd);
    if (fd < 0) {
	elv_err("AVPIPE OUT OPEN failed stream_index=%d, seg_index=%d, type=%d",
            outctx->stream_index, outctx->seg_index, outctx->type);
        return -1;
    }

    outctx->opaque = (int *) malloc(sizeof(int64_t));
    *((int64_t *)(outctx->opaque)) = fd;

    return 0;
}

int
out_read_packet(
    void *opaque,
    uint8_t *buf,
    int buf_size)
{
    elv_err("OUT READ called");
    return 0;
}

int
out_write_packet(
    void *opaque,
    uint8_t *buf,
    int buf_size)
{
    ioctx_t *outctx = (ioctx_t *)opaque;
    ioctx_t *inctx = outctx->inctx;
    int64_t h = *((int64_t *)(inctx->opaque));
    int64_t fd = *(int64_t *)outctx->opaque;
    int bwritten = AVPipeWriteOutput(h, fd, buf, buf_size);
    if (bwritten >= 0) {
        outctx->written_bytes += bwritten;
        outctx->write_pos += bwritten;
    }

#if TRACE_IO
    elv_dbg("OUT WRITE fd=%"PRId64", size=%d written=%d pos=%d total=%d", fd, buf_size, bwritten, outctx->write_pos, outctx->written_bytes);
#endif

    return buf_size;
}

int64_t
out_seek(
    void *opaque,
    int64_t offset,
    int whence)
{
    ioctx_t *outctx = (ioctx_t *)opaque;
    ioctx_t *inctx = outctx->inctx;
    int64_t h = *((int64_t *)(inctx->opaque));
    int64_t fd = *(int64_t *)outctx->opaque;
    int rc = AVPipeSeekOutput(h, fd, offset, whence);
    whence = whence & 0xFFFF; /* Mask out AVSEEK_SIZE and AVSEEK_FORCE */
    switch (whence) {
    case SEEK_SET:
        outctx->write_pos = offset; break;
    case SEEK_CUR:
        outctx->write_pos += offset; break;
    case SEEK_END:
        outctx->write_pos = outctx->sz - offset; break;
    default:
        elv_dbg("OUT SEEK - weird seek\n");
    }

    elv_dbg("OUT SEEK fd=%"PRId64" offset=%d whence=%d", fd, offset, whence);

    return rc;
}

int
out_closer(
    ioctx_t *outctx)
{
    int64_t fd = *(int64_t *)outctx->opaque;
    ioctx_t *inctx = outctx->inctx;
    int64_t h = *((int64_t *)(inctx->opaque));
    int rc = AVPipeCloseOutput(h, fd);
    free(outctx->opaque);
    free(outctx->buf);
    return rc;
}

int
out_stat(
    void *opaque,
    avp_stat_t stat_type)
{
    ioctx_t *outctx = (ioctx_t *)opaque;
    ioctx_t *inctx = outctx->inctx;
    int64_t h = *((int64_t *)(inctx->opaque));
    int64_t fd = *(int64_t *)outctx->opaque;
    int64_t rc = 0;

    fd = *((int64_t *)(outctx->opaque));
    switch (stat_type) {
    case out_stat_bytes_written:
        rc = AVPipeStatOutput(h, fd, stat_type, &outctx->written_bytes);
        break;
    case out_stat_decoding_start_pts:
        rc = AVPipeStatOutput(h, fd, stat_type, &outctx->decoding_start_pts);
        break;
    case out_stat_encoding_end_pts:
        rc = AVPipeStatOutput(h, fd, stat_type, &outctx->encoder_ctx->input_last_pts_sent_encode);
        break;
    default:
        break;
    }
    return rc;
}

void
set_loggers()
{
    elv_set_log_func(elv_log_log, CLog);
    elv_set_log_func(elv_log_debug, CDebug);
    elv_set_log_func(elv_log_warning, CWarn);
    elv_set_log_func(elv_log_error, CError);
}

static void
init_tx_module()
{
    static int initialized = 0;

    if (initialized)
        return;
    srand(time(0));
    initialized = 1;
}

/*
 * Puts txctx in the tx table and returns the handle to txctx entry.
 * If tx table is full, it would return -1.
 */
static int32_t
tx_table_put(
    txctx_t *txctx)
{
    txctx_entry_t *txe = NULL;
    txctx->index = -1;

    pthread_mutex_lock(&tx_mutex);
    for (int i=0; i<MAX_TX; i++) {
        if (tx_table[i] == NULL) {
            txe = calloc(1, sizeof(txctx_entry_t));
            txe->handle = rand();
            if (txe->handle < 0)
                txe->handle = (-1) * txe->handle;
            txe->txctx = txctx;
            tx_table[i] = txe;
            txctx->index = i;
            txctx->handle = txe->handle;
            break;
        }
    }
    pthread_mutex_unlock(&tx_mutex);

    if (txe != NULL)
        return txe->handle;
    return -1;
}

static txctx_entry_t*
tx_table_find(
    int32_t handle)
{
    pthread_mutex_lock(&tx_mutex);
    for (int i=0; i<MAX_TX; i++) {
        if (tx_table[i] != NULL && tx_table[i]->handle == handle) {
            pthread_mutex_unlock(&tx_mutex);
            return tx_table[i];
        }
    }
    pthread_mutex_unlock(&tx_mutex);

    return NULL;
}

static void
tx_table_free(
    int32_t handle)
{
    pthread_mutex_lock(&tx_mutex);
    for (int i=0; i<MAX_TX; i++) {
        if (tx_table[i] != NULL && tx_table[i]->handle == handle) {
            if (tx_table[i]->txctx->index == i) {
                free(tx_table[i]);
                tx_table[i] = NULL;
            } else
                elv_err("tx_table_free index=%d doesn't match with hadnle=%d at %d",
                    tx_table[i]->txctx->index, handle, i);
            pthread_mutex_unlock(&tx_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&tx_mutex);
}

static int
tx_table_cancel(
    int32_t handle)
{
    int rc = 0;
    pthread_mutex_lock(&tx_mutex);
    for (int i=0; i<MAX_TX; i++) {
        if (tx_table[i] != NULL && tx_table[i]->handle == handle) {
            txctx_t *txctx = tx_table[i]->txctx;

            if (txctx->index == i) {
                txctx->decoder_ctx.cancelled = 1;
                txctx->encoder_ctx.cancelled = 1;
            } else {
                elv_err("tx_table_cancel index=%d doesn't match with hadnle=%d at %d",
                    tx_table[i]->txctx->index, handle, i);
                rc = -1;
            }
            pthread_mutex_unlock(&tx_mutex);
            return rc;
        }
    }
    pthread_mutex_unlock(&tx_mutex);
    return rc;
}

static void
set_handlers(
    avpipe_io_handler_t **p_in_handlers,
    avpipe_io_handler_t **p_out_handlers)
{
    if (p_in_handlers) {
        avpipe_io_handler_t *in_handlers = (avpipe_io_handler_t *)calloc(1, sizeof(avpipe_io_handler_t));
        in_handlers->avpipe_opener = in_opener;
        in_handlers->avpipe_closer = in_closer;
        in_handlers->avpipe_reader = in_read_packet;
        in_handlers->avpipe_writer = in_write_packet;
        in_handlers->avpipe_seeker = in_seek;
        in_handlers->avpipe_stater = in_stat;
        *p_in_handlers = in_handlers;
    }

    if (p_out_handlers) {
        avpipe_io_handler_t *out_handlers = (avpipe_io_handler_t *)calloc(1, sizeof(avpipe_io_handler_t));
        out_handlers->avpipe_opener = out_opener;
        out_handlers->avpipe_closer = out_closer;
        out_handlers->avpipe_reader = out_read_packet;
        out_handlers->avpipe_writer = out_write_packet;
        out_handlers->avpipe_seeker = out_seek;
        out_handlers->avpipe_stater = out_stat;
        *p_out_handlers = out_handlers;
    }
}

/*
 * Returns a handle that refers to an initialized trasncoding session.
 * If initialization is not successfull it return -1.
 */
int32_t
tx_init(
    txparams_t *params,
    char *filename,
    int debug_frame_level)
{
    txctx_t *txctx = NULL;
    int64_t rc = 0;
    uint32_t handle;
    avpipe_io_handler_t *in_handlers = NULL;
    avpipe_io_handler_t *out_handlers = NULL;

    if (!filename || filename[0] == '\0' )
        return -1;

    init_tx_module();

    connect_ffmpeg_log();
    set_handlers(&in_handlers, &out_handlers);

    ioctx_t *inctx = (ioctx_t *)calloc(1, sizeof(ioctx_t));

    if (in_handlers->avpipe_opener(filename, inctx) < 0) {
        goto end_tx_init;
    }

    if (avpipe_init(&txctx, in_handlers, inctx, out_handlers, params, filename) < 0) {
        goto end_tx_init;
    }

    if ((handle = tx_table_put(txctx)) < 0) {
        elv_err("tx_init tx_table is full, cancelling transcoding");
        goto end_tx_init;
    }

    txctx->in_handlers = in_handlers;
    txctx->out_handlers = out_handlers;
    txctx->inctx = inctx;
    txctx->debug_frame_level = debug_frame_level;

    return handle;

end_tx_init:
    /* Close input handler resources */
    in_handlers->avpipe_closer(inctx);

    elv_dbg("Releasing all the resources");
    avpipe_fini(&txctx);
    free(in_handlers);
    free(out_handlers);

    return -1;
}

int
tx_run(
    int32_t handle)
{
    int rc = 0;
    txctx_entry_t *txe = tx_table_find(handle);

    if (!txe) {
        elv_err("tx_run invalid handle=%d", handle);
        return -1;
    }

    txctx_t *txctx = txe->txctx;
    if (avpipe_tx(txctx, 0, txctx->debug_frame_level) < 0) {
        elv_err("Error in transcoding");
        rc = -1;
        goto end_tx;
    }

end_tx:
    /* Close input handler resources */
    txctx->in_handlers->avpipe_closer(txctx->inctx);

    elv_dbg("Releasing all the resources");
    tx_table_free(handle);
    avpipe_fini(&txctx);

    return rc;
}

int
tx_cancel(
    int32_t handle)
{ 
    return tx_table_cancel(handle);
}

/*
 * 1) Initializes avpipe with appropriate parameters.
 * 2) Invokes avpipe trnascoding.
 * 3) Releases avpipe resources.
 */
int
tx(
    txparams_t *params,
    char *filename,
    int debug_frame_level)
{
    txctx_t *txctx = NULL;
    int rc = 0;
    avpipe_io_handler_t *in_handlers;
    avpipe_io_handler_t *out_handlers;

    if (!filename || filename[0] == '\0' )
        return -1;

    // Note: If log handler functions are set, log levels set through
    //       av_log_set_level and elv_set_log_level are ignored
    //av_log_set_level(AV_LOG_DEBUG);
    connect_ffmpeg_log();
    //elv_set_log_level(elv_log_debug);

    set_handlers(&in_handlers, &out_handlers);
    ioctx_t *inctx = (ioctx_t *)calloc(1, sizeof(ioctx_t));

    if (in_handlers->avpipe_opener(filename, inctx) < 0) {
        rc = -1;
        goto end_tx;
    }

    if (avpipe_init(&txctx, in_handlers, inctx, out_handlers, params, filename) < 0) {
        rc = -1;
        goto end_tx;
    }

    txctx->in_handlers = in_handlers;
    txctx->out_handlers = out_handlers;
    txctx->inctx = inctx;

    if (avpipe_tx(txctx, 0, debug_frame_level) < 0) {
        elv_err("Error in transcoding");
        rc = -1;
        goto end_tx;
    }

end_tx:
    /* Close input handler resources */
    in_handlers->avpipe_closer(inctx);

    elv_dbg("Releasing all the resources");
    avpipe_fini(&txctx);
    free(in_handlers);
    free(out_handlers);

    return rc;
}

const char *
get_pix_fmt_name(
    int pix_fmt)
{
    return av_get_pix_fmt_name((enum AVPixelFormat) pix_fmt);
}

const char *
get_profile_name(
    int codec_id,
    int profile)
{
    return avcodec_profile_name((enum AVCodecID) codec_id, profile);
}

int
probe(
    char *filename,
    int seekable,
    txprobe_t **txprobe)
{
    ioctx_t inctx;
    avpipe_io_handler_t *in_handlers;
    txprobe_t *probes;
    int rc;

    set_handlers(&in_handlers, NULL);
    if (in_handlers->avpipe_opener(filename, &inctx) < 0) {
        rc = -1;
        goto end_probe;
    }

    rc = avpipe_probe(in_handlers, &inctx, seekable, &probes);
    if (rc < 0)
        goto end_probe;

    *txprobe = probes;

end_probe:
    elv_dbg("Releasing probe resources");
    /* Close input handler resources */
    in_handlers->avpipe_closer(&inctx);
    free(in_handlers);
    return rc;
}
