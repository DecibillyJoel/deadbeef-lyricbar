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


using namespace std;

//Global variables:

//static const char *home_cache = getenv("XDG_CACHE_HOME");
//static const string lyrics_dir = (home_cache ? string(home_cache) : string(getenv("HOME")) + "/.cache") + "/deadbeef/lyrics/";



mutex mtx;

volatile int death_signal = 0;
bool lyricstart = false;
bool syncedlyrics = false;

vector<int> linessizes;


struct sync lrc;

struct timespec ts = {0, 50000000};

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

// Functions to bubble sort lyrics and timestamps.
void swap(double *xp, double *yp)
{
    double temp = *xp;
    *xp = *yp;
    *yp = temp;
}

void swaptext(string *xp, string *yp)
{
    string temp = *xp;
    *xp = *yp;
    *yp = temp;
}

// Fuctions to order lyrics and timestamps.
struct sync bubbleSort(vector<double> arr, vector<string> text ,  int n)
{
	if ((arr.size() == 0) || (text.size() != arr.size())){
		vector<double> nullposition;
		vector<string> errortext;
		nullposition.push_back(0);
		errortext.push_back(_("Error with lyrics file or metadata"));
		cout << "Error with lyrics file or metadata \n";
		struct sync error = {errortext, nullposition};
		return error;
	}
	else{
    	int i, j;
    	for (i = 0; i < n-1; i++){   
    		// Last i elements are already in place.
    		for (j = 0; j < n-i-1; j++){
        		if (arr[j] > arr[j+1]){
        		    swap(&arr[j], &arr[j+1]);
					swaptext(&text[j], &text[j+1]);
				}
			}
		}
	struct sync retur = {text,arr};
	return retur;
	}
}

// Function to parse synced lyrics:
struct sync lyric2vector( string lyrics){
	//cout << "lyric2vector" "\n";
	vector<string> synclyrics;
	vector<double> position;
	string line;
	int squarebracket;
	int repeats = 0; 
	//If last character is a ] add an space to have same number of lyrics and positions.
	if (lyrics.empty() || lyrics.length() <= 3){
		position.push_back(0);
		struct sync  emptylyrics = bubbleSort(position, synclyrics, 1);
		return emptylyrics;
	}
	if (lyrics.at(lyrics.length() - 1) == ']'){
		lyrics.push_back(' ');
	}

	for (unsigned i=0; i < lyrics.length() - 3; ++i){
		if ((lyrics.at(i) == '[') && (lyrics.at(i+3) == ':') && (lyrics.at(i+6) == '.') ) {
  			++repeats;
			position.push_back ((lyrics.at(i + 1) - 48)*600 + (lyrics.at(i + 2) - 48)*60 + (lyrics.at(i + 4) - 48)*10 + (lyrics.at(i + 5) - 48) + (lyrics.at(i + 7) - 48)*0.1 + (lyrics.at(i + 8) - 48)*0.01);
			if ((lyrics.length() > i + 10) && (lyrics.at(i+10) != '[')) {
				line = lyrics.substr(i + 10, lyrics.length() - i - 10);
				squarebracket = line.find_first_of('[');
				if (((lyrics.at(i+8 + squarebracket)) != '\n') && (lyrics.at(i+8 + squarebracket)) != '\r'){
                    line = lyrics.substr(i + 10, squarebracket-1);
				}
				else {
                    line = lyrics.substr(i + 10, squarebracket-2);
				}
				++repeats;
				while (--repeats ) {
					synclyrics.push_back (line);
				}	
			}
		}
	}

	int n = position.size();
	struct sync  goodlyrics = bubbleSort(position, synclyrics, n);

// For debug:
//	for (auto k = goodlyrics.position.begin(); k != goodlyrics.position.end(); ++k)
//    	cout << *k << " ";
//
//	for (auto i = goodlyrics.synclyrics.begin(); i != goodlyrics.synclyrics.end(); ++i)
//    	cout << *i << '\n';
	
	return goodlyrics;
}

