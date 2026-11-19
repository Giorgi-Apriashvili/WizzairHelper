#pragma once
// CityNames (DECISIONS D-72): the city a Wizz Air station's name stands for, for
// the "Homes on Airbnb" search around a destination's city centre. Station names
// carry airport parts ("London Luton", "Barcelona El Prat"), regions in brackets
// ("Catania (Sicily)"), second cities ("Memmingen / Munich West") and stray line
// breaks, so the services look up the candidates below, most specific first, and
// accept a geocoder hit only when it `names_city` the candidate. Pure.

#include <string>
#include <string_view>
#include <vector>

namespace wizz::city_names {

/// The most words a candidate keeps ("Santiago de Compostela").
inline constexpr std::size_t kMaxWords = 3;
/// Shorter candidates (in UTF-16 units) are left out.
inline constexpr std::size_t kMinLength = 3;

/// The station's name with white-space runs (line breaks included) as one space,
/// without "(…)" parts and without anything from a "/" or a dash between spaces on
/// ("Catania (Sicily)" → "Catania", "Memmingen / Munich West" → "Memmingen",
/// "Palanga – Klaipeda" → "Palanga"), trimmed. A hyphen inside a word stays
/// ("Cluj-Napoca"). Empty when nothing is left.
[[nodiscard]] std::string clean_name(std::string_view station_name);

/// The names to look up, most specific first: the cleaned name's leading words, at most
/// kMaxWords of them, down to one ("London Luton" → "London Luton", "London"), then the
/// first word's part before a hyphen ("Basel-Mulhouse-Freiburg" → …, "Basel"). Names
/// shorter than kMinLength are left out and none repeats; empty when none is left.
[[nodiscard]] std::vector<std::string> candidates(std::string_view station_name);

/// True when a geocoder hit called `hit_name` is the city `candidate` names: the hit's
/// name key (text::normalize_key with hyphens read as spaces: case, accents and spacing
/// ignored) starts with the candidate's key and ends there or goes on with a character
/// that is not a letter or digit ("Frankfurt am Main" for "Frankfurt", "Tel Aviv-Yafo"
/// for "Tel-Aviv", "Târgu Mureș" for "Targu Mures"; not "Granadilla" for "Gran", not
/// "El Prat de Llobregat" for "Barcelona El Prat"). False for an empty candidate key.
[[nodiscard]] bool names_city(std::string_view hit_name, std::string_view candidate);

} // namespace wizz::city_names
