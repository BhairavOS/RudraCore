// Clock: an example third-party app for Bhairav OS.
//
// An analog clock face with the time and date under it (UTC, the time the
// hardware clock keeps). Redraws every second.

#include <kapala/gfx.h>
#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

namespace {

constexpr int WIDTH = 320, HEIGHT = 380;
constexpr float PI = 3.14159265f;

// sin and cos without a math library: reduce to [-pi, pi], then a series.
float sine(float x) {
    while (x > PI)
        x -= 2 * PI;
    while (x < -PI)
        x += 2 * PI;
    float term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 10; n++) {
        term *= -x2 / static_cast<float>((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

float cosine(float x) { return sine(x + PI / 2); }

// A hand: round dots along a line, so its edges are smooth.
void hand(Canvas& c, float cx, float cy, float angle, float length, float width, Color color) {
    float dx = sine(angle), dy = -cosine(angle);
    for (float t = -length * 0.12f; t <= length; t += 0.6f)
        c.fillCircle(cx + dx * t, cy + dy * t, width / 2, color);
}

void two(Text<32>& t, unsigned long v) {
    if (v < 10)
        t.add("0");
    t.add(v);
}

class Clock {
public:
    bool open() {
        if (not _window.open("Clock", WIDTH, HEIGHT))
            return false;
        redraw();
        return true;
    }

    int run() {
        for (;;) {
            WindowEvent e;
            unsigned long untilNextSecond = 1000 - Sys::timeMs() % 1000;
            if (not _window.waitEvent(e, untilNextSecond)) {
                redraw();
                continue;
            }
            if (e.kind == WindowEvent::Close)
                return 0;
        }
    }

private:
    void redraw() {
        Sys::SystemInfo info{};
        Sys::systemInfo(info);
        unsigned long t = info.unixTime;
        unsigned long h = t / 3600 % 24, m = t / 60 % 60, s = t % 60;

        Canvas c(_window.surface());
        c.fill(_window.surface().bounds(), Color::hex(0x17151C));
        float cx = WIDTH / 2.0f, cy = 150, r = 118;

        // The face: a soft disc, minute ticks, hour marks.
        c.fillCircle(cx, cy, r + 6, Color::hex(0x2A2733));
        c.fillCircle(cx, cy, r, Color::hex(0xF7F3EC));
        for (int i = 0; i < 60; i++) {
            float a = i * 2 * PI / 60;
            bool hour = i % 5 == 0;
            float outer = r - 8, inner = r - (hour ? 22 : 13);
            for (float d = inner; d <= outer; d += 0.6f)
                c.fillCircle(cx + sine(a) * d, cy - cosine(a) * d, hour ? 2.0f : 0.8f,
                             hour ? Color::hex(0x2A2733) : Color::hex(0x9C99A8));
        }
        Font const& numbers = Font::get("sans-bold", 15);
        for (int i = 1; i <= 12; i++) {
            float a = i * 2 * PI / 12;
            Text<4> label;
            label.add(i);
            int w = numbers.measure(label.cstr());
            c.text({static_cast<int>(cx + sine(a) * (r - 38)) - w / 2,
                    static_cast<int>(cy - cosine(a) * (r - 38)) - numbers.height / 2},
                   label.cstr(), Color::hex(0x2A2733), numbers);
        }

        // Hands: hours, minutes, then a saffron seconds hand.
        hand(c, cx, cy, (static_cast<float>(h % 12) + m / 60.0f) * 2 * PI / 12, r * 0.5f, 7, Color::hex(0x2A2733));
        hand(c, cx, cy, (static_cast<float>(m) + s / 60.0f) * 2 * PI / 60, r * 0.75f, 4.5f, Color::hex(0x2A2733));
        hand(c, cx, cy, static_cast<float>(s) * 2 * PI / 60, r * 0.82f, 1.8f, Theme::SAFFRON);
        c.fillCircle(cx, cy, 5, Theme::SAFFRON);

        // The time in digits, and the date.
        Text<32> digits;
        two(digits, h);
        digits.add(":");
        two(digits, m);
        digits.add(":");
        two(digits, s);
        Font const& big = Font::get("sans-bold", 28);
        c.text({(WIDTH - big.measure(digits.cstr())) / 2, 290}, digits.cstr(), Theme::WHITE, big);

        long days = static_cast<long>(t / 86400);
        long z = days + 719468, era = z / 146097, doe = z - era * 146097;
        long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        long day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
        long year = yoe + era * 400 + (month <= 2);
        static constexpr char const* DAYS[] = {"Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday",
                                               "Wednesday"};
        static constexpr char const* MONTHS[] = {"January", "February", "March",     "April",   "May",      "June",
                                                 "July",    "August",   "September", "October", "November", "December"};
        Text<64> date;
        date.add(DAYS[days % 7]).add(", ").add(day).add(" ").add(MONTHS[month - 1]).add(" ").add(year).add(" (UTC)");
        Font const& font = Font::system();
        c.text({(WIDTH - font.measure(date.cstr())) / 2, 334}, date.cstr(), Theme::TEXT_DIM, font);
        _window.present();
    }

    Window _window;
};

Clock clock;

} // namespace

int main() {
    if (not clock.open())
        return 1;
    return clock.run();
}
