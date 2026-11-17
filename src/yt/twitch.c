#include "yt/twitch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "net/http.h"

#define TWITCH_GQL "https://gql.twitch.tv/gql"
#define TWITCH_CLIENT_ID "kimne78kx3ncx6brgo4mv6wki5h1ko"

static char g_last_error[160];

const char *tw_last_error(void) {
    return g_last_error;
}

static void set_error(const char *text) {
    snprintf(g_last_error, sizeof g_last_error, "%s", text);
}

static const char *json_string(json_t *obj, const char *key) {
    json_t *value = json_object_get(obj, key);
    return json_is_string(value) ? json_string_value(value) : NULL;
}

static int json_integer(json_t *obj, const char *key) {
    json_t *value = json_object_get(obj, key);
    return json_is_integer(value) ? (int)json_integer_value(value) : 0;
}

static void preview_url(char *dst, size_t cap, const char *url) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", url ? url : "");
    char *w = strstr(tmp, "{width}");
    if (w) { memmove(w + 3, w + 7, strlen(w + 7) + 1); memcpy(w, "320", 3); }
    char *h = strstr(tmp, "{height}");
    if (h) { memmove(h + 3, h + 8, strlen(h + 8) + 1); memcpy(h, "180", 3); }
    snprintf(dst, cap, "%s", tmp);
}

static void copy_node(const json_t *node, yt_video_t *video) {
    if (!json_is_object(node)) return;
    const char *login = json_string((json_t *)node, "broadcasterLogin");
    if (!login) login = json_string((json_t *)node, "login");
    if (!login) login = json_string((json_t *)node, "channelLogin");
    json_t *broadcaster = json_object_get((json_t *)node, "broadcaster");
    if (!login && json_is_object(broadcaster))
        login = json_string(broadcaster, "login");
    if (!login) return;

    json_t *stream = json_object_get((json_t *)node, "stream");
    json_t *source = json_is_object(stream) ? stream : (json_t *)node;
    const char *title = json_string(source, "title");
    const char *display = json_string((json_t *)node, "displayName");
    if (!display && json_is_object(broadcaster))
        display = json_string(broadcaster, "displayName");
    const char *preview = json_string(source, "previewImageURL");
    if (!preview) preview = json_string((json_t *)node, "thumbnailURL");

    json_t *game = json_object_get(source, "game");
    const char *game_name = json_is_object(game) ? json_string(game, "displayName") : NULL;
    int viewers = json_integer(source, "viewersCount");
    if (!title) title = "";
    if (!display) display = login;

    snprintf(video->video_id, sizeof video->video_id, "%s", login);
    snprintf(video->channel_id, sizeof video->channel_id, "%s", login);
    snprintf(video->title, sizeof video->title, "%s", title);
    snprintf(video->channel, sizeof video->channel, "%s", display);
    snprintf(video->views, sizeof video->views, "%d espectadores", viewers);
    if (game_name)
        snprintf(video->duration, sizeof video->duration, "%s", game_name);
    else
        video->duration[0] = '\0';
    preview_url(video->thumb_url, sizeof video->thumb_url, preview);
    video->age_min = -1;
}

static void walk_nodes(const json_t *value, yt_video_list_t *out) {
    if (out->count >= YT_LIST_MAX) return;
    if (json_is_array(value)) {
        size_t index;
        json_t *item;
        json_array_foreach(value, index, item) walk_nodes(item, out);
        return;
    }
    if (!json_is_object(value)) return;

    if (json_object_get(value, "broadcasterLogin") ||
        json_object_get(value, "channelLogin") ||
        json_is_object(json_object_get(value, "broadcaster")) ||
        (json_object_get(value, "login") &&
         (json_object_get(value, "title") ||
          json_is_object(json_object_get(value, "stream"))))) {
        yt_video_t video = {0};
        copy_node(value, &video);
        if (video.video_id[0]) {
            int duplicate = 0;
            for (int i = 0; i < out->count; i++)
                if (strcmp(out->items[i].video_id, video.video_id) == 0)
                    duplicate = 1;
            if (!duplicate) out->items[out->count++] = video;
        }
    }

    const char *key;
    json_t *child;
    json_object_foreach((json_t *)value, key, child) walk_nodes(child, out);
}

static int gql_post(const char *query, json_t *variables,
                    volatile int *cancel, http_buf_t *out) {
    json_t *body = json_object();
    json_object_set_new(body, "query", json_string(query));
    json_object_set(body, "variables", variables);
    char *payload = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    if (!payload) return -1;

    const char *headers[] = {
        "Client-Id: " TWITCH_CLIENT_ID,
        "Origin: https://www.twitch.tv",
        "Referer: https://www.twitch.tv/",
        NULL
    };
    long status = 0;
    int ret = http_post(TWITCH_GQL, "application/json", payload, strlen(payload),
                        headers, cancel, out, &status);
    free(payload);
    if (ret < 0) { set_error("Falha de rede Twitch"); return -1; }
    if (status < 200 || status >= 300) { set_error("Twitch recusou a consulta"); return -1; }
    return 0;
}

