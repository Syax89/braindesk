#!/usr/bin/env python3
"""brainplay test suite (runs inside the brainplay-dev container).

    python3 tests/run.py [--bin build/native/brainplay] [test ...]

Every test starts its own Xvfb (854x480, 16 bit) and drives the player the
way the Brain does: through the window (xdotool) and through the slave
protocol. Logs and screenshots end up in tests/out/.
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIPS = os.path.join(ROOT, "tests", "clips")
OUT = os.path.join(ROOT, "tests", "out")
BIN = os.path.join(ROOT, "build", "native", "brainplay")
PREFIX = []           # e.g. ["qemu-arm", "-cpu", "arm926"]
RESULTS = []          # (test, check, ok, detail)
LOCK = threading.Lock()
TLS = threading.local()


def check(ok, what, detail=""):
    name = getattr(TLS, "test", "?")
    with LOCK:
        RESULTS.append((name, what, bool(ok), detail))
        print("  %s %-22s %s %s" % ("ok  " if ok else "FAIL", name, what, detail), flush=True)
    return ok


class Xvfb:
    def __init__(self, num, size="854x480"):
        self.display = ":%d" % num
        self.proc = subprocess.Popen(
            ["Xvfb", self.display, "-screen", "0", size + "x16", "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists("/tmp/.X11-unix/X%d" % num):
                break
            time.sleep(0.05)
        time.sleep(0.1)
        self.env = dict(os.environ, DISPLAY=self.display)

    def shot(self, name):
        path = os.path.join(OUT, name + ".png")
        xwd = subprocess.run(["xwd", "-root", "-silent"], env=self.env, capture_output=True)
        subprocess.run(["convert", "xwd:-", path], input=xwd.stdout, check=False)
        return path

    def pixel(self, x, y):
        """(r, g, b) of one screen pixel, 0..255"""
        xwd = subprocess.run(["xwd", "-root", "-silent"], env=self.env, capture_output=True)
        out = subprocess.run(["convert", "xwd:-", "-crop", "1x1+%d+%d" % (x, y), "-depth", "8", "rgb:-"],
                             input=xwd.stdout, capture_output=True).stdout
        return tuple(out[:3]) if len(out) >= 3 else (0, 0, 0)

    def xdo(self, *args):
        subprocess.run(["xdotool"] + [str(a) for a in args], env=self.env, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def click(self, x, y):
        self.xdo("mousemove", x, y, "mousedown", 1, "sleep", "0.08", "mouseup", 1)

    def key(self, name):
        self.xdo("key", name)

    def close(self):
        self.proc.terminate()
        self.proc.wait()


class Player:
    """brainplay with pipes on stdin/stdout (slave protocol) and stderr in a log file"""

    def __init__(self, name, args, env=None, slave=True, binary=None):
        self.name = name
        self.log = os.path.join(OUT, name + ".log")
        self.lines = []          # (time, text) of stdout
        self.t0 = time.monotonic()
        cmd = PREFIX + [binary or BIN] + (["--slave"] if slave else []) + ["--stats", "-v"] + args
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=open(self.log, "w"), env=env, text=True, bufsize=1)
        self.cv = threading.Condition()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.proc.stdout:
            with self.cv:
                self.lines.append((time.monotonic() - self.t0, line.rstrip("\n")))
                self.cv.notify_all()
        with self.cv:
            self.lines.append((time.monotonic() - self.t0, None))
            self.cv.notify_all()

    def send(self, cmd):
        try:
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass

    def wait_for(self, regex, timeout=5.0, start=0):
        """first stdout line from index start that matches; (index, match) or (None, None)"""
        end = time.monotonic() + timeout
        rx = re.compile(regex)
        i = start
        with self.cv:
            while True:
                while i < len(self.lines):
                    text = self.lines[i][1]
                    if text is not None:
                        m = rx.search(text)
                        if m:
                            return i, m
                    i += 1
                left = end - time.monotonic()
                if left <= 0 or (self.lines and self.lines[-1][1] is None):
                    return None, None
                self.cv.wait(left)

    def mark(self):
        with self.cv:
            return len(self.lines)

    def last(self, key):
        """value of the last 'key value' line"""
        with self.cv:
            for _, text in reversed(self.lines):
                if text and text.startswith(key + " "):
                    return text[len(key) + 1:]
        return None

    def pos(self):
        """fresh position: ask for a status dump"""
        n = self.mark()
        self.send("status")
        _, m = self.wait_for(r"^pos ([0-9.]+)", 3, n)
        return float(m.group(1)) if m else None

    def wait_exit(self, timeout=30):
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return "timeout"

    def quit(self):
        self.send("quit")
        return self.wait_exit(5)

    def stats(self):
        return stats_of(self.log)

    def stderr(self):
        return open(self.log).read()


def stats_of(log):
    text = open(log).read()
    m = re.search(r"stats: video shown=(\d+) decoded=(\d+) late-dropped=(\d+) key-skips=(\d+) "
                  r"skipped-packets=(\d+) max-level=(\d+) lowres=(\d+) \| sync mean=([-0-9.]+) ms "
                  r"max=([0-9.]+) ms \| first-frame=(\d+) ms", text)
    if not m:
        return None
    k = ["shown", "decoded", "late", "keyskips", "skipped", "level", "lowres"]
    d = {k[i]: int(m.group(i + 1)) for i in range(7)}
    d["sync_mean"] = float(m.group(8))
    d["sync_max"] = float(m.group(9))
    d["first"] = int(m.group(10))
    m = re.search(r"(\d+) underruns", text)
    d["underruns"] = int(m.group(1)) if m else 0
    return d


def clip(name):
    return os.path.join(CLIPS, name)


def no_errors(log):
    bad = [l for l in open(log).read().splitlines()
           if re.search(r"error|cannot|failed|Invalid|corrupt|AddressSanitizer|runtime error|LeakSanitizer", l)]
    return check(not bad, "clean log", "; ".join(bad[:3]))


# --------------------------------------------------------------------- tests

VIDEO = [  # file, frames, extra args
    ("h264_aac_424x240_25.mp4", 300), ("h264_aac_640x360_30.mp4", 360),
    ("h264_aac_854x480_24.mp4", 288), ("h264_aac_1280x720_30.mp4", 360),
    ("h264base_aac_424x240_25.mp4", 500), ("mpeg4_mp3_640x360_25.avi", 300),
    ("msmpeg4v3_mp3_424x240_25.avi", 200), ("vp8_vorbis_640x360_30.webm", 360),
    ("h264_opus_640x360_25.mkv", 300), ("h264_ac3_640x360_25.mkv", 250),
    ("mpeg2_mp2_720x576_25.mpg", 300), ("h264_aac_640x360_25.ts", 300),
    ("mjpeg_pcm_424x240_15.avi", 120), ("flv1_mp3_424x240_25.flv", 200),
    ("h264_noaudio_640x360_25.mp4", 250), ("h264_odd_854x482_25.mp4", 150),
    ("mpeg1_mp2_352x288_25.mpg", 200), ("h263_352x288.3gp", 150),
]
AUDIO = [("tone.mp3", 20), ("tone.flac", 15), ("tone.ogg", 15), ("tone.opus", 15),
         ("tone.wav", 10), ("tone.m4a", 15), ("tone.aac", 10), ("tone_22k_mono.mp3", 10)]


def parallel(jobs, width=6):
    """jobs: list of (test name, fn(display number)); own Xvfb for each worker"""
    it = iter(list(enumerate(jobs)))
    lock = threading.Lock()

    def worker(w):
        x = Xvfb(110 + w)
        try:
            while True:
                with lock:
                    nxt = next(it, None)
                if nxt is None:
                    return
                TLS.test = nxt[1][0]
                nxt[1][1](x)
        finally:
            x.close()

    threads = [threading.Thread(target=worker, args=(w,)) for w in range(width)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def play_whole(x, vo_args, name, f, frames, tag):
    """plays a clip to the end; everything must be shown, in sync"""
    t0 = time.monotonic()
    p = Player("%s_%s" % (tag, name), vo_args + ["--ao", "null", clip(f)], env=x.env, slave=True)
    time.sleep(2.5)
    x.shot("%s_%s" % (tag, name))
    _, m = p.wait_for(r"^state ended", 60)
    rc = p.wait_exit(10)
    wall = time.monotonic() - t0
    s = p.stats()
    check(rc == 0 and m, "plays to the end", "rc=%s %.1fs" % (rc, wall))
    if s:
        check(s["shown"] >= frames * 0.97, "all frames shown", "%d/%d late=%d" % (s["shown"], frames, s["late"]))
        check(s["sync_max"] < 45, "in sync", "mean %.1f max %.1f ms" % (s["sync_mean"], s["sync_max"]))
    else:
        check(False, "stats line")
    no_errors(p.log)


def t_formats(_):
    """every container/codec: software output, to the end"""
    jobs = []
    for f, frames in VIDEO:
        n = os.path.splitext(f)[0]
        jobs.append(("fmt:" + n, lambda x, f=f, n=n, fr=frames: play_whole(x, ["--vo", "x11"], n, f, fr, "x11")))
    parallel(jobs)


def t_pxp(_):
    """PXP paths on the simulator: overlay and XShm fallback, all sizes"""
    jobs = []
    for f, frames in [v for v in VIDEO if v[0].startswith("h264_aac") or "odd" in v[0]
                      or "mjpeg" in v[0] or "mpeg2" in v[0]]:
        n = os.path.splitext(f)[0]
        jobs.append(("pxp:" + n, lambda x, f=f, n=n, fr=frames: play_whole(x, ["--pxp-sim"], n, f, fr, "pxp")))
        jobs.append(("pxpshm:" + n,
                     lambda x, f=f, n=n, fr=frames: play_whole(x, ["--pxp-sim=legacy"], n, f, fr, "pxpshm")))
    parallel(jobs)


def audio_one(x, f, dur):
    n = os.path.splitext(f)[0]
    t0 = time.monotonic()
    p = Player("audio_" + n, ["--no-window", "--ao", "null", clip(f)], env=dict(os.environ, DISPLAY=""))
    _, m = p.wait_for(r"^dur ([0-9.]+)", 5)
    d = float(m.group(1)) if m else -1
    check(abs(d - dur) < 0.6, "duration", "%.1f (expected %d)" % (d, dur))
    if "tone." in f and f.split(".")[1] in ("mp3", "flac", "ogg", "opus", "m4a"):
        check(p.last("title") == "Canzone di prova", "title tag", str(p.last("title")))
        check(p.last("artist") == "L’Orchestra Città", "artist tag", str(p.last("artist")))
    _, m = p.wait_for(r"^state ended", dur + 15)
    rc = p.wait_exit(10)
    wall = time.monotonic() - t0
    check(rc == 0 and m, "plays to the end", "rc=%s" % rc)
    check(abs(wall - dur) < 1.5, "in real time", "%.1f s for %d s" % (wall, dur))
    pos = [float(t.split()[1]) for _, t in p.lines if t and t.startswith("pos ")]
    check(len(pos) >= dur and pos == sorted(pos) and pos[-1] > dur - 1.2,
          "position advances", "%d reports, last %.1f" % (len(pos), pos[-1] if pos else -1))
    # periodic position lines (those right after a state change do not count)
    gaps, prev = [], None
    for t, text in p.lines:
        if text and text.startswith("pos "):
            if prev is not None:
                gaps.append(t - prev)
            prev = t
        elif text and text.startswith("state "):
            prev = None
    check(not gaps or min(gaps) > 0.4, "at most 2 pos/s", "min gap %.2f" % (min(gaps) if gaps else 0))
    no_errors(p.log)


def t_audio(_):
    """audio-only files, headless, slave protocol"""
    parallel([("audio:" + f, lambda x, f=f, d=d: audio_one(x, f, d)) for f, d in AUDIO], width=9)


def t_slave(_):
    """pause, seek, volume, quit through the slave protocol (video + PXP overlay simulator)"""
    TLS.test = "slave"
    x = Xvfb(101)
    p = Player("slave", ["--pxp-sim", "--ao", "null", clip("h264_aac_640x360_long.mp4")], env=x.env)
    _, m = p.wait_for(r"^dur ([0-9.]+)", 5)
    check(m and abs(float(m.group(1)) - 60) < 0.5, "dur", m.group(0) if m else "")
    check(p.last("title") == "h264_aac_640x360_long", "title from the file name", str(p.last("title")))
    check(p.wait_for(r"^state playing", 3)[1], "state playing")
    time.sleep(2.0)
    a = p.pos()
    check(a is not None and 1.5 < a < 2.8, "pos after 2 s", str(a))

    n = p.mark()
    p.send("pause")
    check(p.wait_for(r"^state paused", 2, n)[1], "pause")
    time.sleep(0.3)
    a = p.pos()
    time.sleep(1.2)
    b = p.pos()
    check(a == b, "position frozen while paused", "%s %s" % (a, b))
    x.shot("slave_paused")

    n = p.mark()
    p.send("seek 30")
    _, m = p.wait_for(r"^pos ([0-9.]+)", 2, n)
    check(m and abs(float(m.group(1)) - 30) < 0.01, "seek reports the target", m.group(0) if m else "")
    time.sleep(0.8)
    x.shot("slave_seek30_paused")       # the new picture must be there although paused
    b = p.pos()
    check(b is not None and 27.9 <= b <= 30.1, "seek while paused lands on a key frame", str(b))

    n = p.mark()
    p.send("play")
    check(p.wait_for(r"^state playing", 2, n)[1], "play")
    time.sleep(2.0)
    c = p.pos()
    check(c is not None and 1.5 < c - b < 2.6, "plays on after the seek", "%s -> %s" % (b, c))

    p.send("seek +10")
    time.sleep(1.5)
    d = p.pos()
    check(d is not None and c + 8 < d < c + 12.5, "seek +10", "%s -> %s" % (c, d))
    p.send("seek -20")
    time.sleep(1.5)
    e = p.pos()
    check(e is not None and d - 21.5 < e < d - 16, "seek -20", "%s -> %s" % (d, e))

    n = p.mark()
    p.send("volume 40")
    check(p.wait_for(r"^volume 40", 2, n)[1], "volume 40")
    p.send("volume +15")
    check(p.wait_for(r"^volume 55", 2, n)[1], "volume +15")
    p.send("volume 400")
    check(p.wait_for(r"^volume 100", 2, n)[1], "volume clamps")
    n = p.mark()
    p.send("bogus")
    check(p.wait_for(r"^error unknown command: bogus", 2, n)[1], "unknown command")

    # several seeks in a row, then to the end
    for t in (5, 50, 12, 44, 3):
        p.send("seek %d" % t)
        time.sleep(0.15)
    time.sleep(1.0)
    f = p.pos()
    check(f is not None and 1.5 <= f < 5.5, "burst of seeks ends on the last one", str(f))
    n = p.mark()
    p.send("seek 57")
    check(p.wait_for(r"^state ended", 8, n)[1], "state ended at the end")
    rc = p.wait_exit(5)
    check(rc == 0, "exit code 0", str(rc))
    s = p.stats()
    check(s and s["sync_max"] < 60, "in sync through the seeks", str(s and s["sync_max"]))
    no_errors(p.log)

    # quit command and closed stdin
    p = Player("slave_quit", ["--no-window", "--ao", "null", clip("long.mp3")])
    p.wait_for(r"^state playing", 3)
    time.sleep(0.5)
    t0 = time.monotonic()
    rc = p.quit()
    check(rc == 0 and time.monotonic() - t0 < 1.0, "quit", "rc=%s in %.2fs" % (rc, time.monotonic() - t0))
    p = Player("slave_eof", ["--no-window", "--ao", "null", clip("long.mp3")])
    p.wait_for(r"^state playing", 3)
    p.proc.stdin.close()
    rc = p.wait_exit(3)
    check(rc == 0, "closing the pipe quits the player", str(rc))
    p = Player("slave_term", ["--no-window", "--ao", "null", clip("long.mp3")])
    p.wait_for(r"^state playing", 3)
    p.proc.terminate()
    rc = p.wait_exit(3)
    check(rc == 0, "SIGTERM: clean exit", str(rc))
    x.close()


def t_keepopen(_):
    """--keep-open, --start, --paused, replay"""
    TLS.test = "keepopen"
    x = Xvfb(102)
    p = Player("keepopen", ["--pxp-sim", "--ao", "null", "--keep-open", "--start", "8", "--paused",
                            clip("h264_aac_424x240_25.mp4")], env=x.env)
    check(p.wait_for(r"^state paused", 3)[1], "starts paused")
    time.sleep(1.0)
    a = p.pos()
    check(a is not None and 5.9 <= a <= 8.05, "--start 8", str(a))
    x.shot("keepopen_start_paused")
    n = p.mark()
    p.send("play")
    check(p.wait_for(r"^state ended", 9, n)[1], "ends")
    time.sleep(0.5)
    check(p.proc.poll() is None, "stays open")
    b = p.pos()
    check(b is not None and abs(b - 12) < 0.2, "position = duration at the end", str(b))
    x.shot("keepopen_ended")
    n = p.mark()
    p.send("play")
    check(p.wait_for(r"^state playing", 3, n)[1], "play again")
    time.sleep(2)
    c = p.pos()
    check(c is not None and 1.3 < c < 2.8, "restarted from the beginning", str(c))
    check(p.quit() == 0, "quit")
    no_errors(p.log)
    x.close()


def t_ui(_):
    """touch and keys on the window, checked through the slave protocol"""
    TLS.test = "ui"
    x = Xvfb(103)
    p = Player("ui", ["--pxp-sim", "--ao", "null", "--bar-timeout", "3",
                      clip("h264_aac_640x360_long.mp4")], env=x.env)
    p.wait_for(r"^state playing", 3)
    time.sleep(1.5)
    x.shot("ui_playing_bar")
    r, g, b = x.pixel(4, 470)
    check(r < 40 and g < 40 and b < 40, "bar visible at the start", str((r, g, b)))

    n = p.mark()
    x.click(32, 452)                      # play/pause button
    check(p.wait_for(r"^state paused", 2, n)[1], "pause button")
    time.sleep(0.4)
    x.shot("ui_paused")
    n = p.mark()
    x.click(400, 200)                     # tap on the picture
    check(p.wait_for(r"^state playing", 2, n)[1], "tap on the picture resumes")

    n = p.mark()
    x.click(686, 452)                     # volume -
    check(p.wait_for(r"^volume 95", 2, n)[1], "volume - button")
    x.click(765, 452)                     # volume +
    check(p.wait_for(r"^volume 100", 2, n)[1], "volume + button")

    # hold volume - : repeats
    n = p.mark()
    x.xdo("mousemove", 686, 452, "mousedown", 1, "sleep", "1.2", "mouseup", 1)
    time.sleep(0.2)
    v = int(p.last("volume"))
    check(v <= 70, "holding volume - repeats", str(v))
    x.shot("ui_volume")

    # tap on the seek bar at about 50 %
    n = p.mark()
    x.click(351, 452)
    time.sleep(1.0)
    a = p.pos()
    check(a is not None and 26 < a < 33, "tap on the seek bar", str(a))
    # drag the knob to about 80 %
    x.xdo("mousemove", 351, 452, "mousedown", 1, "sleep", "0.1", "mousemove", 420, 455, "sleep", "0.1",
          "mousemove", 476, 450, "sleep", "0.15")
    x.shot("ui_dragging")
    x.xdo("mouseup", 1)
    time.sleep(1.0)
    a = p.pos()
    check(a is not None and 44 < a < 51, "drag on the seek bar", str(a))

    # keys
    n = p.mark()
    x.key("space")
    check(p.wait_for(r"^state paused", 2, n)[1], "space pauses")
    a = p.pos()
    x.key("Left")
    time.sleep(0.8)
    b = p.pos()
    check(b is not None and a - 12.5 < b < a - 7.5, "Left = -10 s", "%s -> %s" % (a, b))
    x.key("Right")
    x.key("Right")
    time.sleep(0.8)
    c = p.pos()
    check(c is not None and b + 16 < c < b + 22.5, "Right twice = +20 s", "%s -> %s" % (b, c))
    n = p.mark()
    x.key("Up")
    time.sleep(0.2)
    x.key("Down")
    x.key("Down")
    time.sleep(0.3)
    check(int(p.last("volume")) == v - 5, "Up / Down change the volume", "%s -> %s" % (v, p.last("volume")))
    p.send("seek 10")
    n = p.mark()
    x.key("space")
    check(p.wait_for(r"^state playing", 2, n)[1], "space resumes")

    # the bar hides after 3 s of playback, the picture takes its rows
    x.xdo("mousemove", 400, 200)
    time.sleep(3.8)
    x.shot("ui_bar_hidden")
    r, g, b = x.pixel(4, 470)
    check(r > 150 and g < 60 and b < 60, "bar hidden: picture in its rows", str((r, g, b)))
    n = p.mark()
    x.click(32, 452)                      # where the play button would be: only shows the bar... and toggles
    check(p.wait_for(r"^state paused", 2, n)[1], "tap with hidden bar = tap on the picture")
    time.sleep(0.3)
    r, g, b = x.pixel(4, 470)
    check(r < 40 and g < 40 and b < 40, "bar back when paused", str((r, g, b)))
    x.shot("ui_bar_back")

    x.click(823, 452)                     # close
    rc = p.wait_exit(3)
    check(rc == 0, "close button", str(rc))
    no_errors(p.log)

    p = Player("ui_q", ["--vo", "x11", "--ao", "null", clip("h264_aac_640x360_long.mp4")], env=x.env)
    p.wait_for(r"^state playing", 3)
    time.sleep(1)
    x.key("q")
    check(p.wait_exit(3) == 0, "q quits")
    x.close()


def t_audioview(_):
    """the window of files without pictures"""
    TLS.test = "audioview"
    x = Xvfb(104)
    p = Player("audioview", ["--ao", "null", clip("long.mp3")], env=x.env)
    p.wait_for(r"^state playing", 3)
    time.sleep(2.2)
    x.shot("audioview_playing")
    r, g, b = x.pixel(10, 10)
    check((r, g, b) != (0, 0, 0) and r < 30, "audio view painted", str((r, g, b)))
    n = p.mark()
    x.click(427, 351)                     # big progress bar, middle
    time.sleep(0.8)
    a = p.pos()
    check(a is not None and 110 < a < 130, "tap on the big progress bar", str(a))
    n = p.mark()
    x.click(427, 120)                     # background: play/pause
    check(p.wait_for(r"^state paused", 2, n)[1], "tap pauses")
    time.sleep(0.3)
    x.shot("audioview_paused")
    check(p.quit() == 0, "quit")
    no_errors(p.log)
    x.close()

    x = Xvfb(105, "480x854")              # rotated panel
    p = Player("portrait_audio", ["--ao", "null", clip("tone.flac")], env=x.env)
    p.wait_for(r"^state playing", 3)
    time.sleep(1.5)
    x.shot("portrait_audio")
    p.quit()
    p = Player("portrait_video", ["--pxp-sim", "--ao", "null", clip("h264_aac_640x360_30.mp4")], env=x.env)
    p.wait_for(r"^state playing", 3)
    time.sleep(1.5)
    x.shot("portrait_video")
    n = p.mark()
    x.click(48, 826)                      # play/pause in the second row
    check(p.wait_for(r"^state paused", 2, n)[1], "two-row bar: pause button")
    check(p.quit() == 0, "quit")
    s = p.stats()
    check(s and s["shown"] > 20, "portrait: frames shown", str(s and s["shown"]))
    no_errors(p.log)
    x.close()


def slow_one(x, name, f, delay, expect):
    p = Player("slow_" + name, ["--pxp-sim", "--ao", "null", "--debug-sync",
                                "--debug-decode-delay", str(delay), clip(f)], env=x.env)
    t0 = time.monotonic()
    _, d = p.wait_for(r"^dur ([0-9.]+)", 5)
    dur = float(d.group(1))
    _, m = p.wait_for(r"^state ended", dur + 20)
    rc = p.wait_exit(10)
    wall = time.monotonic() - t0
    s = p.stats()
    check(rc == 0 and m, "plays to the end", "rc=%s" % rc)
    check(abs(wall - dur) < 1.5, "sound keeps real time", "%.1f s for %.0f s" % (wall, dur))
    if not s:
        check(False, "stats")
        return
    late = [float(m.group(1)) for m in re.finditer(r"late=([-+0-9.]+)ms", p.stderr())]
    worst = max(late) if late else 0
    detail = "shown=%d decoded=%d late=%d keyskips=%d skipped=%d level=%d lowres=%d worst=%.0fms" % (
        s["shown"], s["decoded"], s["late"], s["keyskips"], s["skipped"], s["level"], s["lowres"], worst)
    # never more than the catch-up threshold (0.5 s) plus one slow frame behind
    check(worst < 500 + delay + 150, "picture stays with the sound", detail)
    check(expect(s), "adapts as expected", detail)
    # pts of the frames shown must be spread over the whole clip (no drift, no stall)
    pts = [float(m.group(1)) for m in re.finditer(r"sync pts=([0-9.]+)", p.stderr())]
    check(pts and pts[-1] > dur - 2.6, "pictures up to the end", "last pts %.2f of %.0f" % (pts[-1] if pts else -1, dur))


def t_slow(_):
    """a CPU that is too slow (sleep per decoded frame): frames are dropped, sync is kept"""
    jobs = [
        # 25 fps = 40 ms per frame
        ("slow:fits", lambda x: slow_one(x, "fits", "h264_aac_640x360_long.mp4", 25,
                                         lambda s: s["keyskips"] == 0 and s["shown"] > 1400)),
        ("slow:bframes", lambda x: slow_one(x, "bframes", "h264_longgop_640x360_25.mp4", 50,
                                            lambda s: s["level"] == 3 and s["shown"] > 150)),
        ("slow:baseline", lambda x: slow_one(x, "baseline", "h264base_aac_424x240_25.mp4", 60,
                                             lambda s: s["keyskips"] >= 3 and s["shown"] > 100)),
        ("slow:mpeg4", lambda x: slow_one(x, "mpeg4", "mpeg4_mp3_720x576_25.avi", 60,
                                          lambda s: s["level"] == 3 and s["shown"] > 100)),
        ("slow:mjpeg", lambda x: slow_one(x, "mjpeg", "mjpeg_pcm_424x240_15.avi", 100,
                                          lambda s: s["skipped"] > 20 and s["shown"] > 40)),
        ("slow:hopeless", lambda x: slow_one(x, "hopeless", "h264_aac_640x360_long.mp4", 150,
                                             lambda s: s["keyskips"] >= 5)),
        ("slow:noaudio", lambda x: slow_one(x, "noaudio", "h264_noaudio_640x360_25.mp4", 60,
                                            lambda s: s["keyskips"] >= 1 or s["level"] == 3)),
    ]
    parallel(jobs, width=7)


def t_alsa(_):
    """the ALSA output: a real-time PulseAudio null sink, and the non-pacing "null" PCM"""
    TLS.test = "alsa"
    subprocess.run(["pulseaudio", "--kill"], stderr=subprocess.DEVNULL)
    open("/etc/asound.conf", "w").write(
        "pcm.!default { type pulse }\nctl.!default { type pulse }\n")
    r = subprocess.run(["pulseaudio", "--start", "--exit-idle-time=-1", "--load=module-null-sink",
                        "--load=module-native-protocol-unix", "-n"], capture_output=True, text=True)
    time.sleep(1.0)
    ok = subprocess.run(["pactl", "info"], capture_output=True).returncode == 0
    check(ok, "PulseAudio null sink running", r.stderr.strip()[:80])
    x = Xvfb(106)
    if ok:
        t0 = time.monotonic()
        p = Player("alsa_pulse", ["--pxp-sim", "--ao", "alsa", clip("h264_aac_640x360_30.mp4")], env=x.env)
        time.sleep(3)
        n = p.mark()
        p.send("pause")
        p.wait_for(r"^state paused", 2, n)
        time.sleep(1.0)
        a = p.pos()
        p.send("play")
        time.sleep(3.0)
        b = p.pos()
        # (the latency PulseAudio reports for its null sink moves around by half a second)
        check(a is not None and b is not None and 1.0 < b - a < 3.8, "plays on after a pause",
              "%s -> %s" % (a, b))
        p.send("seek 8")
        _, m = p.wait_for(r"^state ended", 20)
        rc = p.wait_exit(10)
        s = p.stats()
        log = p.stderr()
        check(rc == 0 and m, "plays to the end", "rc=%s" % rc)
        check("ALSA \"default\"" in log, "went through ALSA", "")
        check(s and s["sync_max"] < 120, "in sync on ALSA", str(s and (s["sync_mean"], s["sync_max"])))
        check(s and s["underruns"] <= 1, "no underruns", str(s and s["underruns"]))
        p = Player("alsa_pulse_audio", ["--no-window", "--ao", "alsa", clip("tone.ogg")])
        t0 = time.monotonic()
        _, m = p.wait_for(r"^state ended", 30)
        rc = p.wait_exit(5)
        wall = time.monotonic() - t0
        check(rc == 0 and abs(wall - 15) < 3.5, "audio file on ALSA in real time", "%.1f s" % wall)
        subprocess.run(["pulseaudio", "--kill"], stderr=subprocess.DEVNULL)
    os.remove("/etc/asound.conf")

    # ALSA "null": accepts everything at once; the player must pace it itself
    t0 = time.monotonic()
    p = Player("alsa_null", ["--pxp-sim", "--ao", "alsa", "--audio-device", "null",
                             clip("h264_aac_424x240_25.mp4")], env=x.env)
    time.sleep(3)
    n = p.mark()
    p.send("pause")
    p.wait_for(r"^state paused", 2, n)
    a = p.pos()
    time.sleep(1.0)
    p.send("play")
    time.sleep(2.0)
    b = p.pos()
    check(a is not None and b is not None and 1.6 < b - a < 2.5, "ALSA null: pause/resume keeps the position",
          "%s -> %s" % (a, b))
    _, m = p.wait_for(r"^state ended", 30)
    rc = p.wait_exit(5)
    wall = time.monotonic() - t0
    s = p.stats()
    check(rc == 0 and abs(wall - 13) < 2.0, "ALSA null: real time anyway", "%.1f s" % wall)
    check(s and s["shown"] > 270, "ALSA null: frames shown", str(s and s["shown"]))
    # a device that cannot be opened: silent playback, not a failure
    p = Player("alsa_missing", ["--pxp-sim", "--ao", "alsa", "--audio-device", "hw:9,9",
                                clip("h264_aac_424x240_25.mp4")], env=x.env)
    _, m = p.wait_for(r"^state ended", 30)
    s = p.stats()
    check(p.wait_exit(5) == 0 and s and s["shown"] > 280, "no audio device: plays silently",
          str(s and s["shown"]))
    x.close()


def t_wm(_):
    """under JWM with trays: full screen above them; overlay off while something is on top"""
    TLS.test = "wm"
    x = Xvfb(107)
    rc = os.path.join(OUT, "jwmrc")
    open(rc, "w").write("""<?xml version="1.0"?>
