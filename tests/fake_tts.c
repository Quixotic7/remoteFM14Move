/* SPDX-License-Identifier: MPL-2.0 */
/* fake-tts: Schwung's speech program (schwung-tts, src/host/tts_sidecar.c) as the tests have it: the same shared
 * memory (tts_shm.h version 1, as src/announce.c restates it), the same hand-off, but no engine. It publishes its
 * settings (on, or FAKE_TTS_ENABLED=0), writes each text it is asked to speak to FAKE_TTS_LOG (a line each), and
 * answers each with a quarter second of a 440 Hz tone in the ring, as the real one pushes speech. A request that
 * changes the settings it published is logged as "SETTINGS CHANGED" (RemoteFM1 must only restate them). It leaves
 * when its parent does, or on SIGTERM.
 *   FAKE_TTS_SHM (default /schwung-tts)  FAKE_TTS_LOG (default stderr)  FAKE_TTS_ENABLED (default 1) */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TTS_SHM_MAGIC 0x31535454u
#define TTS_SHM_TEXT_LEN 8192
#define TTS_SHM_RING_SAMPLES 32768u
#define TTS_SHM_FILL_AHEAD 8192u
typedef struct { int32_t enabled, volume, engine; float speed, pitch; } tts_settings_t;
typedef struct {
    uint32_t magic, version;
    volatile uint32_t req_seq;
    uint32_t speak_count;
    tts_settings_t want;
    char text[TTS_SHM_TEXT_LEN];
    volatile uint32_t sidecar_pid, state_seq;
    tts_settings_t state;
    volatile uint32_t discard_until, speak_ack, audio_write, audio_read;
    int16_t ring[TTS_SHM_RING_SAMPLES];
} tts_shm_t;

static volatile sig_atomic_t running = 1;
static void on_term(int sig) { (void)sig; running = 0; }

int main(void)
{
    const char *name = getenv("FAKE_TTS_SHM"), *logp = getenv("FAKE_TTS_LOG"), *en = getenv("FAKE_TTS_ENABLED");
    const pid_t parent = getppid();
    tts_shm_t *shm = NULL;
    FILE *lg = stderr;
    tts_settings_t st = { en && en[0] == '0' ? 0 : 1, 70, 0, 1.0f, 110.0f };
    uint32_t last_speak, last_seq, tone_left = 0, phase = 0;
    static char text[TTS_SHM_TEXT_LEN];
    int i, fd;
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    if (logp && !(lg = fopen(logp, "a")))
        return 1;
    for (i = 0; i < 200 && running && !shm; i++) {   /* (as the real one: 10 s for the segment to appear) */
        if ((fd = shm_open(name ? name : "/schwung-tts", O_RDWR, 0)) >= 0) {
            void *p = mmap(NULL, sizeof(tts_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (p != MAP_FAILED)
                shm = p;
        }
        if (!shm)
            usleep(50000);
    }
    if (!shm || __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE) != TTS_SHM_MAGIC || shm->version != 1) {
        fprintf(lg, "NO SEGMENT\n");
        return 1;
    }
    last_speak = shm->speak_count;
    last_seq = shm->req_seq;
    shm->sidecar_pid = (uint32_t)getpid();
    shm->discard_until = shm->audio_write;
    __atomic_store_n(&shm->speak_ack, last_speak, __ATOMIC_RELEASE);
    shm->state = st;
    __atomic_store_n(&shm->state_seq, shm->state_seq + 1, __ATOMIC_RELEASE);
    fprintf(lg, "UP enabled=%d\n", st.enabled);
    fflush(lg);
    while (running && getppid() == parent) {
        const uint32_t s0 = __atomic_load_n(&shm->req_seq, __ATOMIC_ACQUIRE);
        if (!(s0 & 1) && s0 != last_seq) {
            tts_settings_t want = shm->want;
            const uint32_t count = shm->speak_count;
            if (count != last_speak)
                memcpy(text, shm->text, sizeof text), text[sizeof text - 1] = 0;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&shm->req_seq, __ATOMIC_RELAXED) == s0) {
                last_seq = s0;
                if (memcmp(&want, &st, sizeof st))
                    fprintf(lg, "SETTINGS CHANGED enabled=%d volume=%d engine=%d speed=%.2f pitch=%.1f\n",
                            want.enabled, want.volume, want.engine, want.speed, want.pitch);
                if (count != last_speak) {
                    last_speak = count;
                    fprintf(lg, "%s\n", text);
                    tone_left = 11025;   /* (a quarter second: the words) */
                    shm->discard_until = shm->audio_write;
                    __atomic_store_n(&shm->speak_ack, last_speak, __ATOMIC_RELEASE);
                }
                fflush(lg);
            }
        }
        {   /* the ring: the tone, kept TTS_SHM_FILL_AHEAD ahead at most */
            uint32_t w = shm->audio_write;
            while (tone_left) {
                const uint32_t r = __atomic_load_n(&shm->audio_read, __ATOMIC_ACQUIRE);
                uint32_t used = w - r;
                if (used > TTS_SHM_RING_SAMPLES)
                    used = TTS_SHM_RING_SAMPLES;
                if (used + 2 > TTS_SHM_FILL_AHEAD)
                    break;
                {
                    const int16_t v = (int16_t)(6000.0 * sin(2.0 * M_PI * 440.0 * phase++ / 44100.0));
                    shm->ring[w & (TTS_SHM_RING_SAMPLES - 1)] = v;
                    shm->ring[(w + 1) & (TTS_SHM_RING_SAMPLES - 1)] = v;
                    w += 2;
                    tone_left--;
                }
            }
            __atomic_store_n(&shm->audio_write, w, __ATOMIC_RELEASE);
        }
        usleep(5000);
    }
    shm->sidecar_pid = 0;
    fprintf(lg, "DOWN\n");
    fclose(lg);
    return 0;
}
