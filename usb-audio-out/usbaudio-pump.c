// usbaudio-pump: sends the deck's master to a computer over the USB-B port.
//
// Reads EngineX's Live Stream master (raw 48 kHz s16le stereo from
// http://127.0.0.1:8080/live.wav?format=pcm, fed by EngineX's in-Engine tap) and
// writes it to the USB Audio Class 2 gadget's ALSA playback device (card
// "UAC2Gadget"), which the computer sees as a sound card input.
//
// The deck's audio clock and the USB clock differ slightly, so the ALSA buffer
// level is held near a target latency: with the gadget's "Playback Pitch 1000000"
// control (as Engine itself uses for its own gadget) by nudging the USB rate,
// otherwise by dropping or repeating a single frame now and then.
//
// Options:
//   -d DEVICE   ALSA device (default: auto, the UAC2Gadget card)
//   -u HOST:PORT  Live Stream source (default 127.0.0.1:8080)
//   -l MS       target latency in ms (default 60)
//   -s PPM      test mode: simulated USB device whose clock is off by PPM
//   -t SECONDS  test mode: stop after SECONDS (default: run forever)
//   -P          test mode: no pitch control (exercise frame drop/repeat)
//   -v          print stats every 10 s
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000
#define CHANNELS 2
#define CHUNK 240 // frames per write: 5 ms
#define SRC_FRAME 4 // bytes per s16 stereo frame

static volatile sig_atomic_t g_stop;
static int g_verbose;
static void on_signal(int s) { (void)s; g_stop = 1; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    fprintf(stderr, "%s usbaudio-pump: ", ts);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// ---------------------------------------------------------------- source (HTTP)

struct source {
    char host[64];
    int port;
    int fd;
    int ok_logged, fail_logged;
    unsigned char carry[SRC_FRAME];
    int carry_len;
};

static int src_connect(struct source *s) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(s->port);
    if (inet_pton(AF_INET, s->host, &a.sin_addr) != 1 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    char req[256];
    int n = snprintf(req, sizeof req,
                     "GET /live.wav?format=pcm&buffer=100 HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: usbaudio-pump\r\n\r\n",
                     s->host, s->port);
    if (write(fd, req, n) != n) {
        close(fd);
        return -1;
    }
    // Read the response head, then check for a 200.
    char head[2048];
    int len = 0;
    while (len < (int)sizeof head - 1) {
        ssize_t r = read(fd, head + len, 1);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        len += r;
        head[len] = 0;
        if (len >= 4 && !memcmp(head + len - 4, "\r\n\r\n", 4)) break;
    }
    if (!strstr(head, " 200")) {
        if (!s->fail_logged) {
            char *eol = strchr(head, '\r');
            if (eol) *eol = 0;
            logf_("Live Stream not available (%s). Set Live Stream to ON in the deck control centre.", head);
            s->fail_logged = 1;
        }
        close(fd);
        return -1;
    }
    s->fail_logged = 0;
    s->carry_len = 0;
    logf_("connected to the Live Stream master");
    return fd;
}

// Reads up to max_frames whole s16 stereo frames. Returns frames read, 0 if none
// are available right now, or -1 if the connection is gone.
static int src_read(struct source *s, int16_t *out, int max_frames) {
    unsigned char *buf = (unsigned char *)out;
    memcpy(buf, s->carry, s->carry_len);
    ssize_t r = read(s->fd, buf + s->carry_len, (size_t)max_frames * SRC_FRAME - s->carry_len);
    if (r == 0) return -1;
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    int total = s->carry_len + (int)r;
    int frames = total / SRC_FRAME;
    s->carry_len = total - frames * SRC_FRAME;
    memcpy(s->carry, buf + frames * SRC_FRAME, s->carry_len);
    return frames;
}

// ---------------------------------------------------------------- sink (ALSA or simulated)

struct sink {
    // ALSA
    snd_pcm_t *pcm;
    snd_ctl_t *ctl;
    snd_ctl_elem_value_t *pitch_val;
    int pitch_ok;
    long pitch_min, pitch_max;
    int s32; // device takes S32_LE
    char dev[64];
    // simulation
    int sim;
    double sim_ppm, sim_level, sim_t, sim_pitch;
    long sim_buffer;
};

static int find_gadget_card(char *dev, size_t n) {
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char *id = NULL;
        if (snd_card_get_name(card, &id) == 0 && id) {
            int match = strstr(id, "UAC2") != NULL;
            free(id);
            if (match) {
                snprintf(dev, n, "hw:%d,0", card);
                return card;
            }
        }
    }
    return -1;
}

