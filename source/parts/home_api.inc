/* Home rows use the server's watch state and actual user libraries. */
static const char *const FAVORITES_PARENT_ID = "#FAVORITES#";

static void browse_home_reset(void)
{
    browse_cancel_request();
    memset(g_home_rows, 0, sizeof(g_home_rows));
    g_home_row = g_home_scroll = 0;
    g_library_selected = g_library_first = 0;
    g_home_dirty = false;
}

static void build_home_row_path(int row, char *path, size_t size)
{
    const char *fields = "Overview,PrimaryImageAspectRatio,ChildCount,RecursiveItemCount";
    if (row == 1) {
        snprintf(path, size,
                 "/Users/%s/Items/Resume?Limit=%d&MediaTypes=Video&EnableUserData=true&Fields=%s",
                 g_cfg.user_id, HOME_ROW_ITEMS, fields);
    } else if (row == 2) {
        snprintf(path, size,
                 "/Shows/NextUp?UserId=%s&Limit=%d&EnableResumable=false&EnableRewatching=false&EnableUserData=true&Fields=%s",
                 g_cfg.user_id, HOME_ROW_ITEMS, fields);
    } else if (row >= 3 && row < g_library_count + 3) {
        char id[240];
        url_encode(g_libraries[row - 3].id, id, sizeof(id));
        snprintf(path, size,
                 "/Users/%s/Items/Latest?ParentId=%s&Limit=%d&GroupItems=true&EnableUserData=true&Fields=%s",
                 g_cfg.user_id, id, HOME_ROW_ITEMS, fields);
    } else {
        path[0] = 0;
    }
}

static u64 media_resume_position(const MediaItem *item)
{
    if (!item || item->played || !item->position_ticks ||
        (item->runtime_ticks && item->position_ticks >= item->runtime_ticks)) return 0;
    return item->position_ticks;
}

static void refresh_play_item(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/Users/%s/Items/%s", g_cfg.user_id, g_current.id);
    HttpResponse res;
    Result ret = api_get(path, &res);
    if (R_SUCCEEDED(ret) && res.status == 200 && res.body) {
        MediaItem item;
        parse_media_item_object(res.body, res.body + res.size, &item);
        if (!strcmp(item.id, g_current.id)) g_current = item;
    }
    free_response(&res);
}

static void apply_favorite_state(const char *id, bool favorite)
{
    for (int i = 0; i < g_item_count; ++i)
        if (!strcmp(g_items[i].id, id)) g_items[i].is_favorite = favorite;
    for (int r = 0; r < g_library_count + 2; ++r)
        for (int i = 0; i < g_home_rows[r].count; ++i)
            if (!strcmp(g_home_rows[r].items[i].id, id))
                g_home_rows[r].items[i].is_favorite = favorite;
}

static bool toggle_favorite(MediaItem *item)
{
    if (!item || !item->id[0]) return false;
    char path[400], id[240];
    url_encode(item->id, id, sizeof(id));
    snprintf(path, sizeof(path), "/UserFavoriteItems/%s?UserId=%s", id, g_cfg.user_id);
    HttpResponse res;
    bool favorite = !item->is_favorite;
    Result ret = favorite ? api_post(path, "", true, &res) : api_delete(path, &res);
    bool ok = R_SUCCEEDED(ret) && res.status >= 200 && res.status < 300;
    if (ok) {
        item->is_favorite = favorite;
        apply_favorite_state(item->id, favorite);
        set_status("%s", favorite ? "Added to Favorites" : "Removed from Favorites");
    } else {
        set_http_failure("Could not update Favorites", &res, ret);
    }
    free_response(&res);
    return ok;
}
