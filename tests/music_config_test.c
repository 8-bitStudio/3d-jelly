/* Production config reader/writer, with console credential services stubbed. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef int Result;
#define HOME_ROW_ITEMS 24
#include "browse_types.inc"
static Config g_cfg;
#define CONFIG_DIR "build/host"
#define CONFIG_PATH "build/host/music-config-test.ini"
#define PLAYBACK_MODE_AUTO 0
#define SETTINGS_BUFFER_KB_DEFAULT 256
#define BOTTOM_DIM_DEFAULT_SECONDS 30
#define LANG_EN 0
#define TR_STATUS_CREDENTIAL_DECRYPT_FAIL 0
#define TR_STATUS_CONFIGURE_SERVER 1
static const char *tr(int id) { (void)id; return "fixture"; }
static void set_status(const char *fmt, ...) { (void)fmt; }
static bool config_decrypt_secret(const char *field, const char *in, char *out, size_t size)
{
    (void)field; (void)in; (void)out; (void)size;
    return false;
}
static void save_config_secret(FILE *file, const char *key, const char *field, const char *value)
{
    (void)field;
    assert(!value[0]);
    fprintf(file, "%s_enc=\n", key);
}
static void ensure_defaults(void);
#include "music_config_support.inc"
static void ensure_defaults(void) { music_sound_normalize(&g_cfg.music); }
static void fixture(const char *text)
{
    FILE *file = fopen(CONFIG_PATH, "w"); assert(file);
    fputs(text, file); fclose(file);
}
int main(void)
{
    unlink(CONFIG_PATH);
    load_config();
    assert(g_cfg.music.sample_rate == 48000 && g_cfg.music.channels == 2);
    assert(g_cfg.music.buffer_kb == 1024 && g_cfg.music.volume_percent == 100);
    fixture("audio_sample_rate=22050\nvolume_percent=175\n");
    load_config();
    assert(g_cfg.audio_sample_rate == 22050 && g_cfg.volume_percent == 175);
    fixture("music_buffer_kb=512\n"); load_config();
    assert(g_cfg.music.buffer_kb == 1024); /* The previous default upgrades once. */
    g_cfg.music.buffer_kb = 512; save_config(); load_config();
    assert(g_cfg.music.buffer_kb == 512); /* An explicit new choice remains saved. */
    fixture("music_buffer_kb=128\n"); load_config();
    assert(g_cfg.music.buffer_kb == 128);
    g_cfg.audio_sample_rate = 22050; g_cfg.volume_percent = 175;
    assert(g_cfg.music.sample_rate == 48000 && g_cfg.music.volume_percent == 100);
    g_cfg.music = (MusicSoundSettings){44100, 1, 1024, 0};
    save_config(); memset(&g_cfg, 0, sizeof(g_cfg)); load_config();
    assert(g_cfg.music.sample_rate == 44100 && g_cfg.music.channels == 1);
    assert(g_cfg.music.buffer_kb == 1024 && g_cfg.music.volume_percent == 0);
    assert(g_cfg.audio_sample_rate == 22050 && g_cfg.volume_percent == 175);
    fixture("music_sample_rate=96000\nmusic_channels=8\nmusic_buffer_kb=-10\nmusic_volume_percent=999\n");
    load_config();
    assert(g_cfg.music.sample_rate == 48000 && g_cfg.music.channels == 2);
    assert(g_cfg.music.buffer_kb == 1024 && g_cfg.music.volume_percent == 100);
    unlink(CONFIG_PATH);
    puts("PASS: persisted music settings, old-config migration, muted volume, invalid-value recovery and video isolation");
}