static void sink_close(struct sink *k) {
    if (k->pcm) snd_pcm_close(k->pcm);
    if (k->ctl) snd_ctl_close(k->ctl);
    if (k->pitch_val) snd_ctl_elem_value_free(k->pitch_val);
    k->pcm = NULL;
    k->ctl = NULL;
    k->pitch_val = NULL;
    k->pitch_ok = 0;
}

static int sink_open(struct sink *k, const char *want, long latency_frames) {
    if (k->sim) {
        k->sim_level = 0;
        k->sim_t = now_s();
        k->sim_pitch = 1000000;
        k->sim_buffer = latency_frames * 2;
        return 0;
    }
    char dev[64];
    int card = -1;
    if (!strcmp(want, "auto")) {
        card = find_gadget_card(dev, sizeof dev);
        if (card < 0) return -1;
    } else {
        snprintf(dev, sizeof dev, "%s", want);
    }
    int err = snd_pcm_open(&k->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        k->pcm = NULL;
        return -1;
    }
    k->s32 = 0;
    err = snd_pcm_set_params(k->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, CHANNELS, RATE, 0,
                             (unsigned)(latency_frames * 2 * 1000000LL / RATE));
    if (err < 0) {
        k->s32 = 1;
        err = snd_pcm_set_params(k->pcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED, CHANNELS, RATE, 0,
                                 (unsigned)(latency_frames * 2 * 1000000LL / RATE));
    }
    if (err < 0) {
        logf_("%s: cannot set 48 kHz stereo: %s", dev, snd_strerror(err));
        sink_close(k);
        return -1;
    }
    snprintf(k->dev, sizeof k->dev, "%s", dev);
    // The gadget's pitch control, if the kernel provides it.
    if (card >= 0) {
        char ctlname[32];
        snprintf(ctlname, sizeof ctlname, "hw:%d", card);
        if (snd_ctl_open(&k->ctl, ctlname, 0) == 0) {
            snd_ctl_elem_id_t *id;
            snd_ctl_elem_info_t *info;
            snd_ctl_elem_id_alloca(&id);
            snd_ctl_elem_info_alloca(&info);
            snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);
            snd_ctl_elem_id_set_name(id, "Playback Pitch 1000000");
            snd_ctl_elem_info_set_id(info, id);
            if (snd_ctl_elem_info(k->ctl, info) < 0) {
                snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
                snd_ctl_elem_info_set_id(info, id);
            }
            if (snd_ctl_elem_info(k->ctl, info) == 0 && snd_ctl_elem_value_malloc(&k->pitch_val) == 0) {
                snd_ctl_elem_info_get_id(info, id);
                snd_ctl_elem_value_set_id(k->pitch_val, id);
                k->pitch_min = snd_ctl_elem_info_get_min(info);
                k->pitch_max = snd_ctl_elem_info_get_max(info);
                k->pitch_ok = 1;
            }
        }
    }
    logf_("writing to %s (%s, drift correction by %s)", dev, k->s32 ? "S32_LE" : "S16_LE",
          k->pitch_ok ? "USB pitch control" : "frame drop/repeat");
    return 0;
}

// Simulated USB device: consumes at RATE * (1 + ppm) * pitch, holds up to sim_buffer frames.
static void sim_advance(struct sink *k) {
    double t = now_s();
    double rate = RATE * (1.0 + k->sim_ppm / 1e6) * (k->sim_pitch / 1e6);
    k->sim_level -= (t - k->sim_t) * rate;
    if (k->sim_level < 0) k->sim_level = 0; // (an underrun: the device plays silence)
    k->sim_t = t;
}

