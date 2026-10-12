#include "paxsenix.h"

#include "../json.h"

#include <set>

using std::string;
using std::vector;

namespace {

const char *const kRoot = "https://lyrics.paxsenix.org";
const char *const kUA = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";

string fetch(const string &url) {
	curl_slist *sl = nullptr;
	sl = curl_slist_append(sl, kUA);
	return text_downloader(sl, url, "");
}

// kopuz `should_insert_apple_space`
bool needs_space(const string &current, bool previous_part_continues, const string &next) {
	if (current.empty() || previous_part_continues || next.empty()) return false;
	char c = next[0];
	if (c == ',' || c == '.' || c == '?' || c == '!' || c == ':' || c == ';' || c == ')' || c == ']' || c == '}' || c == '\'')
		return false;
	if (next.compare(0, 3, "\xE2\x80\x99") == 0) return false;   // ’
	return true;
}

// Joins syllable parts; returns the text and the first part timestamp (ms, -1 if none).
string join_parts(const mj::Value &parts, double &first_ts_ms) {
	string text;
	bool prev_continues = false;
	first_ts_ms = -1;
	for (size_t i = 0; i < parts.size(); i++) {
		const mj::Value &p = parts[i];
		string t = p["text"].str();
		if (srcutil::trim_copy(t).empty()) continue;
		if (needs_space(text, prev_continues, t)) text += ' ';
		text += t;
		if (first_ts_ms < 0 && p["timestamp"].type == mj::Value::Num) first_ts_ms = p["timestamp"].num();
		prev_continues = p["part"].truthy();
	}
	return srcutil::trim_copy(text);
}

bool row_has_timing(const mj::Value &rows) {
	for (size_t i = 0; i < rows.size(); i++) {
		const mj::Value &r = rows[i];
		if (r["timestamp"].num() > 0 || r["endtime"].num() > 0) return true;
		for (const char *key : {"text", "backgroundText"})
			for (size_t k = 0; k < r[key].size(); k++)
				if (r[key][k]["timestamp"].num() > 0) return true;
	}
	return false;
}

}  // namespace

parsed_lyrics paxsenix_apple_parse(const string &body) {
	mj::Value root = mj::parse(body);
	if (root.type != mj::Value::Obj) return {"", false};

	// 1. Line/syllable timed content.
	const mj::Value &rows = root["content"];
	if (row_has_timing(rows)) {
		string out;
		std::set<long> distinct;
		for (size_t i = 0; i < rows.size(); i++) {
			const mj::Value &r = rows[i];
			double start_ms = r["timestamp"].num();
			double ts;
			string main_text = join_parts(r["text"], ts);
			bool has_bg_text = r["backgroundText"].size() > 0;
			bool main_is_bg = r["background"].truthy() && !has_bg_text;
			if (!main_text.empty()) {
				if (main_is_bg) main_text = "(" + main_text + ")";
				out += "[" + srcutil::lrc_time(start_ms / 1000.0) + "]" + main_text + "\n";
				distinct.insert((long)(start_ms / 10));
			}
			if (has_bg_text) {
				double bg_ts;
				string bg = join_parts(r["backgroundText"], bg_ts);
				if (!bg.empty()) {
					double t = bg_ts >= 0 ? bg_ts : start_ms;
					out += "[" + srcutil::lrc_time(t / 1000.0) + "](" + bg + ")\n";
					distinct.insert((long)(t / 10));
				}
			}
		}
		if (distinct.size() >= 2) return {out, true};
	}

	// 2. Ready-made LRC.
	string lrc = root["lrc"].str();
	if (srcutil::timestamped_lines(lrc) >= 2) return {lrc, true};

	// 3. Plain text.
	string plain = root["plain"].str();
	if (!srcutil::trim_copy(plain).empty()) return {plain, false};
	return {"", false};
}

vector<ProviderHit> paxsenix_apple_search(const string &song, const string &artist) {
	vector<ProviderHit> hits;
	string query = srcutil::trim_copy(song + " " + artist);
	if (query.empty()) return hits;
	string body = fetch("https://itunes.apple.com/search?term=" + urlencode(query) + "&entity=song&limit=8&country=US");
	mj::Value root = mj::parse(body);
	const mj::Value &results = root["results"];
	for (size_t i = 0; i < results.size(); i++) {
		const mj::Value &s = results[i];
		ProviderHit h;
		h.id = s["trackId"].str();
		h.title = s["trackName"].str();
		h.artist = s["artistName"].str();
		h.album = s["collectionName"].str();
		h.duration = s["trackTimeMillis"].num() / 1000.0;
		if (!h.id.empty() && !h.title.empty()) hits.push_back(h);
	}
	return hits;
}

parsed_lyrics paxsenix_apple_lyrics_downloader(const string &id) {
	if (id.empty() || id.find_first_not_of("0123456789") != string::npos) return {"", false};
	return paxsenix_apple_parse(fetch(string(kRoot) + "/apple-music/lyrics?id=" + id));
}

vector<ProviderHit> paxsenix_youtube_search(const string &song, const string &artist) {
	vector<ProviderHit> hits;
	string query = srcutil::trim_copy(song + " " + artist);
	if (query.empty()) return hits;
	mj::Value root = mj::parse(fetch(string(kRoot) + "/youtube/search?q=" + urlencode(query)));
	for (size_t i = 0; i < root.size(); i++) {
		const mj::Value &v = root[i];
		ProviderHit h;
		h.id = v["videoId"].str();
		h.title = v["title"].str();
		h.artist = v["author"].str();
		double d = srcutil::parse_colon_duration(v["duration"].str());
		h.duration = d > 0 ? d : 0;
		if (!h.id.empty()) hits.push_back(h);
	}
	return hits;
}

parsed_lyrics paxsenix_youtube_lyrics_downloader(const string &video_id) {
	if (video_id.empty()) return {"", false};
	string body = fetch(string(kRoot) + "/youtube/lyrics?id=" + urlencode(video_id));
	string t = srcutil::trim_copy(body);
	if (t.empty() || t[0] == '{') return {"", false};   // error object instead of LRC text
	if (srcutil::timestamped_lines(body) >= 3) return {body, true};
	return {"", false};   // kopuz only accepts this provider when the LRC has usable timing
}

static vector<string> flatten(const vector<ProviderHit> &hits) {
	vector<string> flat;
	for (const ProviderHit &h : hits) {
		flat.push_back(h.artist);
		flat.push_back(h.title);
		flat.push_back(h.album);
		flat.push_back(h.id);
	}
	return flat;
}

vector<string> paxsenix_apple_get_songs(const string &song, const string &artist) {
	return flatten(paxsenix_apple_search(song, artist));
}

vector<string> paxsenix_youtube_get_songs(const string &song, const string &artist) {
	return flatten(paxsenix_youtube_search(song, artist));
}
