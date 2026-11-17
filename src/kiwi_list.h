// kiwi_list.h - the public KiwiSDR list. Source: rx.linkfanel.net/kiwisdr_com.js, generated from
// kiwisdr.com/public (which now sits behind a captcha). It is a JS array of flat objects whose
// values are all strings; this reads exactly that and nothing more.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace kiwi
{

enum Region { kAll = 0, kNAmerica, kSAmerica, kEurope, kAfrica, kAsia, kOceania, kNumRegions };

struct Receiver
{
    std::string name, loc, host;
    int port = 8073;
    int users = 0, usersMax = 0;     // at the list's last update
    int extApi = -1;                // channels the owner allows clients that are not a browser; -1 not stated
    double lat = 0.0, lon = 0.0;
    double freqOffsetKHz = 0.0;     // downconverter Kiwis tune at freq - offset
    double lowKHz = 0.0, highKHz = 30000.0;
    int region = kAll;
    std::string label;              // what the screen shows
};

inline int regionOf(double lat, double lon)
{
    if (lat > 13 && lon >= -170 && lon <= -50) return kNAmerica;
    if (lat <= 13 && lon >= -95 && lon <= -30) return kSAmerica;
    if (lat >= 34 && lat <= 72 && lon >= -25 && lon < 45) return kEurope;
    if (lat >= -36 && lat < 37 && lon >= -20 && lon < 52 && !(lat > 12 && lon > 34)) return kAfrica;
    if (lat < -10 && (lon >= 110 || lon <= -150)) return kOceania;
    if (lon >= 34 && lat > -11) return kAsia;
    return kAll;
}

// keep ASCII and two-byte UTF-8 (Latin letters); drop the emoji and symbols the MPC font lacks
inline std::string screenText(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 && c >= 0xC2 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 && c <= 0xF4 ? 4 : 1;
        for (int k = 1; k < len; ++k)                          // a broken sequence: one byte at a time
            if (i + (size_t)k >= s.size() || ((unsigned char)s[i + (size_t)k] & 0xC0) != 0x80) { len = 1; break; }
        if (len == 1 && c >= 0x20 && c < 0x7f) o += (char)c;
        else if (len == 2 && i + 1 < s.size()) o.append(s, i, 2);
        i += (size_t)len;
    }
    size_t a = o.find_first_not_of(" |-"), b = o.find_last_not_of(" |-");
    return a == std::string::npos ? std::string() : o.substr(a, b - a + 1);
}

// at most n bytes, never splitting a UTF-8 letter
inline std::string cut(const std::string& s, size_t n)
{
    if (s.size() <= n) return s;
    size_t k = n;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) --k;
    return s.substr(0, k);
}

namespace detail
{
// a JSON string starting at s[i] == '"'; returns the index after the closing quote
inline size_t readString(const std::string& s, size_t i, std::string& out)
{
    out.clear();
    for (++i; i < s.size(); ++i)
    {
        char c = s[i];
        if (c == '"') return i + 1;
        if (c == '\\' && i + 1 < s.size())
        {
            char e = s[++i];
            if (e == 'n' || e == 't' || e == 'r') out += ' ';
            else if (e == 'u') i += 4;             // \uXXXX: dropped, the screen cannot show it anyway
            else out += e;
        }
        else out += c;
    }
    return std::string::npos;
}
} // namespace detail

// every active, online receiver with a usable url and gps fix that takes clients like skywave
inline std::vector<Receiver> parse(const std::string& js)
{
    std::vector<Receiver> list;
    size_t i = js.find('[');
    if (i == std::string::npos) return list;
    while ((i = js.find('{', i)) != std::string::npos)
    {
        size_t end = js.find('}', i);
        if (end == std::string::npos) break;
        Receiver r;
        bool active = false, online = false, gpsOk = false, urlOk = false;
        std::string key, val, bands;
        size_t p = i + 1;
        while (p < end)
        {
            p = js.find('"', p);
            if (p == std::string::npos || p >= end) break;
            p = detail::readString(js, p, key);
            p = js.find('"', p);
            if (p == std::string::npos || p >= end) break;
            p = detail::readString(js, p, val);
            if (key == "name") r.name = screenText(val);
            else if (key == "loc") r.loc = screenText(val);
            else if (key == "status") active = val == "active";
            else if (key == "offline") online = val == "no";
            else if (key == "users") r.users = std::atoi(val.c_str());
            else if (key == "users_max") r.usersMax = std::atoi(val.c_str());
            else if (key == "ext_api") r.extApi = std::atoi(val.c_str());
            else if (key == "freq_offset") r.freqOffsetKHz = std::atof(val.c_str());
            else if (key == "bands") bands = val;
            else if (key == "gps") gpsOk = std::sscanf(val.c_str(), " (%lf , %lf)", &r.lat, &r.lon) == 2 && std::isfinite(r.lat) && std::isfinite(r.lon)
                                           && std::fabs(r.lat) <= 90.0 && std::fabs(r.lon) <= 180.0;
            else if (key == "url")
            {
                std::string u = val;
                if (u.rfind("http://", 0) != 0) continue;
                urlOk = true;
                u = u.substr(7);
                size_t slash = u.find('/');
                if (slash != std::string::npos) u = u.substr(0, slash);
                size_t colon = u.rfind(':');
                r.port = colon == std::string::npos ? 80 : std::atoi(u.c_str() + colon + 1);
                r.host = colon == std::string::npos ? u : u.substr(0, colon);
            }
        }
        size_t dash = bands.find('-');
        if (dash != std::string::npos)
        {
            r.lowKHz = std::atof(bands.c_str()) / 1000.0;
            r.highKHz = std::atof(bands.c_str() + dash + 1) / 1000.0;
        }
        // Left out: a receiver whose owner allows no clients but browsers (ext_api 0; the Kiwi refuses
        // skywave there anyway, it connects as kiwirecorder does), and the downconverter Kiwis, which
        // listen above 30 MHz where the dial does not go.
        const bool welcome = r.extApi != 0 && r.freqOffsetKHz == 0.0;
        if (active && online && gpsOk && urlOk && welcome && !r.host.empty())
        {
            r.region = regionOf(r.lat, r.lon);
            std::string place = !r.loc.empty() ? r.loc : r.name;
            r.label = cut(place, 24);
            list.push_back(std::move(r));
        }
        i = end + 1;
    }
    // west to east, so turning the knob travels across the map
    std::sort(list.begin(), list.end(), [](const Receiver& a, const Receiver& b) { return a.lon < b.lon; });
    return list;
}

} // namespace kiwi
