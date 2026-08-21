#include "harmony_data_store_internal.h"

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>

// Origins: how a URL becomes the origin WebKit stores under, which directory
// WebKit gives that origin, and which origins this browser has visited.
//
// The directory name is not a lookup: WebKit derives it from the origin and a
// per-profile salt, and never writes the origin down beside it. Deriving the
// same name here is what lets the site list say "github.com" instead of a
// base64 hash, and what lets a per-origin removal delete exactly that site's
// tree.

namespace harmony::datastore {

namespace {

std::vector<uint8_t> g_salt;
bool g_saltLoaded = false;

std::mutex g_ledgerMutex;
std::vector<LedgerEntry> g_ledger;
bool g_ledgerLoaded = false;
bool g_ledgerDirty = false;

constexpr size_t kSaltSize = 8;

bool sha256(const std::vector<uint8_t>& input, uint8_t digest[32])
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return false;

    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
    if (ok) {
        ok = BCRYPT_SUCCESS(BCryptHashData(
            hash,
            const_cast<PUCHAR>(input.data()),
            static_cast<ULONG>(input.size()),
            0
        ));
    }
    if (ok)
        ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, 32, 0));

    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

// WebKit encodes the digest with base64url and drops the padding, which is what
// makes the name a legal directory name on every platform it runs on.
std::string base64URL(const uint8_t* bytes, size_t size)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t at = 0; at < size; at += 3) {
        const uint32_t first = bytes[at];
        const uint32_t second = at + 1 < size ? bytes[at + 1] : 0;
        const uint32_t third = at + 2 < size ? bytes[at + 2] : 0;
        const uint32_t triple = (first << 16) | (second << 8) | third;

        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        if (at + 1 < size)
            out += kAlphabet[(triple >> 6) & 0x3F];
        if (at + 2 < size)
            out += kAlphabet[triple & 0x3F];
    }
    return out;
}

std::string lowered(const std::string& text)
{
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char value) {
        return static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
    });
    return out;
}

bool isDefaultPort(const std::string& scheme, const std::string& port)
{
    if (scheme == "https" && port == "443")
        return true;
    return scheme == "http" && port == "80";
}

std::vector<uint8_t> makeSalt()
{
    std::vector<uint8_t> salt(kSaltSize, 0);
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        // A salt that is not random still separates one origin's directory from
        // another's, which is all the directory name is for.
        const double now = unixNow();
        const uint64_t seed = static_cast<uint64_t>(now * 1000.0) ^ GetCurrentProcessId();
        for (size_t at = 0; at < salt.size(); ++at)
            salt[at] = static_cast<uint8_t>((seed >> (at * 8)) & 0xFF);
    }
    return salt;
}

void writeSalt(const std::vector<uint8_t>& salt)
{
    HANDLE file = CreateFileW(
        layout().saltFile.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        setError("the profile's storage salt could not be written, so every site's storage directory will be named afresh on the next run");
        return;
    }

    DWORD written = 0;
    const bool ok = WriteFile(file, salt.data(), static_cast<DWORD>(salt.size()), &written, nullptr) != 0;
    CloseHandle(file);
    if (ok && written == salt.size())
        return;

    // A salt of the wrong length is read back as no salt at all, and a profile
    // that makes a new salt every run gives every origin a new storage directory
    // every run. A file that could not be written whole is removed so the next
    // run writes one rather than reading this one.
    DeleteFileW(layout().saltFile.c_str());
    setError("the profile's storage salt could not be written whole");
}

std::vector<uint8_t> readSalt()
{
    HANDLE file = CreateFileW(
        layout().saltFile.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE)
        return { };

    std::vector<uint8_t> salt(kSaltSize, 0);
    DWORD read = 0;
    const bool ok = ReadFile(file, salt.data(), static_cast<DWORD>(salt.size()), &read, nullptr) != 0;
    CloseHandle(file);

    if (!ok || read != salt.size())
        return { };
    return salt;
}

std::string ledgerLine(const LedgerEntry& entry)
{
    char buffer[64] { };
    std::snprintf(buffer, sizeof(buffer), "\t%.0f\t%.0f\n", entry.firstSeen, entry.lastSeen);
    return entry.origin + buffer;
}

} // namespace

// --- Origins ----------------------------------------------------------------

