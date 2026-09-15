import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/asset.dart';
import 'package:mobile_app/models/block_param_descriptor.dart';
import 'package:mobile_app/models/daemon_state.dart';
import 'package:mobile_app/models/effect_block.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/preset_editor_screen.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  Future<FakeDaemonClient> pumpEditor(
    WidgetTester tester, {
    required dynamic preset,
  }) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);
    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(
        controller: controller,
        rig: rigSvt,
        preset: preset,
      ),
    ));
    return fakeClient;
  }

  testWidgets('shows pinned blocks read-only and effects as switches',
      (tester) async {
    await pumpEditor(tester, preset: presetClean);

    // Amp and cab are part of the rig, shared by every preset -- shown so
    // you can see what is sounding, but with no switch to turn them off.
    expect(find.byKey(const Key('pinned-block-amp')), findsOneWidget);
    expect(find.byKey(const Key('pinned-block-cab')), findsOneWidget);
    expect(find.byKey(const Key('preset-block-switch-amp')), findsNothing);
    expect(find.byKey(const Key('preset-block-switch-cab')), findsNothing);

    // The unpinned effects are what this preset actually controls.
    expect(find.byKey(const Key('preset-block-switch-dist')), findsOneWidget);
    expect(find.byKey(const Key('preset-block-switch-reverb')), findsOneWidget);
  });

  testWidgets('resolves each switch from the preset overrides', (tester) async {
    await pumpEditor(tester, preset: presetDrive);

    final dist = tester.widget<SwitchListTile>(
      find.byKey(const Key('preset-block-switch-dist')),
    );
    final reverb = tester.widget<SwitchListTile>(
      find.byKey(const Key('preset-block-switch-reverb')),
    );

    // "Drive" overrides dist on; reverb has no override so it falls back to
    // the block's own default, which is off.
    expect(dist.value, isTrue);
    expect(reverb.value, isFalse);
  });

  testWidgets('resolves the pinned amp asset to its filename', (tester) async {
    await pumpEditor(tester, preset: presetClean);
    expect(find.text('my_amp.nam'), findsOneWidget);
  });

  testWidgets('toggling an effect and saving sends update_preset',
      (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetClean);

    await tester.tap(find.byKey(const Key('preset-block-switch-dist')));
    await tester.pump();
    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    final cmd = fakeClient.sentCommands
        .firstWhere((c) => c.type == 'update_preset')
        .command as UpdatePresetCommand;

    expect(cmd.rigId, 'rig-1');
    expect(cmd.presetId, 'preset-a');
    expect(cmd.blockStates!['dist']!.enabled, isTrue);
    expect(cmd.blockStates!['reverb']!.enabled, isFalse);
  });

  testWidgets('never sends block_states for pinned blocks', (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetClean);

    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    final cmd = fakeClient.sentCommands
        .firstWhere((c) => c.type == 'update_preset')
        .command as UpdatePresetCommand;

    expect(cmd.blockStates!.containsKey('amp'), isFalse);
    expect(cmd.blockStates!.containsKey('cab'), isFalse);
  });

  testWidgets('renaming and saving sends the new name', (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetClean);

    await tester.enterText(
      find.byKey(const Key('preset-name-input')),
      'Clean v2',
    );
    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    final cmd = fakeClient.sentCommands
        .firstWhere((c) => c.type == 'update_preset')
        .command as UpdatePresetCommand;
    expect(cmd.name, 'Clean v2');
  });

  testWidgets(
      'renders a live slider from list_block_types for a native block, '
      'seeded at its default', (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    fakeClient.nextResult = {
      'block_types': [
        {
          'type': 'distortion',
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
      ],
    };
    final controller = DaemonStateController(fakeClient);
    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(
        controller: controller,
        rig: rigSvt,
        preset: presetClean,
      ),
    ));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('live-params-card-dist')), findsOneWidget);
    final sliderFinder = find.descendant(
      of: find.byKey(const Key('live-param-dist-gain_db')),
      matching: find.byType(Slider),
    );
    expect(sliderFinder, findsOneWidget);
    expect(tester.widget<Slider>(sliderFinder).value, 0.0);

    // A "reverb" block has no matching entry in list_block_types here, so
    // it gets no live-controls card at all -- not an empty one.
    expect(find.byKey(const Key('live-params-card-reverb')), findsNothing);
  });

  testWidgets('dragging a live slider sends set_block_param immediately',
      (tester) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    fakeClient.nextResult = {
      'block_types': [
        {
          'type': 'distortion',
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
      ],
    };
    final controller = DaemonStateController(fakeClient);
    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(
        controller: controller,
        rig: rigSvt,
        preset: presetClean,
      ),
    ));
    await tester.pumpAndSettle();

    final sliderFinder = find.descendant(
      of: find.byKey(const Key('live-param-dist-gain_db')),
      matching: find.byType(Slider),
    );
    // Driving onChanged directly (rather than a pixel drag) exercises the
    // same handler a real drag calls, without depending on exact slider
    // geometry in the test surface.
    tester.widget<Slider>(sliderFinder).onChanged!(12.0);
    await tester.pump();

    final sent = fakeClient.sentCommands
        .firstWhere((c) => c.type == 'set_block_param')
        .command as SetBlockParamCommand;
    expect(sent.rigId, 'rig-1');
    expect(sent.presetId, 'preset-a');
    expect(sent.blockId, 'dist');
    expect(sent.paramKey, 'gain_db');
    expect(sent.value, 12.0);

    // The dragged value is reflected locally right away, not just sent.
    expect(tester.widget<Slider>(sliderFinder).value, 12.0);
  });

  testWidgets("renders a vst3 block's live params from its own asset schema",
      (tester) async {
    const vst3Asset = Asset(
      id: 'plug-1',
      kind: AssetKind.vst3,
      filename: 'GainTest.vst3',
      storedPath: '/plugins/GainTest.vst3',
      uploadedAt: 0,
      parameters: [
        BlockParamDescriptor(
          key: '0',
          label: 'Gain',
          unit: 'x',
          min: 0.0,
          max: 2.0,
          defaultValue: 1.0,
        ),
      ],
    );
    final stateWithPlugin = DaemonState(
      rigs: sampleState.rigs,
      assets: {...sampleState.assets, 'plug-1': vst3Asset},
      footswitchMapping: sampleState.footswitchMapping,
      activeRigIndex: sampleState.activeRigIndex,
      activePresetIndex: sampleState.activePresetIndex,
      activeRigId: sampleState.activeRigId,
      activePresetId: sampleState.activePresetId,
      bypass: sampleState.bypass,
      tempoBpm: sampleState.tempoBpm,
    );
    final rigWithPlugin = rigSvt.copyWith(chain: [
      ...svtChain,
      const EffectBlock(id: 'fx1', type: 'vst3', assetId: 'plug-1'),
    ]);

    final fakeClient = FakeDaemonClient(state: stateWithPlugin);
    final controller = DaemonStateController(fakeClient);

    // The extra chain block pushes the "Live controls" section below the
    // default 800x600 test viewport, and ListView's sliver machinery only
    // builds on-screen children -- enlarge the surface rather than fight
    // scrolling (same fix used elsewhere in this app's widget tests).
    tester.view.physicalSize = const Size(800, 2400);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(
        controller: controller,
        rig: rigWithPlugin,
        preset: presetClean,
      ),
    ));
    await tester.pumpAndSettle();

    expect(find.byKey(const Key('live-params-card-fx1')), findsOneWidget);
    expect(find.byKey(const Key('live-param-fx1-0')), findsOneWidget);
  });
}
