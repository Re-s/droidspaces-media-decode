/* SPDX-License-Identifier: Apache-2.0 */
/* HEVC AU / V4L2 completion-order diagnostic. No VA frontend or software decode.
 * From vaapi-driver/: make probe-hevc-lag
 * Or: cc -O2 -g -std=c11 -Wall -Wextra -Werror -Isrc tools/probe_hevc_lag.c \
 *   src/dmd_v4l2_session.c src/v4l2_backend.c src/video_stats.c -lpthread -o probe_hevc_lag
 * Input: ffmpeg -i input -map 0:v:0 -frames:v 16 -an -sn -c:v copy \
 *   -bsf:v 'hevc_mp4toannexb,hevc_metadata=aud=insert' -f hevc sample.hevc
 * JSONL stdout; backend diagnostics stderr. Observation NEVER calls EOS/drain.
 * A separately labelled teardown drain is performed unless --no-drain is set.
 * See tools/probe_hevc_lag.md. --selftest only exercises the host AU parser.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dmd_client.h"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_AUS 64u
#define MAX_FILE_BYTES (256u * 1024u * 1024u)
struct au { size_t offset, size; unsigned vcl_nals, first_slices; };
struct observation {
    uint64_t start_ns, frames;
    unsigned unit2_first_after;
    int hardware_errors;
};

static uint64_t now_ns(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC,&time) < 0) return 0;
    return (uint64_t)time.tv_sec*1000000000u+(uint64_t)time.tv_nsec;
}

static void string(const char *s)
{
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c",c);
        else if (c < 32) printf("\\u%04x",c);
        else putchar(c);
    }
    putchar('"');
}

/* Return the complete three/four-byte prefix, not only the last 00 00 01. */
static size_t start_code(const uint8_t *data, size_t size, size_t from, size_t *prefix)
{
    for (size_t i = from; i+2 < size; ++i) {
        if (data[i] || data[i+1]) continue;
        if (data[i+2] == 1) { *prefix = 3; return i; }
        if (i+3 < size && !data[i+2] && data[i+3] == 1) { *prefix = 4; return i; }
    }
    *prefix = 0;
    return size;
}

static int validate_au(const uint8_t *data, struct au *au)
{
    size_t prefix, pos = start_code(data,au->size,0,&prefix);
    au->vcl_nals = au->first_slices = 0;
    while (pos < au->size) {
        size_t header = pos+prefix;
        if (header+2 >= au->size || (data[header]&0x80) || !(data[header+1]&7)) return -1;
        unsigned type = (data[header]>>1)&63u;
        /* An EOS/EOB NAL would change the firmware's output behavior even
         * without calling finish_input(). Keep observation genuinely EOS-free. */
        if (type == 36 || type == 37) return -1;
        if (type < 32) {
            ++au->vcl_nals;
            if (data[header+2]&0x80) ++au->first_slices;
        }
        pos = start_code(data,au->size,header+2,&prefix);
    }
    return au->vcl_nals && au->first_slices == 1 && au->size <= DMD_MAX_UNIT_BYTES ? 0 : -1;
}

static int split_aud(const uint8_t *data, size_t size, struct au *aus, unsigned limit)
{
    size_t points[MAX_AUS+1], prefix, pos = start_code(data,size,0,&prefix);
    unsigned count = 0;
    if (pos == size) return -1;
    for (size_t i = 0; i < pos; ++i) if (data[i]) return -1;
    while (pos < size) {
        size_t header = pos+prefix;
        if (header+2 > size) return -1;
        if (((data[header]>>1)&63u) == 35u) {
            /* VPS/SPS/PPS preceding the first AUD belong to the first AU. */
            points[count] = count ? pos : 0;
            ++count;
            if (count == limit+1) break;
        }
        pos = start_code(data,size,header+2,&prefix);
    }
    if (!count) return -1;
    unsigned n = count > limit ? limit : count;
    for (unsigned i = 0; i < n; ++i) {
        size_t end = i+1 < count ? points[i+1] : size;
        aus[i] = (struct au){.offset=points[i],.size=end-points[i]};
        if (validate_au(data+aus[i].offset,&aus[i]) < 0) return -1;
    }
    return (int)n;
}

