/* ps5_settings - the options the player sets in the menu (menu_ps5.c), read by
 * the parts of the port they affect. Saved to the game's data folder. */
#ifndef PS5_SETTINGS_H
#define PS5_SETTINGS_H

#define PS5_ASPECT_16_9 0
#define PS5_ASPECT_4_3  1

#define PS5_LANG_ENGLISH 0
#define PS5_LANG_SPANISH 1

typedef struct {
    int aspect;         /* PS5_ASPECT_*                                   */
    int reflections;    /* 0 or 1: the world mirrored in the water        */
    int sky_level;      /* 0..4: how strongly the sky shows in the water  */
    int fps60;          /* 0 or 1: an interpolated frame between the game's */
    int hd_textures;    /* 0 or 1: a texture pack's images, when one is installed */
    int shadows;        /* 0 none, 1 objects cast real shadows, 2 everything does */
    int show_fps;       /* 0 or 1: frames a second in a corner while playing */
    int draw_distance;  /* 0 the game's, 1 three times it, 2 six times it */
    int antialiasing;   /* 0 or 1: supersampled, at 1080p */
    int resolution;     /* 0 1080p, 1 4K */
    int language;       /* 0 English, 1 Spanish: the menus' and the game's text */
    int rumble;         /* 0 or 1: the pad's motors on the game's own cues */
} Ps5Settings;

extern Ps5Settings g_ps5_settings;

/* The sky level as the alpha the mirrored sky goes into the reflection with. */
float ps5_settings_sky_strength(void);

#endif
