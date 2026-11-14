# Making apps for Bhairav OS

Apps here are **third-party apps**: not part of the system, but installed
from the App Store (or the Terminal) onto the disk, in `/apps/<id>/`. This
folder holds two examples, `calculator` and `clock`.

## An app

One folder, `apps/<id>/`, with:

| File | What it is |
|---|---|
| `main.cpp` (and any other `.cpp`) | The program, written against Kapalaframe (`userspace/kapalaframe/include/kapala/`) |
| `manifest.txt` | `id`, `title`, `version`, `author`, `description` and optionally `permissions`, one `key = value` per line |
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

What the files and network connections reach depends on the app's
permissions. Without any, an app sees only its own folder (it's "/" to the
app, `/appdata/<id>` on the disk, removed when the app is) and has no
network. Ask for more in the manifest:

```
permissions = network files
```

- `network`: the internet. `local-network`: only computers on the local
  network (10.x, 192.168.x, ...).
- `files`: the user's files. Never other apps' folders, and `/apps` and
  `/var` stay read-only.

Installing grants what the manifest asks for; the user can change it in the
App Store (Permissions), and an update never gains a permission by itself.
Refused requests return `Sys::ACCESS_DENIED`. Apps can't start programs or
use hardware, and may hold a quarter of the RAM.

## Building and installing

```sh
make packages                               # builds build/packages/<id>.bpk for every app here
make install-app PKG=build/packages/clock.bpk   # copies one onto the disk
```

Every package here is also in the system's built-in catalog, so after
`make`, `apt install clock` (or one click in the App Store) installs it.
A package file can also be installed directly: `apt install /clock.bpk`
or `apt install https://your-site/clock.bpk`.

## Publishing: an apt repository

A repository is a web folder with the `.bpk` files and a `Packages` index:
each app's manifest followed by `file = <its .bpk>`, blank lines between.

```sh
make repo          # build/repo/: every app here, and its Packages index
make serve-repo    # serves it on port 8080 (for testing)
```

In Bhairav OS (QEMU reaches the build machine at 10.0.2.2):

```
apt add-repo http://10.0.2.2:8080
apt update
apt install calculator
```

The index is signed (`Packages.sig`, by the package key), starts with a
`serial` (the time `make repo` ran), and lists each package's SHA-256. apt
refuses an unsigned or changed index, an older one than it last read, and a
package that isn't exactly what the index lists. Run `make repo` again
whenever anything changes; don't edit `Packages` by hand.

To publish for real, copy `build/repo/` to any web server and share its
address. To release a new version, raise `version` in the manifest and run
`make repo` again: `apt upgrade` installs it.

## The package format

A `.bpk` is `"BPK1"`, a 32-bit file count, then per file a 32-byte name,
a 32-bit size and the bytes (`tools/bpk.cpp` makes them; details in
`kapala/apps.h`).

Packages are signed: `make packages` signs each with `keys/package-signing.pem`
(made the first time, never committed; keep it), and the boot image's
`trust.bin` holds its public key. Bhairav OS refuses unsigned or altered
packages, and updates that go back to an older version. The installed app
keeps its signed package (`/apps/<id>/package.bpk`), which is checked again
every time it starts; `apt verify` checks all of them.

To rotate the key, copy the old key's public half to `keys/trusted/`
(`openssl pkey -in keys/package-signing.pem -pubout -out keys/trusted/old.pem`),
then make a new `keys/package-signing.pem`: both are trusted until you delete
`keys/trusted/old.pem`. Boot images are signed by a separate key,
`keys/boot-signing.pem` (see `docs/SECURITY.md` §11).

More permissions in the manifest: `permissions = folder` asks for one of the
user's folders (the user picks it; the app sees it as `/shared`), and
`hosts = api.example.com *.cdn.example.com` limits the app to those hosts:
it can look up only those names and reach only the addresses they return.

**Note:** permissions limit what an installed app can reach, but an app you
granted `files` and `network` could still send your files somewhere.
