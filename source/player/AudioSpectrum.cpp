#include "AudioSpectrum.h"
#include <gccore.h>
#include <ogc/audio.h>
#include <math.h>
#include <string.h>

namespace {

const int   N      = 1024;          /* FFT size: 46.9 Hz per bin at 48 kHz */
const int   LOG2N  = 10;
const float RATE   = 48000.0f;
const float F_LOW  = 50.0f, F_HIGH = 16000.0f;
const int   MAX_BANDS = 64;

float s_win[N];                     /* Hann window */
float s_cos[N / 2], s_sin[N / 2];
bool  s_ready = false;
float s_gain  = 0.0f;               /* dB of the loudest band lately */

void init()
{
    for (int i = 0; i < N; ++i) s_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (N - 1));
    for (int i = 0; i < N / 2; ++i) {
        s_cos[i] = cosf(2.0f * (float)M_PI * i / N);
        s_sin[i] = -sinf(2.0f * (float)M_PI * i / N);
    }
    s_ready = true;
}

/* In-place radix-2 FFT */
void fft(float* re, float* im)
{
    for (int i = 1, j = 0; i < N; ++i) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int s = 1; s <= LOG2N; ++s) {
        int m = 1 << s, half = m >> 1, step = N / m;
        for (int k = 0; k < N; k += m)
            for (int j = 0; j < half; ++j) {
                float wr = s_cos[j * step], wi = s_sin[j * step];
                float* ar = re + k + j;  float* ai = im + k + j;
                float* br = ar + half;   float* bi = ai + half;
                float tr = wr * *br - wi * *bi, ti = wr * *bi + wi * *br;
                *br = *ar - tr; *bi = *ai - ti;
                *ar += tr;      *ai += ti;
            }
    }
}

} // namespace

void AudioSpectrum::reset() { s_gain = 0.0f; }

bool AudioSpectrum::analyse(float* bands, int n, float* bass)
{
    if (n > MAX_BANDS) n = MAX_BANDS;
    if (!AUDIO_GetDMAEnableFlag()) return false;
    u32 len  = AUDIO_GetDMALength();
    u32 addr = AUDIO_GetDMAStartAddr();
    u32 left = AUDIO_GetDMABytesLeft();
    if (len < (u32)N * 4 || !addr || left > len) return false;
    if (!s_ready) init();

    /* N stereo frames (4 bytes) around the playing point, inside the buffer */
    int frames = (int)(len / 4), at = (int)((len - left) / 4) - N / 2;
    if (at < 0) at = 0;
    if (at > frames - N) at = frames - N;
    const s16* pcm = (const s16*)MEM_PHYSICAL_TO_K1(addr & 0x1FFFFFFF) + at * 2;

    static float re[N], im[N];
    for (int i = 0; i < N; ++i) {
        re[i] = (pcm[i * 2] + pcm[i * 2 + 1]) * (0.5f / 32768.0f) * s_win[i];
        im[i] = 0.0f;
    }
    fft(re, im);

    /* bands: the strongest bin between log-spaced edges, in dB, tilted up
     * 3 dB an octave (music's energy falls with frequency) */
    static float db[MAX_BANDS];
    float top = -200.0f;
    const float hz = RATE / N;
    for (int b = 0; b < n; ++b) {
        float f0 = F_LOW * powf(F_HIGH / F_LOW, (float)b / n);
        float f1 = F_LOW * powf(F_HIGH / F_LOW, (float)(b + 1) / n);
        int k0 = (int)(f0 / hz), k1 = (int)(f1 / hz);
        if (k0 < 1) k0 = 1;
        if (k1 <= k0) k1 = k0 + 1;
        float m = 0.0f;
        for (int k = k0; k < k1 && k < N / 2; ++k) {
            float p = re[k] * re[k] + im[k] * im[k];
            if (p > m) m = p;
        }
        float d = 10.0f * log10f(m + 1e-12f) + 3.0f * log2f(sqrtf(f0 * f1) / 1000.0f);
        db[b] = d;
        if (d > top) top = d;
    }

    /* automatic gain: follows a louder track at once, a quieter one slowly */
    if (top > s_gain || s_gain == 0.0f) s_gain = top;
    else                                s_gain += (top - s_gain) * 0.004f;
    const float RANGE = 48.0f;             /* dB shown below the loudest */
    float low = 0.0f;
    int nLow = 0;
    for (int b = 0; b < n; ++b) {
        float v = (db[b] - (s_gain - RANGE)) / RANGE;
        bands[b] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        float fc = F_LOW * powf(F_HIGH / F_LOW, (b + 0.5f) / n);
        if (fc < 150.0f) { low += bands[b]; ++nLow; }
    }
    if (bass) *bass = nLow ? low / nLow : 0.0f;
    return true;
}