static int parser_selftest(void)
{
    const uint8_t bytes[] = {
        0,0,0,1,0x40,1,0x01, /* VPS prefix before first AUD */
        0,0,1,0x46,1,0x50, 0,0,0,1,0x26,1,0x80,0x12,
        0,0,0,1,0x46,1,0x50, 0,0,1,0x02,1,0x80,0x34,
        0,0,1,0x46,1,0x50, 0,0,0,1,0x02,1,0x80,0x56
    };
    struct au au[MAX_AUS];
    int n = split_aud(bytes,sizeof(bytes),au,MAX_AUS);
    if (n != 3 || au[0].offset != 0 || au[0].size != 21 || au[1].size != 14 ||
        au[2].offset+au[2].size != sizeof(bytes)) return 1;
    if (split_aud(bytes,sizeof(bytes),au,1) != 1 || au[0].size != 21) return 1;
    const uint8_t no_aud[] = {0,0,1,0x26,1,0x80};
    if (split_aud(no_aud,sizeof(no_aud),au,MAX_AUS) >= 0) return 1;
    const uint8_t two_pictures[] = {0,0,1,0x46,1,0x50,0,0,1,0x26,1,0x80,
                                  0,0,1,0x02,1,0x80};
    if (split_aud(two_pictures,sizeof(two_pictures),au,MAX_AUS) >= 0) return 1;
    const uint8_t with_eos[] = {0,0,1,0x46,1,0x50,0,0,1,0x26,1,0x80,
                              0,0,1,0x48,1,0x80};
    if (split_aud(with_eos,sizeof(with_eos),au,MAX_AUS) >= 0) return 1;
    puts("PARSER SELFTEST PASS (AUD boundaries, prefix parameter sets, malformed AU rejection)");
    return 0;
}

static uint8_t *load(const char *path, size_t *size)
{
    FILE *file = fopen(path,"rb");
    if (!file) { perror("open input"); return NULL; }
    if (fseek(file,0,SEEK_END) < 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length <= 0 || (unsigned long)length > MAX_FILE_BYTES || fseek(file,0,SEEK_SET) < 0) {
        fprintf(stderr,"Input must be a bounded nonempty Annex-B file <=256 MiB\n");
        fclose(file); return NULL;
    }
    uint8_t *bytes = malloc((size_t)length);
    if (!bytes || fread(bytes,1,(size_t)length,file) != (size_t)length) {
        free(bytes); fclose(file); return NULL;
    }
    fclose(file); *size = (size_t)length;
    return bytes;
}

static int collect(struct dmd_session *session, struct observation *o,
                   const char *phase, unsigned after, int timeout_ms,
                   uint64_t deadline_ns)
{
    /* One output per submitted AU; extra allowance diagnoses broken accounting
     * without allowing an unbounded output loop from malfunctioning hardware. */
    unsigned rounds = 0;
    while (rounds++ < MAX_AUS*2u) {
        struct dmd_frame frame = {0};
        int rc = dmd_session_next_frame(session,&frame,timeout_ms);
        if (rc == DMD_OK) {
            ++o->frames;
            if (!strcmp(phase,"observe") && frame.unit_seq == 2 && !o->unit2_first_after)
                o->unit2_first_after = after;
            o->hardware_errors += !!frame.hardware_error;
            printf("{\"event\":\"frame\",\"phase\":\"%s\",\"after_au\":%u,"
                   "\"unit_seq\":%u,\"frame_seq\":%" PRIu64 ",\"received\":%" PRIu64 ","
                   "\"elapsed_ms\":%.3f,\"width\":%u,\"height\":%u,\"stride\":%d,"
                   "\"slice_height\":%d,\"bytes\":%zu,\"hardware_error\":%d}\n",
                   phase,after,frame.unit_seq,frame.seq,o->frames,
                   (double)(now_ns()-o->start_ns)/1000000.0,frame.width,frame.height,
                   frame.stride,frame.slice_height,frame.size,frame.hardware_error);
            rc = dmd_session_release_frame(session,&frame);
            if (rc != DMD_OK) {
                fprintf(stderr,"release_frame rc=%d: %s\n",rc,dmd_session_last_error(session));
                return -1;
            }
        } else if (rc == DMD_ERR_TIMEOUT) {
            printf("{\"event\":\"quiet\",\"phase\":\"%s\",\"after_au\":%u,"
                   "\"timeout_ms\":%d,\"received\":%" PRIu64 "}\n",phase,after,timeout_ms,o->frames);
            if (!deadline_ns || now_ns() >= deadline_ns) return 0;
        } else if (rc == DMD_EOS) {
            printf("{\"event\":\"eos\",\"phase\":\"%s\",\"after_au\":%u}\n",phase,after);
            return strcmp(phase,"observe") ? 0 : -1;
        } else {
            printf("{\"event\":\"error\",\"phase\":\"%s\",\"rc\":%d,\"message\":",phase,rc);
            string(dmd_session_last_error(session)); puts("}"); return -1;
        }
        if (deadline_ns && now_ns() >= deadline_ns) return 0;
    }
    fprintf(stderr,"Output iteration bound reached; refusing unbounded collection\n");
    return -1;
}

