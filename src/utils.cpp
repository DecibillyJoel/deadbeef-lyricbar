#include "utils.h"
#include "debug.h"
#include "gettext.h"
#include "sources/lrcspotify.h"
#include "sources/lrclrclib.h"
#include "ui.h"
#include "gettext.h"

#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <memory>
#include <algorithm>
#include <map>
#include <chrono>
#include <cstring>
#include "lyrics_finder.h"


using namespace std;

//Global variables:

//static const char *home_cache = getenv("XDG_CACHE_HOME");
//static const string lyrics_dir = (home_cache ? string(home_cache) : string(getenv("HOME")) + "/.cache") + "/deadbeef/lyrics/";



int death_signal = 0;   // set from the GTK thread, read by listener threads (int reads/writes are atomicTarget-independent single-copy; see note below)
bool lyricstart     = false;
bool syncedlyrics   = false;
struct timespec ts  = {0, 50000000};


// For debug:
bool is_playing(DB_playItem_t *track) {
	DB_playItem_t *pl_track = deadbeef->streamer_get_playing_track_safe();
	if ((pl_track) && (pl_track != track)){
		deadbeef->pl_item_unref(pl_track);
		return false;
	}
	else if (!pl_track){
		return false;
	}
	else{
	deadbeef->pl_item_unref(pl_track);
	return pl_track == track;
	}
}

//---------------------------------------------------------------------
//************ Functions to make scroll on sync lyrics. ***************
//---------------------------------------------------------------------

// Sort lyrics by timestamp (stable, keeps text paired with its time).
static struct sync sort_lyrics(const vector<double> &arr, const vector<string> &text) {
	struct sync result;
	if (arr.empty() || text.size() != arr.size()) {
		result.position.push_back(0);
		result.synclyrics.push_back(_("Error with lyrics file or metadata"));
		return result;
	}
	vector<size_t> idx(arr.size());
	for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
	stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return arr[x] < arr[y]; });
	for (size_t i : idx) {
		result.position.push_back(arr[i]);
		result.synclyrics.push_back(text[i]);
	}
	return result;
}

static inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Parse LRC text: "[mm:ss.xx]text", several timestamps per line allowed.
struct sync lyric2vector(const string &lyrics) {
	vector<string> texts;
	vector<double> times;
	size_t start = 0;
	while (start <= lyrics.size()) {
		size_t end = lyrics.find('\n', start);
		if (end == string::npos) end = lyrics.size();
		string line = lyrics.substr(start, end - start);
		start = end + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();

		vector<double> line_times;
		size_t pos = 0;
		while (pos + 9 <= line.size() && line[pos] == '[' && is_digit(line[pos+1]) && is_digit(line[pos+2])
		       && line[pos+3] == ':' && is_digit(line[pos+4]) && is_digit(line[pos+5])
		       && line[pos+6] == '.' && is_digit(line[pos+7]) && is_digit(line[pos+8])) {
			line_times.push_back((line[pos+1]-'0')*600 + (line[pos+2]-'0')*60 + (line[pos+4]-'0')*10
			                     + (line[pos+5]-'0') + (line[pos+7]-'0')*0.1 + (line[pos+8]-'0')*0.01);
			pos += 9;
			while (pos < line.size() && line[pos] != ']') pos++;
			if (pos < line.size()) pos++;
		}
		if (line_times.empty()) continue;
		string text = line.substr(min(pos, line.size()));
		for (double t : line_times) {
			times.push_back(t);
			texts.push_back(text);
		}
	}
	struct sync goodlyrics = sort_lyrics(times, texts);

// For debug:
//	for (auto k = goodlyrics.position.begin(); k != goodlyrics.position.end(); ++k)
//    	cout << *k << " ";
//
//	for (auto i = goodlyrics.synclyrics.begin(); i != goodlyrics.synclyrics.end(); ++i)
//    	cout << *i << '\n';
	
	return goodlyrics;
}

// Everything the listener thread needs, immutable once built.
struct sync_state {
	struct sync lrc;
	vector<int> sizes;
};

static std::atomic<unsigned> listener_generation{0};

