#include "test_framework.h"
#include "tests.h"

#include "core/json.h"

#include <string>

namespace anyadance::tests {

void TestJson() {
    // Round-trips and preserves unrelated keys while editing nested values, the
    // way registration edits openvrpaths.vrpath and steamvr.vrsettings.
    const std::string source =
        "{ \"config\" : [ \"C:\\\\Steam\\\\config\" ], \"external_drivers\" : null, "
        "\"steamvr\" : { \"installID\" : \"abc\", \"requireHmd\" : false }, \"version\" : 1 }";
    auto parsed = anyadance::json::Parse(source);
    EXPECT_TRUE(parsed.has_value());

    anyadance::json::Value root = *parsed;
    EXPECT_TRUE(root.IsObject());
    EXPECT_TRUE(root.Find("version") != nullptr && root.Find("version")->number == 1.0);

    // Add a driver path to a previously-null external_drivers.
    anyadance::json::Value drivers = anyadance::json::Value::Array();
    drivers.array.push_back(anyadance::json::Value::String("C:\\app\\anyadance"));
    root.Set("external_drivers", drivers);

    // Edit a nested object value without disturbing siblings.
    anyadance::json::Value* steamvr = root.Find("steamvr");
    EXPECT_TRUE(steamvr != nullptr);
    steamvr->Set("requireHmd", anyadance::json::Value::Bool(true));
    steamvr->Set("forcedDriver", anyadance::json::Value::String("anyadance"));

    auto reparsed = anyadance::json::Parse(anyadance::json::Serialize(root));
    EXPECT_TRUE(reparsed.has_value());
    const anyadance::json::Value* cfg = reparsed->Find("config");
    EXPECT_TRUE(cfg != nullptr && cfg->IsArray() && cfg->array.size() == 1);
    EXPECT_TRUE(cfg->array[0].string == "C:\\Steam\\config");
    const anyadance::json::Value* ed = reparsed->Find("external_drivers");
    EXPECT_TRUE(ed != nullptr && ed->IsArray() && ed->array.size() == 1);
    EXPECT_TRUE(ed->array[0].string == "C:\\app\\anyadance");
    const anyadance::json::Value* sv = reparsed->Find("steamvr");
    EXPECT_TRUE(sv != nullptr);
    EXPECT_TRUE(sv->Find("requireHmd") != nullptr && sv->Find("requireHmd")->boolean);
    EXPECT_TRUE(sv->Find("forcedDriver") != nullptr && sv->Find("forcedDriver")->string == "anyadance");
    EXPECT_TRUE(sv->Find("installID") != nullptr && sv->Find("installID")->string == "abc");

    EXPECT_TRUE(!anyadance::json::Parse("{ bad json ").has_value());
}

} // namespace anyadance::tests
