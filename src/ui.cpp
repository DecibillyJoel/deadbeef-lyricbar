#include "ui.h"
#include "debug.h"
#include "gettext.h"
#include <deadbeef/deadbeef.h>

#include <vector>
#include <regex>
#include <gtkmm.h>
#include <time.h>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>

using namespace std;
using namespace Gtk;
using namespace Glib;

struct linessizes{
	int titlesize;
	int artistsize;
	int newlinesize;
};

const DB_playItem_t *last;

struct timespec tss = {0, 100000000};

// TODO: eliminate all the global objects, as their initialization is not well defined
	
static TextView *lyricView;
static ScrolledWindow *lyricbar;
static RefPtr<TextBuffer> refBuffer;
static RefPtr<TextTag> tagItalic, tagBold, tagLarge, tagCenter, tagSmall, tagForegroundColorHighlight,tagForegroundColorRegular, tagLeftmargin, tagRightmargin, tagRegular;
static vector<RefPtr<TextTag>> tagsTitle, tagsArtist, tagsSyncline, tagsNosyncline, tagPadding;
static volatile bool lyricbar_active = false;

bool isValidHexaCode(string str) {
    regex hexaCode("^#([a-fA-F0-9]{6}|[a-fA-F0-9]{3})$");
    return regex_match(str, hexaCode);
}

// "Size" lyrics to be able to make lines "dissapear" on top.
vector<int> sizelines(DB_playItem_t * track, string lyrics) {
	set_lyrics(track, lyrics,"","","");
//	std::cout << "Sizelines" << "\n";
	death_signal = 1;

//	I didn't found another way to be sure lyrics are displayed than wait millisenconds with nanosleep.
	nanosleep(&tss, NULL);

	// Use shared_ptr so the result survives even if we time out
	struct SizelineResult {
		std::mutex mtx;
		std::condition_variable cv;
		bool done = false;
		vector<int> values;
	};
	auto result = std::make_shared<SizelineResult>();

	signal_idle().connect_once([result] {
		if (!lyricbar_active || !refBuffer || !lyricView || !lyricbar) {
			std::lock_guard<std::mutex> lk(result->mtx);
			result->done = true;
			result->cv.notify_one();
			return;
		}

		int temporaly = 0;
		int sumatory = 0;

		deadbeef->pl_lock();
		result->values.push_back(lyricbar->get_allocation().get_height()*(deadbeef->conf_get_int("lyricbar.vpostion", 50))/100);
		deadbeef->pl_unlock();
		result->values.push_back(0);
		Gdk::Rectangle rectangle;
		for (int i = 2; i < refBuffer->get_line_count()-1; i++) {
			lyricView->get_iter_location(refBuffer->get_iter_at_line(i-2), rectangle);
			result->values.push_back(rectangle.get_y() - temporaly);
			temporaly = rectangle.get_y();
		}

		if (result->values.size() >= 5) {
			result->values[1] = result->values.size()-3;

			for (unsigned i = 2; i < result->values.size()-2; i++) {
				sumatory += result->values[i];
				if (sumatory > (result->values[0] - result->values[3] - result->values[4])) {
					result->values[1] = i-2;
					break;
				}
			}
		}

		std::lock_guard<std::mutex> lk(result->mtx);
		result->done = true;
		result->cv.notify_one();
	});

	{
		std::unique_lock<std::mutex> lk(result->mtx);
		result->cv.wait_for(lk, std::chrono::seconds(2), [&result]{ return result->done; });
	}

//	std::cout << "Sizelines finished" << "\n";
	death_signal = 0;

	return result->values;
}

void set_lyrics(DB_playItem_t *track, string past, string present, string future, string padding) {
	deadbeef->pl_item_ref(track);
	signal_idle().connect_once([track, past, present, future, padding ] {

		if (!lyricbar_active || !refBuffer || !is_playing(track)) {
			deadbeef->pl_item_unref(track);
			return;
		}
		string artist, title;
		deadbeef->pl_lock();
		artist = deadbeef->pl_find_meta(track, "artist") ?: _("Unknown Artist");
		title  = deadbeef->pl_find_meta(track, "title") ?: _("Unknown Title");
		deadbeef->pl_unlock();
	
		refBuffer->erase(refBuffer->begin(), refBuffer->end());

		refBuffer->insert_with_tags(refBuffer->begin(), title, tagsTitle);

		if (g_utf8_validate(future.c_str(),-1,NULL)){
			refBuffer->insert_with_tags(refBuffer->end(), string{"\n"} + artist + "\n\n", tagsArtist);
			refBuffer->insert_with_tags(refBuffer->end(), padding, tagPadding);
			refBuffer->insert_with_tags(refBuffer->end(),past, tagsNosyncline);
			refBuffer->insert_with_tags(refBuffer->end(),present, tagsSyncline);
			refBuffer->insert_with_tags(refBuffer->end(),future, tagsNosyncline);
		}
		else{
			death_signal = 1;
			string error = "Wrong character encoding";
			refBuffer->insert_with_tags(refBuffer->end(), padding, tagPadding);
			refBuffer->insert_with_tags(refBuffer->end(),error, tagsSyncline);
		}

		deadbeef->pl_item_unref(track);

	});
}