// Scroll and highlight the current line. Skips the GTK update when nothing visible changed.
static void write_synced(DB_playItem_t *it, const sync_state &st, int &previous_pad, string &last_key) {
	const struct sync &l = st.lrc;
	const vector<int> &sizes = st.sizes;
	float pos = deadbeef->streamer_get_playpos();
	int presentpos = 0, minimuntopad = 0, pad = 0;
	string past, present, future, padding;

	if (l.position.size() < 3 || l.synclyrics.size() != l.position.size())
		return;
	DB_output_t *out = deadbeef->get_output();
	if (out && out->state() == DDB_PLAYBACK_STATE_PAUSED)
		return;

	for (size_t i = 0; i + 2 < l.position.size(); i++) {
		if (l.position[i] < pos) {
			if (l.position[i+1] > pos) {
				present = " " + l.synclyrics[i] + " \n";
				presentpos = (int)i;
			}
		}
		else if (pos < l.position[i]) {
			future.append(l.synclyrics[i]);
			future.append("\n");
		}
	}

	if (sizes.size() >= 2) {
		int top = sizes[1];
		size_t nxt = (size_t)max(0, top) + 1;
		if (nxt < l.position.size() && l.position[nxt] > 0) {
			pad = int((l.position[nxt] - pos) / l.position[nxt] * sizes[0]);
			if (pad == previous_pad) return;
			padding.assign(max(0, pad), '\n');
			if (pad > 0) previous_pad = pad;
		}

		size_t sidx = (size_t)(presentpos - top + 5);
		if ((presentpos - top) > 0 && (size_t)(presentpos + 1) < l.position.size() && sidx < sizes.size()) {
			minimuntopad = presentpos - top;
			double span = l.position[presentpos+1] - l.position[presentpos];
			if (span > 0) {
				pad = int(((l.position[presentpos+1] - pos) / span) * (sizes[sidx] - 1));
				if (pad == previous_pad) return;
				padding.assign(max(0, pad), '\n');
				previous_pad = pad;
			}
		}
	}

	for (size_t i = (size_t)minimuntopad; i + 2 < l.position.size() && l.position[i+1] < pos; i++)
		past.append(l.synclyrics[i] + "\n");

	string key = to_string(past.size()) + "|" + present + "|" + to_string(padding.size());
	if (key == last_key) return;
	last_key = key;
	set_lyrics(it, past, present, future, padding);
}

// Realtime update loop. Ends when the track changes or a newer listener starts.
static void thread_listener(DB_playItem_t *track, shared_ptr<sync_state> st, unsigned gen) {
	int previous_pad = 0;
	string last_key;
	// NOTE: death_signal is a plain int read without a lock by listener threads. On all
// supported targets a 4-byte aligned int load/store is single-copy atomic, and the
// flag is only ever 0 or 1, so the worst case is one extra 50 ms loop iteration.
// listener_generation (above) is the real synchronisation primitive.
while (gen == listener_generation.load() && !death_signal && is_playing(track)) {
		nanosleep(&ts, NULL);
		try {
			write_synced(track, *st, previous_pad, last_key);
		} catch (const std::exception &e) {
			cerr << "lyricbar: " << e.what() << endl;
			break;
		}
	}
	deadbeef->pl_item_unref(track);
}

// Parse `lyrics`, measure line sizes on the UI thread, start the listener. Thread owns a ref on `track`.
void chopset_lyrics(DB_playItem_t *track, string lyrics) {
	auto st = make_shared<sync_state>();
	st->lrc = lyric2vector(lyrics);

	DB_playItem_t *it = deadbeef->streamer_get_playing_track_safe();
	if (!it) return;
	if (it != track) { deadbeef->pl_item_unref(it); return; }

	float length = deadbeef->pl_get_item_duration(it);
	st->lrc.position.push_back((double)length - 0.2);
	st->lrc.position.push_back((double)length);
	st->lrc.synclyrics.push_back("\n");
	st->lrc.synclyrics.push_back("\n");

	string prelyrics;
	for (size_t i = 0; i + 2 < st->lrc.synclyrics.size(); i++)
		prelyrics.append(st->lrc.synclyrics[i] + "\n");

	st->sizes = sizelines(track, prelyrics);

	unsigned gen = ++listener_generation;
	try {
		std::thread t(thread_listener, it, st, gen);
		t.detach();
	} catch (...) {
		deadbeef->pl_item_unref(it);
	}
}

//---------------------------------------------------------------------
//*********** /Functions to make scroll on sync lyrics. ***************
//---------------------------------------------------------------------


//---------------------------------------------------------------------
//************ Share functions of lyrics downloaders. *****************
//---------------------------------------------------------------------

