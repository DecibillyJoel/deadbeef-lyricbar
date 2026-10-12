#pragma once
// Musixmatch "richsync" lyrics, ported from kopuz (crates/server/src/lyrics/musixmatch.rs).
// Uses the anonymous token of Musixmatch's desktop web app; opt-in (lyricbar.enable_musixmatch).
#include "../utils.h"
#include "match.h"

#ifdef __cplusplus
#include <string>
#include <vector>

std::vector<ProviderHit> musixmatch_search(const std::string &song, const std::string &artist);
parsed_lyrics musixmatch_lyrics_downloader(const std::string &track_id);
std::vector<std::string> musixmatch_get_songs(const std::string &song, const std::string &artist);

// Pure conversion of a richsync_body JSON string to line-synced LRC (exposed for tests).
std::string musixmatch_richsync_to_lrc(const std::string &richsync_body);
#endif
