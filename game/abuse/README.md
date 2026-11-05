# Abuse

Abuse is a side-scrolling action platform game. This directory supplies the integration maintained in AROS Ports.

The [AROS MetaMake recipe](mmakefile.src) fetches the upstream source and defines its AROS build or packaging steps and destination under `$(AROS_CONTRIB)/Games/Platform/Abuse`.

Known limitation in the recipe: collision detection is reported as broken, allowing the player to fall through the floor.

Additional package notes: [LEGAL](LEGAL).
