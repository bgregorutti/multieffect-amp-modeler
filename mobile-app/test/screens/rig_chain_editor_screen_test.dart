import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
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
    expect(chain.map((b) => b.id).toSet(), hasLength(5),
        reason: 'block ids must be unique within a chain');
  });

  testWidgets('pinning a block marks it always-on', (tester) async {
    final fakeClient = await pumpEditor(tester);

    // Index 2 is the distortion block, which starts unpinned. Block cards
    // are tall, so it sits below the test viewport until scrolled to.
    final pinSwitch = find.byKey(const Key('block-pinned-switch-2'));
    await tester.ensureVisible(pinSwitch);
    await tester.pumpAndSettle();
    await tester.tap(pinSwitch);
    await tester.pump();
    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    final chain = lastUpdate(fakeClient).chain!;
    expect(chain.firstWhere((b) => b.id == 'dist').pinned, isTrue);
    // Pinning must not disturb chain order.
    expect(chain.map((b) => b.id), ['amp', 'cab', 'dist', 'reverb']);
  });

  testWidgets('removing a block drops it from the saved chain', (tester) async {
    final fakeClient = await pumpEditor(tester);

    final removeButton = find.byKey(const Key('remove-block-button-3'));
    await tester.ensureVisible(removeButton);
    await tester.pumpAndSettle();
    await tester.tap(removeButton);
    await tester.pump();
    await tester.tap(find.byKey(const Key('save-rig-button')));
    await tester.pumpAndSettle();

    final chain = lastUpdate(fakeClient).chain!;
    expect(chain.map((b) => b.id), ['amp', 'cab', 'dist']);
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
}
