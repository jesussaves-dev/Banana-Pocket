#include "options.h"

#include <string.h>

static struct core_option items[OPTIONS_MAX];
static size_t count;
static bool updated;

static bool copy_text(char *to, const char *from)
{
	if (!from) from = "";
	size_t n = strlen(from);
	if (n >= OPTIONS_TEXT) return false;
	memcpy(to, from, n + 1);
	return true;
}

static struct core_option *new_item(const char *key, const char *description,
					    const char *info)
{
	if (!key || !*key || strlen(key) >= OPTIONS_TEXT)
		return NULL;
	for (size_t i = 0; i < count; ++i) {
		if (strcmp(items[i].key, key) != 0) continue;
		if (strlen(description ? description : "") >= OPTIONS_TEXT ||
		    strlen(info ? info : "") >= OPTIONS_TEXT) return NULL;
		items[i].choice_count = 0;
		memset(items[i].choices, 0, sizeof(items[i].choices));
		return copy_text(items[i].description, description) && copy_text(items[i].info, info) ?
			&items[i] : NULL;
	}
	if (count == OPTIONS_MAX) return NULL;
	struct core_option *item = &items[count];
	memset(item, 0, sizeof(*item));
	if (!copy_text(item->key, key) || !copy_text(item->description, description) ||
	    !copy_text(item->info, info))
		return NULL;
	item->visible = true;
	++count;
	return item;
}

static bool add_choice(struct core_option *item, const char *value)
{
	if (!value || !*value) return true;
	for (size_t i = 0; i < item->choice_count; ++i)
		if (strcmp(item->choices[i], value) == 0) return true;
	if (item->choice_count == OPTIONS_MAX_VALUES || strlen(value) >= OPTIONS_TEXT ||
	    !copy_text(item->choices[item->choice_count], value))
		return false;
	++item->choice_count;
	return true;
}

static bool finish_item(struct core_option *item, const char *default_value)
{
	if (!item || !default_value || !*default_value) return false;
	if (!add_choice(item, default_value) || !copy_text(item->default_value, default_value))
		return false;
	bool valid = false;
	for (size_t i = 0; i < item->choice_count; ++i)
		if (item->value[0] && strcmp(item->choices[i], item->value) == 0) valid = true;
	if (!valid && !copy_text(item->value, default_value)) return false;
	return true;
}

void options_reset(void)
{
	memset(items, 0, sizeof(items));
	count = 0;
	updated = false;
}

unsigned options_api_version(void) { return 2; }

bool options_register_v0(const struct retro_variable *definitions)
{
	if (!definitions) { options_reset(); return true; }
	for (size_t n = 0; n <= OPTIONS_MAX; ++n) {
		const struct retro_variable *def = &definitions[n];
		if (!def->key) return true;
		if (!def->value) return false;
		const char *sep = strstr(def->value, "; ");
		if (!sep) return false;
		char description[OPTIONS_TEXT];
		size_t dlen = (size_t)(sep - def->value);
		if (dlen >= sizeof(description)) return false;
		memcpy(description, def->value, dlen);
		description[dlen] = '\0';
		struct core_option *item = new_item(def->key, description, "");
		if (!item) return false;
		char values[OPTIONS_TEXT];
		if (!copy_text(values, sep + 2)) return false;
		char *cursor = values;
		char *bar;
		while ((bar = strchr(cursor, '|')) != NULL) {
			*bar = '\0';
			if (!add_choice(item, cursor)) return false;
			cursor = bar + 1;
		}
		if (!add_choice(item, cursor) || item->choice_count == 0 ||
		    !finish_item(item, item->choices[0])) return false;
	}
	return false;
}

bool options_register_v1(const struct retro_core_option_definition *definitions)
{
	if (!definitions) { options_reset(); return true; }
	for (size_t n = 0; n <= OPTIONS_MAX; ++n) {
		const struct retro_core_option_definition *def = &definitions[n];
		if (!def->key) return true;
		struct core_option *item = new_item(def->key, def->desc, def->info);
		if (!item) return false;
		for (size_t i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX; ++i) {
			if (!def->values[i].value) break;
			if (!add_choice(item, def->values[i].value)) return false;
		}
		if (!finish_item(item, def->default_value)) return false;
	}
	return false;
}

bool options_register_v2(const struct retro_core_options_v2 *definitions)
{
	if (!definitions) { options_reset(); return true; }
	if (!definitions->definitions) return false;
	for (size_t n = 0; n <= OPTIONS_MAX; ++n) {
		const struct retro_core_option_v2_definition *def = &definitions->definitions[n];
		if (!def->key) return true;
		struct core_option *item = new_item(def->key,
				def->desc_categorized && *def->desc_categorized ? def->desc_categorized : def->desc,
				def->info_categorized && *def->info_categorized ? def->info_categorized : def->info);
		if (!item) return false;
		for (size_t i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX; ++i) {
			if (!def->values[i].value) break;
			if (!add_choice(item, def->values[i].value)) return false;
		}
		if (!finish_item(item, def->default_value)) return false;
	}
	return false;
}

const char *options_get(const char *key)
{
	if (!key) return NULL;
	for (size_t i = 0; i < count; ++i)
		if (strcmp(items[i].key, key) == 0) return items[i].value;
	return NULL;
}

bool options_set(const char *key, const char *value)
{
	if (!key || !*key || !value || !*value) return false;
	for (size_t i = 0; i < count; ++i) {
		struct core_option *item = &items[i];
		if (strcmp(item->key, key) != 0) continue;
		for (size_t j = 0; j < item->choice_count; ++j) {
			if (strcmp(item->choices[j], value) != 0) continue;
			if (strcmp(item->value, value) != 0) {
				if (!copy_text(item->value, value)) return false;
				item->restart_required = true;
				updated = true;
			}
			return true;
		}
		return false;
	}
	return false;
}

bool options_set_visible(const char *key, bool visible)
{
	if (!key) return false;
	for (size_t i = 0; i < count; ++i) {
		if (strcmp(items[i].key, key) != 0) continue;
		items[i].visible = visible;
		return true;
	}
	return false;
}

bool options_update_pending(void) { return updated; }
void options_clear_update(void) { updated = false; }
size_t options_count(void) { return count; }
const struct core_option *options_at(size_t index) { return index < count ? &items[index] : NULL; }
