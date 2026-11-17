// Skywave.cpp - shortwave radio on a pad. Plays any public KiwiSDR receiver (0 to 30 MHz, about
// 600 of them around the world) as an MPC instrument, and GRAB writes the last few seconds you
// heard as a WAV on the drive, ready for the sampler.
//
//   Region, Receiver   browse the public list; receivers run west to east within a region
//   Station            jumps the dial to a known signal (time stations, the Buzzer, VOLMET, ...)
//   MHz, kHz, Fine     the dial; Mode is AM, LSB, USB, CW or NBFM
//   Listen             takes one listening slot on the receiver; off gives it back
//   Grab               every tap saves the last Grab Len seconds to <drive>/Skywave Grabs/
//   SCOPE (F2)         the dial's neighbourhood from the receiver's waterfall; seek, labels
//   CLEAN (F3)         passband width and shift, AGC, a noise blanker, an auto notch, denoise
//   LIST (F4)          stations, the receiver's labels, the receivers, or Live: what this receiver
//                      hears right now, from one sweep of its waterfall; eight rows, tap one to go
//   Back               the last place you stayed three seconds or more, and the one before
//   Dig                somewhere on the air at random: from Live, else a label, else a station
//   the SIGNAL lamp    lit when the dial stands 10 dB over the band's noise
//   HELP (F5)          every page with its controls named
//
// Threads: a worker owns the network (list download, the Kiwi websocket, keepalives, grabs).
// It cleans the receiver's audio (about 12 kHz, mono) and pushes it into a ring; process()
// resamples it to the host rate with a small jitter buffer whose speed trims itself to the
// receiver's clock.
//
// Manners, because every receiver is somebody's hardware and its slots are few: one slot,
// introduced as skywave-mpc; receivers whose owners allow only browsers are left out of the list;
// a receiver that ends the session (a time limit, an admin) is not called again until Listen is
// switched off and on; a call that fails waits longer each time (5, 10, 20, 40 s) and gives up
// after five; the knob must rest a second on a receiver before it is called; the waterfall is
// opened only when something needs it (the scope, labels, a Live sweep) and sends nothing while
// unused; and Listen switches itself off after 30 minutes in which nothing was touched.

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"

#include "clean.h"
#include "kiwi_list.h"
#include "net.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <sys/stat.h>

#ifdef _WIN32
  #include <direct.h>
  #define mkdirOne(p) _mkdir(p)
#else
  #define mkdirOne(p) mkdir(p, 0775)
#endif

using namespace mpc::dsp;

namespace
{
struct Station { const char* name; double kHz; int mode; int cat; };
enum Mode { kAM = 0, kLSB, kUSB, kCW, kNBFM };
const char* const kModeName[] = {"am", "lsb", "usb", "cw", "nbfm"};
const char* const kModeShown[] = {"AM", "LSB", "USB", "CW", "NBFM"};
const char* const kRegionNames[] = {"All", "N America", "S America", "Europe", "Africa", "Asia", "Oceania"};

// passbands in Hz round the tuned frequency, Narrow, Normal and Wide; the Kiwi's audio is about
// 12 kHz, so 6 kHz either side is the most there is
const int kPass[5][3][2] = {
    {{-2500, 2500}, {-4900, 4900}, {-5900, 5900}},   // AM
    {{-2100, -300}, {-2700, -300}, {-3600, -100}},   // LSB
    {{300, 2100}, {300, 2700}, {100, 3600}},         // USB
    {{400, 600}, {300, 700}, {150, 1000}},           // CW, round the 500 Hz pitch
    {{-5900, 5900}, {-5900, 5900}, {-5900, 5900}},   // NBFM: its deviation sets the width
};
// a CW carrier is heard as a 500 Hz tone: the receiver tunes this far under it, as a Kiwi's own
// page does, so the dial reads the signal's frequency
constexpr double kCwPitchKHz = 0.5;
const int kAgcDecayMs[] = {2500, 1000, 300};   // Slow, Medium, Fast; the fourth choice is Off

// Category is the filter: Station turns through its presets, LIST shows them and its part of Live,
// Dig stays in it; Anything lets everything through. Fax is
// tuned 1.9 kHz under its carrier in USB, so the black and white tones fall at 1.5 to 2.3 kHz.
// Broadcasters move with the seasons; the meter bands always have someone in them.
enum Category { kCatTime, kCatBroadcast, kCatAero, kCatMarine, kCatOdd, kCatHamVoice, kCatHamDigital, kCatBeacon,
                kCatVlf, kCatCb, kNumCats };
// named for how they sound, not who sends them: a sampler's question
const char* const kCatNames[kNumCats] = {"Ticks & Time", "Talk & Music", "Robots & Pilots", "Fax & Teletype", "Buzzers & Odd",
                                         "Ham Chatter", "Chirps & Data", "Morse Beacons", "Drones (VLF)", "CB & Pirates"};
constexpr int kCatAny = kNumCats;           // "Anything", the choice after the last: no filter
inline const char* catName(int c) { return c >= 0 && c < kNumCats ? kCatNames[c] : "Anything"; }

const Station kStations[] = {
    {"Dial", 0.0, kAM, -1},
    // time and frequency
    {"WWV 2.5", 2500.0, kAM, kCatTime}, {"WWV 5", 5000.0, kAM, kCatTime}, {"WWV 10", 10000.0, kAM, kCatTime},
    {"WWV 15", 15000.0, kAM, kCatTime}, {"WWV 20", 20000.0, kAM, kCatTime}, {"WWV 25", 25000.0, kAM, kCatTime},
    {"CHU 3330", 3330.0, kAM, kCatTime}, {"CHU 7850", 7850.0, kAM, kCatTime}, {"CHU 14670", 14670.0, kAM, kCatTime},
    {"RWM 4996", 4996.0, kCW, kCatTime}, {"RWM 9996", 9996.0, kCW, kCatTime}, {"RWM 14996", 14996.0, kCW, kCatTime},
    {"BPM 10", 10000.0, kAM, kCatTime},
    // broadcast: the meter bands, then stations that have stayed put
    {"90m band", 3300.0, kAM, kCatBroadcast}, {"75m band", 3950.0, kAM, kCatBroadcast}, {"60m band", 4900.0, kAM, kCatBroadcast},
    {"49m band", 6000.0, kAM, kCatBroadcast}, {"41m band", 7350.0, kAM, kCatBroadcast}, {"31m band", 9650.0, kAM, kCatBroadcast},
    {"25m band", 11850.0, kAM, kCatBroadcast}, {"22m band", 13700.0, kAM, kCatBroadcast}, {"19m band", 15400.0, kAM, kCatBroadcast},
    {"16m band", 17700.0, kAM, kCatBroadcast}, {"13m band", 21600.0, kAM, kCatBroadcast}, {"11m band", 25800.0, kAM, kCatBroadcast},
    {"MW 540", 540.0, kAM, kCatBroadcast}, {"MW 1000", 1000.0, kAM, kCatBroadcast}, {"MW 1500", 1500.0, kAM, kCatBroadcast},
    {"WRMI 5950", 5950.0, kAM, kCatBroadcast}, {"WRMI 7730", 7730.0, kAM, kCatBroadcast}, {"WRMI 9395", 9395.0, kAM, kCatBroadcast},
    {"WRMI 15770", 15770.0, kAM, kCatBroadcast}, {"WWCR 4840", 4840.0, kAM, kCatBroadcast}, {"WWCR 5890", 5890.0, kAM, kCatBroadcast},
    {"WWCR 9350", 9350.0, kAM, kCatBroadcast}, {"R Havana 6000", 6000.0, kAM, kCatBroadcast}, {"R Havana 6165", 6165.0, kAM, kCatBroadcast},
    // aviation
    {"Shannon VOL 3413", 3413.0, kUSB, kCatAero}, {"Shannon VOL 5505", 5505.0, kUSB, kCatAero}, {"Shannon VOL 8957", 8957.0, kUSB, kCatAero},
    {"Shannon VOL 13264", 13264.0, kUSB, kCatAero}, {"NY VOLMET 3485", 3485.0, kUSB, kCatAero}, {"NY VOLMET 6604", 6604.0, kUSB, kCatAero},
    {"NY VOLMET 10051", 10051.0, kUSB, kCatAero}, {"NY VOLMET 13270", 13270.0, kUSB, kCatAero}, {"RAF VOLMET 5450", 5450.0, kUSB, kCatAero},
    {"RAF VOLMET 11253", 11253.0, kUSB, kCatAero}, {"NAT-A 5598", 5598.0, kUSB, kCatAero}, {"NAT-A 8906", 8906.0, kUSB, kCatAero},
    {"NAT-B 8864", 8864.0, kUSB, kCatAero}, {"NAT-C 8879", 8879.0, kUSB, kCatAero},
    // marine, weather fax and RTTY
    {"USCG wx 4426", 4426.0, kUSB, kCatMarine}, {"USCG wx 6501", 6501.0, kUSB, kCatMarine}, {"USCG wx 8764", 8764.0, kUSB, kCatMarine},
    {"USCG wx 13089", 13089.0, kUSB, kCatMarine}, {"Boston fax 4235", 4233.1, kUSB, kCatMarine}, {"Boston fax 6340", 6338.6, kUSB, kCatMarine},
    {"Boston fax 9110", 9108.1, kUSB, kCatMarine}, {"Boston fax 12750", 12748.1, kUSB, kCatMarine},
    {"Pt Reyes fax 8682", 8680.1, kUSB, kCatMarine}, {"Pt Reyes fax 12786", 12784.1, kUSB, kCatMarine},
    {"N Orleans fax 8504", 8502.0, kUSB, kCatMarine}, {"N Orleans fax 12790", 12788.0, kUSB, kCatMarine},
    {"DWD fax 3855", 3853.1, kUSB, kCatMarine}, {"DWD fax 7880", 7878.1, kUSB, kCatMarine}, {"DWD fax 13882", 13880.6, kUSB, kCatMarine},
    {"Tokyo fax 7795", 7793.1, kUSB, kCatMarine}, {"Tokyo fax 13988", 13986.6, kUSB, kCatMarine},
    {"DWD RTTY 4583", 4581.6, kUSB, kCatMarine}, {"DWD RTTY 7646", 7644.6, kUSB, kCatMarine}, {"DWD RTTY 10101", 10100.8, kUSB, kCatMarine},
    // military and the strange ones
    {"The Buzzer", 4625.0, kUSB, kCatOdd}, {"The Pip night", 3756.0, kUSB, kCatOdd}, {"The Pip day", 5448.0, kUSB, kCatOdd},
    {"Squeaky Wheel n", 3828.0, kUSB, kCatOdd}, {"Squeaky Wheel d", 5473.0, kUSB, kCatOdd}, {"HFGCS 4724", 4724.0, kUSB, kCatOdd},
    {"HFGCS 6739", 6739.0, kUSB, kCatOdd}, {"HFGCS 8992", 8992.0, kUSB, kCatOdd}, {"HFGCS 11175", 11175.0, kUSB, kCatOdd},
    {"HFGCS 13200", 13200.0, kUSB, kCatOdd}, {"HFGCS 15016", 15016.0, kUSB, kCatOdd},
    // ham voice
    {"80m 3850", 3850.0, kLSB, kCatHamVoice}, {"80m AM 3885", 3885.0, kAM, kCatHamVoice}, {"60m 5330.5", 5330.5, kUSB, kCatHamVoice},
    {"40m 7175", 7175.0, kLSB, kCatHamVoice}, {"40m 7200", 7200.0, kLSB, kCatHamVoice}, {"40m AM 7290", 7290.0, kAM, kCatHamVoice},
    {"20m 14200", 14200.0, kUSB, kCatHamVoice}, {"20m 14250", 14250.0, kUSB, kCatHamVoice}, {"20m MMSN 14300", 14300.0, kUSB, kCatHamVoice},
    {"17m 18130", 18130.0, kUSB, kCatHamVoice}, {"15m 21300", 21300.0, kUSB, kCatHamVoice}, {"12m 24950", 24950.0, kUSB, kCatHamVoice},
    {"10m 28400", 28400.0, kUSB, kCatHamVoice}, {"10m FM 29600", 29600.0, kNBFM, kCatHamVoice},
    // ham digital and Morse
    {"FT8 160m", 1840.0, kUSB, kCatHamDigital}, {"FT8 80m", 3573.0, kUSB, kCatHamDigital}, {"FT8 60m", 5357.0, kUSB, kCatHamDigital},
    {"FT8 40m", 7074.0, kUSB, kCatHamDigital}, {"FT8 30m", 10136.0, kUSB, kCatHamDigital}, {"FT8 20m", 14074.0, kUSB, kCatHamDigital},
    {"FT8 17m", 18100.0, kUSB, kCatHamDigital}, {"FT8 15m", 21074.0, kUSB, kCatHamDigital}, {"FT8 12m", 24915.0, kUSB, kCatHamDigital},
    {"FT8 10m", 28074.0, kUSB, kCatHamDigital}, {"FT4 20m", 14080.0, kUSB, kCatHamDigital}, {"WSPR 40m", 7038.6, kUSB, kCatHamDigital},
    {"WSPR 30m", 10138.7, kUSB, kCatHamDigital}, {"WSPR 20m", 14095.6, kUSB, kCatHamDigital}, {"PSK31 20m", 14070.0, kUSB, kCatHamDigital},
    {"SSTV 20m", 14230.0, kUSB, kCatHamDigital}, {"Morse 80m", 3530.0, kCW, kCatHamDigital}, {"Morse 40m", 7030.0, kCW, kCatHamDigital},
    {"Morse 20m", 14025.0, kCW, kCatHamDigital},
    // the NCDXF beacons: 18 stations round the world, one every 10 s, each band in turn
    {"NCDXF 14100", 14100.0, kCW, kCatBeacon}, {"NCDXF 18110", 18110.0, kCW, kCatBeacon}, {"NCDXF 21150", 21150.0, kCW, kCatBeacon},
    {"NCDXF 24930", 24930.0, kCW, kCatBeacon}, {"NCDXF 28200", 28200.0, kCW, kCatBeacon},
    // very low and low frequency: navy transmitters, time codes, the Russian Alpha
    {"NAA 24.0 Maine", 24.0, kCW, kCatVlf}, {"NLK 24.8 Wash.", 24.8, kCW, kCatVlf}, {"NML 25.2 N Dak.", 25.2, kCW, kCatVlf},
    {"NWC 19.8 Austr.", 19.8, kCW, kCatVlf}, {"NPM 21.4 Hawaii", 21.4, kCW, kCatVlf}, {"DHO38 23.4 Ger.", 23.4, kCW, kCatVlf},
    {"GQD 19.6 UK", 19.6, kCW, kCatVlf}, {"JXN 16.4 Norway", 16.4, kCW, kCatVlf}, {"Alpha 11.9", 11.9, kCW, kCatVlf},
    {"Alpha 14.88", 14.88, kCW, kCatVlf}, {"WWVB 60", 60.0, kCW, kCatVlf}, {"MSF 60", 60.0, kCW, kCatVlf},
    {"JJY 40", 40.0, kCW, kCatVlf}, {"DCF77 77.5", 77.5, kCW, kCatVlf}, {"TDF 162", 162.0, kAM, kCatVlf},
    // citizens band and the weekend pirates
    {"CB ch 9", 27065.0, kAM, kCatCb}, {"CB ch 19", 27185.0, kAM, kCatCb}, {"CB 27555 USB", 27555.0, kUSB, kCatCb},
    {"Pirates 6925", 6925.0, kAM, kCatCb}, {"Pirates 6930 USB", 6930.0, kUSB, kCatCb},
};
constexpr int kNumStations = (int)(sizeof(kStations) / sizeof(kStations[0]));
inline bool stationIn(int k, int c) { return k > 0 && k < kNumStations && (c == kCatAny || kStations[k].cat == c); }

// where a category is on the air: its bands, and a couple of kHz round each of its presets
bool freqIn(double f, int c)
{
    if (c == kCatAny) return true;
    struct R { int cat; double lo, hi; };
    static const R kR[] = {
        {kCatBroadcast, 150, 285}, {kCatBroadcast, 520, 1710}, {kCatBroadcast, 2300, 2495}, {kCatBroadcast, 3200, 3400},
        {kCatBroadcast, 3900, 4000}, {kCatBroadcast, 4750, 5060}, {kCatBroadcast, 5900, 6200}, {kCatBroadcast, 7300, 7450},
        {kCatBroadcast, 9400, 9900}, {kCatBroadcast, 11600, 12100}, {kCatBroadcast, 13570, 13870}, {kCatBroadcast, 15100, 15800},
        {kCatBroadcast, 17480, 17900}, {kCatBroadcast, 18900, 19020}, {kCatBroadcast, 21450, 21850}, {kCatBroadcast, 25670, 26100},
        {kCatAero, 2850, 3155}, {kCatAero, 3400, 3500}, {kCatAero, 4650, 4750}, {kCatAero, 5450, 5730}, {kCatAero, 6525, 6765},
        {kCatAero, 8815, 9040}, {kCatAero, 10005, 10100}, {kCatAero, 11175, 11400}, {kCatAero, 13200, 13360}, {kCatAero, 17900, 18030},
        {kCatHamVoice, 1843, 2000}, {kCatHamVoice, 3600, 3900}, {kCatHamVoice, 5330, 5410}, {kCatHamVoice, 7125, 7300},
        {kCatHamVoice, 14150, 14350}, {kCatHamVoice, 18110, 18168}, {kCatHamVoice, 21200, 21450}, {kCatHamVoice, 24930, 24990},
        {kCatHamVoice, 28300, 29700},
        {kCatHamDigital, 1800, 1843}, {kCatHamDigital, 3500, 3600}, {kCatHamDigital, 7000, 7125}, {kCatHamDigital, 10100, 10150},
        {kCatHamDigital, 14000, 14150}, {kCatHamDigital, 18068, 18110}, {kCatHamDigital, 21000, 21200}, {kCatHamDigital, 24890, 24930},
        {kCatHamDigital, 28000, 28300},
        {kCatVlf, 10, 150}, {kCatCb, 26960, 27410}, {kCatCb, 6900, 7000}};
    for (const R& r : kR)
        if (r.cat == c && f >= r.lo && f < r.hi) return true;
    for (int k = 1; k < kNumStations; ++k)
        if (kStations[k].cat == c && std::fabs(kStations[k].kHz - f) <= 2.0) return true;
    return false;
}

// what a preset sounds like, a word or two for the list and the status
const char* soundOf(int k)
{
    if (k <= 0 || k >= kNumStations) return "";
    const Station& s = kStations[k];
    auto has = [&](const char* t) { return std::strstr(s.name, t) != nullptr; };
    switch (s.cat)
    {
        case kCatTime: return has("RWM") ? "beeps" : has("BPM") ? "ticks" : "ticks, voice";
        case kCatBroadcast: return has("band") || has("MW") ? "voices, music" : has("Havana") ? "music, talk" : "talk, music";
        case kCatAero: return has("NAT") ? "pilots" : "robot voice";
        case kCatMarine: return has("fax") ? "scrape" : has("RTTY") ? "warble" : "robot voice";
        case kCatOdd: return has("Buzzer") ? "buzz" : has("Pip") ? "pips" : has("Squeaky") ? "squeaks" : "codes, voice";
        case kCatHamVoice: return "chatter";
        case kCatHamDigital: return has("FT") ? "chirps" : has("WSPR") ? "whistles" : has("PSK") ? "warble" : has("SSTV") ? "screech" : "beeps";
        case kCatBeacon: return "beeps";
        case kCatVlf: return has("Alpha") ? "beeps" : has("TDF") ? "tone" : has("60") || has("40") || has("77") ? "pulses" : "drone";
        case kCatCb: return has("Pirate") ? "music" : "chatter";
        default: return "";
    }
}
const int kGrabSecs[] = {5, 10, 20, 30};
const int kGrabBars[] = {1, 2, 4, 8};       // appended after the seconds: at the project's tempo
// the scope's spans: Kiwi waterfall zooms, 30 MHz over 2^zoom; 14 is the Kiwi's closest, 1.8 kHz
const int kSpanZoom[] = {14, 13, 12, 11, 10, 9, 8, 7, 6, 5};
constexpr int kSpanDefault = 5;    // 59 kHz
// Tune's step: a radio's tuning rates, and the broadcast channel grids (9 kHz MW in Europe, Africa and
// Asia, 10 kHz in the Americas, 5 kHz on shortwave), which a step lands on, as a radio's does
const int kStepHz[] = {10, 50, 100, 500, 1000, 5000, 9000, 10000, 100000, 1000000};
const char* const kStepNames[] = {"10 Hz", "50 Hz", "100 Hz", "500 Hz", "1 kHz", "5 kHz", "9 kHz", "10 kHz", "100 kHz", "1 MHz"};
constexpr int kNumSteps = (int)(sizeof(kStepHz) / sizeof(kStepHz[0]));
constexpr int kStepDefault = 4;    // 1 kHz
constexpr int kNumSpans = (int)(sizeof(kSpanZoom) / sizeof(kSpanZoom[0]));
inline double spanKHz(int zoom, double fullKHz = 30000.0) { return fullKHz / (double)(1 << std::clamp(zoom, 0, 14)); }

// IMA ADPCM, as the Kiwi sends it with compression on: low nibble first
struct Adpcm
{
    int index = 0, prev = 0;
    void reset() { index = prev = 0; }
    int16_t decode(int code)
    {
        static const int kStep[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
            80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
            796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
            5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
            27086, 29794, 32767};
        static const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
        const int step = kStep[index];
        int diff = step >> 3;
        if (code & 1) diff += step >> 2;
        if (code & 2) diff += step >> 1;
        if (code & 4) diff += step;
        if (code & 8) diff = -diff;
        prev = std::max(-32768, std::min(32767, prev + diff));
        index = std::max(0, std::min(88, index + kIndex[code & 15]));
        return (int16_t)prev;
    }
};

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::seconds;
} // namespace

