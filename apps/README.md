# Making apps for Bhairav OS

Apps here are **third-party apps**: not part of the system, but installed
from the App Store (or the Terminal) onto the disk, in `/apps/<id>/`. This
folder holds two examples, `calculator` and `clock`.

## An app

One folder, `apps/<id>/`, with:

| File | What it is |
|---|---|
| `main.cpp` (and any other `.cpp`) | The program, written against Kapalaframe (`userspace/kapalaframe/include/kapala/`) |
| `manifest.txt` | `id`, `title`, `version`, `author` and `description`, one `key = value` per line |
| `icon.png` | Optional: a square icon (128 px works well); without it the app shows its first letter |

The `id` is the folder's name: lowercase letters, digits and `-`. It can't
be the name of a built-in program.

A minimal app:

```cpp
#include <kapala/window.h>

using namespace Kapala;
using namespace Kapala::Gfx;

int main() {
    Window window;
    if (not window.open("Hello", 320, 200))
        return 1;
    Canvas c(window.surface());
    c.fill(window.surface().bounds(), Theme::SURFACE);
    c.text({20, 20}, "Namaste!", Theme::TEXT, Font::get("sans-bold", 22));
    window.present();
    for (;;) {
        WindowEvent e;
        if (window.waitEvent(e) and e.kind == WindowEvent::Close)
            return 0;
    }
}
```

Like every app, it starts with three handles: the shell (1), the files
(2, `Kapala::Files`) and the network (3, `Kapala::Net`, `Kapala::Tls`,
`httpGet`). It can draw, read and write files, use the network, open
other apps (`window.launch`), and so on: see the headers and the built-in
apps in `userspace/programs/`.

## Building and installing

```sh
make packages                               # builds build/packages/<id>.bpk for every app here
make install-app PKG=build/packages/clock.bpk   # copies one onto the disk
```

Every package here is also in the catalog the App Store shows (it's in
the boot image), so after `make`, they're one click away. Packages can
also be installed:

- from the disk: in the App Store's field, or `install /clock.bpk` in the Terminal;
- from the web: `https://your-site/app.bpk` in the same places.

To publish an app, put its `.bpk` file on any web server.

## The package format

A `.bpk` is `"BPK1"`, a 32-bit file count, then per file a 32-byte name,
a 32-bit size and the bytes (`tools/bpk.cpp` makes them; details in
`kapala/apps.h`).

**Note:** apps get the same handles as built-in apps, including the whole
disk and the network. Install only apps you trust.