// Function to scroll and remark line:
void write_synced( DB_playItem_t *it){
	float pos = deadbeef->streamer_get_playpos();
	string past = "";
	string present = "";
	string future = "";
	string padding = "";
	int presentpos = 0;
	int minimuntopad = 0;

	// Take a thread-safe snapshot of shared sync data
	struct sync local_lrc;
	vector<int> local_linessizes;
	{
		std::lock_guard<std::mutex> lock(mtx);
		local_lrc = lrc;
		local_linessizes = linessizes;
	}

	if ( local_lrc.position.size() > 2) {

		for (unsigned i = 0; i < local_lrc.position.size()-2; i++){
			if (local_lrc.position[i] < pos){
				if (local_lrc.position[i+1] > pos){
					present = " " + local_lrc.synclyrics[i] + " " + "\n";
					presentpos = i;
				}
			}
			else if (pos < local_lrc.position[i]){
					future.append(local_lrc.synclyrics[i] + "\n");
			}
		}

		//Add padding variable at beginning of lyrics to show to make scroll with first lines.
		if (local_linessizes.size() >= 2 &&
			(local_linessizes[1]+1) < (int)local_lrc.position.size()){
			for  (int j = 0; j < (int)((local_lrc.position[local_linessizes[1]+1] - pos)/(local_lrc.position[local_linessizes[1]+1])*local_linessizes[0]); j++){
				padding.append("\n");
			}
		}

//cout << "Present position: " << presentpos << " pos: " << pos << "\n";

		//Add padding variable at beginning of lyrics to show to make scroll when removing a past line.
		int ls_idx = presentpos - (local_linessizes.size() >= 2 ? local_linessizes[1] : 0) + 5;
		if (local_linessizes.size() >= 2 && (presentpos - local_linessizes[1]) > 0 &&
			ls_idx >= 0 && ls_idx < (int)local_linessizes.size() &&
			(presentpos + 1) < (int)local_lrc.position.size()){
			minimuntopad = presentpos - local_linessizes[1];
			for  (int j = 0 ; j < (int)(((local_lrc.position[presentpos +1] - pos)/(local_lrc.position[presentpos+1] -local_lrc.position[presentpos]))*(local_linessizes[ls_idx] -1)); j++){
				padding.append("\n");
			}
		}

		//Removing first past lyrics lines to make scroll.
		for (unsigned i = minimuntopad; i < local_lrc.position.size()-2 && local_lrc.position[i+1] < pos; i++){
			past.append(local_lrc.synclyrics[i] + "\n");
		}
	set_lyrics(it, past, present, future, padding);
	}

	else{
		past = " ";
		present = " ";
		future = " ";
		set_lyrics(it, past, present, future, padding);
	}
}

// Main loop to update lyrics on real time.
void thread_listener(DB_playItem_t *track){

	// Immediately display properly-positioned lyrics to avoid
	// a visible flash of the measurement frame from sizelines
	if (track && is_playing(track) && death_signal == 0) {
		write_synced(track);
	}

	while (track && (is_playing(track)) && death_signal == 0){
		nanosleep(&ts, NULL);
		write_synced(track);
	}
	if (track) {
		deadbeef->pl_item_unref(track);
	}
}

