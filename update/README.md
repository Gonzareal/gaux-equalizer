# Aviso de versión nueva (beta → versión final)

Desde la beta 0.26, Gaux EQ revisa este archivo al abrir su ventana, como mucho una vez al día:

`update/latest.json` (rama `main`) → https://raw.githubusercontent.com/Gonzareal/gaux-equalizer/main/update/latest.json

Si la `version` publicada aquí es **más nueva** que la instalada, el plugin muestra un aviso discreto con **DESCARGAR**, **MÁS TARDE** y **omitir**:
- No envía ningún dato del usuario (es una lectura normal de un archivo público).
- Nunca instala nada solo: el usuario baja el instalador nuevo y lo instala encima. Sus presets y sesiones se conservan.
- Sin internet, o con el archivo dañado, no pasa nada.

## Cómo anunciar la versión final

1. Subir la versión en `CMakeLists.txt` (por ejemplo `VERSION 1.0.0`) y dejar que GitHub Actions cree y pruebe los instaladores.
2. Publicar los instaladores donde se descargan (la web).
3. Editar `update/latest.json`:

```json
{
  "version": "1.0.0",
  "channel": "final",
  "title": "Gaux Equalizer 1.0: la versión final",
  "notes": "Gracias por probar la beta.",
  "url_mac": "https://aleksandergaux.com/...",
  "url_win": "https://aleksandergaux.com/...",
  "url": "https://aleksandergaux.com"
}
```

Desde ese momento, cada persona con la beta ve el aviso la próxima vez que abra el plugin. Los links deben empezar con `https://`. Editar este archivo no vuelve a compilar el plugin.
