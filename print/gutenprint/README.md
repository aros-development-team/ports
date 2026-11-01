# Gutenprint for AROS

A printer driver for AROS built on Gutenprint 5.3.5. One
printer.device driver gives access to Gutenprint's printer models (3608 in
5.3.5), with its own settings window opened from Printer prefs, in the
style of TurboPrint.

It is licensed under the GNU General Public License, version 2 or later,
like Gutenprint itself (see COPYING in the fetched sources), and is free to
the AROS community. It is an AROS port: the Gutenprint sources are fetched
from their release and patched, and the AROS driver, renderer and
preferences editor here link them. The AROS core stays under its own
license and never includes or links this code; it talks to AROS only
through printer.device's public driver interface, and core programs only
run it.

## Layout

- `gutenprint-5.3.5-aros.diff` - the AROS changes to Gutenprint 5.3.5: data search paths separated by `;` (`:` ends an AROS volume name), and only `include` and `src/main` built.
- `src/driver/` - `Gutenprint`, the printer.device driver. It takes unscaled
  24-bit page dumps and passes them to GutenprintRender; the printer data
  comes back and leaves through printer.device's own port. The pages of one
  printer.device open/close are one Gutenprint job (one job start and end).
  At open it reports the profile's page through printer.device, the way
  TurboPrint did: the extended data (printable area in dots, dots per inch,
  columns) and the preferences (paper size, sheet, lines, printable size), so
  an application sees the printer its unit is bound to. It answers the V44
  preferences commands: PRD_EDITPREFS runs GutenprintPrefs on the unit and
  reads the page again when it closes (Printer prefs' "Driver settings..."
  button sends it), PRD_READPREFS fills the PrinterTxt/Unit/DeviceUnit/Gfx
  structures with the unit's prefs and the profile's page, PRD_USEPREFS and
  PRD_LOADPREFS read the page again. Reset, save and write are not commands
  here: the profile files belong to GutenprintPrefs. Installed as
  `Devs/Printers/Gutenprint` of the Extras.
- `src/server/` - `GutenprintRender`, the program that runs Gutenprint for the
  driver (public port `GUTENPRINT.RENDER`, started by the driver on first
  use). Settings: `ENV:Gutenprint/Settings`, a `Printer=<model>` line plus
  Gutenprint `Name=value` options. A profile with `Preview=yes` (the
  "Show each page before it prints" box in GutenprintPrefs) opens the
  print preview (`gppreview.c`) for every page before it is rendered:
  the sheet, the printable area and the page image as placed, with
  brightness, contrast, saturation and gamma sliders shown on the image
  at once and applied to the job (TurboPrint's TrueMatch preview); Print,
  Print all (the job's remaining pages unasked), Cancel (the application
  gets PDERR_CANCEL) and "Keep for this printer" (the sliders go into the
  profile in ENV: and ENVARC:). The application waits in printer.device
  meanwhile.
- `src/core/` - pages through Gutenprint, one job at a time (`gpjob`), the
  driver/renderer messages (`gpproto.h`), the profiles (`gpprofile`) and
  Gutenprint's colour adjustments on 8-bit RGB for the preview (`gpcolor`:
  the same curves as print-color.c, without the printer's fixed
  linearisation, so 1.0 everywhere shows the image as sent).
- `src/tools/gp-render.c` - prints a PPM (or `a.ppm,b.ppm,...` as one job) with a
  named model into a file and reports an FNV-1a hash of the printer data.
- `src/tools/gp-dumptest.c` - prints a PPM through printer.device
  (PRD_DUMPRPORT), as an application would.
- `src/tools/gp-query.c` - opens a printer.device unit and prints what an
  application sees of its page (extended data and preferences); `READ`,
  `USE` and `EDIT` after the unit number send the V44 preferences commands
  and print the view again.
## Installing

The port installs into `Extras:Utilities/Print/Gutenprint` (GutenprintRender,
GutenprintPrefs, `xml/` with the printer definitions, `ppd/Generic.ppd`, the
tools) and the driver as `Extras:Devs/Printers/Gutenprint`; the package's
`s/Package-Startup` assigns `GUTENPRINT:` to the drawer at boot. Select the
driver `Gutenprint` in Prefs/Printer (copy it next to the other drivers in
`DEVS:Printers` if Prefs/Printer only lists that drawer) and choose the
printer model and paper in GutenprintPrefs, opened from Prefs/Printer's
"Driver settings..." button or directly with `GutenprintPrefs UNIT=<n>`.

Text (CMD_WRITE, e.g. `Copy file PRT:`) is laid out on pages of the
printable area in DejaVuSansMono.font at 10 pt (topaz if that font is
missing): CR, LF, tab (8 columns) and form feed are honoured, long lines
wrap, and the pages print in the same job as any graphics. Printer text
commands (styles, margins, pitch) are ignored.

## Building

    make ports-print-gutenprint

fetches gutenprint-5.3.5.tar.xz, applies `gutenprint-5.3.5-aros.diff`, builds
libgutenprint static (without CUPS, GIMP, GTK, NLS, modules and tests) and
the AROS programs against it. At run time the library reads its data from
`STP_DATA_PATH` (paths separated by `;`), set by the programs to
`ENVARC:Gutenprint/xml;GUTENPRINT:xml` when it is not already set, so a
user's own definitions in ENVARC: come first.

`Tools/gp-render` renders a PPM with a named model into a file and prints the
output's size and FNV-1a checksum, which must match a Linux build of the same
sources with the same options.
