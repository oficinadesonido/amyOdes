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
