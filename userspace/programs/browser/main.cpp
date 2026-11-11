// Bhairav Browser: a web browser for Bhairav OS.
//
//   handle 1: the shell
//   handle 2: the file system (unused for now)
//   handle 3: the network
//
// Loads pages over HTTP (net, http.cpp), lays them out (html.cpp) and
// draws them on a paper-colored page. Links, an address bar that also
// searches (through FrogFind, a search engine made for simple browsers),
// back/reload/home, and scrolling with the wheel, keys or the scrollbar.

#include "html.h"
#include "http.h"
#include "url.h"

#include <kapala/image.h>
#include <kapala/net.h>
#include <kapala/protocol/input.h>
#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;
using namespace Browser;

namespace {

constexpr int WIDTH = 1000;
constexpr int HEIGHT = 600;
constexpr int TOOLBAR_H = 48;
constexpr int STATUS_H = 26;
constexpr int SCROLLBAR_W = 12;
constexpr int PAD_X = 28, PAD_Y = 18;
constexpr int MAX_HISTORY = 32;
constexpr int MAX_REDIRECTS = 6;
constexpr int MAX_IMAGES = 48;     // pictures loaded per page
constexpr u64 MAX_IMAGE_BYTES = 4 * 1024 * 1024;

// A picture of the current page.
struct CachedImage {
    enum State : u8 { Pending, Ready, Failed };
    char url[Url::MAX_HOST + Url::MAX_PATH + 16];
    State state = Pending;
    Kapala::Image image; // as decoded
    Kapala::Image shown; // scaled to the size it's shown at
};

constexpr Rect BACK{10, 8, 32, 32};
constexpr Rect RELOAD{44, 8, 32, 32};
constexpr Rect HOME{78, 8, 32, 32};

// These follow the window's size (see App::updateLayout).
int windowW = WIDTH, windowH = HEIGHT;
Rect PAGE, SCROLLBAR, ADDRESS;

void updateLayout(int w, int h) {
    windowW = w;
    windowH = h;
    PAGE = {0, TOOLBAR_H, w - SCROLLBAR_W, h - TOOLBAR_H - STATUS_H};
    SCROLLBAR = {w - SCROLLBAR_W, TOOLBAR_H, SCROLLBAR_W, h - TOOLBAR_H - STATUS_H};
    ADDRESS = {122, 8, w - 132, 32};
}

char const HOME_PAGE[] = R"(<title>Bhairav Browser</title>
<h1>Bhairav Browser</h1>
<p>Type an address in the bar above and press Enter, or type some words to search the web.</p>
<h3>Places to visit</h3>
<ul>
<li><a href="http://info.cern.ch/hypertext/WWW/TheProject.html">The World Wide Web project</a> - the first website (CERN, 1991)</li>
<li><a href="http://frogfind.com/">FrogFind</a> - a search engine that simplifies pages for small browsers</li>
<li><a href="http://68k.news/">68k.news</a> - today's headlines, made for vintage computers</li>
<li><a href="https://en.wikipedia.org/wiki/Bhairava">Wikipedia: Bhairava</a> - over HTTPS</li>
<li><a href="https://example.com/">example.com</a> - the classic example page</li>
<li><a href="http://neverssl.com/">NeverSSL</a> - a site that always stays on plain HTTP</li>
</ul>
<h3>Keys</h3>
<ul>
<li>Mouse wheel, arrow keys, Page Up / Page Down, Home / End: scroll</li>
<li>Ctrl+L: type an address; F5: reload; Backspace: go back</li>
</ul>
<h3>What works</h3>
<p>Bhairav Browser reads <b>HTTP</b> and <b>HTTPS</b> pages (TLS 1.3): text, headings, lists, links,
simple tables and pictures. Scripts don't run, and page styles are ignored.</p>
<p><b>Note:</b> HTTPS connections are encrypted, but the site's certificate isn't checked yet,
so the browser can't be sure it's talking to the real site. Don't type passwords.</p>
<hr>
<p><small>Bhairav OS, built from scratch: ShoolLoader, RudraCore, Kapalaframe and KamalShell.</small></p>
)";

// MARK: Browser

class App {
public:
    bool open() {
        updateLayout(WIDTH, HEIGHT);
        if (not _window.open("Bhairav Browser", WIDTH, HEIGHT, 1, true))
            return false;
        _netOk = _net.connect(3);
        if (_window.argument()[0]) // launched with an address (e.g. "browse" in the Terminal)
            go(_window.argument());
        else
            navigateTo("about:home", true);
        return true;
    }

