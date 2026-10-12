#include "musixmatch.h"

#include "../json.h"

#include <chrono>
#include <mutex>

using std::string;
using std::vector;

namespace {

const char *const kRoot = "https://apic-desktop.musixmatch.com/ws/1.1/";
const char *const kApp = "web-desktop-app-v1.0";

struct TokenCache {
	std::mutex m;
	string value;
	long long expires_ms = 0;
};

TokenCache &cache() {
	static TokenCache c;
	return c;
}

long long now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

string get(const string &action, const string &params) {
	curl_slist *sl = nullptr;
	sl = curl_slist_append(sl, "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
	return text_downloader(sl, string(kRoot) + action + "?" + params + "&app_id=" + kApp + "&t=" + std::to_string(now_ms()), "");
}

bool ok_status(const mj::Value &root) { return (long)root["message"]["header"]["status_code"].num() == 200; }

string token() {
	TokenCache &c = cache();
	{
		std::lock_guard<std::mutex> lk(c.m);
		if (!c.value.empty() && c.expires_ms > now_ms()) return c.value;
	}
	mj::Value root = mj::parse(get("token.get", "user_language=en"));
	if (!ok_status(root)) return "";
	string t = root["message"]["body"]["user_token"].str();
	if (!t.empty()) {
		std::lock_guard<std::mutex> lk(c.m);
		c.value = t;
		c.expires_ms = now_ms() + 10 * 60 * 1000;
	}
	return t;
}

}  // namespace

string musixmatch_richsync_to_lrc(const string &body) {
	mj::Value rows = mj::parse(body);
	if (rows.type != mj::Value::Arr) return "";
	string out;
	for (size_t i = 0; i < rows.size(); i++) {
		const mj::Value &row = rows[i];
		if (row["ts"].is_null()) continue;
		string text;
		const mj::Value &words = row["l"];
		for (size_t k = 0; k < words.size(); k++) {
			string c = words[k]["c"].str();
			if (srcutil::trim_copy(c).empty()) continue;
			text += srcutil::trim_copy(c);
			text += ' ';
		}
		text = srcutil::trim_copy(text);
		out += "[" + srcutil::lrc_time(row["ts"].num()) + "]" + text + "\n";
	}
	return out;
}

vector<ProviderHit> musixmatch_search(const string &song, const string &artist) {
	vector<ProviderHit> hits;
	string query = srcutil::trim_copy(song + " " + artist);
	string tok = token();
	if (query.empty() || tok.empty()) return hits;
	mj::Value root = mj::parse(get("track.search", "q=" + urlencode(query) + "&page_size=5&page=1&usertoken=" + urlencode(tok)));
	if (!ok_status(root)) return hits;
	const mj::Value &list = root["message"]["body"]["track_list"];
	for (size_t i = 0; i < list.size(); i++) {
		const mj::Value &t = list[i]["track"];
		ProviderHit h;
		h.id = t["track_id"].str();
		h.title = t["track_name"].str();
		h.artist = t["artist_name"].str();
		h.album = t["album_name"].str();
		h.duration = t["track_length"].num();
		if (!h.id.empty()) hits.push_back(h);
	}
	return hits;
}

parsed_lyrics musixmatch_lyrics_downloader(const string &id) {
	if (id.empty() || id.find_first_not_of("0123456789") != string::npos) return {"", false};
	string tok = token();
	if (tok.empty()) return {"", false};
	mj::Value root = mj::parse(get("track.richsync.get", "track_id=" + id + "&usertoken=" + urlencode(tok)));
	if (!ok_status(root)) return {"", false};
	string lrc = musixmatch_richsync_to_lrc(root["message"]["body"]["richsync"]["richsync_body"].str());
	if (srcutil::timestamped_lines(lrc) < 3) return {"", false};
	return {lrc, true};
}

vector<string> musixmatch_get_songs(const string &song, const string &artist) {
	vector<string> flat;
	for (const ProviderHit &h : musixmatch_search(song, artist)) {
		flat.push_back(h.artist);
		flat.push_back(h.title);
		flat.push_back(h.album);
		flat.push_back(h.id);
	}
	return flat;
}
