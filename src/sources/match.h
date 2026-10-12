#pragma once
// Small helpers shared by the providers ported from kopuz (Paxsenix, Musixmatch).
// Header-only and free of GTK / deadbeef dependencies.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

// One search hit of a provider; `id` is whatever the provider's lyrics endpoint needs.
struct ProviderHit {
	std::string id, artist, title, album;
	double duration = 0;   // seconds, 0 = unknown
};

namespace srcutil {

inline std::string replace_all_copy(std::string s, const std::string &from, const std::string &to) {
	if (from.empty()) return s;
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}

// kopuz `normalized_lyric_match_tokens`: lower-case, "(feat." style openers dropped, split on
// non-alphanumerics (UTF-8 bytes count as alphanumeric so non-Latin titles still compare).
inline std::set<std::string> match_tokens(const std::string &value) {
	std::string v;
	for (unsigned char c : value) v += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
	v = replace_all_copy(v, "(feat.", " ");
	v = replace_all_copy(v, "(ft.", " ");
	v = replace_all_copy(v, "(featuring", " ");
	std::set<std::string> out;
	std::string cur;
	for (unsigned char c : v) {
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
			cur += (char)c;
		} else if (!cur.empty()) {
			out.insert(cur);
			cur.clear();
		}
	}
	if (!cur.empty()) out.insert(cur);
	return out;
}

// kopuz `lyrics_match_score`: Dice coefficient over word sets, 0..100.
inline double match_score(const std::string &candidate, const std::string &query) {
	std::set<std::string> a = match_tokens(candidate), b = match_tokens(query);
	if (a.empty() || b.empty()) return 0;
	size_t shared = 0;
	for (const std::string &t : a)
		if (b.count(t)) shared++;
	return (2.0 * (double)shared) * 100.0 / (double)(a.size() + b.size());
}

// "3:45" / "1:02:03" -> seconds, -1 if malformed.
inline double parse_colon_duration(const std::string &s) {
	if (s.empty()) return -1;
	double total = 0;
	size_t i = 0;
	while (true) {
		size_t j = i;
		while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
		if (j == i || j - i > 6) return -1;
		total = total * 60 + std::atof(s.substr(i, j - i).c_str());
		if (j == s.size()) return total;
		if (s[j] != ':') return -1;
		i = j + 1;
	}
}

// 12.345 -> "00:12.35" (no brackets)
inline std::string lrc_time(double secs) {
	if (secs < 0) secs = 0;
	long cs = std::lround(secs * 100.0);
	char buf[32];
	std::snprintf(buf, sizeof buf, "%02ld:%02ld.%02ld", cs / 6000, (cs / 100) % 60, cs % 100);
	return buf;
}

inline std::string trim_copy(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

// Number of lines starting with an "[m:ss" / "[mm:ss" timestamp.
inline int timestamped_lines(const std::string &text) {
	int n = 0;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		size_t i = start;
		while (i < end && (text[i] == ' ' || text[i] == '\t')) i++;
		if (i < end && text[i] == '[') {
			size_t j = i + 1, digits = 0;
			while (j < end && text[j] >= '0' && text[j] <= '9') { j++; digits++; }
			if (digits > 0 && j < end && text[j] == ':') n++;
		}
		start = end + 1;
	}
	return n;
}

}  // namespace srcutil