// To have scroll bars or not when lyrics are synced or not.
void sync_or_unsync(bool syncedlyrics) {
	signal_idle().connect_once([syncedlyrics] {
		if (!lyricbar_active || !lyricbar) return;
		if (syncedlyrics == true) {
			lyricbar->set_policy(POLICY_EXTERNAL, POLICY_EXTERNAL);
		}
		else{
			lyricbar->set_policy(POLICY_AUTOMATIC, POLICY_AUTOMATIC);
		}
	});
}

Justification get_justification() {

	int align = deadbeef->conf_get_int("lyricbar.lyrics.alignment", 1);
	switch (align) {
		case 0:
			return JUSTIFY_LEFT;
		case 2:
			return JUSTIFY_RIGHT;
		default:
			return JUSTIFY_CENTER;
	}
}

	void get_tags() {
	tagItalic = refBuffer->create_tag();
	tagItalic->property_style() = Pango::STYLE_ITALIC;
	tagItalic->property_scale() = deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagRegular = refBuffer->create_tag();
	tagRegular->property_scale() = deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagLeftmargin = refBuffer->create_tag();
	tagLeftmargin->property_left_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagRightmargin = refBuffer->create_tag();
	tagRightmargin->property_right_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagBold = refBuffer->create_tag();
	tagBold->property_scale() = deadbeef->conf_get_float("lyricbar.fontscale", 1);
	tagBold->property_weight() = Pango::WEIGHT_BOLD;

	tagLarge = refBuffer->create_tag();
	tagLarge->property_scale() = 1.2*deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagSmall = refBuffer->create_tag();
	tagSmall->property_scale() = 0.0001;

	tagCenter = refBuffer->create_tag();
	tagCenter->property_justification() = JUSTIFY_CENTER;

	tagForegroundColorHighlight = refBuffer->create_tag();
	tagForegroundColorRegular = refBuffer->create_tag();

	if (isValidHexaCode(deadbeef->conf_get_str_fast("lyricbar.highlightcolor", "#571c1c"))){
		tagForegroundColorHighlight->property_foreground() = deadbeef->conf_get_str_fast("lyricbar.highlightcolor", "#571c1c");
	}
	else{
		tagForegroundColorHighlight->property_foreground() = "#571c1c";
	}

	if (isValidHexaCode(deadbeef->conf_get_str_fast("lyricbar.regularcolor", "#000000"))){
		tagForegroundColorRegular->property_foreground() = deadbeef->conf_get_str_fast("lyricbar.regularcolor", "#000000");
	}
	else{
		tagForegroundColorRegular->property_foreground() = "#000000";
	}


	tagsTitle = {tagLarge, tagBold, tagCenter};
	tagsArtist = {tagItalic, tagCenter};

	tagsSyncline = {tagBold, tagForegroundColorHighlight};

	if (deadbeef->conf_get_int("lyricbar.bold", 1) == 1) {
		tagsSyncline = {tagBold, tagForegroundColorHighlight};
		tagsNosyncline = {tagRegular, tagLeftmargin, tagRightmargin, tagForegroundColorRegular};
	}
	else{
		tagsSyncline = {tagRegular, tagForegroundColorHighlight};
		tagsNosyncline = {tagRegular, tagForegroundColorRegular};
    	}

    if (get_justification() == JUSTIFY_LEFT) {
		tagRightmargin->property_right_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);
		tagsNosyncline = {tagRegular, tagRightmargin};
    }
	if (get_justification() == JUSTIFY_RIGHT) {
    	tagLeftmargin->property_left_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);
		tagsNosyncline = {tagRegular, tagLeftmargin};
    }

	tagPadding = {tagSmall};
}

extern "C"

