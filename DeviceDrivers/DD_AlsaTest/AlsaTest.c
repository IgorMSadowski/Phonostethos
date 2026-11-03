/* AlsaTest.c
 *
 * Utilitário de linha de comandos para testar captura e reprodução
 * de áudio via ALSA (libasound). Substitui arecord/aplay quando
 * alsa-utils não está instalado na imagem Buildroot.
 *
 * Uso:
 *   ./AlsaTest list
 *   ./AlsaTest rec  <out.wav> <segundos> [pcm]
 *   ./AlsaTest play <in.wav>            [pcm]
 *
 * Exemplos:
 *   ./AlsaTest rec  /media/usb_storage/test.wav 5 default
 *   ./AlsaTest rec  /media/usb_storage/test.wav 5 hw:0,0
 *   ./AlsaTest play /media/usb_storage/test.wav   hw:1,0
 */

/* includes necessários para este programa */
#include <alsa/asoundlib.h>   /* snd_pcm_*, snd_device_name_* */
#include <stdio.h>            /* printf, fprintf, fopen, fwrite */
#include <stdlib.h>           /* malloc, free, atoi */
#include <string.h>           /* strcmp */
#include <stdint.h>           /* uint8_t .. uint32_t */

#define DEF_PCM        "default"
#define DEF_RATE       8000
#define DEF_CHANNELS   1
#define DEF_PERIOD     256
#define BYTES_SAMPLE   2      /* S16_LE */

/* Declarações prévias */
static void list_pcm_devices(void);
static int  setup_pcm(snd_pcm_t *pcm);
static void write_wav_header(FILE *f, uint32_t data_bytes);
static int  do_record(const char *dev, const char *path, int seconds);
static int  do_play(const char *dev, const char *path);

/* ---------- equivalente a "arecord -l" / "aplay -l" ---------- */
static void list_pcm_devices(void)
{
    void **hints = NULL;

    if (snd_device_name_hint(-1, "pcm", &hints) != 0) {
        fprintf(stderr, "snd_device_name_hint falhou\n");
        return;
    }

    printf("%-20s | %-6s | %s\n",
           "NAME", "IOID", "DESC");
    printf("%-20s-+-%-6s-+-%s\n",
           "--------------------", "------",
           "--------------------------------");

    for (void **h = hints; *h != NULL; h++) {
        char *name = (char *)snd_device_name_get_hint(*h, "NAME");
        char *desc = (char *)snd_device_name_get_hint(*h, "DESC");
        char *ioid = (char *)snd_device_name_get_hint(*h, "IOID");

        printf("%-20s | %-6s | %s\n",
               (name != NULL) ? name : "(null)",
               (ioid != NULL) ? ioid : "both",
               (desc != NULL) ? desc : "");

        free(name);
        free(desc);
        free(ioid);
    }

    snd_device_name_free_hint(hints);
}

/* ---------- configuração comum a captura e reprodução ---------- */
static int setup_pcm(snd_pcm_t *pcm)
{
    snd_pcm_hw_params_t *hw;
    unsigned int        rate   = DEF_RATE;
    snd_pcm_uframes_t   period = DEF_PERIOD;
    int                 err;

    snd_pcm_hw_params_alloca(&hw);

    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0) {
        fprintf(stderr, "hw_params_any: %s\n", snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_hw_params_set_access(pcm, hw,
                SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "set_access: %s\n", snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_hw_params_set_format(pcm, hw,
                SND_PCM_FORMAT_S16_LE)) < 0) {
        fprintf(stderr, "set_format: %s\n", snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw,
                DEF_CHANNELS)) < 0) {
        fprintf(stderr, "set_channels: %s\n", snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw,
                &rate, 0)) < 0) {
        fprintf(stderr, "set_rate_near: %s\n", snd_strerror(err));
        return -1;
    }
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);

    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "hw_params: %s\n", snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_prepare(pcm)) < 0) {
        fprintf(stderr, "prepare: %s\n", snd_strerror(err));
        return -1;
    }
    return 0;
}

/* ---------- header WAV PCM 16-bit mono ---------- */
static void write_wav_header(FILE *f, uint32_t data_bytes)
{
    uint32_t chunk_size   = 36 + data_bytes;
    uint32_t subchunk2    = data_bytes;
    uint32_t subchunk1    = 16;
    uint32_t byte_rate    = DEF_RATE * DEF_CHANNELS * BYTES_SAMPLE;
    uint32_t sample_rate  = DEF_RATE;
    uint16_t audio_format = 1;                              /* PCM */
    uint16_t num_ch       = DEF_CHANNELS;
    uint16_t block_align  = DEF_CHANNELS * BYTES_SAMPLE;
    uint16_t bps          = 16;

    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    fwrite(&subchunk1, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_ch, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&subchunk2, 4, 1, f);
}

