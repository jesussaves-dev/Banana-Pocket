#pragma once
// Small string helpers shared by banana-ui and BananaTube.
#include <fstream>
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
}
