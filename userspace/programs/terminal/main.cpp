// Terminal: a command line for Bhairav OS.
//
//   handle 1: the shell
//   handle 2: the file system
//   handle 3: the network
//
// Commands work with files (ls, cd, cat, write, mkdir, rm), the network
// (ip, ping, host, get) and the system (date, uptime, mem), and open apps
// (open, edit, browse). Type "help" for the list. Up/Down recall earlier
// commands; the wheel or Page Up/Down scroll back.

#include <kapala/files.h>
#include <kapala/net.h>
#include <kapala/protocol/input.h>
#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/tls.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

namespace {

constexpr int WIDTH = 760, HEIGHT = 460;
constexpr int PAD = 12, LINE_H = 18;
constexpr int MAX_LINES = 1200, LINE_CAP = 240;
constexpr int MAX_HISTORY = 32;

constexpr Color BACKGROUND = Color::hex(0x0E0A1C);
constexpr Color PROMPT = Theme::SAFFRON;
constexpr Color OUTPUT = Color::hex(0xE4DDF0);
constexpr Color ERROR = Color::hex(0xFF8FAB);
constexpr Color GOOD = Color::hex(0x7BE0A8);

unsigned long length(char const* s) {
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

void copy(char* dest, char const* src, unsigned long capacity) {
    unsigned long i = 0;
    for (; src[i] and i + 1 < capacity; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

bool same(char const* a, char const* b) {
    while (*a and *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void two(Text<64>& t, unsigned long v) {
    if (v < 10)
        t.add("0");
    t.add(v);
}

class Terminal {
public:
    bool open() {
        if (not _window.open("Terminal", WIDTH, HEIGHT, 1, true))
            return false;
        _filesOk = _files.connect(2);
        _netOk = _net.connect(3);
        print("Bhairav OS terminal. Type \"help\" to see the commands.", GOOD);
        print("");
        redraw();
        return true;
    }

    int run() {
        for (;;) {
            WindowEvent e;
            if (not _window.waitEvent(e, 530)) {
                _caret = not _caret;
                redraw();
                continue;
            }
            switch (e.kind) {
            case WindowEvent::Close: return 0;
            case WindowEvent::Resize: redraw(); break;
            case WindowEvent::Wheel: scrollBy(-e.wheel * 3); break;
            case WindowEvent::Key:
                if (e.pressed)
                    onKey(e);
                break;
            case WindowEvent::Focus: redraw(); break;
            default: break;
            }
        }
    }

private:
    // MARK: Output

    struct Line {
        char text[LINE_CAP];
        Color color;
    };

    void print(char const* text, Color color = OUTPUT) {
        // Split on newlines; long lines are wrapped when drawn.
        char const* start = text;
        for (char const* p = text;; p++) {
            if (*p == '\n' or *p == '\0') {
                Line& line = _lines[(_first + _count) % MAX_LINES];
                unsigned long n = static_cast<unsigned long>(p - start);
                if (n >= LINE_CAP)
                    n = LINE_CAP - 1;
                for (unsigned long i = 0; i < n; i++) {
                    char c = start[i];
                    line.text[i] = c == '\t' ? ' ' : (c < ' ' or c > '~') ? (c == '\r' ? ' ' : '?') : c;
                }
                line.text[n] = '\0';
                line.color = color;
                if (_count < MAX_LINES)
                    _count++;
                else
                    _first = (_first + 1) % MAX_LINES;
                if (not *p)
                    break;
                start = p + 1;
            }
        }
        _scrollBack = 0;
    }

    template <unsigned long N>
    void print(Text<N> const& text, Color color = OUTPUT) { print(text.cstr(), color); }

    void error(char const* text) { print(text, ERROR); }

    void errorCode(char const* what, long code) {
        Text<160> t;
        t.add(what).add(": ").add(describe(code));
        error(t.cstr());
    }

    static char const* describe(long code) {
        switch (code) {
        case Sys::NOT_FOUND: return "not found";
        case Sys::ALREADY_EXISTS: return "already exists";
        case Sys::NOT_EMPTY: return "the folder isn't empty";
        case Sys::IS_DIRECTORY: return "that's a folder";
        case Sys::NOT_DIRECTORY: return "not a folder";
        case Sys::INVALID_ARGUMENT: return "not a valid name";
        case Sys::NO_SPACE: return "the disk is full";
        case Sys::IO_ERROR: return "the disk failed";
        case Sys::TIMED_OUT: return "no answer in time";
        case Sys::CONNECTION_REFUSED: return "connection refused";
        case Sys::CONNECTION_RESET: return "connection reset";
        case Sys::NETWORK_DOWN: return "the network isn't up";
        default: return "failed";
        }
    }

    // MARK: Input

    void onKey(WindowEvent const& e) {
        switch (e.scancode) {
        case Protocol::KEY_UP: recall(-1); return;
        case Protocol::KEY_DOWN: recall(1); return;
        case Protocol::KEY_PAGE_UP: scrollBy(rows() - 2); return;
        case Protocol::KEY_PAGE_DOWN: scrollBy(-(rows() - 2)); return;
        default: break;
        }
        char ch = e.character;
        unsigned long n = length(_input);
        if (ch == '\n') {
            char command[sizeof(_input)];
            copy(command, _input, sizeof(command));
            Text<300> echo;
            echo.add(promptText()).add(command);
            print(echo.cstr(), PROMPT);
            _input[0] = '\0';
            remember(command);
            execute(command);
        } else if (ch == '\b') {
            if (n)
                _input[n - 1] = '\0';
        } else if (ch >= ' ' and ch <= '~' and n + 1 < sizeof(_input)) {
            _input[n] = ch;
            _input[n + 1] = '\0';
        }
        _caret = true;
        _scrollBack = 0;
        redraw();
    }

    void remember(char const* command) {
        if (not command[0])
            return;
        if (_historyCount == MAX_HISTORY) {
            for (int i = 1; i < MAX_HISTORY; i++)
                copy(_history[i - 1], _history[i], sizeof(_history[0]));
            _historyCount--;
        }
        copy(_history[_historyCount++], command, sizeof(_history[0]));
        _historyAt = _historyCount;
    }

    void recall(int step) {
        int at = _historyAt + step;
        if (at < 0 or at > _historyCount)
            return;
        _historyAt = at;
        copy(_input, at == _historyCount ? "" : _history[at], sizeof(_input));
        redraw();
    }

    void scrollBy(int lines) {
        _scrollBack += lines;
        if (_scrollBack > _count - 1)
            _scrollBack = _count - 1;
        if (_scrollBack < 0)
            _scrollBack = 0;
        redraw();
    }

    // MARK: Commands

    // Splits the line into words ("quoted words" stay together).
    int split(char* line, char** words, int max) {
        int n = 0;
        char* p = line;
        while (*p and n < max) {
            while (*p == ' ')
                p++;
            if (not *p)
                break;
            if (*p == '"') {
                words[n++] = ++p;
                while (*p and *p != '"')
                    p++;
            } else {
                words[n++] = p;
                while (*p and *p != ' ')
                    p++;
            }
            if (*p)
                *p++ = '\0';
        }
        return n;
    }

    // An absolute path from one relative to the current folder.
    void resolve(char const* path, char* out, unsigned long capacity) {
        char full[192];
        if (path[0] == '/') {
            copy(full, path, sizeof(full));
        } else {
            copy(full, _cwd, sizeof(full));
            unsigned long n = length(full);
            if (n == 0 or full[n - 1] != '/')
                copy(full + n, "/", sizeof(full) - n);
            n = length(full);
            copy(full + n, path, sizeof(full) - n);
        }
        // Drop "." and resolve "..".
        char result[192] = "";
        unsigned long r = 0;
        char const* p = full;
        while (*p) {
            while (*p == '/')
                p++;
            char const* start = p;
            while (*p and *p != '/')
                p++;
            unsigned long len = static_cast<unsigned long>(p - start);
            if (len == 0 or (len == 1 and start[0] == '.'))
                continue;
            if (len == 2 and start[0] == '.' and start[1] == '.') {
                while (r > 0 and result[r - 1] != '/')
                    r--;
                if (r > 0)
                    r--;
                result[r] = '\0';
                continue;
            }
            if (r + len + 2 >= sizeof(result))
                break;
            result[r++] = '/';
            for (unsigned long i = 0; i < len; i++)
                result[r++] = start[i];
            result[r] = '\0';
        }
        copy(out, r ? result : "/", capacity);
    }

    char const* promptText() {
        _prompt = {};
        _prompt.add("bhairav:").add(_cwd).add("$ ");
        return _prompt.cstr();
    }

    void execute(char* line) {
        char* w[16];
        int n = split(line, w, 16);
        if (n == 0)
            return;
        char const* cmd = w[0];

        if (same(cmd, "help")) {
            print("Files:   ls [folder]  cd folder  pwd  cat file  write file text...  mkdir folder  rm path", OUTPUT);
            print("Network: ip  ping host [count]  host name  get http(s)://address", OUTPUT);
            print("System:  date  uptime  mem  echo text...  clear", OUTPUT);
            print("Apps:    open browser|files|notes|monitor|about|terminal  edit file  browse address", OUTPUT);
        } else if (same(cmd, "clear")) {
            _count = 0;
            _first = 0;
        } else if (same(cmd, "echo")) {
            Text<LINE_CAP> t;
            for (int i = 1; i < n; i++)
                t.add(w[i]).add(i + 1 < n ? " " : "");
            print(t);
        } else if (same(cmd, "pwd")) {
            print(_cwd);
        } else if (same(cmd, "ls")) {
            ls(n > 1 ? w[1] : ".");
        } else if (same(cmd, "cd")) {
            cd(n > 1 ? w[1] : "/");
        } else if (same(cmd, "cat")) {
            if (n < 2)
                return error("usage: cat file");
            cat(w[1]);
        } else if (same(cmd, "write")) {
            if (n < 2)
                return error("usage: write file text...");
            Text<1024> text;
            for (int i = 2; i < n; i++)
                text.add(w[i]).add(i + 1 < n ? " " : "");
            text.add("\n");
            char path[96];
            resolve(w[1], path, sizeof(path));
            long r = _files.write(path, text.cstr(), text.length);
            if (r < 0)
                errorCode(path, r);
        } else if (same(cmd, "mkdir")) {
            if (n < 2)
                return error("usage: mkdir folder");
            char path[96];
            resolve(w[1], path, sizeof(path));
            long r = _files.makeDir(path);
            if (r < 0)
                errorCode(path, r);
        } else if (same(cmd, "rm")) {
            if (n < 2)
                return error("usage: rm path");
            char path[96];
            resolve(w[1], path, sizeof(path));
            long r = _files.remove(path);
            if (r < 0)
                errorCode(path, r);
        } else if (same(cmd, "ip")) {
            ip();
        } else if (same(cmd, "ping")) {
            if (n < 2)
                return error("usage: ping host [count]");
            int count = n > 2 ? static_cast<int>(number(w[2])) : 4;
            ping(w[1], count < 1 ? 1 : count > 20 ? 20 : count);
        } else if (same(cmd, "host")) {
            if (n < 2)
                return error("usage: host name");
            host(w[1]);
        } else if (same(cmd, "get")) {
            if (n < 2)
                return error("usage: get http(s)://address");
            get(w[1]);
        } else if (same(cmd, "date")) {
            date();
        } else if (same(cmd, "uptime")) {
            Sys::SystemInfo info{};
            Sys::systemInfo(info);
            unsigned long s = info.uptimeMs / 1000;
            Text<80> t;
            t.add("up ").add(s / 3600).add(" h ").add(s / 60 % 60).add(" min ").add(s % 60).add(" s, ")
                .add(info.taskCount).add(" tasks");
            print(t);
        } else if (same(cmd, "mem")) {
            Sys::SystemInfo info{};
            Sys::systemInfo(info);
            Text<120> t;
            t.add("memory: ").add((info.totalMemory - info.freeMemory) / (1024 * 1024)).add(" MiB used, ")
                .add(info.freeMemory / (1024 * 1024)).add(" MiB free of ").add(info.totalMemory / (1024 * 1024))
                .add(" MiB");
            print(t);
        } else if (same(cmd, "open")) {
            if (n < 2)
                return error("usage: open browser|files|notes|monitor|about|terminal");
            static constexpr char const* APPS[] = {"browser", "files", "notes", "monitor", "about", "terminal"};
            for (char const* app : APPS) {
                if (same(w[1], app)) {
                    _window.launch(app);
                    return;
                }
            }
            error("no such app");
        } else if (same(cmd, "edit")) {
            if (n < 2)
                return error("usage: edit file");
            char path[96];
            resolve(w[1], path, sizeof(path));
            _window.launch("notes", path);
        } else if (same(cmd, "browse")) {
            _window.launch("browser", n > 1 ? w[1] : "");
        } else {
            Text<120> t;
            t.add(cmd).add(": unknown command (try \"help\")");
            error(t.cstr());
        }
    }

    static long number(char const* s) {
        long v = 0;
        while (*s >= '0' and *s <= '9')
            v = v * 10 + (*s++ - '0');
        return v;
    }

    void ls(char const* where) {
        char path[96];
        resolve(where, path, sizeof(path));
        static FileEntry entries[256];
        long n = _files.list(path, entries, 256);
        if (n < 0)
            return errorCode(path, n);
        if (n == 0)
            print("(empty)", Theme::TEXT_DIM);
        for (long i = 0; i < n; i++) {
            Text<120> t;
            if (entries[i].directory) {
                t.add("  <folder>  ").add(entries[i].name).add("/");
                print(t, Theme::GOLD);
            } else {
                Text<16> size;
                size.add(entries[i].size);
                t.add("  ");
                for (unsigned long pad = size.length; pad < 8; pad++)
                    t.add(" ");
                t.add(size.cstr()).add("  ").add(entries[i].name);
                print(t);
            }
        }
    }

    void cd(char const* where) {
        char path[96];
        resolve(where, path, sizeof(path));
        FileInfo info{};
        long r = _files.stat(path, info);
        if (r < 0)
            return errorCode(path, r);
        if (not info.directory)
            return error("not a folder");
        copy(_cwd, path, sizeof(_cwd));
    }

    void cat(char const* file) {
        char path[96];
        resolve(file, path, sizeof(path));
        static char data[32 * 1024];
        long n = _files.read(path, data, sizeof(data) - 1);
        if (n < 0)
            return errorCode(path, n);
        data[n] = '\0';
        print(data);
        if (n == static_cast<long>(sizeof(data)) - 1)
            print("... (only the first 32 KB)", Theme::TEXT_DIM);
    }

    void ip() {
        NetStatus s{};
        if (not _netOk or _net.status(s) < 0)
            return error("no network connection");
        static constexpr char HEX[] = "0123456789abcdef";
        Text<120> t;
        t.add("card:     e1000, mac ");
        for (int i = 0; i < 6; i++) {
            char b[3] = {HEX[s.mac[i] >> 4], HEX[s.mac[i] & 15], 0};
            t.add(b).add(i < 5 ? ":" : "");
        }
        print(t);
        if (not s.up)
            return print("address:  none yet (waiting for DHCP)");
        Text<120> a, g, d;
        a.add("address:  ");
        Net::format(s.address, a);
        a.add(" / ");
        Net::format(s.netmask, a);
        g.add("gateway:  ");
        Net::format(s.gateway, g);
        d.add("dns:      ");
        Net::format(s.dns, d);
        print(a);
        print(g);
        print(d);
    }

    void ping(char const* name, int count) {
        unsigned address;
        long r = _net.resolve(name, address);
        if (r < 0)
            return errorCode(name, r);
        Text<120> head;
        head.add("PING ").add(name).add(" (");
        Net::format(address, head);
        head.add(")");
        print(head);
        redraw();
        int received = 0;
        for (int i = 0; i < count; i++) {
            long ms = _net.ping(address, 2000);
            Text<120> t;
            if (ms >= 0) {
                received++;
                t.add("reply from ");
                Net::format(address, t);
                t.add(": seq=").add(i + 1).add(" time=").add(ms).add(" ms");
                print(t);
            } else {
                t.add("seq=").add(i + 1).add(": ").add(describe(ms));
                print(t.cstr(), ERROR);
            }
            redraw();
            if (i + 1 < count)
                Sys::sleep(ms >= 0 and ms < 1000 ? 1000 - static_cast<unsigned long>(ms) : 0);
        }
        Text<80> t;
        t.add(count).add(" sent, ").add(received).add(" received");
        print(t.cstr(), received == count ? GOOD : ERROR);
    }

    void host(char const* name) {
        unsigned address;
        unsigned long start = Sys::timeMs();
        long r = _net.resolve(name, address);
        if (r < 0)
            return errorCode(name, r);
        Text<120> t;
        t.add(name).add(" has address ");
        Net::format(address, t);
        t.add(" (").add(Sys::timeMs() - start).add(" ms)");
        print(t);
    }

    // Fetches a page and prints the response (headers and the start of the body).
    void get(char const* address) {
        char const* rest = address;
        bool secure = rest[0] == 'h' and rest[1] == 't' and rest[2] == 't' and rest[3] == 'p' and rest[4] == 's';
        if (secure)
            rest += 8;
        else if (rest[0] == 'h' and rest[1] == 't' and rest[2] == 't' and rest[3] == 'p' and rest[4] == ':')
            rest += 7;
        char hostName[96];
        unsigned long h = 0;
        while (rest[h] and rest[h] != '/' and h + 1 < sizeof(hostName)) {
            hostName[h] = rest[h];
            h++;
        }
        hostName[h] = '\0';
        char const* path = rest[h] ? rest + h : "/";

        unsigned ip;
        long r = _net.resolve(hostName, ip);
        if (r < 0)
            return errorCode(hostName, r);
        long socket = _net.tcpConnect(ip, secure ? 443 : 80);
        if (socket < 0)
            return errorCode(hostName, socket);
        Tls tls;
        if (secure) {
            if (tls.connect(_net, socket, hostName) < 0) {
                Text<160> t;
                t.add("TLS failed: ").add(tls.error());
                tls.close();
                return error(t.cstr());
            }
            Text<120> t;
            t.add("(encrypted: ").add(tls.cipher()).add("; certificate not checked)");
            print(t.cstr(), Theme::TEXT_DIM);
        }
        Text<400> request;
        request.add("GET ").add(path).add(" HTTP/1.1\r\nHost: ").add(hostName)
            .add("\r\nUser-Agent: BhairavOS-terminal\r\nConnection: close\r\n\r\n");
        if (secure)
            tls.send(request.cstr(), request.length);
        else
            _net.send(socket, request.cstr(), request.length);
        static char data[24 * 1024];
        unsigned long total = 0;
        unsigned long start = Sys::timeMs();
        for (;;) {
            long got = secure ? tls.receive(data + total, sizeof(data) - 1 - total, 10000)
                              : _net.receive(socket, data + total, sizeof(data) - 1 - total, 10000);
            if (got <= 0)
                break;
            total += static_cast<unsigned long>(got);
            if (total >= sizeof(data) - 1)
                break;
        }
        if (secure)
            tls.close();
        else
            _net.close(socket);
        data[total] = '\0';
        print(data);
        Text<80> t;
        t.add("(").add(total).add(" bytes in ").add(Sys::timeMs() - start).add(" ms")
            .add(total >= sizeof(data) - 1 ? ", cut at 24 KB)" : ")");
        print(t.cstr(), Theme::TEXT_DIM);
    }

    void date() {
        Sys::SystemInfo info{};
        Sys::systemInfo(info);
        unsigned long t = info.unixTime;
        long days = static_cast<long>(t / 86400), rest = static_cast<long>(t % 86400);
        long z = days + 719468, era = z / 146097, doe = z - era * 146097;
        long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        long day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
        long year = yoe + era * 400 + (month <= 2);
        static constexpr char const* DAYS[] = {"Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday"};
        Text<64> out;
        out.add(DAYS[days % 7]).add(" ").add(year).add("-");
        two(out, static_cast<unsigned long>(month));
        out.add("-");
        two(out, static_cast<unsigned long>(day));
        out.add(" ");
        two(out, static_cast<unsigned long>(rest / 3600));
        out.add(":");
        two(out, static_cast<unsigned long>(rest / 60 % 60));
        out.add(":");
        two(out, static_cast<unsigned long>(rest % 60));
        out.add(" UTC");
        print(out);
    }

    // MARK: Drawing

    int columns() const { return (_window.width() - 2 * PAD) / Font::mono().width; }
    int rows() const { return (_window.height() - 2 * PAD) / LINE_H; }

    // How many screen rows a line takes when wrapped.
    int rowsFor(char const* text) const {
        int n = static_cast<int>(length(text));
        int cols = columns() > 0 ? columns() : 1;
        return n == 0 ? 1 : (n + cols - 1) / cols;
    }

    void redraw() {
        Canvas c(_window.surface());
        c.fill(_window.surface().bounds(), BACKGROUND);
        Font const& f = Font::mono();
        int cols = columns() > 0 ? columns() : 1;
        int totalRows = rows();

        // The input line (prompt + what's typed) is the last row(s).
        Text<400> current;
        current.add(promptText()).add(_input);
        int inputRows = rowsFor(current.cstr());

        // Walk back from the newest line until the screen is full.
        int y = PAD + (totalRows - inputRows) * LINE_H;
        int skip = _scrollBack;
        char chunk[LINE_CAP];
        for (int k = _count - 1; k >= 0 and y > PAD - LINE_H; k--) {
            Line const& line = _lines[(_first + k) % MAX_LINES];
            if (skip > 0) {
                skip--;
                continue;
            }
            int r = rowsFor(line.text);
            y -= r * LINE_H;
            for (int row = 0; row < r; row++) {
                copy(chunk, line.text + row * cols, static_cast<unsigned long>(cols) + 1);
                if (y + row * LINE_H >= PAD - LINE_H)
                    c.text({PAD, y + row * LINE_H}, chunk, line.color, f);
            }
        }

        int iy = PAD + (totalRows - inputRows) * LINE_H;
        if (_scrollBack == 0) {
            char const* text = current.cstr();
            unsigned long promptLen = length(promptText());
            int x = PAD;
            for (int row = 0; row < inputRows; row++) {
                copy(chunk, text + row * cols, static_cast<unsigned long>(cols) + 1);
                c.text({PAD, iy + row * LINE_H}, chunk, OUTPUT, f);
                if (row == 0) { // the prompt in its own color
                    char p[128];
                    copy(p, text, promptLen + 1 < sizeof(p) ? promptLen + 1 : sizeof(p));
                    c.text({PAD, iy}, p, PROMPT, f);
                }
                x = PAD + static_cast<int>(length(chunk)) * f.width;
            }
            if (_caret and _window.focused())
                c.fill({x, iy + (inputRows - 1) * LINE_H, f.width, f.height}, Theme::SAFFRON.withAlpha(200));
        } else {
            Text<64> t;
            t.add("-- scrolled back ").add(_scrollBack).add(" lines: Page Down returns --");
            c.text({PAD, iy}, t.cstr(), Theme::TEXT_DIM, f);
        }
        _window.present();
    }

    Window _window;
    Files _files;
    Net _net;
    bool _filesOk = false, _netOk = false;

    Line _lines[MAX_LINES];
    int _first = 0, _count = 0, _scrollBack = 0;

    char _input[200] = {};
    char _cwd[96] = "/";
    Text<128> _prompt;
    bool _caret = true;

    char _history[MAX_HISTORY][200] = {};
    int _historyCount = 0, _historyAt = 0;
};

} // namespace

int main() {
    auto* t = new Terminal();
    if (not t->open())
        return 1;
    return t->run();
}