static long sink_delay(struct sink *k) {
    if (k->sim) {
        sim_advance(k);
        return (long)k->sim_level;
    }
    snd_pcm_sframes_t d = 0;
    if (snd_pcm_delay(k->pcm, &d) < 0) return -1;
    return (long)d;
}

static void sink_set_pitch(struct sink *k, long ppm) {
    if (k->sim) {
        k->sim_pitch = (double)ppm;
        return;
    }
    if (!k->pitch_ok) return;
    if (ppm < k->pitch_min) ppm = k->pitch_min;
    if (ppm > k->pitch_max) ppm = k->pitch_max;
    snd_ctl_elem_value_set_integer(k->pitch_val, 0, ppm);
    snd_ctl_elem_write(k->ctl, k->pitch_val);
}

// Writes frames of s16 stereo. Returns 0, 1 after an underrun was recovered, or -1 if
// the device is gone.
static int sink_write(struct sink *k, const int16_t *src, int frames) {
    if (k->sim) {
        for (;;) {
            sim_advance(k);
            if (k->sim_level + frames <= k->sim_buffer) break;
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
        k->sim_level += frames;
        return 0;
    }
    static int32_t wide[CHUNK * 2 * CHANNELS];
    const void *buf = src;
    if (k->s32) {
        for (int i = 0; i < frames * CHANNELS; i++) wide[i] = (int32_t)src[i] << 16;
        buf = wide;
    }
    int recovered = 0;
    while (frames > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(k->pcm, buf, frames);
        if (w == -EPIPE || w == -ESTRPIPE || w == -EINTR) {
            if (snd_pcm_recover(k->pcm, (int)w, 1) < 0) return -1;
            recovered = 1;
            continue;
        }
        if (w < 0) return -1; // -ENODEV, -EBADFD, -EIO: the gadget went away
        frames -= (int)w;
        buf = (const char *)buf + w * (k->s32 ? 8 : 4);
    }
    return recovered;
}

// ---------------------------------------------------------------- main loop

int main(int argc, char **argv) {
    const char *dev = "auto", *hostport = "127.0.0.1:8080";
    long latency_ms = 60;
    double run_for = 0;
    int no_pitch = 0;
    struct sink k = {0};
    int opt;
    while ((opt = getopt(argc, argv, "d:u:l:s:t:vP")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'u': hostport = optarg; break;
        case 'l': latency_ms = atol(optarg); break;
        case 's': k.sim = 1; k.sim_ppm = atof(optarg); break;
        case 't': run_for = atof(optarg); break;
        case 'v': g_verbose = 1; break;
        case 'P': no_pitch = 1; break;
        default:
            fprintf(stderr, "usage: %s [-d device] [-u host:port] [-l ms] [-s ppm] [-t seconds] [-v]\n", argv[0]);
            return 2;
        }
    }
    if (latency_ms < 20) latency_ms = 20;
    if (latency_ms > 500) latency_ms = 500;
    const long target = latency_ms * RATE / 1000;

    struct source s = {0};
    s.fd = -1;
    {
        const char *colon = strrchr(hostport, ':');
        size_t hl = colon ? (size_t)(colon - hostport) : strlen(hostport);
        if (hl >= sizeof s.host) hl = sizeof s.host - 1;
        memcpy(s.host, hostport, hl);
        s.port = colon ? atoi(colon + 1) : 8080;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    static int16_t chunk[(CHUNK + 1) * CHANNELS];
    static const int16_t silence[(CHUNK + 1) * CHANNELS];
    double start = now_s(), last_stats = start, last_connect = 0;
    double avg = (double)target;
    long underruns = 0, drops = 0, dups = 0, frames_out = 0;
    int dev_open = 0, dev_fail_logged = 0;
    long pitch = 1000000;

    while (!g_stop) {
        if (run_for > 0 && now_s() - start > run_for) break;

        if (!dev_open) {
            if (sink_open(&k, dev, target) == 0) {
                dev_open = 1;
                dev_fail_logged = 0;
                avg = (double)target;
                pitch = 1000000;
                sink_set_pitch(&k, pitch);
                // Prime the buffer to the target latency.
                for (long f = 0; f < target; f += CHUNK) sink_write(&k, silence, CHUNK);
            } else {
                if (!dev_fail_logged) {
                    logf_("USB gadget sound card not found yet (is the USB-B audio gadget set up?); waiting");
                    dev_fail_logged = 1;
                }
                sleep(1);
                continue;
            }
        }

        if (s.fd < 0 && now_s() - last_connect > 2.0) {
            last_connect = now_s();
            s.fd = src_connect(&s);
        }

        // Wait for audio only as long as the USB buffer can cover (down to a third of
        // the target); after that, fill with silence rather than let it run dry.
        long level = sink_delay(&k);
        int wait_ms = level < 0 ? 5 : (int)((level - target / 3) * 1000 / RATE);
        if (wait_ms < 0) wait_ms = 0;
        if (wait_ms > 200) wait_ms = 200;
        int n = 0;
        if (s.fd >= 0) {
            struct pollfd pfd = {s.fd, POLLIN, 0};
            int pr = poll(&pfd, 1, wait_ms);
            n = pr > 0 ? src_read(&s, chunk, CHUNK) : 0;
            if (n < 0) {
                logf_("Live Stream connection closed; reconnecting");
                close(s.fd);
                s.fd = -1;
                n = 0;
            }
        }

        const int16_t *out = chunk;
        if (n == 0) {
            // No music right now (stream off, starting, or stalled): keep the USB side
            // running with silence so the computer sees a steady input, but only up to
            // the target level, so the music starts with the normal latency.
            if (level >= target) {
                struct timespec ts = {0, 2000000};
                nanosleep(&ts, NULL);
                continue;
            }
            out = silence;
            n = CHUNK;
        }

        // Drift control on the averaged buffer level.
        long d = sink_delay(&k);
        if (d >= 0) avg += 0.02 * ((double)d - avg);
        double err = avg - (double)target; // frames; > 0: the buffer is filling
        if ((k.sim && !no_pitch) || k.pitch_ok) {
            long want = 1000000 + (long)(err * 4.0); // 1 ms (48 frames) over -> +192 ppm
            if (want > 1000000 + 2000) want = 1000000 + 2000;
            if (want < 1000000 - 2000) want = 1000000 - 2000;
            if (labs(want - pitch) >= 5) {
                pitch = want;
                sink_set_pitch(&k, pitch);
            }
        } else if (out == chunk) {
            const long tol = RATE / 200; // 5 ms
            if (err > tol && n > 1) {
                n -= 1; // drop the last frame of this chunk
                drops++;
                avg -= 1;
            } else if (err < -tol) {
                chunk[n * CHANNELS] = chunk[(n - 1) * CHANNELS];
                chunk[n * CHANNELS + 1] = chunk[(n - 1) * CHANNELS + 1];
                n += 1; // repeat the last frame
                dups++;
                avg += 1;
            }
        }

        int r = sink_write(&k, out, n);
        if (r < 0) {
            logf_("USB gadget sound card went away (cable unplugged or gadget removed); waiting for it");
            sink_close(&k);
            dev_open = 0;
            continue;
        }
        if (r == 1) underruns++;
        frames_out += n;

        if (g_verbose && now_s() - last_stats >= 10.0) {
            last_stats = now_s();
            logf_("level %.1f ms (target %ld ms), pitch %+ld ppm, underruns %ld, drops %ld, repeats %ld, %s",
                  avg * 1000.0 / RATE, latency_ms, pitch - 1000000, underruns, drops, dups,
                  s.fd >= 0 ? "streaming" : "silence");
        }
    }
    if (g_verbose || k.sim)
        logf_("done: level %.1f ms (target %ld ms), pitch %+ld ppm, underruns %ld, drops %ld, repeats %ld, frames %ld",
              avg * 1000.0 / RATE, latency_ms, pitch - 1000000, underruns, drops, dups, frames_out);
    sink_set_pitch(&k, 1000000);
    sink_close(&k);
    if (s.fd >= 0) close(s.fd);
    return 0;
}
