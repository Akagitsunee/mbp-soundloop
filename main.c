#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>

volatile int g_quit = 0;
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* audio.c */
int         paInit(void);
void        paTerm(void);
int         paFindDev(const char *substr, int wantInput);
int         paDevMaxIn(int idx);
int         paDevMaxOut(int idx);
const char *paDevName(int idx);
void        paListDevs(void);
int         paHasSignal(int devIdx, int channels, double rate, int bufFrames, float threshold);
int         openAudioStream(int inIdx, int outIdx, int channels, double rate,
                            unsigned long bufFrames, float threshold, int silenceLimit, float gain, float gate, int printRms);
void        closeAudioStream(void);
uint64_t    getCbTicks(void);
int         getSilentFlag(void);

/* power.c */
void init_power_notifications(void);
void wait_if_sleeping(void);

/* ── config ───────────────────────────────────────────────────────────── */

static const char *input_name  = "USB";
static const char *output_name = "WH-1000XM4";
static double      rate        = 48000.0;
static int         buf_frames  = 256;
static double      silence_s   = 30.0;
static float       threshold   = 2e-3f;
static double      retry_s     = 2.0;
static float       gain        = 0.07f;
static float       gate        = 0.0f;
static int         list_flag   = 0;
static int         rms_flag    = 0;

static double parse_duration_s(const char *s) {
    char *end;
    double v = strtod(s, &end);
    if (strncmp(end, "ms", 2) == 0) return v / 1000.0;
    if (*end == 'm') return v * 60.0;
    return v; /* "s" suffix or bare number → seconds */
}

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *key = argv[i];
        while (*key == '-') key++;

        if (strcmp(key, "list") == 0) { list_flag = 1; continue; }
        if (strcmp(key, "rms")  == 0) { rms_flag  = 1; continue; }

        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!val) continue;
        i++;

        if      (strcmp(key, "input")     == 0) input_name  = val;
        else if (strcmp(key, "output")    == 0) output_name = val;
        else if (strcmp(key, "rate")      == 0) rate        = strtod(val, NULL);
        else if (strcmp(key, "buf")       == 0) buf_frames  = (int)strtol(val, NULL, 10);
        else if (strcmp(key, "silence")   == 0) silence_s   = parse_duration_s(val);
        else if (strcmp(key, "threshold") == 0) threshold   = (float)strtod(val, NULL);
        else if (strcmp(key, "gain")      == 0) gain        = (float)strtod(val, NULL);
        else if (strcmp(key, "gate")      == 0) gate        = (float)strtod(val, NULL);
        else if (strcmp(key, "retry")     == 0) retry_s     = parse_duration_s(val);
    }
}

/* ── routing ──────────────────────────────────────────────────────────── */

#define OK             0
#define ERR_DISCONNECT 1
#define ERR_SILENT     2

static int route(void) {
    if (paInit() != 0) return ERR_DISCONNECT;

    int rc = ERR_DISCONNECT;

    int in_idx = paFindDev(input_name, 1);
    if (in_idx < 0) { fprintf(stderr, "input %s not found\n", input_name); goto done; }

    int out_idx = paFindDev(output_name, 0);
    if (out_idx < 0) { fprintf(stderr, "output %s not found\n", output_name); goto done; }

    int ch = paDevMaxIn(in_idx) < paDevMaxOut(out_idx)
             ? paDevMaxIn(in_idx) : paDevMaxOut(out_idx);
    if (ch > 2) ch = 2;

    int silence_limit = (int)(silence_s * rate / buf_frames);

    if (openAudioStream(in_idx, out_idx, ch, rate,
                        (unsigned long)buf_frames, threshold, silence_limit, gain, gate, rms_flag) != 0) {
        goto done;
    }

    fprintf(stderr, "active: %s → %s  [%dch @ %.0fHz buf=%d]\n",
            paDevName(in_idx), paDevName(out_idx), ch, rate, buf_frames);

    uint64_t prev = getCbTicks();
    while (!g_quit) {
        usleep(500000);
        uint64_t curr = getCbTicks();
        if (curr == prev) {
            fprintf(stderr, "stream stopped (device disconnected?)\n");
            rc = ERR_DISCONNECT;
            break;
        }
        prev = curr;
        if (getSilentFlag()) { rc = ERR_SILENT; break; }
    }

    closeAudioStream();
done:
    paTerm();
    return rc;
}

static int detect_signal(void) {
    if (paInit() != 0) return 0;
    int idx = paFindDev(input_name, 1);
    int result = 0;
    if (idx >= 0) {
        int ch = paDevMaxIn(idx);
        if (ch > 2) ch = 2;
        result = paHasSignal(idx, ch, rate, buf_frames, threshold);
    }
    paTerm();
    return result;
}

static void poll_for_signal(void) {
    fprintf(stderr, "idle: no signal — stream closed, polling\n");
    while (!g_quit) {
        wait_if_sleeping();
        usleep(500000);
        if (detect_signal()) {
            fprintf(stderr, "signal detected — reopening stream\n");
            return;
        }
    }
}

/* ── entry point ──────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    parse_args(argc, argv);

    if (list_flag) {
        if (paInit() != 0) return 1;
        printf("Available audio devices:\n");
        paListDevs();
        paTerm();
        return 0;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    init_power_notifications();
    fprintf(stderr, "soundloop: input=%s output=%s\n", input_name, output_name);

    while (!g_quit) {
        int rc = route();
        if (g_quit) break;
        if (rc == ERR_SILENT) {
            poll_for_signal();
            continue;
        }
        wait_if_sleeping();
        usleep((useconds_t)(retry_s * 1000000));
    }
    fprintf(stderr, "soundloop: exiting\n");
    return 0;
}
