/* Real curl + production player; a deterministic DSP sink replaces 3DS hardware. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int Result;
#include "music_host_threads.h"
#define R_FAILED(x) ((x)<0)
#define R_SUCCEEDED(x) ((x)>=0)
#define TICKS_PER_SECOND 10000000ULL
#define MAX_ITEMS 2048
#define HOME_ROW_ITEMS 24
#define HTTP_CAP (1024*1024)
#define STREAM_URL_CAP 2048
#define APP_VERSION "music-test"
#define KEY_A 1
#define KEY_B 2
#define KEY_R 4
#define KEY_L 8
#define KEY_RIGHT 16
#define KEY_LEFT 32
#define KEY_UP 64
#define KEY_DOWN 128
#define KEY_Y 256
#define KEY_X 512
#define KEY_TOUCH 1024
#define KEY_SELECT 2048
#define NDSP_WBUF_QUEUED 1
#define NDSP_WBUF_PLAYING 2
#define NDSP_WBUF_DONE 3
#define NDSP_OUTPUT_STEREO 1
#define NDSP_CLIP_SOFT 1
#define NDSP_INTERP_POLYPHASE 0
#define NDSP_FORMAT_STEREO_PCM16 2
#define NDSP_FORMAT_MONO_PCM16 1
#include "browse_types.inc"
static Config g_cfg;
static int saved_configs;
static void save_config(void) { ++saved_configs; }
static MediaItem g_current, g_items[MAX_ITEMS];
static int g_item_count;
static View g_view = VIEW_ITEMS;
static char g_play_session[96], g_play_media_source_id[128];
static bool g_playback_report_active, g_playback_ended_naturally;
static void json_escape(const char *, char *, size_t);
static int playback_report_volume_level(int, bool);
static u64 clamp_media_ticks(u64 ticks) { return ticks; }
#include "browse_support.inc"
typedef struct { char *data; size_t size, cap; } CurlResponseBuffer;
#include "music_http_support.inc"
static bool curl_http_runtime_init(void) { return true; }
static u64 osGetTime(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000ULL+t.tv_nsec/1000000; }
static int clamp_int(int n,int low,int high) { return n<low?low:n>high?high:n; }
static bool app_system_closing(void) { return false; }
static void browse_cancel_request(void) {}
static bool toggle_favorite(MediaItem *item) { item->is_favorite=!item->is_favorite; return true; }
static int reports, stops, natural_stops, encoding_stops;
static bool report_playback_start(u64 at,bool paused,bool muted,int volume) { (void)at;(void)paused;(void)muted;(void)volume; ++reports;g_playback_report_active=true;return true; }
static bool report_playback_stopped(u64 at) { (void)at;++stops;natural_stops+=g_playback_ended_naturally;g_playback_report_active=false;return true; }
static bool report_playback_progress(u64 at,bool paused,bool muted,int volume) { (void)at;(void)paused;(void)muted;(void)volume;return true; }
static void report_playback_progress_periodic(u64 at,bool paused,bool muted,int volume) { (void)report_playback_progress(at,paused,muted,volume); }
static void stop_active_encoding(void) { ++encoding_stops; }
typedef struct { int px,py; } touchPosition;
static void hidTouchRead(touchPosition *p) { p->px=p->py=0; }
typedef struct { s16 *data_pcm16; u32 nsamples; _Atomic u8 status; _Atomic u16 sequence_id; } ndspWaveBuf;
static pthread_mutex_t sink_lock=PTHREAD_MUTEX_INITIALIZER;
static ndspWaveBuf *sink[16];
static int sink_count, sink_format;
static u32 sink_pos;
static u16 sink_seq;
static bool sink_paused;
static u64 heard_frames;
static int dsp_inits, pcm_allocations, sink_rate;
static int allocation_fail_after=-1;
static int next_sample;
static bool have_sample;
static int ndspInit(void) { ++dsp_inits; return 0; }
static void ndspExit(void) {}
static void ndspChnReset(int c) { (void)c;pthread_mutex_lock(&sink_lock);sink_count=sink_pos=0;have_sample=false;pthread_mutex_unlock(&sink_lock); }
static void ndspSetOutputMode(int v) { assert(v==NDSP_OUTPUT_STEREO); }
static void ndspSetClippingMode(int v) { (void)v; }
static void ndspChnSetInterp(int c,int v) { (void)c;assert(v==NDSP_INTERP_POLYPHASE); }
static void ndspChnSetFormat(int c,int v) { (void)c;sink_format=v; }
static void ndspChnSetRate(int c,float v) { (void)c;assert(v==g_cfg.music.sample_rate);sink_rate=(int)v; }
static void ndspChnSetPaused(int c,bool v) { (void)c;pthread_mutex_lock(&sink_lock);sink_paused=v;pthread_mutex_unlock(&sink_lock); }
static void ndspChnSetMix(int c,float *v) { (void)c;assert(v[0]==v[1]); }
static void *linearAlloc(size_t size) { if(allocation_fail_after==0)return NULL;if(allocation_fail_after>0)--allocation_fail_after;++pcm_allocations;return malloc(size); }
static void linearFree(void *p) { free(p); }
static void DSP_FlushDataCache(void *p,size_t n) { (void)p;(void)n; }
static u32 ndspChnGetSamplePos(int c) { (void)c;pthread_mutex_lock(&sink_lock);u32 pos=sink_pos;pthread_mutex_unlock(&sink_lock);return pos; }
static u16 ndspChnGetWaveBufSeq(int c) { (void)c;pthread_mutex_lock(&sink_lock);u16 seq=sink_count?sink[0]->sequence_id:0;pthread_mutex_unlock(&sink_lock);return seq; }
static void ndspChnWaveBufAdd(int c,ndspWaveBuf *buf)
{
    (void)c;pthread_mutex_lock(&sink_lock);assert(sink_count<16 && buf->nsamples>0 && buf->nsamples<=4096);
    for(u32 i=0;i<buf->nsamples;++i) {
        if (sink_format==NDSP_FORMAT_STEREO_PCM16) {
            int sample=buf->data_pcm16[2*i];
            assert(sample+buf->data_pcm16[2*i+1]==0);
            if(have_sample) assert(sample==next_sample); /* No lost/reordered/duplicate frames across ring wrap. */
            next_sample=sample+1;if(next_sample==8000)next_sample=-8000;have_sample=true;
        }
        else assert(buf->data_pcm16[i]==0);
    }
    buf->sequence_id=++sink_seq;buf->status=NDSP_WBUF_QUEUED;sink[sink_count++]=buf;pthread_mutex_unlock(&sink_lock);
}
static void consume(unsigned frames)
{
    pthread_mutex_lock(&sink_lock);
    while(frames && sink_count && !sink_paused) {
        ndspWaveBuf *buf=sink[0];buf->status=NDSP_WBUF_PLAYING;
        unsigned count=buf->nsamples-sink_pos;if(count>frames)count=frames;
        sink_pos+=count;frames-=count;heard_frames+=count;
        if(sink_pos==buf->nsamples) {buf->status=NDSP_WBUF_DONE;memmove(sink,sink+1,(--sink_count)*sizeof(*sink));sink_pos=0;}
    }
    pthread_mutex_unlock(&sink_lock);
}
#include "../source/parts/music_stream.inc"
#include "../source/parts/music_player.inc"
static void tick(bool play)
{
    if(play)consume(sink_rate / 100);
    music_tick();usleep(1000);
}
static void wait_ready(void)
{
    for(int i=0;i<6000 && !g_music.started && g_music.phase!=MUSIC_ERROR;++i)tick(false);
    if(g_music.phase==MUSIC_ERROR)fprintf(stderr,"%s\n",g_music.error);
    assert(g_music.started && g_music.phase==MUSIC_STREAM);
}
static atomic_bool hardware_stop;
static void *hardware_clock(void *unused)
{
    (void)unused;
    u64 start=osGetTime(), delivered=0;
    while(!atomic_load(&hardware_stop)) {
        u64 due=(osGetTime()-start)*(u64)sink_rate/1000;
        consume((unsigned)(due-delivered));delivered=due;
        svcSleepThread(5000000LL);
    }
    return NULL;
}
static void blocked_ui_test(const MediaItem *item)
{
    g_item_count=0;
    assert(music_start(item));wait_ready();
    /* Hardware keeps consuming while the main/UI thread is stuck in an HTTP call. */
    atomic_store(&hardware_stop,false);pthread_t hardware;
    assert(!pthread_create(&hardware,NULL,hardware_clock,NULL));
    u64 before=music_position();
    char url[1024],auth[384],header[448];build_url(url,sizeof(url),"/SlowUI");auth_header(auth,sizeof(auth),true);
    snprintf(header,sizeof(header),"Authorization: %s",auth);
    CURL *request=curl_easy_init();struct curl_slist *headers=curl_slist_append(NULL,header);
    curl_easy_setopt(request,CURLOPT_URL,url);curl_easy_setopt(request,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(request,CURLOPT_WRITEFUNCTION,playback_discard_response);
    assert(curl_easy_perform(request)==CURLE_OK);
    curl_easy_cleanup(request);curl_slist_free_all(headers);
    atomic_store(&hardware_stop,true);pthread_join(hardware,NULL);
    music_tick();
    if(music_position()<=before+TICKS_PER_SECOND || g_music.buffering || g_music.underruns)
        fprintf(stderr,"Blocked UI: advanced=%llu buffering=%d underruns=%u error=%s\n",(unsigned long long)(music_position()-before),g_music.buffering,g_music.underruns,g_music.error);
    assert(music_position()>before+TICKS_PER_SECOND && !g_music.buffering && !g_music.underruns);
    music_stop();assert(!atomic_load(&host_thread_count));g_view=VIEW_ITEMS;
}
static void parser_tests(void)
{
    /* Odd-sized unknown chunks, data-like bytes in metadata, and split headers. */
    u8 wav[64]={0};memcpy(wav,"RIFF",4);memcpy(wav+8,"WAVEJUNK",8);wav[16]=3;memcpy(wav+20,"dat",3);
    memcpy(wav+24,"fmt ",4);wav[28]=16;wav[32]=1;wav[34]=2;wav[36]=0x44;wav[37]=0xAC;
    wav[44]=4;wav[46]=16;memcpy(wav+48,"data",4);wav[52]=8;wav[56]=1;wav[60]=2;
    MusicPcmStream p={0};p.capacity=512 * 1024;p.data=malloc(p.capacity);
    for(size_t i=0;i<sizeof(wav);++i) {assert(music_ring_write(&p,wav+i,1));music_wav_header(&p);assert(!p.failed);}
    assert(p.ready && p.rate==44100 && p.channels==2 && music_pcm_available(&p)==8);
    u8 pcm[8];assert(music_pcm_read(&p,pcm,7)==4 && pcm[0]==1);
    assert(music_pcm_read(&p,pcm,8)==4 && pcm[0]==2 && !p.remaining);
    free(p.data);memset(&p,0,sizeof(p));p.capacity=512 * 1024;p.data=malloc(p.capacity);
    wav[32]=3;assert(music_ring_write(&p,wav,sizeof(wav)));assert(!music_wav_header(&p)&&p.failed);free(p.data);
    memset(&p,0,sizeof(p));p.capacity=512 * 1024;p.data=malloc(p.capacity);p.ready=true;p.frame_bytes=4;
    u8 *data=malloc((512 * 1024));for(size_t i=0;i<(512 * 1024);++i)data[i]=(u8)i;
    assert(music_ring_write(&p,data,(512 * 1024)));assert(!music_ring_write(&p,data,4));
    u8 *out=malloc((512 * 1024));assert(music_pcm_read(&p,out,12348)==12348);
    assert(!memcmp(data,out,12348));assert(music_ring_write(&p,data,12348));
    assert(music_pcm_read(&p,out,(512 * 1024))==(512 * 1024));
    assert(!memcmp(out,data+12348,(512 * 1024)-12348));assert(!memcmp(out+(512 * 1024)-12348,data,12348));
    free(data);free(out);free(p.data);
}
int main(int argc,char **argv)
{
    assert(argc==2);curl_global_init(CURL_GLOBAL_DEFAULT);parser_tests();
    copy_safe(g_cfg.server,sizeof(g_cfg.server),argv[1]);strcpy(g_cfg.token,"test-token");strcpy(g_cfg.user_id,"user1");strcpy(g_cfg.device_id,"test-device");g_cfg.volume_percent=175;g_cfg.music=music_sound_defaults();
    g_item_count=2;
    for(int i=0;i<2;++i) {snprintf(g_items[i].id,sizeof(g_items[i].id),"song%d",i+1);strcpy(g_items[i].type,"Audio");g_items[i].runtime_ticks=(i?1:8)*TICKS_PER_SECOND;g_items[i].media_source_count=1;}
    assert(music_start(&g_items[0]));wait_ready();
    assert(sink_rate==48000 && sink_format==NDSP_FORMAT_STEREO_PCM16 && g_music.stream.capacity==1024*1024);
    CURL *connection=g_music.http; CURLM *multi=g_music.multi;
    u8 *ring=g_music.stream.data; s16 *pcm=g_music.pcm[0];
    for(int i=0;i<500;++i)tick(false);
    assert(g_music_io.flow_paused && g_music.stream.used<=g_music.stream.capacity);
    music_toggle_pause();for(int i=0;i<20;++i)tick(false);u64 at=music_position();for(int i=0;i<20;++i)tick(true);assert(music_position()==at);
    music_toggle_pause();for(int i=0;i<120;++i)tick(true);assert(music_position()>TICKS_PER_SECOND);
    music_seek(2*TICKS_PER_SECOND);wait_ready();assert(music_position()==2*TICKS_PER_SECOND);
    assert(g_music.http==connection && g_music.multi==multi && g_music.stream.data==ring && g_music.pcm[0]==pcm);
    assert(dsp_inits==1 && pcm_allocations==MUSIC_BUFFER_COUNT);
    for(int i=0;i<10000 && g_music.phase!=MUSIC_ENDED && g_music.phase!=MUSIC_ERROR;++i)tick(true);
    if(g_music.phase==MUSIC_ERROR)fprintf(stderr,"%s\n",g_music.error);
    assert(g_music.phase==MUSIC_ENDED && g_music.index==1 && natural_stops==2);
    assert(reports==3 && stops==3 && heard_frames>7*48000ULL);
    assert(dsp_inits==1 && pcm_allocations==MUSIC_BUFFER_COUNT); /* No DSP/linear-memory churn between songs. */
    handle_music_input(KEY_B);assert(!g_music.active && g_view==VIEW_ITEMS && !sink_count);
    MediaItem steady=g_items[0];blocked_ui_test(&steady);g_item_count=2;
    assert(music_start(&g_items[0]));wait_ready();
    for(int i=0;i<80;++i)tick(true);
    music_toggle_pause();for(int i=0;i<20;++i)tick(false);at=music_position();
    handle_music_input(KEY_SELECT);assert(g_view==VIEW_MUSIC_SETTINGS);
    handle_music_settings(KEY_LEFT);assert(g_music_settings_draft.sample_rate==44100);
    assert(g_cfg.music.sample_rate==48000 && g_music.paused);
    handle_music_settings(KEY_B);assert(g_view==VIEW_MUSIC && g_cfg.music.sample_rate==48000 && music_position()==at);
    open_music_settings();handle_music_settings(KEY_LEFT);handle_music_settings(KEY_DOWN);handle_music_settings(KEY_A);
    handle_music_settings(KEY_DOWN);handle_music_settings(KEY_LEFT);handle_music_settings(KEY_LEFT); /* 128 KB, mono 44.1 kHz */
    handle_music_settings(KEY_SELECT);assert(g_view==VIEW_MUSIC && saved_configs==1);wait_ready();
    assert(g_music.paused && music_position()==at && sink_rate==44100 && sink_format==NDSP_FORMAT_MONO_PCM16);
    assert(g_music.stream.capacity==128*1024 && g_music.index==0 && g_music.count==2);
    assert(g_cfg.volume_percent==175); /* Music controls never change video volume. */
    for(int i=0;i<20;++i)tick(true);
    assert(music_position()==at);
    int starts=reports;
    open_music_settings();g_music_settings_row=3;handle_music_settings(KEY_LEFT);handle_music_settings(KEY_SELECT);
    assert(g_cfg.music.volume_percent==95 && g_music.phase==MUSIC_STREAM && reports==starts);
    open_music_settings();g_music_settings_row=0;handle_music_settings(KEY_LEFT);handle_music_settings(KEY_SELECT);wait_ready();
    assert(sink_rate==32000 && g_music.paused && music_position()==at);
    open_music_settings();g_music_settings_row=2;handle_music_settings(KEY_LEFT);handle_music_settings(KEY_SELECT);wait_ready();
    assert(g_music.stream.capacity==1024*1024);
    open_music_settings();g_music_settings_row=4;handle_music_settings(KEY_A);
    assert(g_music_settings_draft.sample_rate==48000 && g_cfg.music.sample_rate==32000);
    handle_music_settings(KEY_SELECT);wait_ready();assert(sink_rate==48000 && sink_format==NDSP_FORMAT_STEREO_PCM16);
    music_toggle_pause();for(int i=0;i<50;++i)tick(true);assert(music_position()>at);
    handle_music_input(KEY_DOWN);assert(g_cfg.music.volume_percent==95 && g_cfg.volume_percent==175);
    int saves=saved_configs;handle_music_input(KEY_B);assert(saved_configs==saves+1 && !g_music.dsp && !g_music.http);
    MediaItem jitter=g_items[0];strcpy(jitter.id,"jitter");jitter.runtime_ticks=3*TICKS_PER_SECOND;g_item_count=0;
    assert(music_start(&jitter));wait_ready();
    bool underrun=false, recovered=false;
    u64 heard_before=heard_frames;
    for(int i=0;i<6000 && g_music.phase==MUSIC_STREAM;++i) {
        /* Drain faster than the server bursts, independent of host timer resolution. */
        consume(MUSIC_BUFFER_FRAMES * MUSIC_BUFFER_COUNT);tick(false);
        if(g_music.buffering) underrun=true;
        if(underrun && !g_music.buffering && g_music.phase==MUSIC_STREAM) recovered=true;
    }
    if(!underrun || !recovered || g_music.phase!=MUSIC_ENDED)
        fprintf(stderr,"Jitter: underrun=%d recovered=%d phase=%d error=%s\n",underrun,recovered,g_music.phase,g_music.error);
    assert(underrun && recovered && g_music.phase==MUSIC_ENDED);
    assert(heard_frames-heard_before==3*48000);
    handle_music_input(KEY_B);
    MediaItem bad=g_items[0];strcpy(bad.id,"badwav");g_item_count=0;
    assert(music_start(&bad));for(int i=0;i<6000 && g_music.phase!=MUSIC_ERROR;++i)tick(true);
    assert(g_music.phase==MUSIC_ERROR && !g_music.dsp && !g_music.stream.data && !g_music.http);
    handle_music_input(KEY_B);assert(!g_music.active);
    assert(music_start(&g_items[0]));music_stop();assert(!g_music.http && !g_music.active && !g_music.stream.data);
    for(int fail_after=0;fail_after<2;++fail_after) {
        host_thread_fail_after=fail_after;
        assert(music_start(&g_items[0]));
        for(int i=0;i<6000 && g_music.phase!=MUSIC_ERROR;++i)tick(false);
        assert(g_music.phase==MUSIC_ERROR && !g_music_io.running && !atomic_load(&host_thread_count));
        assert(!g_music.http && !g_music.stream.data && !g_music.dsp);
        music_stop();g_view=VIEW_ITEMS;
    }
    host_thread_fail_after=-1;
    for(int fail_after=0;fail_after<=3;fail_after+=3) {
        allocation_fail_after=fail_after;
        assert(music_start(&g_items[0]));
        for(int i=0;i<6000 && g_music.phase!=MUSIC_ERROR;++i)tick(false);
        assert(g_music.phase==MUSIC_ERROR && !g_music_io.running && !atomic_load(&host_thread_count));
        assert(!g_music.http && !g_music.stream.data && !g_music.dsp);
        for(int i=0;i<MUSIC_BUFFER_COUNT;++i)assert(!g_music.pcm[i]);
        music_stop();g_view=VIEW_ITEMS;
    }
    allocation_fail_after=-1;
    g_core1_reader_available=false;
    assert(music_start(&g_items[0]));wait_ready();music_toggle_pause();
    music_stop();assert(!atomic_load(&host_thread_count));
    curl_global_cleanup();
    puts("PASS: PCM stereo/mono at 32/44.1/48 kHz, split WAV headers, backpressure, pause, seeking, queue advance and cleanup");
    puts("PASS: sound settings apply/cancel/defaults, paused position and queue preserved, separate volume, 128/512/1024 KB buffers");
    puts("PASS: connections, DSP and audio buffers reused across seeking and automatic track changes");
    puts("PASS: delayed network delivery recovers from underruns without dropping PCM or ending the song early");
    puts("PASS: audio continues with zero underruns through a 1.5-second blocked UI/server request");
    puts("PASS: worker creation failures, same-core fallback and cancellation while paused leave no threads or audio allocated");
}