//	Function to save lyrics to tag.
void save_meta_data(DB_playItem_t *playing_song, struct parsed_lyrics lyrics){

	deadbeef->pl_lock();
	if (lyrics.sync == true){
		if (deadbeef->pl_find_meta(playing_song, "SYLT"))
			deadbeef->pl_delete_meta(playing_song, "SYLT");
		if (deadbeef->pl_find_meta(playing_song, "LYRICS"))
			deadbeef->pl_delete_meta(playing_song, "LYRICS");
		deadbeef->pl_add_meta(playing_song, "LYRICS", lyrics.lyrics.c_str());
	}
	else{
		if (deadbeef->pl_find_meta(playing_song, "UNSYNCEDLYRICS"))
			deadbeef->pl_delete_meta(playing_song, "UNSYNCEDLYRICS");
		deadbeef->pl_add_meta(playing_song, "UNSYNCEDLYRICS", lyrics.lyrics.c_str());	
	}
	const char *dec = deadbeef->pl_find_meta_raw(playing_song, ":DECODER"); 
	char decoder_id[100] = {0};

	if (dec){
		strncpy(decoder_id, dec, sizeof(decoder_id) - 1);
		decoder_id[sizeof(decoder_id) - 1] = 0;
	}
	int match = playing_song && dec;
	deadbeef->pl_unlock();

	if (match){
		DB_decoder_t *dec = NULL;
		DB_decoder_t **decoders = deadbeef->plug_get_decoder_list();

		for (int i = 0; decoders && decoders[i]; i++){

			if (!strcmp(decoders[i]->plugin.id, decoder_id)){
				dec = decoders[i];

				if (dec->write_metadata) {
					dec->write_metadata(playing_song);
				}
				break;
			}
		}
	}
}

// Function to encode characters to URL hexadecimal.
std::string urlencode(const std::string &s) {
    static const char hexachars[]= "0123456789abcdef";
    std::string converted;
    for(int i=0, ix=s.length(); i<ix; i++) {
        const char& c = s[i];
        if ( (c >= 'A' &&  c <= 'Z') || (c >= 'a' &&  c <= 'z') || (c >= '0' &&  c <= '9') || (c=='-' || c=='_' || c=='.' || c=='~')) {
            converted = converted + c;
        }
        else {
            converted = converted + '%';
            converted = converted + hexachars[(c&0xF0)>>4];
            converted = converted + hexachars[(c&0x0F)];
        }
    }
    return converted;
}

// Function to remove special characters.
string specialforplus(const char* text) {
	string counter = string(text);
	for(unsigned i = 0; i < counter.size(); i++)
    {
        if( (counter[i] < 'A' ||  counter[i] > 'Z') &&  (counter[i] < 'a' ||  counter[i] > 'z') &&  (counter[i] < '0' ||  counter[i] > '9')) {  
            counter[i] = '+';
		}
    }
    return counter;
}
// Function to remove replace substrings.
std::string replace_string(string subject, const string& search, const string& replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != string::npos) {
		subject.replace(pos, search.length(), replace);
		pos += replace.length();
    }
    return subject;
}

size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Function to split strings.
vector<string> split(string s, string delimiter) {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    string token;
    vector<string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }
    res.push_back (s.substr (pos_start));
    return res;
}

// Function to parse webpages. Takes ownership of (and frees) `slist`.
string text_downloader(curl_slist *slist, string url, string post) {
	CURL *curl_handle;
	curl_handle = curl_easy_init();
	string readBuffer = "";
	if (curl_handle) {
		curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 15L);
		curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 8L);
		curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 2L);
		curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 0);
		curl_easy_setopt(curl_handle, CURLOPT_HEADER, 0);
		curl_easy_setopt(curl_handle, CURLOPT_ENCODING, NULL);
		curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, slist);
		if (post != ""){
			curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "POST");
		    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post.c_str());
		}
		else{
		    curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "GET");
		}
		curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteCallback);
		curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &readBuffer);
		CURLcode rc = curl_easy_perform(curl_handle);
		curl_easy_cleanup(curl_handle);
		if (rc != CURLE_OK) readBuffer.clear();
	}
	curl_slist_free_all(slist);
	return readBuffer;
}

//---------------------------------------------------------------------
//************ /Share functions of lyrics downloaders. ****************
//---------------------------------------------------------------------

//inline string cached_filename(string artist, string title) {
//	return 0;
//}

