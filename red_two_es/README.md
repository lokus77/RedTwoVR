# red_two_es

The game in Spanish: the texts, menus and radio voices of the Spanish release of Rogue Leader
(GSWS64), used with the USA game Red Two is made from. This folder holds them once you extract
them from **your own** Spanish disc; they are game data and are never committed (see `.gitignore`).

```
red_two.exe --extract-spanish <your GSWS64 disc image> <this folder>
```

It writes `text_Spanish.bin`, `staticGame_sp.bin`, `frontendSP.bin`, `voiceDir.bin`,
`r2voice.vdy` and `resDir_rows.bin` (about 140 MB). Put the folder next to the USA disc (or set
`spanish_disc_path` in `user\settings.ini`; a relative path is read from the `user` folder) and
choose **Game language: Español** in the launcher.

---

El juego en castellano: textos, menús y voces de radio de la edición española (GSWS64), con el
juego USA. Esta carpeta los guarda cuando los extraes de **tu propio** disco español; son datos del
juego y nunca se suben al repositorio. Extráelos con el comando de arriba, deja la carpeta junto al
disco USA y elige **Idioma del juego: Español** en el launcher.
