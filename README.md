# Harmony Browser

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

The app currently uses a deliberately narrow native boundary:

1. KiraUI/KiraGraphics create the top-level Sokol window.
2. `NativeLibs/harmony_webkit.cpp` dynamically loads `WebKit2.dll`.
3. WebKit creates a native `WKView` child HWND inside the Kira window.
4. The Kira frame callback resizes the child and services WebKit's hidden
   `RunLoopMessageWindow`.

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
kira run .
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

This first slice is a WebKit host showcase: it creates a Google home page in a
WebKit view covering the Kira window. The host presents the Windows desktop
Safari/WebKit user-agent shape and the window's rounded DPI backing scale so
desktop sites get their normal layout without claiming to be Chromium.

Google may still show an unusual-traffic or human-verification page for an
embedded WebKit session. That is Google's network and browser-integrity check,
not a KiraUI rendering failure; it must be completed by a human when shown.
Browser chrome, navigation state, downloads, permissions, and packaging of the
WebKit process binaries are the next layer.
