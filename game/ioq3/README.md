# ioquake3

ioquake3 is an open-source Quake III Arena game engine. This directory supplies the integration maintained in AROS Ports.

The [AROS MetaMake recipe](mmakefile.src) fetches the upstream source and defines its AROS build or packaging steps and destination under `$(AROS_CONTRIB)/Games/FPS/ioquake3`.

The AROS recipe uses SDL2 and a supplied target CMake configuration; it does not include retail game assets.
