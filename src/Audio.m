#import "Audio.h"

#import <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#else
#import <CoreAudio/CoreAudio.h>
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

enum { RING = 1 << 15 };

struct AudioOut {
    AudioUnit unit;
    double rate;
    int device_frames;
    float ring[RING];
    _Atomic uint32_t head;  // written by the emulator
    _Atomic uint32_t tail;  // read by the device
    float last;             // the last sample played, faded out on underrun
#if TARGET_OS_IPHONE
    void *observer;         // restarts the unit after an interruption
#endif
};

static OSStatus render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 frames, AudioBufferList *data) {
    AudioOut *a = ctx;
    float *out = data->mBuffers[0].mData;
    uint32_t tail = atomic_load_explicit(&a->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&a->head, memory_order_acquire);
    for (UInt32 i = 0; i < frames; i++) {
        float s;
        if (tail != head) {
            s = a->ring[tail++ & (RING - 1)];
        } else {
            a->last *= 0.995f;
            s = a->last;
        }
        a->last = s;
        out[2 * i] = out[2 * i + 1] = s;
    }
    atomic_store_explicit(&a->tail, tail, memory_order_release);
    return noErr;
}

AudioOut *audio_open(void) {
    AudioOut *a = calloc(1, sizeof *a);
    if (!a) return NULL;
#if TARGET_OS_IPHONE
    // Ambient, as games are: it follows the silent switch and mixes with
    // other apps' sound.
    AVAudioSession *session = AVAudioSession.sharedInstance;
    [session setCategory:AVAudioSessionCategoryAmbient error:nil];
    [session setPreferredIOBufferDuration:256 / session.sampleRate error:nil];
    [session setActive:YES error:nil];
#endif
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
        .componentSubType = kAudioUnitSubType_RemoteIO,
#else
        .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &a->unit) != noErr) {
        free(a);
        return NULL;
    }

    // Run at the device's own rate so nothing resamples after us, and ask
    // for small device buffers.
#if TARGET_OS_IPHONE
    a->rate = session.sampleRate > 0 ? session.sampleRate : 48000;
    a->device_frames = (int)lround(session.IOBufferDuration * a->rate);
#else
    AudioStreamBasicDescription hw = {0};
    UInt32 size = sizeof hw;
    AudioUnitGetProperty(a->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &hw, &size);
    a->rate = hw.mSampleRate > 0 ? hw.mSampleRate : 48000;
    AudioDeviceID device = 0;
    size = sizeof device;
    if (AudioUnitGetProperty(a->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device,
                             &size) == noErr) {
        UInt32 frames = 256;
        AudioObjectPropertyAddress addr = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        AudioObjectSetPropertyData(device, &addr, 0, NULL, sizeof frames, &frames);
        size = sizeof frames;
        if (AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, &frames) == noErr) a->device_frames = frames;
    }
#endif
    if (!a->device_frames) a->device_frames = 512;

    AudioStreamBasicDescription fmt = {
        .mSampleRate = a->rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 8,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 8,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 32,
    };
    AURenderCallbackStruct cb = {render, a};
    if (AudioUnitSetProperty(a->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt) ||
        AudioUnitSetProperty(a->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(a->unit) || AudioOutputUnitStart(a->unit)) {
        AudioComponentInstanceDispose(a->unit);
        free(a);
        return NULL;
    }
#if TARGET_OS_IPHONE
    // A call or another app's audio stops the unit; it stays stopped
    // unless started again.
    AudioUnit unit = a->unit;
    id observer = [NSNotificationCenter.defaultCenter
        addObserverForName:AVAudioSessionInterruptionNotification
                    object:nil
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *note) {
                    if ([note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] !=
                        AVAudioSessionInterruptionTypeEnded)
                        return;
                    [AVAudioSession.sharedInstance setActive:YES error:nil];
                    AudioOutputUnitStart(unit);
                }];
    a->observer = (void *)CFBridgingRetain(observer);
#endif
    return a;
}

void audio_close(AudioOut *a) {
    if (!a) return;
#if TARGET_OS_IPHONE
    [NSNotificationCenter.defaultCenter removeObserver:CFBridgingRelease(a->observer)];
#endif
    AudioOutputUnitStop(a->unit);
    AudioUnitUninitialize(a->unit);
    AudioComponentInstanceDispose(a->unit);
    free(a);
}

double audio_rate(const AudioOut *a) { return a->rate; }

int audio_device_frames(const AudioOut *a) { return a->device_frames; }

void audio_write(AudioOut *a, const float *samples, int n) {
    uint32_t head = atomic_load_explicit(&a->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&a->tail, memory_order_acquire);
    uint32_t room = RING - (head - tail);
    if ((uint32_t)n > room) n = (int)room;
    for (int i = 0; i < n; i++) a->ring[head++ & (RING - 1)] = samples[i];
    atomic_store_explicit(&a->head, head, memory_order_release);
}

int audio_buffered(const AudioOut *a) {
    return (int)(atomic_load_explicit(&a->head, memory_order_acquire) -
                 atomic_load_explicit(&a->tail, memory_order_acquire));
}
