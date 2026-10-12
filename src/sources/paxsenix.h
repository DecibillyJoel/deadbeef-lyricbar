#pragma once
// Lyrics providers ported from kopuz (crates/server/src/lyrics/paxsenix.rs):
//  * Apple Music lyrics through lyrics.paxsenix.org (track found via the iTunes search API)
//  * YouTube lyrics through lyrics.paxsenix.org (video found via its search endpoint)
#include "../utils.h"
#include "match.h"

#ifdef __cplusplus
#include <string>
#include <vector>

std::vector<ProviderHit> paxsenix_apple_search(const std::string &song, const std::string &artist);
parsed_lyrics paxsenix_apple_lyrics_downloader(const std::string &itunes_track_id);

std::vector<ProviderHit> paxsenix_youtube_search(const std::string &song, const std::string &artist);
parsed_lyrics paxsenix_youtube_lyrics_downloader(const std::string &video_id);

// Flat [artist, title, album, id] groups for the manual search dialog.
std::vector<std::string> paxsenix_apple_get_songs(const std::string &song, const std::string &artist);
std::vector<std::string> paxsenix_youtube_get_songs(const std::string &song, const std::string &artist);

// Pure conversion of the Paxsenix Apple Music JSON body to LRC / plain text (exposed for tests).
parsed_lyrics paxsenix_apple_parse(const std::string &json_body);
#endif