    int run() {
        for (;;) {
            WindowEvent e;
            bool loading = nextPendingImage() >= 0;
            if (not _window.waitEvent(e, loading ? 0 : 500)) {
                if (loading) {
                    loadNextImage(); // one at a time, so events still get through
                } else if (_editing) {
                    _caret = not _caret;
                    redraw();
                }
                continue;
            }
            switch (e.kind) {
            case WindowEvent::Close: return 0;
            case WindowEvent::Key: onKey(e); break;
            case WindowEvent::MouseDown: onMouseDown(e.at); break;
            case WindowEvent::MouseUp: _draggingThumb = false; break;
            case WindowEvent::MouseMove: onMouseMove(e.at); break;
            case WindowEvent::Wheel: scrollBy(e.wheel * 60); break;
            case WindowEvent::Resize: onResize(); break;
            case WindowEvent::Focus:
                _ctrl = false;
                redraw();
                break;
            default: break;
            }
        }
    }

private:
    // MARK: Navigation

    // Loads an address typed in the bar, or searches for it.
    void go(char const* typed) {
        while (isSpace(*typed))
            typed++;
        if (not *typed)
            return;

        bool hasSpace = false, hasDot = false;
        for (char const* p = typed; *p; p++) {
            hasSpace |= *p == ' ';
            hasDot |= *p == '.';
        }
        bool looksLikeAddress = startsWith(typed, "http://") or startsWith(typed, "https://") or
                                startsWith(typed, "about:") or (hasDot and not hasSpace);
        if (looksLikeAddress) {
            navigateTo(typed, true);
            return;
        }
        char query[600];
        encodeQuery(typed, query, sizeof(query));
        char address[700] = "http://frogfind.com/?q=";
        append(address, query, sizeof(address));
        navigateTo(address, true);
    }

    void navigateTo(char const* address, bool remember) {
        Url url;
        if (not url.parse(address)) {
            _remember = remember;
            showMessage("Not an address", "That doesn't look like a web address.", address);
            return;
        }
        load(url, remember);
    }

    void load(Url url, bool remember) {
        _editing = false;
        _hoverLink = -1;
        _remember = remember; // error pages take this navigation's place in history too

        for (int redirects = 0;; redirects++) {
            url.format(_address, sizeof(_address));
            if (url.isAbout()) {
                if (not equals(url.path, "home")) {
                    showMessage("Unknown page", "There's no built-in page with that name.", _address);
                    return;
                }
                show(url, HOME_PAGE, sizeof(HOME_PAGE) - 1, false, remember);
                setStatus("Ready");
                return;
            }
            if (not _netOk) {
                showMessage("No network", "Bhairav OS has no network connection.", _address);
                return;
            }

            unsigned long started = Sys::timeMs();
            setStatus("Loading...");
            fetch(_net, url, _response, [](char const* message) { current->setStatus(message); });
            if (_response.error != Sys::OK) {
                if (url.isHttps() and _response.security[0])
                    showHttps(url, remember, _response.security); // the TLS handshake failed
                else
                    showError(url, _response.error);
                return;
            }

            bool redirect = _response.status >= 300 and _response.status < 400 and _response.location[0];
            if (redirect and redirects < MAX_REDIRECTS) {
                Url next;
                if (next.resolve(url, _response.location)) {
                    url = next;
                    continue;
                }
            }

            bool plain = startsWith(_response.contentType, "text/plain");
            bool html = not _response.contentType[0] or startsWith(_response.contentType, "text/html") or
                        startsWith(_response.contentType, "application/xhtml");
            if (not plain and not html) {
                Text<200> detail;
                detail.add("It is ").add(_response.contentType).add(", and Bhairav Browser only shows web pages and text.");
                showMessage("Can't show this file", detail.cstr(), _address);
                return;
            }
            show(url, _response.body.data(), _response.body.size() - 1, plain, remember);

            Text<200> status;
            status.add("Done: ").add(_response.body.size() / 1024).add(" KB in ")
                .add(Sys::timeMs() - started).add(" ms");
            if (_response.status != 200)
                status.add(" (HTTP ").add(_response.status).add(")");
            if (url.isHttps())
                status.add("  |  encrypted: ").add(_response.security).add(" - the site's certificate isn't checked yet");
            setStatus(status.cstr());
            return;
        }
    }

