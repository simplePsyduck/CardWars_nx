/* cw_mediandk.c -- libmediandk (AMediaCodec / AMediaExtractor / AMediaFormat),
 * which Unity 2022's VideoPlayer links for hardware video decoding.
 *
 * This port has no video decoder: every entry reports failure the way a
 * device without the codec does (null handles, AMEDIA_ERROR_UNKNOWN). Unity's
 * VideoPlayer then reports an error for that clip and the game carries on.
 * If Card Wars plays video worth having, this file is where a decoder goes
 * (NOTES.md). MIT.
 */
#include <stdint.h>
#include <stddef.h>

#define AMEDIA_ERROR_UNKNOWN (-10000)

/* AMediaFormat keys: `extern const char *AMEDIAFORMAT_KEY_X` -- a variable
 * holding the string, so the import is the address of one. */
const char *b_AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char *b_AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char *b_AMEDIAFORMAT_KEY_DURATION = "durationUs";
const char *b_AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
const char *b_AMEDIAFORMAT_KEY_HEIGHT = "height";
const char *b_AMEDIAFORMAT_KEY_LANGUAGE = "language";
const char *b_AMEDIAFORMAT_KEY_MIME = "mime";
const char *b_AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
const char *b_AMEDIAFORMAT_KEY_STRIDE = "stride";
const char *b_AMEDIAFORMAT_KEY_WIDTH = "width";

/* AMediaExtractor */
void *b_AMediaExtractor_new(void) { return NULL; }
int b_AMediaExtractor_delete(void *ex) { return 0; }
int b_AMediaExtractor_setDataSource(void *ex, const char *loc) { return AMEDIA_ERROR_UNKNOWN; }
int b_AMediaExtractor_setDataSourceFd(void *ex, int fd, int64_t off, int64_t len) { return AMEDIA_ERROR_UNKNOWN; }
size_t b_AMediaExtractor_getTrackCount(void *ex) { return 0; }
void *b_AMediaExtractor_getTrackFormat(void *ex, size_t idx) { return NULL; }
int b_AMediaExtractor_selectTrack(void *ex, size_t idx) { return AMEDIA_ERROR_UNKNOWN; }
int b_AMediaExtractor_seekTo(void *ex, int64_t us, int mode) { return AMEDIA_ERROR_UNKNOWN; }
int b_AMediaExtractor_advance(void *ex) { return 0; }
int b_AMediaExtractor_readSampleData(void *ex, uint8_t *buf, size_t cap) { return -1; }
int b_AMediaExtractor_getSampleTrackIndex(void *ex) { return -1; }
int64_t b_AMediaExtractor_getSampleTime(void *ex) { return -1; }

/* AMediaFormat */
int b_AMediaFormat_delete(void *f) { return 0; }
int b_AMediaFormat_getInt32(void *f, const char *name, int32_t *out) { return 0; }
int b_AMediaFormat_getInt64(void *f, const char *name, int64_t *out) { return 0; }
int b_AMediaFormat_getFloat(void *f, const char *name, float *out) { return 0; }
int b_AMediaFormat_getString(void *f, const char *name, const char **out) { return 0; }
void b_AMediaFormat_setInt32(void *f, const char *name, int32_t v) {}

/* AMediaCodec */
void *b_AMediaCodec_createDecoderByType(const char *mime) { return NULL; }
int b_AMediaCodec_delete(void *c) { return 0; }
int b_AMediaCodec_configure(void *c, const void *fmt, void *surf, void *crypto, uint32_t flags) { return AMEDIA_ERROR_UNKNOWN; }
int b_AMediaCodec_start(void *c) { return AMEDIA_ERROR_UNKNOWN; }
int b_AMediaCodec_stop(void *c) { return AMEDIA_ERROR_UNKNOWN; }
int b_AMediaCodec_flush(void *c) { return AMEDIA_ERROR_UNKNOWN; }
int b_AMediaCodec_dequeueInputBuffer(void *c, int64_t timeout_us) { return -1; }
int b_AMediaCodec_dequeueOutputBuffer(void *c, void *info, int64_t timeout_us) { return -1; }
void *b_AMediaCodec_getInputBuffer(void *c, size_t idx, size_t *size) { return NULL; }
void *b_AMediaCodec_getOutputBuffer(void *c, size_t idx, size_t *size) { return NULL; }
void *b_AMediaCodec_getOutputFormat(void *c) { return NULL; }
int b_AMediaCodec_queueInputBuffer(void *c, size_t idx, int off, size_t size, uint64_t t, uint32_t f) { return AMEDIA_ERROR_UNKNOWN; }
int b_AMediaCodec_releaseOutputBuffer(void *c, size_t idx, int render) { return AMEDIA_ERROR_UNKNOWN; }