extern "C"
bool is_cached(const char *artist, const char *title) {
	return false;
}

extern "C"
void ensure_lyrics_path_exists() {
}

//---------------------------------------------------------------------
//****** Main: Try to get lyrics from Metadata,file,LrcLib. **********
//---------------------------------------------------------------------

struct parsed_lyrics get_lyrics_next_to_file(DB_playItem_t *track) {
	ifstream infile;
	string lyrics;
	string line;

	deadbeef->pl_lock();
	const char *track_location = deadbeef->pl_find_meta(track, ":URI");
	string trackstring = track_location ? track_location : "";
	deadbeef->pl_unlock();
	if (trackstring.empty() || trackstring.find("://") != string::npos)
		return {"", false};
	size_t lastindex = trackstring.find_last_of(".");
	trackstring = trackstring.substr(0, lastindex);
	infile.open(trackstring + ".lrc", ios_base::in);
	if (!infile.is_open()){
	    infile.clear();
	    infile.open(trackstring + ".txt", ios_base::in);
	}
	
	if(infile.is_open()){
		while (getline(infile, line)) {
		    lyrics = lyrics + line + "\n";
		}
	}
	return {lyrics, lyricfinder::looks_synced(lyrics)};
}

struct parsed_lyrics get_lyrics_from_metadata(DB_playItem_t *track) {
	deadbeef->pl_lock();
	const char *lyrics = deadbeef->pl_find_meta(track, "lyrics")
							?: deadbeef->pl_find_meta(track, "SYLT")
							?: deadbeef->pl_find_meta(track, "unsynced lyrics")
							?: deadbeef->pl_find_meta(track, "UNSYNCEDLYRICS");
	string string_lyrics = lyrics ? lyrics : "";
	deadbeef->pl_unlock();
	// Classify by content, not tag name: a plain LYRICS tag must not read as synced.
	bool is_sync = !string_lyrics.empty() && lyricfinder::looks_synced(string_lyrics);
	syncedlyrics = is_sync;
	return {string_lyrics, is_sync};
}


void save_next_to_file(DB_playItem_t *track, struct parsed_lyrics lyrics) {
	deadbeef->pl_lock();
	const char *track_location = deadbeef->pl_find_meta(track, ":URI");
	string trackstring = track_location ? track_location : "";
	deadbeef->pl_unlock();
	if (trackstring.empty() || trackstring.find("://") != string::npos)
		return;
	ofstream outfile;
	size_t lastindex = trackstring.find_last_of(".");
	trackstring = trackstring.substr(0, lastindex);
	
	if (lyrics.sync == true) {
		try {
		outfile.open(trackstring + ".lrc", ios_base::trunc);//ios_base::app
		}
		catch (...) {
			std::cout << "Unable to write file\n";
		}
	}
	else {
		try {
		outfile.open(trackstring + ".txt", ios_base::trunc);//ios_base::app
		}
		catch (...)	{
			std::cout << "Unable to write file\n";
		}
	}
	if (!outfile.is_open()) {
		cerr << "lyricbar: unable to write lyrics file" << endl;
		return;
	}
	outfile << lyrics.lyrics << endl;
	outfile.close();
}

