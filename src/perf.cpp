// TemucraftCrash performance monitor (no Windows): see perf.h.
#include "perf.h"

#include <string.h>

namespace tc {
namespace perf {

static const char* const kCatNames[CAT_COUNT] = {"lua", "files", "textures", "models", "character", "calls", "present"};

const char* CatName(int cat) { return cat >= 0 && cat < CAT_COUNT ? kCatNames[cat] : "?"; }

void DefaultSettings(Settings* s) {
    s->hitchMs = 50;
    s->settleMs = 33;
    s->windowSec = 10;
}

static bool ParseNumber(const char* p, const char* end, double* out) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    double v = 0;
    bool any = false;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        any = true;
    }
    if (p < end && *p == '.') {
        p++;
        double scale = 0.1;
        while (p < end && *p >= '0' && *p <= '9') {
            v += (*p++ - '0') * scale;
            scale /= 10;
            any = true;
        }
    }
    if (any) *out = v;
    return any;
}

void ParseSettings(const char* text, size_t len, Settings* s) {
    const char* p = text;
    const char* end = text + len;
    while (p < end) {
        const char* eol = p;
        while (eol < end && *eol != '\n' && *eol != '\r') eol++;
        const char* eq = p;
        while (eq < eol && *eq != '=') eq++;
        if (eq < eol) {
            const char* k = p;
            while (k < eq && (*k == ' ' || *k == '\t')) k++;
            size_t klen = static_cast<size_t>(eq - k);
            while (klen && (k[klen - 1] == ' ' || k[klen - 1] == '\t')) klen--;
            double v;
            if (ParseNumber(eq + 1, eol, &v)) {
                if (klen == 5 && memcmp(k, "hitch", 5) == 0 && v >= 5 && v <= 10000) s->hitchMs = v;
                if (klen == 6 && memcmp(k, "settle", 6) == 0 && v >= 1 && v <= 10000) s->settleMs = v;
                if (klen == 6 && memcmp(k, "window", 6) == 0 && v >= 1 && v <= 3600) s->windowSec = static_cast<unsigned>(v);
            }
        }
        p = eol;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }
}

uint64_t HashName(const char* s, size_t max) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; s && i < max && s[i]; i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
        if (c == '/') c = '\\';
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t MixKey(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return k;
}

void PutMs(Buf& b, double ms) {
    if (ms < 0) ms = 0;
    uint64_t tenths = static_cast<uint64_t>(ms * 10 + 0.5);
    b.dec(tenths / 10);
    b.ch('.');
    b.ch(static_cast<char>('0' + tenths % 10));
}

