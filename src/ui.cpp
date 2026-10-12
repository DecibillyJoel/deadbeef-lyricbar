#include "ui.h"
#include "debug.h"
#include "gettext.h"
#include <deadbeef/deadbeef.h>

#include <vector>
#include <regex>
#include <gtkmm.h>
#include <time.h>
#include <string>
#include <iostream>
#include <future>
#include <atomic>
#include <memory>
#include <chrono>

using namespace std;
using namespace Gtk;
using namespace Glib;

struct linessizes{
	int titlesize;
	int artistsize;
	int newlinesize;
};

static std::atomic<bool> ui_alive{false};

struct timespec tss = {0, 100000000};

// TODO: eliminate all the global objects, as their initialization is not well defined
	
static TextView *lyricView;
static ScrolledWindow *lyricbar;
static RefPtr<TextBuffer> refBuffer;
static RefPtr<TextTag> tagItalic, tagBold, tagLarge, tagCenter, tagSmall, tagForegroundColorHighlight,tagForegroundColorRegular, tagLeftmargin, tagRightmargin, tagRegular;
static vector<RefPtr<TextTag>> tagsTitle, tagsArtist, tagsSyncline, tagsNosyncline, tagPadding;

bool isValidHexaCode(string str) {
    regex hexaCode("^#([a-fA-F0-9]{6}|[a-fA-F0-9]{3})$");
    return regex_match(str, hexaCode);
}

// Fill the text buffer. MUST run on the GTK main thread.
static void apply_lyrics(DB_playItem_t *track, const string &past, const string &present,
                         const string &future, const string &padding) {
	if (!ui_alive || !refBuffer) return;
	if (!is_playing(track)) return;
	string artist, title;
	deadbeef->pl_lock();
	artist = deadbeef->pl_find_meta(track, "artist") ?: _("Unknown Artist");
	title  = deadbeef->pl_find_meta(track, "title") ?: _("Unknown Title");
	deadbeef->pl_unlock();

	refBuffer->erase(refBuffer->begin(), refBuffer->end());
	refBuffer->insert_with_tags(refBuffer->begin(), title, tagsTitle);

	if (g_utf8_validate(future.c_str(), -1, NULL) && g_utf8_validate(present.c_str(), -1, NULL)
	    && g_utf8_validate(past.c_str(), -1, NULL)) {
		refBuffer->insert_with_tags(refBuffer->end(), string{"\n"} + artist + "\n\n", tagsArtist);
		refBuffer->insert_with_tags(refBuffer->end(), padding, tagPadding);
		refBuffer->insert_with_tags(refBuffer->end(), past, tagsNosyncline);
		refBuffer->insert_with_tags(refBuffer->end(), present, tagsSyncline);
		refBuffer->insert_with_tags(refBuffer->end(), future, tagsNosyncline);
	} else {
		refBuffer->insert_with_tags(refBuffer->end(), "\n\n", tagsArtist);
		refBuffer->insert_with_tags(refBuffer->end(), _("Wrong character encoding"), tagsSyncline);
	}
}

// Thread-safe: may be called from any thread; the track is ref'd until the idle callback runs.
void set_lyrics(DB_playItem_t *track, string past, string present, string future, string padding) {
	if (!ui_alive || !track) return;
	deadbeef->pl_item_ref(track);
	signal_idle().connect_once([track, past, present, future, padding] {
		try {
			apply_lyrics(track, past, present, future, padding);
		} catch (const std::exception &e) {
			cerr << "lyricbar: " << e.what() << endl;
		}
		deadbeef->pl_item_unref(track);
	});
}

