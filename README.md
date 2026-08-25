# Harmony

A monorepo holding four packages. Three of them are programs that run in
separate processes, and one is the library that lets them understand each other.

```
shell/    the window. Owns every pixel, and starts the components it finds.
browser/  the web engine, its tabs, and everything that reaches WebKit.
ai/       the model runtime and the harness around it.
core/     what the three share: who they are, how they find each other, and
          every message they exchange.
```

`core` is built on [KiraIpc](../kira-ipc), whose typed messages survive the two
sides being built from different releases -- which is the property this whole
arrangement rests on.

## What you get when you install it

A component is installed by putting its executable **beside the shell**, and
uninstalled by removing it. There is no registry key and no manifest, so the
question "is the AI runtime installed" has one true answer rather than a cached
one.

| You install | You get | What runs |
| --- | --- | --- |
| Harmony Browser | `harmony-shell` + `harmony-browser` | a browser; every AI feature off |
| The AI runtime | `harmony-shell` + `harmony-ai` | a conversation, and no web engine |
| Both | one shell, both components | a browser with AI in it |

Nothing is told which arrangement it is in. The shell starts what it finds, and
what it does not find is simply absent -- which it can say, rather than failing
to load something.

The split is a **desktop** shape. iOS forbids one program starting another, so
`harmonySupportsComponents()` answers false there and the shell says so instead
of waiting for a component that can never arrive.

## How a component draws

A component has no window. It renders each of its regions into a shared GPU
texture and tells the shell where each one goes:

```kira
TextureTarget(handle = pageHandle, zLevel = 0) { PageContent() }
TextureTarget(handle = sidebarHandle, zLevel = 0) { Sidebar() }
OverlayTarget(handle = overlayHandle)
```

Each region is its own texture, so a sidebar does not repaint when the content
beside it scrolls. Everything a component FLOATS goes into one overlay target the
size of the whole canvas, because a menu opened from the sidebar runs across the
content beside it.

The shell composites in four bands, and the rule lives in one place
(`core/app/Protocol/Surface.kira`):

```
0  every component's content, at its rectangle
1  the shell's own content
2  component overlays, most recently raised last
3  the shell's own overlay, over everything
```

The shell is top no matter what. A component able to cover the shell's menus
would be a component able to take the window away from the person using it.

