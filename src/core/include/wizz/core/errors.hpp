#pragma once
// Error kinds shared across core modules. They mirror the .NET exception types
// the specification's control flow depends on (02 §5.1, 03 §7.2, 04 §10):
//   FormatException          -> wizz::FormatError
//   JsonException            -> wizz::json::ParseError
//   InvalidOperationException (wrong JSON element kind) -> wizz::json::TypeError
// Client-level errors (WizzApiException, ...) live in the services library.

#include <stdexcept>

namespace wizz {

/// A document or value had a format the parser cannot use, e.g.
/// "Wizz route map JSON has no 'cities' array." or "Frankfurter v2: expected an array.".
class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace wizz
