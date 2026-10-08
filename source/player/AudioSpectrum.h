#pragma once

/* -----------------------------------------------------------------------
 * AudioSpectrum — what the music sounds like right now, for the music
 * player's visualizer.
 *
 * MPlayer's audio output (ao_gekko) hands the DSP one 8 KB buffer at a time
 * through the audio DMA; libogc tells which buffer is playing and how far
 * it is.  1024 samples around that point (21 ms at 48 kHz) are read
 * uncached, so the decoder's writes are never disturbed, windowed and
 * transformed (FFT), then grouped into log-spaced bands like a graphic
 * equalizer.  A slow automatic gain keeps quiet and loud tracks alike.
 * ----------------------------------------------------------------------- */
namespace AudioSpectrum {

/* Levels 0..1 of n bands from 50 Hz to 16 kHz, and of the bass (below
 * 150 Hz) in *bass.  False when no audio is playing (nothing written). */
bool analyse(float* bands, int n, float* bass);

/* Forget the gain (a new track). */
void reset();

}
