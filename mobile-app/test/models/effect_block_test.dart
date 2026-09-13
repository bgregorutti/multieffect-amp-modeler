import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/effect_block.dart';

import 'protocol_fixtures.dart';

void main() {
  group('EffectBlock', () {
    test('parses the create_preset README fixture block', () {
      final blockJson =
          (createPresetFixture['blocks'] as List).first as Map<String, dynamic>;
      final block = EffectBlock.fromJson(blockJson);

      expect(block.type, 'reverb');
      expect(block.enabled, true);
      expect(block.params, {'decay': 4.2});
    });

    test('round-trips through toJson/fromJson', () {
      const original = EffectBlock(
        type: 'delay',
        enabled: false,
        params: {'time_ms': 350, 'feedback': 0.4, 'sync': true, 'name': 'x'},
      );

      final roundTripped = EffectBlock.fromJson(original.toJson());

      expect(roundTripped.type, original.type);
      expect(roundTripped.enabled, original.enabled);
      expect(roundTripped.params, original.params);
    });

    test('defaults enabled=true and params={} when omitted', () {
      final block = EffectBlock.fromJson({'type': 'noop'});
      expect(block.enabled, true);
      expect(block.params, isEmpty);
    });

    test('copyWith overrides only the given fields', () {
      const original = EffectBlock(type: 'reverb', params: {'decay': 1.0});
      final updated = original.copyWith(enabled: false);

      expect(updated.type, 'reverb');
      expect(updated.enabled, false);
      expect(updated.params, {'decay': 1.0});
    });
  });
}
