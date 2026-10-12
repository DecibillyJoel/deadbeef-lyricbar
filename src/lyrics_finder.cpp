#include "lyrics_finder.h"
#include "json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <curl/curl.h>

#include "sources/musixmatch.h"
#include "sources/paxsenix.h"

#ifndef LYRICFINDER_NO_LEGACY
#include "sources/megalobiz.h"
#include "sources/rclyricsband.h"
#endif

// Provided by utils.cpp. text_downloader() takes ownership of (and frees) the header list.
std::string text_downloader(curl_slist *slist, std::string url, std::string post);
std::string urlencode(const std::string &s);

using std::string;
using std::vector;

namespace lyricfinder {

// ======================================================================
//  Small text helpers
// ======================================================================

static string lower_ascii(string s) {
	for (char &c : s)
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
	return s;
}

static string trim(const string &s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

static void replace_all(string &s, const string &from, const string &to) {
	if (from.empty()) return;
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
}

// Keeps ASCII letters/digits (lower-cased) and all UTF-8 bytes; everything else becomes one space.
static string normalize_text(const string &s) {
	string out;
	bool space = true;
	for (unsigned char c : s) {
		if ((c >= 'A' && c <= 'Z')) c = (unsigned char)(c - 'A' + 'a');
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
			out += (char)c;
			space = false;
		} else if (!space) {
			out += ' ';
			space = true;
		}
	}
	if (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
}

string clean_title(const string &s) {
	string t;
	int depth = 0;
	for (char c : s) {
		if (c == '(' || c == '[' || c == '{') depth++;
		else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
		else if (depth == 0) t += c;
	}
	string lower = lower_ascii(t);
	for (const char *tok : {" feat.", " feat ", " ft.", " ft ", " featuring "}) {
		size_t pos = lower.find(tok);
		if (pos != string::npos) { t.resize(pos); lower.resize(pos); }
	}
	size_t dash = lower.find(" - ");
	if (dash != string::npos) {
		string rest = lower.substr(dash + 3);
		for (const char *kw : {"remaster", "version", "mono", "stereo", "edit", "deluxe", "bonus", "single", "live"}) {
			if (rest.find(kw) != string::npos) { t.resize(dash); break; }
		}
	}
	t = trim(t);
	return t.empty() ? trim(s) : t;
}

// Dice coefficient on character bigrams of the space-free normalized strings.
static double sim_norm(const string &an, const string &bn) {
	if (an.empty() || bn.empty()) return 0;
	if (an == bn) return 1;
	string a, b;
	for (char c : an) if (c != ' ') a += c;
	for (char c : bn) if (c != ' ') b += c;
	if (a == b) return 1;
	if (a.size() < 2 || b.size() < 2) return 0;

	auto grams = [](const string &s) {
		vector<uint16_t> g;
		for (size_t i = 0; i + 1 < s.size(); i++)
			g.push_back((uint16_t)(((unsigned char)s[i] << 8) | (unsigned char)s[i + 1]));
		std::sort(g.begin(), g.end());
		return g;
	};
	vector<uint16_t> ga = grams(a), gb = grams(b);
	size_t i = 0, j = 0, common = 0;
	while (i < ga.size() && j < gb.size()) {
		if (ga[i] == gb[j]) { common++; i++; j++; }
		else if (ga[i] < gb[j]) i++;
		else j++;
	}
	double dice = 2.0 * (double)common / (double)(ga.size() + gb.size());

	size_t mn = std::min(a.size(), b.size()), mx = std::max(a.size(), b.size());
	if (mn >= 3 && (a.find(b) != string::npos || b.find(a) != string::npos))
		dice = std::max(dice, 0.5 + 0.5 * (double)mn / (double)mx);
	return dice;
}

double similarity(const string &a, const string &b) { return sim_norm(normalize_text(a), normalize_text(b)); }

static vector<string> artist_parts(const string &s) {
	string l = " " + lower_ascii(s) + " ";
	for (const char *sep : {" feat. ", " feat ", " ft. ", " ft ", " featuring ", " and ", " x ", " with ", " & ", "&", ",", ";", "/", " + "})
		replace_all(l, sep, "|");
	vector<string> parts;
	size_t start = 0;
	while (start <= l.size()) {
		size_t end = l.find('|', start);
		if (end == string::npos) end = l.size();
		string p = normalize_text(l.substr(start, end - start));
		if (!p.empty()) parts.push_back(p);
		start = end + 1;
	}
	return parts;
}

static double artist_sim(const string &qa, const string &ca) {
	double best = sim_norm(normalize_text(qa), normalize_text(ca));
	for (const string &x : artist_parts(qa))
		for (const string &y : artist_parts(ca))
			best = std::max(best, sim_norm(x, y));
	return best;
}

static bool artist_unknown(const string &a) {
	string n = normalize_text(a);
	return n.empty() || n == "unknown artist" || n == "unknown" || n == "various artists";
}

string base64_decode(const string &in) {
	static int8_t T[256];
	static std::once_flag once;
	std::call_once(once, [] {
		for (int i = 0; i < 256; i++) T[i] = -1;
		const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		for (int i = 0; i < 64; i++) T[(unsigned char)a[i]] = (int8_t)i;
		T[(unsigned char)'-'] = 62;   // url-safe variant
		T[(unsigned char)'_'] = 63;
	});
	string out;
	uint32_t buf = 0;
	int bits = 0;
	for (unsigned char c : in) {
		if (c == '=') break;
		int8_t v = T[c];
		if (v < 0) continue;
		buf = (buf << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += (char)((buf >> bits) & 0xFF);
		}
	}
	return out;
}

string html_unescape(const string &in) {
	string out;
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] != '&') { out += in[i]; continue; }
		size_t semi = in.find(';', i);
		if (semi == string::npos || semi - i > 10) { out += in[i]; continue; }
		string ent = in.substr(i + 1, semi - i - 1);
		string rep;
		bool ok = true;
		if (ent == "amp") rep = "&";
		else if (ent == "lt") rep = "<";
		else if (ent == "gt") rep = ">";
		else if (ent == "quot") rep = "\"";
		else if (ent == "apos") rep = "'";
		else if (ent == "nbsp") rep = " ";
		else if (!ent.empty() && ent[0] == '#') {
			char *end = nullptr;
			unsigned long cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
			                       ? strtoul(ent.c_str() + 2, &end, 16)
			                       : strtoul(ent.c_str() + 1, &end, 10);
			if (end && *end == 0 && cp > 0 && cp < 0x110000) {
				if (cp < 0x80) rep += (char)cp;
				else if (cp < 0x800) { rep += (char)(0xC0 | (cp >> 6)); rep += (char)(0x80 | (cp & 0x3F)); }
				else if (cp < 0x10000) { rep += (char)(0xE0 | (cp >> 12)); rep += (char)(0x80 | ((cp >> 6) & 0x3F)); rep += (char)(0x80 | (cp & 0x3F)); }
				else { rep += (char)(0xF0 | (cp >> 18)); rep += (char)(0x80 | ((cp >> 12) & 0x3F)); rep += (char)(0x80 | ((cp >> 6) & 0x3F)); rep += (char)(0x80 | (cp & 0x3F)); }
			} else ok = false;
		} else ok = false;
		if (ok) { out += rep; i = semi; }
		else out += in[i];
	}
	return out;
}

