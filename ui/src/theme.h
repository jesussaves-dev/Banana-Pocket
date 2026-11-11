#pragma once
#include <string>
#include <vector>

namespace banana
{
	// Themes made by users: a .json in /data/banana/themes (or one of the image's in /usr/share/banana/themes), picked
	// in Settings > Display & sound > Theme as "user:<file name without .json>". A theme starts from Banana's dark or
	// light palette ("base") and replaces any of its colours by name ("colors") and the accent ("accent"); see
	// /usr/share/banana/themes/README.txt.

	// a theme's .json: "user:<name>" from the card or the image, "file:<path>" any file (headless runs); else ""
	std::string themePath(const std::string& value);
	// Pocket, the layout a colour theme is drawn in
	inline std::string pocketTheme() { return "file:" + themePath("user:pocket"); }

	// "user:ocean", the theme's own name ("Ocean"), and its style: the whole look it belongs to, its "style" in the
	// .json ("snes" for Banana Street's), "classic" when it names none (Pocket and its family, and older themes)
	struct UserTheme { std::string value, name, style; };
	std::vector<UserTheme> userThemes();				// the card's first, then the image's (a card file of the same name wins)
	// the styles that have a theme, Classic first, then SNES and Genesis, then any other in name order
	std::vector<std::string> themeStyles(const std::vector<UserTheme>& themes);
	// a style's name in Settings: "Classic", "SNES", "Genesis", or the word itself with a capital
	std::string styleName(const std::string& style);

	// the Theme setting's value -> the palette it draws with ("dark" or "light"), with a user theme's colours
	// and accent set for it (rnds::themeColors, rnds::accent); any other theme clears them and sets accentName's accent.
	// A theme that can't be read falls back to dark, its reason in *why
	std::string applyTheme(const std::string& value, const std::string& accentName, std::string* why = nullptr);
	// a system's icon: the theme's own for it (themeFiles.icons by system id), else def
	std::string themedIcon(const std::string& systemId, const std::string& def);

	// the last applied theme's "crisp": boxes drawn hard-edged -- sharp corners, one flat border, no bands, shadows
	// or dither. The layout menus read it from their own .json; Settings and the search keyboard ask for it here.
	bool themeCrisp();
}
