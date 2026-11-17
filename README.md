# Skywave

Shortwave radio for sampling, by databurn. An instrument for the Akai MPC that plays any public
KiwiSDR receiver, 0 to 30 MHz, about 700 of them around the world, and GRAB writes the last
seconds you heard as a WAV on the drive, ready for the sampler.

## Requirements and install

- An MPC running MPC OS 3.10.1 or later, connected to the internet.
- In Preferences > Enhancements (Beta), turn on Enable Enhancements and External Plugin Support.
- Copy the folder `databurn - VST - Skywave` into a folder named `VST2 Plugins` at the top of an
  MPC drive, then restart the MPC. Skywave is in the plugin browser under External Plugins.

Grabs go to `Skywave Grabs` at the top of the same drive.

The operator's manual, DB-WP-009, is `docs/skywave_manual_rally.pdf` (also in `engn`,
`rally-dark` and `engn-dark`): every page of the panel with its parts numbered, tuning, the
scope, finding something, GRAB, and every control.

## Finding something to sample

- **LIST (F4), Live** is what the receiver hears right now: one sweep of its whole band, a few
  seconds, then its strongest 200 signals, loudest first, those of the Category shown, each with how
  far it stands over the noise (`9650.0 AM +44 31m band`) and the name of the label or preset on it when there is one.
  Tap a row to go there. Page Up at the top of the list sweeps again.
- **DIG** goes somewhere in the Category at random (anywhere, with Category on Anything). On air, it sweeps the band first when there is no
  sweep of this receiver from the last ten minutes, then picks from what it found, the strong
  more often (a sweep under a minute old has just heard it, so it stays). Otherwise it listens to
  each place for a moment and digs again if it hears only noise, up to five places. Off air, or with no waterfall slot, or nothing of the Category in the sweep, it picks
  a label or a preset of the Category instead.
- **SEEK** on RADIO steps to the next signal up or down: through a fresh Live sweep without
  holding the waterfall, else through the scope's signals.
- **The SIGNAL lamp** lights when the dial stands 10 dB or more over that band's noise, as the
  radio has learned it while you listen: something is there.
- **BACK** returns to the last place you stayed three seconds or more, and again to the one
  before, like a browser.
- **SCOPE (F2)** shows the signals around the dial as bars over a waterfall; tap one and the radio
  tunes onto it.
- **Knobs**: a tap only picks a knob (its Q-Link bank, and the data wheel); the wheel, its Q-Link or
  a double-tap's editor turn it. A tap never moves it.
- GRAB, BACK and DIG are on every page.
- **HELP (F5)**, four pages: each page of the panel drawn with its parts numbered and named.

## RADIO (F1)

| control | what it does |
| --- | --- |
| Region, Receiver | browse the public list. Within a region the receivers run west to east, so turning Receiver travels across the map. Each shows how many listeners it had at the list's last update, `5/8`. |
| Listen (POWER) | the red power switch beside the lamps on RADIO (a smaller one on the other pages): on takes one listening slot on that receiver, its LED lit; off gives the slot back. |
| Category, Station | 140 presets in ten categories named for how they sound: Ticks & Time (WWV, CHU, RWM, BPM), Talk & Music (the metre bands, MW, WRMI, WWCR, Radio Havana), Robots & Pilots (Shannon, New York and RAF VOLMET, the North Atlantic families), Fax & Teletype (USCG weather, weather fax from Boston, Point Reyes, New Orleans, Hamburg and Tokyo, DWD RTTY), Buzzers & Odd (the Buzzer, the Pip, the Squeaky Wheel, HFGCS), Ham Chatter, Chirps & Data (FT8 on every band, FT4, WSPR, PSK31, SSTV, Morse), Morse Beacons (NCDXF), Drones (VLF) (the navy transmitters, Alpha, WWVB, MSF, JJY, DCF77, TDF), CB & Pirates, and Anything, which lets everything through. Category is the filter: Station steps through its stations only, round and round, the LIST rows show its stations and its part of the Live sweep, and DIG stays in it (its bands, and a couple of kHz round each of its presets). Turning Category browses: Station goes to `Dial` while the radio keeps playing. The status says what each station sounds like (`The Buzzer: buzz`). Broadcasters move with the seasons; the metre bands always hold someone. |
| Tune, Step | the tuning knob: tap it, then turn the data wheel (or its Q-Link), a Step a click. Step is 10, 50, 100 or 500 Hz, 1, 5, 9 or 10 kHz, 100 kHz or 1 MHz. Off its grid, the first click lands on it, as a radio's channel steps do: 5 kHz from 9652.3 goes to 9655, then 9660, so 5 kHz walks the shortwave broadcast channels, 9 kHz Europe's, Africa's and Asia's medium wave and 10 kHz the Americas'. The frequency reads to 10 Hz. A KiwiSDR covers 10 kHz to 30 MHz, so FM broadcast is out of reach. In CW the dial reads the signal's frequency and you hear it as a 500 Hz tone. (MHz, kHz and Fine, which hold the frequency, are still parameters, off the panel.) |
| Mode | AM, LSB, USB, CW, NBFM. |
| Grab, Grab Len | every tap saves what you just heard (before Level): the last 5, 10, 20 or 30 s, or the last 1, 2, 4 or 8 bars at the project's tempo, to the sample, so a bar grab loops on the grid (bars longer than 40 s are halved until they fit, and the status says so). Each grab is normalised to peak at -1 dBFS (at most 24 dB of gain) and named for the station or the receiver's label, the receiver, the mode, the bars and tempo of a bar grab, and the time: `WWV 15 Manhattan, KS, USA am 2bar 90bpm 261115-171636.wav`. Silence is not saved. |
| Level | output, -24 to +12 dB. |

