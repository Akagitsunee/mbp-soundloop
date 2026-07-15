#include <portaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>

/* ── audio passthrough callback ───────────────────────────────────────── */

typedef struct {
    _Atomic uint64_t ticks;
    _Atomic int32_t  silentTick;
    _Atomic int32_t  silentFlag;
    int32_t          silenceLimit;
    float            threshold;
    float            gain;
    float            gate;     /* noise gate input-RMS threshold; 0 = off */
    float            gateGain; /* envelope follower state, [0,1] */
    int              channels;
    int              printRms;
} AudioCtx;

static AudioCtx   gCtx;
static PaStream  *gStream = NULL;

static int audioCb(const void *in, void *out,
                   unsigned long frames,
                   const PaStreamCallbackTimeInfo *ti,
                   PaStreamCallbackFlags flags,
                   void *ud)
{
    (void)ti; (void)flags;
    AudioCtx *ctx = (AudioCtx *)ud;
    int n = (int)frames * ctx->channels;
    const float *src = (const float *)in;
    float       *dst = (float *)out;
    float        g   = ctx->gain;
    for (int i = 0; i < n; i++) dst[i] = src[i] * g;
    atomic_fetch_add(&ctx->ticks, 1);

    double sum  = 0.0;
    float  peak = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += (double)src[i] * src[i];
        float a = fabsf(src[i]);
        if (a > peak) peak = a;
    }
    double rms = sqrt(sum / n);

    if (ctx->printRms) {
        uint64_t t = atomic_load(&ctx->ticks);
        if (t % 188 == 0) { /* ~every second at 48000/256 */
            printf("[rms] %.2e  peak=%.2e  thr=%.2e%s\n",
                   rms, (double)peak, (double)ctx->threshold,
                   peak >= 0.999f ? "  *** CLIP ***" : "");
            fflush(stdout);
        }
    }

    /* noise gate: smooth open/close to avoid clicking */
    if (ctx->gate > 0.0f) {
        float target = ((float)rms >= ctx->gate) ? 1.0f : 0.0f;
        float coeff  = (target > ctx->gateGain) ? 0.7f : 0.04f; /* fast open, slow close */
        ctx->gateGain += (target - ctx->gateGain) * coeff;
        for (int i = 0; i < n; i++) dst[i] *= ctx->gateGain;
    }

    if (rms < ctx->threshold) {
        if (atomic_fetch_add(&ctx->silentTick, 1) + 1 >= ctx->silenceLimit)
            atomic_store(&ctx->silentFlag, 1);
    } else {
        atomic_store(&ctx->silentTick, 0);
    }
    return paContinue;
}

int openAudioStream(int inIdx, int outIdx, int channels,
                    double rate, unsigned long bufFrames,
                    float threshold, int32_t silenceLimit, float gain, float gate, int printRms)
{
    gCtx.ticks        = 0;
    gCtx.silentTick   = 0;
    gCtx.silentFlag   = 0;
    gCtx.silenceLimit = silenceLimit;
    gCtx.threshold    = threshold;
    gCtx.gain         = gain;
    gCtx.gate         = gate;
    gCtx.gateGain     = (gate > 0.0f) ? 0.0f : 1.0f;
    gCtx.channels     = channels;
    gCtx.printRms     = printRms;

    PaStreamParameters inp = {
        .device                    = inIdx,
        .channelCount              = channels,
        .sampleFormat              = paFloat32,
        .suggestedLatency          = Pa_GetDeviceInfo(inIdx)->defaultLowInputLatency,
        .hostApiSpecificStreamInfo = NULL,
    };
    PaStreamParameters outp = {
        .device                    = outIdx,
        .channelCount              = channels,
        .sampleFormat              = paFloat32,
        .suggestedLatency          = Pa_GetDeviceInfo(outIdx)->defaultLowOutputLatency,
        .hostApiSpecificStreamInfo = NULL,
    };

    PaError err = Pa_OpenStream(&gStream, &inp, &outp,
                                rate, bufFrames, paNoFlag, audioCb, &gCtx);
    if (err != paNoError) return (int)err;

    err = Pa_StartStream(gStream);
    if (err != paNoError) { Pa_CloseStream(gStream); gStream = NULL; }
    return (int)err;
}

void closeAudioStream(void) {
    if (gStream) {
        Pa_AbortStream(gStream); /* non-blocking; Pa_StopStream can hang when CoreAudio is suspended */
        Pa_CloseStream(gStream);
        gStream = NULL;
    }
}

uint64_t    getCbTicks(void)    { return atomic_load(&gCtx.ticks); }
int         getSilentFlag(void) { return atomic_load(&gCtx.silentFlag); }

/* ── PortAudio lifecycle ───────────────────────────────────────────────── */

int  paInit(void) { return (int)Pa_Initialize(); }
void paTerm(void) { Pa_Terminate(); }

/* ── device enumeration ───────────────────────────────────────────────── */

int paFindDev(const char *substr, int wantInput) {
    int n = Pa_GetDeviceCount();
    for (int i = 0; i < n; i++) {
        const PaDeviceInfo *d = Pa_GetDeviceInfo(i);
        if (!d || !strstr(d->name, substr)) continue;
        if (wantInput  && d->maxInputChannels  > 0) return i;
        if (!wantInput && d->maxOutputChannels > 0) return i;
    }
    return -1;
}

int         paDevMaxIn(int i)  { const PaDeviceInfo *d = Pa_GetDeviceInfo(i); return d ? d->maxInputChannels  : 0; }
int         paDevMaxOut(int i) { const PaDeviceInfo *d = Pa_GetDeviceInfo(i); return d ? d->maxOutputChannels : 0; }
const char *paDevName(int i)   { const PaDeviceInfo *d = Pa_GetDeviceInfo(i); return d ? d->name : ""; }

void paListDevs(void) {
    int n = Pa_GetDeviceCount();
    for (int i = 0; i < n; i++) {
        const PaDeviceInfo *d = Pa_GetDeviceInfo(i);
        if (d) printf("  [in:%-2d out:%-2d]  %s\n",
                      d->maxInputChannels, d->maxOutputChannels, d->name);
    }
}

/* ── input signal probe (used by idle poller) ─────────────────────────── */

int paHasSignal(int devIdx, int channels, double rate, int bufFrames, float threshold) {
    PaStreamParameters inp = {
        .device                    = devIdx,
        .channelCount              = channels,
        .sampleFormat              = paFloat32,
        .suggestedLatency          = Pa_GetDeviceInfo(devIdx)->defaultLowInputLatency,
        .hostApiSpecificStreamInfo = NULL,
    };
    PaStream *s = NULL;
    if (Pa_OpenStream(&s, &inp, NULL, rate, (unsigned long)bufFrames,
                      paNoFlag, NULL, NULL) != paNoError)
        return 0;
    if (Pa_StartStream(s) != paNoError) { Pa_CloseStream(s); return 0; }

    int n = bufFrames * channels;
    float *buf = malloc((size_t)n * sizeof(float));
    if (!buf) { Pa_StopStream(s); Pa_CloseStream(s); return 0; }

    Pa_ReadStream(s, buf, (unsigned long)bufFrames); /* warmup */
    int ok = Pa_ReadStream(s, buf, (unsigned long)bufFrames) == paNoError;

    int result = 0;
    if (ok) {
        double sum = 0.0;
        for (int i = 0; i < n; i++) sum += (double)buf[i] * buf[i];
        result = sqrt(sum / n) >= threshold ? 1 : 0;
    }

    free(buf);
    Pa_StopStream(s);
    Pa_CloseStream(s);
    return result;
}