// Main loop thread caller.
void chopset_lyrics(DB_playItem_t *track, string lyrics){
//	cout << "Chopset lyrics" "\n";
	struct sync local_lrc = lyric2vector(lyrics);
	DB_playItem_t *it = deadbeef->streamer_get_playing_track_safe();
	if (!it) {
		return;
	}
	float length = deadbeef->pl_get_item_duration(it);

	local_lrc.position.push_back((float)length -0.2);
	local_lrc.position.push_back((float)length);
	local_lrc.synclyrics.push_back("\n");
	local_lrc.synclyrics.push_back("\n");
	string prelyrics = "";
	for (unsigned i = 0; i < local_lrc.synclyrics.size()-2; i++){
			prelyrics.append(local_lrc.synclyrics[i] + "\n");
	}
	vector<int> local_linessizes;
	if (track == it){
		local_linessizes = sizelines(track, prelyrics);
	}

	// Atomically install new sync data before starting listener thread
	{
		std::lock_guard<std::mutex> lock(mtx);
		lrc = std::move(local_lrc);
		linessizes = std::move(local_linessizes);
	}

	// thread_listener will unref `it` when it finishes
	thread t1(thread_listener, it);
	t1.detach();
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
	bool has_SYLT = deadbeef->pl_find_meta(playing_song, "SYLT") != NULL;
	bool has_LYRICS = deadbeef->pl_find_meta(playing_song, "LYRICS") != NULL;
	bool has_UNSYNCEDLYRICS = deadbeef->pl_find_meta(playing_song, "UNSYNCEDLYRICS") != NULL;
	deadbeef->pl_unlock();
	if (lyrics.sync == true){	
		if (has_SYLT){
			deadbeef->pl_delete_meta(playing_song, "SYLT");
		}

		if (has_LYRICS){
			deadbeef->pl_delete_meta(playing_song, "LYRICS");
		}
		deadbeef->pl_add_meta(playing_song, "LYRICS", lyrics.lyrics.c_str());	
	}
	else{
		if (has_UNSYNCEDLYRICS){
			deadbeef->pl_delete_meta(playing_song, "UNSYNCEDLYRICS");
		}
		deadbeef->pl_add_meta(playing_song, "UNSYNCEDLYRICS", lyrics.lyrics.c_str());	
	}
	deadbeef->pl_lock(); 
	const char *dec = deadbeef->pl_find_meta_raw(playing_song, ":DECODER"); 
	char decoder_id[100];

	if (dec){
		strncpy(decoder_id, dec, sizeof(decoder_id));
	}
	int match = playing_song && dec;
	deadbeef->pl_unlock();

	if (match){
		DB_decoder_t *dec = NULL;
		DB_decoder_t **decoders = deadbeef->plug_get_decoder_list();

		for (int i = 0; decoders[i]; i++){

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

// Function to parse webpages.
string text_downloader(curl_slist *slist, string url, string post) {
	CURL *curl_handle;
	curl_handle = curl_easy_init();
	string readBuffer = "";
	if (curl_handle) {	
		curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 600);
		curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0);
		curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 0);
		curl_easy_setopt(curl_handle, CURLOPT_HEADER, 0);
		curl_easy_setopt(curl_handle, CURLOPT_ENCODING, NULL);
		curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, false);
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
		curl_easy_perform(curl_handle);
		curl_easy_cleanup(curl_handle);
	}
	return readBuffer;
}

//---------------------------------------------------------------------
//************ /Share functions of lyrics downloaders. ****************
//---------------------------------------------------------------------

inline string cached_filename(string artist, string title) {
	return 0;
}

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
	bool sync = false;
	bool first_timestamped = false;
	int total_lines = 1;
	int timestamped_lines = 1;
	ifstream infile;
	string lyrics;
	string line;

	deadbeef->pl_lock();
	const char *track_location = deadbeef->pl_find_meta(track, ":URI");
	string trackstring = track_location ? track_location : "";
	deadbeef->pl_unlock();
	size_t lastindex = trackstring.find_last_of(".");
	trackstring = trackstring.substr(0, lastindex);
	infile.open(trackstring + ".lrc", ios_base::out);
	if (!infile.is_open()){
	    infile.open(trackstring + ".txt", ios_base::out);
	}
	
	if(infile.is_open()){
		while (getline(infile, line)) {
		    lyrics = lyrics + line + "\n";
		    
		    if (line.length() > 4){
		        for (unsigned i=0; i < line.length() - 3; ++i){
		            if ((line.at(i) == '[') && (line.at(i+3) == ':') && (line.at(i+6) == '.') ) {
		                first_timestamped = true;
		                timestamped_lines += 1;
		            }
		        }
		    }
		    if (first_timestamped == true){
		        total_lines += 1;
		    }
		}
		if ((timestamped_lines*100/total_lines > 75) && (total_lines != 1)){
			sync = true;		
		};
			
	}
	return{lyrics, sync};	
}

struct parsed_lyrics get_lyrics_from_metadata(DB_playItem_t *track) {
	syncedlyrics = true;
	deadbeef->pl_lock();
	const char *lyrics = deadbeef->pl_find_meta(track, "lyrics")
							?: deadbeef->pl_find_meta(track, "SYLT");
	if (!lyrics) {
		syncedlyrics = false;
		lyrics = deadbeef->pl_find_meta(track, "unsynced lyrics")
	                  ?: deadbeef->pl_find_meta(track, "UNSYNCEDLYRICS");
		if (!lyrics) {
			lyrics = "";
		}
	}
	string string_lyrics = lyrics;
	deadbeef->pl_unlock();
	return {string_lyrics, syncedlyrics};
}


