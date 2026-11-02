# Arx Libertatis

Arx Libertatis is an open-source implementation of the Arx Fatalis game engine. This directory supplies the integration maintained in AROS Ports.

The [AROS MetaMake recipe](mmakefile.src) fetches the upstream source and defines its AROS build or packaging steps and destination under `$(AROS_CONTRIB)/Games/Adventure/ArxLibertatis`.

The recipe uses CMake and declares SDL, OpenAL, GLEW, FreeType, and Boost dependencies.
