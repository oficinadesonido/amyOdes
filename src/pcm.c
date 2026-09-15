// pcm.c

#include "amy.h"

#ifdef AMY_DAISY
#define malloc_caps(a, b) qspi_malloc(a)
#define free(a) qspi_free(a)
#endif


// This is for any in-memory PCM samples.
typedef struct {
    int16_t * sample_ram;
    uint32_t length;
    uint32_t loopstart;
    uint32_t loopend;
    uint8_t midinote;
    uint32_t samplerate;
    float log2sr;
} memorypcm_preset_t;

// linked list of memorypcm presets
typedef struct memorypcm_ll_t{
    memorypcm_preset_t *preset;
    struct memorypcm_ll_t *next;
    uint16_t preset_number;
} memorypcm_ll_t;


memorypcm_ll_t * memorypcm_ll_start;

#define PCM_AMY_LOG2_SAMPLE_RATE log2f(PCM_AMY_SAMPLE_RATE / ZERO_LOGFREQ_IN_HZ)


memorypcm_preset_t * memorypreset_for_preset_number(uint16_t preset_number) {
    // run through the LL looking for the preset
    memorypcm_ll_t *preset = memorypcm_ll_start;
    while(preset != NULL) {
        if(preset->preset_number == preset_number) {
            if(preset->preset->sample_ram != NULL) {
                return preset->preset;
            }
        }
        preset = preset->next;
    }
    return NULL;
}

void pcm_init() {
    memorypcm_ll_start = NULL;
}

// How many bits used for fractional part of PCM table index.
#define PCM_INDEX_FRAC_BITS 8
// The number of bits used to hold the table index.
#define PCM_INDEX_BITS (31 - PCM_INDEX_FRAC_BITS)

// [amyOdes] Declick del redisparo. Una batería vuelve a disparar la misma voz mientras
// la cola del golpe anterior todavía suena (un bombo de 1 s a 16avos de 120 BPM se
// redispara a los 125 ms). pcm_note_on llevaba la fase a 0 de golpe y la onda saltaba
// del valor de la cola al inicio del sample: el clic. Ahora la cola sigue unos ms más,
// bajando lineal, mientras el golpe nuevo arranca encima (crossfade).
// Estado por osc: lo que tocó el último bloque (desde dónde fundir) y el fundido en
// curso. Solo las primeras PCM_DECLICK_OSCS voces; las demás se comportan como antes.
#define PCM_DECLICK_OSCS    32
#define PCM_DECLICK_SAMPLES 256     // ~5,8 ms @ 44.1 kHz

typedef struct {
    const LUTSAMPLE *table;
    uint32_t length;
    PHASOR step;
    SAMPLE amp;
    int16_t preset;
    uint32_t block;     // total_blocks del render que dejó estos datos
    uint8_t valid;      // la voz seguía dentro del sample al terminar ese bloque
} pcm_last_t;

typedef struct {
    const LUTSAMPLE *table;
    uint32_t length;
    PHASOR phase;
    PHASOR step;
    SAMPLE amp;
    int16_t preset;
    uint32_t block;     // bloque en que se espera el próximo render del fundido
    uint16_t left;      // muestras que faltan; 0 = sin fundido
} pcm_fade_t;

static pcm_last_t pcm_last[PCM_DECLICK_OSCS];
static pcm_fade_t pcm_fade[PCM_DECLICK_OSCS];

// Se llama en el note-on, ANTES de volver la fase a 0: la cola pasa al fundido.
static void pcm_declick_capture(uint16_t osc) {
    if (osc >= PCM_DECLICK_OSCS) return;
    pcm_last_t *l = &pcm_last[osc];
    pcm_fade_t *f = &pcm_fade[osc];
    f->left = 0;
    // Solo si el bloque anterior sonó de verdad: un dato viejo no se funde.
    if (!l->valid || amy_global.total_blocks - l->block > 1) return;
    PHASOR phase = synth[osc]->phase;
    if ((uint32_t)INT_OF_P(phase, PCM_INDEX_BITS) + 1 >= l->length) return;
    f->table  = l->table;
    f->length = l->length;
    f->phase  = phase;
    f->step   = l->step;
    f->amp    = l->amp;
    f->preset = l->preset;
    f->block  = amy_global.total_blocks;
    f->left   = PCM_DECLICK_SAMPLES;
    l->valid  = 0;
}

