import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/preset.dart';

import 'protocol_fixtures.dart';

void main() {
  group('Preset', () {
    test('parses the preset embedded in the full daemon state fixture', () {
      final presetsJson =
          fullDaemonStateFixture['presets'] as Map<String, dynamic>;
      final preset = Preset.fromJson(
        presetsJson['abc123'] as Map<String, dynamic>,
      );

      expect(preset.id, 'abc123');
      expect(preset.name, 'Ambient Swell');
      expect(preset.blocks, hasLength(1));
      expect(preset.blocks.first.type, 'reverb');
      expect(preset.namAssetId, 'nam1');
      expect(preset.irAssetId, isNull);
      expect(preset.createdAt, 1000.0);
      expect(preset.updatedAt, 1001.5);
    });

    test('round-trips through toJson/fromJson', () {
      const original = Preset(
        id: 'p1',
        name: 'My Preset',
        blocks: [],
        namAssetId: null,
        irAssetId: 'ir1',
        createdAt: 10.0,
        updatedAt: 20.0,
      );

      final roundTripped = Preset.fromJson(original.toJson());

      expect(roundTripped.id, original.id);
      expect(roundTripped.name, original.name);
      expect(roundTripped.blocks, isEmpty);
      expect(roundTripped.namAssetId, isNull);
      expect(roundTripped.irAssetId, 'ir1');
      expect(roundTripped.createdAt, original.createdAt);
      expect(roundTripped.updatedAt, original.updatedAt);
    });

    test('copyWith can explicitly clear an asset id', () {
      const original = Preset(
        id: 'p1',
        name: 'x',
        namAssetId: 'nam1',
        createdAt: 0,
        updatedAt: 0,
      );

      final cleared = original.copyWith(namAssetId: () => null);

      expect(cleared.namAssetId, isNull);
      // irAssetId untouched since its updater wasn't passed.
      expect(cleared.irAssetId, original.irAssetId);
    });
  });
}