static void CopyLabel(char* dst, size_t cap, const char* src, size_t len) {
    size_t n = 0;
    for (; src && n < len && n + 1 < cap && src[n]; n++) {
        char c = src[n];
        dst[n] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
    dst[n] = 0;
}

void Monitor::Init(int64_t frequency, const Settings& s, EmitFn fn, void* ctx) {
    memset(this, 0, sizeof(*this));
    set = s;
    freq = frequency > 0 ? frequency : 1;
    emit = fn;
    emitCtx = ctx;
    lua.Init();
    files.Init();
    assets.Init();
    winMinFps = 1e9;
    CopyLabel(screen, sizeof(screen), "login", 5);
}

void Monitor::Out(const Buf& b) {
    if (emit && b.len) emit(emitCtx, b.p, b.len);
}

int Monitor::Enter(int cat, int64_t now) {
    if (sp >= MAX_DEPTH || cat < 0 || cat >= CAT_COUNT) return -1;
    Scope& s = st[sp];
    s.cat = cat;
    s.start = now;
    s.child = 0;
    catDepth[cat]++;
    return sp++;
}

int64_t Monitor::Leave(int token, int64_t now, bool* outermost) {
    if (outermost) *outermost = false;
    if (token < 0 || token >= sp) return 0;      // dropped already (Truncate)
    if (token < sp - 1) Truncate(token + 1);    // scopes above it never left (a Lua error jumped over them)
    Scope& s = st[token];
    int64_t dur = now - s.start;
    if (dur < 0) dur = 0;
    int64_t self = dur - s.child;
    if (self < 0) self = 0;
    selfFrame[s.cat] += self;
    selfAll[s.cat] += self;
    selfWin[s.cat] += self;
    bool outer = --catDepth[s.cat] == 0;
    if (outer) {
        inclFrame[s.cat] += dur;
        countFrame[s.cat]++;
    }
    if (token > 0) st[token - 1].child += dur;
    sp = token;
    if (outermost) *outermost = outer;
    return dur;
}

void Monitor::Truncate(int depth) {
    if (depth < 0) depth = 0;
    while (sp > depth) {
        sp--;
        catDepth[st[sp].cat]--;
    }
}

void Monitor::StartWatch(const char* label, int64_t now) {
    // now: when the interaction started
    if (watch.on && !watch.sawFrame) {
        // more in the same frame (a click that calls several timed functions): one watch for all of them
        size_t used = strlen(watch.label);
        if (used + 4 < sizeof(watch.label)) {
            memcpy(watch.label + used, " + ", 3);
            CopyLabel(watch.label + used + 3, sizeof(watch.label) - used - 3, label, 200);
        }
        return;
    }
    if (watch.on) EndWatch(now, "cut short by the next one");
    watch.on = true;
    watch.sawFrame = false;
    CopyLabel(watch.label, sizeof(watch.label), label, 200);
    watch.start = now;
    watch.lastBad = 0;
    watch.n = 0;
    watch.calm = 0;
}

void Monitor::EndWatch(int64_t now, const char* why) {
    if (!watch.on) return;
    watch.on = false;
    char line[700];
    Buf b(line, sizeof(line));
    b.str("  after ");
    b.str(watch.label);
    b.str(": ");
    if (!watch.n) {
        b.str("no frame yet");
    } else {
        b.str("next frame +");
        PutMs(b, watch.ms[0]);
        b.str(" ms, ");
        if (why) {
            b.str(why);
            b.str(" after ");
            PutMs(b, Ms(now - watch.start));
            b.str(" ms");
        } else {
            b.str("smooth again +");
            PutMs(b, Ms(watch.lastBad - watch.start));
            b.str(" ms");
        }
        b.str(" (");
        b.dec(watch.n);
        b.str(watch.n == 1 ? " frame): " : " frames): ");
        for (unsigned i = 0; i < watch.n; i++) {
            if (i) b.ch(' ');
            PutMs(b, watch.ms[i]);
            if (watch.slow[i]) b.ch('*');
        }
    }
    b.nl();
    Out(b);
}

void Monitor::Mark(const char* label, size_t len, int64_t now) {
    char name[64];
    CopyLabel(name, sizeof(name), label, len);
    char line[160];
    Buf b(line, sizeof(line));
    b.str("mark ");
    b.str(name);
    b.str(" (frame ");
    b.dec(frameNo);
    b.str(")");
    b.nl();
    Out(b);
    char w[80];
    Buf wb(w, sizeof(w));
    wb.str("mark ");
    wb.str(name);
    StartWatch(w, now);
}

void Monitor::SpanToggle(const char* label, size_t len, int64_t now) {
    char name[48];
    CopyLabel(name, sizeof(name), label, len);
    for (Span& s : spans) {
        if (s.on && strcmp(s.label, name) == 0) {
            s.on = false;
            char line[200];
            Buf b(line, sizeof(line));
            b.str("span ");
            b.str(name);
            b.str(": ");
            PutMs(b, Ms(now - s.start));
            b.str(" ms, ");
            b.dec(frameNo - s.frame0);
            b.str(" frames ended, ");
            b.dec(hitchesAll - s.hitches0);
            b.str(" hitches");
            b.nl();
            Out(b);
            char w[80];
            Buf wb(w, sizeof(w));
            wb.str("span ");
            wb.str(name);
            StartWatch(w, now);
            return;
        }
    }
    Span* free = nullptr;
    for (Span& s : spans)
        if (!s.on) {
            free = &s;
            break;
        }
    if (!free) {
        // all in use: the oldest goes
        free = &spans[0];
        for (Span& s : spans)
            if (s.start < free->start) free = &s;
    }
    free->on = true;
    memcpy(free->label, name, sizeof(name));
    free->start = now;
    free->frame0 = frameNo;
    free->hitches0 = hitchesAll;
}

void Monitor::CallBegin(CallMark* m, int64_t now) {
    m->start = now;
    memcpy(m->selfAll, selfAll, sizeof(selfAll));
    m->token = Enter(CAT_CALL, now);
}

void Monitor::CallEnd(const CallMark& m, const char* label, int64_t now) {
    Leave(m.token, now, nullptr);
    double total = Ms(now - m.start);
    char line[400];
    Buf b(line, sizeof(line));
    b.str("call ");
    b.str(label);
    b.ch(' ');
    PutMs(b, total);
    b.str(" ms");
    bool first = true;
    for (int c = 0; c < CAT_COUNT; c++) {
        double v = Ms(selfAll[c] - m.selfAll[c]);
        if (v < 0.05) continue;
        b.str(first ? ": " : ", ");
        first = false;
        b.str(c == CAT_CALL ? "own code" : CatName(c));
        b.ch(' ');
        PutMs(b, v);
    }
    b.nl();
    Out(b);
    if (frameCallCount < FRAME_CALLS) {
        Buf f(frameCalls[frameCallCount++], 64);
        f.str(label);
        f.ch(' ');
        PutMs(f, total);
        f.str(" ms");
    }
    StartWatch(label, m.start);
}

void Monitor::Screen(const char* name, size_t len, int64_t now) {
    CopyLabel(screen, sizeof(screen), name, len);
    char line[100];
    Buf b(line, sizeof(line));
    b.str("glue screen ");
    b.str(screen);
    b.nl();
    Out(b);
    char w[80];
    Buf wb(w, sizeof(w));
    wb.str("glue screen ");
    wb.str(screen);
    StartWatch(w, now);
}

template <unsigned S, unsigned M>
static void PutTop(Buf& b, const NameTable<S, M>& t, unsigned n, double (*ms)(const void*, int64_t), const void* self) {
    unsigned top[8];
    if (n > 8) n = 8;
    unsigned got = t.Top(n, top);
    for (unsigned i = 0; i < got; i++) {
        const NameEntry& e = t.e[top[i]];
        b.str(i ? ", " : ": ");
        b.str(e.label[0] ? e.label : "?");
        b.ch(' ');
        PutMs(b, ms(self, e.ticks));
        if (e.calls > 1) {
            b.str(" (x");
            b.dec(e.calls);
            b.ch(')');
        }
    }
    if (t.lostCalls) {
        b.str(", and ");
        b.dec(t.lostCalls);
        b.str(" more ");
        PutMs(b, ms(self, t.lostTicks));
    }
}

static double MsOf(const void* self, int64_t ticks) { return static_cast<const Monitor*>(self)->Ms(ticks); }

void Monitor::EmitHitch(double ms, int64_t ticks) {
    char text[3000];
    Buf b(text, sizeof(text));
    b.str("HITCH ");
    PutMs(b, ms);
    b.str(" ms (frame ");
    b.dec(frameNo);
    b.str(", ");
    b.str(glueThisWindow ? screen : "world");
    b.ch(')');
    b.nl();

    b.str("  self ms:");
    int64_t sum = 0;
    bool any = false;
    for (int c = 0; c < CAT_COUNT; c++) {
        sum += selfFrame[c];
        double v = Ms(selfFrame[c]);
        if (v < 0.05) continue;
        b.str(any ? ", " : " ");
        any = true;
        b.str(c == CAT_CALL ? "calls" : CatName(c));
        b.ch(' ');
        PutMs(b, v);
    }
    b.str(any ? ", " : " ");
    b.str("other ");
    PutMs(b, Ms(ticks - sum));
    b.nl();

    if (countFrame[CAT_LUA]) {
        b.str("  lua ");
        PutMs(b, Ms(inclFrame[CAT_LUA]));
        b.str(" ms, ");
        b.dec(countFrame[CAT_LUA]);
        b.str(" scripts");
        PutTop(b, lua, 5, MsOf, this);
        b.nl();
    }
    if (countFrame[CAT_FILES]) {
        b.str("  files ");
        PutMs(b, Ms(inclFrame[CAT_FILES]));
        b.str(" ms, ");
        b.dec(countFrame[CAT_FILES]);
        b.str(" opens/reads");
        PutTop(b, files, 5, MsOf, this);
        b.nl();
    }
    if (countFrame[CAT_TEXTURES] || countFrame[CAT_MODELS]) {
        b.str("  textures ");
        PutMs(b, Ms(inclFrame[CAT_TEXTURES]));
        b.str(" ms (");
        b.dec(countFrame[CAT_TEXTURES]);
        b.str("), models ");
        PutMs(b, Ms(inclFrame[CAT_MODELS]));
        b.str(" ms (");
        b.dec(countFrame[CAT_MODELS]);
        b.ch(')');
        PutTop(b, assets, 5, MsOf, this);
        b.nl();
    }
    if (countFrame[CAT_CHARACTER]) {
        b.str("  character build ");
        PutMs(b, Ms(inclFrame[CAT_CHARACTER]));
        b.str(" ms (");
        b.dec(countFrame[CAT_CHARACTER]);
        b.str(" calls)");
        b.nl();
    }
    if (frameCallCount) {
        b.str("  during:");
        for (unsigned i = 0; i < frameCallCount; i++) {
            b.str(i ? ", " : " ");
            b.str(frameCalls[i]);
        }
        b.nl();
    }
    Out(b);
}

static double Percentile(const uint32_t* hist, uint32_t frames, double p, double maxMs) {
    if (!frames) return 0;
    uint32_t want = static_cast<uint32_t>(p * frames + 0.999999);
    if (want < 1) want = 1;
    uint32_t sum = 0;
    for (unsigned i = 0; i < HIST_BUCKETS; i++) {
        sum += hist[i];
        if (sum >= want) {
            double v = i < 1000 ? i * 0.1 : 100.0 + (i - 1000);   // the low end of the step
            return v > maxMs ? maxMs : v;
        }
    }
    return maxMs;
}

void Monitor::EmitStats(int64_t now) {
    double secs = Ms(now - winStart) / 1000.0;
    double maxMs = Ms(winMax);
    char text[600];
    Buf b(text, sizeof(text));
    b.str("stats ");
    PutMs(b, secs);
    b.str(" s: ");
    b.dec(winFrames);
    b.str(" frames, fps ");
    PutMs(b, secs > 0 ? winFrames / secs : 0);
    b.str(" avg, ");
    PutMs(b, winMinFps < 1e8 ? winMinFps : (secs > 0 ? winFrames / secs : 0));
    b.str(" worst second | frame ms p50 ");
    PutMs(b, Percentile(hist, winFrames, 0.50, maxMs));
    b.str(", p95 ");
    PutMs(b, Percentile(hist, winFrames, 0.95, maxMs));
    b.str(", max ");
    PutMs(b, maxMs);
    b.str(" | ");
    b.dec(winHitches);
    b.str(winHitches == 1 ? " hitch" : " hitches");
    b.str(" | time:");
    bool any = false;
    for (int c = 0; c < CAT_COUNT; c++) {
        double pct = winTicks ? 100.0 * static_cast<double>(selfWin[c]) / static_cast<double>(winTicks) : 0;
        if (pct < 0.05) continue;
        b.str(any ? ", " : " ");
        any = true;
        b.str(c == CAT_CALL ? "calls" : CatName(c));
        b.ch(' ');
        PutMs(b, pct);
        b.ch('%');
    }
    if (!any) b.str(" -");
    if (bgFileOps) {
        b.str(" | other threads: ");
        b.dec(bgFileOps);
        b.str(" file ops ");
        PutMs(b, Ms(bgFileTicks));
        b.str(" ms");
    }
    b.str(" | ");
    b.str(glueThisWindow ? screen : "world");
    b.nl();
    Out(b);
}

void Monitor::FrameEnd(int64_t now) {
    if (!lastFrame) {
        // the first frame: start counting from here
        lastFrame = now;
        winStart = secStart = now;
        Truncate(0);
        return;
    }
    // scopes still open (none should be, but a long load may draw from inside one): count them up to here
    for (int i = sp - 1; i >= 0; i--) {
        Scope& s = st[i];
        int64_t dur = now - s.start;
        if (dur < 0) dur = 0;
        int64_t self = dur - s.child;
        if (self < 0) self = 0;
        selfFrame[s.cat] += self;
        selfAll[s.cat] += self;
        selfWin[s.cat] += self;
        bool lowest = true;
        for (int j = 0; j < i; j++)
            if (st[j].cat == s.cat) lowest = false;
        if (lowest) inclFrame[s.cat] += dur;
        if (i > 0) st[i - 1].child += dur;
    }
    for (int i = 0; i < sp; i++) {
        st[i].start = now;
        st[i].child = 0;
    }

    int64_t ticks = now - lastFrame;
    if (ticks < 0) ticks = 0;
    double ms = Ms(ticks);
    frameNo++;

    winFrames++;
    secFrames++;
    winTicks += ticks;
    if (ticks > winMax) winMax = ticks;
    unsigned bucket = ms < 100 ? static_cast<unsigned>(ms * 10) : 1000 + static_cast<unsigned>(ms - 100);
    hist[bucket < static_cast<unsigned>(HIST_BUCKETS) ? bucket : static_cast<unsigned>(HIST_BUCKETS)]++;

    if (ms >= set.hitchMs) {
        winHitches++;
        hitchesAll++;
        EmitHitch(ms, ticks);
    }

    if (watch.on) {
        double v = watch.sawFrame ? ms : Ms(now - watch.start);
        bool slow = ms >= set.settleMs;
        if (watch.n < WATCH_FRAMES) {
            watch.slow[watch.n] = slow;
            watch.ms[watch.n++] = static_cast<float>(v);
        }
        if (!watch.sawFrame) watch.lastBad = now;   // "smooth again" is at least the end of this frame
        watch.sawFrame = true;
        if (slow) {
            watch.calm = 0;
            watch.lastBad = now;
        } else {
            watch.calm++;
        }
        if (watch.calm >= 3) EndWatch(now, nullptr);
        else if (watch.n >= WATCH_FRAMES) EndWatch(now, "still not smooth");
        else if (now - watch.start > 5 * freq) EndWatch(now, "still not smooth");
    }

    if (now - secStart >= freq) {
        double fps = static_cast<double>(secFrames) * static_cast<double>(freq) / static_cast<double>(now - secStart);
        if (fps < winMinFps) winMinFps = fps;
        secStart = now;
        secFrames = 0;
    }
    if (now - winStart >= static_cast<int64_t>(set.windowSec) * freq) {
        EmitStats(now);
        winStart = now;
        winFrames = 0;
        winTicks = 0;
        winMax = 0;
        winHitches = 0;
        winMinFps = 1e9;
        memset(hist, 0, sizeof(hist));
        memset(selfWin, 0, sizeof(selfWin));
        glueThisWindow = false;
        bgFileTicks = 0;
        bgFileOps = 0;
    }

    memset(selfFrame, 0, sizeof(selfFrame));
    memset(inclFrame, 0, sizeof(inclFrame));
    memset(countFrame, 0, sizeof(countFrame));
    lua.Reset();
    files.Reset();
    assets.Reset();
    frameCallCount = 0;
    lastFrame = now;
}

}  // namespace perf
}  // namespace tc