// Suma la cola en fundido a buf. Si el render no fue contiguo, descarta el fundido.
static void pcm_declick_render(SAMPLE *buf, uint16_t osc) {
    if (osc >= PCM_DECLICK_OSCS) return;
    pcm_fade_t *f = &pcm_fade[osc];
    if (!f->left) return;
    uint32_t tb = amy_global.total_blocks;
    if (tb != f->block && tb != f->block + 1) { f->left = 0; return; }
    f->block = tb + 1;
    const float inv = 1.0f / (float)PCM_DECLICK_SAMPLES;
    for (uint16_t i = 0; i < AMY_BLOCK_SIZE && f->left; i++, f->left--) {
        uint32_t ix = INT_OF_P(f->phase, PCM_INDEX_BITS);
        if (ix + 1 >= f->length) { f->left = 0; break; }
        SAMPLE frac = S_FRAC_OF_P(f->phase, PCM_INDEX_BITS);
        LUTSAMPLE b = f->table[ix];
        LUTSAMPLE c = f->table[ix + 1];
        SAMPLE s = L2S(b) + MUL0_SS(L2S(c - b), frac);
        SAMPLE g = F2S((float)f->left * inv);
        buf[i] += MUL8_SS(g, MUL4_SS(f->amp, s));
        f->phase = P_WRAPPED_SUM(f->phase, f->step);
    }
}


void pcm_note_on(uint16_t osc) {
    if(AMY_IS_SET(synth[osc]->preset)) {
        memorypcm_preset_t *preset = memorypreset_for_preset_number(synth[osc]->preset);
        if(preset == NULL) {
            // baked-in PCM - don't overrun.
            if(synth[osc]->preset >= pcm_samples) synth[osc]->preset = 0;
        }
        pcm_declick_capture(osc);  // [amyOdes] la cola anterior se funde en vez de cortarse
        synth[osc]->phase = 0; // s16.15 index into the table; as if a PHASOR into a 16 bit sample table.
        // Special case: We use the msynth feedback flag to indicate note-off for looping PCM.  As a result, it's explicitly NOT set in amy:hold_and_modify for PCM voices.  Set it here.
        msynth[osc]->feedback = synth[osc]->feedback;
    }
}

void pcm_mod_trigger(uint16_t osc) {
    pcm_note_on(osc);
}


void pcm_note_off(uint16_t osc) {
    if(AMY_IS_SET(synth[osc]->preset)) {
        uint32_t length = 0;
        memorypcm_preset_t * preset = memorypreset_for_preset_number(synth[osc]->preset);
        if(preset != NULL) {
            length = preset->length;
        } else if(synth[osc]->preset < pcm_samples) {
            length = pcm_map[synth[osc]->preset].length;
        }   // [amyOdes] preset de memoria ya liberado: length 0 → para la nota

        if(msynth[osc]->feedback == 0) {
            // Non-looping note: Set phase to the end to cause immediate stop.
            synth[osc]->phase = F2P(length / (float)(1 << PCM_INDEX_BITS));
        } else {
            // Looping is requested, disable future looping, sample will play through to end.
            // (sending a second note-off will stop it immediately).
            msynth[osc]->feedback = 0;
        }
    }
}