class Skywave : public mpc::Plugin
{
public:
    // Order and ids are permanent once a project uses the plugin; append new ones at the end.
    enum Params { kLevel = 0, kListen, kRegion, kReceiver, kStation, kMHz, kKHz, kFine, kMode, kGrab, kGrabLen, kStatus,
                  kDial, kSignal, kLon, kLat, kLink, kCategory,
                  kSeekDown, kSeekUp, kSeekBy, kSpan, kLabel, kScopeLo, kScopeHi, kBin0,
                  kBandwidth = kBin0 + 64, kShift, kAgc, kGain, kBlanker, kNotch, kDenoise,
                  kPassLo, kPassHi, kBlanking, kNotching, kRemoved,
                  kList, kScroll, kPageUp, kPageDown, kBack, kDig, kRow0, kLamp = kRow0 + 8, kWheel,
                  kStep, kTune, kTunePos, kScale, kScopePassLo, kScopePassHi, kPeak, kWf0,
                  kNumParams = kWf0 + 12 * 64 };
    static constexpr int kBins = 64;                          // the scope's bars
    static constexpr int kWfRows = 12, kWfCols = kBins;       // the waterfall under them, the newest row on top
    static constexpr int kWfLevels = 12;                      // its cells' shades, the film strip's frames
    static_assert(kNumParams == kWf0 + kWfRows * kWfCols, "a parameter for every waterfall cell");
    static constexpr int kWheelMid = 16;
    static constexpr int kRows = 8;                           // the list page's rows
    enum ListKind { kListStations, kListLabels, kListReceivers, kListLive };
    enum Link { kLinkOff, kLinkCalling, kLinkOnAir, kLinkFault };

    static constexpr int kSrcBits = 17;                       // 131072 samples, 10.9 s at 12 kHz
    static constexpr int64_t kSrcLen = (int64_t)1 << kSrcBits;
    static constexpr int kGrabRingBits = 21;                  // 2.1 M samples, 47 s at 44.1 kHz
    static constexpr int64_t kGrabRingLen = (int64_t)1 << kGrabRingBits;
    static constexpr double kBufferSecs = 0.35;               // jitter buffer target

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Skywave";
        i.vendor = "databurn";
        i.uniqueId = MPC_FOURCC('d', 'b', 'S', 'k');   // == "uid" in plugin.json
        i.version = 602;   // 0.6.2
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Skywave() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        std::vector<std::string> regions(kRegionNames, kRegionNames + kiwi::kNumRegions);
        std::vector<std::string> stations, modes(kModeShown, kModeShown + 5), lens;
        for (const Station& s : kStations) stations.push_back(s.name);
        for (int s : kGrabSecs) lens.push_back(std::to_string(s) + " s");
        for (int b : kGrabBars) lens.push_back(std::to_string(b) + (b == 1 ? " bar" : " bars"));
        // read-outs for the skin: the plugin writes them, nobody automates them
        auto readout = [&](mpc::Param p) { p.automatable = false; addParam(p); };
        addParam(floatParam("level", "Level", -24.0f, 12.0f, 0.0f, "dB", 1));   // parameter 0: the host nudges it on load
        addParam(boolParam("listen", "Listen", false));
        addParam(choiceParam("region", "Region", regions, kiwi::kNAmerica));
        addParam(intParam("receiver", "Receiver", 0, 1023, 0));
        addParam(choiceParam("station", "Station", stations, 3));
        addParam(intParam("mhz", "MHz", 0, 29, 10, "MHz"));
        addParam(intParam("khz", "kHz", 0, 999, 0, "kHz"));                // whole kHz: a Q-Link detent is 1 kHz
        addParam(floatParam("fine", "Fine", -1.0f, 1.0f, 0.0f, "kHz", 2));   // and Fine the rest, to 10 Hz
        addParam(choiceParam("mode", "Mode", modes, kAM));
        addParam(boolParam("grab", "Grab", false));
        addParam(choiceParam("grablen", "Grab Len", lens, 1));
        readout(intParam("status", "Status", 0, 1, 0));
        readout(floatParam("dial", "Dial", 0.0f, 1.0f, 0.0f, "", 3));       // the frequency as a share of 0 to 30 MHz
        readout(floatParam("signal", "Signal", 0.0f, 1.0f, 0.0f, "", 2));   // S0 (-127 dBm) to S9+40 (-33 dBm)
        readout(floatParam("lon", "Lon", 0.0f, 1.0f, 0.5f, "", 3));         // the receiver, -180 to 180
        readout(floatParam("lat", "Lat", 0.0f, 1.0f, 0.5f, "", 3));         // -90 to 90
        readout(choiceParam("link", "Link", {"Off", "Calling", "On Air", "Fault"}, kLinkOff));
        std::vector<std::string> cats(kCatNames, kCatNames + kNumCats);
        cats.push_back(catName(kCatAny));
        addParam(choiceParam("category", "Category", cats, kCatAny));
        // the scope page: seek, the receiver's own labels, a spectrum of the dial's neighbourhood
        addParam(boolParam("seekdown", "Seek Down", false));
        addParam(boolParam("seekup", "Seek Up", false));
        addParam(choiceParam("seekby", "Seek By", {"Signals", "Labels"}, 0));
        std::vector<std::string> spans;
        for (int z : kSpanZoom) { char b[16]; std::snprintf(b, sizeof(b), "%.0f kHz", spanKHz(z)); spans.push_back(b); }
        addParam(choiceParam("span", "Span", spans, kSpanDefault));
        addParam(intParam("label", "Label", 0, 1023, 0));
        readout(floatParam("scopelo", "Scope Lo", 0.0f, 1.0f, 0.0f, "", 4));
        readout(floatParam("scopehi", "Scope Hi", 0.0f, 1.0f, 0.0f, "", 4));
        for (int b = 0; b < kBins; ++b)
            readout(floatParam(("bin" + std::to_string(b)).c_str(), ("Bin " + std::to_string(b + 1)).c_str(), 0.0f, 1.0f, 0.0f, "", 2));
        // the clean page: what the receiver sends (passband, AGC), then what we do to it here
        addParam(choiceParam("bandwidth", "Bandwidth", {"Narrow", "Normal", "Wide"}, 1));
        addParam(floatParam("shift", "Shift", -1000.0f, 1000.0f, 0.0f, "Hz", 0));
        addParam(choiceParam("agc", "AGC", {"Slow", "Medium", "Fast", "Off"}, 1));
        addParam(floatParam("gain", "Gain", 0.0f, 120.0f, 50.0f, "dB", 0));   // the receiver's gain with AGC off
        addParam(floatParam("blanker", "Blanker", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("notch", "Notch", {"Off", "Auto"}, 0));
        addParam(floatParam("denoise", "Denoise", 0.0f, 100.0f, 0.0f, "%", 0));
        readout(floatParam("passlo", "Pass Lo", 0.0f, 1.0f, 0.525f, "", 4));   // the passband's edges, -6 to +6 kHz
        readout(floatParam("passhi", "Pass Hi", 0.0f, 1.0f, 0.725f, "", 4));
        readout(choiceParam("blanking", "Blanking", {"Off", "On"}, 0));
        readout(choiceParam("notching", "Notching", {"Off", "On"}, 0));
        readout(floatParam("removed", "Removed", 0.0f, 1.0f, 0.0f, "", 2));     // what the cleanup takes out, 0 to 20 dB
        // the list page and the two ways out of a dead end: back where you were, or somewhere new
        addParam(choiceParam("list", "List", {"Stations", "Labels", "Receivers", "Live"}, kListStations));
        addParam(intParam("scroll", "Scroll", 0, 1023, 0));                   // the top row; its text is where you are in the list
        addParam(boolParam("pageup", "Page Up", false));
        addParam(boolParam("pagedown", "Page Down", false));
        addParam(boolParam("back", "Back", false));
        addParam(boolParam("dig", "Dig", false));
        for (int r = 0; r < kRows; ++r)                                        // a tap flips one; its text is the entry
            readout(boolParam(("row" + std::to_string(r)).c_str(), ("Row " + std::to_string(r + 1)).c_str(), false));
        readout(choiceParam("lamp", "Lamp", {"Off", "On"}, 0));             // something over the band's noise here
        std::vector<std::string> steps;                                        // the data wheel's steps: one row each
        for (int k = 0; k <= 2 * kWheelMid; ++k) steps.push_back(std::to_string(k - kWheelMid));
        readout(choiceParam("wheel", "Wheel", steps, kWheelMid));
        // tuning: Tune is a jog (a step of Step a turn, back to the middle after each), so a tap on it
        // only selects it and the data wheel or its Q-Link tunes; Tune Pos turns its knob as it goes
        addParam(choiceParam("step", "Step", std::vector<std::string>(kStepNames, kStepNames + kNumSteps), kStepDefault));
        addParam(choiceParam("tune", "Tune", steps, kWheelMid));
        readout(floatParam("tunepos", "Tune Pos", 0.0f, 1.0f, 0.0f, "", 3));
        // the scope: how the bars are scaled, the passband's edges over it, the peak nearest the dial, the waterfall
        addParam(choiceParam("scale", "Scale", {"Auto", "20 dB", "40 dB", "60 dB"}, 0));
        readout(floatParam("scopepasslo", "Scope Pass Lo", 0.0f, 1.0f, 0.5f, "", 4));
        readout(floatParam("scopepasshi", "Scope Pass Hi", 0.0f, 1.0f, 0.5f, "", 4));
        readout(floatParam("peak", "Peak", 0.0f, 1.0f, 0.0f, "", 0));        // its text; the value flips to redraw it
        for (int r = 0; r < kWfRows; ++r)
            for (int c = 0; c < kWfCols; ++c)
                readout(floatParam(("wf" + std::to_string(r) + "_" + std::to_string(c)).c_str(),
                                   ("WF " + std::to_string(r + 1) + "." + std::to_string(c + 1)).c_str(), 0.0f, 1.0f, 0.0f, "", 2));
        if (numParams() != kNumParams) mpc::log("skywave: parameter count mismatch");

        srcRing_.assign((size_t)kSrcLen, 0.0f);
        grabRing_.assign((size_t)kGrabRingLen, 0);
        seedEdges();
        setStatus("turn POWER on to start");
        ws_.setAbort(&quit_);                                  // closing the plugin cuts a slow call short
        wf_.setAbort(&wfAbort_);
        worker_ = std::thread([this] { run(); });
    }

    // Closing waits for the threads; their calls give up within 100 ms of quit_. A name lookup
    // (getaddrinfo) cannot be cut short, so a dead DNS can still hold it for its own timeout.
    ~Skywave() override
    {
        quit_.store(true);
        wfAbort_.store(true);
        if (worker_.joinable()) worker_.join();
        if (wfOpener_.joinable()) wfOpener_.join();
        if (writer_.joinable()) writer_.join();
    }

    // the receiver by its address, not its place in a list that changes daily
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        // always something, so loadExtraState runs on every load (it turns Listen off)
        kiwi::Receiver r;
        const std::string key = (selected(r) ? r.host + ":" + std::to_string(r.port) : "") + "\n";
        out.assign(key.begin(), key.end());
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        {
            std::lock_guard<std::mutex> g(textMu_);
            const std::string all((const char*)data, size);
            savedKey_ = all.substr(0, all.find('\n'));   // the first line; a later version may add more
        }
        relocate_.store(true);
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        grabSr_.store(sampleRate);
        level_.setTime(30.0f, sampleRate);
        level_.snap(dbToGain(get(kLevel)));
        lp_.setCoeffs(BiquadCoeffs::lowpass(sampleRate, 5800.0f, 0.707f));
        dc_.prepare(sampleRate, 20.0f);
        prepared_.store(true);
    }

    void reset() override
    {
        // a state or program load: what was saved is already applied, so nothing may fire now
        seedEdges();
        resetGen_.fetch_add(1, std::memory_order_release);   // and the worker forgets the taps it has not acted on
        binsSkip_.store(true);
        lp_.reset();
        dc_.reset();
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[96];
        if (index == kStatus) { std::lock_guard<std::mutex> g(textMu_); out = status_; return true; }
        if (index == kPeak) { std::lock_guard<std::mutex> g(textMu_); out = peakText_; return true; }
        if (index == kScopeLo || index == kScopeHi)
        {
            const double c = dialKHz(getInt(kMHz), get(kKHz), get(kFine)), half = 0.5 * spanKHz(viewZoom());
            std::snprintf(b, sizeof(b), "%.1f", index == kScopeLo ? c - half : c + half);
            out = b;
            return true;
        }
        if (index == kPassLo || index == kPassHi)
        {
            int lo, hi;
            passband(lo, hi);
            std::snprintf(b, sizeof(b), "%+d Hz", index == kPassLo ? lo : hi);
            out = b;
            return true;
        }
        if (index == kGain)
        {
            if (getInt(kAgc) != 3) { out = "AGC on"; return true; }
            std::snprintf(b, sizeof(b), "%.0f dB", plain);
            out = b;
            return true;
        }
        if (index == kLabel)
        {
            std::lock_guard<std::mutex> g(labelsMu_);
            if (labels_.empty()) { out = connected_.load() ? "turn for labels" : "Listen for labels"; return true; }
            const Label& l = labels_[(size_t)((int)std::lround(plain) % (int)labels_.size())];
            std::snprintf(b, sizeof(b), "%.1f %s", l.kHz, l.name.c_str());
            out = b;
            return true;
        }
        if (index == kReceiver)
        {
            std::lock_guard<std::mutex> g(listMu_);
            const std::vector<int>& idx = byRegion_[(size_t)getInt(kRegion)];
            if (idx.empty()) { out = list_.empty() ? "loading list" : "none here"; return true; }
            const int n = (int)std::lround(plain) % (int)idx.size();
            const kiwi::Receiver& r = list_[(size_t)idx[(size_t)n]];
            const std::string num = std::to_string(n + 1) + " ";
            const std::string busy = r.usersMax > 0 ? " " + std::to_string(r.users) + "/" + std::to_string(r.usersMax) : "";   // busy at the list's last look
            out = num + kiwi::cut(r.label, 31 - num.size() - busy.size()) + busy;
            return true;
        }
        if (index == kScroll) { out = listHeader(); return true; }
        if (index >= kRow0 && index < kRow0 + kRows) { out = rowText(index - kRow0); return true; }
        if (index == kKHz || index == kDial)
        {
            // the whole frequency. The panel's readout shows Dial's text: the worker tells the host
            // whenever Dial moves, MHz and Fine included, where kHz's text is only redrawn when kHz moves
            const double f = index == kKHz ? dialKHz(getInt(kMHz), plain, get(kFine)) : dialKHz(getInt(kMHz), get(kKHz), get(kFine));
            std::snprintf(b, sizeof(b), "%.2f kHz", f);   // to 10 Hz, Tune's finest step
            out = b;
            return true;
        }
        return false;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        blocks_.fetch_add(1, std::memory_order_relaxed);
        const mpc::Transport& tr = ctx.transport;           // a bar, for Grab Len in bars; 120 bpm 4/4 without one
        if (tr.valid && tr.tempoValid && tr.bpm > 1.0)
        {
            bpm_ = tr.bpm;
            barQuarters_ = std::max(1, tr.timeSigNumerator) * 4.0 / std::max(1, tr.timeSigDenominator);
        }
        edges();
        level_.setTarget(dbToGain(get(kLevel)));

        const int gen = streamGen_.load(std::memory_order_acquire);
        if (gen != playGen_)
        {
            playGen_ = gen;
            primed_ = false;
            readPos_ = (double)srcW_.load(std::memory_order_acquire);
        }
        const double srcRate = srcRate_.load(std::memory_order_relaxed);
        const int64_t w = srcW_.load(std::memory_order_acquire);
        const double target = kBufferSecs * srcRate;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int64_t gw = grabW_.load(std::memory_order_relaxed);

        double avail = (double)w - readPos_;
        if (!primed_ && srcRate > 0.0 && avail >= target) { primed_ = true; fill_ = avail; }
        // twice the target behind (a burst after a network stall, or the host not calling for a
        // while): skip to the target; the speed trim would take minutes to drain it
        if (primed_ && avail > 2.0 * target) { readPos_ = (double)w - target; avail = fill_ = target; }
        // trim speed so the buffer sits at its target: the receiver's clock is not ours
        double step = 0.0;
        if (primed_)
        {
            fill_ += 0.01 * (avail - fill_);
            const double err = (fill_ - target) / target;
            step = srcRate / sr_ * (1.0 + std::max(-0.01, std::min(0.01, 0.004 * err)));
        }
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            float x = 0.0f;
            if (primed_)
            {
                if (!((double)w - readPos_ >= 3.0)) primed_ = false;  // underrun: rebuffer
                else
                {
                    const int64_t n = (int64_t)readPos_;
                    const float t = (float)(readPos_ - (double)n);
                    const float y0 = srcRing_[(size_t)((n - 1) & (kSrcLen - 1))], y1 = srcRing_[(size_t)(n & (kSrcLen - 1))];
                    const float y2 = srcRing_[(size_t)((n + 1) & (kSrcLen - 1))], y3 = srcRing_[(size_t)((n + 2) & (kSrcLen - 1))];
                    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                    x = ((c3 * t + c2) * t + c1) * t + y1;
                    readPos_ += step;
                }
            }
            x = softClip(dc_.process(lp_.process(x)) * kMakeup);
            grabRing_[(size_t)(gw & (kGrabRingLen - 1))] = (int16_t)(x * 32767.0f);
            ++gw;
            const float y = x * level_.process();
            L[i] = y;
            if (R) R[i] = y;
        }
        grabW_.store(gw, std::memory_order_release);
        srcR_.store((int64_t)readPos_, std::memory_order_relaxed);
    }

