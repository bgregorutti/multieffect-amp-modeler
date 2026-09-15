import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/asset.dart';
import 'package:mobile_app/models/effect_block.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/rig_chain_editor_screen.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  Future<FakeDaemonClient> pumpEditor(WidgetTester tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);
    await tester.pumpWidget(MaterialApp(
      home: RigChainEditorScreen(controller: controller, rig: rigSvt),
    ));
    return fakeClient;
  }

  UpdateRigCommand lastUpdate(FakeDaemonClient client) =>
      client.sentCommands.firstWhere((c) => c.type == 'update_rig').command
          as UpdateRigCommand;

  testWidgets('saving sends the chain in its displayed order', (tester) async {
    final fakeClient = await pumpEditor(tester);

    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    final cmd = lastUpdate(fakeClient);
    expect(cmd.rigId, 'rig-1');
    // Chain order is the signal path, so it must survive a round trip
    // through the editor untouched.
    expect(cmd.chain!.map((b) => b.id), ['amp', 'cab', 'dist', 'reverb']);
  });

  testWidgets('preserves each block asset_id and pinned flag', (tester) async {
    final fakeClient = await pumpEditor(tester);

    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    final chain = lastUpdate(fakeClient).chain!;
    final amp = chain.firstWhere((b) => b.id == 'amp');
    final dist = chain.firstWhere((b) => b.id == 'dist');

    expect(amp.assetId, 'nam-1');
    expect(amp.pinned, isTrue);
    expect(dist.assetId, isNull);
    expect(dist.pinned, isFalse);
  });

  testWidgets('adding a block gives it an id so presets can reference it',
      (tester) async {
    final fakeClient = await pumpEditor(tester);

    await tester.tap(find.byKey(const Key('add-block-button')));
    await tester.pump();
    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    final chain = lastUpdate(fakeClient).chain!;
    expect(chain, hasLength(5));
    expect(chain.last.id, isNotEmpty);
    expect(chain.last.pinned, isTrue,
        reason: 'a block added here is part of the backline');
    expect(chain.map((b) => b.id).toSet(), hasLength(5),
        reason: 'block ids must be unique within a chain');
  });

  testWidgets(
      'effects are not shown here, but are kept in place when saving',
      (tester) async {
    final fakeClient = await pumpEditor(tester);

    // Only the backline (amp, cab) gets an editor card; dist and reverb
    // are managed from presets.
    expect(find.byKey(const Key('block-card-0')), findsOneWidget);
    expect(find.byKey(const Key('block-card-1')), findsOneWidget);
    expect(find.byKey(const Key('block-card-2')), findsNothing);

    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    final chain = lastUpdate(fakeClient).chain!;
    expect(chain.map((b) => b.id), ['amp', 'cab', 'dist', 'reverb']);
  });

  testWidgets('removing a block drops it from the saved chain', (tester) async {
    final fakeClient = await pumpEditor(tester);

    final removeButton = find.byKey(const Key('remove-block-button-1'));
    await tester.ensureVisible(removeButton);
    await tester.pumpAndSettle();
    await tester.tap(removeButton);
    await tester.pump();
    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    final chain = lastUpdate(fakeClient).chain!;
    expect(chain.map((b) => b.id), ['amp', 'dist', 'reverb']);
  });

  testWidgets('renaming the rig sends the new name', (tester) async {
    final fakeClient = await pumpEditor(tester);

    await tester.enterText(
      find.byKey(const Key('rig-name-input')),
      'Ampeg SVT II',
    );
    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    expect(lastUpdate(fakeClient).name, 'Ampeg SVT II');
  });

  const blockTypesResult = {
    'block_types': [
      {
        'type': 'gain',
        'parameters': [
          {
            'key': 'gain_db',
            'label': 'Gain',
            'unit': 'dB',
            'min': -60.0,
            'max': 24.0,
            'default': 0.0,
            'step_count': 0,
          },
        ],
      },
      {'type': 'delay', 'parameters': <Map<String, dynamic>>[]},
    ],
  };

  Future<FakeDaemonClient> pumpEditorWithBlockTypes(WidgetTester tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    fakeClient.nextResult = blockTypesResult;
    final controller = DaemonStateController(fakeClient);
    await tester.pumpWidget(MaterialApp(
      home: RigChainEditorScreen(controller: controller, rig: rigSvt),
    ));
    await tester.pumpAndSettle();
    return fakeClient;
  }

  /// Backline stages are filled/tappable cards; the type/asset/params
  /// fields (`block-type-field-*`, `block-asset-dropdown-*`, etc.) only
  /// exist once a tap on the card opens its editor sheet.
  Future<void> openStageEditor(WidgetTester tester, int index) async {
    await tester.tap(find.byKey(Key('block-card-$index')));
    await tester.pumpAndSettle();
  }

  testWidgets(
      'changing a block\'s type fills in its schema defaults and clears its '
      'asset', (tester) async {
    final fakeClient = await pumpEditorWithBlockTypes(tester);

    // "amp" (index 0) is a nam block with an asset -- switching it to
    // "gain" must drop the asset and pick up gain's schema default. It
    // already has an asset, so tapping it opens the full editor directly
    // (not the empty-stage asset picker).
    await openStageEditor(tester, 0);
    await tester.tap(find.byKey(const Key('block-type-field-0')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('gain').last);
    await tester.pumpAndSettle();

    // Dismiss the editor sheet (tap its scrim) before reaching the AppBar's
    // save button underneath it.
    await tester.tapAt(const Offset(10, 10));
    await tester.pumpAndSettle();

    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    final amp = lastUpdate(fakeClient).chain!.firstWhere((b) => b.id == 'amp');
    expect(amp.type, 'gain');
    expect(amp.assetId, isNull);
    expect(amp.params['gain_db'], 0.0);
    expect(amp.pinned, isTrue);
  });

  testWidgets('the type list offers backline types only, not effects',
      (tester) async {
    await pumpEditorWithBlockTypes(tester);

    await openStageEditor(tester, 0);
    await tester.tap(find.byKey(const Key('block-type-field-0')));
    await tester.pumpAndSettle();

    expect(find.text('tone_stack'), findsWidgets);
    expect(find.text('delay'), findsNothing);
    expect(find.text('vst3'), findsNothing);
  });

  test(
      'assetKindForBlockType maps each asset-backed type and nothing else '
      '(this is what keeps an "ir" block\'s asset picker from offering a '
      '.nam file -- exactly the mismatch that made the engine reject a '
      'preset with "missing \'RIFF\' chunk id")', () {
    expect(assetKindForBlockType('nam'), AssetKind.nam);
    expect(assetKindForBlockType('ir'), AssetKind.ir);
    expect(assetKindForBlockType('vst3'), AssetKind.vst3);
    expect(assetKindForBlockType('gain'), isNull);
    expect(assetKindForBlockType('volume'), isNull);
    expect(assetKindForBlockType('eq'), isNull);
    expect(assetKindForBlockType('tone_stack'), isNull);
    expect(assetKindForBlockType('delay'), isNull);
    expect(assetKindForBlockType('reverb'), isNull);
  });

  testWidgets('an unrecognized type still uses the generic key/value editor',
      (tester) async {
    final controller = DaemonStateController(FakeDaemonClient(state: sampleState));
    await tester.pumpWidget(MaterialApp(
      home: RigChainEditorScreen(
        controller: controller,
        rig: rigSvt.copyWith(chain: const [
          EffectBlock(
              id: 'odd', type: 'reverb', pinned: true, params: {'decay': 4.2}),
        ]),
      ),
    ));
    await tester.pumpAndSettle();

    // A backline block whose type has no known schema must still be
    // editable via the fallback key/value editor, reached by tapping its
    // stage card open (it needs no asset, so this opens the full editor
    // directly, not a picker).
    await openStageEditor(tester, 0);
    final paramRow = find.byKey(const Key('param-row-decay'));
    await tester.ensureVisible(paramRow);
    await tester.pumpAndSettle();
    expect(paramRow, findsOneWidget);
  });

  testWidgets(
      'tapping an empty stage opens a picker; selecting an asset assigns it',
      (tester) async {
    // "amp" here has no asset yet -- an empty stage -- while "cab" already
    // has one, so this exercises the empty-stage picker path specifically
    // (a filled stage instead opens the full editor -- see the "changing a
    // block's type" test above).
    final rig = rigSvt.copyWith(chain: const [
      EffectBlock(id: 'amp', type: 'nam', pinned: true),
      EffectBlock(id: 'cab', type: 'ir', assetId: 'ir-1', pinned: true),
    ]);
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);
    await tester.pumpWidget(MaterialApp(
      home: RigChainEditorScreen(controller: controller, rig: rig),
    ));
    await tester.pumpAndSettle();

    await tester.tap(find.byKey(const Key('block-card-0')));
    await tester.pumpAndSettle();

    // The picker only offers assets of the matching kind (nam, here).
    expect(find.byKey(const Key('asset-picker-option-nam-1')), findsOneWidget);
    await tester.tap(find.byKey(const Key('asset-picker-option-nam-1')));
    await tester.pumpAndSettle();

    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    final amp =
        lastUpdate(fakeClient).chain!.firstWhere((b) => b.id == 'amp');
    expect(amp.assetId, 'nam-1');
  });
}