// Get either memory preset or baked in preset for preset number
memorypcm_preset_t get_preset_for_preset_number(uint16_t preset_number) {
    // Get the memory preset. If it's null, copy params in from ROM preset
    memorypcm_preset_t preset;
    memorypcm_preset_t * preset_p = memorypreset_for_preset_number(preset_number);
    if(preset_p == NULL) {
        // [amyOdes] Un preset de memoria liberado mientras sonaba caía acá y indexaba
        // pcm_map fuera de rango (lectura salvaje). Degradar a silencio: con length 0
        // el render apaga la nota en el primer bloque.
        if(preset_number >= pcm_samples) {
            preset.sample_ram = (int16_t*)pcm;
            preset.length = 0; preset.loopstart = 0; preset.loopend = 0;
            preset.midinote = 60;
            preset.samplerate = PCM_AMY_SAMPLE_RATE;
            preset.log2sr = PCM_AMY_LOG2_SAMPLE_RATE;
            return preset;
        }
        const pcm_map_t cpreset =  pcm_map[preset_number];
        preset.sample_ram = (int16_t*)pcm + cpreset.offset;
        preset.length = cpreset.length;
        preset.loopstart = cpreset.loopstart;
        preset.loopend = cpreset.loopend;
        preset.midinote = cpreset.midinote;
        preset.samplerate = PCM_AMY_SAMPLE_RATE;
        preset.log2sr = PCM_AMY_LOG2_SAMPLE_RATE;   // amyOdes: rate por-preset (horneado)
    } else {
        preset.sample_ram = preset_p->sample_ram;
        preset.length = preset_p->length;
        preset.loopstart = preset_p->loopstart;
        preset.loopend = preset_p->loopend;
        preset.midinote = preset_p->midinote;
        preset.samplerate = preset_p->samplerate;
        preset.log2sr = preset_p->log2sr;           // amyOdes: rate por-preset (memoria)
    }
    return preset;
}

SAMPLE render_pcm(SAMPLE* buf, uint16_t osc) {
    if(AMY_IS_SET(synth[osc]->preset)) {
        memorypcm_preset_t preset = get_preset_for_preset_number(synth[osc]->preset);
        // Presets can be > 32768 samples long.
        // We need s16.15 fixed-point indexing.
        float logfreq = msynth[osc]->logfreq;
        // If osc[midi_note] is set, shift the freq by the preset's default base_note.
        if (AMY_IS_SET(synth[osc]->midi_note))  logfreq -= logfreq_for_midi_note(preset.midinote);
        // amyOdes: usar el rate por-preset (preset.log2sr) en vez de la constante global,
        // para que samples cargados a otro rate (p.ej. kits a 44.1k) suenen a su velocidad.
        float playback_freq = freq_of_logfreq(preset.log2sr + logfreq);

        SAMPLE max_value = 0;
        SAMPLE amp = F2S(msynth[osc]->amp);
        PHASOR step = F2P((playback_freq / (float)AMY_SAMPLE_RATE) / (float)(1 << PCM_INDEX_BITS));
        const LUTSAMPLE* table = preset.sample_ram;
        uint32_t base_index = INT_OF_P(synth[osc]->phase, PCM_INDEX_BITS);
        //fprintf(stderr, "render_pcm: time=%.3f preset=%d base_index=%d length=%d loopstart=%d loopend=%d fb=%f is_unset_note_off %d\n", amy_global.total_blocks*AMY_BLOCK_SIZE / (float)AMY_SAMPLE_RATE, synth[osc]->preset, base_index, preset->length, preset->loopstart, preset->loopend, msynth[osc]->feedback, AMY_IS_UNSET(synth[osc]->note_off_clock));
        pcm_declick_render(buf, osc);  // [amyOdes]
        for(uint16_t i=0; i < AMY_BLOCK_SIZE; i++) {
            SAMPLE frac = S_FRAC_OF_P(synth[osc]->phase, PCM_INDEX_BITS);
            // [amyOdes] no leer fuera de la tabla: con length 0 (preset liberado) esto
            // era una lectura a ciegas. El bloque de abajo apaga la nota igual.
            LUTSAMPLE b = (base_index < preset.length) ? table[base_index] : 0;
            LUTSAMPLE c = b;
            if (base_index < preset.length) c = table[base_index + 1];
            SAMPLE sample = L2S(b) + MUL0_SS(L2S(c - b), frac);
            synth[osc]->phase = P_WRAPPED_SUM(synth[osc]->phase, step);
            base_index = INT_OF_P(synth[osc]->phase, PCM_INDEX_BITS);
            if(base_index >= preset.length) { // end
                synth[osc]->status = SYNTH_OFF;// is this right?
                sample = 0;
            } else {
                if(msynth[osc]->feedback > 0) { // still looping.  The feedback flag is cleared by pcm_note_off.
                    if(base_index >= preset.loopend) { // loopend
                        // back to loopstart
                        int32_t loop_len = preset.loopend - preset.loopstart;
                        synth[osc]->phase -= F2P(loop_len / (float)(1 << PCM_INDEX_BITS));
                        base_index -= loop_len;
                    }
                }
            }
            SAMPLE value = buf[i] + MUL4_SS(amp, sample);
            buf[i] = value;
            if (value < 0) value = -value;
            if (value > max_value) max_value = value;        
        }
        if (osc < PCM_DECLICK_OSCS) {  // [amyOdes] desde dónde fundir si llega un redisparo
            pcm_last_t *l = &pcm_last[osc];
            l->table  = table;
            l->length = preset.length;
            l->step   = step;
            l->amp    = amp;
            l->preset = synth[osc]->preset;
            l->block  = amy_global.total_blocks;
            l->valid  = base_index < preset.length;
        }
        //printf("render_pcm: osc %d preset %d len %d base_ix %d phase %f step %f tablestep %f amp %f\n",
        //       osc, synth[osc]->preset, preset->length, base_index, P2F(synth[osc]->phase), P2F(step), (1 << PCM_INDEX_BITS) * P2F(step), S2F(msynth[osc]->amp));

        return max_value;
    }
    return 0;
}


