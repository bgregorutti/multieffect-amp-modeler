#include "audio_engine/preset_model.hpp"

#include <gtest/gtest.h>

using namespace audio_engine;

namespace {

// Shaped exactly like a real control-daemon ResolvedPreset as sent over
// the control socket (see control-daemon/src/control_daemon/models.py:
// ResolvedPreset, ResolvedBlock) -- field names/types/optionality
// intentionally match so this can deserialize the daemon's own JSON with
// no translation layer. Note there is no preset-level nam/ir asset id and
// no timestamps: assets hang off individual blocks, and the amp ("nam")
// and cab ("ir") are ordinary, explicitly ordered chain positions.
constexpr const char* kFixtureJson = R"JSON(
{
  "id": "abc123def456",
  "name": "Ambient Swell",
  "rig_id": "rig-svt",
  "rig_name": "Ampeg SVT",
  "blocks": [
    {"id": "amp", "type": "nam", "asset_id": "nam001", "enabled": true, "params": {}},
    {"id": "cab", "type": "ir", "asset_id": "ir002", "enabled": true, "params": {}},
    {"id": "boost", "type": "gain", "asset_id": null, "enabled": true, "params": {"gain_db": 3.5}},
    {"id": "tone", "type": "eq", "asset_id": null, "enabled": false, "params": {"freq_hz": 800.0, "gain_db": -4.0, "q": 1.2, "filter_type": "peaking"}},
    {"id": "echo", "type": "delay", "asset_id": null, "enabled": true, "params": {"delay_ms": 350.0, "feedback": 0.35, "mix": 0.4}}
  ]
}
)JSON";

}  // namespace

TEST(PresetModel, RoundTripsRealisticFixture) {
    Preset preset = parsePresetJson(kFixtureJson);

    EXPECT_EQ(preset.id, "abc123def456");
    EXPECT_EQ(preset.name, "Ambient Swell");
    EXPECT_EQ(preset.rig_id, "rig-svt");
    EXPECT_EQ(preset.rig_name, "Ampeg SVT");
    ASSERT_EQ(preset.blocks.size(), 5u);

    // Chain order is the wire order, amp and cab included -- it is the
    // signal path, so it must survive parsing untouched.
    EXPECT_EQ(preset.blocks[0].id, "amp");
    EXPECT_EQ(preset.blocks[0].type, "nam");
    ASSERT_TRUE(preset.blocks[0].asset_id.has_value());
    EXPECT_EQ(*preset.blocks[0].asset_id, "nam001");

    EXPECT_EQ(preset.blocks[1].id, "cab");
    EXPECT_EQ(preset.blocks[1].type, "ir");
    ASSERT_TRUE(preset.blocks[1].asset_id.has_value());
    EXPECT_EQ(*preset.blocks[1].asset_id, "ir002");

    EXPECT_EQ(preset.blocks[2].id, "boost");
    EXPECT_EQ(preset.blocks[2].type, "gain");
    EXPECT_FALSE(preset.blocks[2].asset_id.has_value());
    EXPECT_TRUE(preset.blocks[2].enabled);
    ASSERT_TRUE(preset.blocks[2].params.count("gain_db"));
    EXPECT_DOUBLE_EQ(std::get<double>(preset.blocks[2].params.at("gain_db")), 3.5);

    EXPECT_EQ(preset.blocks[3].type, "eq");
    EXPECT_FALSE(preset.blocks[3].enabled);
    EXPECT_EQ(std::get<std::string>(preset.blocks[3].params.at("filter_type")), "peaking");

    EXPECT_EQ(preset.blocks[4].id, "echo");

    // Round trip: serialize back out and re-parse, expect the same logical
    // content (field-by-field, since map/vector ordering of the
    // serialization is not guaranteed to be byte-identical text).
    std::string serialized = serializePresetJson(preset);
    Preset reparsed = parsePresetJson(serialized);
    EXPECT_EQ(reparsed.id, preset.id);
    EXPECT_EQ(reparsed.name, preset.name);
    EXPECT_EQ(reparsed.rig_id, preset.rig_id);
    EXPECT_EQ(reparsed.rig_name, preset.rig_name);
    EXPECT_EQ(reparsed.blocks, preset.blocks);  // compares ids, asset_ids and order
}

