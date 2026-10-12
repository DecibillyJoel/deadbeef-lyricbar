#pragma once
// Tiny forgiving JSON parser. Never throws and never reads out of bounds:
// malformed input yields a Null value, and every accessor on a missing member
// returns Null, so provider code can chain lookups like v["data"]["song"]["list"][0].
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace mj {

struct Value {
	enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Value> a;            // array items
	std::vector<std::string> keys;   // object keys (parallel to vals)
	std::vector<Value> vals;         // object values

	static const Value &null() { static const Value v; return v; }

	const Value &operator[](const char *key) const {
		if (type == Obj)
			for (size_t i = 0; i < keys.size(); i++)
				if (keys[i] == key) return vals[i];
		return null();
	}
	const Value &operator[](size_t i) const { return (type == Arr && i < a.size()) ? a[i] : null(); }

	size_t size() const { return type == Arr ? a.size() : type == Obj ? vals.size() : 0; }
	bool is_null() const { return type == Null; }
	bool truthy() const { return type == Bool && b; }

	std::string str(const std::string &def = "") const {
		if (type == Str) return s;
		if (type == Num) { char buf[40]; snprintf(buf, sizeof buf, "%.0f", n); return buf; }
		return def;
	}
	double num(double def = 0) const {
		if (type == Num) return n;
		if (type == Str) {
			char *e = nullptr;
			double v = strtod(s.c_str(), &e);
			if (e != s.c_str()) return v;
		}
		return def;
	}
};

namespace detail {

struct Parser {
	const char *p, *e;
	bool ok = true;

	void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }

	static void put_utf8(std::string &o, unsigned cp) {
		if (cp < 0x80) o += (char)cp;
		else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
		else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
		else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
	}

	bool hex4(unsigned &out) {
		if (e - p < 4) return false;
		unsigned v = 0;
		for (int i = 0; i < 4; i++) {
			char c = p[i];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
			else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
			else return false;
		}
		p += 4;
		out = v;
		return true;
	}

	bool string_(std::string &out) {
		if (p >= e || *p != '"') return false;
		p++;
		while (p < e && *p != '"') {
			char c = *p++;
			if (c != '\\') { out += c; continue; }
			if (p >= e) return false;
			char x = *p++;
			switch (x) {
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'u': {
					unsigned cp;
					if (!hex4(cp)) return false;
					if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
						const char *save = p;
						p += 2;
						unsigned lo;
						if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						else p = save;
					}
					if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;   // lone surrogate -> U+FFFD, not CESU-8
					put_utf8(out, cp);
					break;
				}
				default: out += x; break;   // \" \\ \/
			}
		}
		if (p >= e) return false;
		p++;   // closing quote
		return true;
	}

	bool value(Value &v, int depth) {
		if (depth > 64) return false;
		ws();
		if (p >= e) return false;
		char c = *p;
		if (c == '{') {
			p++;
			v.type = Value::Obj;
			ws();
			if (p < e && *p == '}') { p++; return true; }
			while (true) {
				ws();
				std::string k;
				if (!string_(k)) return false;
				ws();
				if (p >= e || *p != ':') return false;
				p++;
				Value child;
				if (!value(child, depth + 1)) return false;
				v.keys.push_back(std::move(k));
				v.vals.push_back(std::move(child));
				ws();
				if (p < e && *p == ',') { p++; continue; }
				if (p < e && *p == '}') { p++; return true; }
				return false;
			}
		}
		if (c == '[') {
			p++;
			v.type = Value::Arr;
			ws();
			if (p < e && *p == ']') { p++; return true; }
			while (true) {
				Value child;
				if (!value(child, depth + 1)) return false;
				v.a.push_back(std::move(child));
				ws();
				if (p < e && *p == ',') { p++; continue; }
				if (p < e && *p == ']') { p++; return true; }
				return false;
			}
		}
		if (c == '"') { v.type = Value::Str; return string_(v.s); }
		if (e - p >= 4 && std::string(p, 4) == "true") { p += 4; v.type = Value::Bool; v.b = true; return true; }
		if (e - p >= 5 && std::string(p, 5) == "false") { p += 5; v.type = Value::Bool; v.b = false; return true; }
		if (e - p >= 4 && std::string(p, 4) == "null") { p += 4; return true; }
		// number
		char *endp = nullptr;
		std::string tmp(p, (size_t)std::min<ptrdiff_t>(e - p, 64));
		double d = strtod(tmp.c_str(), &endp);
		if (endp == tmp.c_str()) return false;
		p += (endp - tmp.c_str());
		v.type = Value::Num;
		v.n = d;
		return true;
	}
};

}  // namespace detail

// Parses `text` (a JSONP wrapper or leading junk before the first { or [ is skipped).
inline Value parse(const std::string &text) {
	size_t start = text.find_first_of("{[");
	if (start == std::string::npos) return Value();
	detail::Parser ps{text.data() + start, text.data() + text.size()};
	Value v;
	if (!ps.value(v, 0)) return Value();
	return v;
}

}  // namespace mj
