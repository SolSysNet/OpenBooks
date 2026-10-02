// Self-contained test runner (no external framework needed).

#include "openplugin/json.hpp"
#include "openplugin/manifest.hpp"
#include "openplugin/process.hpp"
#include "openplugin/registry.hpp"
#include "openplugin/rpc.hpp"
#include "openplugin/sha256.hpp"
#include "openplugin/store.hpp"

#include "test.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace opl;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace testing;

namespace {

json::Value parse(const std::string& s) { return json::parse(s); }

// ===================================================================== JSON

TEST(json_parses_values) {
    const json::Value v = parse(R"( {"a": [1, -2, 3.5, true, false, null], "b": {"c": "d"}} )");
    CHECK(v.isObject());
    const json::Array& a = v.find("a")->asArray();
    CHECK_EQ(a.size(), std::size_t(6));
    CHECK_EQ(a[0].asInt(), 1);
    CHECK_EQ(a[1].asInt(), -2);
    CHECK(a[2].type() == json::Type::Double);
    CHECK_EQ(a[2].asDouble(), 3.5);
    CHECK(a[3].asBool());
    CHECK(!a[4].asBool());
    CHECK(a[5].isNull());
    CHECK_EQ(v.find("b")->find("c")->asString(), std::string("d"));
    CHECK(v.find("missing") == nullptr);
}

TEST(json_integers_are_exact_and_bounded) {
    CHECK_EQ(parse("9223372036854775807").asInt(), INT64_MAX);
    CHECK_EQ(parse("-9223372036854775808").asInt(), INT64_MIN);
    CHECK_THROWS(parse("9223372036854775808"), json::Error, "integer out of range");
    CHECK_THROWS(parse("-9223372036854775809"), json::Error, "integer out of range");
    CHECK_THROWS(parse("1e400"), json::Error, "number out of range");
    CHECK(parse("1e2").type() == json::Type::Double);
    CHECK(parse("-0").isInteger());
}

TEST(json_rejects_malformed_numbers) {
    for (const char* bad : {"01", "-", "1.", ".5", "+1", "1e", "1e+", "0x10", "NaN", "Infinity", "--1"})
        CHECK_THROWS(parse(bad), json::Error, "invalid JSON");
}

TEST(json_rejects_malformed_documents) {
    for (const char* bad : {"", " ", "{", "[1,]", "{\"a\":1,}", "{'a':1}", "[1 2]", "{\"a\" 1}", "tru", "nul",
                            "\"unterminated", "{} {}", "[]x", "{1:2}", "\xEF\xBB\xBF{}"})
        CHECK_THROWS(parse(bad), json::Error, "");
}

TEST(json_rejects_duplicate_keys) {
    CHECK_THROWS(parse(R"({"a":1,"a":2})"), json::Error, "duplicate key \"a\"");
}

TEST(json_strings_and_escapes) {
    CHECK_EQ(parse(R"("a\"b\\c\/d\b\f\n\r\t")").asString(), std::string("a\"b\\c/d\b\f\n\r\t"));
    CHECK_EQ(parse(R"("\u00e9")").asString(), std::string("\xC3\xA9"));
    CHECK_EQ(parse(R"("\uD83D\uDE00")").asString(), std::string("\xF0\x9F\x98\x80"));
    CHECK_EQ(parse("\"\xF0\x9F\x98\x80\"").asString(), std::string("\xF0\x9F\x98\x80"));
    CHECK_THROWS(parse(R"("\uD83D")"), json::Error, "lone high surrogate");
    CHECK_THROWS(parse(R"("\uDE00")"), json::Error, "lone low surrogate");
    CHECK_THROWS(parse(R"("\uD83D\u0041")"), json::Error, "invalid surrogate pair");
    CHECK_THROWS(parse(R"("\x")"), json::Error, "invalid escape");
    CHECK_THROWS(parse("\"a\nb\""), json::Error, "control character");
}

TEST(json_rejects_invalid_utf8) {
    CHECK_THROWS(parse("\"\xC0\xAF\""), json::Error, "invalid UTF-8");          // overlong '/'
    CHECK_THROWS(parse("\"\xED\xA0\x80\""), json::Error, "invalid UTF-8");      // encoded surrogate
    CHECK_THROWS(parse("\"\xF4\x90\x80\x80\""), json::Error, "invalid UTF-8");  // past U+10FFFF
    CHECK_THROWS(parse("\"\xE2\x82\""), json::Error, "invalid UTF-8");          // truncated
    CHECK_THROWS(parse("\"\x80\""), json::Error, "invalid UTF-8");              // stray continuation
    CHECK(json::isValidUtf8("plain ascii"));
    CHECK(json::isValidUtf8("\xE2\x82\xAC"));
    CHECK(!json::isValidUtf8("\xFF"));
}

TEST(json_limits_depth_and_size) {
    std::string ok(64, '['), tooDeep(65, '[');
    ok += std::string(64, ']');
    tooDeep += std::string(65, ']');
    CHECK(parse(ok).isArray());
    CHECK_THROWS(parse(tooDeep), json::Error, "nested more than 64");
    json::Limits small;
    small.maxBytes = 10;
    CHECK_THROWS(json::parse("[1,2,3,4,5,6]", small), json::Error, "larger than");
}