GtkWidget *construct_lyricbar() {
	Gtk::Main::init_gtkmm_internals();
	refBuffer = TextBuffer::create();
	get_tags();
	lyricView = new TextView(refBuffer);
	lyricView->set_editable(false);
	lyricView->set_can_focus(false);
	lyricView->set_name("lyricView");
	lyricView->set_left_margin(2);
	lyricView->set_right_margin(2);
	lyricView->set_justification(get_justification());
    	lyricView->get_vadjustment()->set_value(1000);
	lyricView->set_wrap_mode(WRAP_WORD_CHAR);
    	if (get_justification() == JUSTIFY_LEFT) {
    		lyricView->set_left_margin(20);
    	}
	lyricView->show();
	lyricbar = new ScrolledWindow();
	lyricbar->add(*lyricView);
	lyricbar->set_name("lyricbar");
	lyricbar->set_policy(POLICY_EXTERNAL, POLICY_EXTERNAL);


	/**********/

	//Create css config text.
	std::string cssconfig("\
  		#lyricView text {\
  		background-color: ");
	if (isValidHexaCode(deadbeef->conf_get_str_fast("lyricbar.backgroundcolor", "#F6F6F6"))){
		cssconfig.append(deadbeef->conf_get_str_fast("lyricbar.backgroundcolor", "#F6F6F6"));
	}
	else{
		cssconfig.append("#F6F6F6");
	}
	cssconfig.append(";\
		}\
	");

	//load css
	auto data = g_strdup_printf("%s", cssconfig.c_str());

	RefPtr<Gtk::CssProvider> cssProvider = Gtk::CssProvider::create();
	cssProvider->load_from_data(data);
	
	RefPtr<Gtk::StyleContext> styleContext = Gtk::StyleContext::create();
	
	//get default screen
	RefPtr<Gdk::Screen> screen = Gdk::Screen::get_default();
	
	//add provider for screen in all application
	styleContext->add_provider_for_screen(screen, cssProvider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	lyricbar_active = true;
	return GTK_WIDGET(lyricbar->gobj());
}




extern "C"
int message_handler(struct ddb_gtkui_widget_s*, uint32_t id, uintptr_t ctx, uint32_t, uint32_t) {
	auto event = reinterpret_cast<ddb_event_track_t *>(ctx);
	switch (id) {
		case DB_EV_CONFIGCHANGED:
			debug_out << "CONFIG CHANGED\n";
			if (lyricbar_active && refBuffer) {
				get_tags();
			}
			signal_idle().connect_once([]{
				if (lyricbar_active && lyricView) {
					lyricView->set_justification(get_justification());
				}
			});
			break;
		case DB_EV_TERMINATE:
			debug_out << "DEADBEEF CLOSED \n";
			death_signal = 1;
			break;
		case DB_EV_SONGCHANGED:
		case DB_EV_SONGFINISHED:
		{
			debug_out << "SONG FINISHED/CHANGED\n";
			death_signal = 1;
			DB_playItem_t *pl_track = deadbeef->streamer_get_playing_track_safe();
			if (!pl_track) {
				// No track is playing; clear the lyrics display
				last = NULL;
				signal_idle().connect_once([] {
					if (lyricbar_active && refBuffer) {
						refBuffer->erase(refBuffer->begin(), refBuffer->end());
					}
				});
			} else {
				deadbeef->pl_item_unref(pl_track);
			}
			break;
		}
//		case DB_EV_TRACKINFOCHANGED:
//			debug_out << "TRACKINFOCHANGED" << "\n";
//			break;
		case DB_EV_PLUGINSLOADED:
		case DB_EV_SONGSTARTED:
		{
			debug_out << "SONG STARTED\n";
			death_signal = 1;
			if (!event->track || event->track == last || deadbeef->pl_get_item_duration(event->track) <= 0.0){
//				std::cout << "if in" << "\n";
				return 0;
			}
			last = event->track;
			deadbeef->pl_item_ref(event->track);
//			std::cout << "SONG STARTED" << "\n";
			auto tid = deadbeef->thread_start(update_lyrics, event->track);
			if (!tid) {
				deadbeef->pl_item_unref(event->track);
				break;
			}
			deadbeef->thread_detach(tid);
			break;
		}
	}

	return 0;
}

extern "C"
void lyricbar_destroy() {
	lyricbar_active = false;
	death_signal = 1;
	delete lyricbar;
	lyricbar = nullptr;
	delete lyricView;
	lyricView = nullptr;
	tagsArtist.clear();
	tagPadding.clear();
	tagsSyncline.clear();
	tagsNosyncline.clear();
	tagRegular.clear();
	tagsTitle.clear();
	tagLarge.reset();
	tagSmall.reset();
	tagBold.reset();
	tagItalic.reset();
	tagRightmargin.reset();
	tagLeftmargin.reset();
	refBuffer.reset();
}