Handles do not travel inside messages. A handle naming GPU memory means nothing
outside the process that owns it, so the CHANNEL transfers it -- `SCM_RIGHTS` on
POSIX, `DuplicateHandle` into the peer on Windows -- and the message carries only
an id saying which transferred handle it means. See
[KiraIpc](../kira-ipc#handles).

## Running it

Each package builds on its own:

```bash
kira build browser
kira build ai
```

Components find each other by executable name, so a run needs them staged in one
directory as `harmony-shell`, `harmony-browser` and `harmony-ai`.

To watch the arrangement without a window in the way:

```bash
kira run core/Examples/component-handshake
```

To look at the shell, which is the only honest way to review a window:

```bash
kira build --backend llvm shell
HARMONY_SHELL_CAPTURE=/tmp/shell.ppm KIRA_GRAPHICS_BACKEND=dawn ./harmony-shell
```

A browser started with an endpoint argument runs as a component with no window
of its own; started with none it opens its own window, which is the development
path and the same program either way.

---

## The browser

Harmony Browser is the first KiraUI application intended to host WebKit on
Windows.

## Windows WebKit decision

The upstream WebKit project has a Windows port, but it is not distributed as a
general-purpose Windows SDK or runtime. The official project README says that
Windows users must build WebKit themselves. The Windows port is 64-bit and
uses Cairo for graphics and libcurl for networking.

That means Harmony needs a pinned WebKit source checkout and a local/source
build for its first Windows implementation. WebKitGTK and WPE are not a
Windows shortcut: they are Linux ports.

The native boundary is one archive, `harmony_browser`, holding nine groups of
translation units that call each other in both directions. It works like this:

1. KiraUI/KiraGraphics create the top-level Sokol window.
2. The tab registry dynamically loads `WebKit2.dll` on a thread of its own and
   owns everything WebKit: the process context, one `WKView` child HWND per tab,
   and the run loop. Nothing else loads the engine or cycles that loop.
3. WebKit keeps ONE client of each kind per page, so the registry installs the
   one `WKPageUIClient`, `WKPageNavigationClient` and `WKPageStateClient` a page
   carries, and every other system fills its own fields in through the registry's
   hooks in `NativeLibs/harmony_tabs_embed.h`.
4. `NativeLibs/harmony_webkit.cpp` is the host: it registers every system with
   the registry before the first page exists, keeps the showing tab's view sized
   for the chrome above it at the host's DPI backing scale, and answers with the
   first system that has something to say about why there is no page.
5. `app/WebKitBridge.kira` is the frame: one ordered pass per frame, and the
   shutdown order the systems require.

Loading WebKit dynamically keeps the large WebKit build out of the Kira package
build. The WebKit runtime and its WebContent/Network process binaries still
need to be deployed together with the app.

## Build prerequisites

Follow the upstream Windows port prerequisites: Visual Studio with Desktop
development with C++, Developer Mode, Git, CMake, Ninja, LLVM/clang, gperf,
Perl, Ruby, and Python 3.11 with `pywin32`. The upstream documentation
currently calls out Python 3.11 rather than 3.12.

The scripts select `clang-cl` by version, not by `PATH` order, and require
clang 20 or newer: Visual Studio 18's MSVC STL rejects anything older with
`error STL1000`. Other toolchains ship their own older `clang-cl` and can come
first on `PATH`. Override the choice with `-ClangCl`, or point `LLVM_ROOT` at
the installation to prefer.

Check the machine and checkout:

```powershell
.\scripts\check-webkit-windows.ps1 -WebKitRoot .\third_party\WebKit
```

Build WebKit using the upstream scripts:

```powershell
git clone https://github.com/WebKit/WebKit.git third_party\WebKit
.\scripts\build-webkit-windows.ps1 -WebKitRoot .\third_party\WebKit
$env:HARMONY_WEBKIT_ROOT = (Resolve-Path .\third_party\WebKit\WebKitBuild\Release\bin)
kira run browser
```

The build runs at `BelowNormal` priority and uses half the logical processors,
so it stays behind interactive work. Override with `-Priority` and `-Jobs`.
Lower `-Jobs` when free memory is short: each `clang-cl` holds roughly a
gigabyte, and WebKit's unified sources make individual translation units large.

Re-running the script resumes an interrupted build; Ninja rebuilds only what
changed.

Unified builds are off. WebKit bundles dozens of translation units into one
compilation unit by default, which makes a single edited file rebuild its whole
bundle. Harmony builds each file separately: the cold build is slower, the
incremental loop is far shorter. Pass `-UnifiedBuilds` to restore upstream's
default. Switching the flag either way reconfigures CMake and rebuilds
everything.

## Current scope

Harmony Browser is a browser. Eight systems are wired into one window:

- **Tabs.** Open, close, reopen closed, reorder, pin, and select, with the list
  drawn as a column of rows or as a strip across the top. Pages that call
  `window.open` or follow `target=_blank` create tabs from inside WebKit, so the
  native registry is the source of truth and the window draws what it publishes.
  Background tabs are suspended and show their last frame while they wake.
- **Chrome.** Toolbar with back, forward, reload/stop, home, an address bar with
  a transport indicator, a determinate load lane, and the window title.
- **Navigation.** Per-tab address, title, progress, load state, back/forward
  availability, failure text, and the full back/forward list, published from
  WebKit's own callbacks and read without entering WebKit.
- **Downloads.** Attachments, files the engine cannot render, and "save link as",
  with a panel carrying progress, cancel, reveal, open, and a history that
  survives a restart.
- **Dialogs.** `alert`, `confirm`, `prompt`, before-unload, HTTP and proxy
  sign-in, the certificate interstitial, and the native file picker.
- **Permissions.** Geolocation, notifications, camera, microphone and screen
  capture, prompted once per origin and remembered, with a panel to review and
  revoke what was granted.
- **Data store.** A profile under `%LOCALAPPDATA%\HarmonyBrowser`, private tabs
  in an ephemeral store, per-origin and time-scoped clearing, a site-data list,
  and session restore across a restart.
- **Input.** Shortcuts matched in a message hook on both pumps, so they work
  after a page has taken the keyboard: new/close/reopen/cycle tab, focus the
  address bar, reload, back, forward, stop, find in page, page zoom remembered
  per site, and print.

The host presents the Windows desktop Safari/WebKit user-agent shape and the
window's rounded DPI backing scale so desktop sites get their normal layout
without claiming to be Chromium.

Google may still show an unusual-traffic or human-verification page for an
embedded WebKit session. That is Google's network and browser-integrity check,
not a KiraUI rendering failure; it must be completed by a human when shown.
Packaging the WebKit process binaries with the app is the next layer.

## The Apple port

On macOS the engine is not an archive at all. `app/WebEngine/` drives AppKit and
WebKit DIRECTLY through the Objective-C runtime -- one typed `objc_msgSend`
alias per call shape, the way Kira Graphics' Metal backend drives Metal -- so
there is no shim and no second registry for the view's pointers to fall out of;
they hang off the shared application as associated objects. The manifest row
(`harmony_webkit`) carries only Apple frameworks plus libobjc, and is optional
for the same reason `kira_metal` is: a target without a row excludes the
binding rather than failing a link.

The frame keeps its own order: attach on the first window there is, drain what
WebKit did on its own schedule (`runMode:beforeDate:` with distant past --
which is what makes loads commit inside a loop that never runs the application
run loop), size the page once in points, apply the command a person asked for,
read back URL/title/loading/history flags as plain property reads, settle the
window title. One tab, whose id is 1, answering every seam the chrome already
reads through `NavigationBridge` and `TabsBridge`; multi-tab, dialogs,
permissions, downloads and the data store still belong to the Windows archive,
and say so where they are asked.

Two things were learned by driving a live WKWebView through these shapes:
the run-loop drain above is sufficient for real page loads (URL, title and
loading state all arrive), and `-valueForKeyPath:` against a live view
segfaults. The progress estimate therefore is not read at all -- it is
OBSERVED. A class built at runtime (`HarmonyWebEngineWatcher`) carries
`-observeValueForKeyPath:ofObject:change:context:` as a Kira IMP -- the same
installation `MetalForeign.kira` uses for `-windowDidResize:` -- and WebKit
pushes each change to it with the new value boxed in an NSNumber, which crosses
as text and parses on arrival. The lane is determinate; the snapshot keeps the
last number delivered.
