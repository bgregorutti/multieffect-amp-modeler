import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/effect_block.dart';

void main() {
  test('round-trips every field through JSON', () {
    const json = {
      'id': 'amp',
      'type': 'nam',
      'asset_id': 'nam1',
      'pinned': true,
      'enabled': true,
      'params': {'gain': 4.2, 'label': 'hall', 'sync': false, 'count': 3},
    };

    final block = EffectBlock.fromJson(Map<String, dynamic>.from(json));

    expect(block.id, 'amp');
    expect(block.type, 'nam');
    expect(block.assetId, 'nam1');
    expect(block.pinned, isTrue);
    expect(block.params['gain'], 4.2);
    expect(block.params['label'], 'hall');
    expect(block.params['sync'], false);
    expect(block.params['count'], 3);
    expect(block.toJson(), json);
  });

  test('asset_id survives as null rather than being dropped', () {
    final block = EffectBlock.fromJson({
      'id': 'dist',
      'type': 'distortion',
      'asset_id': null,
      'enabled': false,
    });

    expect(block.assetId, isNull);
    expect(block.pinned, isFalse, reason: 'pinned defaults to false');
    expect(block.toJson()['asset_id'], isNull);
    expect(block.toJson().containsKey('asset_id'), isTrue);
  });

  test('defaults match the daemon: enabled true, pinned false, no params', () {
    final block = EffectBlock.fromJson({'id': 'x', 'type': 'reverb'});

    expect(block.enabled, isTrue);
    expect(block.pinned, isFalse);
    expect(block.params, isEmpty);
    expect(block.assetId, isNull);
  });

  test('copyWith can clear assetId via the sentinel closure', () {
    const block = EffectBlock(id: 'cab', type: 'ir', assetId: 'ir1');

    expect(block.copyWith().assetId, 'ir1', reason: 'omitted means unchanged');
    expect(block.copyWith(assetId: () => null).assetId, isNull);
    expect(block.copyWith(assetId: () => 'ir2').assetId, 'ir2');
  });

  test('copyWith preserves the id', () {
    const block = EffectBlock(id: 'amp', type: 'nam');
    expect(block.copyWith(type: 'other').id, 'amp');
  });
}