    // Lays out and shows a page (keeping its source for later re-layouts).
    void show(Url const& url, char const* source, u64 length, bool plain, bool remember) {
        if (remember) {
            if (_historyCount == MAX_HISTORY) {
                for (int i = 1; i < MAX_HISTORY; i++)
                    _history[i - 1] = _history[i];
                _historyCount--;
            }
            _history[_historyCount++] = url;
        } else if (_historyCount) {
            _history[_historyCount - 1] = url;
        }
        _url = url;
        _source.clear();
        _source.append(source, length);
        _source.push('\0');
        _plain = plain;
        forgetImages();
        relayout();
        queueImages();
        _scroll = 0;

        Text<160> title;
        title.add(_doc.title[0] ? _doc.title : url.host[0] ? url.host : "Bhairav Browser");
        _window.setTitle(title.cstr());
        redraw();
    }

    void showMessage(char const* heading, char const* detail, char const* address) {
        Vector<char> page;
        auto add = [&](char const* s) { page.append(s, length(s)); };
        auto addEscaped = [&](char const* s) {
            for (; *s; s++) {
                if (*s == '<')
                    add("&lt;");
                else if (*s == '&')
                    add("&amp;");
                else
                    page.push(*s);
            }
        };
        add("<title>");
        addEscaped(heading);
        add("</title><h2>");
        addEscaped(heading);
        add("</h2><p>");
        addEscaped(detail);
        add("</p><p><code>");
        addEscaped(address);
        add("</code></p><p><a href=\"about:home\">Go to the start page</a></p>");
        Url url;
        url.parse("about:message");
        if (address[0])
            url.parse(address);
        show(url, page.data(), page.size(), false, _remember);
        _remember = false;
        setStatus(heading);
    }

    void showError(Url const& url, long error) {
        char const* why = error == Sys::NOT_FOUND          ? "The name couldn't be found (DNS)."
                          : error == Sys::CONNECTION_REFUSED ? "The server refused the connection."
                          : error == Sys::CONNECTION_RESET   ? "The server closed the connection."
                          : error == Sys::TIMED_OUT          ? "The server didn't answer in time."
                          : error == Sys::NETWORK_DOWN       ? "The network isn't up yet. Try again in a moment."
                                                             : "Something went wrong while loading it.";
        char address[Url::MAX_HOST + Url::MAX_PATH + 16];
        url.format(address, sizeof(address));
        showMessage("Couldn't load the page", why, address);
    }

    // A secure connection couldn't be made: say why, offer alternatives.
    void showHttps(Url const& url, bool remember, char const* reason) {
        char address[Url::MAX_HOST + Url::MAX_PATH + 16];
        url.format(address, sizeof(address));
        char encoded[1600];
        encodeQuery(address, encoded, sizeof(encoded));

        Vector<char> page;
        auto add = [&](char const* s) { page.append(s, length(s)); };
        add("<title>Secure connection failed</title><h2>Couldn't make a secure connection</h2>");
        add("<p><code>");
        add(address);
        add("</code></p><p>Reason: ");
        add(reason);
        add(".</p><p>Bhairav Browser speaks TLS 1.3 with X25519 keys, which most sites support. You can:</p><ul>");
        add("<li><a href=\"http://frogfind.com/read.php?a=");
        add(encoded);
        add("\">Read a simplified copy through FrogFind</a>. FrogFind, a third-party service, fetches the page for you over HTTPS and sends back plain HTML.</li>");
        add("<li><a href=\"http://");
        add(url.host);
        add(url.path);
        add("\">Try the plain HTTP address</a>. Many sites redirect straight back to HTTPS.</li></ul>");
        add("<p><a href=\"about:home\">Go to the start page</a></p>");
        _remember = remember;
        show(url, page.data(), page.size(), false, remember);
        setStatus("Secure connection failed");
    }

    // MARK: Pictures

    // The page's layout, knowing the sizes of the pictures loaded so far.
    void relayout() {
        _doc.clear();
        layout(_source.data(), _source.size() ? _source.size() - 1 : 0, _plain, PAGE.w - 2 * PAD_X, _doc,
               [](char const* src, int& w, int& h) {
                   int i = current->findImage(src);
                   if (i < 0 or current->_images[i].state != CachedImage::Ready)
                       return false;
                   w = current->_images[i].image.width;
                   h = current->_images[i].image.height;
                   return true;
               });
    }