std::string originForURL(const std::string& url)
{
    const auto separator = url.find("://");
    if (separator == std::string::npos)
        return { };

    const std::string scheme = lowered(url.substr(0, separator));
    if (scheme.empty() || scheme == "about" || scheme == "data" || scheme == "blob" || scheme == "javascript")
        return { };
    if (scheme == "file")
        return "file://";

    const size_t authorityStart = separator + 3;
    size_t authorityEnd = url.size();
    for (size_t at = authorityStart; at < url.size(); ++at) {
        const char character = url[at];
        if (character == '/' || character == '?' || character == '#') {
            authorityEnd = at;
            break;
        }
    }

    std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
    if (const auto credentials = authority.find('@'); credentials != std::string::npos)
        authority = authority.substr(credentials + 1);
    if (authority.empty())
        return { };

    // An IPv6 literal keeps its brackets and its own colons; only a colon after
    // the closing bracket introduces a port.
    std::string host = authority;
    std::string port;
    if (authority.front() == '[') {
        if (const auto close = authority.find(']'); close != std::string::npos) {
            host = authority.substr(0, close + 1);
            if (close + 1 < authority.size() && authority[close + 1] == ':')
                port = authority.substr(close + 2);
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }

    host = lowered(host);
    if (host.empty())
        return { };
    if (port.empty() || isDefaultPort(scheme, port))
        return scheme + "://" + host;
    return scheme + "://" + host + ":" + port;
}

std::string hostForOrigin(const std::string& origin)
{
    const auto separator = origin.find("://");
    if (separator == std::string::npos)
        return origin;

    std::string rest = origin.substr(separator + 3);
    if (rest.empty())
        return origin;

    if (rest.front() != '[') {
        if (const auto colon = rest.rfind(':'); colon != std::string::npos)
            rest = rest.substr(0, colon);
    }
    if (rest.rfind("www.", 0) == 0)
        rest = rest.substr(4);
    return rest;
}

const std::vector<uint8_t>& storageSalt()
{
    if (!g_saltLoaded) {
        g_saltLoaded = true;
        g_salt = readSalt();
        if (g_salt.size() != kSaltSize) {
            g_salt = makeSalt();
            writeSalt(g_salt);
        }
    }
    return g_salt;
}

std::string saltedName(const std::string& origin)
{
    const std::vector<uint8_t>& salt = storageSalt();

    std::vector<uint8_t> input;
    input.reserve(origin.size() + salt.size());
    input.insert(input.end(), origin.begin(), origin.end());
    input.insert(input.end(), salt.begin(), salt.end());

    uint8_t digest[32] { };
    if (!sha256(input, digest))
        return { };
    return base64URL(digest, sizeof(digest));
}

std::vector<std::wstring> originStorageDirectories(const std::string& origin)
{
    std::vector<std::wstring> directories;
    const std::string encoded = saltedName(origin);
    if (encoded.empty())
        return directories;

    const std::wstring name = widen(encoded);
    const std::wstring root = layout().generalStorage;

    // The directory the origin is the top of. Everything a third party stored
    // while this site was in front lives inside it, and goes with it.
    const std::wstring own = root + L'\\' + name;
    if (directoryExists(own))
        directories.push_back(own);

    // The directories the origin has as a third party under other sites.
    for (const std::wstring& top : childDirectories(root)) {
        if (top == own)
            continue;
        const std::wstring partitioned = top + L'\\' + name;
        if (directoryExists(partitioned))
            directories.push_back(partitioned);
    }
    return directories;
}

// --- The ledger -------------------------------------------------------------

void ledgerLoad()
{
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    if (g_ledgerLoaded)
        return;
    g_ledgerLoaded = true;

    HANDLE file = CreateFileW(
        layout().originIndexFile.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE)
        return;

    std::string text;
    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        text.append(buffer, read);
    CloseHandle(file);

    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();

        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        const size_t firstTab = line.find('\t');
        if (firstTab == std::string::npos)
            continue;
        const size_t secondTab = line.find('\t', firstTab + 1);
        if (secondTab == std::string::npos)
            continue;

        LedgerEntry entry;
        entry.origin = line.substr(0, firstTab);
        entry.firstSeen = std::strtod(line.substr(firstTab + 1, secondTab - firstTab - 1).c_str(), nullptr);
        entry.lastSeen = std::strtod(line.substr(secondTab + 1).c_str(), nullptr);
        if (!entry.origin.empty())
            g_ledger.push_back(std::move(entry));
    }
}

namespace {

// Marks the ledger unwritten again, so a save that failed is retried rather than
// dropped: the next visit to any site rewrites the whole file.
void ledgerSaveFailed(const char* reason)
{
    {
        std::lock_guard<std::mutex> lock(g_ledgerMutex);
        g_ledgerDirty = true;
    }
    setError(reason);
}

}

void ledgerSave()
{
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_ledgerMutex);
        if (!g_ledgerDirty)
            return;
        g_ledgerDirty = false;
        text.reserve(g_ledger.size() * 64);
        for (const LedgerEntry& entry : g_ledger)
            text += ledgerLine(entry);
    }

    HANDLE file = CreateFileW(
        layout().originIndexFile.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        ledgerSaveFailed("the list of visited sites could not be opened for writing");
        return;
    }

    DWORD written = 0;
    const bool ok = text.empty()
        || WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0;
    CloseHandle(file);

    if (!ok || written != text.size())
        ledgerSaveFailed("the list of visited sites could not be written whole");
}

void ledgerNote(const std::string& origin)
{
    if (origin.empty())
        return;

    const double now = unixNow();
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    for (LedgerEntry& entry : g_ledger) {
        if (entry.origin != origin)
            continue;
        // A second visit inside the same minute is the same visit as far as a
        // site list is concerned, and rewriting the file for it is not worth a
        // disk write.
        if (now - entry.lastSeen < 60.0)
            return;
        entry.lastSeen = now;
        g_ledgerDirty = true;
        return;
    }

    LedgerEntry entry;
    entry.origin = origin;
    entry.firstSeen = now;
    entry.lastSeen = now;
    g_ledger.push_back(std::move(entry));
    g_ledgerDirty = true;
}

void ledgerForget(const std::string& origin)
{
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    const size_t before = g_ledger.size();
    g_ledger.erase(
        std::remove_if(g_ledger.begin(), g_ledger.end(), [&origin](const LedgerEntry& entry) {
            return entry.origin == origin;
        }),
        g_ledger.end()
    );
    if (g_ledger.size() != before)
        g_ledgerDirty = true;
}

void ledgerForgetSince(double since)
{
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    const size_t before = g_ledger.size();
    g_ledger.erase(
        std::remove_if(g_ledger.begin(), g_ledger.end(), [since](const LedgerEntry& entry) {
            return entry.lastSeen >= since;
        }),
        g_ledger.end()
    );
    if (g_ledger.size() != before)
        g_ledgerDirty = true;
}

void ledgerClear()
{
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    if (g_ledger.empty())
        return;
    g_ledger.clear();
    g_ledgerDirty = true;
}

std::vector<LedgerEntry> ledgerSnapshot()
{
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    return g_ledger;
}

} // namespace harmony::datastore
