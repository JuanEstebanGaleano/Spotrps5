# Plan: Spotify Connect en la PS5

Objetivo: ejecutar librespot (Spotify Connect, Rust) con el loader de Linux x86-64 de este repositorio, de modo que la
PS5 aparezca como dispositivo en la app de Spotify y reproduzca por su salida de audio.

## Hitos

| # | Hito | Cómo se comprueba |
|---|------|-------------------|
| 0 | El repo compila en GitHub Actions y produce un `.ffpfsc` (todavía con el loader de PokeMMO) | el artefacto `PPSA98001-ffpfsc` del workflow `build` |
| 1 | Título propio (otro Title ID, nombre "Spotrps5"), sin ROMs, Google Drive ni descarga del cliente de PokeMMO | arranca y muestra la pantalla de carga |
| 2 | librespot compilado para `x86_64-unknown-linux-gnu` y analizado: qué llamadas de libc faltan en el loader | informe de `tools/analyze_binary.py` |
| 3 | Shims que faltan (epoll, futex, getrandom, TLS global-dynamic, multicast UDP para mDNS) | librespot arranca en el loader de PC (`make host-run`) |
| 4 | Backend de audio de librespot hacia `AudioOut` de la PS5 | suena un tono de prueba |
| 5 | Conexión: la PS5 aparece en Spotify y reproduce | prueba en consola |
| 6 | Pantalla de TV: nombre del dispositivo, canción, mando | prueba en consola |

## Decisiones

- TLS con `rustls` (sin OpenSSL ni almacén de certificados del sistema).
- Descubrimiento por mDNS (el móvil envía las credenciales; la consola no necesita teclado).
- Todo se compila en GitHub Actions con la imagen de `buildenv/` (herramientas fijadas con checksum).

## Riesgos

- Rust usa TLS (almacenamiento por hilo) con `%fs`, que en la PS5 pertenece a su libc: compilar con modelo `global-dynamic`.
- Multicast UDP en la capa de red del loader sin verificar.
- Firmware 13.60 no probado con el loader base más allá de que arranca.
