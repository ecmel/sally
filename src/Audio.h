// Sound output: the default output device through an AudioUnit, fed from a
// single-producer single-consumer ring buffer of mono samples.

#ifndef SALLY_AUDIO_H
#define SALLY_AUDIO_H

typedef struct AudioOut AudioOut;

// Opens the default output device. Returns NULL without sound.
AudioOut *audio_open(void);
void audio_close(AudioOut *a);
double audio_rate(const AudioOut *a);
// Samples the device asks for at a time.
int audio_device_frames(const AudioOut *a);
// Appends samples; drops what does not fit.
void audio_write(AudioOut *a, const float *samples, int n);
// Samples waiting to be played.
int audio_buffered(const AudioOut *a);

#endif
