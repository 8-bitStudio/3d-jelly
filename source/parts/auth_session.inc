/* Do not commit a partial authentication response over a saved session. */
static bool accept_authentication_response(const char *body, size_t size, bool quick_connect)
{
    if (!body) return false;
    const char *end = body + size, *root, *root_end, *user, *user_end;
    char token[sizeof(g_cfg.token)] = {0};
    char id[sizeof(g_cfg.user_id)] = {0};
    char name[sizeof(g_cfg.username)] = {0};
    if (!json_object_range_after(body, end, &root, &root_end) ||
        skip_ws(root_end, end) != end ||
        !json_get_string_range(root, root_end, "AccessToken", token, sizeof(token)) ||
        !json_get_object_range(root, root_end, "User", &user, &user_end) ||
        !json_get_string_range(user, user_end, "Id", id, sizeof(id)) ||
        !token[0] || !id[0] || strlen(token) >= sizeof(token) - 1 || strlen(id) >= sizeof(id) - 1)
        return false;
    json_get_string_range(user, user_end, "Name", name, sizeof(name));
    if (quick_connect && !name[0]) return false;
    copy_safe(g_cfg.token, sizeof(g_cfg.token), token);
    copy_safe(g_cfg.user_id, sizeof(g_cfg.user_id), id);
    if (name[0]) copy_safe(g_cfg.username, sizeof(g_cfg.username), name);
    if (quick_connect) g_cfg.password[0] = 0;
    save_config();
    g_session_capabilities_reported = false;
    return true;
}