<JWM>
  <Tray x="0" y="0" height="40" autohide="off" layer="above"><TrayButton label="  Terminale  ">exec:true</TrayButton></Tray>
  <Tray x="0" y="-1" height="30" autohide="off" layer="above"><TaskList maxwidth="200"/><Clock format="%H:%M"/></Tray>
  <Group><Option>maximized</Option></Group>
  <WindowStyle decorations="flat"><Height>28</Height><Width>3</Width></WindowStyle>
  <FocusModel>click</FocusModel>
</JWM>
""")
    jwm = subprocess.Popen(["jwm", "-f", rc], env=x.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    x.shot("wm_desktop")
    p = Player("wm", ["--pxp-sim", "--ao", "null", "--bar-timeout", "0", "-v",
                      clip("h264_aac_640x360_long.mp4")], env=x.env)
    p.wait_for(r"^state playing", 3)
    time.sleep(2.0)
    x.shot("wm_fullscreen")
    r, g, b = x.pixel(250, 5)
    check(g > 200 and r < 60, "picture covers the top tray", str((r, g, b)))
    r, g, b = x.pixel(4, 470)
    check(r < 40 and g < 40 and b < 40, "bar covers the bottom tray", str((r, g, b)))

    check(p.quit() == 0, "quit")
    s = p.stats()
    check(s and s["sync_max"] < 80, "in sync", str(s and s["sync_max"]))
    check("PXP: screen overlay" in p.stderr(), "overlay used under the window manager")
    no_errors(p.log)
    jwm.terminate()
    x.close()

    # Another window on top (no window manager: new windows are mapped above
    # ours). The overlay must leave it alone: the picture goes through X
    # until the window is gone.
    x = Xvfb(108)
    p = Player("covered", ["--pxp-sim", "--ao", "null", "--bar-timeout", "0", "-v",
                           clip("h264_aac_640x360_long.mp4")], env=x.env)
    p.wait_for(r"^state playing", 3)
    time.sleep(1.5)
    r, g, b = x.pixel(100, 200)
    check(r > 200 and g < 60 and b < 60, "picture (red bar) before", str((r, g, b)))
    clock = subprocess.Popen(["xclock", "-geometry", "100x150+60+100", "-digital", "-update", "1"],
                             env=x.env, stderr=subprocess.DEVNULL)
    time.sleep(2.5)
    x.shot("covered")
    r, g, b = x.pixel(100, 200)
    check(not (r > 200 and g < 60 and b < 60), "the other window stays on top of the picture", str((r, g, b)))
    r, g, b = x.pixel(300, 200)
    check(g > 200 and r < 60, "picture still shown around it", str((r, g, b)))
    check("through XShm" in p.stderr(), "overlay switched off while covered")
    clock.terminate()
    time.sleep(1.5)
    x.shot("uncovered")
    r, g, b = x.pixel(100, 200)
    check(r > 200 and g < 60 and b < 60, "picture back after the window is gone", str((r, g, b)))
    check(p.stderr().count("PXP: screen overlay") >= 2, "overlay back on")
    check(p.quit() == 0, "quit")
    s = p.stats()
    check(s and s["sync_max"] < 80, "in sync", str(s and s["sync_max"]))
    no_errors(p.log)
    x.close()


def seek_one(x, f, dur, has_video):
    args = (["--pxp-sim"] if has_video else ["--no-window"]) + ["--ao", "null", clip(f)]
    p = Player("seek_" + os.path.splitext(f)[0], args, env=x.env if has_video else dict(os.environ, DISPLAY=""))
    p.wait_for(r"^state playing", 5)
    time.sleep(0.7)
    mid = dur * 0.5
    p.send("seek %.1f" % mid)
    time.sleep(1.2)
    a = p.pos()
    # key frames every 2 s at most in the test clips; then 1.2 s of playback.
    # MPEG-TS has no index: playback resumes at the first key frame AFTER the target.
    ts = f.endswith(".ts")
    check(a is not None and mid - 2.6 < a < mid + (3.6 if ts else 2.0), "seek to the middle",
          "%.1f -> %s" % (mid, a))
    back = 5 if ts else 3
    p.send("seek -%d" % back)
    time.sleep(1.0)
    b = p.pos()
    check(b is not None and a is not None and a - back - 2.5 < b < a - 0.9, "seek -%d" % back,
          "%s -> %s" % (a, b))
    p.send("pause")
    p.send("seek %.1f" % (dur - 2.5))
    time.sleep(0.8)
    n = p.mark()
    p.send("play")
    _, m = p.wait_for(r"^state ended", 8, n)
    rc = p.wait_exit(5)
    check(m and rc == 0, "seek near the end, play to the end", "rc=%s" % rc)
    no_errors(p.log)


def t_seek(_):
    """seeking in every container"""
    clips = [("h264_aac_640x360_30.mp4", 12, 1), ("mpeg4_mp3_640x360_25.avi", 12, 1),
             ("vp8_vorbis_640x360_30.webm", 12, 1), ("h264_opus_640x360_25.mkv", 12, 1),
             ("mpeg2_mp2_720x576_25.mpg", 12, 1), ("h264_aac_640x360_25.ts", 12, 1),
             ("flv1_mp3_424x240_25.flv", 8, 1), ("mjpeg_pcm_424x240_15.avi", 8, 1),
             ("tone.mp3", 20, 0), ("tone.flac", 15, 0), ("tone.ogg", 15, 0), ("tone.opus", 15, 0),
             ("tone.wav", 10, 0), ("tone.m4a", 15, 0), ("tone.aac", 10, 0)]
    parallel([("seek:" + f, lambda x, f=f, d=d, v=v: seek_one(x, f, d, v)) for f, d, v in clips], width=8)


def cpu_ticks(pid):
    f = open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])


def t_idle(_):
    """no CPU is used while paused; little for music"""
    TLS.test = "idle"
    hz = os.sysconf("SC_CLK_TCK")
    x = Xvfb(109)
    p = Player("idle_paused", ["--pxp-sim", "--ao", "null", clip("h264_aac_640x360_long.mp4")], env=x.env)
    p.wait_for(r"^state playing", 3)
    time.sleep(1.5)
    p.send("pause")
    time.sleep(1.0)
    c0 = cpu_ticks(p.proc.pid)
    time.sleep(4.0)
    used = (cpu_ticks(p.proc.pid) - c0) / hz
    check(used <= 0.02, "paused video: idle", "%.3f s of CPU in 4 s" % used)
    threads = len(os.listdir("/proc/%d/task" % p.proc.pid))
    check(threads == 4, "threads: main, reader, video, audio", str(threads))
    p.quit()
    p = Player("idle_music", ["--ao", "null", clip("long.mp3")], env=x.env)
    p.wait_for(r"^state playing", 3)
    time.sleep(1.0)
    c0 = cpu_ticks(p.proc.pid)
    time.sleep(5.0)
    used = (cpu_ticks(p.proc.pid) - c0) / hz
    check(used <= 0.25, "music with the window: light", "%.3f s of CPU in 5 s" % used)
    p.send("pause")
    time.sleep(0.5)
    c0 = cpu_ticks(p.proc.pid)
    time.sleep(3.0)
    used = (cpu_ticks(p.proc.pid) - c0) / hz
    check(used <= 0.02, "paused music: idle", "%.3f s of CPU in 3 s" % used)
    p.quit()
    x.close()


def t_robust(_):
    """files that are missing, empty, truncated or damaged: an error or a clean end, never a crash"""
    TLS.test = "robust"
    x = Xvfb(100)
    tmp = os.path.join(OUT, "bad")
    os.makedirs(tmp, exist_ok=True)

    def make(name, data):
        path = os.path.join(tmp, name)
        open(path, "wb").write(data)
        return path

    def run(name, path, expect_rc, window=True, timeout=30):
        args = (["--pxp-sim"] if window else ["--no-window"]) + ["--ao", "null", path]
        p = Player("bad_" + name, args, env=x.env)
        rc = p.wait_exit(timeout)
        log = p.stderr()
        check(rc == expect_rc, name, "rc=%s" % rc)
        check(not re.search(r"Sanitizer|runtime error|Segmentation", log), name + ": no crash", "")
        return p

    run("missing", os.path.join(tmp, "nothing.mp4"), 1)
    run("empty", make("empty.mp4", b""), 1)
    run("text", make("text.mp3", b"this is not a media file\n" * 2000), 1)
    run("random", make("random.avi", os.urandom(300000)), 1)
    good = open(clip("h264_aac_640x360_30.mp4"), "rb").read()
    run("mp4_no_index", make("cut.mp4", good[:len(good) // 2]), 1)      # moov atom is at the end
    for f in ("h264_opus_640x360_25.mkv", "h264_aac_640x360_25.ts", "mpeg4_mp3_640x360_25.avi",
              "mpeg2_mp2_720x576_25.mpg", "vp8_vorbis_640x360_30.webm", "flv1_mp3_424x240_25.flv"):
        data = open(clip(f), "rb").read()
        ext = os.path.splitext(f)[1]
        p = run("truncated" + ext, make("cut" + ext, data[:len(data) * 2 // 5]), 0)
        s = p.stats()
        check(s and s["shown"] > 50, "truncated%s: shows what is there" % ext, str(s and s["shown"]))
        # garbage in the middle of the file
        bad = bytearray(data)
        for off in range(len(bad) // 3, len(bad) // 3 + 60000, 7):
            bad[off] ^= 0x5a
        p = run("damaged" + ext, make("bad" + ext, bytes(bad)), 0)
        s = p.stats()
        # (FLV cannot resynchronise after a broken tag size: neither can stock ffmpeg)
        check(s and s["shown"] > (40 if ext == ".flv" else 100), "damaged%s: plays on after the damage" % ext,
              str(s and s["shown"]))
    for f in ("tone.mp3", "tone.ogg", "tone.flac", "tone.m4a", "tone.opus"):
        data = bytearray(open(clip(f), "rb").read())
        ext = os.path.splitext(f)[1]
        for off in range(len(data) // 2, min(len(data) // 2 + 20000, len(data)), 5):
            data[off] ^= 0xa5
        run("damaged" + ext, make("bad" + ext, bytes(data)), 0, window=False)
    x.close()


def t_bench(_):
    """--benchmark on every clip (under qemu this runs every decoder on the emulated ARM926)"""
    TLS.test = "bench"
    for f, _n in VIDEO + AUDIO:
        r = subprocess.run(PREFIX + [BIN, "--benchmark", clip(f)], capture_output=True, text=True, timeout=600)
        line = " | ".join(l.split(":")[0] + ":" + l.split("CPU:")[-1] for l in r.stdout.splitlines()[:2])
        check(r.returncode == 0 and ("video:" in r.stdout or "audio:" in r.stdout) and not r.stderr.strip(),
              f, line + r.stderr.strip()[:100])


TESTS = {"formats": t_formats, "pxp": t_pxp, "audio": t_audio, "slave": t_slave, "keepopen": t_keepopen,
         "ui": t_ui, "audioview": t_audioview, "slow": t_slow, "alsa": t_alsa, "wm": t_wm,
         "seek": t_seek, "idle": t_idle, "robust": t_robust, "bench": t_bench}


def main():
    global BIN, PREFIX
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=BIN)
    ap.add_argument("--qemu", action="store_true", help="run the binary with qemu-arm -cpu arm926")
    ap.add_argument("tests", nargs="*")
    a = ap.parse_args()
    BIN = os.path.abspath(a.bin)
    if a.qemu:
        PREFIX = ["qemu-arm", "-cpu", "arm926"]
    os.makedirs(OUT, exist_ok=True)
    os.chdir(ROOT)
    names = a.tests or [t for t in TESTS if t != "bench"]
    t0 = time.monotonic()
    for n in names:
        print("== %s: %s" % (n, TESTS[n].__doc__.strip().splitlines()[0]), flush=True)
        TESTS[n](None)
    failed = [r for r in RESULTS if not r[2]]
    print("\n%d checks, %d failed, %.0f s" % (len(RESULTS), len(failed), time.monotonic() - t0))
    for name, what, _, detail in failed:
        print("  FAILED %s: %s %s" % (name, what, detail))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
