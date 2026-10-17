/* C interface of the engine for the Swift app. */
#ifndef ORE_API_H
#define ORE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OreEngine OreEngine;

typedef struct OreConfig {
	const char *data_dir;   /* folder holding the *.LPK archives */
	const char *patch_dir;  /* Chinese patch folder (SCRIPT_CHS, SYS, DATA), or NULL */
	const char *save_dir;   /* writable folder for saves and settings */
	int chinese;            /* 1: play the Chinese patch (requires patch_dir) */
} OreConfig;

/* Returns NULL on failure with a UTF-8 message in err. */
OreEngine *ore_create(const OreConfig *config, char *err, int err_len);
void ore_destroy(OreEngine *engine);

/* Runs one frame at monotonic time now_ms. Returns 0 to keep going, 1 when
   the game asked to quit, -1 on a fatal script error (see ore_last_error). */
int ore_frame(OreEngine *engine, int64_t now_ms);
const char *ore_last_error(OreEngine *engine);

/* The 800x600 BGRA frame (premultiplied=no, alpha=255). */
const uint8_t *ore_framebuffer(OreEngine *engine, int *width, int *height);
/* Increments whenever the frame buffer content changed. */
uint64_t ore_frame_serial(OreEngine *engine);

/* Input in game coordinates (0..799, 0..599). */
void ore_mouse_move(OreEngine *engine, int x, int y);
void ore_mouse_button(OreEngine *engine, int button /*0 left,1 right,2 middle*/, int down);
void ore_mouse_wheel(OreEngine *engine, int delta /* +120 = away from user */);
/* Windows virtual-key codes (VK_RETURN=0x0D, VK_CONTROL=0x11, ...). */
void ore_key(OreEngine *engine, int vk, int down);
/* One-shot script command: 0 quick save, 1 quick load, 2 previous choice,
   3 save, 4 load, 5 skip, 6 auto, 7 toggle history, 8 menu, 9 hide text,
   10 settings, 11 title. The script decides whether it is currently allowed. */
void ore_game_command(OreEngine *engine, int command);

/* Audio: interleaved stereo float at ore_audio_rate(). Called from the audio thread. */
int ore_audio_rate(void);
void ore_audio_mix(OreEngine *engine, float *out, int frames);

/* Modal dialogs and text entry requested by the game (called on the thread
   running ore_frame). kind: 0 = OK, 1 = Yes/No. The app answers later with
   ore_answer_dialog (1 = OK/Yes, 0 = No) / ore_answer_text_input. */
typedef void (*OreDialogCallback)(void *ctx, const char *utf8_text, int kind);
typedef void (*OreTextInputCallback)(void *ctx, const char *utf8_initial);
void ore_set_ui_callbacks(OreEngine *engine, void *ctx, OreDialogCallback dialog, OreTextInputCallback text_input);
void ore_answer_dialog(OreEngine *engine, int result);
void ore_answer_text_input(OreEngine *engine, int ok, const char *utf8);

/* App lifecycle. */
void ore_set_paused(OreEngine *engine, int paused);

#if DEBUG
/* Device smoke test; the host must supply an isolated temporary save directory. */
int ore_debug_start_movie(OreEngine *engine, int index);
#endif

#ifdef __cplusplus
}
#endif

#endif
