import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/effect_block.dart';
import 'package:mobile_app/models/preset.dart';

void main() {
  group('PresetBlockState', () {
    test('round-trips through JSON', () {
      const json = {
        'enabled': false,
        'params': {'gain': 9.0},
      };
      final state = PresetBlockState.fromJson(Map<String, dynamic>.from(json));

      expect(state.enabled, isFalse);
      expect(state.params['gain'], 9.0);
      expect(state.toJson(), json);
    });

    test('defaults to enabled with no param overrides', () {
      final state = PresetBlockState.fromJson({});
      expect(state.enabled, isTrue);
      expect(state.params, isEmpty);
    });
  });

  group('Preset', () {
    test('round-trips block_states through JSON', () {
      const json = {
        'id': 'p1',
        'name': 'Drive',
        'block_states': {
          'dist': {'enabled': true, 'params': {}},
          'fuzz': {'enabled': false, 'params': {}},
        },
        'created_at': 1000.0,
        'updated_at': 1001.5,
      };

      final preset = Preset.fromJson(Map<String, dynamic>.from(json));

      expect(preset.id, 'p1');
      expect(preset.name, 'Drive');
      expect(preset.blockStates['dist']!.enabled, isTrue);
      expect(preset.blockStates['fuzz']!.enabled, isFalse);
      expect(preset.toJson(), json);
    });

    test('a preset with no overrides round-trips as an empty map', () {
      final preset = Preset.fromJson({
        'id': 'p1',
        'name': 'Clean',
        'created_at': 0.0,
        'updated_at': 0.0,
      });

      expect(preset.blockStates, isEmpty);
      expect(preset.toJson()['block_states'], isEmpty);
    });
  });

  group('isBlockEnabled mirrors the daemon resolve rules', () {
    const pinnedAmp = EffectBlock(id: 'amp', type: 'nam', pinned: true);
    const dist = EffectBlock(id: 'dist', type: 'distortion', enabled: false);
    const reverb = EffectBlock(id: 'reverb', type: 'reverb', enabled: true);

    test('pinned blocks are always on, even if a preset says otherwise', () {
      const preset = Preset(
        id: 'p',
        name: 'Broken',
        blockStates: {'amp': PresetBlockState(enabled: false)},
        createdAt: 0,
        updatedAt: 0,
      );
      expect(preset.isBlockEnabled(pinnedAmp), isTrue);
    });

    test('an override wins over the block default', () {
      const preset = Preset(
        id: 'p',
        name: 'Drive',
        blockStates: {'dist': PresetBlockState(enabled: true)},
        createdAt: 0,
        updatedAt: 0,
      );
      expect(preset.isBlockEnabled(dist), isTrue);
    });

    test('with no override the block default applies', () {
      const preset = Preset(id: 'p', name: 'Clean', createdAt: 0, updatedAt: 0);
      expect(preset.isBlockEnabled(dist), isFalse);
      expect(preset.isBlockEnabled(reverb), isTrue);
    });
  });
}