The window shows the receiver on a world map, the frequency, a signal meter from the receiver's
own S-meter, four lamps (CALLING, ON AIR, FAULT, and SIGNAL: something over the noise here) and
what the radio is doing, with the owner's time limits when there are any. Beside the status:
SEEK down and up (through the Live sweep when there is a fresh one, else the scope's signals),
BACK and DIG.

To put the radio straight on a pad without the file: open the Sampler, set its input to
Resample, arm it and record while Skywave plays. GRAB is for the moment you already heard.

## LIST (F4)

| control | what it does |
| --- | --- |
| List | what the rows show: Stations (the presets of the current Category, with what each sounds like), Labels (the receiver's own station list; it loads once you are listening), Receivers (those of the current Region, with how many listeners each had at the list's last update; `8/8` is full) or Live (what this receiver hears now, loudest first; the first time you pick it on a receiver it sweeps the band). |
| Category, Region | which stations and which part of Live, which receivers. Category does not retune: a row tap does. Region moves the radio to that region's receiver (with POWER on it calls it a second later). |
| the header, Page Up, Page Down | tap the header, then turn the data wheel: a row a step. Page Up and Down move eight rows. The header says where you are: `Talk & Music  1-8 of 24`, `Live 3 min ago  1-8 of 120`, `Ham Chatter live  1-8 of 12`. Page Up at the top of Live sweeps again. |
| Back, Dig | as on RADIO. |
| rows | tap one to go there. `>` marks where the radio is; the rows follow it when it moves. |

Beside the rows: the frequency, the receiver, the status, the signal meter and the four lamps.

## SCOPE (F2)

64 bars show the dial's neighbourhood over Span (2 kHz to 938 kHz; a receiver that zooms only so
far shows its closest), from the receiver's own waterfall, and under them a waterfall of the last
seven seconds, a row every 0.6 s, newest on top; it slides with the dial and starts again with a
new Span. The amber line is the dial, the two white lines the passband, what you hear.

- Tap a bar and the radio tunes onto the strongest signal in it, to 10 Hz (in USB or LSB a
  kilohertz beside the voice, where its carrier is). Only noise in it: the bar's middle.
- PEAK names the strongest signal within an eighth of the span of the dial, how strong it is over
  the noise and how far off: `9650.00 kHz +44 dB (+0 Hz)`. Turn Tune until it reads +0 Hz. It is
  where the receiver hears it: a receiver without GPS runs a few parts per million off, so WWV can
  sit at 15000.17 kHz, and the audio agrees with the scope.
- Scale: Auto puts the strongest signal of the last moments at the top (bars and waterfall alike);
  20, 40 and 60 dB fix the range over the noise, to see how strong things really are. A bar's
  colour says how high it reaches: green, then amber, then near white.

- Seek Down and Seek Up, by Signals: the next peak 15 dB over the floor (in USB or LSB, the
  carrier beside the voice), or half a span on if there is none in view; by Labels: the next of
  the receiver's labels.
- Label: the receiver's own list of stations, with each one's name and mode; turning it tunes to
  each in turn.

