# Cambios locales de vitarecomp

Origen: https://github.com/sp00nznet/vitarecomp.git
Commit base: 6ed4a5c59045706292c8ec03dfe5207ecc4033a4 TRANSLATION: what the running program asks for

Para reconstruir: clonar el origen, `git checkout` de ese commit, `git apply cambios_locales.patch`
y copiar `nuevos\` encima. En la máquina de trabajo NO se hace checkout/restore/reset de
vitarecomp (ver AGENTS.md, regla número uno): esto es solo una copia de seguridad.