    void forgetImages() {
        for (int i = 0; i < _imageCount; i++)
            _images[i] = CachedImage{};
        _imageCount = 0;
    }

    // The cache slot for a picture address as written on the page, or -1.
    int findImage(char const* src) {
        Url u;
        if (not u.resolve(_url, src))
            return -1;
        char address[sizeof(CachedImage::url)];
        u.format(address, sizeof(address));
        for (int i = 0; i < _imageCount; i++) {
            if (equals(_images[i].url, address))
                return i;
        }
        return -1;
    }

    void queueImages() {
        for (u64 k = 0; k < _doc.images.size() and _imageCount < MAX_IMAGES; k++) {
            char const* src = _doc.at(_doc.images[k].src);
            if (findImage(src) >= 0)
                continue;
            Url u;
            if (not u.resolve(_url, src) or u.isAbout())
                continue;
            CachedImage& slot = _images[_imageCount++];
            slot = CachedImage{};
            u.format(slot.url, sizeof(slot.url));
        }
    }

    int nextPendingImage() const {
        for (int i = 0; i < _imageCount; i++) {
            if (_images[i].state == CachedImage::Pending)
                return i;
        }
        return -1;
    }

    void loadNextImage() {
        int i = nextPendingImage();
        if (i < 0)
            return;
        CachedImage& slot = _images[i];
        slot.state = CachedImage::Failed;
        int done = 0;
        for (int k = 0; k < _imageCount; k++)
            done += _images[k].state != CachedImage::Pending;
        Text<120> status;
        status.add("Loading pictures: ").add(done).add(" of ").add(_imageCount);
        setStatus(status.cstr());

        Url url;
        if (not url.parse(slot.url) or not _netOk)
            return finishImages();
        for (int redirects = 0; redirects < 4; redirects++) {
            fetch(_net, url, _response, [](char const*) {});
            if (_response.error != Sys::OK)
                return finishImages();
            if (_response.status >= 300 and _response.status < 400 and _response.location[0]) {
                Url next;
                if (not next.resolve(url, _response.location))
                    return finishImages();
                url = next;
                continue;
            }
            break;
        }
        u64 length = _response.body.size() ? _response.body.size() - 1 : 0;
        if (_response.status == 200 and length and length <= MAX_IMAGE_BYTES and
            Kapala::Image::decode(reinterpret_cast<unsigned char const*>(_response.body.data()), length, slot.image,
                                  8 * 1024 * 1024)) {
            slot.state = CachedImage::Ready;
            int oldHeight = _doc.height > 0 ? _doc.height : 1;
            int scroll = _scroll;
            relayout();
            _scroll = -1;
            setScroll(scroll == 0 ? 0 : static_cast<int>(static_cast<long>(scroll) * _doc.height / oldHeight));
        }
        finishImages();
    }

    void finishImages() {
        if (nextPendingImage() >= 0) {
            redraw();
            return;
        }
        int ready = 0;
        for (int k = 0; k < _imageCount; k++)
            ready += _images[k].state == CachedImage::Ready;
        if (_imageCount) {
            Text<200> status;
            status.add("Done: ").add(ready).add(ready == 1 ? " picture" : " pictures");
            if (ready < _imageCount)
                status.add(" (").add(_imageCount - ready).add(" in formats not supported yet, or missing)");
            setStatus(status.cstr());
        }
    }

    // Lays the page out again for the new width, keeping the reading position.
    void onResize() {
        int oldHeight = _doc.height > 0 ? _doc.height : 1;
        int scroll = _scroll;
        updateLayout(_window.width(), _window.height());
        if (not _source.empty()) {
            relayout();
        }
        int target = static_cast<int>(static_cast<long>(scroll) * _doc.height / oldHeight);
        _scroll = -1; // so setScroll always applies (and clamps) it
        setScroll(target);
        redraw();
    }

    void back() {
        if (_historyCount < 2)
            return;
        _historyCount--;
        load(_history[_historyCount - 1], false);
    }

    void reload() { load(_url, false); }

    // MARK: Input

