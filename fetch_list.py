"""Write Content/receivers.js, the receiver list skywave ships, from the public KiwiSDR list.

    python3 fetch_list.py                 # download it from rx.linkfanel.net
    python3 fetch_list.py FILE            # or trim a copy already downloaded
    python3 fetch_list.py --out PATH      # write PATH instead (the copy databurn.org serves)

The list (rx.linkfanel.net/kiwisdr_com.js, generated from kiwisdr.com/public) carries everything
each owner publishes: names, places, antennas, contact details. skywave reads eleven fields, so
the shipped copy keeps those and nothing else, and leaves out receivers it would never call
(inactive, offline, browsers only, or listening above 30 MHz through a downconverter). The
plugin's own refresh on the MPC fetches databurn.org/mpc/receivers.js, written by this script
from cron, and falls back to the full list when that is missing or its epoch line is three days
old. The file is replaced in one rename, so a reader never gets half of it.
"""
import json
import os
import re
import sys
import tempfile
import time
import urllib.request

URL = "http://rx.linkfanel.net/kiwisdr_com.js"
KEEP = ("name", "loc", "status", "offline", "users", "users_max", "ext_api", "freq_offset", "bands", "gps", "url")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Content", "receivers.js")


def load(text):
    body = text[text.index("["): text.rindex("]") + 1]
    body = re.sub(r",\s*([}\]])", r"\1", body)            # JS allows trailing commas, JSON does not
    return json.loads(body)


def wanted(r):
    """A receiver skywave would call; a row that is not what the list promises is left out."""
    if not isinstance(r, dict):
        return False
    try:
        offset = float(r.get("freq_offset", "0") or 0)
    except (TypeError, ValueError):
        return False
    return (r.get("status") == "active" and r.get("offline") == "no" and r.get("ext_api", "1") != "0"
            and offset == 0.0 and str(r.get("url", "")).startswith("http://"))


def main():
    args = sys.argv[1:]
    out = OUT
    if "--out" in args:
        i = args.index("--out")
        out = args[i + 1]
        del args[i:i + 2]
    if args:
        text = open(args[0], encoding="utf-8", errors="replace").read()
    else:
        req = urllib.request.Request(URL, headers={"User-Agent": "skywave-mpc build (fetch_list.py)"})
        text = urllib.request.urlopen(req, timeout=30).read(32 << 20).decode("utf-8", errors="replace")
    stamp = re.search(r"KiwiSDR\.com data timestamp: ([^\n]*)", text)
    stamp = re.sub(r"[^\w :,.+-]", "", stamp.group(1))[:80] if stamp else "unknown"   # it goes in a comment line
    rows = [{k: r[k] for k in KEEP if isinstance(r.get(k), str)} for r in load(text) if wanted(r)]   # strings only
    if len(rows) < 50:
        sys.exit("only %d receivers: not written" % len(rows))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".receivers.", dir=os.path.dirname(os.path.abspath(out)))
    os.chmod(tmp, 0o644)                                   # the web server reads it
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
        f.write("// public KiwiSDR receivers, from %s (generated from kiwisdr.com/public)\n" % URL)
        f.write("// list timestamp: %s; trimmed to the fields skywave reads by fetch_list.py\n" % stamp)
        f.write("// list epoch: %d\n\n" % int(time.time()))
        f.write("var kiwisdr_com =\n[\n")
        f.write(",\n".join("\t" + json.dumps(r, ensure_ascii=False) for r in rows))
        f.write("\n];\n")
    os.replace(tmp, out)
    print("%d receivers, %d KB -> %s" % (len(rows), os.path.getsize(out) // 1024, out))


if __name__ == "__main__":
    main()
