// A value the schema doesn't accept goes back to the default, for every kind of list. Built with AddressSanitizer by
// ui/tests/run.sh, since a list without its end marker reads past the array without failing any check here.
#include "../src/settings_defaults.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

static int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { bad++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv)
{
	if (argc < 2) { puts("usage: settings-defaults-test <scratch dir>"); return 2; }
	std::string dir = argv[1];
	static const char* const keys[] = { "sounds", "accent", "boxart_source", "usb_mass_storage", "night_light", "shader_mask", "shader_color" };
	{
		std::ofstream f(dir + "/settings.conf");
		for (const char* k : keys) f << k << " = bogus\n";
	}
	banana::normalizeSettings(dir);
	std::map<std::string, std::string> got = banana::readSettings(dir);
	for (const char* k : keys)
		CHECK(got[k] == banana::settingDefault(k), "%s kept %s", k, got[k].c_str());
	printf("settings-defaults: %s\n", bad ? "FAILED" : "all checks passed");
	return bad != 0;
}