SAMPLE compute_mod_pcm(uint16_t osc) {
    if(AMY_IS_SET(synth[osc]->preset)) {
        memorypcm_preset_t preset = get_preset_for_preset_number(synth[osc]->preset);
        float mod_sr = (float)AMY_SAMPLE_RATE / (float)AMY_BLOCK_SIZE;
        PHASOR step = F2P(((float)preset.samplerate / mod_sr) / (1 << PCM_INDEX_BITS));
        const LUTSAMPLE* table = preset.sample_ram;
        uint32_t base_index = INT_OF_P(synth[osc]->phase, PCM_INDEX_BITS);
        SAMPLE sample;
        if(base_index >= preset.length) { // end
            synth[osc]->status = SYNTH_OFF;// is this right?
            sample = 0;
        } else {
            sample = L2S(table[base_index]);
            synth[osc]->phase = P_WRAPPED_SUM(synth[osc]->phase, step);
        }
        return MUL4_SS(F2S(msynth[osc]->amp), sample);
    }
    return 0;
}



// load mono samples (let python parse wave files) into preset # 
// set loopstart, loopend, midinote, samplerate (and log2sr)
// return the allocated sample ram that AMY will fill in.
int16_t * pcm_load(uint16_t preset_number, uint32_t length, uint32_t samplerate, uint8_t midinote, uint32_t loopstart, uint32_t loopend) {
    // if preset was already a memorypcm, we need to unload it
    pcm_unload_preset(preset_number); // this is a no-op if preset doesn't exist or is a const pcm
    // now alloc a new LL entry and preset (the old LL entry is removed with pcm_unload_preset)
    memorypcm_ll_t *new_preset_pointer = malloc_caps(sizeof(memorypcm_ll_t) + sizeof(memorypcm_preset_t) + length * sizeof(int16_t),
						     amy_global.config.ram_caps_sample);
    if(new_preset_pointer  == NULL) {
        fprintf(stderr, "No RAM left for sample load\n");
        return NULL; // no ram for sample
    }
    new_preset_pointer->next = memorypcm_ll_start;
    memorypcm_ll_start = new_preset_pointer;
    new_preset_pointer->preset_number = preset_number;
    memorypcm_preset_t *memory_preset = (memorypcm_preset_t *)(((uint8_t *)new_preset_pointer) + sizeof(memorypcm_ll_t));
    memory_preset->samplerate = samplerate;
    memory_preset->log2sr = log2f((float)samplerate / ZERO_LOGFREQ_IN_HZ);
    memory_preset->midinote = midinote;
    memory_preset->loopstart = loopstart;
    memory_preset->length = length;
    memory_preset->sample_ram = (int16_t *)(((uint8_t *)memory_preset) + sizeof(memorypcm_preset_t));
    if(loopend == 0) {  // loop whole sample
        memory_preset->loopend = memory_preset->length-1;
    } else {
        memory_preset->loopend = loopend;
    }
    new_preset_pointer->preset = memory_preset;
    return memory_preset->sample_ram;
}

