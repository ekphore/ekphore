Ekphore CLAP - Linux x86-64

Requirements
  A 64-bit x86 Linux CLAP host and glibc 2.35 or newer.
  The graphical editor uses X11 (XWayland works on Wayland desktops).
  Alpine/musl systems and ARM processors are outside this release's target.

Installation
  Extract this zip. Copy ekphore.clap into ~/.clap/ (create the directory if
  necessary), or into your host's custom CLAP search directory. Rescan in
  your DAW.

  The file includes its private libraries. No Nix installation is required.
  At CLAP initialization it unpacks a private runtime into a unique directory
  under XDG_CACHE_HOME (or ~/.cache). The directory is removed at shutdown.
  The cache must be writable and permit shared libraries to be mapped.

Use
  Route MIDI to the plug-in track in your DAW. Choose your instrument library
  in Ekphore's browser. Instruments and sample content are not included.
  NKS keyboard browsing/control requires a separate nksd installation;
  ordinary host MIDI routing does not require nksd.

Release contents
  ekphore.clap              Self-contained CLAP instrument

The minimum libc version is checked for every shipped ELF file. A smoke host
checks CLAP discovery, initialization, activation, processing and teardown with
the minimum libc and no libraries mapped from /nix/store. The runtime closure's
manifest and dependency notices are retained with the release build metadata.