int tw_top_streams(volatile int *cancel, yt_video_list_t *out) {
    memset(out, 0, sizeof *out);
    g_last_error[0] = '\0';

    const char *query =
        "query { streams(first: 40) { edges { node { id title viewersCount "
        "previewImageURL(width:320,height:180) broadcaster { login displayName } "
        "game { displayName } } } } }";
    json_t *variables = json_object();
    http_buf_t response = {0};
    int ret = gql_post(query, variables, cancel, &response);
    json_decref(variables);
    if (ret != 0) { http_buf_free(&response); return ret; }

    json_error_t error;
    json_t *root = json_loads(response.data, 0, &error);
    http_buf_free(&response);
    if (!root) { set_error("Resposta Twitch invalida"); return -1; }
    json_t *errors = json_object_get(root, "errors");
    if (json_is_array(errors) && json_array_size(errors) > 0) {
        set_error("Twitch nao respondeu a consulta");
        json_decref(root);
        return -1;
    }
    walk_nodes(root, out);
    json_decref(root);
    if (out->count == 0) set_error("Nenhuma live encontrada");
    return out->count > 0 ? 0 : -1;
}

int tw_search(const char *query_text, volatile int *cancel,
              yt_video_list_t *out, yt_channel_list_t *out_channels) {
    memset(out, 0, sizeof *out);
    if (out_channels) memset(out_channels, 0, sizeof *out_channels);
    g_last_error[0] = '\0';

    const char *query =
        "query Search($query: String!) { searchFor(userQuery: $query, "
        "platform: \"WEB\") { channels { items { login displayName "
        "stream { title viewersCount previewImageURL(width:320,height:180) "
        "game { displayName } } } } } }";
    json_t *variables = json_object();
    json_object_set_new(variables, "query", json_string(query_text));
    http_buf_t response = {0};
    int ret = gql_post(query, variables, cancel, &response);
    json_decref(variables);
    if (ret != 0) { http_buf_free(&response); return ret; }

    json_error_t error;
    json_t *root = json_loads(response.data, 0, &error);
    http_buf_free(&response);
    if (!root) { set_error("Resposta Twitch invalida"); return -1; }
    walk_nodes(root, out);
    json_decref(root);
    if (out->count == 0) set_error("Nenhuma live encontrada");
    return out->count > 0 ? 0 : -1;
}

int tw_resolve_stream(const char *channel_login, volatile int *cancel,
                      yt_stream_t *out) {
    memset(out, 0, sizeof *out);
    g_last_error[0] = '\0';
    snprintf(out->channel, sizeof out->channel, "%s", channel_login);
    snprintf(out->video_id, sizeof out->video_id, "%s", channel_login);
    out->is_live = 1;

    const char *query =
        "query PlaybackAccessToken_Template($login: String!) { "
        "streamPlaybackAccessToken(channelName: $login, params: "
        "{platform: \"web\", playerBackend: \"mediaplayer\", playerType: \"site\"}) "
        "{ value signature } }";
    json_t *variables = json_object();
    json_object_set_new(variables, "login", json_string(channel_login));

    http_buf_t response = {0};
    int ret = gql_post(query, variables, cancel, &response);
    json_decref(variables);
    if (ret != 0) { http_buf_free(&response); return ret; }

    json_error_t error;
    json_t *root = json_loads(response.data, 0, &error);
    http_buf_free(&response);
    if (!root) { set_error("Resposta de playback invalida"); return -1; }
    json_t *pat = json_object_get(root, "data");
    pat = pat ? json_object_get(pat, "streamPlaybackAccessToken") : NULL;
    const char *token = pat ? json_string(pat, "value") : NULL;
    const char *signature = pat ? json_string(pat, "signature") : NULL;
    if (!token || !signature) {
        set_error("Live indisponivel");
        json_decref(root);
        return -1;
    }

    char *escaped = http_escape(token);
    if (!escaped) escaped = strdup(token);
    snprintf(out->hls_manifest, sizeof out->hls_manifest,
             "https://usher.ttvnw.net/api/channel/hls/%s.m3u8"
             "?sig=%s&token=%s&allow_source=true&allow_audio_only=true",
             channel_login, signature, escaped ? escaped : "");
    free(escaped);
    json_decref(root);
    return 0;
}
