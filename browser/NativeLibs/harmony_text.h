#ifndef HARMONY_TEXT_H
#define HARMONY_TEXT_H

#include <string>

// The one conversion between the two spellings of a name.
//
// Windows names paths, windows and shell text in UTF-16; everything this browser
// shows, stores and hands to WebKit is UTF-8. Every module needs both
// directions, and a module that carries its own copy is a module whose copy can
// disagree with the rest about a surrogate pair. So there is one, here.
namespace harmony::text {

// UTF-16 as UTF-8, and back. A conversion that could not be sized or could not
// be written answers empty rather than a buffer of zeros: a name that failed to
// convert must not come back as a name made of nothing.
std::string narrow(const std::wstring& value);
std::string narrow(const wchar_t* value);
std::wstring widen(const std::string& value);
std::wstring widen(const char* value);

} // namespace harmony::text

#endif