// ======================================================================
//  LRC handling
// ======================================================================

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// "mm:ss", "mm:ss.xx", "m:ss.xxx", "mm:ss:xx" -> seconds
static bool parse_time(const string &in, double &secs) {
	size_t i = 0, n = in.size();
	int mm = 0, ss = 0, nd = 0;
	while (i < n && is_digit(in[i]) && nd < 4) { mm = mm * 10 + (in[i] - '0'); i++; nd++; }
	if (nd == 0 || nd > 3) return false;
	if (i >= n || in[i] != ':') return false;
	i++;
	nd = 0;
	while (i < n && is_digit(in[i]) && nd < 3) { ss = ss * 10 + (in[i] - '0'); i++; nd++; }
	if (nd == 0 || nd > 2) return false;
	double frac = 0;
	if (i < n) {
		if (in[i] != '.' && in[i] != ':' && in[i] != ',') return false;
		i++;
		double scale = 0.1;
		nd = 0;
		while (i < n && is_digit(in[i])) { frac += (in[i] - '0') * scale; scale /= 10; i++; nd++; }
		if (nd == 0 || i != n) return false;
	}
	secs = mm * 60 + ss + frac;
	return true;
}

static bool is_meta_tag(const string &inner) {   // [ti:..] [ar:..] [offset:..] ...
	size_t i = 0;
	while (i < inner.size() && ((inner[i] >= 'a' && inner[i] <= 'z') || (inner[i] >= 'A' && inner[i] <= 'Z'))) i++;
	return i > 0 && i < inner.size() && inner[i] == ':';
}

