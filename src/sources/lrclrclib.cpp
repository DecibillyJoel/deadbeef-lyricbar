#include "lrclrclib.h"
#include "../json.h"

#include <string>
#include <vector>

#include <curl/curl.h>

using namespace std;

// Headers for the LRCLIB API. text_downloader() takes ownership and frees the list.
static struct curl_slist *lrclib_headers() {
	struct curl_slist *h = NULL;
	h = curl_slist_append(h, "app-platform: WebPlayer");
	h = curl_slist_append(h, "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
	return h;
}

// Returns [artist, track, album, id] groups, which is what the search window expects.
vector<string> lrclib_get_songs(string song, string artist, string album, int duration) {
	vector<string> artists_and_songs;
	string url = "https://lrclib.net/api/search?artist_name=" + urlencode(artist) + "&track_name=" + urlencode(song);
	if (album != "") url += "&album_name=" + urlencode(album);
	if (duration != 0) url += "&duration=" + to_string(duration);

	mj::Value results = mj::parse(text_downloader(lrclib_headers(), url, ""));
	for (size_t i = 0; i < results.size(); i++) {
		const mj::Value &r = results[i];
		string id = r["id"].str();
		if (id.empty()) continue;
		artists_and_songs.push_back(r["artistName"].str());
		artists_and_songs.push_back(r["trackName"].str());
		artists_and_songs.push_back(r["albumName"].str());
		artists_and_songs.push_back(id);
	}
	return artists_and_songs;
}

struct parsed_lyrics lrclib_lyrics_downloader(string trackid) {
	mj::Value v = mj::parse(text_downloader(lrclib_headers(), "https://lrclib.net/api/get/" + trackid, ""));
	string synced_lyrics = v["syncedLyrics"].str();
	if (!synced_lyrics.empty()) return {synced_lyrics, true};
	return {v["plainLyrics"].str(), false};
}
// ------------------------------------- MAIN ------------------------------------------- 

struct parsed_lyrics lrclib(string song, string artist, string album) {

	vector<string> songs_list = lrclib_get_songs(song, artist, album, 0);

//	Download first result:
	struct parsed_lyrics string_lyrics = {"",false};
	if (songs_list.size() > 3){
		string_lyrics = lrclib_lyrics_downloader(songs_list[3]);
	}
	return string_lyrics;
}
