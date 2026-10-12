#!/bin/sh
# Builds the new Paxsenix / Musixmatch providers and the lyric finder against a fake HTTP layer
# (no GTK, libcurl or network needed) and runs them under ASan/UBSan.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/src/sources" "$tmp/curl"
cp "$here"/src/json.h "$here"/src/lyrics_finder.h "$here"/src/lyrics_finder.cpp "$tmp/src/"
cp "$here"/src/sources/match.h "$here"/src/sources/paxsenix.* "$here"/src/sources/musixmatch.* "$tmp/src/sources/"
cp "$here"/tests/provider_test.cpp "$tmp/test.cpp"
cat > "$tmp/curl/curl.h" <<'H'
#pragma once
struct curl_slist { char *data; curl_slist *next; };
inline curl_slist *curl_slist_append(curl_slist *l, const char *) { return l ? l : new curl_slist{nullptr, nullptr}; }
inline void curl_global_init(long) {}
#define CURL_GLOBAL_DEFAULT 0
H
cat > "$tmp/src/utils.h" <<'H'
#pragma once
#include <string>
#include <vector>
#include "../curl/curl.h"
struct parsed_lyrics { std::string lyrics; bool sync; };
std::string text_downloader(curl_slist *slist, std::string url, std::string post);
std::string urlencode(const std::string &s);
H
cd "$tmp"
g++ -std=c++20 -Wall -Wextra -fsanitize=address,undefined -g -DLYRICFINDER_NO_LEGACY -I. test.cpp \
    src/sources/paxsenix.cpp src/sources/musixmatch.cpp src/lyrics_finder.cpp -pthread -o t
ASAN_OPTIONS=detect_leaks=0 ./t