static string strip_inline_word_times(const string &t) {   // enhanced LRC: <00:12.34>word
	string out;
	for (size_t i = 0; i < t.size(); i++) {
		if (t[i] == '<') {
			size_t close = t.find('>', i);
			double dummy;
			if (close != string::npos && close - i < 14 && parse_time(t.substr(i + 1, close - i - 1), dummy)) {
				i = close;
				continue;
			}
		}
		out += t[i];
	}
	return out;
}

// Canonicalises to "[mm:ss.xx]text" lines sorted by time (metadata and untimed lines dropped).
// If no timestamps exist at all, returns the cleaned plain lines instead.
static string normalize_impl(const string &in, size_t &ts_lines, size_t &plain_lines, double *first, double *last) {
	struct Entry { double t; string text; };
	vector<Entry> timed;
	vector<string> plain;
	ts_lines = plain_lines = 0;

	size_t start = 0;
	bool first_line = true;
	while (start <= in.size()) {
		size_t end = in.find('\n', start);
		if (end == string::npos) end = in.size();
		string line = in.substr(start, end - start);
		start = end + 1;
		if (first_line && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
		first_line = false;
		line = trim(line);
		if (line.empty()) { plain.push_back(""); continue; }

		vector<double> times;
		size_t pos = 0;
		bool meta = false;
		while (pos < line.size() && line[pos] == '[') {
			size_t close = line.find(']', pos);
			if (close == string::npos) break;
			string inner = line.substr(pos + 1, close - pos - 1);
			double t;
			if (parse_time(inner, t)) { times.push_back(t); pos = close + 1; }
			else {
				if (times.empty() && is_meta_tag(inner)) meta = true;
				break;
			}
		}
		if (meta) continue;
		string text = trim(strip_inline_word_times(line.substr(pos)));
		if (times.empty()) { plain.push_back(text); plain_lines++; }
		else {
			for (double t : times) timed.push_back({t, text});
			ts_lines += times.size();
		}
	}

	string out;
	if (!timed.empty()) {
		std::stable_sort(timed.begin(), timed.end(), [](const Entry &a, const Entry &b) { return a.t < b.t; });
		if (first) *first = timed.front().t;
		if (last) *last = timed.back().t;
		for (const Entry &e : timed) {
			long cs = std::lround(e.t * 100.0);
			char buf[32];
			snprintf(buf, sizeof buf, "[%02ld:%02ld.%02ld]", cs / 6000, (cs / 100) % 60, cs % 100);
			out += buf;
			out += e.text;
			out += '\n';
		}
	} else {
		bool prev_blank = true;   // collapse blank runs, trim leading blanks
		for (const string &l : plain) {
			if (l.empty()) { if (!prev_blank) out += '\n'; prev_blank = true; }
			else { out += l; out += '\n'; prev_blank = false; }
		}
		while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
	}
	return out;
}

string normalize_lrc(const string &text) {
	size_t a, b;
	return normalize_impl(text, a, b, nullptr, nullptr);
}

bool looks_synced(const string &text) {
	size_t ts, plain;
	normalize_impl(text, ts, plain, nullptr, nullptr);
	return ts >= 3 && ts >= plain;
}

// Converts already-normalized "[mm:ss.xx]text" lines to plain lines.
static string strip_norm(const string &norm) {
	string out;
	bool prev_blank = true;
	size_t start = 0;
	while (start < norm.size()) {
		size_t end = norm.find('\n', start);
		if (end == string::npos) end = norm.size();
		string line = norm.substr(start, end - start);
		start = end + 1;
		if (line.size() >= 10 && line[0] == '[' && line[3] == ':' && line[6] == '.' && line[9] == ']') line.erase(0, 10);
		if (line.empty()) { if (!prev_blank) out += '\n'; prev_blank = true; }
		else { out += line; out += '\n'; prev_blank = false; }
	}
	while (!out.empty() && out.back() == '\n') out.pop_back();
	return out;
}

string strip_timestamps(const string &text) { return strip_norm(normalize_lrc(text)); }

// ======================================================================
//  Candidate evaluation
// ======================================================================

static double dur_score(double want, double got, double &diff) {
	diff = 1e9;
	if (want <= 0 || got <= 0) return 0.5;
	diff = std::fabs(want - got);
	if (diff <= 2) return 1.0;
	if (diff <= 5) return 0.75;
	if (diff <= 10) return 0.4;
	if (diff <= 25) return 0.1;
	return -0.3;
}

// Validates the lyrics, downgrades bad synced data to plain text, and scores the candidate.
// Returns false if the candidate should be discarded.
static bool evaluate(const Query &q, Candidate &c) {
	size_t ts = 0, plain = 0;
	double first = 0, last = 0;
	string norm = normalize_impl(c.lyrics, ts, plain, &first, &last);

	bool synced = false;
	if (ts >= 4 && ts >= plain) {
		synced = true;
		if (q.duration > 0 && (last > q.duration + 8 || last < 0.3 * q.duration)) synced = false;
	}
	if (synced) c.lyrics = norm;
	else c.lyrics = ts > 0 ? strip_norm(norm) : norm;
	c.synced = synced;

	if (!synced) {
		string compact = trim(c.lyrics);
		if (compact.size() < 20) return false;   // empty / "instrumental" placeholders
	}

	double ts_sim = std::max(sim_norm(normalize_text(clean_title(q.title)), normalize_text(clean_title(c.title))),
	                         sim_norm(normalize_text(q.title_clean), normalize_text(clean_title(c.title))));
	bool known = !artist_unknown(q.artist);
	double as = known ? artist_sim(q.artist_clean.empty() ? q.artist : q.artist_clean, c.artist) : 0.6;
	double diff;
	double ds = dur_score(q.duration, c.duration, diff);

	bool title_ok = ts_sim >= 0.6 || (diff <= 2 && as >= 0.7 && ts_sim >= 0.3);
	if (!title_ok) return false;
	if (known && as < 0.35) return false;

	double base = 0.5 * ts_sim + 0.3 * as + 0.2 * ds;
	if (base < 0.55) return false;

	c.score = base + (synced ? 0.6 : 0.0);
	if (synced && q.duration > 0) c.score += 0.05 * std::min(1.0, last / q.duration);
	c.score -= 0.001 * c.prio;
	c.confident = synced && ts_sim >= 0.85 && as >= 0.7 &&
	                ((q.duration > 0 && c.duration > 0 && diff <= 3) || q.duration <= 0);
	return true;
}

// ======================================================================
//  HTTP + providers
// ======================================================================

static std::atomic<int> g_http_ok{0};

static string http(const string &url, const vector<string> &headers = {}, const string &post = "") {
	static std::once_flag once;
	std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
	curl_slist *sl = nullptr;
	sl = curl_slist_append(sl, "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
	for (const string &h : headers) sl = curl_slist_append(sl, h.c_str());
	string body = text_downloader(sl, url, post);
	if (!body.empty()) g_http_ok++;
	return body;
}

struct Meta {
	string id, artist, title, album;
	double duration = 0;
};

// Keep the few search hits that look most like the query (avoids downloading junk).
static vector<Meta> top_metas(vector<Meta> metas, const Query &q, size_t n) {
	bool known = !artist_unknown(q.artist);
	vector<std::pair<double, Meta>> scored;
	for (Meta &m : metas) {
		double t = std::max(similarity(clean_title(m.title), q.title_clean), similarity(clean_title(m.title), clean_title(q.title)));
		double a = known ? artist_sim(q.artist_clean, m.artist) : 0.6;
		double d = 0.5, diff;
		if (q.duration > 0 && m.duration > 0) { d = dur_score(q.duration, m.duration, diff); }
		double s = 0.5 * t + 0.3 * a + 0.2 * d;
		if (t >= 0.5 && (!known || a >= 0.3)) scored.push_back({s, std::move(m)});
	}
	std::stable_sort(scored.begin(), scored.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
	vector<Meta> out;
	for (size_t i = 0; i < scored.size() && i < n; i++) out.push_back(std::move(scored[i].second));
	return out;
}

static Candidate make_candidate(const char *provider, const Meta &m, string lyrics) {
	Candidate c;
	c.provider = provider;
	c.artist = m.artist;
	c.title = m.title;
	c.album = m.album;
	c.duration = m.duration;
	c.lyrics = std::move(lyrics);
	return c;
}

// ---- LRCLIB (exact lookup + search; both return lyrics inline) ----
static vector<Candidate> p_lrclib(const Query &q) {
	vector<Candidate> out;
	auto add = [&](const mj::Value &v) {
		if (v.type != mj::Value::Obj || v["instrumental"].truthy()) return;
		Meta m;
		m.title = v["trackName"].str();
		m.artist = v["artistName"].str();
		m.album = v["albumName"].str();
		m.duration = v["duration"].num();
		string syn = v["syncedLyrics"].str(), pl = v["plainLyrics"].str();
		if (!syn.empty()) out.push_back(make_candidate("LRCLIB", m, syn));
		if (!pl.empty()) out.push_back(make_candidate("LRCLIB", m, pl));
	};

	if (q.duration > 0 && !q.title.empty() && !artist_unknown(q.artist)) {
		string url = "https://lrclib.net/api/get?artist_name=" + urlencode(q.artist) + "&track_name=" + urlencode(q.title);
		if (!q.album.empty()) url += "&album_name=" + urlencode(q.album);
		url += "&duration=" + std::to_string((long)std::lround(q.duration));
		add(mj::parse(http(url)));
	}

	string url = "https://lrclib.net/api/search?track_name=" + urlencode(q.title_clean);
	if (!artist_unknown(q.artist)) url += "&artist_name=" + urlencode(q.artist_clean);
	mj::Value arr = mj::parse(http(url));
	if (arr.size() == 0 && !artist_unknown(q.artist))
		arr = mj::parse(http("https://lrclib.net/api/search?q=" + urlencode(q.title_clean + " " + q.artist_clean)));
	for (size_t i = 0; i < arr.size() && i < 10; i++) add(arr[i]);
	return out;
}

// ---- Providers ported from kopuz ----

// Apple Music lyrics through lyrics.paxsenix.org; the track is found with the iTunes search API.
static vector<Candidate> p_paxsenix_apple(const Query &q) {
	vector<Candidate> out;
	vector<Meta> metas;
	for (const ProviderHit &h : paxsenix_apple_search(q.title_clean, q.artist_clean)) {
		Meta m;
		m.id = h.id; m.title = h.title; m.artist = h.artist; m.album = h.album; m.duration = h.duration;
		metas.push_back(m);
	}
	for (const Meta &m : top_metas(metas, q, 2)) {
		parsed_lyrics pl = paxsenix_apple_lyrics_downloader(m.id);
		if (!pl.lyrics.empty()) out.push_back(make_candidate("Apple Music", m, pl.lyrics));
	}
	return out;
}

// YouTube lyrics through lyrics.paxsenix.org. Video titles are noisy, so the match uses kopuz's
// word-overlap score (>= 55) plus a +-12 s duration window instead of the title similarity.
static vector<Candidate> p_paxsenix_youtube(const Query &q) {
	vector<Candidate> out;
	string query = q.title + (artist_unknown(q.artist) ? "" : " " + q.artist);
	double best_score = -1;
	vector<ProviderHit> hits = paxsenix_youtube_search(q.title_clean, q.artist_clean);
	std::string best_id;
	double best_dur = 0;
	for (const ProviderHit &h : hits) {
		double text = srcutil::match_score(h.title + " " + h.artist, query);
		if (text < 55) continue;
		double ds = 0;
		if (q.duration > 0 && h.duration > 0) {
			double d = std::fabs(h.duration - q.duration);
			if (d > 12) continue;
			ds = 12 - d;
		}
		if (text + ds > best_score) { best_score = text + ds; best_id = h.id; best_dur = h.duration; }
	}
	if (best_id.empty()) return out;
	parsed_lyrics pl = paxsenix_youtube_lyrics_downloader(best_id);
	if (pl.lyrics.empty()) return out;
	Meta m;
	m.title = q.title;
	m.artist = q.artist;
	m.duration = best_dur;
	out.push_back(make_candidate("YouTube", m, pl.lyrics));
	return out;
}

// Musixmatch richsync (opt-in, like in kopuz).
static vector<Candidate> p_musixmatch(const Query &q) {
	vector<Candidate> out;
	if (!q.musixmatch) return out;
	string query = q.title + (artist_unknown(q.artist) ? "" : " " + q.artist);
	vector<Meta> metas;
	for (const ProviderHit &h : musixmatch_search(q.title_clean, q.artist_clean)) {
		if (srcutil::match_score(h.title + " " + h.artist, query) < 65) continue;   // kopuz threshold
		Meta m;
		m.id = h.id; m.title = h.title; m.artist = h.artist; m.album = h.album; m.duration = h.duration;
		metas.push_back(m);
	}
	for (const Meta &m : top_metas(metas, q, 1)) {
		parsed_lyrics pl = musixmatch_lyrics_downloader(m.id);
		if (!pl.lyrics.empty()) out.push_back(make_candidate("Musixmatch", m, pl.lyrics));
	}
	return out;
}

// ---- NetEase Cloud Music ----
static vector<Candidate> p_netease(const Query &q) {
	vector<Candidate> out;
	vector<string> hdr = {"Referer: https://music.163.com/"};
	string kw = q.title_clean + (artist_unknown(q.artist) ? "" : " " + q.artist_clean);
	mj::Value root = mj::parse(http("https://music.163.com/api/search/get?type=1&offset=0&sub=false&limit=6&s=" + urlencode(kw), hdr));
	const mj::Value &songs = root["result"]["songs"];
	vector<Meta> metas;
	for (size_t i = 0; i < songs.size() && i < 8; i++) {
		Meta m;
		m.id = songs[i]["id"].str();
		m.title = songs[i]["name"].str();
		const mj::Value &ar = songs[i]["artists"];
		for (size_t k = 0; k < ar.size(); k++) m.artist += (k ? ", " : "") + ar[k]["name"].str();
		m.album = songs[i]["album"]["name"].str();
		m.duration = songs[i]["duration"].num() / 1000.0;
		if (!m.id.empty()) metas.push_back(m);
	}
	for (const Meta &m : top_metas(metas, q, 2)) {
		if (m.id.find_first_not_of("0123456789") != string::npos) continue;
		string body = http("https://music.163.com/api/song/lyric?_nmclfl=1", hdr, "id=" + m.id + "&tv=-1&lv=-1&rv=-1&kv=-1");
		string lrc = mj::parse(body)["lrc"]["lyric"].str();
		if (!lrc.empty()) out.push_back(make_candidate("NetEase", m, lrc));
	}
	return out;
}

// ---- QQ Music ----
static vector<Candidate> p_qq(const Query &q) {
	vector<Candidate> out;
	vector<string> hdr = {"Referer: https://y.qq.com/"};
	string kw = q.title_clean + (artist_unknown(q.artist) ? "" : " " + q.artist_clean);
	mj::Value root = mj::parse(http(
	    "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?ct=24&qqmusic_ver=1298&new_json=1&remoteplace=txt.yqq.center"
	    "&t=0&aggr=1&cr=1&catZhida=1&lossless=0&flag_qc=0&p=1&n=8&format=json&inCharset=utf8&outCharset=utf-8"
	    "&notice=0&platform=yqq.json&needNewCode=0&w=" + urlencode(kw), hdr));
	const mj::Value &list = root["data"]["song"]["list"];
	vector<Meta> metas;
	for (size_t i = 0; i < list.size() && i < 8; i++) {
		const mj::Value &s = list[i];
		Meta m;
		m.id = s["mid"].str(s["songmid"].str());
		m.title = s["name"].str(s["songname"].str(s["title"].str()));
		const mj::Value &sg = s["singer"];
		for (size_t k = 0; k < sg.size(); k++) m.artist += (k ? ", " : "") + sg[k]["name"].str();
		m.album = s["album"]["name"].str(s["albumname"].str());
		m.duration = s["interval"].num();
		if (!m.id.empty()) metas.push_back(m);
	}
	for (const Meta &m : top_metas(metas, q, 2)) {
		string body = http("https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?g_tk=5381&format=json&inCharset=utf8"
		                   "&outCharset=utf-8&nobase64=0&songmid=" + urlencode(m.id), hdr);
		string lrc = html_unescape(base64_decode(mj::parse(body)["lyric"].str()));
		if (!lrc.empty()) out.push_back(make_candidate("QQ Music", m, lrc));
	}
	return out;
}

// ---- Kugou ----
static vector<Candidate> p_kugou(const Query &q) {
	vector<Candidate> out;
	string kw = q.title_clean + (artist_unknown(q.artist) ? "" : " " + q.artist_clean);
	mj::Value root = mj::parse(http("http://mobilecdn.kugou.com/api/v3/search/song?format=json&showtype=1&page=1&pagesize=8&keyword=" + urlencode(kw)));
	const mj::Value &info = root["data"]["info"];
	vector<Meta> metas;
	for (size_t i = 0; i < info.size() && i < 8; i++) {
		Meta m;
		m.id = info[i]["hash"].str();
		m.title = info[i]["songname"].str();
		m.artist = info[i]["singername"].str();
		m.album = info[i]["album_name"].str();
		m.duration = info[i]["duration"].num();
		if (!m.id.empty()) metas.push_back(m);
	}
	for (const Meta &m : top_metas(metas, q, 2)) {
		double d = m.duration > 0 ? m.duration : q.duration;
		mj::Value s = mj::parse(http("http://lyrics.kugou.com/search?ver=1&man=yes&client=pc&keyword=" + urlencode(m.artist + " - " + m.title) +
		                             "&duration=" + std::to_string((long)(d * 1000)) + "&hash=" + urlencode(m.id)));
		const mj::Value &cand = s["candidates"][(size_t)0];
		string id = cand["id"].str(), key = cand["accesskey"].str();
		if (id.empty() || key.empty()) continue;
		mj::Value dl = mj::parse(http("http://lyrics.kugou.com/download?ver=1&client=pc&fmt=lrc&charset=utf8&id=" + urlencode(id) + "&accesskey=" + urlencode(key)));
		string lrc = base64_decode(dl["content"].str());
		if (!lrc.empty()) out.push_back(make_candidate("Kugou", m, lrc));
	}
	return out;
}

// ---- lyrics.ovh (plain text only) ----
static vector<Candidate> p_ovh(const Query &q) {
	vector<Candidate> out;
	if (artist_unknown(q.artist) || q.title_clean.empty()) return out;
	mj::Value v = mj::parse(http("https://api.lyrics.ovh/v1/" + urlencode(q.artist_clean) + "/" + urlencode(q.title_clean)));
	string text = v["lyrics"].str();
	if (text.empty()) return out;
	Meta m;
	m.artist = q.artist_clean;
	m.title = q.title_clean;
	out.push_back(make_candidate("lyrics.ovh", m, text));
	return out;
}

#ifndef LYRICFINDER_NO_LEGACY
// ---- Existing HTML-scraping sources; results arrive as groups of [artist, title, album, url] ----
static vector<Candidate> from_legacy(const char *name, const vector<string> &songs,
                                     std::function<parsed_lyrics(string)> download, const Query &q) {
	vector<Candidate> out;
	vector<Meta> metas;
	for (size_t i = 0; i + 3 < songs.size(); i += 4) {
		Meta m;
		m.artist = songs[i];
		m.title = songs[i + 1];
		m.album = songs[i + 2];
		m.id = songs[i + 3];
		metas.push_back(m);
	}
	for (const Meta &m : top_metas(metas, q, 2)) {
		parsed_lyrics pl = download(m.id);
		if (!pl.lyrics.empty()) out.push_back(make_candidate(name, m, pl.lyrics));
	}
	return out;
}

static vector<Candidate> p_megalobiz(const Query &q) {
	string t = specialforplus(q.title_clean.c_str()), a = specialforplus(q.artist_clean.c_str());
	return from_legacy("Megalobiz", megalobiz_get_songs(t, a), [](string u) { return megalobiz_lyrics_downloader(u); }, q);
}

static vector<Candidate> p_rclyricsband(const Query &q) {
	string t = specialforplus(q.title_clean.c_str()), a = specialforplus(q.artist_clean.c_str());
	return from_legacy("RCLyricsBand", rclyricsband_get_songs(t, a), [](string u) { return rclyricsband_lyrics_downloader(u); }, q);
}
#endif

struct Provider {
	const char *name;
	vector<Candidate> (*fn)(const Query &);
};

static const vector<Provider> &providers() {
	// Order = tie-break priority (earlier wins on equal score).
	static const vector<Provider> v = {
	    {"LRCLIB", p_lrclib},
	    {"Apple Music", p_paxsenix_apple},
	    {"YouTube", p_paxsenix_youtube},
	    {"Musixmatch", p_musixmatch},
	    {"NetEase", p_netease},
	    {"QQ Music", p_qq},
	    {"Kugou", p_kugou},
#ifndef LYRICFINDER_NO_LEGACY
	    {"Megalobiz", p_megalobiz},
#endif
	    {"lyrics.ovh", p_ovh},
#ifndef LYRICFINDER_NO_LEGACY
	    {"RCLyricsBand", p_rclyricsband},
#endif
	};
	return v;
}

vector<string> provider_names() {
	vector<string> n;
	for (const Provider &p : providers()) n.push_back(p.name);
	return n;
}

// ======================================================================
//  Orchestrator
// ======================================================================

struct Shared {
	std::mutex m;
	std::condition_variable cv;
	vector<Candidate> cands;
	int pending = 0;
	bool confident = false;
};

static const Candidate *best_of(const vector<Candidate> &v) {
	const Candidate *best = nullptr;
	for (const Candidate &c : v)
		if (!best || c.score > best->score) best = &c;
	return best;
}

// Cancellation (or a confident hit) only stops the wait below; provider threads are detached and
// exception-safe, so in-flight requests run to completion in the background by design.
Result find_best(const Query &in, Candidate &out, const Cancel &cancelled, int timeout_ms) {
	Query q = in;
	q.title_clean = clean_title(q.title);
	q.artist_clean = artist_unknown(q.artist) ? "" : clean_title(q.artist);
	if (q.title_clean.empty()) return Result::NotFound;

	int http_before = g_http_ok.load();
	auto sh = std::make_shared<Shared>();
	const vector<Provider> &ps = providers();
	sh->pending = (int)ps.size();

	for (size_t i = 0; i < ps.size(); i++) {
		auto job = [sh, q, i] {
			vector<Candidate> good;
			try {
				vector<Candidate> got = providers()[i].fn(q);
				for (Candidate &c : got) {
					c.prio = (int)i;
					if (c.provider.empty()) c.provider = providers()[i].name;
					if (evaluate(q, c)) good.push_back(std::move(c));
				}
			} catch (...) {
			}
			std::lock_guard<std::mutex> lk(sh->m);
			for (Candidate &c : good) {
				if (c.confident) sh->confident = true;
				sh->cands.push_back(std::move(c));
			}
			sh->pending--;
			sh->cv.notify_all();
		};
		try {
			std::thread(job).detach();
		} catch (...) {
			std::lock_guard<std::mutex> lk(sh->m);
			sh->pending--;
		}
	}

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	Candidate winner;
	bool have = false, cancel = false;
	{
		std::unique_lock<std::mutex> lk(sh->m);
		while (true) {
			if (sh->pending <= 0 || sh->confident) break;
			if (cancelled && cancelled()) { cancel = true; break; }
			if (std::chrono::steady_clock::now() >= deadline) break;
			sh->cv.wait_for(lk, std::chrono::milliseconds(100));
		}
		if (!cancel) {
			const Candidate *b = best_of(sh->cands);
			if (b) { winner = *b; have = true; }
		}
	}

	if (cancel) return Result::Cancelled;
	if (have) { out = std::move(winner); return Result::Found; }
	return g_http_ok.load() == http_before ? Result::NetworkError : Result::NotFound;
}

}  // namespace lyricfinder
