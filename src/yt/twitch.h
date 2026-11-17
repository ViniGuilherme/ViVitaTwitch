/* Twitch GraphQL and playback helpers for the Vita client. */
#ifndef VITUBE_YT_TWITCH_H
#define VITUBE_YT_TWITCH_H

#include "yt/model.h"

int tw_top_streams(volatile int *cancel, yt_video_list_t *out);
int tw_search(const char *query, volatile int *cancel,
              yt_video_list_t *out, yt_channel_list_t *out_channels);
int tw_resolve_stream(const char *channel_login, volatile int *cancel,
                      yt_stream_t *out);
const char *tw_last_error(void);

#endif
