#pragma once
// Small string helpers shared by banana-ui and BananaTube.
#include <fstream>
#include <cstdlib>
#include <string>

namespace banana
{
	inline std::string trim(const std::string& s)
	{
		size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
		return a == std::string::npos ? "" : s.substr(a, b - a + 1);
	}

	// the first line of a file, trimmed ("" if it can't be read)
	inline std::string readLine(const std::string& path)
	{
		std::ifstream f(path);
		std::string s;
		std::getline(f, s);
		return trim(s);
	}

	// s as one shell word, whatever it holds
	inline std::string shellQuote(const std::string& s)
	{
		std::string r = "'";
		for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
		return r + "'";
	}

	inline std::string replaceAll(std::string s, const std::string& from, const std::string& to)
	{
		if (from.empty()) return s;
		for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
		return s;
	}

	inline std::string systemRunCommand(const std::string& run, const std::string& rom, const std::string& system)
	{
		return replaceAll(replaceAll(run, "%ROM%", shellQuote(rom)), "%SYSTEM%", shellQuote(system));
	}

	inline bool setSystemRunEnvironment(const std::string& system)
	{
		return setenv("BANANA_SYSTEM", system.c_str(), 1) == 0;
	}
}
