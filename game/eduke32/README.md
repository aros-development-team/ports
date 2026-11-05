# EDuke32

EDuke32 is a game engine for Duke Nukem 3D and compatible content. This directory supplies the integration maintained in AROS Ports.

The [AROS MetaMake recipe](mmakefile.src) fetches the upstream source and defines its AROS build or packaging steps and destination under `$(AROS_CONTRIB)/Games/FPS/EDuke32`.

The recipe uses a pinned EDuke32 synthesis snapshot and builds `EDuke32` and `Mapster32`; it does not supply commercial game data.