private:
    struct Label { double kHz; int mode; std::string name; };
    struct LivePeak { double kHz; int over; };                   // a peak of the sweep, dB over its window's floor
    struct Live { double kHz; int mode; int over; std::string name; };
    enum LiveState { kLiveNone, kLiveScanning, kLiveDone, kLiveNoSlot };
    static constexpr int kLiveZoom = 3, kLiveMax = 200;
    static constexpr auto kLiveFresh = std::chrono::minutes(10);
    static constexpr float kMakeup = 3.0f;   // the Kiwi's AGC sits around -28 dBFS
    static constexpr int kMaxTries = 5;      // failed calls in a row before giving up on a receiver
    static constexpr auto kSessionEnded = seconds(30);   // on air this long and dropped: the receiver ended it
    static constexpr auto kIdle = std::chrono::minutes(30);
    static constexpr auto kScopeIdle = std::chrono::minutes(5);
    static constexpr auto kWfAskAgain = std::chrono::seconds(15);   // a refused waterfall, asked again on a turn

    static float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
    static double dialKHz(int mhz, float khz, float fine) { return mhz * 1000.0 + khz + fine; }
    // tanh's shape, rational: x(27 + x^2) / (27 + 9x^2), exactly 1 from 3 on
    static float softClip(float x)
    {
        if (x >= 3.0f) return 1.0f;
        if (x <= -3.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    // the passband the receiver is asked for: the mode's width, moved by Shift (up in pitch),
    // a sideband kept on its own side of the carrier
    void passband(int& lo, int& hi)
    {
        const int mode = std::clamp(getInt(kMode), 0, 4), bw = std::clamp(getInt(kBandwidth), 0, 2);
        lo = kPass[mode][bw][0];
        hi = kPass[mode][bw][1];
        if (mode == kNBFM) return;
        const int s = (int)std::lround(get(kShift)) * (mode == kLSB ? -1 : 1);
        lo += s;
        hi += s;
        if (mode == kUSB || mode == kCW) { const int d = std::max(0, 50 - lo); lo += d; hi += d; }
        if (mode == kLSB) { const int d = std::max(0, hi + 50); lo -= d; hi -= d; }
        lo = std::clamp(lo, -5900, 5900);
        hi = std::clamp(hi, -5900, 5900);
    }

    void setStatus(const std::string& text, bool update = true)
    {
        const std::string s = kiwi::cut(text, 31);   // the most the host shows
        if (s.size() < text.size()) mpc::log("skywave: status cut: %s", text.c_str());
        {
            std::lock_guard<std::mutex> g(textMu_);
            if (status_ == s) return;
            status_ = s;
        }
        mpc::log("skywave: %s", s.c_str());
        if (update) requestDisplayUpdate();
    }

    // ----- what the user does, seen on the audio thread -----------------------------------------
    // A turn of Station, Category, Label, Seek, Span, Scale or Grab is an edge. They are found here, not on the
    // worker, because after a project load the framework runs reset() before the next block: the
    // worker would take a restored value for a turn of the knob. The worker acts on the counts.
    void seedEdges()
    {
        pStation_ = getInt(kStation);
        pCategory_ = getInt(kCategory);
        pLabel_ = getInt(kLabel);
        pSeekUp_ = getInt(kSeekUp);
        pSeekDown_ = getInt(kSeekDown);
        pGrab_ = getInt(kGrab);
        pSpan_ = getInt(kSpan);
        pScale_ = getInt(kScale);
        pPageUp_ = getInt(kPageUp);
        pPageDown_ = getInt(kPageDown);
        pBack_ = getInt(kBack);
        pDig_ = getInt(kDig);
        for (int r = 0; r < kRows; ++r) pRow_[r] = getInt(kRow0 + r);
    }

    void edges()
    {
        auto edge = [](int now, int& seen, std::atomic<int>& ev) {
            if (now == seen) return;
            seen = now;
            ev.fetch_add(1, std::memory_order_release);
        };
        edge(getInt(kStation), pStation_, evStation_);
        edge(getInt(kCategory), pCategory_, evCategory_);
        edge(getInt(kLabel), pLabel_, evLabel_);
        edge(getInt(kSeekUp), pSeekUp_, evSeekUp_);
        edge(getInt(kSeekDown), pSeekDown_, evSeekDown_);
        edge(getInt(kSpan), pSpan_, evSpan_);
        edge(getInt(kScale), pScale_, evSpan_);     // Scale, like Span, is the scope in use
        edge(getInt(kPageUp), pPageUp_, evPageUp_);
        edge(getInt(kPageDown), pPageDown_, evPageDown_);
        edge(getInt(kBack), pBack_, evBack_);
        edge(getInt(kDig), pDig_, evDig_);
        for (int r = 0; r < kRows; ++r) edge(getInt(kRow0 + r), pRow_[r], evRow_[r]);
        const int g = getInt(kGrab);
        if (g != pGrab_)
        {
            pGrab_ = g;
            grabEnd_.store(grabW_.load(std::memory_order_relaxed), std::memory_order_relaxed);   // the seconds before the tap
            grabBpm_.store(bpm_, std::memory_order_relaxed);                                      // and the tempo at the tap
            grabQuarters_.store(barQuarters_, std::memory_order_relaxed);
            evGrab_.fetch_add(1, std::memory_order_release);
        }
    }

    // ----- the worker -------------------------------------------------------------------------
    // a new list keeps the receiver that was selected, if it is still there
    void publish(std::vector<kiwi::Receiver> list)
    {
        kiwi::Receiver was;
        const bool had = selected(was);
        int keep = -1;
        {
            std::lock_guard<std::mutex> g(listMu_);
            list_ = std::move(list);
            for (auto& v : byRegion_) v.clear();
            for (int i = 0; i < (int)list_.size(); ++i)
            {
                byRegion_[kiwi::kAll].push_back(i);
                if (list_[(size_t)i].region != kiwi::kAll) byRegion_[(size_t)list_[(size_t)i].region].push_back(i);
            }
            const std::vector<int>& idx = byRegion_[(size_t)getInt(kRegion)];
            for (int k = 0; had && k < (int)idx.size(); ++k)
            {
                const kiwi::Receiver& r = list_[(size_t)idx[(size_t)k]];
                if (r.host == was.host && r.port == was.port) { keep = k; break; }
            }
        }
        if (keep >= 0 && keep != getInt(kReceiver)) { set(kReceiver, (float)keep); notifyHostParameterChanged(kReceiver); }
    }

    bool selected(kiwi::Receiver& out)
    {
        std::lock_guard<std::mutex> g(listMu_);
        const std::vector<int>& idx = byRegion_[(size_t)getInt(kRegion)];
        if (idx.empty()) return false;
        out = list_[(size_t)idx[(size_t)(getInt(kReceiver) % (int)idx.size())]];
        return true;
    }

    void loadList()
    {
        std::string text;
        const std::string folder = mpc::pluginFolder();
        cachePath_ = folder.empty() ? "" : folder + "/receivers.cache.js";
        if (cachePath_.empty() || !mpc::readTextFile(cachePath_, text)) mpc::readTextFile(mpc::contentPath("receivers.js"), text);
        auto l = kiwi::parse(text);
        if (l.size() < 50 && !cachePath_.empty())                // a cache cut short: the shipped list
        {
            text.clear();
            mpc::readTextFile(mpc::contentPath("receivers.js"), text);
            l = kiwi::parse(text);
        }
        if (!l.empty()) publish(std::move(l));
    }

    // fetch_list.py's "// list epoch: N" line, within three days of the clock
    static bool recentList(const std::string& body)
    {
        const size_t p = body.find("// list epoch: ");
        if (p == std::string::npos || p > 1024) return false;
        const long long at = std::atoll(body.c_str() + p + 15);
        return at > 0 && std::llabs((long long)std::time(nullptr) - at) < 3 * 24 * 3600;
    }

    // the list, at most once a day: it changes slowly
    void refreshList()
    {
        listFetched_ = true;
        struct stat st;
        if (!cachePath_.empty() && stat(cachePath_.c_str(), &st) == 0 && std::llabs((long long)(std::time(nullptr) - st.st_mtime)) < 24 * 3600) return;
        setStatus("fetching the receiver list");
        // databurn.org's trimmed copy first (a tenth the size, no owners' contact details), unless
        // its regeneration stopped three days ago; else the full list from the volunteer's server
        std::string body;
        std::vector<kiwi::Receiver> l;
        if (net::httpGet("http://databurn.org/mpc/receivers.js", body, 2u << 20, 8000, &quit_) && recentList(body)) l = kiwi::parse(body);
        if (quit_.load()) return;
        if (l.size() < 50)
        {
            body.clear();
            if (!net::httpGet("http://rx.linkfanel.net/kiwisdr_com.js", body, 8u << 20, 8000, &quit_)) { setStatus("offline: the built-in list"); return; }
            l = kiwi::parse(body);
        }
        if (l.size() < 50) { setStatus("offline: the built-in list"); return; }
        publish(std::move(l));
        if (!cachePath_.empty())
            if (std::FILE* f = std::fopen(cachePath_.c_str(), "wb")) { std::fwrite(body.data(), 1, body.size(), f); std::fclose(f); }
        requestDisplayUpdate();
    }

    void tuneStation(int s)
    {
        if (s <= 0 || s >= kNumStations) return;
        const Station& st = kStations[s];
        if (!stationIn(s, getInt(kCategory)))                // the category follows the station
        {
            set(kCategory, (float)st.cat);
            notifyHostParameterChanged(kCategory);
        }
        setTune(st.kHz, st.mode);
    }

    // the stations of category c in order: from `from` one step dir, round; from outside it, an end
    static int walkCategory(int c, int from, int dir)
    {
        std::vector<int> in;
        for (int k = 1; k < kNumStations; ++k) if (stationIn(k, c)) in.push_back(k);
        if (in.empty()) return 0;
        const auto it = std::find(in.begin(), in.end(), from);
        if (it == in.end()) return dir > 0 ? in.front() : in.back();
        const int n = (int)in.size();
        return in[(size_t)(((int)(it - in.begin()) + dir + n) % n)];
    }

    // Station turns through the chosen Category only. The plugin's own picks (a LIST row, Dig)
    // set the Category first, so only a hand on the dial lands outside it.
    void onStation()
    {
        int s = getInt(kStation);
        if (s > 0 && s < kNumStations && !stationIn(s, getInt(kCategory)))
        {
            s = walkCategory(getInt(kCategory), lastStation_, s > lastStation_ ? 1 : -1);
            if (s <= 0) return;
            set(kStation, (float)s);
            notifyHostParameterChanged(kStation);
        }
        lastStation_ = s;
        tuneStation(s);
        if (s > 0 && s < kNumStations && onAir_ && digTry_ == 0 && !digAfterSurvey_) setStatus(std::string(kStations[s].name) + ": " + soundOf(s));
    }

    // A category picked: the LIST rows show its stations and the radio stays where it is, so a
    // catch keeps playing while you look. Station goes to "Dial" (which tunes nothing): any
    // station of the category is then a change, and a row tap or Station tunes.
    void onCategory()
    {
        const int c = getInt(kCategory), now = getInt(kStation);
        if (now <= 0 || now >= kNumStations || stationIn(now, c)) return;
        set(kStation, 0.0f);
        notifyHostParameterChanged(kStation);
    }

    // the dial to f kHz (and the mode, unless -1): whole kHz on kHz, the rest on Fine
    void setTune(double f, int mode)
    {
        if (!std::isfinite(f)) return;
        f = std::clamp(f, 0.0, 29999.99);
        const int mhz = (int)(f / 1000.0);
        const double rem = f - mhz * 1000.0;
        const int k = std::min(999, (int)std::floor(rem + 1e-6));
        set(kMHz, (float)mhz);
        set(kKHz, (float)k);
        set(kFine, (float)(rem - k));
        if (mode >= 0) set(kMode, (float)mode);
        for (int p : {kMHz, kKHz, kFine, kMode}) notifyHostParameterChanged(p);
    }

    // Tune turned d steps of Step. Off Step's grid, the first step lands on it (a radio's channel
    // steps: 5 kHz from 9652.3 goes to 9655, then 9660), so the broadcast grids are a turn each
    void stepTune(int d)
    {
        const double step = kStepHz[std::clamp(getInt(kStep), 0, kNumSteps - 1)] / 1000.0;
        const double g = dialKHz(getInt(kMHz), get(kKHz), get(kFine)) / step;
        const double on = std::round(g);
        const double base = std::fabs(g - on) < 1e-3 ? on : d > 0 ? std::floor(g) : std::ceil(g);
        setTune(std::clamp((base + d) * step, 0.0, 29999.99), -1);
    }

    // ----- the scope, the labels, seek ----------------------------------------------------------
    // A second websocket, /<same time>/W/F, pairs with the audio one as one listener, as a browser
    // does. It brings the Kiwi's waterfall rows (1024 bytes of dB, here centred on the dial over
    // Span) and, once, the receiver's own DX labels. Most receivers have three waterfalls for eight
    // listeners, so it is opened only while the scope is in use: Seek, Span, Label or a tapped bar
    // opens it. Closing it would end the audio too (the Kiwi tears down the pair), so five minutes
    // unused parks it instead: wf_speed=0, no rows sent, as a browser's waterfall turned off. It
    // closes with the audio, when Listen goes off or the receiver changes.

    // Span's zoom, as far in as this receiver goes (its zoom_cap: some stop at 11, 15 kHz)
    int viewZoom() const { return std::min(kSpanZoom[std::clamp(getInt(kSpan), 0, kNumSpans - 1)], zoomCap_.load()); }

    void wantScope(Clock::time_point now)
    {
        scopeUntil_ = now + kScopeIdle;
        wfRetryAt_ = std::min(wfRetryAt_, wfRefusedAt_ + kWfAskAgain);   // a hand on the scope asks again sooner
        if (surveyOn_) surveyWas_ = scopeUntil_;             // still wanted once the sweep is done
    }

    // The waterfall's call is made on a thread of its own: a far receiver's handshake takes longer
    // than the audio buffer, and this thread must keep draining the audio meanwhile. Nothing here
    // touches wf_ while the call is out (wfConnecting_).
    void openScope()
    {
        if (wfOpener_.joinable()) wfOpener_.join();
        wfReady_ = false;
        wfCenter_ = -1.0;
        wfZoom_ = -1;
        zoomCap_.store(14);
        wfAbort_.store(false);
        if (quit_.load()) { wfAbort_.store(true); return; }    // closing: the destructor set it first
        wfConnecting_.store(true);
        wfOpener_ = std::thread([this, host = connHost_, port = connPort_, path = "/" + ts_ + "/W/F"] {
            std::string rh;
            int rp = 0;
            const bool ok = wf_.connect(host, port, path, 5000, rh, rp) == 0;
            if (ok) { wf_.setTimeoutMs(1); wf_.sendText("SET auth t=kiwi p="); }
            wfResult_.store(ok ? 1 : 2);
            wfConnecting_.store(false);
        });
    }

    void closeScope()
    {
        if (surveyOn_) abortSurvey();
        wfAbort_.store(true);                                  // a call still out gives up within 100 ms
        if (wfOpener_.joinable()) wfOpener_.join();
        wfConnecting_.store(false);
        wfResult_.store(0);
        wf_.close();
        wfOpen_ = wfReady_ = wfParked_ = false;
        if (!haveLabels()) labelsAsked_ = false;            // closed before they came: ask again next time
        dropBars();
    }

    void dropBars()
    {
        binsLive_ = false;
        row_.clear();
        rowSpanKHz_ = 0.0;
        for (int b = 0; b < kBins; ++b)                      // the bars fall
        {
            binWritten_[b] = 0.0f;
            barDb_[b] = 0.0;
            if (get(kBin0 + b) != 0.0f) { set(kBin0 + b, 0.0f); notifyHostParameterChanged(kBin0 + b); }
        }
        clearWaterfall();
        setPeak(noWf_.load() ? kNoSlot : kScopeHint);
    }

    // the waterfall's history forgotten, its cells dark
    void clearWaterfall()
    {
        for (auto& r : wfHist_) r.fill(0);
        for (auto& v : wfSlice_) v = -1;
        wfHistSpan_ = 0.0;
        for (int k = 0; k < kWfRows * kWfCols; ++k)
            if (get(kWf0 + k) != 0.0f) { set(kWf0 + k, 0.0f); notifyHostParameterChanged(kWf0 + k); }
    }

    static constexpr const char* kScopeHint = "turn SPAN to start the scope";
    static constexpr const char* kNoSlot = "no waterfall slot: try later";   // the receiver's waterfalls are all in use

    // the Peak read-out's text; its value flips so the host asks for the text again
    void setPeak(const std::string& text)
    {
        {
            std::lock_guard<std::mutex> g(textMu_);
            if (peakText_ == text) return;
            peakText_ = text;
        }
        set(kPeak, get(kPeak) > 0.5f ? 0.0f : 1.0f);
        notifyHostParameterChanged(kPeak);
    }

    void scope(Clock::time_point now)
    {
        if (surveyOn_ && now >= surveyEnd_ && !wfReady_) abortSurvey();   // the waterfall never came
        if (wfConnecting_.load()) return;
        if (const int res = wfResult_.exchange(0))           // the call came back
        {
            if (wfOpener_.joinable()) wfOpener_.join();
            if (res == 1) { wfOpen_ = true; lastWfKeepalive_ = now; noWf_.store(false); }
            else
            {
                wfRetryAt_ = now + seconds(60);
                wfRefusedAt_ = now;
                noWf_.store(true);
                setPeak(kNoSlot);
                setStatus(surveyOn_ ? "no waterfall slot: try another" : "no waterfall slot: scope off");
                if (surveyOn_) abortSurvey();
            }
        }
        if (!wfOpen_)
        {
            if (now < scopeUntil_ && now >= wfRetryAt_) openScope();
            else tappedClosed(now);
            return;
        }
        std::vector<uint8_t> m;
        for (int k = 0; k < 16; ++k)
        {
            const int r = wf_.receive(m);
            if (r < 0)
            {
                // refused before it began: no slot; ended later: the receiver's doing. Either way, wait
                if (!wfReady_) { wfRetryAt_ = now + seconds(60); noWf_.store(true); setStatus(surveyOn_ ? "no waterfall slot: try another" : "no waterfall slot: scope off"); }
                else wfRetryAt_ = now + seconds(30);
                wfRefusedAt_ = now;
                closeScope();
                return;
            }
            if (r == 0) break;
            if (m.size() > 4 && m[0] == 'M' && m[1] == 'S' && m[2] == 'G') wfMessage(std::string(m.begin() + 4, m.end()));
            else if (m.size() > 16 && m[0] == 'W' && m[1] == '/' && m[2] == 'F') wfRow(m, now);
        }
        if (!wfReady_) return;
        if (now - lastWfKeepalive_ >= seconds(1)) { wf_.sendText("SET keepalive"); lastWfKeepalive_ = now; }
        if (now >= scopeUntil_ && !wfParked_)                // unused: no rows, the slot kept
        {
            wf_.sendText("SET wf_speed=0");
            wfParked_ = true;
            dropBars();
        }
        else if (now < scopeUntil_ && wfParked_)             // wanted again
        {
            wf_.sendText("SET wf_speed=2");
            wfParked_ = false;
            wfZoom_ = -1;
        }
        if (wfParked_) { tappedClosed(now); return; }
        if (surveyOn_) { surveyStep(now); return; }
        if (binsSkip_.exchange(false)) { binsLive_ = false; barPending_ = false; }   // a project just loaded its bars: not touched
        if (binsLive_)
            if (const int b = touchedBar(now); b >= 0) { tapTune(b); wantScope(now); touched(now); }
        const int zoom = viewZoom();
        const double c = dialKHz(getInt(kMHz), get(kKHz), get(kFine));
        if (zoom != wfZoom_ || std::fabs(c - wfCenter_) > 0.005)
        {
            char b[64];
            std::snprintf(b, sizeof(b), "SET zoom=%d cf=%.3f", zoom, c);
            wf_.sendText(b);
            wfZoom_ = zoom;
            wfCenter_ = c;
        }
    }

    // a bar that moved from what we wrote, and stayed moved 30 ms: a load writes the bars before its
    // reset() (a block or two later), and those were not touched
    int touchedBar(Clock::time_point now)
    {
        int hit = -1;
        for (int b = 0; b < kBins && hit < 0; ++b)
            if (std::fabs(get(kBin0 + b) - binWritten_[b]) > 0.08f) hit = b;
        if (hit < 0) { barPending_ = false; return -1; }
        if (!barPending_) { barPending_ = true; barSince_ = now; return -1; }
        if (now - barSince_ < milliseconds(30)) return -1;
        barPending_ = false;
        set(kBin0 + hit, binWritten_[hit]);
        notifyHostParameterChanged(kBin0 + hit);
        return hit;
    }

    // a bar tapped while the waterfall is closed: open it, and tune where the bar would be
    void tappedClosed(Clock::time_point now)
    {
        if (binsSkip_.exchange(false))                       // a project loaded its bars: they were not touched
        {
            barPending_ = false;
            for (int b = 0; b < kBins; ++b)
                if (get(kBin0 + b) != 0.0f) { set(kBin0 + b, 0.0f); notifyHostParameterChanged(kBin0 + b); }
            clearWaterfall();
        }
        const int b = touchedBar(now);
        if (b < 0) return;
        const double span = spanKHz(viewZoom());
        setTune(dialKHz(getInt(kMHz), get(kKHz), get(kFine)) - 0.5 * span + (b + 0.5) * span / kBins, -1);
        wantScope(now);
        touched(now);
    }

    void wfMessage(const std::string& body)
    {
        size_t p = 0;
        while (p < body.size())
        {
            size_t e = body.find(' ', p);
            if (e == std::string::npos) e = body.size();
            const std::string kv = body.substr(p, e - p);
            p = e + 1;
            const size_t eq = kv.find('=');
            const std::string k = kv.substr(0, eq);
            if (k == "wf_setup")
            {
                wfReady_ = true;
                wf_.sendText("SET maxdb=-10 mindb=-110");
                wf_.sendText("SET wf_speed=2");
                wf_.sendText("SET wf_comp=0");
                wf_.sendText("SET ident_user=skywave-mpc");
                if (!labelsAsked_)
                {
                    wf_.sendText("SET MARKER db=0 min=0 max=30000 zoom=0 width=1024 eibi_types_mask=0x0 filter_tod=0 anti_clutter=0");
                    labelsAsked_ = true;
                }
            }
            else if (k == "mkr" && eq != std::string::npos) parseLabels(urlDecode(kv.substr(eq + 1)));
            else if (k == "zoom_cap" && eq != std::string::npos)
            {
                const int z = std::atoi(kv.c_str() + eq + 1);
                if (z >= 5 && z <= 14) zoomCap_.store(z);
            }
        }
    }

    // One waterfall row (a byte a bin, dB, 1024 over Span). The floor is the row's median; each bar
    // is the loudest bin of its sixteenth of the row in dB over it, falling back 4 dB a tenth of a
    // second so a fade reads as one; Scale sets the top: Auto, the strongest in view lately (20 dB at
    // least, 6 dB a second down when it goes), or a fixed range. Every 600 ms the loudest of each bar's slice since
    // the last becomes a new waterfall row on top; the history slides with the dial and clears with
    // Span. The Peak read-out names the strongest signal near the dial and how far off it is.
    void wfRow(const std::vector<uint8_t>& m, Clock::time_point now)
    {
        const uint8_t* d = m.data() + 4;
        const uint32_t xBin = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
        const int zoom = d[4] & 0x1f;
        const double full = rx_.highKHz > 30500.0 ? 32000.0 : 30000.0;     // some Kiwis run to 32 MHz
        if (wfParked_) return;                               // rows already on their way when it was parked
        if (surveyOn_) { surveyRow(xBin / 16777216.0 * full, zoom, m.data() + 16, m.size() - 16); return; }
        row_.assign(m.begin() + 16, m.end());
        if (row_.size() < (size_t)kBins) return;
        rowStartKHz_ = xBin / 16777216.0 * full;                         // bins of the 2^14-zoomed 1024: 2^24 over the band
        rowSpanKHz_ = spanKHz(zoom, full);
        std::vector<uint8_t> sorted = row_;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        rowFloor_ = sorted[sorted.size() / 2];
        const size_t per = row_.size() / kBins;
        int top = 0;
        for (int b = 0; b < kBins; ++b)
        {
            int v = 0;
            for (size_t i = b * per; i < (b + 1) * per; ++i) v = std::max(v, (int)row_[i]);
            sliceDb_[b] = v - rowFloor_;
            top = std::max(top, sliceDb_[b]);
        }
        slideWaterfall();                                    // a new span starts Auto again too
        const double since = std::min(1.0, std::chrono::duration<double>(now - lastRow_).count());
        lastRow_ = now;
        // the strongest lately: up a quarter of the way a row (one crash of static does not squash the
        // view), down 6 dB a second
        autoTop_ = top > autoTop_ ? autoTop_ + 0.25 * (top - autoTop_) : std::max((double)top, autoTop_ - 6.0 * since);
        for (int b = 0; b < kBins; ++b) wfSlice_[b] = std::max(wfSlice_[b], sliceDb_[b]);
        if (now - lastWfPush_ >= milliseconds(600)) { lastWfPush_ = now; pushWaterfall(); }
        if (now - lastBins_ < milliseconds(100) && binsLive_) return;   // ten times a second is enough for the eye
        const double fall = binsLive_ ? 4.0 * std::chrono::duration<double>(now - lastBins_).count() / 0.1 : 1e9;
        lastBins_ = now;
        for (int b = 0; b < kBins; ++b)
        {
            barDb_[b] = std::max((double)sliceDb_[b], barDb_[b] - fall);
            const float y = (float)scaled(barDb_[b]);
            if (std::fabs(y - get(kBin0 + b)) >= 0.02f) { set(kBin0 + b, y); notifyHostParameterChanged(kBin0 + b); }
            binWritten_[b] = get(kBin0 + b);
        }
        binsLive_ = true;
        peakNearDial();
    }

    // dB over the floor to a bar's height: the first 3 dB are the noise's own ripple; Auto's top just over the strongest
    double scaled(double db) const
    {
        const int s = std::clamp(getInt(kScale), 0, 3);
        const double range = s == 0 ? std::max(20.0, autoTop_ + 2.0) : 20.0 * s;
        return std::clamp((db - 3.0) / (range - 3.0), 0.0, 1.0);
    }

    // the history follows the dial: a new span starts it again, a move slides it across
    void slideWaterfall()
    {
        if (rowSpanKHz_ != wfHistSpan_)
        {
            for (auto& r : wfHist_) r.fill(0);
            for (auto& v : wfSlice_) v = -1;
            wfHistSpan_ = rowSpanKHz_;
            wfHistStart_ = rowStartKHz_;
            autoTop_ = 0.0;
            return;
        }
        const int k = (int)std::lround((rowStartKHz_ - wfHistStart_) / (rowSpanKHz_ / kWfCols));
        if (k == 0) return;
        if (std::abs(k) >= kWfCols) autoTop_ = 0.0;         // a jump past the whole view: a new place
        wfHistStart_ += k * rowSpanKHz_ / kWfCols;
        auto shift = [k](auto& a, auto blank) {
            const auto was = a;
            for (int c = 0; c < kWfCols; ++c) a[(size_t)c] = c + k >= 0 && c + k < kWfCols ? was[(size_t)(c + k)] : blank;
        };
        for (auto& r : wfHist_) shift(r, (uint8_t)0);
        shift(wfSlice_, -1);
    }

    void pushWaterfall()
    {
        bool any = false;
        for (int v : wfSlice_) any = any || v >= 0;
        if (!any) return;
        for (int r = kWfRows - 1; r > 0; --r) wfHist_[(size_t)r] = wfHist_[(size_t)r - 1];
        for (int c = 0; c < kWfCols; ++c)
        {
            wfHist_[0][(size_t)c] = wfSlice_[(size_t)c] < 0 ? 0 : (uint8_t)std::lround(scaled(wfSlice_[(size_t)c]) * (kWfLevels - 1));
            wfSlice_[(size_t)c] = -1;
        }
        for (int r = 0; r < kWfRows; ++r)
            for (int c = 0; c < kWfCols; ++c)
            {
                const float v = wfHist_[(size_t)r][(size_t)c] / (float)(kWfLevels - 1);
                const int p = kWf0 + r * kWfCols + c;
                if (get(p) != v) { set(p, v); notifyHostParameterChanged(p); }
            }
    }

    // where a signal's peak is, between bins: the vertex of the parabola through the top three
    double peakBin(int i) const
    {
        const int n = (int)row_.size();
        if (i <= 0 || i >= n - 1) return i + 0.5;
        const double a = row_[(size_t)i - 1], b = row_[(size_t)i], c = row_[(size_t)i + 1], den = a - 2.0 * b + c;
        return i + 0.5 + (den < 0.0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0);
    }

    // the strongest signal within an eighth of the span of the dial, to 10 Hz, and how far it is from it
    void peakNearDial()
    {
        const int n = (int)row_.size();
        if (n == 0 || rowSpanKHz_ <= 0.0) return;
        const double per = rowSpanKHz_ / n, f = dialKHz(getInt(kMHz), get(kKHz), get(kFine)), win = rowSpanKHz_ / 8.0;
        const int i0 = std::max(0, (int)((f - win - rowStartKHz_) / per)), i1 = std::min(n - 1, (int)((f + win - rowStartKHz_) / per));
        int best = -1;
        for (int i = i0; i <= i1; ++i) if (best < 0 || row_[(size_t)i] > row_[(size_t)best]) best = i;
        if (best < 0 || row_[(size_t)best] - rowFloor_ < 10) { setPeak("no signal at the dial"); return; }
        const double pk = rowStartKHz_ + peakBin(best) * per, off = (pk - f) * 1000.0;
        char b[64];
        if (std::fabs(off) < 999.5) std::snprintf(b, sizeof(b), "%.2f kHz +%d dB (%+.0f Hz)", pk, row_[(size_t)best] - rowFloor_, off);
        else std::snprintf(b, sizeof(b), "%.2f kHz +%d dB (%+.1f kHz)", pk, row_[(size_t)best] - rowFloor_, off / 1000.0);
        setPeak(b);
    }

    // A tapped bar: onto the strongest signal in it (half a bar either side, so a carrier on its edge
    // is found), to 10 Hz; in USB or LSB that is the voice, about a kilohertz from its carrier, so the
    // dial goes there. Only noise in it: the bar's middle
    void tapTune(int b)
    {
        const int n = (int)row_.size();
        if (n < kBins || rowSpanKHz_ <= 0.0) { setTune(rowStartKHz_ + (b + 0.5) * rowSpanKHz_ / kBins, -1); return; }
        const int per = n / kBins, i0 = std::max(0, b * per - per / 2), i1 = std::min(n - 1, (b + 1) * per + per / 2 - 1);
        int best = i0;
        for (int i = i0; i <= i1; ++i) if (row_[(size_t)i] > row_[(size_t)best]) best = i;
        if (row_[(size_t)best] - rowFloor_ < 8) { setTune(rowStartKHz_ + (b + 0.5) * rowSpanKHz_ / kBins, -1); return; }
        const int mode = getInt(kMode);
        const double side = mode == kUSB ? -1.0 : mode == kLSB ? 1.0 : 0.0;
        setTune(std::round((rowStartKHz_ + peakBin(best) * rowSpanKHz_ / n + side) * 100.0) / 100.0, -1);
    }

    // the Kiwi's labels: a JSON array of flat objects, f in kHz, fl the flags (mode in the low bits), i the name
    void parseLabels(const std::string& json)
    {
        std::vector<Label> out;
        size_t p = 0;
        while ((p = json.find('{', p)) != std::string::npos)
        {
            const size_t e = json.find('}', p);
            if (e == std::string::npos) break;
            const std::string o = json.substr(p, e - p + 1);
            p = e + 1;
            if (o.find("\"tc\"") != std::string::npos) continue;   // the header
            double f = 0.0, fl = 0.0;
            if (!jsonNumber(o, "f", f) || !(f > 0.0 && f < 30000.0)) continue;
            jsonNumber(o, "fl", fl);
            const int flags = std::isfinite(fl) && fl >= 0.0 && fl < 2147483647.0 ? (int)fl : 0;
            const int m = (flags & 0xf) | ((flags & 0x40000) ? 16 : 0);
            static const int kMap[18] = {kAM, kAM, kUSB, kLSB, kCW, kCW, kNBFM, kAM, kAM, kUSB, kLSB, kAM, kUSB, kLSB, kAM, kAM, kNBFM, kAM};
            std::string name = kiwi::screenText(urlDecode(jsonString(o, "i")));
            if (name.empty()) name = "?";
            out.push_back({f, kMap[std::clamp(m, 0, 17)], kiwi::cut(name, 40)});
        }
        std::sort(out.begin(), out.end(), [](const Label& a, const Label& b) { return a.kHz < b.kHz; });
        const double lo = rx_.lowKHz, hi = rx_.highKHz > 0.0 ? rx_.highKHz : 30000.0;
        out.erase(std::remove_if(out.begin(), out.end(), [&](const Label& l) { return l.kHz < lo - 0.5 || l.kHz > hi + 0.5; }), out.end());
        if (out.size() > 1024)                              // too many: doubles first
            out.erase(std::unique(out.begin(), out.end(), [](const Label& a, const Label& b) { return b.kHz - a.kHz < 0.05; }), out.end());
        if (out.size() > 1024)                              // thin evenly, not the top bands off
        {
            std::vector<Label> keep;
            keep.reserve(1024);
            for (size_t k = 0; k < 1024; ++k) keep.push_back(out[k * out.size() / 1024]);
            out = std::move(keep);
        }
        {
            std::lock_guard<std::mutex> g(labelsMu_);
            labels_ = std::move(out);
        }
        notifyHostParameterChanged(kLabel);   // its text: the list arrived
        nameLive();                           // a sweep that came first gets its names now
    }

    // a whole number from the receiver's own settings (load_cfg: URL-encoded JSON), 0 when absent
    static int cfgInt(const std::string& json, const char* key)
    {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = json.find(k);
        if (p == std::string::npos) return 0;
        p = json.find(':', p + k.size());
        return p == std::string::npos ? 0 : std::atoi(json.c_str() + p + 1);
    }

    // "on <receiver>", and the owner's limits when there are any
    std::string onAirText() const
    {
        // the receiver's name is on the RCVR line already
        std::string s = "on air";
        if (idleLimit_ > 0) s += ": idle " + std::to_string(idleLimit_);
        if (dayLimit_ > 0) s += (idleLimit_ > 0 ? ", " : ": ") + std::to_string(dayLimit_) + " min/day";
        else if (idleLimit_ > 0) s += " min";
        return s;
    }

    static bool jsonNumber(const std::string& o, const char* key, double& v)
    {
        const std::string k = std::string("\"") + key + "\":";
        const size_t p = o.find(k);
        if (p == std::string::npos) return false;
        v = std::atof(o.c_str() + p + k.size());
        return true;
    }

    static std::string jsonString(const std::string& o, const char* key)
    {
        const std::string k = std::string("\"") + key + "\":\"";
        size_t p = o.find(k);
        if (p == std::string::npos) return "";
        std::string out;
        for (p += k.size(); p < o.size() && o[p] != '"'; ++p)
        {
            if (o[p] == '\\' && p + 1 < o.size()) { ++p; out += o[p] == 'n' ? ' ' : o[p]; }
            else out += o[p];
        }
        return out;
    }

    static std::string urlDecode(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == '%' && i + 2 < in.size() && std::isxdigit((unsigned char)in[i + 1]) && std::isxdigit((unsigned char)in[i + 2]))
            {
                out += (char)std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
                i += 2;
            }
            else out += in[i];
        }
        return out;
    }

    bool labelAt(int k, Label& out)
    {
        std::lock_guard<std::mutex> g(labelsMu_);
        if (labels_.empty()) return false;
        out = labels_[(size_t)(k % (int)labels_.size())];
        return true;
    }

    bool haveLabels()
    {
        std::lock_guard<std::mutex> g(labelsMu_);
        return !labels_.empty();
    }

    // Label and Seek. With the waterfall closed there are no labels and no row yet: the turn waits
    // (up to 3 s) for them to arrive
    void onLabel(Clock::time_point now)
    {
        if (getInt(kLabel) == ourLabel_.exchange(-1)) return;   // set by a row, a dig or Seek: tuned already
        wantScope(now);
        Label l;
        if (labelAt(getInt(kLabel), l)) setTune(l.kHz, l.mode);
        else if (connected_.load()) { pendingLabel_ = true; pendingUntil_ = now + seconds(3); }
    }

    void onSeek(int dir, Clock::time_point now)
    {
        const bool byLabels = getInt(kSeekBy) == 1;
        if (!byLabels && (!wfOpen_ || wfParked_) && liveFresh(now))   // a sweep is fresh: step through it, the waterfall left alone
        {
            if (!seekLive(dir)) setStatus(dir > 0 ? "nothing higher in Live" : "nothing lower in Live");
            return;
        }
        wantScope(now);
        if (connected_.load() && (byLabels ? !haveLabels() : row_.empty())) { pendingSeek_ = dir; pendingUntil_ = now + seconds(3); return; }
        seek(dir);
    }

    void pending(Clock::time_point now)
    {
        const bool late = now >= pendingUntil_;
        if (pendingLabel_ && (haveLabels() || late))
        {
            pendingLabel_ = false;
            Label l;
            if (labelAt(getInt(kLabel), l)) setTune(l.kHz, l.mode);
        }
        if (pendingSeek_ != 0 && ((getInt(kSeekBy) == 1 ? haveLabels() : !row_.empty()) || late))
        {
            const int dir = pendingSeek_;
            pendingSeek_ = 0;
            seek(dir);
        }
    }

    void seek(int dir)
    {
        const double f = dialKHz(getInt(kMHz), get(kKHz), get(kFine));
        if (getInt(kSeekBy) == 1) seekLabel(f, dir);
        else seekSignal(f, dir);
    }

    void seekLabel(double f, int dir)
    {
        int found = -1;
        Label l;
        {
            std::lock_guard<std::mutex> g(labelsMu_);   // copy it out: setTune calls the host, which may ask for text
            const int n = (int)labels_.size();
            for (int k = dir > 0 ? 0 : n - 1; k >= 0 && k < n; k += dir)
                if ((dir > 0 && labels_[(size_t)k].kHz > f + 0.05) || (dir < 0 && labels_[(size_t)k].kHz < f - 0.05)) { found = k; l = labels_[(size_t)k]; break; }
        }
        if (found < 0) return;
        setTune(l.kHz, l.mode);
        ourLabel_.store(found);
        set(kLabel, (float)found);
        notifyHostParameterChanged(kLabel);
    }

    // the next peak 15 dB over the floor, past this signal; none in view: half a span on. In
    // USB or LSB the peak is the voice, about a kilohertz from its carrier, so the dial goes there
    void seekSignal(double f, int dir)
    {
        if (row_.empty() || rowSpanKHz_ <= 0.0) { setTune(f + dir * 0.5 * spanKHz(viewZoom()), -1); return; }
        const int n = (int)row_.size();
        const double per = rowSpanKHz_ / n;
        const double guard = std::clamp(rowSpanKHz_ * 0.03, 0.5, 5.0);
        int i = (int)((f + dir * guard - rowStartKHz_) / per);
        const int strong = rowFloor_ + 15;
        while (i >= 0 && i < n && row_[(size_t)i] >= strong) i += dir;   // off the signal we are on first
        for (; i >= 0 && i < n; i += dir)
            if (row_[(size_t)i] >= strong)
            {
                int peak = i;                                  // climb to the top of it
                while (peak + dir >= 0 && peak + dir < n && row_[(size_t)(peak + dir)] >= row_[(size_t)peak]) peak += dir;
                const int mode = getInt(kMode);
                const double side = mode == kUSB ? -1.0 : mode == kLSB ? 1.0 : 0.0;
                setTune(rowStartKHz_ + (peak + 0.5) * per + side, -1);
                return;
            }
        setTune(f + dir * 0.5 * rowSpanKHz_, -1);
    }

    // ----- Live: what this receiver hears now ---------------------------------------------------
    // One sweep of the waterfall across the receiver's range at zoom 3 (eight windows of 3.75 MHz,
    // 3.7 kHz a bin), two settled rows per window held at their peaks, then the waterfall is given
    // back: a few seconds, less than the scope holds it, and it brings the labels. Peaks 15 dB over
    // their window's median are kept, the strongest 64, each named for a label or preset under it,
    // else for its band, with a mode guessed from where it is.

    std::string rxKey() const { return rx_.host + ":" + std::to_string(rx_.port); }
    double surveySpan() const { return spanKHz(kLiveZoom, rx_.highKHz > 30500.0 ? 32000.0 : 30000.0); }
    void bumpLive() { liveSig_.fetch_add(1, std::memory_order_relaxed); }

    bool liveFresh(Clock::time_point now)
    {
        std::lock_guard<std::mutex> g(liveMu_);
        return !live_.empty() && liveFor_ == rxKey() && now - liveAt_ < kLiveFresh;
    }

    // tried on this receiver within the last while (a sweep that failed counts: no slot, no hammering)
    bool surveyedWithin(Clock::time_point now, Clock::duration d) const { return surveyTriedFor_ == rxKey() && now - surveyTriedAt_ < d; }

    void startSurvey(Clock::time_point now)
    {
        if (surveyOn_ || !connected_.load() || !onAir_) return;
        surveyTriedFor_ = rxKey();
        surveyTriedAt_ = now;
        if (now < wfRetryAt_)                                // refused or dropped a moment ago: not again yet
        {
            liveState_.store(kLiveNoSlot);
            bumpLive();
            setStatus("no waterfall slot: try another");
            surveyEnded_ = true;
            return;
        }
        const double span = surveySpan(), lo = rx_.lowKHz, hi = rx_.highKHz > 0.0 ? rx_.highKHz : 30000.0;
        surveyWins_.clear();
        for (int k = 0; k < 8; ++k)
            if ((k + 1) * span > lo && k * span < hi) surveyWins_.push_back(k);
        surveyPeaks_.clear();
        surveyAt_ = 0;
        surveySent_ = false;
        surveyOn_ = true;
        surveyWas_ = scopeUntil_;
        scopeUntil_ = std::max(scopeUntil_, now + seconds(15));
        surveyEnd_ = now + seconds(12);                      // inside the 15 s it holds the waterfall for
        liveProgress_.store((int)surveyWins_.size());
        liveState_.store(kLiveScanning);
        bumpLive();
        setStatus("scanning the band");
    }

    void surveyStep(Clock::time_point now)
    {
        if (now >= surveyEnd_ || surveyAt_ >= (int)surveyWins_.size()) { endSurvey(now); return; }
        if (!surveySent_)
        {
            char b[64];
            std::snprintf(b, sizeof(b), "SET zoom=%d cf=%.3f", kLiveZoom, (surveyWins_[(size_t)surveyAt_] + 0.5) * surveySpan());
            if (surveyAt_ == 0) wf_.sendText("SET wf_speed=4");   // fast rows for the sweep
            wf_.sendText(b);
            surveySent_ = true;
            surveyWinAt_ = now;
            surveyRows_ = 0;
            surveyMax_.clear();
            wfZoom_ = -1;                                       // the scope's own view is sent again after
        }
        else if (now - surveyWinAt_ > milliseconds(1500))
        {
            // no row for the first window: this channel has no waterfall (many Kiwis have fewer than slots)
            if (surveyAt_ == 0 && surveyRows_ == 0) { noRows(now); return; }
            nextWindow();                                       // else on to the next
        }
    }

    void noRows(Clock::time_point now)
    {
        surveyOn_ = false;
        scopeUntil_ = surveyWas_;
        {
            std::lock_guard<std::mutex> g(liveMu_);
            livePeaks_.clear();
            live_.clear();
            liveFor_ = rxKey();
            liveAt_ = now;
        }
        liveState_.store(kLiveNoSlot);
        bumpLive();
        setStatus("no waterfall here: try another");
        surveyEnded_ = true;
    }

    // swept (with or without a catch) in the last ten minutes: Dig does not sweep again
    bool liveSwept(Clock::time_point now)
    {
        std::lock_guard<std::mutex> g(liveMu_);
        return liveFor_ == rxKey() && now - liveAt_ < kLiveFresh;
    }

    void nextWindow()
    {
        ++surveyAt_;
        surveySent_ = false;
        liveProgress_.store(surveyAt_ * 16 + (int)surveyWins_.size());
        bumpLive();
    }

    void surveyRow(double startKHz, int zoom, const uint8_t* d, size_t n)
    {
        if (!surveySent_ || zoom != kLiveZoom || n < 64) return;
        const double span = surveySpan();
        if (std::fabs(startKHz - surveyWins_[(size_t)surveyAt_] * span) > 0.02 * span) return;   // a row from before the move
        if (++surveyRows_ == 1) return;                        // the first may straddle the move
        if (surveyMax_.size() != n) surveyMax_.assign(d, d + n);
        else for (size_t i = 0; i < n; ++i) surveyMax_[i] = std::max(surveyMax_[i], d[i]);
        if (surveyRows_ < 3) return;
        peaks(startKHz, span);
        nextWindow();
    }

    // the window's tops 15 dB over its median, placed between bins by a parabola through three
    void peaks(double start, double span)
    {
        const int n = (int)surveyMax_.size();
        std::vector<uint8_t> sorted = surveyMax_;
        std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
        const int floor = sorted[(size_t)n / 2], strong = floor + 15;
        const double per = span / n, lo = std::max(10.0, rx_.lowKHz), hi = rx_.highKHz > 0.0 ? rx_.highKHz : 32000.0;
        for (int i = 1; i + 1 < n; ++i)
        {
            const int v = surveyMax_[(size_t)i], a = surveyMax_[(size_t)i - 1], c = surveyMax_[(size_t)i + 1];
            if (v < strong || v < a || v <= c) continue;
            const double den = a - 2.0 * v + c, off = den < 0.0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
            const double f = start + (i + 0.5 + off) * per;
            if (f >= lo && f <= hi) surveyPeaks_.push_back({f, v - floor});
        }
    }

    void endSurvey(Clock::time_point now)
    {
        surveyOn_ = false;
        if (wfOpen_ && wfReady_) wf_.sendText("SET wf_speed=2");
        scopeUntil_ = surveyWas_;                            // parked again unless the scope is in use
        std::sort(surveyPeaks_.begin(), surveyPeaks_.end(), [](const LivePeak& a, const LivePeak& b) { return a.over > b.over; });
        std::vector<LivePeak> keep;
        const double apart = 2.5 * surveySpan() / 1024.0;     // one entry per signal
        for (const LivePeak& p : surveyPeaks_)
        {
            if ((int)keep.size() >= kLiveMax) break;
            bool crowded = false;
            for (const LivePeak& k : keep) crowded = crowded || std::fabs(k.kHz - p.kHz) < apart;
            if (!crowded) keep.push_back(p);
        }
        const int n = (int)keep.size();
        {
            std::lock_guard<std::mutex> g(liveMu_);
            livePeaks_ = std::move(keep);
            liveFor_ = rxKey();
            liveAt_ = now;
        }
        nameLive();
        liveState_.store(kLiveDone);
        bumpLive();
        setStatus(n ? "scanned: " + std::to_string(n) + " signals" : "scanned: nothing over the noise");
        surveyEnded_ = true;
    }

    // the waterfall went (refused, dropped, Listen off) before the sweep was done
    void abortSurvey()
    {
        surveyOn_ = false;
        liveState_.store(noWf_.load() ? kLiveNoSlot : kLiveNone);
        bumpLive();
        surveyEnded_ = true;
    }

    // the sweep's peaks as rows: on a label or preset, its name, frequency and mode; else the band's
    // name, a mode from the band, and AM snapped to its channel grid
    void nameLive()
    {
        std::vector<LivePeak> peaks;
        {
            std::lock_guard<std::mutex> g(liveMu_);
            peaks = livePeaks_;
            if (peaks.empty()) live_.clear();
        }
        if (peaks.empty()) { bumpLive(); return; }
        std::vector<Label> labels;
        {
            std::lock_guard<std::mutex> g(labelsMu_);
            labels = labels_;
        }
        const double tol = std::max(1.0, 0.6 * surveySpan() / 1024.0);   // within the bin it came from
        std::vector<Live> out;
        for (const LivePeak& p : peaks)
        {
            Live e{p.kHz, guessMode(p.kHz), p.over, ""};
            double best = tol;
            for (const Label& l : labels)
                if (std::fabs(l.kHz - p.kHz) < best) { best = std::fabs(l.kHz - p.kHz); e.kHz = l.kHz; e.mode = l.mode; e.name = l.name; }
            if (e.name.empty())
                for (int k = 1; k < kNumStations; ++k)
                    if (!std::strstr(kStations[k].name, " band") && std::fabs(kStations[k].kHz - p.kHz) < best)
                    {
                        best = std::fabs(kStations[k].kHz - p.kHz);
                        e.kHz = kStations[k].kHz;
                        e.mode = kStations[k].mode;
                        e.name = kStations[k].name;
                    }
            if (e.name.empty())                                 // in USB or LSB the peak is the voice, a kilohertz from its carrier
            {
                e.kHz = snapChannel(p.kHz + (e.mode == kUSB ? -1.0 : e.mode == kLSB ? 1.0 : 0.0), e.mode);
                e.name = bandName(p.kHz);
            }
            bool twice = false;
            for (const Live& o : out) twice = twice || std::fabs(o.kHz - e.kHz) < 0.05;
            if (!twice) out.push_back(e);
        }
        {
            std::lock_guard<std::mutex> g(liveMu_);
            live_ = std::move(out);
        }
        bumpLive();
    }

    // broadcast bands AM, ham voice LSB under 10 MHz and USB over, Morse at the bottoms, FT8 USB
    static int guessMode(double f)
    {
        struct R { double lo, hi; int mode; };
        static const R kR[] = {
            {0, 150, kCW}, {150, 1800, kAM}, {1800, 1838, kCW}, {1838, 1845, kUSB}, {1845, 2000, kLSB},
            {2300, 2500, kAM}, {3200, 3400, kAM}, {3500, 3570, kCW}, {3570, 3600, kUSB}, {3600, 4000, kLSB},
            {4750, 5060, kAM}, {5330, 5410, kUSB}, {5900, 6200, kAM}, {7000, 7040, kCW}, {7040, 7080, kUSB},
            {7080, 7200, kLSB}, {7200, 7450, kAM}, {9400, 9900, kAM}, {10100, 10130, kCW}, {10130, 10150, kUSB},
            {11600, 12100, kAM}, {13570, 13870, kAM}, {14000, 14070, kCW}, {15100, 15800, kAM}, {17480, 17900, kAM},
            {18068, 18095, kCW}, {18900, 19020, kAM}, {21000, 21070, kCW}, {21450, 21850, kAM}, {24890, 24915, kCW},
            {25670, 26100, kAM}, {26960, 27410, kAM}, {28000, 28070, kCW}, {29500, 29700, kNBFM}};
        for (const R& r : kR)
            if (f >= r.lo && f < r.hi) return r.mode;
        return kUSB;
    }

    // AM broadcasters sit on channels: 5 kHz on shortwave, 9 on long and medium wave (10 in the Americas)
    double snapChannel(double f, int mode) const
    {
        if (mode != kAM) return std::round(f * 10.0) / 10.0;
        const bool americas = rx_.region == kiwi::kNAmerica || rx_.region == kiwi::kSAmerica;
        const double grid = f >= 2300.0 ? 5.0 : f >= 520.0 && f <= 1710.0 && americas ? 10.0 : 9.0;
        return std::round(f / grid) * grid;
    }

    static std::string bandName(double f)
    {
        struct B { double lo, hi; const char* name; };
        static const B kB[] = {
            {0, 150, "VLF"}, {150, 285, "longwave"}, {520, 1710, "mediumwave"}, {1800, 2000, "160m ham"},
            {2300, 2495, "120m band"}, {3200, 3400, "90m band"}, {3500, 4000, "80m ham"}, {4750, 5060, "60m band"},
            {5330, 5410, "60m ham"}, {5900, 6200, "49m band"}, {7000, 7200, "40m ham"}, {7200, 7450, "41m band"},
            {9400, 9900, "31m band"}, {10100, 10150, "30m ham"}, {11600, 12100, "25m band"}, {13570, 13870, "22m band"},
            {14000, 14350, "20m ham"}, {15100, 15800, "19m band"}, {17480, 17900, "16m band"}, {18068, 18168, "17m ham"},
            {18900, 19020, "15m band"}, {21000, 21450, "15m ham"}, {21450, 21850, "13m band"}, {24890, 24990, "12m ham"},
            {25670, 26100, "11m band"}, {26960, 27410, "CB"}, {28000, 29700, "10m ham"}};
        for (const B& b : kB)
            if (f >= b.lo && f < b.hi) return b.name;
        return "utility";
    }

    // the i-th of the sweep's entries in category cat
    bool liveAt(int i, Live& out, int cat)
    {
        std::lock_guard<std::mutex> g(liveMu_);
        for (const Live& e : live_)
            if (freqIn(e.kHz, cat) && i-- == 0) { out = e; return true; }
        return false;
    }

    static std::string liveText(const Live& e)
    {
        char b[48];
        std::snprintf(b, sizeof(b), "%.1f +%d dB", e.kHz, e.over);
        return e.name.empty() ? std::string(b) : e.name + " +" + std::to_string(e.over) + " dB";
    }

    // the next Live entry above or below the dial
    bool seekLive(int dir)
    {
        const double f = dialKHz(getInt(kMHz), get(kKHz), get(kFine));
        Live best{};
        bool found = false;
        {
            std::lock_guard<std::mutex> g(liveMu_);
            for (const Live& e : live_)
                if ((dir > 0 && e.kHz > f + 0.5 && (!found || e.kHz < best.kHz)) || (dir < 0 && e.kHz < f - 0.5 && (!found || e.kHz > best.kHz)))
                {
                    best = e;
                    found = true;
                }
        }
        if (!found) return false;
        setTune(best.kHz, best.mode);
        setStatus(liveText(best));
        return true;
    }

    // ----- the list page ------------------------------------------------------------------------
    // Eight rows over one of three lists: the stations of the current category, the receiver's own
    // labels by frequency, or the receivers of the current region. Scroll is the top row; a tapped
    // row is where to go. The rows' text comes from formatValue, so when it changes the worker
    // asks the host to redraw (their values only flip when tapped).

    static int stationAt(int cat, int i)
    {
        for (int k = 1, n = 0; k < kNumStations; ++k)
            if (stationIn(k, cat) && n++ == i) return k;
        return -1;
    }

    // how many entries the list has, and which one is where the radio is (-1: none)
    int listCount(int kind, int& current, int cat)
    {
        current = -1;
        if (kind == kListStations)
        {
            const int st = getInt(kStation);
            int n = 0;
            for (int k = 1; k < kNumStations; ++k)
                if (stationIn(k, cat)) { if (k == st) current = n; ++n; }
            return n;
        }
        if (kind == kListLive)
        {
            const double f = dialKHz(getInt(kMHz), get(kKHz), get(kFine));
            std::lock_guard<std::mutex> g(liveMu_);
            double best = 1.0;
            int n = 0;
            for (const Live& e : live_)
            {
                if (!freqIn(e.kHz, cat)) continue;
                if (std::fabs(e.kHz - f) < best) { best = std::fabs(e.kHz - f); current = n; }
                ++n;
            }
            return n;
        }
        if (kind == kListLabels)
        {
            const double f = dialKHz(getInt(kMHz), get(kKHz), get(kFine));
            std::lock_guard<std::mutex> g(labelsMu_);
            double best = 0.05;
            for (int k = 0; k < (int)labels_.size(); ++k)
                if (std::fabs(labels_[(size_t)k].kHz - f) < best) { best = std::fabs(labels_[(size_t)k].kHz - f); current = k; }
            return (int)labels_.size();
        }
        std::lock_guard<std::mutex> g(listMu_);
        const int n = (int)byRegion_[(size_t)getInt(kRegion)].size();
        if (n > 0) current = getInt(kReceiver) % n;
        return n;
    }

    int listTop(int n) { return std::clamp(getInt(kScroll), 0, std::max(0, n - kRows)); }

    // minutes since the sweep, -1 none
    int liveAge()
    {
        std::lock_guard<std::mutex> g(liveMu_);
        if (liveFor_.empty()) return -1;
        return (int)std::chrono::duration_cast<std::chrono::minutes>(Clock::now() - liveAt_).count();
    }

    // "Ticks & Time  1-8 of 13": the list and where the rows are in it
    std::string listHeader()
    {
        const int kind = getInt(kList), cat = std::clamp(getInt(kCategory), 0, kCatAny);
        int cur;
        const int n = listCount(kind, cur, cat), top = listTop(n);
        const int age = kind == kListLive ? liveAge() : -1;
        const std::string live = age < 0 ? std::string("Live") : age == 0 ? std::string("Live now") : "Live " + std::to_string(age) + " min ago";
        const std::string what = kind == kListStations ? (cat == kCatAny ? "All stations" : catName(cat))
                               : kind == kListLabels ? "Labels"
                               : kind == kListLive ? (cat == kCatAny ? live : std::string(catName(cat)) + " live")
                               : kRegionNames[std::clamp(getInt(kRegion), 0, kiwi::kNumRegions - 1)];
        if (n == 0) return what;
        char b[64];
        std::snprintf(b, sizeof(b), "%s  %d-%d of %d", what.c_str(), top + 1, std::min(n, top + kRows), n);
        if (std::strlen(b) > 31) std::snprintf(b, sizeof(b), "%s %d-%d/%d", what.c_str(), top + 1, std::min(n, top + kRows), n);
        return b;
    }

    std::string rowText(int r)
    {
        const int kind = getInt(kList), cat = getInt(kCategory);   // read once: the worker may change it
        int cur;
        const int n = listCount(kind, cur, cat), i = listTop(n) + r;
        if (n == 0)
        {
            if (r != 0 || kind == kListStations) return "";
            if (kind == kListLabels) return !connected_.load() ? "Listen to load its labels" : noWf_.load() ? "no waterfall slot: no labels" : "fetching its labels";
            if (kind == kListLive)
            {
                const int st = liveState_.load(), pr = liveProgress_.load();
                if (!connected_.load()) return "Listen to scan the band";
                if (st == kLiveScanning) return "scanning the band " + std::to_string(pr / 16) + "/" + std::to_string(pr % 16);
                if (st == kLiveNoSlot) return "no waterfall here: try another";
                if (st == kLiveDone) return cat == kCatAny ? "nothing over the noise" : "none on the air: try Dig";
                return "scanning once on air";
            }
            return "loading the list";
        }
        if (i >= n) return "";
        const char* mark = i == cur ? "> " : "";
        char b[128];
        if (kind == kListStations)
        {
            const int k = stationAt(cat, i);
            if (k < 0) return "";
            const Station& s = kStations[k];
            if (std::strstr(s.name, std::to_string((int)s.kHz).c_str()))   // "WRMI 5950" says it already
                std::snprintf(b, sizeof(b), "%s%s  %s", mark, s.name, kModeShown[s.mode]);
            else
                std::snprintf(b, sizeof(b), "%s%s  %.1f %s", mark, s.name, s.kHz, kModeShown[s.mode]);
            const std::string with = std::string(b) + "  " + soundOf(k);   // what it sounds like, when it fits whole
            if (with.size() <= 31) return with;
        }
        else if (kind == kListLabels)
        {
            Label l;
            if (!labelAt(i, l)) return "";
            std::snprintf(b, sizeof(b), "%s%.1f %s", mark, l.kHz, l.name.c_str());
        }
        else if (kind == kListLive)
        {
            Live e{};
            if (!liveAt(i, e, cat)) return "";
            std::snprintf(b, sizeof(b), "%s%.1f %s +%d %s", mark, e.kHz, kModeShown[std::clamp(e.mode, 0, 4)], e.over, e.name.c_str());
        }
        else
        {
            std::lock_guard<std::mutex> g(listMu_);
            const std::vector<int>& idx = byRegion_[(size_t)getInt(kRegion)];
            if (i >= (int)idx.size()) return "";
            const kiwi::Receiver& rx = list_[(size_t)idx[(size_t)i]];
            std::string busy = rx.usersMax > 0 ? std::to_string(rx.users) + "/" + std::to_string(rx.usersMax) + " " : "";
            std::snprintf(b, sizeof(b), "%s%s%s", mark, busy.c_str(), rx.label.c_str());
        }
        return kiwi::cut(b, 31);   // the most the host takes
    }

    void setScroll(int top)
    {
        if (top == getInt(kScroll)) return;
        set(kScroll, (float)top);
        notifyHostParameterChanged(kScroll);
    }

    void scrollRows(int d)
    {
        int cur;
        const int n = listCount(getInt(kList), cur, getInt(kCategory));
        setScroll(std::clamp(listTop(n) + d, 0, std::max(0, n - kRows)));
    }

    // Page Up at the top of Live sweeps again (a minute apart at the most)
    void onPage(int dir, Clock::time_point now)
    {
        const int kind = getInt(kList);
        int cur;
        const int n = listCount(kind, cur, getInt(kCategory));
        if (kind == kListLive && dir < 0 && listTop(n) == 0 && onAir_)
        {
            if (surveyedWithin(now, seconds(60))) { if (!surveyOn_) setStatus("scanned under a minute ago"); }
            else startSurvey(now);
            return;
        }
        setScroll(std::clamp(listTop(n) + dir * kRows, 0, std::max(0, n - kRows)));
    }

    void onRow(int r)
    {
        const int kind = getInt(kList), cat = getInt(kCategory);
        int cur;
        const int n = listCount(kind, cur, cat), i = listTop(n) + r;
        if (i >= n) return;
        if (kind == kListStations)
        {
            const int k = stationAt(cat, i);
            if (k < 0) return;
            set(kStation, (float)k);
            notifyHostParameterChanged(kStation);
            tuneStation(k);
        }
        else if (kind == kListLabels)
        {
            Label l;
            if (!labelAt(i, l)) return;
            ourLabel_.store(i);
            set(kLabel, (float)i);
            notifyHostParameterChanged(kLabel);
            setTune(l.kHz, l.mode);
        }
        else if (kind == kListLive)
        {
            Live e{};
            if (!liveAt(i, e, cat)) return;
            setTune(e.kHz, e.mode);
            setStatus(liveText(e));
        }
        else
        {
            set(kReceiver, (float)i);
            notifyHostParameterChanged(kReceiver);
        }
    }

    // Another list, category or region: show where the radio is. The radio moved off the rows by
    // some other way (a knob, Seek, Dig): follow it. The rows' text changed: redraw, at most
    // about three times a second, since a redraw rebuilds the page.
    void listView(Clock::time_point now)
    {
        const int kind = getInt(kList), cat = getInt(kCategory), region = getInt(kRegion);
        int cur;
        const int n = listCount(kind, cur, cat);
        const bool newView = kind != viewKind_ || cat != viewCat_ || region != viewRegion_ || (n > 0) != (viewN_ > 0);
        if (newView && kind == kListLabels && n == 0 && connected_.load()) wantScope(now);   // the labels come with the waterfall
        // Live with no sweep of this receiver yet: one, once on air (a failed one waits 3 minutes)
        if (kind == kListLive && onAir_ && now - onAirSince_ >= seconds(1) && !surveyOn_ && !surveyedWithin(now, std::chrono::minutes(3)))
        {
            bool have;
            {
                std::lock_guard<std::mutex> g(liveMu_);
                have = liveFor_ == rxKey();
            }
            if (!have) startSurvey(now);
        }
        int want = -1;
        if (newView || cur != viewCur_)
        {
            int at = cur;
            if (at < 0 && kind == kListLabels && newView)   // between labels: the first one above the dial
            {
                const double f = dialKHz(getInt(kMHz), get(kKHz), get(kFine));
                std::lock_guard<std::mutex> g(labelsMu_);
                at = (int)(std::lower_bound(labels_.begin(), labels_.end(), f, [](const Label& l, double v) { return l.kHz < v; }) - labels_.begin());
            }
            const int top = listTop(n);
            if (at >= 0 && (newView || at < top || at >= top + kRows)) want = std::clamp(at - 2, 0, std::max(0, n - kRows));
            else if (at < 0 && newView) want = 0;
        }
        viewKind_ = kind;
        viewCat_ = cat;
        viewRegion_ = region;
        viewN_ = n;
        viewCur_ = cur;
        if (want >= 0) setScroll(want);
        else if (n > 0 && getInt(kScroll) > std::max(0, n - kRows)) setScroll(std::max(0, n - kRows));   // the knob past the end: no slack to wind back
        const int64_t sig = (((((int64_t)kind * 64 + cat) * 8 + region) * 2048 + n) * 2048 + listTop(n) + 1031 * (int64_t)(cur + 1) + (connected_.load() ? 7 : 0) + (noWf_.load() ? 13 : 0))
                            * 1000003 + liveSig_.load(std::memory_order_relaxed) * 131 + (kind == kListLive ? liveAge() : 0);
        if (sig != viewSig_) { viewSig_ = sig; viewDirty_ = true; }
        if (viewDirty_ && now - lastView_ >= milliseconds(300))
        {
            viewDirty_ = false;
            lastView_ = now;
            requestDisplayUpdate();
        }
    }

    // ----- Back and Dig -------------------------------------------------------------------------
    // Back: the radio keeps the places you stayed at for 3 s or more (frequency, mode, receiver),
    // 32 of them. Back goes to the last one, and again to the one before; tuning somewhere new
    // after going back forgets the ones after it, as a browser does.
    struct Spot { double kHz = -1.0; int mode = -1; std::string rx; };
    static bool same(const Spot& a, const Spot& b) { return std::fabs(a.kHz - b.kHz) < 0.05 && a.mode == b.mode && a.rx == b.rx; }

    Spot here(const std::string& key) { return {dialKHz(getInt(kMHz), get(kKHz), get(kFine)), getInt(kMode), key}; }

    void history(const std::string& key, Clock::time_point now)
    {
        const Spot s = here(key);
        if (!same(s, seen_)) { seen_ = s; seenSince_ = now; return; }
        if (key.empty() || now - seenSince_ < seconds(3)) return;
        if (histPos_ >= 0 && same(s, hist_[(size_t)histPos_])) return;
        hist_.resize((size_t)(histPos_ + 1));
        hist_.push_back(s);
        if (hist_.size() > 32) hist_.erase(hist_.begin());
        histPos_ = (int)hist_.size() - 1;
    }

    void onBack(const std::string& key)
    {
        if (histPos_ < 0) { setStatus("nothing to go back to yet"); return; }
        if (same(here(key), hist_[(size_t)histPos_]))
        {
            if (histPos_ == 0) { setStatus("that is as far back as it goes"); return; }
            --histPos_;
        }
        const Spot s = hist_[(size_t)histPos_];
        setTune(s.kHz, s.mode);
        if (s.rx != key) pointAt(s.rx);
        char b[64];
        std::snprintf(b, sizeof(b), "back to %.1f kHz", s.kHz);
        setStatus(b);
    }

    // Dig: somewhere at random, for when you do not know what you want yet. Mostly one of the
    // receiver's own labels (real stations, nearby ones), else one of the presets. On air, each
    // place gets a listen on the S-meter, and dead air is dug again, up to kDigTries places; none
    // on the air: back to the one that stood out most.
    static constexpr int kDigTries = 5;

    // A band's noise, learned while on air: the quietest the S-meter has read there, per kHz of
    // passband (AM wide hears 14 dB more noise than CW narrow on the same empty band). Noise falls
    // 20 to 30 dB from 2 to 25 MHz, so each band keeps its own. It creeps up 1 dB a minute, so a
    // noise that went away is forgotten.
    static constexpr int kBands = 6;
    static int bandOf(double kHz) { return kHz < 500 ? 0 : kHz < 2000 ? 1 : kHz < 5000 ? 2 : kHz < 10000 ? 3 : kHz < 18000 ? 4 : 5; }

    double perKHz()
    {
        int lo, hi;
        passband(lo, hi);
        return smeterDbm_ - 10.0 * std::log10(std::max(0.05, (hi - lo) / 1000.0));
    }

    // a band's noise as known: learned, else a guess
    double floorOf(int b) const
    {
        static const double kGuess[kBands] = {-95.0, -100.0, -105.0, -110.0, -113.0, -115.0};   // dBm per kHz, low bands noisier
        return floor_[b] < 1e8 ? floor_[b] : kGuess[b];
    }

    // how far over its band's noise a reading stands
    double overNoise(double kHz, double n) { return n - floorOf(bandOf(kHz)); }

    // the S-meter reads this receiver, here: 2 s on air (the first SND frames), inside its range
    bool meterGood(Clock::time_point now) const { return onAir_ && !outside_ && now - onAirSince_ >= seconds(2); }

    void lower(int b, double n)
    {
        floor_[b] = std::min(floor_[b], n);
        floorMin_[b] = std::min(floorMin_[b], n);
    }

    void learnNoise(const std::string& key, Clock::time_point now)
    {
        if (now - floorCreep_ >= seconds(60))
        {
            floorCreep_ = now;
            for (int b = 0; b < kBands; ++b) if (floor_[b] < 1e8) floor_[b] = std::min(floor_[b] + 1.0, floorMin_[b] + 10.0);
        }
        const Spot h = here(key);
        if (!same(h, noiseAt_)) { noiseAt_ = h; noiseSince_ = now; return; }
        if (!meterGood(now) || digTry_ != 0 || now - noiseSince_ < milliseconds(1500)) return;   // a dig feeds it its own way
        // A band not heard yet takes at most its guess + 10: sitting on a station first must not
        // teach the band that the station is its noise. A quieter place lowers it from there.
        const int b = bandOf(h.kHz);
        lower(b, std::min(perKHz(), floorOf(b) + 10.0));
    }

    // the SIGNAL lamp: the dial 10 dB over its band's noise (off again under 7), after 0.7 s there
    void lamp(Clock::time_point now)
    {
        if (now - lastLamp_ < milliseconds(40)) return;
        lastLamp_ = now;
        const bool was = get(kLamp) > 0.5f;
        bool on = false;
        if (meterGood(now) && now - noiseSince_ >= milliseconds(700))
        {
            lampDbm_ = lampFresh_ ? perKHz() : lampDbm_ + 0.2 * (perKHz() - lampDbm_);
            lampFresh_ = false;
            const double over = overNoise(noiseAt_.kHz, lampDbm_);
            on = was ? over >= 7.0 : over >= 10.0;
        }
        else lampFresh_ = true;
        if (on != was) { set(kLamp, on ? 1.0f : 0.0f); notifyHostParameterChanged(kLamp); }
    }

    std::string digPick(Clock::time_point now)
    {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const int cat = getInt(kCategory);
        digFromLive_ = false;
        if (connected_.load() && liveFresh(now))             // what is on the air now, the strong more often
        {
            std::vector<Live> l;
            {
                std::lock_guard<std::mutex> g(liveMu_);
                l = live_;
            }
            const double f = dialKHz(getInt(kMHz), get(kKHz), get(kFine));
            std::vector<double> w(l.size());
            double total = 0.0;
            for (size_t i = 0; i < l.size(); ++i)
            {
                bool tried = std::fabs(l[i].kHz - f) < 1.0 || !freqIn(l[i].kHz, cat) || l[i].kHz < rx_.lowKHz || (rx_.highKHz > 0.0 && l[i].kHz > rx_.highKHz);
                for (double t : digTried_) tried = tried || std::fabs(l[i].kHz - t) < 1.0;
                w[i] = tried ? 0.0 : std::max(5, l[i].over - 10);
                total += w[i];
            }
            double r = u(rng_) * total;
            for (size_t i = 0; total > 0.0 && i < l.size(); ++i)
            {
                if (w[i] <= 0.0 || (r -= w[i]) > 0.0) continue;
                setTune(l[i].kHz, l[i].mode);
                digTried_.push_back(l[i].kHz);
                digFromLive_ = true;
                char b[32];
                std::snprintf(b, sizeof(b), "%.1f kHz", l[i].kHz);
                return l[i].name.empty() ? std::string(b) : l[i].name;
            }
        }
        std::vector<int> in;                                    // the labels in the category
        bool none;
        {
            std::lock_guard<std::mutex> g(labelsMu_);
            none = labels_.empty();
            for (int k = 0; k < (int)labels_.size(); ++k) if (freqIn(labels_[(size_t)k].kHz, cat)) in.push_back(k);
        }
        if (connected_.load() && none) wantScope(now);         // so the next dig has the labels
        const int n = (int)in.size();
        std::vector<int> presets;
        for (int k = 1; k < kNumStations; ++k) if (stationIn(k, cat) && !std::strstr(kStations[k].name, " band")) presets.push_back(k);
        const double lo = connected_.load() ? rx_.lowKHz : 0.0, hi = connected_.load() && rx_.highKHz > 0.0 ? rx_.highKHz : 30000.0;
        for (int tries = 0; tries < 16; ++tries)                // a place this receiver hears; a try is not spent on one it does not
        {
            Label l;
            if (n > 0 && (presets.empty() || u(rng_) < 0.7))
            {
                const int k = in[(size_t)std::min(n - 1, (int)(u(rng_) * n))];
                if (labelAt(k, l) && l.kHz >= lo && l.kHz <= hi)
                {
                    ourLabel_.store(k);
                    set(kLabel, (float)k);
                    notifyHostParameterChanged(kLabel);
                    setTune(l.kHz, l.mode);
                    return l.name;
                }
                continue;
            }
            if (presets.empty()) break;
            const int k = presets[(size_t)std::min((int)presets.size() - 1, (int)(u(rng_) * presets.size()))];
            const Station& st = kStations[k];
            if (st.kHz < lo || st.kHz > hi) continue;          // a band's preset is its edge, mostly empty: left out above
            set(kStation, (float)k);
            notifyHostParameterChanged(kStation);
            tuneStation(k);
            return st.name;
        }
        return "";
    }

    // on air with no fresh sweep: sweep first (a few seconds), then dig in what it found
    void onDig(Clock::time_point now)
    {
        digTry_ = 0;
        digBestOver_ = -1e9;
        digTried_.clear();
        if (onAir_ && !liveSwept(now) && !surveyedWithin(now, seconds(60))) startSurvey(now);
        if (surveyOn_) { digAfterSurvey_ = true; digFrom_ = here(rxKey()); return; }
        digNext(now);
    }

    void digNext(Clock::time_point now)
    {
        digName_ = digPick(now);
        ++digTry_;
        bool justSwept = false;                               // the sweep heard it seconds ago: no second opinion from the S-meter
        if (digFromLive_)
        {
            std::lock_guard<std::mutex> g(liveMu_);
            justSwept = now - liveAt_ < seconds(60);
        }
        if (digName_.empty() || !onAir_ || justSwept) { digTry_ = 0; setStatus(digName_.empty() ? "nowhere to dig" : "dug up " + digName_); return; }
        digSpot_ = here(rx_.host + ":" + std::to_string(rx_.port));
        digJudge_ = now + milliseconds(1400);               // 0.7 s for the receiver and its AGC, then 0.7 s heard
        digSum_ = 0.0;
        digN_ = 0;
        setStatus("digging " + std::to_string(digTry_) + "/" + std::to_string(kDigTries));
    }

    // Each place is heard for 0.7 s and passes 10 dB over its band's noise. A place that does not
    // pass teaches the band its noise.
    void digListen(const std::string& key, Clock::time_point now)
    {
        if (digTry_ == 0) return;
        if (!connected_.load() || !onAir_ || !same(here(key), digSpot_))   // turned away: the dig is over
        {
            digTry_ = 0;
            if (onAir_) setStatus(onAirText());
            return;
        }
        if (!meterGood(now)) { digJudge_ = std::max(digJudge_, onAirSince_ + milliseconds(2700)); return; }
        if (now < digJudge_ - milliseconds(700)) return;
        if (now < digJudge_) { digSum_ += perKHz(); ++digN_; return; }
        const double n = digN_ ? digSum_ / digN_ : perKHz(), over = overNoise(digSpot_.kHz, n);
        if (over > digBestOver_) { digBestOver_ = over; digBest_ = digSpot_; digBestName_ = digName_; }
        if (over >= 10.0) { digTry_ = 0; setStatus("dug up " + digName_); return; }
        lower(bandOf(digSpot_.kHz), n);
        if (digTry_ < kDigTries) { digNext(now); return; }
        digTry_ = 0;
        setTune(digBest_.kHz, digBest_.mode);
        setStatus("all quiet; best: " + digBestName_);
    }

    // ----- the receiver ---------------------------------------------------------------------------
    void tune()
    {
        const int mode = std::clamp(getInt(kMode), 0, 4);
        const double dial = dialKHz(getInt(kMHz), get(kKHz), get(kFine));
        int lo, hi;
        passband(lo, hi);
        if (dial == tunedKHz_ && mode == tunedMode_ && lo == tunedLo_ && hi == tunedHi_) return;
        char b[128];
        std::snprintf(b, sizeof(b), "SET mod=%s low_cut=%d high_cut=%d freq=%.3f", kModeName[mode], lo, hi,
                      mode == kCW ? dial - kCwPitchKHz : dial);
        ws_.sendText(b);
        tunedKHz_ = dial;
        tunedMode_ = mode;
        tunedLo_ = lo;
        tunedHi_ = hi;
        // a receiver that does not cover the dial: say so, once, and take it back when it does
        const bool outside = dial < rx_.lowKHz - 0.5 || dial > rx_.highKHz + 0.5;
        if (outside != outside_)
        {
            outside_ = outside;
            char s[96];
            std::snprintf(s, sizeof(s), "it hears only %.1f-%.1f MHz", rx_.lowKHz / 1000.0, rx_.highKHz / 1000.0);
            setStatus(outside ? std::string(s) : onAirText());
        }
    }

    void agc()
    {
        const int a = std::clamp(getInt(kAgc), 0, 3), gain = (int)std::lround(get(kGain));
        if (a == agcSent_ && (a != 3 || gain == gainSent_)) return;
        char b[128];
        if (a == 3) std::snprintf(b, sizeof(b), "SET agc=0 hang=0 thresh=-100 slope=6 decay=1000 manGain=%d", gain);
        else std::snprintf(b, sizeof(b), "SET agc=1 hang=0 thresh=-100 slope=6 decay=%d manGain=%d", kAgcDecayMs[a], gain);
        ws_.sendText(b);
        if ((a == 3) != (agcSent_ == 3)) notifyHostParameterChanged(kGain);   // its text says whether it is in use
        agcSent_ = a;
        gainSent_ = gain;
    }

    void disconnect(const std::string& why, int link = kLinkFault)
    {
        ws_.close();
        closeScope();
        connected_ = false;
        onAir_ = false;
        link_ = link;
        streamGen_.fetch_add(1);
        setStatus(why);
    }

    // 0 on the line, else what net reports: an HTTP status, -1 no answer, -2 name not found
    int connect()
    {
        link_ = kLinkCalling;
        setStatus("calling " + rx_.label);
        std::string host = rx_.host, rh;
        int port = rx_.port, rp = 0;
        ts_ = std::to_string((long long)std::time(nullptr));
        const std::string path = "/" + ts_ + "/SND";
        int r = ws_.connect(host, port, path, 5000, rh, rp);
        if (r >= 300 && r < 400 && !rh.empty())   // the kiwisdr.com proxy redirects to the Kiwi's own port
        {
            host = rh;
            port = rp ? rp : port;
            r = ws_.connect(host, port, path, 5000, rh, rp);
        }
        if (r != 0) return r;
        connHost_ = host;
        connPort_ = port;
        ws_.setTimeoutMs(10);   // drain what has come, then look round: frames come every 40 ms, so a longer wait never ends
        ws_.sendText("SET auth t=kiwi p=");
        const std::string key = rx_.host + ":" + std::to_string(rx_.port);
        if (key != labelsFor_)                                // another receiver: its own labels
        {
            labelsFor_ = key;
            labelsAsked_ = false;
            {
                std::lock_guard<std::mutex> g(labelsMu_);
                labels_.clear();
            }
            {
                std::lock_guard<std::mutex> g(liveMu_);
                live_.clear();
                livePeaks_.clear();
                liveFor_.clear();
            }
            liveState_.store(kLiveNone);
            bumpLive();
        }
        wfRetryAt_ = Clock::now();
        noWf_.store(false);
        setPeak(kScopeHint);
        adpcm_.reset();
        chain_.reset();
        tunedKHz_ = -1.0;
        tunedMode_ = tunedLo_ = tunedHi_ = agcSent_ = gainSent_ = -1;
        outside_ = false;
        idleLimit_ = dayLimit_ = 0;
        kicked_ = false;
        for (int b = 0; b < kBands; ++b) floor_[b] = floorMin_[b] = 1e9;   // another receiver, another noise
        smeterDbm_ = -127.0;
        haveRate_ = false;
        connected_ = true;
        lastKeepalive_ = Clock::now();
        lastAudio_ = lastKeepalive_;
        streamGen_.fetch_add(1);
        return 0;
    }

    // a call that failed: wait 5, 10, 20, 40 s, then give up on this receiver until Listen goes off and on
    void callFailed(const std::string& what, const std::string& key, Clock::time_point now)
    {
        link_ = kLinkFault;
        if (++fails_ >= kMaxTries)
        {
            blockedHost_ = key;
            setStatus("gave up: POWER off/on to retry");
            return;
        }
        const int wait = 5 << (fails_ - 1);
        retryAt_ = now + seconds(wait);
        setStatus(kiwi::cut(what, 20) + ", retry " + std::to_string(wait) + " s");
    }

    // the line went quiet or closed. After a while on air that is the receiver's doing (a time
    // limit, an admin, a preemption): it is not called again. Early on it is the network: back off.
    void lost(const std::string& what, const std::string& key, Clock::time_point now)
    {
        const bool ended = kicked_ || (onAir_ && now - onAirSince_ >= kSessionEnded);
        const auto mins = std::chrono::duration_cast<std::chrono::minutes>(now - onAirSince_).count();
        disconnect(what);
        if (!ended) { callFailed(what, key, now); return; }
        blockedHost_ = key;
        if (dayLimit_ > 0 && mins + 1 >= dayLimit_) setStatus("day limit (" + std::to_string(dayLimit_) + " min) used up");
        else if (idleLimit_ > 0) setStatus("idle limit (" + std::to_string(idleLimit_) + " min) ended it");
        else setStatus("the receiver ended the session");
    }

    void onMessage(const std::vector<uint8_t>& m)
    {
        if (m.size() < 4) return;
        if (m[0] == 'M' && m[1] == 'S' && m[2] == 'G')
        {
            std::string body(m.begin() + 4, m.end());
            size_t p = 0;
            while (p < body.size())
            {
                size_t e = body.find(' ', p);
                if (e == std::string::npos) e = body.size();
                const std::string kv = body.substr(p, e - p);
                p = e + 1;
                const size_t eq = kv.find('=');
                const std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
                if (k == "audio_rate") ws_.sendText("SET AR OK in=" + v + " out=44100");
                else if (k == "sample_rate")
                {
                    const double rate = std::atof(v.c_str());
                    if (!(rate >= 1000.0 && rate <= 100000.0)) { disconnect("bad sample rate"); return; }
                    srcRate_.store(rate);
                    haveRate_ = true;
                    ws_.sendText("SET squelch=0 max=0");
                    ws_.sendText("SET compression=0");
                    agc();
                    ws_.sendText("SET ident_user=skywave-mpc");
                    tune();
                    ws_.sendText("SET keepalive");
                    link_ = kLinkOnAir;
                    onAir_ = true;
                    onAirSince_ = Clock::now();
                    if (!outside_) setStatus(onAirText());
                }
                else if (k == "load_cfg")
                {
                    const std::string c = urlDecode(v);
                    idleLimit_ = cfgInt(c, "inactivity_timeout_mins");
                    dayLimit_ = cfgInt(c, "ip_limit_mins");
                    if (onAir_ && !outside_) setStatus(onAirText());
                }
                else if (k == "too_busy") { blockedHost_ = rx_.host + ":" + std::to_string(rx_.port); disconnect("busy: all slots taken"); return; }
                else if (k == "badp" && v != "0") { blockedHost_ = rx_.host + ":" + std::to_string(rx_.port); disconnect("password only"); return; }
                else if (k == "ip_limit" || k == "inactivity_timeout" || k == "password_timeout") kicked_ = true;   // a limit; it closes next
                else if (k == "client_public_ip") {}                                     // ours: never logged
                else if (k == "down") { blockedHost_ = rx_.host + ":" + std::to_string(rx_.port); disconnect("receiver down"); return; }
                else if (std::find(msgSeen_.begin(), msgSeen_.end(), k) == msgSeen_.end() && msgSeen_.size() < 256)
                {
                    msgSeen_.push_back(k);   // learn what receivers say, a time limit's warning above all
                    mpc::log("skywave: MSG %s", kv.substr(0, 96).c_str());
                }
            }
            return;
        }
        if (m[0] == 'S' && m[1] == 'N' && m[2] == 'D' && m.size() > 10 && haveRate_)
        {
            const uint8_t flags = m[3];
            const uint8_t* d = m.data() + 10;
            const size_t n = m.size() - 10;
            int64_t w = srcW_.load(std::memory_order_relaxed);
            int64_t room = kSrcLen - 64 - (w - srcR_.load(std::memory_order_relaxed));   // full: drop the rest
            const float blank = get(kBlanker) * 0.01f, denoise = get(kDenoise) * 0.01f;
            const bool notch = getInt(kNotch) == 1;
            auto push = [&](int16_t s) {
                const float v = chain_.process(s * (1.0f / 32768.0f), blank, notch, denoise);
                if (room-- > 0) srcRing_[(size_t)(w++ & (kSrcLen - 1))] = v;
            };
            if (flags & 0x08) return;                       // IQ: not asked for
            smeterDbm_ = ((m[8] << 8) | m[9]) * 0.1 - 127.0;     // big-endian, tenths of a dB above -127 dBm
            if (flags & 0x10) for (size_t i = 0; i < n; ++i) { push(adpcm_.decode(d[i] & 15)); push(adpcm_.decode(d[i] >> 4)); }
            else for (size_t i = 0; i + 1 < n; i += 2) push((int16_t)((d[i] << 8) | d[i + 1]));
            srcW_.store(w, std::memory_order_release);
            lastAudio_ = Clock::now();
        }
    }

    // the read-outs, about 25 times a second: set, and told to the host so the screen follows
    void readouts(const kiwi::Receiver* rx, Clock::time_point now)
    {
        if (now - lastReadout_ < milliseconds(40)) return;
        lastReadout_ = now;
        const double s = connected_ && haveRate_ ? std::clamp((smeterDbm_ + 127.0) / 94.0, 0.0, 1.0) : 0.0;
        signal_ = s > signal_ ? s : signal_ + 0.25 * (s - signal_);   // a meter's ballistics: up at once, down slower
        auto put = [&](int p, double v, double eps) {
            if (std::fabs(v - get(p)) < eps) return;
            set(p, (float)v);
            notifyHostParameterChanged(p);
        };
        put(kDial, std::clamp(dialKHz(getInt(kMHz), get(kKHz), get(kFine)) / 30000.0, 0.0, 1.0), 2e-7);   // 6 Hz: the readout shows Dial's text
        put(kSignal, signal_, 0.01);
        if (rx) { put(kLon, (rx->lon + 180.0) / 360.0, 1e-4); put(kLat, (rx->lat + 90.0) / 180.0, 1e-4); }
        put(kLink, link_, 0.5);
        const double c = dialKHz(getInt(kMHz), get(kKHz), get(kFine)), half = 0.5 * spanKHz(viewZoom());
        put(kScopeLo, std::clamp((c - half) / 30000.0, 0.0, 1.0), 1e-6);   // their text is the edge frequency
        put(kScopeHi, std::clamp((c + half) / 30000.0, 0.0, 1.0), 1e-6);
        // Tune's knob turns with the frequency, a turn for 32 of the current Step: a new Step leaves it where it is
        const double step = kStepHz[std::clamp(getInt(kStep), 0, kNumSteps - 1)] / 1000.0;
        if (tuneKnobKHz_ < 0.0) tuneKnobKHz_ = c;
        tuneTurns_ += (c - tuneKnobKHz_) / (32.0 * step);
        tuneTurns_ -= std::floor(tuneTurns_);
        tuneKnobKHz_ = c;
        put(kTunePos, tuneTurns_, 1e-3);
        int lo, hi;
        passband(lo, hi);
        // the passband's edges over the scope, as shares of its width round the dial (CW listens under it)
        const double view = rowSpanKHz_ > 0.0 ? rowSpanKHz_ : 2.0 * half, under = getInt(kMode) == kCW ? kCwPitchKHz : 0.0;
        put(kScopePassLo, std::clamp(0.5 + (lo / 1000.0 - under) / view, 0.0, 1.0), 1e-5);
        put(kScopePassHi, std::clamp(0.5 + (hi / 1000.0 - under) / view, 0.0, 1.0), 1e-5);
        put(kPassLo, (lo + 6000) / 12000.0, 1e-5);                          // their text is the edge in Hz
        put(kPassHi, (hi + 6000) / 12000.0, 1e-5);
        const bool on = connected_ && haveRate_;
        if (chain_.nb.fired != firedSeen_) { firedSeen_ = chain_.nb.fired; lastBlank_ = now; }
        put(kBlanking, on && get(kBlanker) > 0.0f && now - lastBlank_ < milliseconds(250) ? 1.0 : 0.0, 0.5);
        put(kNotching, on && getInt(kNotch) == 1 && chain_.notchDb() >= 3.0f ? 1.0 : 0.0, 0.5);
        put(kRemoved, on ? std::clamp(chain_.removedDb() / 20.0, 0.0, 1.0) : 0.0, 0.01);
    }

    // ----- grab -----------------------------------------------------------------------------------
    std::string grabFolder()
    {
        // <drive>/VST2 Plugins/<plugin folder>  ->  <drive>/Skywave Grabs
        std::string f = mpc::pluginFolder();
        if (f.empty()) return "";
        for (int up = 0; up < 2; ++up) { size_t s = f.find_last_of('/'); if (s == std::string::npos) return ""; f = f.substr(0, s); }
        return f.empty() ? "" : f + "/Skywave Grabs";
    }

    // what it was, where, how, the bars and tempo of a bar grab, and when: "WWV 15 Kansas, USA am 4bar 92bpm 261115-1540"
    std::string grabName(const std::string& tag)
    {
        const double f = dialKHz(getInt(kMHz), get(kKHz), get(kFine));
        std::string what;
        const int s = getInt(kStation);
        if (s > 0 && s < kNumStations && std::fabs(kStations[s].kHz - f) < 0.05) what = kStations[s].name;
        else
        {
            std::lock_guard<std::mutex> g(labelsMu_);
            double best = 0.5;
            for (const Label& l : labels_) if (std::fabs(l.kHz - f) < best) { best = std::fabs(l.kHz - f); what = l.name; }
        }
        char b[48];
        if (what.empty()) { std::snprintf(b, sizeof(b), "%.1f kHz", f); what = b; }
        char when[32];
        std::time_t t = std::time(nullptr);
        std::strftime(when, sizeof(when), "%y%m%d-%H%M%S", std::localtime(&t));
        std::string name = what + " " + rx_.label + " " + kModeName[std::clamp(getInt(kMode), 0, 4)] + tag + " " + when, out;
        for (char c : name)   // the characters every file system takes
            if (std::isalnum((unsigned char)c) || std::strchr(" .,-_()+", c)) out += c;
        return out.substr(0, 120);
    }

    void grab()
    {
        const int len = std::clamp(getInt(kGrabLen), 0, 7);
        const int64_t end = grabEnd_.load(std::memory_order_relaxed);
        const double rate = grabSr_.load();
        // what the ring still holds before the tap, less 2 s for the head to move while we copy
        const int64_t held = kGrabRingLen - (grabW_.load(std::memory_order_acquire) - end) - (int64_t)(2.0 * rate);
        const int64_t most = std::max<int64_t>(0, std::min({end, (int64_t)(40.0 * rate), held}));
        int64_t n;
        std::string length, tag;
        if (len < 4)
        {
            n = std::min<int64_t>((int64_t)kGrabSecs[len] * (int64_t)std::llround(rate), most);
            length = std::to_string(n < (int64_t)kGrabSecs[len] * (int64_t)std::llround(rate) ? (int)(n / rate) : kGrabSecs[len]) + " s";
        }
        else   // bars at the tempo of the tap, sample-exact; too long for the ring: half as many
        {
            const double bpm = grabBpm_.load(std::memory_order_relaxed), q = grabQuarters_.load(std::memory_order_relaxed);
            const int asked = kGrabBars[len - 4];
            int bars = asked;
            auto samples = [&](int k) { return (int64_t)std::llround(k * q * 60.0 / bpm * rate); };
            while (bars > 1 && samples(bars) > most) bars /= 2;
            n = samples(bars);
            if (n > most) { setStatus("not enough heard yet"); return; }
            length = std::to_string(bars) + (bars == 1 ? " bar" : " bars") + (bars < asked ? " (" + std::to_string(asked) + " too long)" : "");
            char t[32];
            if (bpm == std::floor(bpm)) std::snprintf(t, sizeof(t), " %dbar %.0fbpm", bars, bpm);
            else std::snprintf(t, sizeof(t), " %dbar %.1fbpm", bars, bpm);
            tag = t;
        }
        int peak = 0;
        for (int64_t i = end - n; i < end; ++i) peak = std::max(peak, std::abs((int)grabRing_[(size_t)(i & (kGrabRingLen - 1))]));
        if (n <= 0 || peak < 33) { setStatus("nothing to grab: silence"); return; }   // -60 dBFS
        const std::string dir = grabFolder();
        if (dir.empty()) { setStatus("cannot find the drive"); return; }
        mkdirOne(dir.c_str());
        const std::string base = grabName(tag);
        std::string file = base + ".wav";
        for (int k = 2; k < 100; ++k)   // two taps in the same second keep both
        {
            std::FILE* f = std::fopen((dir + "/" + file).c_str(), "rb");
            if (!f) break;
            std::fclose(f);
            file = base + " " + std::to_string(k) + ".wav";
        }
        // peaks at -1 dBFS, as a sample should arrive; at most 24 dB up, so static stays static
        const double gain = std::min(16.0, 29204.0 / peak);
        std::vector<int16_t> data((size_t)n);
        for (int64_t i = 0; i < n; ++i)
            data[(size_t)i] = (int16_t)std::clamp(std::lround(grabRing_[(size_t)((end - n + i) & (kGrabRingLen - 1))] * gain), -32767L, 32767L);
        if (writer_.joinable()) writer_.join();
        grabFile_ = file;
        grabLength_ = length;
        setStatus("grabbing " + length);
        // a card can take a while over 2.6 MB; written aside, the network keeps draining
        writer_ = std::thread([this, path = dir + "/" + file, data = std::move(data), rate = (uint32_t)rate] {
            grabDone_.store(writeWav(path, data, rate) ? 1 : 2);
        });
    }

    static bool writeWav(const std::string& path, const std::vector<int16_t>& data, uint32_t rate)
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        const uint32_t bytes = (uint32_t)(data.size() * 2);
        std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
        std::fwrite("data", 1, 4, f); u32(bytes);
        const bool ok = std::fwrite(data.data(), 2, data.size(), f) == data.size();
        return std::fclose(f) == 0 && ok;
    }

    // ----- the loop -------------------------------------------------------------------------------
    // A project load restored Receiver as a place in today's list: find the saved receiver by its
    // address and point Receiver (and Region if need be) at it. Listen comes back off either way:
    // a project left open, a template or a boot without network must not call anyone.
    void relocate()
    {
        restored_ = true;
        std::string key;
        {
            std::lock_guard<std::mutex> g(textMu_);
            key = savedKey_;
        }
        const bool was = getBool(kListen);
        if (was) { set(kListen, 0.0f); notifyHostParameterChanged(kListen); }
        if (!key.empty() && !pointAt(key) && any()) { gone_ = was; setStatus("saved receiver gone from list"); }
        else if (was) setStatus("power is off: tap to call");
    }

    // A new instance: a free receiver of the region at random. Every copy starting on the list's
    // first entry would all call the same volunteer's radio.
    void spreadOut()
    {
        if (getBool(kListen) || getInt(kReceiver) != 0) return;
        std::vector<int> free;
        {
            std::lock_guard<std::mutex> g(listMu_);
            const std::vector<int>& idx = byRegion_[(size_t)getInt(kRegion)];
            for (int k = 0; k < (int)idx.size(); ++k)
            {
                const kiwi::Receiver& r = list_[(size_t)idx[(size_t)k]];
                if (r.usersMax <= 0 || r.users < r.usersMax) free.push_back(k);
            }
        }
        if (free.empty()) return;
        set(kReceiver, (float)free[std::uniform_int_distribution<size_t>(0, free.size() - 1)(rng_)]);
        notifyHostParameterChanged(kReceiver);
    }

    bool any()
    {
        std::lock_guard<std::mutex> g(listMu_);
        return !list_.empty();
    }

    // Receiver (and Region, if it is not in this one) to the receiver at host:port; false when the list has none
    bool pointAt(const std::string& key)
    {
        int region = getInt(kRegion), found = -1;
        {
            std::lock_guard<std::mutex> g(listMu_);
            auto look = [&](int reg) {
                const std::vector<int>& idx = byRegion_[(size_t)reg];
                for (int k = 0; k < (int)idx.size(); ++k)
                {
                    const kiwi::Receiver& r = list_[(size_t)idx[(size_t)k]];
                    if (r.host + ":" + std::to_string(r.port) == key) return k;
                }
                return -1;
            };
            found = look(region);
            if (found < 0 && (found = look(kiwi::kAll)) >= 0) region = kiwi::kAll;
        }
        if (found < 0) return false;
        if (region != getInt(kRegion)) { set(kRegion, (float)region); notifyHostParameterChanged(kRegion); }
        if (found != getInt(kReceiver)) { set(kReceiver, (float)found); notifyHostParameterChanged(kReceiver); }
        return true;
    }

    void touched(Clock::time_point now) { lastTouch_ = now; }

    // anything the user can turn, as it was last time round: a difference is a touch
    void watchTouches(Clock::time_point now)
    {
        static const int kWatch[] = {kLevel, kListen, kRegion, kReceiver, kMHz, kKHz, kFine, kMode, kGrabLen, kSeekBy, kSpan,
                                     kBandwidth, kShift, kAgc, kGain, kBlanker, kNotch, kDenoise, kList, kScroll, kStep, kScale};
        static_assert(sizeof(kWatch) / sizeof(kWatch[0]) == sizeof(watched_) / sizeof(watched_[0]), "one remembered value each");
        bool changed = false;
        for (size_t k = 0; k < sizeof(kWatch) / sizeof(kWatch[0]); ++k)
            if (get(kWatch[k]) != watched_[k]) { watched_[k] = get(kWatch[k]); changed = true; }
        if (changed) touched(now);
    }

    void run()
    {
        while (!prepared_.load() && !quit_.load()) std::this_thread::sleep_for(milliseconds(5));
        loadList();
        const auto fresh = Clock::now() + seconds(1);   // a project load comes within it
        bool freshDone = false;
        int seenGrab = evGrab_.load(), seenSt = evStation_.load(), seenCat = evCategory_.load(), seenLab = evLabel_.load();
        int seenUp = evSeekUp_.load(), seenDown = evSeekDown_.load(), seenSpan = evSpan_.load();
        int seenPgUp = evPageUp_.load(), seenPgDown = evPageDown_.load(), seenBack = evBack_.load(), seenDig = evDig_.load();
        int seenRow[kRows], seenReset = resetGen_.load(), seenTotal = 0;
        bool holding = false;
        Clock::time_point holdAt{};
        auto total = [&] {
            int t = evGrab_.load() + evStation_.load() + evCategory_.load() + evLabel_.load() + evSeekUp_.load() + evSeekDown_.load()
                  + evSpan_.load() + evPageUp_.load() + evPageDown_.load() + evBack_.load() + evDig_.load();
            for (int r = 0; r < kRows; ++r) t += evRow_[r].load();
            return t;
        };
        auto resync = [&] {
            holding = false;
            seenTotal = total();
            seenGrab = evGrab_.load(); seenSt = evStation_.load(); seenCat = evCategory_.load(); seenLab = evLabel_.load();
            seenUp = evSeekUp_.load(); seenDown = evSeekDown_.load(); seenSpan = evSpan_.load();
            seenPgUp = evPageUp_.load(); seenPgDown = evPageDown_.load(); seenBack = evBack_.load(); seenDig = evDig_.load();
            for (int r = 0; r < kRows; ++r) seenRow[r] = evRow_[r].load();
        };
        resync();
        rng_.seed((unsigned)std::time(nullptr));
        uint64_t lastBlocks = 0;
        auto lastProcess = Clock::now(), keySince = lastProcess;
        lastTouch_ = lastProcess;
        std::string rxKey, pendingKey;
        while (!quit_.load())
        {
            const auto now = Clock::now();
            const uint64_t b = blocks_.load(std::memory_order_relaxed);
            if (b != lastBlocks) { lastBlocks = b; lastProcess = now; }
            const bool alive = now - lastProcess < seconds(3);

            // What the user did since last time round. A load onto a running instance writes the
            // parameters while process() runs and only then calls reset(): a block in between takes
            // restored toggles for taps. So a tap waits 30 ms (two blocks at the most); a reset() in
            // that time means it was the load, and it is dropped.
            if (const int g = resetGen_.load(std::memory_order_acquire); g != seenReset)
            {
                seenReset = g;
                resync();
                pendingSeek_ = 0;
                pendingLabel_ = false;
                digTry_ = 0;
                digAfterSurvey_ = false;
                lastStation_ = getInt(kStation);
            }
            if (const int t = total(); t != seenTotal && !holding) { holding = true; holdAt = now; }
            const bool held = holding && now - holdAt < milliseconds(30);
            if (holding && !held) { holding = false; seenTotal = total(); }
            auto fired = [held](std::atomic<int>& ev, int& seen) {
                if (held) return false;
                const int e = ev.load(std::memory_order_acquire);
                if (e == seen) return false;
                seen = e;
                return true;
            };
            if (fired(evCategory_, seenCat)) { onCategory(); touched(now); }
            if (fired(evStation_, seenSt)) { onStation(); touched(now); }
            if (fired(evLabel_, seenLab)) { onLabel(now); touched(now); }
            if (fired(evSeekUp_, seenUp)) { onSeek(1, now); touched(now); }
            if (fired(evSeekDown_, seenDown)) { onSeek(-1, now); touched(now); }
            if (fired(evGrab_, seenGrab)) { grab(); touched(now); }
            if (fired(evSpan_, seenSpan)) wantScope(now);
            if (fired(evPageUp_, seenPgUp)) { onPage(-1, now); touched(now); }
            if (fired(evPageDown_, seenPgDown)) { onPage(1, now); touched(now); }
            if (const int w = getInt(kWheel); w != kWheelMid)   // a turn of the wheel: scroll by it, then back to the middle
            {
                set(kWheel, (float)kWheelMid);
                notifyHostParameterChanged(kWheel);
                if (!held) { scrollRows(w - kWheelMid); touched(now); }
            }
            if (const int t = getInt(kTune); t != kWheelMid)    // Tune turned: Step a step, then back to the middle
            {
                set(kTune, (float)kWheelMid);
                notifyHostParameterChanged(kTune);
                if (!held) { stepTune(t - kWheelMid); touched(now); }
            }
            for (int r = 0; r < kRows; ++r)
                if (fired(evRow_[r], seenRow[r])) { digTry_ = 0; digAfterSurvey_ = false; onRow(r); touched(now); }
            if (fired(evDig_, seenDig)) { onDig(now); touched(now); }
            if (const int d = grabDone_.exchange(0)) setStatus(d == 1 ? "grabbed " + grabLength_ + ": " + grabFile_ : "cannot write the grab");
            if (relocate_.exchange(false)) relocate();
            if (!freshDone && now >= fresh) { freshDone = true; if (!restored_) spreadOut(); }
            watchTouches(now);
            pending(now);

            kiwi::Receiver want;
            const bool have = selected(want);
            const std::string key = have ? want.host + ":" + std::to_string(want.port) : "";
            if (key != pendingKey) { pendingKey = key; keySince = now; }
            if (fired(evBack_, seenBack)) { digTry_ = 0; digAfterSurvey_ = false; onBack(key); touched(now); }
            if (surveyEnded_)                                   // a sweep ended: the dig that waited for it
            {
                surveyEnded_ = false;
                // not if the dial was moved or the line went while it swept
                if (digAfterSurvey_) { digAfterSurvey_ = false; if (onAir_ && same(here(this->rxKey()), digFrom_)) digNext(now); }
            }
            digListen(key, now);
            learnNoise(key, now);
            lamp(now);
            history(key, now);
            listView(now);
            const bool listenOn = getBool(kListen);
            if (listenOn && now - lastTouch_ > kIdle)        // nobody here: give the slot back
            {
                set(kListen, 0.0f);
                notifyHostParameterChanged(kListen);
                if (connected_) disconnect("30 min untouched: power off", kLinkOff);
                else setStatus("30 min untouched: power off");
                continue;
            }
            const bool listen = listenOn && alive && have;
            if (listenOn && !listFetched_) refreshList();

            if (connected_ && (!listen || key != rxKey))
                disconnect(listen ? "changing receiver" : listenOn ? "idle" : gone_ ? "saved receiver gone from list" : "power is off",
                           listen ? kLinkCalling : kLinkOff);
            gone_ = false;
            if (!listenOn && !connected_) link_ = kLinkOff;
            readouts(have ? &want : nullptr, now);
            if (!listenOn) { blockedHost_.clear(); fails_ = 0; }   // Listen off and on: everything may be tried again
            const bool settled = key == rxKey || now - keySince >= seconds(1);   // a knob passing through is not a call
            if (listen && !connected_ && key != blockedHost_ && settled && (now >= retryAt_ || key != rxKey))
            {
                if (key != rxKey) fails_ = 0;
                rx_ = want;
                rxKey = key;
                const int r = connect();
                if (r != 0) callFailed(r == -2 ? "no network/host" : r > 0 ? "refused (" + std::to_string(r) + ")" : "no answer", key, now);
            }

            if (connected_)
            {
                std::vector<uint8_t> m;
                for (int k = 0; k < 64 && connected_; ++k)   // drain, then look around again
                {
                    const int r = ws_.receive(m);
                    if (r < 0) { lost("dropped", key, now); break; }
                    if (r == 0) break;
                    onMessage(m);
                }
                if (connected_)
                {
                    const auto t = Clock::now();
                    if (onAir_ && fails_ > 0 && t - onAirSince_ >= kSessionEnded) fails_ = 0;   // a good session forgives
                    scope(t);
                    if (haveRate_) { tune(); agc(); }
                    if (t - lastKeepalive_ >= seconds(1)) { ws_.sendText("SET keepalive"); lastKeepalive_ = t; }
                    if (t - lastAudio_ > seconds(10)) lost("no audio", key, t);
                }
            }
            else std::this_thread::sleep_for(milliseconds(50));
        }
        if (connected_) ws_.close();
    }

    // audio side
    double sr_ = 44100.0;
    std::vector<float> srcRing_;
    std::vector<int16_t> grabRing_;
    std::atomic<int64_t> srcW_{0}, srcR_{0}, grabW_{0}, grabEnd_{0};
    std::atomic<double> srcRate_{0.0};
    std::atomic<int> streamGen_{0};
    std::atomic<int> evStation_{0}, evCategory_{0}, evLabel_{0}, evSeekUp_{0}, evSeekDown_{0}, evGrab_{0}, evSpan_{0};
    std::atomic<int> evPageUp_{0}, evPageDown_{0}, evBack_{0}, evDig_{0}, evRow_[kRows] = {};
    std::atomic<uint64_t> blocks_{0};
    std::atomic<int> resetGen_{0};
    std::atomic<bool> binsSkip_{false};
    std::atomic<bool> prepared_{false};
    std::atomic<int> ourLabel_{-1};                        // the Label the worker set itself: no turn of the user's
    bool barPending_ = false;                              // a bar seen moved, waiting out a load's reset()
    Clock::time_point barSince_{};
    int lastStation_ = 0;                                // the worker's: where the Station dial was
    int pStation_ = 0, pCategory_ = 0, pLabel_ = 0, pSeekUp_ = 0, pSeekDown_ = 0, pGrab_ = 0, pSpan_ = 0, pScale_ = 0;
    int pPageUp_ = 0, pPageDown_ = 0, pBack_ = 0, pDig_ = 0, pRow_[kRows] = {};
    int playGen_ = -1;
    bool primed_ = false;
    double readPos_ = 0.0, fill_ = 0.0;
    Smoother level_;
    Biquad lp_;
    DCBlocker dc_;

    // worker side
    net::WebSocket wf_;
    std::string ts_, labelsFor_, connHost_;
    int connPort_ = 0;
    bool wfOpen_ = false, wfReady_ = false, wfParked_ = false, labelsAsked_ = false, binsLive_ = false;
    double wfCenter_ = -1.0, rowStartKHz_ = 0.0, rowSpanKHz_ = 0.0;
    int wfZoom_ = -1, rowFloor_ = 0;
    std::vector<uint8_t> row_;
    float binWritten_[kBins] = {};
    int sliceDb_[kBins] = {};                              // this row's bars, dB over the floor
    double barDb_[kBins] = {};                             // as shown: up at once, down 4 dB a tenth
    double autoTop_ = 20.0;                                // Scale Auto's top, dB over the floor
    double tuneKnobKHz_ = -1.0, tuneTurns_ = 0.0;           // Tune's knob: where it last turned, its angle
    std::array<std::array<uint8_t, kWfCols>, kWfRows> wfHist_{};   // the waterfall's shades, the newest row first
    std::array<int, kWfCols> wfSlice_{};                   // the loudest of each column since the last row; -1 none
    double wfHistSpan_ = 0.0, wfHistStart_ = 0.0;          // where the history was drawn
    Clock::time_point lastWfKeepalive_{}, scopeUntil_{}, wfRetryAt_{}, wfRefusedAt_{}, lastBins_{}, lastWfPush_{}, lastRow_{};
    int pendingSeek_ = 0;
    bool pendingLabel_ = false;
    Clock::time_point pendingUntil_{};
    std::mutex labelsMu_;
    std::vector<Label> labels_;
    std::thread worker_, writer_, wfOpener_;
    std::atomic<bool> wfConnecting_{false}, wfAbort_{false}, relocate_{false}, noWf_{false};
    std::atomic<int> wfResult_{0};
    std::atomic<int> zoomCap_{14};                         // the receiver's closest zoom; formatValue reads it   // the waterfall call: 0 none, 1 open, 2 failed
    std::atomic<double> grabSr_{44100.0}, grabBpm_{120.0}, grabQuarters_{4.0};
    double bpm_ = 120.0, barQuarters_ = 4.0;   // the audio thread's, from the transport
    std::atomic<bool> quit_{false};
    std::atomic<int> grabDone_{0};
    std::string grabFile_, grabLength_;
    net::WebSocket ws_;

    kiwi::Receiver rx_;
    int link_ = kLinkOff;
    double smeterDbm_ = -127.0, signal_ = 0.0;
    Clock::time_point lastReadout_{}, lastTouch_{}, retryAt_{}, onAirSince_{}, lastBlank_{};
    float watched_[22] = {};
    Adpcm adpcm_;
    clean::Chain chain_;
    unsigned firedSeen_ = 0;
    std::atomic<bool> connected_{false};
    bool haveRate_ = false, listFetched_ = false, onAir_ = false, outside_ = false;
    bool gone_ = false, kicked_ = false, restored_ = false;   // restored_: a project load came   // Listen went off because the project's receiver is gone
    int idleLimit_ = 0, dayLimit_ = 0;   // the owner's: minutes untouched, minutes a day per listener
    int fails_ = 0;
    double tunedKHz_ = -1.0;
    int tunedMode_ = -1, tunedLo_ = -1, tunedHi_ = -1, agcSent_ = -1, gainSent_ = -1;
    std::vector<std::string> msgSeen_;
    // the list page, as last drawn
    int viewKind_ = -1, viewCat_ = -1, viewRegion_ = -1, viewN_ = -1, viewCur_ = -2;
    int64_t viewSig_ = -1;
    bool viewDirty_ = false;
    Clock::time_point lastView_{};
    // Back's places, Dig's dice
    std::vector<Spot> hist_;
    int histPos_ = -1;
    Spot seen_;
    Clock::time_point seenSince_{};
    std::mt19937 rng_;
    int digTry_ = 0, digN_ = 0;                  // the dig's place number, 0 when not digging
    double digSum_ = 0.0, digBestOver_ = 0.0;
    double floor_[kBands] = {1e9, 1e9, 1e9, 1e9, 1e9, 1e9};   // each band's noise, dBm per kHz; 1e9 not heard yet
    double floorMin_[kBands] = {1e9, 1e9, 1e9, 1e9, 1e9, 1e9};   // the lowest it was: creep stops 10 dB over
    Spot noiseAt_;
    Clock::time_point noiseSince_{}, floorCreep_{};
    Spot digSpot_, digBest_, digFrom_;           // digFrom_: where the dial was when a dig began with a sweep
    bool digFromLive_ = false;
    std::string digName_, digBestName_;
    Clock::time_point digJudge_{};
    std::vector<double> digTried_;               // a dig's places from Live, not picked twice
    // the SIGNAL lamp
    double lampDbm_ = -127.0;
    bool lampFresh_ = true;
    Clock::time_point lastLamp_{};
    // Live: the sweep (worker), and its rows (liveMu_)
    bool surveyOn_ = false, surveySent_ = false, surveyEnded_ = false, digAfterSurvey_ = false;
    int surveyAt_ = 0, surveyRows_ = 0;
    std::vector<int> surveyWins_;
    std::vector<uint8_t> surveyMax_;
    std::vector<LivePeak> surveyPeaks_;
    std::string surveyTriedFor_;
    Clock::time_point surveyTriedAt_{}, surveyWinAt_{}, surveyEnd_{}, surveyWas_{};
    std::mutex liveMu_;
    std::vector<LivePeak> livePeaks_;
    std::vector<Live> live_;                     // strongest first
    std::string liveFor_;
    Clock::time_point liveAt_{};
    std::atomic<int> liveState_{0}, liveSig_{0}, liveProgress_{0};   // progress: windows done * 16 + windows
    std::string blockedHost_, cachePath_, savedKey_;   // savedKey_ under textMu_
    Clock::time_point lastKeepalive_, lastAudio_;

    // shared text
    std::mutex listMu_, textMu_;
    std::vector<kiwi::Receiver> list_;
    std::vector<int> byRegion_[kiwi::kNumRegions];
    std::string peakText_ = kScopeHint, status_;
};

MPC_PLUGIN_ENTRY(Skywave)
