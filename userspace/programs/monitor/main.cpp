// System Monitor: time, uptime, tasks and memory, refreshed every second.

#include <kapala/sys.h>
#include <kapala/text.h>
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

namespace {

constexpr int WIDTH = 480;
constexpr int HEIGHT = 340;
constexpr int HISTORY = 60; // seconds of memory history

void addTwoDigits(Text<64>& t, unsigned long v) {
    if (v < 10)
        t.add("0");
    t.add(v);
}

void addClock(Text<64>& t, unsigned long seconds) {
    addTwoDigits(t, seconds / 3600);
    t.add(":");
    addTwoDigits(t, seconds / 60 % 60);
    t.add(":");
    addTwoDigits(t, seconds % 60);
}

class Monitor {
public:
    bool open() {
        if (not _window.open("System Monitor", WIDTH, HEIGHT))
            return false;
        sample();
        return true;
    }

    int run() {
        for (;;) {
            WindowEvent e;
            if (not _window.waitEvent(e, 1000 - Sys::timeMs() % 1000)) {
                sample();
                continue;
            }
            if (e.kind == WindowEvent::Close)
                return 0;
        }
    }

private:
    void sample() {
        Sys::systemInfo(_info);
        unsigned long used = _info.totalMemory - _info.freeMemory;
        for (int i = 0; i + 1 < HISTORY; i++)
            _history[i] = _history[i + 1];
        _history[HISTORY - 1] = used;
        if (_samples < HISTORY)
            _samples++;
        draw();
    }

    void draw() {
        Canvas c(_window.surface());
        c.fill(_window.surface().bounds(), Theme::SURFACE);
        constexpr unsigned long MIB = 1024 * 1024;

        Text<64> clock;
        addClock(clock, _info.timeOfDay);
        clock.add(" UTC");
        c.text({24, 20}, clock.cstr(), Theme::SAFFRON, 2);

        Text<64> uptime;
        uptime.add("up ");
        addClock(uptime, _info.uptimeMs / 1000);
        uptime.add("    ").add(_info.taskCount).add(" tasks running");
        c.text({24, 62}, uptime.cstr(), Theme::TEXT_DIM);

        // Memory bar.
        unsigned long used = _info.totalMemory - _info.freeMemory;
        float fraction = _info.totalMemory ? float(used) / float(_info.totalMemory) : 0;
        Rect bar{24, 100, WIDTH - 48, 22};
        c.fillRounded(bar, 11, Theme::SURFACE_ALT);
        int filled = static_cast<int>(bar.w * fraction);
        if (filled < 22)
            filled = 22;
        c.fillRoundedGradient({bar.x, bar.y, filled, bar.h}, 11, Theme::SAFFRON, Theme::LOTUS);

        Text<64> memory;
        memory.add("memory: ").add(used / MIB).add(" MiB used, ").add(_info.freeMemory / MIB)
            .add(" MiB free of ").add(_info.totalMemory / MIB).add(" MiB");
        c.text({24, 132}, memory.cstr(), Theme::TEXT);

        // History graph: one bar per second. The top of the graph is 25%
        // above the busiest moment, so steady use doesn't look like "full".
        Rect graph{24, 168, WIDTH - 48, HEIGHT - 168 - 40};
        c.fillRounded(graph, 10, Theme::SURFACE_ALT);
        unsigned long peak = 1;
        for (int i = HISTORY - _samples; i < HISTORY; i++)
            peak = _history[i] > peak ? _history[i] : peak;
        unsigned long top = peak + peak / 4;
        float slot = float(graph.w - 16) / HISTORY;
        for (int i = HISTORY - _samples; i < HISTORY; i++) {
            int h = static_cast<int>((graph.h - 16) * (float(_history[i]) / float(top)));
            if (h < 2)
                h = 2;
            int x = graph.x + 8 + static_cast<int>(i * slot);
            Color color = Theme::LOTUS.mix(Theme::SAFFRON, float(i) / HISTORY);
            c.fill({x, graph.bottom() - 8 - h, static_cast<int>(slot) - 1, h}, color);
        }
        Text<64> legend;
        legend.add("memory used, last 60 seconds (top of graph: ").add(top / MIB).add(" MiB)");
        c.text({24, HEIGHT - 30}, legend.cstr(), Theme::TEXT_DIM);

        _window.present();
    }

    Window _window;
    Sys::SystemInfo _info{};
    unsigned long _history[HISTORY] = {};
    int _samples = 0;
};

Monitor monitor;

} // namespace

int main() {
    if (not monitor.open())
        return 1;
    return monitor.run();
}
