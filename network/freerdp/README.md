# RemoteDesktop for AROS x86-64

A native Zune RDP client based on FreeRDP 3.32.2, with a separate MuPDF 1.28.5
helper for PDF/XPS printing through `printer.device`.

## Build

Place this Ports tree at `ports` and current Contrib at `contrib` in an ADT
source tree. Configure an x86-64 AROS build with its cross compiler, then run:

```
make ports-network-freerdp
```

The recipe fetches the two upstream release archives, applies the included
patches, builds against the configured target SDK and installs into
`Extras/Networking/RemoteDesktop` (or the configured Contrib destination).
Host requirements include Python 3, CMake, make, a C/C++ cross compiler, and
the usual ADT build prerequisites. `FREERDP_JOBS` controls compilation jobs
(default 4). Source archive hashes are recorded in `SHA256SUMS`.

OpenSSL comes from `contrib-openssl`; zlib and the native API link libraries
come from ADT. Host libraries are excluded from target dependency discovery.
Only x86-64 is enabled. AHI capture on 64-bit systems also needs the core
`devsupp.c` correction which advances source offsets by `sizeof(struct sample)`
instead of `sizeof(struct sample *)`.

## Use

Copy the application drawer to writable storage and double-click
`RemoteDesktop`. From a Shell, set `Stack 4194304` first. Enter a server,
username and password, select sharing options, then Connect. A blank account
can reach an xrdp server's own login screen if its configuration permits it.
No server address, account or certificate is supplied with this port.

Load profile opens a file requester, initially in `PROGDIR:Profiles`. Save
updates the current profile, asking for a destination the first time. Save As
chooses another file and makes it the current profile; replacing an existing
file asks for confirmation. Saved profiles receive a project icon whose
Default Tool points to this copy of RemoteDesktop. Double-clicking a profile
loads its settings. A profile with a saved password asks for the master
password and connects after unlocking; otherwise the login dialog opens.
Keep the application at that location or update the icon's Default Tool if
it is moved. Remember password additionally
stores an AES-256-GCM encrypted password; saving asks for a master password
and confirmation, and loading asks for the master password. The master
password is not stored. PBKDF2-HMAC-SHA256 derives the key with a random salt;
the server, account, domain and certificate pin are authenticated with the
record. A wrong master password or altered record is rejected. Cancel does
not write a profile. Unchecking Remember password and saving removes the
saved password and its previous backup. Keep the master password: there is
no recovery mechanism.

The previous profile is normally retained as `.bak` during replacement.
A SHA-256 certificate pin is optional; enter 64 hexadecimal digits. Verify
unknown certificates before accepting them. Changed pins reject connection.
Ctrl+Alt+Enter switches between windowed and fullscreen mode. Resize remote
desktop requests a matching resolution when the server supports it.

Tab and Shift+Tab move through the preferences controls in display order.
During a connection, Shift+right-click opens the Project menu; ordinary
right-clicks go to the remote desktop. Preferences (reconnect) and Disconnect
end the connection and return to the dialog. Quit closes the application.
The menu shortcuts are Right-Amiga+P, Right-Amiga+D and Right-Amiga+Q.
Re-enter the password or load a saved profile before reconnecting. Command-line
options outside the fields supported by the dialog are not retained there;
the dialog represents one shared directory and one printer.

The Input menu provides Send Keys presets for Ctrl+Alt+Delete, Ctrl+Shift+Esc,
Alt+Tab, Alt+Shift+Tab, Alt+Esc, Alt+F4, Ctrl+Esc, the Windows key, Win+R,
Win+E, Win+D, Win+L, Win+Tab, Print Screen and Alt+Print Screen. Custom
combination accepts modifier names followed by a key, such as Win+Shift+S.
It supports letters, digits, F1-F24, navigation keys and named punctuation.
Keys are released in reverse order. The remote connection remains active
while the custom dialog is open.

Sharing is opt-in: text clipboard, audio playback, microphone capture, a
selected local directory, and either text or PDF/XPS printer mode. The server
must support and permit the corresponding channels. Directory sharing rejects
parent-directory traversal and symlink components. Smartcards, arbitrary USB
and multiple monitors are not supported by this client.

## Printing

The text mode announces Generic / Text Only. Document mode announces an
ADT PDF/XPS printer and accepts PDF or XPS spool data. The server needs a
compatible printer bridge; an ordinary xrdp installation may not provide one.
The client does not modify the remote server or install a server printer.

Keep `adt-rdp-render` beside `RemoteDesktop`. It runs as a separate DOS process
and sends RGB rasters to unit 0 of the stock `printer.device`; Gutenprint can
be the configured driver. It does not call Gutenprint directly. Device and
printer preferences determine the physical destination and print options.

Document limits: 32 MiB input, 100 pages, 150 dpi, 16 million pixels per page,
100 million pixels per job and a 256 MiB tracked renderer allocation budget.
This is not a total process-memory or CPU-time limit. Password-protected
files and PostScript/PCL streams are rejected. One job can be active at a time.
A successful remote CLOSE means the job was queued, not that paper printed.
Logs and completion status are retained under `RAM:RemoteDesktopJobs`.

## Validation

The port's source has been tested in native AROS under QEMU: TLS/NLA with
controlled servers, saved-profile round trips, clipboard, dynamic resolution,
synthetic audio capture/playback, directory operations and PDF/XPS rendering
through `printer.device` to a virtual sink. A user-authenticated xrdp session
also verified TLS 1.3, desktop rendering, input and fullscreen resizing.
Physical microphone, speakers and printer qualification remains pending.

The recipe's fresh-build results are separate from those earlier runtime
tests. Host persistence tests can be run against the patched source with:

```
python3 tests/test-profiles.py /path/to/patched/freerdp-3.32.2
```

## Licenses and source

FreeRDP and its platform patch use Apache-2.0. The renderer and MuPDF platform
patch use AGPL-3.0-or-later. The two executables communicate via spool files;
MuPDF is not linked into RemoteDesktop. Upstream third-party licenses remain
in the released source trees and must accompany applicable redistribution.
When distributing renderer binaries, provide the corresponding MuPDF release,
platform patch, renderer sources and build recipe under the AGPL terms.
The Workbench icon is a standard AROS tool icon with a 4 MiB stack setting;
`client/AROS/make-icon.c` contains its generator.
