#pragma once
// Automatic lyrics lookup: queries every provider in parallel, validates and scores each
// result, and returns the best match, always preferring correctly timestamped (synced) lyrics.
#include <functional>
#include <string>
#include <vector>

namespace lyricfinder {

struct Query {
	std::string title, artist, album;   // raw tags
	double duration = 0;                // seconds, 0 = unknown
	bool musixmatch = false;            // opt-in provider (lyricbar.enable_musixmatch)
	std::string title_clean, artist_clean;   // filled in by find_best()
};

struct Candidate {
	std::string provider, artist, title, album;
	double duration = 0;     // seconds, 0 = unknown
	std::string lyrics;      // LRC text if synced, plain text otherwise
	bool synced = false;
	double score = 0;
	int prio = 0;            // provider order, used as a tie-break
	bool confident = false;  // good enough to stop waiting for other providers
};

enum class Result { Found, NotFound, NetworkError, Cancelled };

using Cancel = std::function<bool()>;

// Searches all providers. Returns Found and fills `out` on success.
Result find_best(const Query &q, Candidate &out, const Cancel &cancelled = {}, int timeout_ms = 12000);

std::vector<std::string> provider_names();

// ---- helpers (exposed for the UI code and tests) ----
bool looks_synced(const std::string &text);              // >= 3 timestamped lines, mostly timestamped
std::string normalize_lrc(const std::string &text);      // canonical "[mm:ss.xx]text" lines, metadata dropped
std::string strip_timestamps(const std::string &text);   // synced -> plain text
double similarity(const std::string &a, const std::string &b);   // 0..1 on normalized text
std::string clean_title(const std::string &s);            // drops (..), [..], feat., "- Remastered"
std::string base64_decode(const std::string &in);
std::string html_unescape(const std::string &in);

}  // namespace lyricfinder
