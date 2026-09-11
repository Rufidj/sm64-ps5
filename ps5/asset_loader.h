/* asset_loader - puts the game's ROM-derived data back into the program from
 * the player's own ROM. See asset_loader.c. */
#ifndef ASSET_LOADER_H
#define ASSET_LOADER_H

/* Restores every asset the map lists. Returns 0 when done, 1 when there is no
 * map - a build that still carries its assets - and a negative value on
 * failure, when asset_loader_error() says what went wrong. */
int asset_loader_restore(void);
const char *asset_loader_error(void);

#endif
