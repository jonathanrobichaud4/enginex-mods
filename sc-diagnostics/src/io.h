/* sc-diagnostics: touch, control surface MIDI and audio. */
#pragma once
#include <stdint.h>

/* ---- touch ---- up to 10 fingers, in canvas coordinates */
enum { MAXT = 10 };
typedef struct { int down, x, y; } touch_t;
extern touch_t touches[MAXT];
int touch_open(void);              /* 0 ok */
int touch_poll(void);              /* reads pending events; returns 1 if anything changed */
const char *touch_name(void);
extern int touch_fd;

/* ---- control surface MIDI ---- */
typedef struct { uint8_t st, d1, d2; } midi_msg_t;
int midi_open(void);               /* 0 ok */
int midi_poll(midi_msg_t *m, int max); /* parsed channel messages (SysEx dropped) */
void midi_send(const uint8_t *b, int n);
void midi_note(int ch, int note, int vel);
void midi_cc(int ch, int cc, int val);
void midi_sysex_hex(const char *hex);  /* "F0 00 02 ... F7" */
const char *midi_name(void);
extern int midi_fd;                /* for poll(); -1 if none */

/* ---- audio ---- the JP21 codec: 8 outputs (Main 1/2, Booth 3/4, Phones 5/6), inputs metered */
int audio_open(void);              /* starts the audio threads; 0 ok */
void audio_close(void);
const char *audio_name(void);
void audio_tone(int mask, double hz); /* mask: bit per output channel, 0 = silence; hz 0 = sweep */
int audio_tone_mask(void);
double audio_tone_hz(void);
int audio_in_channels(void);
double audio_in_level(int ch);     /* dBFS peak of the last ~50 ms, -100 when silent */
extern int audio_out_ok, audio_in_ok;