// Measure line heights. All GTK access runs on the main thread; the caller only waits for the result.
vector<int> sizelines(DB_playItem_t *track, string lyrics) {
	if (!ui_alive || !track) return {};
	auto prom = std::make_shared<std::promise<vector<int>>>();
	auto fut = prom->get_future();
	deadbeef->pl_item_ref(track);

	signal_idle().connect_once([track, lyrics, prom] {
		try {
			apply_lyrics(track, lyrics, "", "", "");
		} catch (...) {}
		// Let GTK lay out the new text before measuring it.
		Glib::signal_timeout().connect_once([track, prom] {
			vector<int> values;
			try {
				if (ui_alive && lyricView && lyricbar && refBuffer) {
					int sumatory = 0, temporaly = 0;
					values.push_back(lyricbar->get_allocation().get_height()
					                 * deadbeef->conf_get_int("lyricbar.vpostion", 50) / 100);
					values.push_back(0);
					Gdk::Rectangle rectangle;
					for (int i = 2; i < refBuffer->get_line_count() - 1; i++) {
						lyricView->get_iter_location(refBuffer->get_iter_at_line(i - 2), rectangle);
						values.push_back(rectangle.get_y() - temporaly);
						temporaly = rectangle.get_y();
					}
					values[1] = (int)values.size() - 3;
					for (size_t i = 2; values.size() >= 5 && i + 2 < values.size(); i++) {
						sumatory += values[i];
						if (sumatory > (values[0] - values[3] - values[4])) {
							values[1] = (int)i - 2;
							break;
						}
					}
				}
			} catch (...) { values.clear(); }
			deadbeef->pl_item_unref(track);
			prom->set_value(std::move(values));
		}, 100);
	});

	if (fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready)
		return fut.get();
	return {};
}

// To have scroll bars or not when lyrics are synced or not.
void sync_or_unsync(bool syncedlyrics) {
	if (!ui_alive) return;
	signal_idle().connect_once([syncedlyrics] {
		if (!ui_alive || !lyricbar) return;
		lyricbar->set_policy(syncedlyrics ? POLICY_EXTERNAL : POLICY_AUTOMATIC,
		                     syncedlyrics ? POLICY_EXTERNAL : POLICY_AUTOMATIC);
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

static void forget_old_tags() {
	if (!refBuffer) return;
	auto table = refBuffer->get_tag_table();
	for (auto *t : {&tagItalic, &tagRegular, &tagLeftmargin, &tagRightmargin, &tagBold, &tagLarge,
	                &tagSmall, &tagCenter, &tagForegroundColorHighlight, &tagForegroundColorRegular}) {
		if (*t) { table->remove(*t); t->reset(); }
	}
}

void get_tags() {
	if (!refBuffer) return;
	forget_old_tags();
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
	ui_alive = true;
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
	RefPtr<Gtk::CssProvider> cssProvider = Gtk::CssProvider::create();
	cssProvider->load_from_data(cssconfig);
	
	RefPtr<Gtk::StyleContext> styleContext = Gtk::StyleContext::create();
	
	//get default screen
	RefPtr<Gdk::Screen> screen = Gdk::Screen::get_default();
	
	//add provider for screen in all application
	styleContext->add_provider_for_screen(screen, cssProvider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);


	return GTK_WIDGET(lyricbar->gobj());
}




extern "C"
int message_handler(struct ddb_gtkui_widget_s*, uint32_t id, uintptr_t ctx, uint32_t, uint32_t) {
	switch (id) {
		case DB_EV_CONFIGCHANGED:
			if (!ui_alive) break;
			signal_idle().connect_once([] {
				if (!ui_alive || !lyricView) return;
				get_tags();
				lyricView->set_justification(get_justification());
			});
			break;
		case DB_EV_TERMINATE:
			death_signal = 1;
			break;
		case DB_EV_SONGSTARTED: {
			auto event = reinterpret_cast<ddb_event_track_t *>(ctx);
			if (!ui_alive || !event || !event->track || deadbeef->pl_get_item_duration(event->track) <= 0.0f)
				return 0;
			death_signal = 0;
			deadbeef->pl_item_ref(event->track);   // released by update_lyrics()
			intptr_t tid = deadbeef->thread_start(update_lyrics, event->track);
			if (tid) deadbeef->thread_detach(tid);
			else deadbeef->pl_item_unref(event->track);
			break;
		}
	}
	return 0;
}

extern "C"
void lyricbar_destroy() {
	// GTK owns and destroys the widgets; deleting the wrappers here caused double frees.
	ui_alive = false;
	death_signal = 1;
	lyricView = nullptr;
	lyricbar = nullptr;
	tagsArtist.clear(); tagPadding.clear(); tagsSyncline.clear(); tagsNosyncline.clear(); tagsTitle.clear();
	tagRegular.reset(); tagLarge.reset(); tagSmall.reset(); tagBold.reset(); tagItalic.reset();
	tagRightmargin.reset(); tagLeftmargin.reset(); tagCenter.reset();
	tagForegroundColorHighlight.reset(); tagForegroundColorRegular.reset();
	refBuffer.reset();
}
