#include "../../ui/src/util.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main()
{
	char temp[] = "/tmp/banana-run-command.XXXXXX";
	char* dir = mkdtemp(temp);
	assert(dir);
	const std::string root(dir);
	const std::string script = root + "/capture arguments.sh";
	const std::string output = root + "/captured output.txt";
	const std::string rom = root + "/A game's ROM with spaces.gba";
	const std::string system = "custom system";
	{
		std::ofstream file(script);
		assert(file);
		file << "#!/bin/sh\nprintf '%s\\n' \"$BANANA_SYSTEM\" \"$@\" > "
		     << banana::shellQuote(output) << "\n";
	}
	assert(chmod(script.c_str(), 0700) == 0);
	const std::string run = banana::shellQuote(script) + " %ROM% %SYSTEM% %ROM%";
	const std::string command = banana::systemRunCommand(run, rom, system);
	const std::string expected = banana::shellQuote(script) + " " + banana::shellQuote(rom) + " " +
	                             banana::shellQuote(system) + " " + banana::shellQuote(rom);
	assert(command == expected);
	assert(command.find("%ROM%") == std::string::npos && command.find("%SYSTEM%") == std::string::npos);
	assert(banana::setSystemRunEnvironment(system));
	assert(std::system(command.c_str()) == 0);
	std::ifstream capturedFile(output);
	assert(capturedFile);
	std::string captured((std::istreambuf_iterator<char>(capturedFile)), std::istreambuf_iterator<char>());
	assert(captured == system + "\n" + rom + "\n" + system + "\n" + rom + "\n");
	assert(unlink(output.c_str()) == 0);
	assert(unlink(script.c_str()) == 0);
	assert(rmdir(root.c_str()) == 0);
}