//Sets track info if no lyrics founded.
void set_info(DB_playItem_t *track) {
	string info = "";
	deadbeef->pl_lock();
    int count  = deadbeef->pl_find_meta_int(track, "PLAY_COUNTER", -1);
	if (count == -1){
		count  = deadbeef->pl_find_meta_int(track, "play_count", -1);
	}
    const char *las  = deadbeef->pl_find_meta (track, "LAST_PLAYED");
    const char *firs  = deadbeef->pl_find_meta (track, "FIRST_PLAYED");
    const char *publisher = deadbeef->pl_find_meta (track, "PUBLISHER");
    const char *media = deadbeef->pl_find_meta (track, "MEDIA TYPE");
    const char *catalog_number = deadbeef->pl_find_meta (track, "CATALOGNUMBER");
    const char *release_country = deadbeef->pl_find_meta (track, "MUSICBRAINZ ALBUM RELEASE COUNTRY");
    const char *original_date = deadbeef->pl_find_meta (track, "ORIGINAL_RELEASE_TIME");
    const char *genre = deadbeef->pl_find_meta (track, "GENRE");
    
    
    
    //INVOLVED_PEOPLE_LIST and GENRE are a little more tricky to retrieve.
    DB_metaInfo_t *involved_people_meta = deadbeef->pl_meta_for_key (track, "InvolvedPeople");
	int playcount = deadbeef->playqueue_get_count();
	int bitrate = deadbeef->streamer_get_apx_bitrate();

    std::string involved_buf;
    const char *involved_people = NULL;

    if (involved_people_meta != NULL && involved_people_meta->value) {
        size_t value_size = min<size_t>(involved_people_meta->valuesize, 100000);
        involved_buf.assign(involved_people_meta->value, value_size);
        for (char &c : involved_buf) if (c == 0) c = '\n';
        involved_people = involved_buf.c_str();
    }
    // Copy the strings still needed below, then release the lock.
    string s_first = firs ? firs : "", s_last = las ? las : "", s_pub = publisher ? publisher : "",
           s_media = media ? media : "", s_cat = catalog_number ? catalog_number : "",
           s_country = release_country ? release_country : "", s_orig = original_date ? original_date : "",
           s_genre = genre ? genre : "";
    bool has_first = firs, has_last = las, has_pub = publisher, has_media = media && catalog_number && release_country,
         has_orig = original_date, has_genre = genre;
    deadbeef->pl_unlock();

	info.append("\n \n");

	if (count == -1){
		count = 0;
	}
	
	if (count == 1){
		info.append(_("Listened once"));
		info.append(" \n \n");
	}
	else {
			info.append(_("Listened "));
			info.append(to_string(count));
			info.append(_(" times"));
			info.append("\n \n");
	}

	if (has_first) {
		string first = s_first;
		info.append(_("For the first time "));
		info.append(first.substr(0, min<size_t>(10, first.size())));
		info.append("\n");
		info.append(_("at "));
		info.append((first.size() > 11 ? first.substr(11) : string()));
		info.append("\n \n");
	}
	else{
		info.append(_("Never listened before"));
		info.append("\n \n");
	}

	if (has_last) {
		string last = s_last;
		info.append(_("For the last time the "));
		info.append(last.substr(0, min<size_t>(10, last.size())));
		info.append("\n");
		info.append(_("at "));
		info.append( (last.size() > 11 ? last.substr(11) : string()));
		info.append("\n \n");
	}


	info.append("Bitrate: ");
	info.append(to_string(bitrate));
	info.append(" kbps\n \n");
	if (playcount == 1){
		info.append(_("There is one file in line"));
		info.append("\n \n");
	}
	else{
		info.append(_("There are "));
		info.append(to_string(playcount));
		info.append(_(" files in line"));
		info.append("\n \n");
	}
	
	if (has_pub) {
	    info.append(_("Publisher: "));
	    info.append(s_pub);
	    info.append("\n \n");
	}
	
	if (has_media) {
	    info.append(s_media);
	    info.append(" - ");
	    info.append(s_cat);
	    info.append(" - ");
	    info.append(s_country);
	    info.append("\n \n");
	}

	if (has_orig) {
	    info.append(_("Released date: "));
	    info.append(s_orig);
	    info.append("\n \n");
	}

	if (has_genre) {
	    info.append(_("Genre: "));
	    info.append(s_genre);
	    info.append("\n \n");
	}

	if (involved_people != NULL) {
	    info.append(_("Creators:"));
	    info.append("\n");
	    vector<string> involved = split(involved_people, " / ");
	    if (involved.size() == 1){
	        involved = split(involved[0], "\n");
	    }
	    if (involved.size() > 1){
	        for(unsigned int i = 0; i < involved.size(); i+=1) {
	            info.append(involved[i]);
	    	    if (i % 2 == 0){
	                info.append(" - ");
	            }
	            else {
	                info.append("\n");
	            }       
	        }
	    }
	    else{
	        info.append(involved[0]);
	    }
	    info.append("\n");
	}

	set_lyrics(track, info, "",  "", "");
}

static void show_lyrics(DB_playItem_t *track, const parsed_lyrics &l) {
	if (l.sync) chopset_lyrics(track, lyricfinder::normalize_lrc(l.lyrics));
	else set_lyrics(track, "", "", l.lyrics, "");
	sync_or_unsync(l.sync);
}

// Tracks no provider had lyrics for (30 min), so replaying them skips the network.
static std::mutex miss_mutex;
static std::map<string, std::chrono::steady_clock::time_point> misses;