TEST(json_writes_compact_deterministic_output) {
    json::Object o;
    o.set("z", 1);
    o.set("a", json::Array{json::Value("x\ny"), json::Value(nullptr), json::Value(2.0), json::Value(-0.5)});
    o.set("m", json::Object{{"k", true}});
    const std::string text = json::write(json::Value(o));
    CHECK_EQ(text, std::string(R"({"z":1,"a":["x\ny",null,2.0,-0.5],"m":{"k":true}})"));
    CHECK(text.find('\n') == std::string::npos);
    CHECK(parse(text) == json::Value(o));
    CHECK_EQ(json::write(json::Value(std::string("\x01\x7F"))), std::string("\"\\u0001\\u007f\""));
    CHECK_THROWS(json::write(json::Value(std::string("\xFF"))), json::Error, "valid UTF-8");
    CHECK_THROWS(json::write(json::Value(std::nan(""))), json::Error, "NaN");
    // Object equality ignores order; set() replaces.
    o.set("z", 5);
    CHECK_EQ(o.size(), std::size_t(3));
    CHECK(json::Object({{"a", 1}, {"b", 2}}) == json::Object({{"b", 2}, {"a", 1}}));
}

TEST(json_type_errors_name_the_types) {
    CHECK_THROWS(parse("\"x\"").asInt(), json::Error, "expected integer, found string");
    CHECK_THROWS(parse("1.5").asInt(), json::Error, "expected integer, found number");
    CHECK_THROWS(parse("[]").asObject(), json::Error, "expected object, found array");
}

// =================================================================== SHA-256

