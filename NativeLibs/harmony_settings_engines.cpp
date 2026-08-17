#include "harmony_settings_internal.h"

// The search engines, and the one substitution that turns a typed phrase into a
// URL.
//
// An engine is a name and a query TEMPLATE holding `{searchTerms}` where the
// phrase goes, which is the spelling OpenSearch settled on and the only one that
// can hold an engine whose query carries anything after the terms. A browser
// that stored prefixes could not describe half the engines a person might add.

namespace harmony::settings {

namespace {

// Where the token sits in a template, or npos when the template cannot carry a
// phrase at all.
size_t termsPosition(const std::string& query)
{
    return query.find(kSearchTermsToken);
}

bool isUnreservedByte(unsigned char byte)
{
    if (byte >= 'A' && byte <= 'Z')
        return true;
    if (byte >= 'a' && byte <= 'z')
        return true;
    if (byte >= '0' && byte <= '9')
        return true;
    return byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

// One line, with the token left intact: a query template is typed by a person
// and a pasted line break would split the record it is stored in.
std::string oneLine(const char* text)
{
    std::string value = text ? text : "";
    std::string clean;
    clean.reserve(value.size());
    for (const char character : value) {
        if (character == '\r' || character == '\n' || character == '\t')
            continue;
        clean.push_back(character);
    }

    const size_t first = clean.find_first_not_of(' ');
    if (first == std::string::npos)
        return { };
    const size_t last = clean.find_last_not_of(' ');
    return clean.substr(first, last - first + 1);
}

// This thread's copy, so a caller reads a string the lock protects without
// racing another thread's edit.
const char* publish(std::string value)
{
    static thread_local std::string storage;
    storage = std::move(value);
    return storage.c_str();
}

} // namespace

std::vector<SearchEngine> defaultEngines()
{
    return {
        SearchEngine { "Google", "https://www.google.com/search?q={searchTerms}" },
        SearchEngine { "DuckDuckGo", "https://duckduckgo.com/?q={searchTerms}" },
        SearchEngine { "Bing", "https://www.bing.com/search?q={searchTerms}" },
        SearchEngine { "Wikipedia", "https://en.wikipedia.org/w/index.php?search={searchTerms}" },
    };
}

bool isUsableQueryTemplate(const std::string& query)
{
    if (query.empty())
        return false;
    return termsPosition(query) != std::string::npos;
}

std::string percentEncoded(const std::string& text)
{
    static const char kHex[] = "0123456789ABCDEF";

    std::string encoded;
    encoded.reserve(text.size());
    for (const char raw : text) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        if (isUnreservedByte(byte)) {
            encoded.push_back(static_cast<char>(byte));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[byte >> 4]);
        encoded.push_back(kHex[byte & 0x0F]);
    }
    return encoded;
}

std::string searchURL(const std::string& query)
{
    ensureLoaded();

    std::string tmpl;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_values.engines.empty())
            return { };
        int index = g_values.defaultEngine;
        if (index < 0 || index >= static_cast<int>(g_values.engines.size()))
            index = 0;
        tmpl = g_values.engines[static_cast<size_t>(index)].query;
    }

    const size_t position = termsPosition(tmpl);
    if (position == std::string::npos)
        return { };

    // The phrase is encoded as the UTF-8 bytes it is made of, which is what a
    // server expects to read back.
    return tmpl.substr(0, position)
        + percentEncoded(query)
        + tmpl.substr(position + std::string(kSearchTermsToken).size());
}

} // namespace harmony::settings

// --- The host's surface -------------------------------------------------------

using namespace harmony::settings;

extern "C" int hb_settings_engine_count(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_values.engines.size());
}

extern "C" const char* hb_settings_engine_name(int index)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < 0 || index >= static_cast<int>(g_values.engines.size()))
        return publish(std::string());
    return publish(g_values.engines[static_cast<size_t>(index)].name);
}

extern "C" const char* hb_settings_engine_query(int index)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < 0 || index >= static_cast<int>(g_values.engines.size()))
        return publish(std::string());
    return publish(g_values.engines[static_cast<size_t>(index)].query);
}

extern "C" int hb_settings_engine_default(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values.defaultEngine;
}

extern "C" void hb_settings_set_engine_default(int index)
{
    withValues([index](Values& values) {
        if (index < 0 || index >= static_cast<int>(values.engines.size()))
            return;
        values.defaultEngine = index;
    });
}

extern "C" int hb_settings_add_engine(const char* name, const char* query)
{
    const std::string wantedName = oneLine(name);
    const std::string wantedQuery = oneLine(query);
    if (wantedName.empty() || !isUsableQueryTemplate(wantedQuery)) {
        setError("a search engine needs a name and a query holding {searchTerms}");
        return -1;
    }

    int added = -1;
    withValues([&](Values& values) {
        values.engines.push_back(SearchEngine { wantedName, wantedQuery });
        added = static_cast<int>(values.engines.size()) - 1;
    });
    return added;
}

extern "C" void hb_settings_update_engine(int index, const char* name, const char* query)
{
    const std::string wantedName = oneLine(name);
    const std::string wantedQuery = oneLine(query);
    if (wantedName.empty() || !isUsableQueryTemplate(wantedQuery)) {
        setError("a search engine needs a name and a query holding {searchTerms}");
        return;
    }

    withValues([&](Values& values) {
        if (index < 0 || index >= static_cast<int>(values.engines.size()))
            return;
        values.engines[static_cast<size_t>(index)] = SearchEngine { wantedName, wantedQuery };
    });
}

extern "C" void hb_settings_remove_engine(int index)
{
    withValues([index](Values& values) {
        if (index < 0 || index >= static_cast<int>(values.engines.size()))
            return;
        // The last one stays. A browser with no search engine cannot answer a
        // typed phrase at all, and the address bar has no other way to.
        if (values.engines.size() <= 1)
            return;

        values.engines.erase(values.engines.begin() + index);
        if (values.defaultEngine == index)
            values.defaultEngine = 0;
        else if (values.defaultEngine > index)
            values.defaultEngine = values.defaultEngine - 1;
    });
}

extern "C" const char* hb_settings_search_url(const char* query)
{
    return publish(searchURL(query ? query : ""));
}