static bool recently_missed(const string &key) {
	std::lock_guard<std::mutex> lk(miss_mutex);
	auto it = misses.find(key);
	if (it == misses.end()) return false;
	if (std::chrono::steady_clock::now() - it->second > std::chrono::minutes(30)) { misses.erase(it); return false; }
	return true;
}

static void remember_miss(const string &key) {
	std::lock_guard<std::mutex> lk(miss_mutex);
	misses[key] = std::chrono::steady_clock::now();
}

static void update_lyrics_impl(DB_playItem_t *track) {
	// 1. Lyrics already stored with the track (tag, then .lrc/.txt next to the file).
	struct parsed_lyrics local = get_lyrics_from_metadata(track);
	if (local.lyrics.empty())
		local = get_lyrics_next_to_file(track);

	bool have_local = !local.lyrics.empty();
	if (have_local) {
		show_lyrics(track, local);
		if (local.sync) return;
	}

	// 2. Automatic online lookup, honoring the autosearch and upgrade settings.
	bool autosearch = deadbeef->conf_get_int("lyricbar.autosearch.enable", 1) != 0;
	bool upgrade = deadbeef->conf_get_int("lyricbar.upgrade_unsynced", 1) != 0;
	if (!autosearch || (have_local && !upgrade)) {
		if (!have_local) set_info(track);
		return;
	}

	lyricfinder::Query q;
	{
		deadbeef->pl_lock();
		const char *t = deadbeef->pl_find_meta(track, "title");
		const char *a = deadbeef->pl_find_meta(track, "artist");
		const char *al = deadbeef->pl_find_meta(track, "album");
		q.title = t ? t : "";
		q.artist = a ? a : "";
		q.album = al ? al : "";
		deadbeef->pl_unlock();
	}
	q.duration = deadbeef->pl_get_item_duration(track);
	q.musixmatch = deadbeef->conf_get_int("lyricbar.enable_musixmatch", 0) != 0;

	string key = q.artist + "|" + q.title + "|" + std::to_string((int)q.duration) + (q.musixmatch ? "|mxm" : "");
	if (q.title.empty() || recently_missed(key)) {
		if (!have_local) set_info(track);
		return;
	}

	if (!have_local) set_lyrics(track, "", "", _("Loading..."), "");

	lyricfinder::Candidate best;
	auto cancelled = [track] { return !is_playing(track); };
	lyricfinder::Result res = lyricfinder::find_best(q, best, cancelled);

	if (res == lyricfinder::Result::Cancelled || !is_playing(track)) return;

	if (res == lyricfinder::Result::Found && (!have_local || best.synced)) {
		parsed_lyrics found = {best.lyrics, best.synced};
		show_lyrics(track, found);

		int method = deadbeef->conf_get_int("save_method", 0);
		if (method == 0) save_meta_data(track, found);
		else if (method == 1) save_next_to_file(track, found);
		return;
	}

	if (res == lyricfinder::Result::NotFound) remember_miss(key);
	if (!have_local) set_info(track);   // nothing found anywhere: show track info
}

// Thread entry point. Owns one reference to the track (taken by the caller).
void update_lyrics(void *tr) {
	DB_playItem_t *track = static_cast<DB_playItem_t*>(tr);
	if (!track) return;
	try {
		update_lyrics_impl(track);
	} catch (const std::exception &e) {
		cerr << "lyricbar: update_lyrics failed: " << e.what() << endl;
	} catch (...) {
		cerr << "lyricbar: update_lyrics failed" << endl;
	}
	deadbeef->pl_item_unref(track);
}

//---------------------------------------------------------------------
//****** /Main: Try to get lyrics from Metadata,file,LRCLIB. *********
//---------------------------------------------------------------------


/**
 * Creates the directory tree.
 * @param name the directory path, including trailing slash
 * @return 0 on success; errno after mkdir call if something went wrong
 */

int mkpath(const string &name, mode_t mode) {
	string dir;
	size_t pos = 0;
	while ((pos = name.find_first_of('/', pos)) != string::npos){
		dir = name.substr(0, pos++);
		if (dir.empty())
			continue; // ignore the leading slash
		if (mkdir(dir.c_str(), mode) && errno != EEXIST)
			return errno;
	}
	return 0;
}

int remove_from_cache_action(DB_plugin_action_t *, ddb_action_context_t) {
	// Lyrics are stored in tags / next to the file; there is no cache to purge.
	return 0;
}