    void onKey(WindowEvent const& e) {
        if (e.scancode == Protocol::KEY_CTRL) {
            _ctrl = e.pressed;
            return;
        }
        if (not e.pressed)
            return;
        if (_ctrl and e.character == 'l') {
            startEditing();
            return;
        }
        if (e.scancode == Protocol::KEY_F5) {
            reload();
            return;
        }

        if (_editing) {
            if (e.character == '\n') {
                _editing = false;
                char typed[sizeof(_typed)];
                copy(typed, _typed, sizeof(typed));
                go(typed);
            } else if (e.character == 0x1b) {
                _editing = false;
                redraw();
            } else if (e.character == '\b') {
                u64 n = length(_typed);
                if (_selected)
                    _typed[0] = '\0';
                else if (n)
                    _typed[n - 1] = '\0';
                _selected = false;
                redraw();
            } else if (e.character >= ' ' and e.character <= '~' and length(_typed) + 1 < sizeof(_typed)) {
                if (_selected)
                    _typed[0] = '\0'; // typing replaces the selected address
                _selected = false;
                char ch[2] = {e.character, 0};
                append(_typed, ch, sizeof(_typed));
                _caret = true;
                redraw();
            } else if (e.scancode == Protocol::KEY_END or e.scancode == Protocol::KEY_RIGHT) {
                _selected = false;
                redraw();
            }
            return;
        }

        int view = PAGE.h;
        switch (e.scancode) {
        case Protocol::KEY_DOWN: scrollBy(40); return;
        case Protocol::KEY_UP: scrollBy(-40); return;
        case Protocol::KEY_PAGE_DOWN: scrollBy(view - 40); return;
        case Protocol::KEY_PAGE_UP: scrollBy(-(view - 40)); return;
        case Protocol::KEY_HOME: scrollBy(-_doc.height); return;
        case Protocol::KEY_END: scrollBy(_doc.height); return;
        default: break;
        }
        if (e.character == ' ')
            scrollBy(view - 40);
        else if (e.character == '\b')
            back();
    }

    void startEditing() {
        _editing = true;
        _caret = true;
        _selected = true; // all of it, so typing replaces it
        copy(_typed, _address, sizeof(_typed));
        redraw();
    }

    void onMouseDown(Point at) {
        if (ADDRESS.contains(at)) {
            if (not _editing)
                startEditing();
            return;
        }
        if (_editing) {
            _editing = false;
            redraw();
        }
        if (BACK.contains(at)) {
            back();
        } else if (RELOAD.contains(at)) {
            reload();
        } else if (HOME.contains(at)) {
            navigateTo("about:home", true);
        } else if (SCROLLBAR.contains(at)) {
            Rect thumb = thumbRect();
            if (thumb.contains(at)) {
                _draggingThumb = true;
                _dragStart = at.y;
                _dragScroll = _scroll;
            } else {
                scrollBy(at.y < thumb.y ? -(PAGE.h - 40) : PAGE.h - 40);
            }
        } else if (PAGE.contains(at)) {
            int link = linkAt(at);
            if (link >= 0) {
                Url next;
                if (next.resolve(_url, _doc.href(link)))
                    load(next, true);
            }
        }
    }

    void onMouseMove(Point at) {
        if (_draggingThumb) {
            int track = SCROLLBAR.h - thumbRect().h;
            int range = maxScroll();
            if (track > 0)
                setScroll(_dragScroll + (at.y - _dragStart) * range / track);
            return;
        }
        int link = PAGE.contains(at) ? linkAt(at) : -1;
        int button = BACK.contains(at) ? 0 : RELOAD.contains(at) ? 1 : HOME.contains(at) ? 2 : -1;
        if (link != _hoverLink or button != _hoverButton) {
            _hoverLink = link;
            _hoverButton = button;
            redraw();
        }
    }

    int linkAt(Point at) const {
        int x = at.x - PAGE.x - PAD_X;
        int y = at.y - PAGE.y - PAD_Y + _scroll;
        for (u64 i = 0; i < _doc.runs.size(); i++) {
            Run const& r = _doc.runs[i];
            if (r.link >= 0 and x >= r.x and x < r.x + r.w and y >= r.y - 2 and y < r.y + r.h + 3)
                return r.link;
        }
        return -1;
    }

    // MARK: Scrolling

    int maxScroll() const {
        int max = _doc.height + 2 * PAD_Y - PAGE.h;
        return max > 0 ? max : 0;
    }

    void setScroll(int value) {
        if (value > maxScroll())
            value = maxScroll();
        if (value < 0)
            value = 0;
        if (value != _scroll) {
            _scroll = value;
            redraw();
        }
    }