TEST(PresetModel, NullBlockAssetIdRoundTrips) {
    // An explicitly-null asset_id and an omitted one must both parse as
    // "no asset", and must serialize back out as JSON null (not as a
    // missing key, and never as the string "null") so the daemon sees the
    // same shape it sent.
    constexpr const char* json = R"JSON(
    {"id": "p1", "name": "Clean", "rig_id": "r1", "rig_name": "Rig One",
     "blocks": [
       {"id": "boost", "type": "gain", "asset_id": null, "enabled": true, "params": {}},
       {"id": "echo", "type": "delay", "enabled": true, "params": {}}
     ]}
    )JSON";
    Preset preset = parsePresetJson(json);
    ASSERT_EQ(preset.blocks.size(), 2u);
    EXPECT_FALSE(preset.blocks[0].asset_id.has_value()) << "explicit null should mean unset";
    EXPECT_FALSE(preset.blocks[1].asset_id.has_value()) << "absent key should mean unset";

    nlohmann::json out = nlohmann::json::parse(serializePresetJson(preset));
    EXPECT_TRUE(out["blocks"][0]["asset_id"].is_null());
    EXPECT_TRUE(out["blocks"][1]["asset_id"].is_null());

    Preset reparsed = parsePresetJson(serializePresetJson(preset));
    EXPECT_EQ(reparsed.blocks, preset.blocks);
}

TEST(PresetModel, BlockAssetIdOfWrongTypeThrows) {
    constexpr const char* json = R"JSON(
    {"id": "p1", "name": "Clean", "rig_id": "r1", "rig_name": "Rig One",
     "blocks": [{"id": "amp", "type": "nam", "asset_id": 7, "enabled": true, "params": {}}]}
    )JSON";
    EXPECT_THROW(parsePresetJson(json), PresetParseError);
}

TEST(PresetModel, EmptyBlockListRoundTrips) {
    constexpr const char* json = R"JSON(
    {"id": "p1", "name": "Clean", "rig_id": "r1", "rig_name": "Rig One", "blocks": []}
    )JSON";
    Preset preset = parsePresetJson(json);
    EXPECT_EQ(preset.rig_id, "r1");
    EXPECT_TRUE(preset.blocks.empty());
}

TEST(PresetModel, MissingRequiredFieldThrows) {
    constexpr const char* missingName = R"JSON({"id": "p1", "blocks": []})JSON";
    EXPECT_THROW(parsePresetJson(missingName), PresetParseError);

    // rig_id/rig_name are required in the daemon's ResolvedPreset.
    constexpr const char* missingRig = R"JSON({"id": "p1", "name": "x", "blocks": []})JSON";
    EXPECT_THROW(parsePresetJson(missingRig), PresetParseError);
}

TEST(PresetModel, BlockMissingIdThrows) {
    constexpr const char* json = R"JSON(
    {"id": "p1", "name": "x", "rig_id": "r1", "rig_name": "Rig One",
     "blocks": [{"type": "gain", "enabled": true, "params": {}}]}
    )JSON";
    EXPECT_THROW(parsePresetJson(json), PresetParseError);
}

TEST(PresetModel, MalformedJsonThrows) {
    EXPECT_THROW(parsePresetJson("{not valid json"), PresetParseError);
}

TEST(PresetModel, BlockWithBadParamTypeThrows) {
    constexpr const char* json = R"JSON(
    {"id": "p1", "name": "x", "rig_id": "r1", "rig_name": "Rig One",
     "blocks": [{"id": "boost", "type": "gain", "params": {"gain_db": [1,2,3]}}]}
    )JSON";
    EXPECT_THROW(parsePresetJson(json), PresetParseError);
}

TEST(PresetModel, AssetRoundTrip) {
    Asset a;
    a.id = "asset1";
    a.kind = AssetKind::Ir;
    a.filename = "cab.wav";
    a.stored_path = "/data/assets/cab.wav";
    a.size_bytes = 12345;
    a.sha256 = "deadbeef";
    a.uploaded_at = 42.0;

    nlohmann::json j = a;
    Asset back = j.get<Asset>();
    EXPECT_EQ(back.id, a.id);
    EXPECT_EQ(back.kind, AssetKind::Ir);
    EXPECT_EQ(back.filename, a.filename);
    EXPECT_EQ(back.stored_path, a.stored_path);
    EXPECT_EQ(back.size_bytes, a.size_bytes);
    ASSERT_TRUE(back.sha256.has_value());
    EXPECT_EQ(*back.sha256, "deadbeef");
}

TEST(PresetModel, AssetKindRejectsUnknownString) {
    EXPECT_THROW(assetKindFromString("bogus"), PresetParseError);
}