TEST(sha256_matches_nist_vectors) {
    CHECK_EQ(sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(sha256Hex(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(sha256_incremental_matches_one_shot) {
    std::string data;
    for (int i = 0; i < 1000; ++i) data += static_cast<char>(i * 7);
    for (std::size_t chunk : {1u, 3u, 63u, 64u, 65u, 500u}) {
        Sha256 h;
        for (std::size_t i = 0; i < data.size(); i += chunk) h.update(std::string_view(data).substr(i, chunk));
        const auto d = h.finish();
        CHECK_EQ(toHex(d.data(), d.size()), sha256Hex(data));
    }
    TempDir dir;
    writeFile(dir.path / "f.bin", data);
    CHECK_EQ(sha256File(dir.path / "f.bin"), sha256Hex(data));
    CHECK_THROWS(sha256File(dir.path / "missing"), Error, "cannot read");
}

// ================================================================== manifest

const char* kLuaManifest = R"({
  "schema": 1,
  "id": "org.example.stripe-payments",
  "name": "Stripe Payments",
  "version": "1.2.0",
  "publisher": "Example Org",
  "description": "Pay links.",
  "homepage": "https://example.org/stripe",
  "apps": { "openbooks": ">=0.6 <1.0" },
  "protocol": 1,
  "runtime": "lua",
  "main": "main.lua",
  "network": ["api.stripe.com"],
  "permissions": ["read:invoices", "propose:payments", "store"],
  "extensions": ["openbooks.payments"],
  "commands": [ { "id": "sync", "title": "Sync payments", "menu": "Banking" }, { "id": "settings", "title": "Settings..." } ]
})";

// kLuaManifest with one top-level key replaced (or added, or removed when value is empty).
std::string manifestWith(const std::string& key, const std::string& value) {
    json::Value v = parse(kLuaManifest);
    if (value.empty()) v.asObject().erase(key);
    else v.asObject().set(key, parse(value));
    return json::write(v);
}

TEST(manifest_parses_a_lua_plugin) {
    const Manifest m = parseManifest(kLuaManifest);
    CHECK_EQ(m.id, std::string("org.example.stripe-payments"));
    CHECK_EQ(m.name, std::string("Stripe Payments"));
    CHECK(m.runtime == Runtime::Lua);
    CHECK_EQ(m.main, std::string("main.lua"));
    CHECK_EQ(m.network.size(), std::size_t(1));
    CHECK_EQ(m.permissions.size(), std::size_t(3));
    CHECK_EQ(m.commands.size(), std::size_t(2));
    CHECK_EQ(m.commands[0].menu, std::string("Banking"));
    CHECK_EQ(m.commands[1].menu, std::string("Plugins"));
    CHECK(m.appRange("openbooks") != nullptr);
    CHECK(m.appRange("opentax") == nullptr);
}

TEST(manifest_parses_a_native_plugin) {
    json::Value v = parse(kLuaManifest);
    v.asObject().set("runtime", "native");
    v.asObject().erase("main");
    v.asObject().set("run", parse(R"({"windows": ["bin/plugin.exe", "--flag"], "linux": ["./bin/plugin"]})"));
    const Manifest m = parseManifest(json::write(v));
    CHECK(m.runtime == Runtime::Native);
    CHECK_EQ(m.runWindows.size(), std::size_t(2));
    CHECK_EQ(m.runLinux[0], std::string("./bin/plugin"));
    CHECK(m.runMacos.empty());
}

TEST(manifest_rejects_bad_fields) {
    CHECK_THROWS(parseManifest(manifestWith("surprise", "1")), ManifestError, "unknown key \"surprise\"");
    CHECK_THROWS(parseManifest(manifestWith("schema", "2")), ManifestError, "unsupported \"schema\"");
    CHECK_THROWS(parseManifest(manifestWith("id", "\"Bad Id\"")), ManifestError, "\"id\"");
    CHECK_THROWS(parseManifest(manifestWith("name", "")), ManifestError, "\"name\" is required");
    CHECK_THROWS(parseManifest(manifestWith("name", "\"\"")), ManifestError, "can't be empty");
    CHECK_THROWS(parseManifest(manifestWith("version", "\"1.2-beta\"")), ManifestError, "\"version\"");
    CHECK_THROWS(parseManifest(manifestWith("homepage", "\"http://example.org\"")), ManifestError, "https://");
    CHECK_THROWS(parseManifest(manifestWith("apps", "{}")), ManifestError, "\"apps\"");
    CHECK_THROWS(parseManifest(manifestWith("apps", R"({"openbooks": "soon"})")), ManifestError, "version range");
    CHECK_THROWS(parseManifest(manifestWith("runtime", "\"python\"")), ManifestError, "\"runtime\"");
    CHECK_THROWS(parseManifest(manifestWith("permissions", R"(["read:everything"])")), ManifestError,
                 "unknown permission \"read:everything\"");
    CHECK_THROWS(parseManifest(manifestWith("permissions", R"(["store", "store"])")), ManifestError, "twice");
    CHECK_THROWS(parseManifest(manifestWith("network", R"(["https://api.stripe.com"])")), ManifestError, "valid host");
    CHECK_THROWS(parseManifest(manifestWith("run", R"({"linux": ["./x"]})")), ManifestError, "only for native");
    CHECK_THROWS(parseManifest(manifestWith("commands", R"([{"id": "a", "title": "A"}, {"id": "a", "title": "B"}])")),
                 ManifestError, "used twice");
    CHECK_THROWS(parseManifest("[]"), ManifestError, "JSON object");
    CHECK_THROWS(parseManifest("{"), ManifestError, "plugin.json");
}

TEST(manifest_keeps_scripts_inside_the_folder) {
    for (const char* bad : {"\"../main.lua\"", "\"/main.lua\"", "\"lib\\\\main.lua\"", "\"C:/main.lua\"",
                            "\"main.txt\"", "\"./main.lua\"", "\"lib//main.lua\""})
        CHECK_THROWS(parseManifest(manifestWith("main", bad)), ManifestError, "\"main\"");
    CHECK_EQ(parseManifest(manifestWith("main", "\"lib/start.lua\"")).main, std::string("lib/start.lua"));

    json::Value v = parse(kLuaManifest);
    v.asObject().set("runtime", "native");
    v.asObject().erase("main");
    v.asObject().set("run", parse(R"({"linux": ["/usr/bin/curl"]})"));
    CHECK_THROWS(parseManifest(json::write(v)), ManifestError, "inside the plugin folder");
    v.asObject().set("run", parse(R"({"linux": ["../../bin/sh"]})"));
    CHECK_THROWS(parseManifest(json::write(v)), ManifestError, "inside the plugin folder");
    v.asObject().erase("run");
    CHECK_THROWS(parseManifest(json::write(v)), ManifestError, "need \"run\"");
}

TEST(manifest_safe_relative_paths) {
    CHECK(isSafeRelativePath("main.lua"));
    CHECK(isSafeRelativePath("lib/a-b_c.lua"));
    for (const char* bad : {"", "/a", "a/../b", "..", "./a", "a//b", "a\\b", "C:a", "a/", "a.", "a ", "a\tb", "a|b"})
        CHECK(!isSafeRelativePath(bad));
}

TEST(manifest_host_patterns) {
    for (const char* good : {"api.stripe.com", "*.example.com", "localhost", "localhost:8080", "127.0.0.1",
                             "10.0.0.5:8443", "a-b.example.co.uk", "example.com:443"})
        CHECK(isValidHostPattern(good));
    for (const char* bad : {"", "com", "*.com", "API.stripe.com", "https://api.stripe.com", "api.stripe.com/",
                            "*.127.0.0.1", "*.localhost", "1.2.3", "256.1.1.1", "01.2.3.4", "example.com:0",
                            "example.com:65536", ":443", "exa_mple.com", "-a.example.com", "*", "*.*.example.com",
                            "a..example.com", "example.com.", "api.stripe.com:08"})
        CHECK(!isValidHostPattern(bad));

    CHECK(hostMatches("api.stripe.com", "api.stripe.com", 443));
    CHECK(!hostMatches("api.stripe.com", "api.stripe.com", 8443));
    CHECK(!hostMatches("api.stripe.com", "evil-api.stripe.com", 443));
    CHECK(hostMatches("*.example.com", "a.example.com", 443));
    CHECK(!hostMatches("*.example.com", "example.com", 443));
    CHECK(!hostMatches("*.example.com", "a.b.example.com", 443));
    CHECK(!hostMatches("*.example.com", "evilexample.com", 443));
    CHECK(!hostMatches("*.example.com", ".example.com", 443));
    CHECK(hostMatches("localhost:8080", "localhost", 8080));
    CHECK(!hostMatches("localhost:8080", "localhost", 443));
    CHECK(!hostMatches("not a pattern", "x", 443));

    CHECK(isDevelopmentHostPattern("localhost"));
    CHECK(isDevelopmentHostPattern("127.0.0.1"));
    CHECK(isDevelopmentHostPattern("api.example.com:8443"));
    CHECK(!isDevelopmentHostPattern("api.example.com"));
}

TEST(manifest_versions_and_ranges) {
    CHECK(parseVersion("1.2.3").has_value());
    CHECK(!parseVersion("1.2.3-beta").has_value());
    CHECK(!parseVersion("01.2").has_value());
    CHECK(!parseVersion("1..2").has_value());
    CHECK(!parseVersion("1.2.3.4.5").has_value());
    CHECK_EQ(compareVersions(*parseVersion("1.2"), *parseVersion("1.2.0")), 0);
    CHECK_EQ(compareVersions(*parseVersion("0.10"), *parseVersion("0.9")), 1);
    CHECK(versionInRange("0.6.0", ">=0.6 <1.0"));
    CHECK(versionInRange("0.9.9", ">=0.6 <1.0"));
    CHECK(!versionInRange("1.0", ">=0.6 <1.0"));
    CHECK(!versionInRange("0.5.0", ">=0.6 <1.0"));
    CHECK(versionInRange("1.2", "1.2.0"));
    CHECK(versionInRange("2", ">1.9"));
    CHECK(!versionInRange("1.2", "=1.3"));
    CHECK(!isValidVersionRange(""));
    CHECK(!isValidVersionRange(">=x"));
    CHECK(!isValidVersionRange("~1.2"));
}

TEST(manifest_permission_table) {
    CHECK(findPermission("read:return") != nullptr);
    CHECK(findPermission("read:return")->high);
    CHECK(!findPermission("read:invoices")->high);
    CHECK(findPermission("network") == nullptr);  // hosts are listed, not a permission
}

// ================================================================== registry

const AppIdentity kApp{"openbooks", "0.6.0", 1, 1};

// Writes a plugin folder from kLuaManifest with optional overrides.
fs::path makePlugin(const fs::path& pluginsDir, const std::string& folder, const std::string& manifest = kLuaManifest) {
    const fs::path dir = pluginsDir / folder;
    writeFile(dir / "plugin.json", manifest);
    writeFile(dir / "main.lua", "op.command('sync', function() end)\n");
    writeFile(dir / "lib" / "client.lua", "return {}\n");
    return dir;
}

const PluginEntry* findEntry(const std::vector<PluginEntry>& entries, const std::string& folder) {
    for (const PluginEntry& e : entries)
        if (e.folder.filename().u8string() == folder) return &e;
    return nullptr;
}

TEST(registry_hashes_whole_folders) {
    TempDir dir;
    const fs::path p = makePlugin(dir.path, "p");
    const std::string h1 = hashPluginFolder(p);
    CHECK_EQ(h1.size(), std::size_t(64));
    CHECK_EQ(hashPluginFolder(p), h1);
    CHECK_EQ(listPluginFiles(p), (std::vector<std::string>{"lib/client.lua", "main.lua", "plugin.json"}));

    writeFile(p / ".DS_Store", "finder junk");  // OS clutter doesn't count
    writeFile(p / "lib" / "Thumbs.db", "explorer junk");
    CHECK_EQ(hashPluginFolder(p), h1);

    writeFile(p / "lib" / "client.lua", "return {evil = true}\n");
    const std::string h2 = hashPluginFolder(p);
    CHECK(h2 != h1);
    writeFile(p / "extra.lua", "");  // even an empty new file counts
    CHECK(hashPluginFolder(p) != h2);
    fs::rename(p / "extra.lua", p / "other.lua");  // and so does a rename
    CHECK(hashPluginFolder(p) != h2);
    CHECK_THROWS(hashPluginFolder(dir.path / "missing"), Error, "doesn't exist");
}

TEST(registry_refuses_links) {
    TempDir dir;
    const fs::path p = makePlugin(dir.path, "p");
    writeFile(dir.path / "outside.lua", "secret");
    std::error_code ec;
    fs::create_symlink(dir.path / "outside.lua", p / "link.lua", ec);
    if (ec) {
        std::cout << "  (skipped: creating links needs Developer Mode or admin rights here)\n";
        return;
    }
    CHECK_THROWS(hashPluginFolder(p), Error, "contains a link");
}

TEST(registry_enable_and_reload) {
    TempDir dir;
    const fs::path plugins = dir.path / "plugins";
    const fs::path settings = dir.path / "config" / "plugins.json";
    makePlugin(plugins, "stripe");

    Registry reg(plugins, settings, kApp);
    reg.load();  // no file yet: empty
    auto entries = reg.scan();
    CHECK_EQ(entries.size(), std::size_t(1));
    const PluginEntry& e = entries[0];
    CHECK(e.status == PluginStatus::NotEnabled);
    CHECK_EQ(e.id(), std::string("org.example.stripe-payments"));

    // The user grants less than requested.
    reg.enable(e, {"read:invoices", "store"}, {"api.stripe.com"});
    CHECK(fs::exists(settings));
    CHECK(reg.scan()[0].status == PluginStatus::Enabled);

    Registry again(plugins, settings, kApp);
    again.load();
    const auto reloaded = again.scan();
    CHECK(reloaded[0].status == PluginStatus::Enabled);
    CHECK_EQ(reloaded[0].grant->permissions, (std::vector<std::string>{"read:invoices", "store"}));
    CHECK_EQ(reloaded[0].grant->requestedPermissions.size(), std::size_t(3));

    again.disable("org.example.stripe-payments");
    CHECK(again.scan()[0].status == PluginStatus::NotEnabled);
    Registry third(plugins, settings, kApp);
    third.load();
    CHECK(third.grant("org.example.stripe-payments") == nullptr);
}

TEST(registry_enable_checks_what_was_requested) {
    TempDir dir;
    makePlugin(dir.path / "plugins", "stripe");
    Registry reg(dir.path / "plugins", dir.path / "plugins.json", kApp);
    const PluginEntry e = reg.scan()[0];
    CHECK_THROWS(reg.enable(e, {"read:ledger"}, {}), Error, "didn't ask for the permission \"read:ledger\"");
    CHECK_THROWS(reg.enable(e, {}, {"evil.example.com"}), Error, "didn't ask to contact \"evil.example.com\"");
    CHECK(reg.grant(e.id()) == nullptr);
}

TEST(registry_enable_rejects_files_changed_during_review) {
    TempDir dir;
    const fs::path p = makePlugin(dir.path / "plugins", "stripe");
    Registry reg(dir.path / "plugins", dir.path / "plugins.json", kApp);
    const PluginEntry e = reg.scan()[0];
    writeFile(p / "main.lua", "-- swapped after the consent screen was shown\n");
    CHECK_THROWS(reg.enable(e, {}, {}), Error, "changed while you were reviewing");
    CHECK(reg.grant(e.id()) == nullptr);
}

TEST(registry_changed_files_need_approval) {
    TempDir dir;
    const fs::path plugins = dir.path / "plugins";
    const fs::path p = makePlugin(plugins, "stripe");
    Registry reg(plugins, dir.path / "plugins.json", kApp);
    reg.enable(reg.scan()[0], {"store"}, {"api.stripe.com"});

    writeFile(p / "lib" / "client.lua", "return {exfiltrate = true}\n");
    auto e = reg.scan()[0];
    CHECK(e.status == PluginStatus::NeedsApproval);
    CHECK(e.detail.find("files changed") != std::string::npos);
    CHECK(e.newPermissions.empty());

    // An update that asks for more: the new requests are listed for the consent screen.
    json::Value v = parse(kLuaManifest);
    v.asObject().set("version", "1.3.0");
    v.asObject().set("permissions", parse(R"(["read:invoices", "propose:payments", "store", "read:ledger"])"));
    v.asObject().set("network", parse(R"(["api.stripe.com", "files.stripe.com"])"));
    writeFile(p / "plugin.json", json::write(v));
    e = reg.scan()[0];
    CHECK(e.status == PluginStatus::NeedsApproval);
    CHECK(e.detail.find("updated from 1.2.0 to 1.3.0") != std::string::npos);
    CHECK_EQ(e.newPermissions, std::vector<std::string>{"read:ledger"});
    CHECK_EQ(e.newNetwork, std::vector<std::string>{"files.stripe.com"});

    reg.enable(e, {"store"}, {"api.stripe.com"});
    CHECK(reg.scan()[0].status == PluginStatus::Enabled);
}

TEST(registry_reports_incompatible_and_invalid_plugins) {
    TempDir dir;
    const fs::path plugins = dir.path / "plugins";
    makePlugin(plugins, "wrong-app", manifestWith("apps", R"({"opentax": ">=0.1"})"));
    json::Value newer = parse(kLuaManifest);
    newer.asObject().set("id", "org.example.newer");
    newer.asObject().set("apps", parse(R"({"openbooks": ">=2.0"})"));
    makePlugin(plugins, "newer", json::write(newer));
    json::Value proto = parse(kLuaManifest);
    proto.asObject().set("id", "org.example.proto");
    proto.asObject().set("protocol", 7);
    makePlugin(plugins, "proto", json::write(proto));
    makePlugin(plugins, "broken", "{ not json");
    fs::create_directories(plugins / "empty");
    writeFile(plugins / "loose-file.txt", "ignored");

    Registry reg(plugins, dir.path / "plugins.json", kApp);
    const auto entries = reg.scan();
    CHECK_EQ(entries.size(), std::size_t(5));
    CHECK(findEntry(entries, "wrong-app")->status == PluginStatus::Incompatible);
    CHECK(findEntry(entries, "newer")->status == PluginStatus::Incompatible);
    CHECK(findEntry(entries, "newer")->detail.find(">=2.0") != std::string::npos);
    CHECK(findEntry(entries, "proto")->status == PluginStatus::Incompatible);
    CHECK(findEntry(entries, "broken")->status == PluginStatus::Invalid);
    CHECK(findEntry(entries, "empty")->status == PluginStatus::Invalid);
    CHECK(findEntry(entries, "empty")->detail.find("no plugin.json") != std::string::npos);
    CHECK_THROWS(reg.enable(*findEntry(entries, "newer"), {}, {}), Error, "can't be enabled");
}

TEST(registry_duplicate_ids_are_invalid) {
    TempDir dir;
    makePlugin(dir.path / "plugins", "one");
    makePlugin(dir.path / "plugins", "two");
    Registry reg(dir.path / "plugins", dir.path / "plugins.json", kApp);
    for (const PluginEntry& e : reg.scan()) {
        CHECK(e.status == PluginStatus::Invalid);
        CHECK(e.detail.find("more than one installed plugin") != std::string::npos);
    }
}

TEST(registry_damaged_settings_disable_everything) {
    TempDir dir;
    makePlugin(dir.path / "plugins", "stripe");
    const fs::path settings = dir.path / "plugins.json";
    Registry reg(dir.path / "plugins", settings, kApp);
    reg.enable(reg.scan()[0], {}, {});
    writeFile(settings, "{\"schema\": 1, \"plugins\": {\"org.example.stripe-payments\": {\"hash\": 5}}}");
    Registry again(dir.path / "plugins", settings, kApp);
    CHECK_THROWS(again.load(), Error, "damaged");
    CHECK(again.scan()[0].status == PluginStatus::NotEnabled);
}

// ===================================================================== store

TEST(store_set_get_erase) {
    PluginStore s;
    s.set("org.example.p", "lastSync", "2026-09-30", false);
    s.set("org.example.p", "apiKey", "rk_live_123", true);
    CHECK_EQ(s.get("org.example.p", "lastSync")->value, std::string("2026-09-30"));
    CHECK(s.get("org.example.p", "apiKey")->secret);
    CHECK(s.hasSecrets("org.example.p"));
    CHECK(s.get("org.example.p", "nope") == nullptr);
    CHECK(s.get("org.example.other", "lastSync") == nullptr);
    CHECK_EQ(s.plugins(), std::vector<std::string>{"org.example.p"});
    CHECK(s.erase("org.example.p", "apiKey"));
    CHECK(!s.hasSecrets("org.example.p"));
    CHECK(s.erase("org.example.p", "lastSync"));
    CHECK(s.empty());  // a plugin with no entries left disappears
}

TEST(store_reserves_host_keys) {
    PluginStore s;
    CHECK_THROWS(s.set("org.example.p", "@use", "1", false), StoreError, "reserved");
    s.setUsedWithFile("org.example.p", true);
    CHECK(s.usedWithFile("org.example.p"));
    CHECK(s.get("org.example.p", "@use") == nullptr);  // plugins can't see host keys
    CHECK(!s.erase("org.example.p", "@use"));
    CHECK_EQ(*s.getHost("org.example.p", "@use"), std::string("1"));
    CHECK_THROWS(s.setHost("org.example.p", "use", "1"), StoreError, "must start with '@'");
    s.setUsedWithFile("org.example.p", false);
    CHECK(!s.usedWithFile("org.example.p"));
    CHECK(s.empty());
}

TEST(store_enforces_limits) {
    StoreLimits limits;
    limits.maxKeyBytes = 8;
    limits.maxValueBytes = 10;
    limits.maxPluginBytes = 30;
    PluginStore s(limits);
    CHECK_THROWS(s.set("org.example.p", "123456789", "v", false), StoreError, "keys are limited");
    CHECK_THROWS(s.set("org.example.p", "k", std::string(11, 'v'), false), StoreError, "values are limited");
    s.set("org.example.p", "a", std::string(10, 'v'), false);  // 11 bytes
    s.set("org.example.p", "b", std::string(10, 'v'), false);  // 22 bytes
    CHECK_THROWS(s.set("org.example.p", "c", std::string(10, 'v'), false), StoreError, "exceed");
    s.set("org.example.p", "b", std::string(10, 'w'), false);  // replacing counts the difference only
    CHECK_EQ(s.bytesUsed("org.example.p"), std::size_t(22));
    s.set("org.example.other", "c", std::string(10, 'v'), false);  // limits are per plugin

    CHECK_THROWS(s.set("Not Valid", "k", "v", false), StoreError, "invalid plugin id");
    CHECK_THROWS(s.set("org.example.p", "", "v", false), StoreError, "can't be empty");
    CHECK_THROWS(s.set("org.example.p", "a\nb", "v", false), StoreError, "control characters");
    CHECK_THROWS(s.set("org.example.p", "k", "\xFF", false), StoreError, "UTF-8");

    // Loading from a file skips the size limits (a newer app may allow more) but not validity.
    s.load("org.example.p", "big", std::string(100, 'v'), false);
    CHECK_EQ(s.get("org.example.p", "big")->value.size(), std::size_t(100));
    CHECK_THROWS(s.load("org.example.p", "k", "\xFF", false), StoreError, "UTF-8");

    s.erasePlugin("org.example.p");
    CHECK_EQ(s.plugins(), std::vector<std::string>{"org.example.other"});
}

// ============================================================ process + channel

SpawnOptions childOptions(std::vector<std::string> extraArgs = {}) {
    SpawnOptions o;
    o.argv.push_back(fs::absolute(OPENPLUGIN_TEST_CHILD).u8string());
    for (std::string& a : extraArgs) o.argv.push_back(std::move(a));
    o.workingDir = fs::temp_directory_path();
    o.environment = scrubbedEnvironment();
    o.environment.emplace_back("OPENPLUGIN_PROTOCOL", "1");
    return o;
}

std::unique_ptr<rpc::Channel> startChild(std::vector<std::string> args = {}, rpc::ChannelLimits limits = {}) {
    return std::make_unique<rpc::Channel>(Process::spawn(childOptions(std::move(args))), limits);
}

// Polls until `done` is true or 10 seconds pass.
bool pollUntil(rpc::Channel& ch, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        ch.poll(20ms);
    }
    return true;
}

// Sends one request and waits for its outcome.
rpc::Outcome call(rpc::Channel& ch, const std::string& method, json::Value params = json::Object{},
                  std::chrono::milliseconds timeout = 10s) {
    std::optional<rpc::Outcome> result;
    ch.request(method, std::move(params), timeout, [&](const rpc::Outcome& o) { result = o; });
    if (!pollUntil(ch, [&] { return result.has_value(); })) {
        rpc::Outcome o;
        o.status = rpc::Outcome::Status::Error;
        o.message = "test gave up waiting";
        return o;
    }
    return *result;
}

TEST(process_requires_absolute_existing_programs) {
    SpawnOptions o = childOptions();
    o.argv[0] = "openplugin_test_child";
    CHECK_THROWS(Process::spawn(o), Error, "must be absolute");
    o.argv[0] = (fs::temp_directory_path() / "openplugin-no-such-program").u8string();
    CHECK_THROWS(Process::spawn(o), Error, "");
    o.argv.clear();
    CHECK_THROWS(Process::spawn(o), Error, "no program");
}

TEST(channel_round_trips_requests) {
    auto ch = startChild();
    const json::Value params = json::Object{{"amount", "1250.00"}, {"name", "Café \xE2\x82\xAC"}, {"n", 7}};
    const rpc::Outcome o = call(*ch, "echo", params);
    CHECK(o.ok());
    CHECK(o.result == params);
    for (int i = 0; i < 50; ++i) {  // many in flight at once
        std::optional<rpc::Outcome> r;
        ch->request("echo", json::Object{{"i", i}}, 10s, [&](const rpc::Outcome& x) { r = x; });
        CHECK(pollUntil(*ch, [&] { return r.has_value(); }));
        CHECK_EQ(r->result.find("i")->asInt(), i);
    }
    const rpc::Outcome unknown = call(*ch, "nope");
    CHECK(unknown.status == rpc::Outcome::Status::Error);
    CHECK_EQ(unknown.code, int(rpc::MethodNotFound));
    CHECK(!ch->closed());
}

TEST(process_passes_arguments_exactly) {
    const std::vector<std::string> args = {"plain", "two words", "", "quote\"inside", "trailing\\", "back\\\\\"slash",
                                           "tab\there", "\xC3\xBCn\xC3\xAF\x63\xC3\xB8\x64\xC3\xA9", "--opt=a b",
                                           "%PATH%", "$(touch pwned)", "; rm -rf ~"};
    auto ch = startChild(args);
    const rpc::Outcome o = call(*ch, "args");
    CHECK(o.ok());
    json::Array expected;
    for (const std::string& a : args) expected.emplace_back(a);
    CHECK(o.result == json::Value(expected));
}

TEST(process_environment_is_scrubbed) {
    setEnv("OPENBOOKS_PASSWORD", "hunter2");
    setEnv("OPENPLUGIN_TEST_SECRET", "leaked");
    auto ch = startChild();
    CHECK(call(*ch, "getenv", json::Object{{"name", "OPENBOOKS_PASSWORD"}}).result.isNull());
    CHECK(call(*ch, "getenv", json::Object{{"name", "OPENPLUGIN_TEST_SECRET"}}).result.isNull());
    CHECK_EQ(call(*ch, "getenv", json::Object{{"name", "OPENPLUGIN_PROTOCOL"}}).result.asString(), std::string("1"));
    CHECK(call(*ch, "getenv", json::Object{{"name", "PATH"}}).result.isString());
    for (const auto& [name, value] : scrubbedEnvironment()) {
        CHECK(name != "OPENBOOKS_PASSWORD");
        CHECK(name != "OPENPLUGIN_TEST_SECRET");
    }
}

TEST(process_runs_in_the_working_directory) {
    TempDir dir;
    SpawnOptions o = childOptions();
    o.workingDir = dir.path;
    rpc::Channel ch(Process::spawn(o));
    const rpc::Outcome r = call(ch, "cwd");
    CHECK(r.ok());
    CHECK(fs::equivalent(fs::u8path(r.result.asString()), dir.path));
}

TEST(channel_answers_plugin_requests) {
    auto ch = startChild();
    std::vector<std::string> seen;
    ch->onRequest([&](const std::string& method, const json::Value& params, rpc::Responder respond) {
        seen.push_back(method);
        if (method == "host/read") respond.result(json::Object{{"view", *params.find("view")}, {"rows", json::Array{}}});
        else if (method == "host/forbidden") throw rpc::RemoteError(rpc::PermissionDenied, "not granted");
        else if (method == "host/bad-params") (void)params.find("view")->asInt();  // json::Error -> InvalidParams
        else throw std::runtime_error("boom");
    });

    rpc::Outcome o = call(*ch, "ask", json::Object{{"method", "host/read"}, {"params", json::Object{{"view", "invoices"}}}});
    CHECK(o.ok());
    CHECK_EQ(o.result.find("got")->find("view")->asString(), std::string("invoices"));

    o = call(*ch, "ask", json::Object{{"method", "host/forbidden"}});
    CHECK_EQ(o.result.find("got")->find("code")->asInt(), int(rpc::PermissionDenied));
    CHECK_EQ(o.result.find("got")->find("message")->asString(), std::string("not granted"));

    o = call(*ch, "ask", json::Object{{"method", "host/bad-params"}, {"params", json::Object{{"view", "x"}}}});
    CHECK_EQ(o.result.find("got")->find("code")->asInt(), int(rpc::InvalidParams));

    o = call(*ch, "ask", json::Object{{"method", "host/other"}});
    CHECK_EQ(o.result.find("got")->find("code")->asInt(), int(rpc::InternalError));
    CHECK_EQ(seen.size(), std::size_t(4));
}

TEST(channel_answers_later_across_polls) {
    // host/propose waits for the user's review, which spans many frames.
    auto ch = startChild();
    std::optional<rpc::Responder> later;
    ch->onRequest([&](const std::string&, const json::Value&, rpc::Responder respond) { later = respond; });
    std::optional<rpc::Outcome> result;
    ch->request("ask", json::Object{{"method", "host/propose"}}, 10s, [&](const rpc::Outcome& o) { result = o; });
    CHECK(pollUntil(*ch, [&] { return later.has_value(); }));
    for (int i = 0; i < 5; ++i) ch->poll(10ms);  // "frames" go by
    CHECK(!result.has_value());
    CHECK(!later->answered());
    later->result(json::Object{{"applied", true}});
    later->result(json::Object{{"applied", false}});  // second answer is ignored
    CHECK(later->answered());
    CHECK(pollUntil(*ch, [&] { return result.has_value(); }));
    CHECK(result->result.find("got")->find("applied")->asBool());
}

TEST(channel_without_a_handler_reports_method_not_found) {
    auto ch = startChild();
    const rpc::Outcome o = call(*ch, "ask", json::Object{{"method", "host/read"}});
    CHECK_EQ(o.result.find("got")->find("code")->asInt(), int(rpc::MethodNotFound));
}

TEST(channel_delivers_notifications) {
    auto ch = startChild();
    std::vector<std::string> got;
    ch->onNotification([&](const std::string& method, const json::Value& params) {
        got.push_back(method);
        CHECK_EQ(params.find("n")->asInt(), 1);
    });
    CHECK(call(*ch, "notify", json::Object{{"method", "$/log"}}).ok());
    CHECK_EQ(got, std::vector<std::string>{"$/log"});
}

TEST(channel_times_out_then_keeps_working) {
    auto ch = startChild();
    const auto start = std::chrono::steady_clock::now();
    const rpc::Outcome o = call(*ch, "slow", json::Object{{"ms", 1500}}, 200ms);
    CHECK(o.status == rpc::Outcome::Status::Timeout);
    CHECK(o.message.find("didn't answer") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - start < 1400ms);
    // The late answer is ignored; the next request works.
    const rpc::Outcome next = call(*ch, "echo", json::Object{{"x", 1}});
    CHECK(next.ok());
    CHECK_EQ(next.result.find("x")->asInt(), 1);
    CHECK_EQ(ch->pendingRequests(), std::size_t(0));
}

TEST(channel_cancel_and_extend) {
    auto ch = startChild();
    std::optional<rpc::Outcome> r;
    const auto id = ch->request("slow", json::Object{{"ms", 500}}, 10s, [&](const rpc::Outcome& o) { r = o; });
    ch->cancel(id);
    CHECK(pollUntil(*ch, [&] { return r.has_value(); }));
    CHECK(r->status == rpc::Outcome::Status::Cancelled);

    r.reset();
    const auto id2 = ch->request("slow", json::Object{{"ms", 600}}, 300ms, [&](const rpc::Outcome& o) { r = o; });
    ch->extend(id2, 5s);  // e.g. the plugin reported progress
    CHECK(pollUntil(*ch, [&] { return r.has_value(); }));
    CHECK(r->ok());
}

TEST(channel_captures_stderr_with_a_cap) {
    rpc::ChannelLimits limits;
    limits.maxLogBytes = 100;
    auto ch = startChild({}, limits);
    CHECK(call(*ch, "stderr", json::Object{{"text", std::string(300, 'a') + "END"}}).ok());
    CHECK(pollUntil(*ch, [&] { return ch->log().find("END") != std::string::npos; }));
    CHECK(ch->log().size() <= 100);
}

// Every way a plugin can break the protocol closes the channel with a useful reason, and anything
// still waiting completes with Closed.
void expectViolation(const json::Value& rawParams, const std::string& method, const std::string& reasonFragment,
                     rpc::ChannelLimits limits = {}) {
    auto ch = startChild({}, limits);
    const rpc::Outcome o = call(*ch, method, rawParams);
    CHECK(o.status == rpc::Outcome::Status::Closed);
    CHECK(ch->closed());
    if (ch->closeReason().find(reasonFragment) == std::string::npos) {
        ++g_failures;
        std::cerr << "  close reason was: " << ch->closeReason() << "\n  expected: " << reasonFragment << "\n";
    }
    // Requests after closing complete with Closed too, never hang.
    const rpc::Outcome after = call(*ch, "echo");
    CHECK(after.status == rpc::Outcome::Status::Closed);
}

TEST(channel_closes_on_protocol_violations) {
    expectViolation(json::Object{{"line", "this is not json"}}, "raw", "invalid message");
    expectViolation(json::Object{{"line", "[1,2,3]"}}, "raw", "must be a JSON object");
    expectViolation(json::Object{{"line", R"({"jsonrpc":"1.0","id":1,"result":1})"}}, "raw", "\"jsonrpc\": \"2.0\"");
    expectViolation(json::Object{{"line", R"({"jsonrpc":"2.0","result":1})"}}, "raw", "must have an id");
    expectViolation(json::Object{{"line", R"({"jsonrpc":"2.0","id":"1","result":1})"}}, "raw", "integers");
    expectViolation(json::Object{{"line", R"({"jsonrpc":"2.0","id":1,"result":1,"error":{}})"}}, "raw", "exactly one");
    expectViolation(json::Object{{"line", R"({"jsonrpc":"2.0","id":1,"method":"x","params":5})"}}, "raw",
                    "\"params\" must be");
    // {"jsonrpc":"2.0","method":"<0xFF>"}
    expectViolation(json::Object{{"hex", "7b226a736f6e727063223a22322e30222c226d6574686f64223a22ff227d"}}, "rawhex",
                    "invalid UTF-8");
    rpc::ChannelLimits small;
    small.maxMessageBytes = 1 << 16;
    expectViolation(json::Object{{"bytes", 1 << 17}}, "huge", "larger than", small);
}

TEST(channel_closes_on_oversized_default_limit) {
    // The real 16 MiB limit, with a line that never ends before it.
    auto ch = startChild();
    const rpc::Outcome o = call(*ch, "huge", json::Object{{"bytes", (16 << 20) + 1024}});
    CHECK(o.status == rpc::Outcome::Status::Closed);
    CHECK(ch->closeReason().find("16 MiB") != std::string::npos);
}

TEST(channel_reports_exits_and_crashes) {
    {
        auto ch = startChild();
        const rpc::Outcome o = call(*ch, "exit", json::Object{{"code", 3}});
        CHECK(o.status == rpc::Outcome::Status::Closed);
        CHECK(ch->closeReason().find("exited (code 3)") != std::string::npos);
    }
    {
        auto ch = startChild();
        const rpc::Outcome o = call(*ch, "crash");
        CHECK(o.status == rpc::Outcome::Status::Closed);
        CHECK(ch->closeReason().find("exited") != std::string::npos);
    }
}

TEST(channel_close_kills_a_stuck_plugin_promptly) {
    auto ch = startChild();
    std::optional<rpc::Outcome> r;
    ch->request("stall", json::Object{}, 0ms, [&](const rpc::Outcome& o) { r = o; });  // no timeout
    ch->poll(100ms);
    const auto start = std::chrono::steady_clock::now();
    ch->close("the user pressed Cancel");
    CHECK(pollUntil(*ch, [&] { return r.has_value(); }));
    CHECK(r->status == rpc::Outcome::Status::Closed);
    CHECK_EQ(r->message, std::string("the user pressed Cancel"));
    ch.reset();  // destructor joins the I/O threads
    CHECK(std::chrono::steady_clock::now() - start < 3s);
}

TEST(channel_destructor_kills_a_plugin_that_ignores_stdin) {
    auto ch = startChild();
    std::optional<rpc::Outcome> r;
    ch->request("stall", json::Object{}, 0ms, [&](const rpc::Outcome& o) { r = o; });
    // Fill its stdin: the plugin isn't reading any more, so our writer thread blocks.
    for (int i = 0; i < 20; ++i) ch->notify("event/fileSaved", json::Object{{"pad", std::string(64 * 1024, 'p')}});
    const auto start = std::chrono::steady_clock::now();
    ch.reset();
    CHECK(std::chrono::steady_clock::now() - start < 3s);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0;
    for (const TestCase& t : registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
        const int before = g_failures;
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::cerr << t.name << ": unexpected exception: " << e.what() << "\n";
        }
        std::cout << (g_failures == before ? "  ok    " : "  FAIL  ") << t.name << "\n";
        ++ran;
    }
    std::cout << "\n" << ran << " tests, " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
