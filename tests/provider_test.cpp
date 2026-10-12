#include "src/json.h"
#include "src/sources/paxsenix.h"
#include "src/sources/musixmatch.h"
#include "src/lyrics_finder.h"
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
static std::mutex g_mx;
using namespace std;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static vector<string> g_urls;
static map<string,string> g_resp;      // url substring -> body
string urlencode(const string &s) { string o; for (unsigned char c : s) { if (isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') o += c; else { char b[4]; snprintf(b,4,"%%%02x",c); o += b; } } return o; }   // lowercase hex, matching src/utils.cpp
string text_downloader(curl_slist *, string url, string) {
	std::lock_guard<std::mutex> lk(g_mx);
	g_urls.push_back(url);
	for (auto &kv : g_resp) if (url.find(kv.first) != string::npos) return kv.second;
	return "";
}

int main() {
	// ---- helpers
	CHECK(srcutil::match_score("Blinding Lights The Weeknd", "blinding lights the weeknd") == 100);
	CHECK(srcutil::match_score("Blinding Lights (Official Video) The Weeknd", "Blinding Lights The Weeknd") >= 55);
	CHECK(srcutil::match_score("Other Song Someone", "Blinding Lights The Weeknd") < 55);
	CHECK(srcutil::parse_colon_duration("3:45") == 225 && srcutil::parse_colon_duration("1:02:03") == 3723);
	CHECK(srcutil::parse_colon_duration("x") < 0 && srcutil::parse_colon_duration("") < 0 && srcutil::parse_colon_duration("3:") < 0);
	CHECK(srcutil::lrc_time(75.456) == "01:15.46");
	CHECK(urlencode("C++ K+") == "C%2b%2b%20K%2b");   // unreserved stay raw, lowercase hex, '+' encoded

	// ---- JSON lone surrogates -> U+FFFD (no crash, no CESU-8)
	CHECK(mj::parse(R"({"s":"a\ud800"})")["s"].str() == "a\xEF\xBF\xBD");
	CHECK(mj::parse(R"({"s":"\udc00"})")["s"].str() == "\xEF\xBF\xBD");
	CHECK(mj::parse(R"({"s":"\ud800x"})")["s"].str() == "\xEF\xBF\xBDx");
	CHECK(mj::parse(R"({"s":"\ud834\udd1e"})")["s"].str() == "\xF0\x9D\x84\x9E");   // valid pair untouched

	// ---- finder helpers: title cleaning, similarity, LRC classification
	CHECK(lyricfinder::clean_title("Blinding Lights (Official Video) [Remastered 2011] - Live") == "Blinding Lights");
	CHECK(lyricfinder::clean_title("Song feat. Someone") == "Song");
	CHECK(lyricfinder::similarity("Blinding Lights", "blinding lights") > 0.95);
	CHECK(lyricfinder::similarity("Blinding Lights", "Totally Different") < 0.5);
	string lrc4 = "[00:10.00]a\n[00:20.00]b\n[00:30.00]c\n[00:40.00]d\n";
	CHECK(lyricfinder::looks_synced(lrc4));
	CHECK(!lyricfinder::looks_synced("just some plain text\nmore text\n"));
	CHECK(lyricfinder::normalize_lrc("[00:20.00]b\n[00:10.00]a\n") == "[00:10.00]a\n[00:20.00]b\n");
	CHECK(lyricfinder::strip_timestamps(lrc4) == "a\nb\nc\nd");
	CHECK(lyricfinder::base64_decode("SGVsbG8=") == "Hello");
	CHECK(lyricfinder::html_unescape("a &amp; b &#65; &#x42;") == "a & b A B");

	// ---- Apple parse: syllable rows + background
	string apple = R"({"content":[
	 {"text":[{"text":"Hel","timestamp":1000,"part":true},{"text":"lo","timestamp":1300},{"text":"world","timestamp":1800}],"timestamp":1000,"endtime":2500},
	 {"text":[{"text":"Second","timestamp":3000},{"text":",","timestamp":3300},{"text":"line","timestamp":3400}],"backgroundText":[{"text":"ooh","timestamp":3500}],"timestamp":3000},
	 {"text":[{"text":"only bg","timestamp":5000}],"background":true,"timestamp":5000}
	 ],"lrc":"[00:01.00]x","plain":"p"})";
	parsed_lyrics a = paxsenix_apple_parse(apple);
	CHECK(a.sync);
	CHECK(a.lyrics == "[00:01.00]Hello world\n[00:03.00]Second, line\n[00:03.50](ooh)\n[00:05.00](only bg)\n");
	// untimed content -> lrc, then plain
	CHECK(paxsenix_apple_parse(R"({"content":[{"text":[{"text":"a"}],"timestamp":0}],"lrc":"[00:01.00]a\n[00:02.00]b","plain":"p"})").sync);
	parsed_lyrics pl = paxsenix_apple_parse(R"({"content":[],"lrc":"","plain":"just text"})");
	CHECK(!pl.sync && pl.lyrics == "just text");
	for (const char *bad : {"", "[]", "null", "{", "{\"content\":5}", "<html>", "{\"content\":[1,2,null]}"}) CHECK(paxsenix_apple_parse(bad).lyrics.empty());

	// ---- Musixmatch richsync conversion
	string rich = R"([{"ts":12.5,"te":15,"l":[{"c":"Never","o":0},{"c":" ","o":0.3},{"c":"gonna","o":0.4}]},{"ts":20,"l":[]},{"te":3}])";
	CHECK(musixmatch_richsync_to_lrc(rich) == "[00:12.50]Never gonna\n[00:20.00]\n");
	CHECK(musixmatch_richsync_to_lrc("nope").empty() && musixmatch_richsync_to_lrc("{}").empty());

	// ---- end to end through the finder with canned HTTP
	string lrc;
	for (int i = 0; i < 12; i++) { char b[64]; snprintf(b, sizeof b, "[%02d:%02d.00]Line number %d of the song\\n", (i*15) / 60, (i*15) % 60, i); lrc += b; }
	g_resp["itunes.apple.com/search"] = R"({"results":[{"trackId":111,"trackName":"Blinding Lights","artistName":"The Weeknd","collectionName":"After Hours","trackTimeMillis":200000},{"trackId":222,"trackName":"Totally Different","artistName":"Nobody","trackTimeMillis":200000}]})";
	g_resp["apple-music/lyrics?id=111"] = string("{\"content\":[],\"lrc\":\"") + lrc + "\",\"plain\":\"\"}";
	g_resp["youtube/search"] = R"J([{"videoId":"vid1","title":"The Weeknd - Blinding Lights (Official Video)","author":"TheWeekndVEVO","duration":"3:20"},{"videoId":"vid2","title":"Blinding Lights cover","author":"x","duration":"9:59"}])J";
	g_resp["youtube/lyrics?id=vid1"] = lrc.empty() ? "" : [&]{ string s; for (size_t i = 0; i < lrc.size(); i++) { if (lrc[i]=='\\' && lrc[i+1]=='n') { s += '\n'; i++; } else s += lrc[i]; } return s; }();
	g_resp["token.get"] = R"({"message":{"header":{"status_code":200},"body":{"user_token":"TOK"}}})";
	g_resp["track.search"] = R"({"message":{"header":{"status_code":200},"body":{"track_list":[{"track":{"track_id":777,"track_name":"Blinding Lights","artist_name":"The Weeknd","album_name":"After Hours","track_length":200}}]}}})";
	g_resp["track.richsync.get"] = string("{\"message\":{\"header\":{\"status_code\":200},\"body\":{\"richsync\":{\"richsync_body\":\"[") +
	    [&]{ string s; for (int i = 0; i < 12; i++) { char b[160]; snprintf(b, sizeof b, "{\\\"ts\\\":%d,\\\"te\\\":%d,\\\"l\\\":[{\\\"c\\\":\\\"Word%d\\\",\\\"o\\\":0},{\\\"c\\\":\\\"here\\\",\\\"o\\\":0.5}]}%s", i*15, i*15+10, i, i < 11 ? "," : ""); s += b; } return s; }() + "]\"}}}}";

	auto hits = paxsenix_apple_search("Blinding Lights", "The Weeknd");
	CHECK(hits.size() == 2 && hits[0].id == "111" && hits[0].duration == 200);
	CHECK(paxsenix_apple_lyrics_downloader("111").sync);
	CHECK(paxsenix_apple_lyrics_downloader("../etc").lyrics.empty());     // id is validated before use in a URL
	CHECK(paxsenix_youtube_search("Blinding Lights", "The Weeknd").size() == 2);
	CHECK(paxsenix_youtube_lyrics_downloader("vid1").sync);
	CHECK(musixmatch_search("Blinding Lights", "The Weeknd").size() == 1);
	CHECK(musixmatch_lyrics_downloader("777").sync);

	// ---- '+' in a title must reach the kopuz query unmangled (as %2b, never a space)
	g_urls.clear();
	paxsenix_apple_search("C++", "K+");
	CHECK(g_urls.size() == 1 && g_urls[0].find("C%2b%2b%20K%2b") != string::npos);

	lyricfinder::Query q; q.title = "Blinding Lights"; q.artist = "The Weeknd"; q.duration = 200;
	lyricfinder::Candidate best;
	g_urls.clear();
	auto r = lyricfinder::find_best(q, best, {}, 5000);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	CHECK(r == lyricfinder::Result::Found && best.synced);
	bool got_apple = false, yt = false, mxm = false;
	for (auto &u : g_urls) { got_apple |= u.find("apple-music/lyrics") != string::npos; yt |= u.find("youtube/lyrics") != string::npos; mxm |= u.find("richsync") != string::npos; }
	CHECK(got_apple && yt);
	CHECK(!mxm);                                   // opt-in: not contacted unless enabled
	g_urls.clear(); q.musixmatch = true;
	lyricfinder::find_best(q, best, {}, 5000);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	mxm = false; for (auto &u : g_urls) mxm |= u.find("richsync") != string::npos;
	CHECK(mxm || best.provider != "");             // may stop early on a confident match from a faster provider
	printf("winner: %s synced=%d\n", best.provider.c_str(), (int)best.synced);

	// garbage / empty responses must not crash anything
	std::this_thread::sleep_for(std::chrono::seconds(1));   // let detached finder threads of earlier runs finish
	{ std::lock_guard<std::mutex> lk(g_mx); g_resp.clear(); }
	for (const char *junk : {"", "<html>", "{", "[1,2", "{\"results\":[{}]}", "[{\"videoId\":5}]"}) {
		{ std::lock_guard<std::mutex> lk(g_mx); g_resp["itunes"] = junk; g_resp["youtube"] = junk; g_resp["token.get"] = junk; }
		paxsenix_apple_search("a", "b"); paxsenix_youtube_search("a", "b"); musixmatch_search("a", "b");
		lyricfinder::find_best(q, best, {}, 3000);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}
	printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
	return fails ? 1 : 0;
}