/* ---------- equivalente a "arecord -d N -f cd" ---------- */
static int do_record(const char *dev, const char *path, int seconds)
{
    snd_pcm_t *pcm = NULL;
    FILE      *f   = NULL;
    int16_t    buf[DEF_PERIOD];
    size_t     total, done = 0;
    int        err;
    uint32_t   data_bytes;

    if (seconds <= 0) {
        fprintf(stderr, "duração inválida: %d\n", seconds);
        return -1;
    }

    if ((err = snd_pcm_open(&pcm, dev,
                    SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "abrir captura '%s': %s\n",
                dev, snd_strerror(err));
        return -1;
    }
    if (setup_pcm(pcm) < 0) {
        snd_pcm_close(pcm);
        return -1;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        perror("fopen");
        snd_pcm_close(pcm);
        return -1;
    }

    data_bytes = (uint32_t)DEF_RATE * DEF_CHANNELS * BYTES_SAMPLE * seconds;
    write_wav_header(f, data_bytes);

    total = (size_t)DEF_RATE * seconds;
    fprintf(stderr, "a gravar %d s de '%s' -> %s\n",
            seconds, dev, path);

    while (done < total) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, DEF_PERIOD);

        if (n == -EPIPE) {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (n < 0) {
            fprintf(stderr, "snd_pcm_readi: %s\n", snd_strerror(n));
            break;
        }
        fwrite(buf, BYTES_SAMPLE, (size_t)n, f);
        done += (size_t)n;
    }

    fclose(f);
    snd_pcm_close(pcm);
    fprintf(stderr, "gravado: %zu amostras\n", done);
    return 0;
}

/* ---------- equivalente a "aplay" ---------- */
static int do_play(const char *dev, const char *path)
{
    snd_pcm_t *pcm = NULL;
    FILE      *f   = NULL;
    int16_t    buf[DEF_PERIOD];
    size_t     n;
    int        err;

    f = fopen(path, "rb");
    if (f == NULL) {
        perror("fopen");
        return -1;
    }
    if (fseek(f, 44, SEEK_SET) != 0) {          /* saltar header WAV */
        perror("fseek");
        fclose(f);
        return -1;
    }

    if ((err = snd_pcm_open(&pcm, dev,
                    SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "abrir reprodução '%s': %s\n",
                dev, snd_strerror(err));
        fclose(f);
        return -1;
    }
    if (setup_pcm(pcm) < 0) {
        snd_pcm_close(pcm);
        fclose(f);
        return -1;
    }

    fprintf(stderr, "a reproduzir %s em '%s'\n", path, dev);

    while ((n = fread(buf, BYTES_SAMPLE, DEF_PERIOD, f)) > 0) {
        size_t off = 0;

        while (off < n) {
            snd_pcm_sframes_t w =
                snd_pcm_writei(pcm, buf + off, n - off);

            if (w == -EPIPE) {
                snd_pcm_prepare(pcm);
                continue;
            }
            if (w < 0) {
                fprintf(stderr, "snd_pcm_writei: %s\n",
                        snd_strerror(w));
                goto out;
            }
            off += (size_t)w;
        }
    }

out:
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    fclose(f);
    return 0;
}

/* ---------- main ---------- */
int main(int argc, char **argv)
{
    const char *dev;

    if (argc < 2) {
        fprintf(stderr,
            "uso:\n"
            "  %s list\n"
            "  %s rec  <out.wav> <segundos> [pcm]\n"
            "  %s play <in.wav>            [pcm]\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        list_pcm_devices();
        return 0;
    }

    if (strcmp(argv[1], "rec") == 0 && argc >= 4) {
        dev = (argc >= 5) ? argv[4] : DEF_PCM;
        return (do_record(dev, argv[2], atoi(argv[3])) < 0) ? 1 : 0;
    }

    if (strcmp(argv[1], "play") == 0 && argc >= 3) {
        dev = (argc >= 4) ? argv[3] : DEF_PCM;
        return (do_play(dev, argv[2]) < 0) ? 1 : 0;
    }

    fprintf(stderr, "argumentos inválidos\n");
    return 1;
}
