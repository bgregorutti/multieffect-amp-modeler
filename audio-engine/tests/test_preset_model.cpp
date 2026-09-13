#include "audio_engine/preset_model.hpp"

#include <gtest/gtest.h>

using namespace audio_engine;

namespace {

// Shaped exactly like a real control-daemon Preset export (see
// control-daemon/src/control_daemon/models.py: Preset, EffectBlock) --
// field names/types/optionality intentionally match so this can
// deserialize the daemon's own JSON with no translation layer.
constexpr const char* kFixtureJson = R"JSON(
{
  "id": "abc123def456",
  "name": "Ambient Swell",
  "blocks": [
    {"type": "gain", "enabled": true, "params": {"gain_db": 3.5}},
    {"type": "eq", "enabled": false, "params": {"freq_hz": 800.0, "gain_db": -4.0, "q": 1.2, "filter_type": "peaking"}},
    {"type": "delay", "enabled": true, "params": {"delay_ms": 350.0, "feedback": 0.35, "mix": 0.4}}
  ],
  "nam_asset_id": "nam001",
  "ir_asset_id": "ir002",
  "created_at": 1700000000.0,
  "updated_at": 1700000123.0
}
)JSON";

}  // namespace

TEST(PresetModel, RoundTripsRealisticFixture) {
    Preset preset = parsePresetJson(kFixtureJson);

    EXPECT_EQ(preset.id, "abc123def456");
    EXPECT_EQ(preset.name, "Ambient Swell");
    ASSERT_EQ(preset.blocks.size(), 3u);

    EXPECT_EQ(preset.blocks[0].type, "gain");
    EXPECT_TRUE(preset.blocks[0].enabled);
    ASSERT_TRUE(preset.blocks[0].params.count("gain_db"));
    EXPECT_DOUBLE_EQ(std::get<double>(preset.blocks[0].params.at("gain_db")), 3.5);

    EXPECT_EQ(preset.blocks[1].type, "eq");
    EXPECT_FALSE(preset.blocks[1].enabled);
    EXPECT_EQ(std::get<std::string>(preset.blocks[1].params.at("filter_type")), "peaking");

    ASSERT_TRUE(preset.nam_asset_id.has_value());
    EXPECT_EQ(*preset.nam_asset_id, "nam001");
    ASSERT_TRUE(preset.ir_asset_id.has_value());
    EXPECT_EQ(*preset.ir_asset_id, "ir002");
    EXPECT_DOUBLE_EQ(preset.created_at, 1700000000.0);
    EXPECT_DOUBLE_EQ(preset.updated_at, 1700000123.0);

    // Round trip: serialize back out and re-parse, expect the same logical
    // content (field-by-field, since map/vector ordering of the
    // serialization is not guaranteed to be byte-identical text).
    std::string serialized = serializePresetJson(preset);
    Preset reparsed = parsePresetJson(serialized);
    EXPECT_EQ(reparsed.id, preset.id);
    EXPECT_EQ(reparsed.name, preset.name);
    EXPECT_EQ(reparsed.blocks, preset.blocks);
    EXPECT_EQ(reparsed.nam_asset_id, preset.nam_asset_id);
    EXPECT_EQ(reparsed.ir_asset_id, preset.ir_asset_id);
}

TEST(PresetModel, NullAssetIdsRoundTrip) {
    constexpr const char* json = R"JSON(
    {"id": "p1", "name": "Clean", "blocks": [], "nam_asset_id": null, "ir_asset_id": null}
    )JSON";
    Preset preset = parsePresetJson(json);
    EXPECT_FALSE(preset.nam_asset_id.has_value());
    EXPECT_FALSE(preset.ir_asset_id.has_value());
    EXPECT_TRUE(preset.blocks.empty());
}

TEST(PresetModel, MissingRequiredFieldThrows) {
    constexpr const char* missingName = R"JSON({"id": "p1", "blocks": []})JSON";
    EXPECT_THROW(parsePresetJson(missingName), PresetParseError);
}

TEST(PresetModel, MalformedJsonThrows) {
    EXPECT_THROW(parsePresetJson("{not valid json"), PresetParseError);
}

TEST(PresetModel, BlockWithBadParamTypeThrows) {
    constexpr const char* json = R"JSON(
    {"id": "p1", "name": "x", "blocks": [{"type": "gain", "params": {"gain_db": [1,2,3]}}]}
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