// [amyOdes] Igual que pcm_load pero SIN copiar: el preset apunta a una tabla const en
// flash mapeada. Aloja solo nodo + preset, así que cargar un kit entero cuesta 8 mallocs
// de ~40 bytes en vez de cientos de KB de memcpy. Devuelve 0 si ok, -1 si no hay RAM.
int pcm_load_const(uint16_t preset_number, const int16_t *data, uint32_t length, uint32_t samplerate, uint8_t midinote, uint32_t loopstart, uint32_t loopend) {
    pcm_unload_preset(preset_number);
    memorypcm_ll_t *new_preset_pointer = malloc_caps(sizeof(memorypcm_ll_t) + sizeof(memorypcm_preset_t),
						     amy_global.config.ram_caps_sample);
    if(new_preset_pointer == NULL) {
        fprintf(stderr, "No RAM left for const sample load\n");
        return -1;
    }
    new_preset_pointer->next = memorypcm_ll_start;
    memorypcm_ll_start = new_preset_pointer;
    new_preset_pointer->preset_number = preset_number;
    memorypcm_preset_t *memory_preset = (memorypcm_preset_t *)(((uint8_t *)new_preset_pointer) + sizeof(memorypcm_ll_t));
    memory_preset->samplerate = samplerate;
    memory_preset->log2sr = log2f((float)samplerate / ZERO_LOGFREQ_IN_HZ);
    memory_preset->midinote = midinote;
    memory_preset->loopstart = loopstart;
    memory_preset->length = length;
    memory_preset->sample_ram = (int16_t *)data;   // flash mapeada: solo se lee
    if(loopend == 0) {
        memory_preset->loopend = memory_preset->length-1;
    } else {
        memory_preset->loopend = loopend;
    }
    new_preset_pointer->preset = memory_preset;
    return 0;
}

// [amyOdes] ¿Alguna voz sigue leyendo este preset (sonando o en fundido)? Sirve para
// liberar un kit recién cuando se apagó, en vez de a un tiempo fijo. Se llama desde otra
// tarea que el render y sin lock: vale para decidir "todavía no", no es exclusión mutua.
int pcm_preset_in_use(uint16_t preset_number) {
    memorypcm_preset_t *p = memorypreset_for_preset_number(preset_number);
    if (p == NULL) return 0;
    for (uint16_t osc = 0; osc < AMY_OSCS; osc++) {
        if (osc < PCM_DECLICK_OSCS && pcm_fade[osc].left && (uint16_t)pcm_fade[osc].preset == preset_number) return 1;
        if (synth[osc] == NULL || synth[osc]->wave != PCM || synth[osc]->status != SYNTH_AUDIBLE) continue;
        if ((uint16_t)synth[osc]->preset != preset_number) continue;
        if ((uint32_t)INT_OF_P(synth[osc]->phase, PCM_INDEX_BITS) < p->length) return 1;
    }
    return 0;
}

void pcm_unload_preset(uint16_t preset_number) {
    // run through the LL looking for the preset
    memorypcm_ll_t **preset_pointer = &memorypcm_ll_start;
    while(*preset_pointer != NULL) {
        if((*preset_pointer)->preset_number == preset_number) {
	    memorypcm_ll_t *next = (*preset_pointer)->next;
	    //fprintf(stderr, "unload_preset: unloading %d\n", (*preset_pointer)->preset_number);
            // free the memory we allocated
            free((*preset_pointer));
	    // close up the list
	    *preset_pointer = next;
	    return;
        } else {
            preset_pointer = &(*preset_pointer)->next;
        }
    }
    //fprintf(stderr, "pcm_unload_preset: preset %d not found\n", preset_number);  // This happens during a routine load_preset.
}

void pcm_unload_all_presets() {
    memorypcm_ll_t *preset_pointer = memorypcm_ll_start;
    while(preset_pointer != NULL) {
        memorypcm_ll_t *next_pointer = preset_pointer->next;
	//fprintf(stderr, "unload_all_presets: unloading %d\n", preset_pointer->preset_number);
        free(preset_pointer);
        // Go to the next one
        preset_pointer = next_pointer;
    }
    memorypcm_ll_start = NULL;
}