    void scrollBy(int delta) { setScroll(_scroll + delta); }

    Rect thumbRect() const {
        int total = _doc.height + 2 * PAD_Y;
        if (total <= PAGE.h)
            return {SCROLLBAR.x + 3, SCROLLBAR.y + 3, SCROLLBAR.w - 6, SCROLLBAR.h - 6};
        int h = SCROLLBAR.h * PAGE.h / total;
        if (h < 30)
            h = 30;
        int y = SCROLLBAR.y + (SCROLLBAR.h - h) * _scroll / maxScroll();
        return {SCROLLBAR.x + 3, y + 3, SCROLLBAR.w - 6, h - 6};
    }

    // MARK: Drawing

    void setStatus(char const* message) {
        copy(_status, message, sizeof(_status));
        redraw();
    }

    void redraw() {
        Canvas c(_window.surface());
        Font const& font = Font::system();

        // The page.
        c.clip = PAGE;
        c.fill(PAGE, Colors::PAPER);
        int originX = PAGE.x + PAD_X, originY = PAGE.y + PAD_Y - _scroll;
        for (u64 i = 0; i < _doc.runs.size(); i++) {
            Run const& r = _doc.runs[i];
            int y = originY + r.y;
            if (y > PAGE.bottom() or y + r.h < PAGE.y)
                continue;
            int x = originX + r.x;
            if (r.kind == RunKind::Rule) {
                c.fill({x, y, r.w, 1}, r.color);
                continue;
            }
            if (r.kind == RunKind::Bullet) {
                c.fillCircle(static_cast<float>(x + 3), static_cast<float>(y + 9), 2.6f, r.color);
                continue;
            }
            if (r.kind == RunKind::Image) {
                drawImage(c, r, x, y);
                continue;
            }
            Color color = r.link >= 0 and r.link == _hoverLink ? Colors::HEADING : r.color;
            Font const& font = fontFor(r.font);
            c.text({x, y}, _doc.text.data() + r.start, r.length, color, font);
            if (r.underline)
                c.fill({x, y + r.h - 1, r.w, r.link == _hoverLink ? 2 : 1}, color.withAlpha(150));
        }
        c.clip = _window.surface().bounds();

        // Scrollbar.
        c.fill(SCROLLBAR, Colors::PAPER);
        if (maxScroll() > 0)
            c.fillRounded(thumbRect(), 3, _draggingThumb ? Theme::SAFFRON : Color::hex(0xC9C4D2));

        // Toolbar.
        Rect toolbar{0, 0, windowW, TOOLBAR_H};
        c.fill(toolbar, Theme::SURFACE);
        c.fill({0, TOOLBAR_H - 1, windowW, 1}, Color::hex(0x000000, 70));
        iconButton(c, BACK, 0, _historyCount > 1);
        iconButton(c, RELOAD, 1, true);
        iconButton(c, HOME, 2, true);

        c.fillRounded(ADDRESS, 9, Theme::SURFACE_ALT);
        if (_editing)
            c.strokeRounded(ADDRESS, 9, 1.5f, Theme::SAFFRON.withAlpha(200));
        if (_url.isHttps() and not _editing) { // a small lock
            int lx = ADDRESS.x + 13, ly = ADDRESS.y + 9;
            c.strokeRounded({lx + 2, ly, 7, 9}, 3.5f, 1.5f, Theme::TEXT_DIM);
            c.fillRounded({lx, ly + 6, 11, 9}, 2, Theme::TEXT_DIM);
        }
        int addressX = ADDRESS.x + (_url.isHttps() and not _editing ? 32 : 14);
        char const* shown = _editing ? _typed : _address;
        int maxChars = (ADDRESS.w - 46) / font.width;
        u64 n = length(shown);
        char const* visible = n > static_cast<u64>(maxChars) ? shown + n - static_cast<u64>(maxChars) : shown;
        int textY = ADDRESS.y + (ADDRESS.h - font.height) / 2;
        if (_editing and _selected and visible[0])
            c.fillRounded({addressX - 2, textY - 2, font.measure(visible) + 4, font.height + 4}, 3, Theme::SAFFRON.withAlpha(90));
        int end = c.text({addressX, textY}, visible, _editing ? Theme::TEXT : Theme::TEXT_DIM);
        if (_editing and _caret)
            c.fill({end + 1, textY - 1, 2, font.height + 2}, Theme::SAFFRON);

        // Status bar.
        Rect status{0, windowH - STATUS_H, windowW, STATUS_H};
        c.fill(status, Theme::SURFACE);
        c.fill({0, status.y, windowW, 1}, Color::hex(0x000000, 70));
        int statusY = status.y + (STATUS_H - font.height) / 2;
        if (_hoverLink >= 0) {
            Url target;
            char text[300] = "(not a web link)";
            if (target.resolve(_url, _doc.href(_hoverLink)))
                target.format(text, sizeof(text));
            c.text({12, statusY}, text, Theme::GOLD);
        } else {
            c.text({12, statusY}, _status, Theme::TEXT_DIM);
        }

        _window.present();
    }

