import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/bank.dart';
import 'package:mobile_app/models/daemon_state.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/banks_screen.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  testWidgets('lists banks and their slot assignments', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: BanksScreen(controller: controller),
    ));

    expect(find.text('Live Set 1'), findsOneWidget);
    // Slot 0 holds preset-a ("Ambient Swell"), slot 1 is empty.
    expect(find.text('Ambient Swell'), findsOneWidget);
    expect(find.text('Slot 2: empty'), findsOneWidget);
  });

  testWidgets('creating a bank sends create_bank with the entered name',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: BanksScreen(controller: controller),
    ));

    await tester.tap(find.byKey(const Key('add-bank-button')));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('bank-name-field')),
      'Live Set 2',
    );
    await tester.tap(find.byKey(const Key('bank-name-save')));
    await tester.pumpAndSettle();

    final createCommands = fakeClient.sentCommands
        .where((c) => c.type == 'create_bank')
        .toList();
    expect(createCommands, hasLength(1));
    expect(
      (createCommands.single.command as CreateBankCommand).name,
      'Live Set 2',
    );
  });

  testWidgets('assigning a preset to an empty slot sends update_bank',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: BanksScreen(controller: controller),
    ));

    await tester.tap(find.byKey(const Key('bank-slot-bank-1-1')));
    await tester.pumpAndSettle();

    await tester.tap(find.byKey(const Key('slot-choice-preset-b')));
    await tester.pumpAndSettle();

    final updateCommands = fakeClient.sentCommands
        .where((c) => c.type == 'update_bank')
        .toList();
    expect(updateCommands, hasLength(1));
    final cmd = updateCommands.single.command as UpdateBankCommand;
    expect(cmd.bankId, 'bank-1');
    expect(cmd.slots, ['preset-a', 'preset-b']);
  });

  testWidgets('moving a bank down sends reorder_banks with the new order',
      (tester) async {
    const secondBank = Bank(id: 'bank-2', name: 'Live Set 2', slots: [null]);
    final twoBankState = DaemonState(
      presets: sampleState.presets,
      banks: const [bankOne, secondBank],
      assets: sampleState.assets,
      footswitchMapping: sampleState.footswitchMapping,
      activePresetId: sampleState.activePresetId,
    );
    final fakeClient = FakeDaemonClient(state: twoBankState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: BanksScreen(controller: controller),
    ));

    await tester.tap(find.byKey(const Key('move-bank-down-bank-1')));
    await tester.pumpAndSettle();

    final reorderCommands = fakeClient.sentCommands
        .where((c) => c.type == 'reorder_banks')
        .toList();
    expect(reorderCommands, hasLength(1));
    expect(
      (reorderCommands.single.command as ReorderBanksCommand).bankIds,
      ['bank-2', 'bank-1'],
    );
  });

  testWidgets('moving the first bank up is a no-op (nothing sent)',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);

    await tester.pumpWidget(MaterialApp(
      home: BanksScreen(controller: controller),
    ));

    await tester.tap(find.byKey(const Key('move-bank-up-bank-1')));
    await tester.pumpAndSettle();

    expect(
      fakeClient.sentCommands.where((c) => c.type == 'reorder_banks'),
      isEmpty,
    );
  });
}
