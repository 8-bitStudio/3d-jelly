/* Jellyfin music metadata and requests, shared by browsing and playback. */
static bool music_is_audio(const MediaItem *item)
{
    return item && !strcmp(item->type, "Audio");
}

static bool music_is_item(const MediaItem *item)
{
    return item && (music_is_audio(item) || !strcmp(item->type, "MusicAlbum") || !strcmp(item->type, "MusicArtist"));
}

static bool music_is_root_type(const char *type)
{
    return !strcmp(type, "MusicAlbums") || !strcmp(type, "MusicArtists") || !strcmp(type, "MusicSongs");
}

static const char *music_mode_type(int mode)
{
    return mode == 1 ? "MusicArtists" : mode == 2 ? "MusicSongs" : "MusicAlbums";
}

static int music_mode_for_type(const char *type)
{
    return !strcmp(type, "MusicArtists") ? 1 : !strcmp(type, "MusicSongs") ? 2 : 0;
}

static void music_parse_artists(const char *start, const char *end, char *out, size_t size)
{
    if (!out || !size) return;
    const char *p = find_key_range(start, end, "Artists");
    out[0] = 0;
    if (p && p < end && *p == '[') {
        p = skip_ws(p + 1, end);
        while (p < end && *p == '"') {
            const char *q = p + 1;
            for (; q < end && *q != '"'; ++q) if (*q == '\\' && q + 1 < end) ++q;
            if (q == end || q - p > 450) break;
            char object[480], name[128];
            snprintf(object, sizeof(object), "{\"Name\":%.*s}", (int)(q - p + 1), p);
            json_get_string_range(object, object + strlen(object), "Name", name, sizeof(name));
            size_t used = strlen(out);
            if (used) append_text(out, size, &used, ", ");
            append_text(out, size, &used, name);
            p = skip_ws(q + 1, end);
            if (p == end || *p != ',') break;
            p = skip_ws(p + 1, end);
        }
    }
    if (!out[0]) json_get_string_range(start, end, "AlbumArtist", out, size);
}

static bool music_build_items_path(char *path, size_t size, const char *parent, const char *type, int start, int limit)
{
    const char *fields = "Overview,ChildCount,RecursiveItemCount,MediaSources,Genres";
    if (!strcmp(type, "MusicArtists")) {
        snprintf(path, size, "/Artists?UserId=%s&ParentId=%s&StartIndex=%d&Limit=%d&SortBy=SortName&SortOrder=Ascending&EnableImages=true&EnableUserData=true&Fields=%s",
                 g_cfg.user_id, parent, start, limit, fields);
    } else if (music_is_root_type(type) || !strcmp(type, "MusicArtist") || !strcmp(type, "MusicAlbum")) {
        bool songs = !strcmp(type, "MusicSongs") || !strcmp(type, "MusicAlbum");
        bool artist = !strcmp(type, "MusicArtist");
        snprintf(path, size, "/Users/%s/Items?%s=%s&Recursive=true&IncludeItemTypes=%s&StartIndex=%d&Limit=%d&SortBy=%s&SortOrder=Ascending&EnableImages=true&EnableUserData=true&Fields=%s",
                 g_cfg.user_id, artist ? "ArtistIds" : "ParentId", parent, songs ? "Audio" : "MusicAlbum", start, limit,
                 !strcmp(type, "MusicAlbum") ? "ParentIndexNumber,IndexNumber,SortName" : "SortName", fields);
    } else return false;
    return true;
}

static void music_build_playback_body(char *out, size_t size, u64 ticks, const MusicSoundSettings *sound)
{
    snprintf(out, size,
        "{\"UserId\":\"%s\",\"StartTimeTicks\":%llu,\"IsPlayback\":true,\"EnableDirectPlay\":false,"
        "\"EnableDirectStream\":false,\"EnableTranscoding\":true,\"MaxAudioChannels\":%d,"
        "\"DeviceProfile\":{\"Name\":\"3dJelly Music\",\"MaxStreamingBitrate\":2000000,\"MusicStreamingTranscodingBitrate\":%d,"
        "\"DirectPlayProfiles\":[],\"TranscodingProfiles\":[{\"Container\":\"wav\",\"Type\":\"Audio\","
        "\"AudioCodec\":\"pcm_s16le\",\"Protocol\":\"http\",\"Context\":\"Streaming\",\"MaxAudioChannels\":\"%d\"}]}}",
        g_cfg.user_id, (unsigned long long)ticks, sound->channels, sound->sample_rate * sound->channels * 16, sound->channels);
}

static void music_build_stream_path(char *out, size_t size, const MediaItem *item, u64 ticks, const MusicSoundSettings *sound)
{
    char id[240], device[240], source[384], session[288];
    url_encode(item->id, id, sizeof(id));
    url_encode(g_cfg.device_id, device, sizeof(device));
    url_encode(g_play_media_source_id, source, sizeof(source));
    url_encode(g_play_session, session, sizeof(session));
    snprintf(out, size, "/Audio/%s/stream.wav?DeviceId=%s&MediaSourceId=%s&PlaySessionId=%s&Static=false&AudioCodec=pcm_s16le&AudioSampleRate=%d&AudioChannels=%d&MaxAudioChannels=%d&EnableAutoStreamCopy=false&AllowAudioStreamCopy=false&StartTimeTicks=%llu",
             id, device, source, session, sound->sample_rate, sound->channels, sound->channels, (unsigned long long)ticks);
}