The waterfall is opened when you turn Span, Scale, Seek or Label or tap a bar, for a Live sweep, and
to fetch the receiver's labels when LIST or DIG needs them; until then PEAK reads `turn SPAN to start
the scope`, and the first rows take a few seconds. If the receiver's waterfalls are all in use,
PEAK reads `no waterfall slot: try later`; it asks again after a minute, or after 15 s if you turn
Span. Seek just after a Live sweep steps through the sweep and leaves the waterfall closed. After five minutes without any of these
it stops sending (the Kiwi ends the audio too if its waterfall is closed, so it is parked, as a
browser's waterfall turned off), and it closes with the audio when Listen goes off or the receiver
changes.

## CLEAN (F3)

What the receiver sends, and what Skywave does to it after.

| control | what it does |
| --- | --- |
| Bandwidth | the receiver's passband for the mode: Narrow, Normal or Wide. The window draws it against the tuned frequency. |
| Shift | moves the passband up or down in pitch, up to 1 kHz, to step away from a neighbour. |
| AGC, Gain | the receiver's automatic gain: Slow, Medium or Fast, or Off, when Gain sets its gain. |
| Blanker | a noise blanker: clicks and crashes far above the average are gated out for 2 ms. The BLANK lamp lights while it works. |
| Notch | Auto finds steady whistles (heterodynes, open carriers) and takes them out. The NOTCH lamp lights while it is removing one. |
| Denoise | keeps what is predictable (voices, tones) and lets the hiss go, by the amount set. |

The meter shows how many dB the three take out together.

## Be kind to receivers

Every KiwiSDR is a volunteer's hardware with a few listening slots. Skywave tries to behave like
a considerate listener:

- It takes one slot and says what it is (`skywave-mpc`, with a link to databurn.org/mpc). It
  leaves out of the list the receivers whose owners allow only web browsers.
- A receiver that ends the session after 30 s or more on air, or that says a time limit was
  reached, is not called again until you switch Listen off and on. Busy, password-only and down
  receivers are treated the same.
- A call that fails waits 5, 10, 20, then 40 s before the next, and gives up after five.
- Turning Receiver does not call every receiver it passes: it waits for the knob to rest a second.
- A Live sweep is one waterfall for a few seconds, of the receiver you are on; one is made when
  you ask (Live, Dig, Page Up), at most once a minute; a receiver whose channel has no waterfall says so. Skywave never sweeps receivers you are not
  listening to.
- Listen switches itself off after 30 minutes in which nothing was touched, and a project always
  opens with Listen off: a slot is only taken by a hand.

Please do the same: switch Listen off when you are done, and do not leave a project playing a
receiver overnight.

What you sample off the air may be someone's broadcast; what you do with it is up to you.

## How it works

A worker thread owns the network: it reads the receiver list, opens the receiver's websocket,
answers its messages, cleans the audio (about 12 kHz mono) and pushes it into a ring. The audio
thread resamples it to the host rate from a 0.35 s jitter buffer whose speed trims itself to
the receiver's clock. The list comes from `rx.linkfanel.net/kiwisdr_com.js`, generated from
kiwisdr.com/public: the plugin ships a trimmed copy and, the first time Listen goes on (at most
once a day), fetches a fresh one and keeps it in its folder. It asks
`databurn.org/mpc/receivers.js` first, the same list trimmed every six hours by `fetch_list.py` (about
200 KB instead of 900, without the owners' contact details), and falls back to rx.linkfanel.net
when that is missing or three days old. databurn.org's server log sees that one request a day.

## Building

Skywave builds with the MPC Plugin Builder SDK
([github.com/inmusic-software/mpc-plugin-builder-sdk](https://github.com/inmusic-software/mpc-plugin-builder-sdk)):
put this folder in the SDK's `plugins/`, then

    python plugins/skywave/fetch_list.py      # writes Content/receivers.js, the list it ships
    python tools/build.py plugins/skywave
    python tools/package.py plugins/skywave   # the zip; add LICENSE to it

The panel in `skins/` is committed. `make_skin.py <ne_50m_land.geojson>` redraws it after a
build (it reads `build/params.json`), then build again to ship it; it needs Python with numpy and
Pillow, Natural Earth's 1:50m land, and `skinkit.py` and `preview.py` with the Barlow fonts from
the databurn MPC skin kit.

The manual is built from the plugin, after the build and `make_skin.py`:

    python plugins/skywave/docs/build_skywave_manual.py all

It reads `build/params.json`, the skin, `make_skin.py`'s HELP and `Content/receivers.js` (through
the plugin's own parser, `docs/receivers_json.cpp`, which it compiles with clang++), and asserts
every number it prints against the source before it draws a page. It needs reportlab and the
databurn docs kit (`plugins/docs_kit`, the house Manual profile). Check the result with
`plugins/docs_kit/check_manual.py`.

## Credits

- The KiwiSDR network and its owners, and the KiwiSDR project (kiwisdr.com).
- The receiver list from rx.linkfanel.net.
- Map: Natural Earth (public domain). Lettering: Barlow (SIL Open Font License).
- The MPC Plugin Builder SDK, MIT, Copyright (c) 2026 inMusic Brands, Inc.

## Licence

MIT. See `LICENSE`, which also carries the SDK's notice.
