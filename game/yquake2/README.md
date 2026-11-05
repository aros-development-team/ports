# Yamagi Quake II

Yamagi Quake II is an open-source Quake II game engine. This directory supplies the integration maintained in AROS Ports.

The [AROS MetaMake recipe](mmakefile.src) fetches the upstream source and defines its AROS build or packaging steps and destination under `$(AROS_CONTRIB)/Games/FPS/Quake2`.

The recipe pins the exact upstream commit used by its AROS patch; game data is not included.