    void drawImage(Canvas& c, Run const& r, int x, int y) {
        PageImage const& pi = _doc.images[static_cast<u64>(r.image)];
        int i = findImage(_doc.at(pi.src));
        if (i >= 0 and _images[i].state == CachedImage::Ready) {
            CachedImage& slot = _images[i];
            Kapala::Image const* draw = &slot.image;
            if (slot.image.width != r.w or slot.image.height != r.h) {
                if (slot.shown.width != r.w or slot.shown.height != r.h)
                    slot.image.scaled(r.w, r.h, slot.shown);
                draw = &slot.shown;
            }
            if (draw->pixels)
                c.image({x, y}, draw->pixels, draw->width, draw->height);
            if (r.link >= 0 and r.link == _hoverLink)
                c.strokeRounded({x - 2, y - 2, r.w + 4, r.h + 4}, 3, 2, Colors::LINK);
            return;
        }
        // Not here (yet): a placeholder with the description.
        c.fillRounded({x, y, r.w, r.h}, 4, Color::hex(0xEDE7F2));
        c.strokeRounded({x, y, r.w, r.h}, 4, 1, Colors::RULE);
        char const* alt = _doc.at(pi.alt);
        if (alt[0] and r.w > 40 and r.h > 24) {
            Rect saved = c.clip;
            c.clip = c.clip.intersect({x + 4, y + 4, r.w - 8, r.h - 8});
            c.paragraph({x + 8, y + 6, r.w - 16, r.h - 12}, alt, Colors::DIM, 18);
            c.clip = saved;
        }
    }

    // Back (an arrow), reload (a circular arrow) and home (a little house).
    void iconButton(Canvas& c, Rect r, int which, bool enabled) {
        if (_hoverButton == which and enabled)
            c.fillRounded(r, 8, Theme::HOVER);
        Color ink = enabled ? Theme::TEXT : Theme::TEXT_DIM.withAlpha(90);
        if (which == 2) {
            int cx = r.x + r.w / 2, base = r.y + 23;
            for (int row = 0; row < 7; row++) // the roof, row by row
                c.fill({cx - row - 1, base - 13 + row, 2 * row + 2, 1}, ink);
            c.fill({cx - 6, base - 7, 12, 8}, ink);
            c.fill({cx - 2, base - 4, 4, 5}, _hoverButton == which ? Theme::HOVER : Theme::SURFACE);
            return;
        }
        Font const& font = Font::get("sans-bold", 18);
        char const* glyph = which == 0 ? "\u2190" : "\u21bb";
        c.text({r.x + (r.w - font.measure(glyph)) / 2, r.y + (r.h - font.height) / 2}, glyph, ink, font);
    }

    Window _window;
    Net _net;
    bool _netOk = false;

    Url _url;
    Url _history[MAX_HISTORY];
    int _historyCount = 0;
    char _address[Url::MAX_HOST + Url::MAX_PATH + 16] = {};

    Response _response;
    CachedImage* _images = new CachedImage[MAX_IMAGES];
    int _imageCount = 0;
    Vector<char> _source;
    bool _plain = false;
    Document _doc;
    int _scroll = 0;
    int _hoverLink = -1;

    bool _remember = false;
    bool _editing = false, _caret = false, _ctrl = false, _selected = false;
    int _hoverButton = -1;
    char _typed[600] = {};
    char _status[160] = "Ready";

    bool _draggingThumb = false;
    int _dragStart = 0, _dragScroll = 0;

public:
    static App* current;
};

App* App::current = nullptr;

} // namespace

int main() {
    auto* app = new App();
    App::current = app;
    if (not app->open())
        return 1;
    return app->run();
}