static unsigned number(const char *text, unsigned maximum)
{
    char *end = NULL;
    errno = 0;
    unsigned long n = strtoul(text,&end,10);
    return errno || !*text || *end || !n || n > maximum ? 0 : (unsigned)n;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    unsigned limit = 16, width = 1920, height = 1088, timeout_ms = 200;
    int inspect = 0, drain = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i],"--selftest")) return parser_selftest();
        if (!strcmp(argv[i],"--inspect")) inspect = 1;
        else if (!strcmp(argv[i],"--no-drain")) drain = 0;
        else if (!strcmp(argv[i],"--units") && i+1 < argc) limit = number(argv[++i],MAX_AUS);
        else if (!strcmp(argv[i],"--width") && i+1 < argc) width = number(argv[++i],8192);
        else if (!strcmp(argv[i],"--height") && i+1 < argc) height = number(argv[++i],4320);
        else if (!strcmp(argv[i],"--timeout-ms") && i+1 < argc) timeout_ms = number(argv[++i],2000);
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { path = NULL; break; }
    }
    if (!path || !limit || width < 96 || height < 96 || !timeout_ms) {
        fprintf(stderr,"Usage: %s input.hevc [--units 16] [--width 1920] [--height 1088] "
                "[--timeout-ms 200] [--no-drain] [--inspect] | --selftest\n",argv[0]); return 2;
    }
    size_t bytes = 0;
    uint8_t *data = load(path,&bytes);
    if (!data) return 2;
    struct au aus[MAX_AUS];
    int count = split_aud(data,bytes,aus,limit);
    if (count <= 0) {
        fprintf(stderr,"Invalid/missing AUD boundaries, source EOS/EOB, or AU lacks exactly one first slice; "
                "generate with hevc_metadata=aud=insert\n"); free(data); return 2;
    }
    setvbuf(stdout,NULL,_IOLBF,0);
    printf("{\"event\":\"config\",\"codec\":\"hevc\",\"width\":%u,\"height\":%u,"
           "\"ten_bit\":true,\"aus\":%d,\"file_bytes\":%zu,\"quiet_timeout_ms\":%u,"
           "\"observation_eos\":false,\"display_delay_env\":",width,height,count,bytes,timeout_ms);
    const char *delay = getenv("DMD_V4L2_DISPLAY_DELAY");
    if (delay) string(delay); else fputs("null",stdout);
    puts("}");
    for (int i = 0; i < count; ++i)
        printf("{\"event\":\"au\",\"input_au\":%d,\"offset\":%zu,\"bytes\":%zu,"
               "\"vcl_nals\":%u,\"first_slices\":%u}\n",i+1,aus[i].offset,aus[i].size,
               aus[i].vcl_nals,aus[i].first_slices);
    if (inspect) { free(data); return 0; }
    struct dmd_session_config cfg;
    dmd_session_config_init(&cfg);
    cfg.codec = DMD_CODEC_HEVC; cfg.width = (int)width; cfg.height = (int)height;
    cfg.ten_bit = 1; cfg.io_timeout_ms = (int)timeout_ms;
    struct dmd_error error = {0};
    struct dmd_session *session = dmd_session_create(&cfg,&error);
    if (!session) {
        fprintf(stderr,"session_create code=%d: %s\n",error.code,error.msg);
        free(data); return 1;
    }
    struct observation o = {.start_ns=now_ns()};
    unsigned sent = 0;
    int result = 0;
    for (int i = 0; i < count; ++i) {
        int rc = dmd_session_send_unit(session,data+aus[i].offset,aus[i].size);
        if (rc != DMD_OK) {
            fprintf(stderr,"send AU%d rc=%d: %s\n",i+1,rc,dmd_session_last_error(session));
            result = 1; break;
        }
        ++sent;
        printf("{\"event\":\"sent\",\"phase\":\"observe\",\"input_au\":%u,"
               "\"session_units_sent\":%" PRIu64 ",\"elapsed_ms\":%.3f}\n",sent,
               dmd_session_units_sent(session),(double)(now_ns()-o.start_ns)/1000000.0);
        if (collect(session,&o,"observe",sent,(int)timeout_ms,0) < 0) { result = 1; break; }
    }
    printf("{\"event\":\"observation_end\",\"sent_aus\":%u,\"received\":%" PRIu64 ","
           "\"unit2_first_after_au\":%u,\"hardware_errors\":%d,\"eos_sent\":false}\n",
           sent,o.frames,o.unit2_first_after,o.hardware_errors);
    /* Never move this before observation_end: STOP changes firmware behavior. */
    if (drain) {
        puts("{\"event\":\"teardown_begin\",\"note\":\"observations complete; EOS allowed now\"}");
        int rc = dmd_session_finish_input(session);
        printf("{\"event\":\"finish_input\",\"phase\":\"teardown\",\"rc\":%d}\n",rc);
        if (rc != DMD_OK || collect(session,&o,"teardown",sent,(int)timeout_ms,
                                     now_ns()+2000000000u) < 0) result = 1;
    }
    printf("{\"event\":\"destroy\",\"received_all_phases\":%" PRIu64 "}\n",o.frames);
    dmd_session_destroy(session); free(data);
    return result || o.hardware_errors ? 1 : 0;
}