void save_next_to_file(DB_playItem_t *track, struct parsed_lyrics lyrics) {
	deadbeef->pl_lock();
	const char *track_location = deadbeef->pl_find_meta(track, ":URI");
	string trackstring = track_location ? track_location : "";
	deadbeef->pl_unlock();
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
	outfile << lyrics.lyrics << endl;
	outfile.close();
}

//Sets track info if no lyrics founded.
void set_info(DB_playItem_t *track) {
	string info = "";
	string las_str, firs_str, publisher_str, media_str;
	string catalog_number_str, release_country_str, original_date_str, genre_str;
	string involved_str;
	int count;

	deadbeef->pl_lock();
    count = deadbeef->pl_find_meta_int(track, "PLAY_COUNTER", -1);
	if (count == -1){
		count  = deadbeef->pl_find_meta_int(track, "play_count", -1);
	}
	// Copy all metadata to local strings while under lock to avoid
	// use-after-free if another thread modifies metadata concurrently.
	const char *tmp;
	tmp = deadbeef->pl_find_meta(track, "LAST_PLAYED");
	if (tmp) las_str = tmp;
	tmp = deadbeef->pl_find_meta(track, "FIRST_PLAYED");
	if (tmp) firs_str = tmp;
	tmp = deadbeef->pl_find_meta(track, "PUBLISHER");
	if (tmp) publisher_str = tmp;
	tmp = deadbeef->pl_find_meta(track, "MEDIA TYPE");
	if (tmp) media_str = tmp;
	tmp = deadbeef->pl_find_meta(track, "CATALOGNUMBER");
	if (tmp) catalog_number_str = tmp;
	tmp = deadbeef->pl_find_meta(track, "MUSICBRAINZ ALBUM RELEASE COUNTRY");
	if (tmp) release_country_str = tmp;
	tmp = deadbeef->pl_find_meta(track, "ORIGINAL_RELEASE_TIME");
	if (tmp) original_date_str = tmp;
	tmp = deadbeef->pl_find_meta(track, "GENRE");
	if (tmp) genre_str = tmp;

    DB_metaInfo_t *involved_people_meta = deadbeef->pl_meta_for_key (track, "InvolvedPeople");
    if (involved_people_meta != NULL) {
        size_t value_size = involved_people_meta->valuesize;
        involved_str.assign(involved_people_meta->value, value_size);
        // Replace null-byte separators (deadbeef multi-value encoding) with newlines
        for (size_t i = 0; i < involved_str.size(); i++) {
            if (involved_str[i] == '\0') {
                involved_str[i] = '\n';
            }
        }
    }
    deadbeef->pl_unlock();

	int playcount = deadbeef->playqueue_get_count();
	int bitrate = deadbeef->streamer_get_apx_bitrate();

	while (bitrate == -1){
		bitrate = deadbeef->streamer_get_apx_bitrate();
	}
    
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

	if (!firs_str.empty()) {
		info.append(_("For the first time "));
		info.append(firs_str.substr(0,10));
		info.append("\n");
		info.append(_("at "));
		info.append(firs_str.substr(11, firs_str.length() -1));
		info.append("\n \n");
	}
	else{
		info.append(_("Never listened before"));
		info.append("\n \n");
	}

	if (!las_str.empty()) {
		info.append(_("For the last time the "));
		info.append(las_str.substr(0,10));
		info.append("\n");
		info.append(_("at "));
		info.append( las_str.substr(11, las_str.length() -1));
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
	
	if (!publisher_str.empty()) {
	    info.append(_("Publisher: "));
	    info.append(publisher_str);
	    info.append("\n \n");
	}
	
	if (!media_str.empty() && !catalog_number_str.empty() && !release_country_str.empty()) {
	    info.append(media_str);
	    info.append(" - ");
	    info.append(catalog_number_str);
	    info.append(" - ");
	    info.append(release_country_str);
	    info.append("\n \n");
	}
	
	if (!original_date_str.empty()) {
	    info.append(_("Released date: "));
	    info.append(original_date_str);
	    info.append("\n \n");
	}

	if (!genre_str.empty()) {
	    info.append(_("Genre: "));
	    info.append(genre_str);
	    info.append("\n \n");
	}

	if (!involved_str.empty()) {
	    info.append(_("Creators:"));
	    info.append("\n");
	    vector<string> involved = split(involved_str, " / ");
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

//Main function:
void update_lyrics(void *tr) {
	{
		std::lock_guard<std::mutex> lock(mtx);
		linessizes.clear();
	}
	DB_playItem_t *track = static_cast<DB_playItem_t*>(tr);
	struct parsed_lyrics meta_lyrics = get_lyrics_from_metadata(track);
	if (meta_lyrics.lyrics != "") {
		if (meta_lyrics.sync == true) {
			chopset_lyrics(track, meta_lyrics.lyrics);
		}
		else{
			set_lyrics(track, "", "", meta_lyrics.lyrics, "");
		}
		sync_or_unsync(meta_lyrics.sync);
		deadbeef->pl_item_unref(track);
		return;
	}

	string artist_str;
	string title_str;
	{
		deadbeef->pl_lock();
		const char *a = deadbeef->pl_find_meta(track, "artist");
		const char *t = deadbeef->pl_find_meta(track, "title");
		artist_str = a ? a : _("Unknown Artist");
		title_str  = t ? t : _("Unknown Title");
		deadbeef->pl_unlock();
	}



	if (!artist_str.empty() && !title_str.empty()) {
		struct parsed_lyrics cached_lyrics = get_lyrics_next_to_file(track);

		if (cached_lyrics.lyrics != "") {
			if (cached_lyrics.sync == true) {
				chopset_lyrics(track, cached_lyrics.lyrics);
			}
			else{
				set_lyrics(track, "", "", cached_lyrics.lyrics, "");
			}
			sync_or_unsync(cached_lyrics.sync);
			deadbeef->pl_item_unref(track);
			return;
		}
	}
	set_lyrics(track, "", "", _("Loading..."), "");

//	 No lyrics in the tag or cache; try to get some and cache if succeeded.
//	Search for lyrics on LRCLIB:
    if (deadbeef->conf_get_float("lyricbar.fontscale", 1) == 1){
    	struct parsed_lyrics lrclib_lyrics = {"",false};
    	lrclib_lyrics = lrclib(title_str, artist_str, "");
    	if (lrclib_lyrics.lyrics != "") {
    		if (lrclib_lyrics.sync){
    			chopset_lyrics(track, lrclib_lyrics.lyrics);

	    	}
		    else{
		    	set_lyrics(track, "", "", lrclib_lyrics.lyrics, "");
		    }
	    	sync_or_unsync(lrclib_lyrics.sync);
	    	
	    	if (deadbeef->conf_get_int("save_method", 0) == 0){
	    		save_meta_data(track, lrclib_lyrics);
	    	}
	    	else if (deadbeef->conf_get_int("save_method", 0) == 1){
	    	    save_next_to_file(track, lrclib_lyrics);
	    	}
	    	
	    	deadbeef->pl_item_unref(track);
	    	return;
    	}
    }
//	If no lyrics founded in any site, show track info:
	set_info(track);
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

int remove_from_cache_action(DB_plugin_action_t *, ddb_action_context_t ctx) {
    bindtextdomain("deadbeef-lyricbar", "gettext");
    textdomain("deadbeef-lyricbar");
  
	if (ctx == DDB_ACTION_CTX_SELECTION) {
		deadbeef->pl_lock();
		ddb_playlist_t *playlist = deadbeef->plt_get_curr();
		if (playlist) {
			DB_playItem_t *current = deadbeef->plt_get_first(playlist, PL_MAIN);
			while (current) {
				if (deadbeef->pl_is_selected (current)) {
					const char *artist = deadbeef->pl_find_meta(current, "artist");
					const char *title  = deadbeef->pl_find_meta(current, "title");
					if (is_cached(artist, title)){
						remove(cached_filename(artist, title).c_str());
					}
				}
				DB_playItem_t *next = deadbeef->pl_get_next(current, PL_MAIN);
				deadbeef->pl_item_unref(current);
				current = next;
			}
			deadbeef->plt_unref(playlist);
		}
		deadbeef->pl_unlock();
	}
	return 0;
}
