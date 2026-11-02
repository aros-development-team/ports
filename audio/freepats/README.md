# Freepats

Freepats is a collection of instrument samples used with TiMidity. This directory supplies the integration maintained in AROS Ports.

The [AROS MetaMake recipe](mmakefile.src) fetches the samples and packages them under `$(AROS_CONTRIB)/MultiMedia/Audio/Timidity`.

This is an instrument-data package, not a standalone audio application; the recipe also installs `timidity.cfg`.
