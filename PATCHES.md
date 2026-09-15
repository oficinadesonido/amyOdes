# PATCHES — amyOdes vs upstream

**amyOdes** es el fork de AMY que usan todos los proyectos de Oficina de Sonido.
Fuente única; pinnear por versión/tag en cada proyecto. Base: AMY
(github.com/shorepine/amy) v1.1.4.

Regla: mantener los cambios respecto a upstream **mínimos y documentados aquí**.
Lo específico de cada proyecto vive en el sketch, no dentro de amyOdes.

---

## Latencia de reproducción de samples (mod previa)
- `src/i2s.c`: buffers DMA `6×240` → `2×256` frames; I2S `32-bit` → `16-bit`.
  Latencia de reproducción ~38 ms → ~17 ms.

## Selección de banco PCM (mod previa)
- `src/amy_pcm_select.h`: `AMY_PCM_SET` (0 = `pcm_small` / 808, 1 = `custom1`).
- `src/amy.c`: en el build Arduino usa `pcm_small.h` en lugar de `pcm_tiny.h`
  (sin esto, la conga sonaba a ruido).

## Sin retardos de nota (mod previa)
- Eliminado `synth_delay_ms` / `noteon_delay_ms` en `api.c`, `instrument.c`,
  `parse.c`, `amy.h`.

## Sample rate por-preset (odes — 2026-05-25)
- `src/pcm.c`: `render_pcm` usa `preset.log2sr` (rate por-preset) en vez de la
  constante global `PCM_AMY_LOG2_SAMPLE_RATE`. `log2sr` se puebla en
  `get_preset_for_preset_number` (horneado = la constante; memoria =
  `preset_p->log2sr`, que `pcm_load` ya calculaba).
- **Por qué:** permite que samples cargados en runtime a 44.1 kHz (kits de
  usuario vía `pcm_load`) convivan con los bancos horneados a 22050 Hz
  (marimba, O-HAT…) sin regenerar nada.
- Candidato a proponer upstream (completa un campo que AMY ya tenía).

## Declick al redisparar un PCM (odes — 2026-09-15)
- `src/pcm.c`: `pcm_note_on` volvía la fase a 0 aunque la cola del golpe anterior
  siguiera sonando → la onda saltaba y sonaba un clic (bombos a 16avos desde ~120 BPM).
  Ahora la cola pasa a un fundido lineal de 256 muestras (~5,8 ms) que se suma
  encima del golpe nuevo (`pcm_declick_capture` / `pcm_declick_render`). Solo osc 0–31.
- `src/amy.c`: el note-on ya no hace `reset_filter` en un PCM que estaba sonando
  (poner el estado a cero cortaba la salida del filtro: clic aunque el sample se funda).
  Desde silencio se sigue reseteando como antes.
- `src/pcm.c` + `amy.h`: `pcm_preset_in_use(n)` — 1 si alguna voz sigue leyendo el
  preset (sonando o en fundido). El sketch lo usa para liberar un kit viejo recién
  cuando se apagó, en vez de a tiempo fijo.
- Medido offline (bombo de 1 s cada 129 ms, mayor |segunda diferencia| en el
  redisparo): sin filtro 1211 → 44 · LPF24 8 kHz 1165 → 18 · LPF24 1 kHz 1038 → 4.

## Hook de mezcla antes de los efectos (odes — 2026-09-15)
- `src/amy.c`: `amy_prefx_process(block, n, max_val)` (weak, default no-op), llamado en
  `amy_fill_buffer` justo después de sumar los cores y ANTES de EQ, chorus, eco y reverb.
  `block` es planar (L luego R), en `SAMPLE`.
- **Por qué:** el filtro master de CLAVE se aplicaba como LPF24 en cada voz (12) y con
  reverb el render no llegaba a tiempo (audio roto al tocar el filtro). Filtrar la mezcla
  es equivalente (filtro lineal) y cuesta 2 filtros; `amy_master_postprocess` no sirve
  porque corre DESPUÉS del reverb, y el filtro tiene que ir antes de los efectos.
