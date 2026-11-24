#ifndef BANANA_RETRO_OPTIONS_H
#define BANANA_RETRO_OPTIONS_H

#include "libretro.h"

#include <stdbool.h>
#include <stddef.h>

enum { OPTIONS_MAX = 128, OPTIONS_MAX_VALUES = 24, OPTIONS_TEXT = 128 };

struct core_option {
	char key[OPTIONS_TEXT];
	char description[OPTIONS_TEXT];
	char info[OPTIONS_TEXT];
	char default_value[OPTIONS_TEXT];
	char value[OPTIONS_TEXT];
	char choices[OPTIONS_MAX_VALUES][OPTIONS_TEXT];
	size_t choice_count;
	bool visible;
	bool restart_required;
};

void options_reset(void);
unsigned options_api_version(void);
bool options_register_v0(const struct retro_variable *definitions);
bool options_register_v1(const struct retro_core_option_definition *definitions);
bool options_register_v2(const struct retro_core_options_v2 *definitions);
const char *options_get(const char *key);
bool options_set(const char *key, const char *value);
bool options_set_visible(const char *key, bool visible);
bool options_update_pending(void);
void options_clear_update(void);
size_t options_count(void);
const struct core_option *options_at(size_t index);

#endif
