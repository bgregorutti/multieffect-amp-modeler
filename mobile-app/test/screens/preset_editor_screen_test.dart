import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/models/asset.dart';
import 'package:mobile_app/models/block_param_descriptor.dart';
import 'package:mobile_app/models/daemon_state.dart';
import 'package:mobile_app/models/effect_block.dart';
import 'package:mobile_app/models/preset.dart';
import 'package:mobile_app/models/ws_messages.dart';
import 'package:mobile_app/screens/preset_editor_screen.dart';
import 'package:mobile_app/state/daemon_state_controller.dart';
import 'package:mobile_app/widgets/chain_stage_card.dart';

import 'fake_daemon_client.dart';
import 'test_fixtures.dart';

void main() {
  Future<FakeDaemonClient> pumpEditor(
    WidgetTester tester, {
    required dynamic preset,
  }) async {
    final fakeClient = FakeDaemonClient(state: sampleState);
    final controller = DaemonStateController(fakeClient);
    // Live controls sit below the default 800x600 viewport, and ListView
    // only builds on-screen children.
    tester.view.physicalSize = const Size(800, 2400);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
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

  testWidgets(
      'pinned blocks render as locked, non-tappable chain-stage cards',
      (tester) async {
    await pumpEditor(tester, preset: presetClean);

    final ampCard =
        tester.widget<ChainStageCard>(find.byKey(const Key('pinned-block-amp')));
    expect(ampCard.isLocked, isTrue);
    expect(ampCard.onTap, isNull,
        reason: 'a pinned block is never editable from the preset screen');

    final effectCard = tester.widget<ChainStageCard>(
      find.byKey(const Key('switchable-block-dist')),
    );
    expect(effectCard.isLocked, isFalse);
  });

  UpdatePresetCommand lastPresetUpdate(FakeDaemonClient client) =>
      client.sentCommands.lastWhere((c) => c.type == 'update_preset').command
          as UpdatePresetCommand;

  testWidgets('toggling an effect sends update_preset immediately',
      (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetClean);

    await tester.tap(find.byKey(const Key('preset-block-switch-dist')));
    await tester.pump();

    final cmd = lastPresetUpdate(fakeClient);
    expect(cmd.rigId, 'rig-1');
    expect(cmd.presetId, 'preset-a');
    expect(cmd.blockStates!['dist']!.enabled, isTrue);
    expect(cmd.blockStates!['reverb']!.enabled, isFalse);
  });

  testWidgets('never sends block_states for pinned blocks', (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetClean);

    await tester.tap(find.byKey(const Key('preset-block-switch-dist')));
    await tester.pump();

    final cmd = lastPresetUpdate(fakeClient);
    expect(cmd.blockStates!.containsKey('amp'), isFalse);
    expect(cmd.blockStates!.containsKey('cab'), isFalse);
  });

  testWidgets(
      'toggling keeps the preset\'s existing param overrides (update_preset '
      'replaces block_states wholesale)', (tester) async {
    const presetTweaked = Preset(
      id: 'preset-a',
      name: 'Clean',
      blockStates: {
        'dist': PresetBlockState(enabled: true, params: {'gain_db': 6.0}),
      },
      createdAt: 0,
      updatedAt: 0,
    );
    final rig = rigSvt.copyWith(presets: const [presetTweaked, presetDrive]);
    final fakeClient = FakeDaemonClient(
      state: DaemonState(rigs: [rig, rigOrange], assets: sampleState.assets),
    );
    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(
        controller: DaemonStateController(fakeClient),
        rig: rig,
        preset: presetTweaked,
      ),
    ));

    await tester.tap(find.byKey(const Key('preset-block-switch-reverb')));
    await tester.pump();

    final cmd = lastPresetUpdate(fakeClient);
    expect(cmd.blockStates!['reverb']!.enabled, isTrue);
    expect(cmd.blockStates!['dist']!.params['gain_db'], 6.0);
  });

  testWidgets(
      'saving only renames -- it never sends block_states, which would wipe '
      'live param overrides', (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetDrive);

    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    final cmd = lastPresetUpdate(fakeClient);
    expect(cmd.name, 'Drive');
    expect(cmd.blockStates, isNull);
  });

  testWidgets(
      'the board is fixed here: no way to add or remove an effect',
      (tester) async {
    // The rig is the pedalboard and the preset only stomps its pedals, so
    // this screen deliberately offers neither. Both live in
    // RigChainEditorScreen -- see its own test for them.
    await pumpEditor(tester, preset: presetClean);

    expect(find.byKey(const Key('add-effect-button')), findsNothing);
    expect(find.byKey(const Key('remove-effect-dist')), findsNothing);
    expect(find.byKey(const Key('remove-effect-reverb')), findsNothing);

    // What it does offer is a switch per effect.
    expect(find.byKey(const Key('preset-block-switch-dist')), findsOneWidget);
    expect(find.byKey(const Key('preset-block-switch-reverb')), findsOneWidget);
  });

  testWidgets('editing a preset never changes the rig chain', (tester) async {
    final fakeClient = await pumpEditor(tester, preset: presetClean);

    await tester.tap(find.byKey(const Key('preset-block-switch-dist')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('save-preset-button')));
    await tester.pumpAndSettle();

    expect(
      fakeClient.sentCommands.where((c) => c.type == 'update_rig'),
      isEmpty,
      reason: 'a preset edit must not touch the shared board',
    );
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
    // Live controls sit below the default 800x600 viewport, and ListView
    // only builds on-screen children.
    tester.view.physicalSize = const Size(800, 2400);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(
        controller: controller,
        rig: rigSvt,
        // dist is on in "Drive" -- effects that are off get no controls.
        preset: presetDrive,
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

    // "Drive" only overrides `dist`'s enabled flag, not its gain_db param --
    // this is the block's own default, so no override dot.
    expect(
      find.descendant(
        of: find.byKey(const Key('live-param-dist-gain_db')),
        matching: find.byKey(const Key('live-param-override-dot')),
      ),
      findsNothing,
    );

    // A "reverb" block has no matching entry in list_block_types here, so
    // it gets no live-controls card at all -- not an empty one.
    expect(find.byKey(const Key('live-params-card-reverb')), findsNothing);
  });

  testWidgets(
      'shows an override dot only for a parameter this preset actually '
      'overrides', (tester) async {
    const presetWithOverride = Preset(
      id: 'preset-a',
      name: 'Clean',
      blockStates: {
        'dist': PresetBlockState(enabled: true, params: {'gain_db': 9.0}),
      },
      createdAt: 0,
      updatedAt: 0,
    );
    final rig = rigSvt.copyWith(presets: const [presetWithOverride, presetDrive]);
    final fakeClient = FakeDaemonClient(
      state: DaemonState(rigs: [rig, rigOrange], assets: sampleState.assets),
    );
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
    tester.view.physicalSize = const Size(800, 2400);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(
        controller: controller,
        rig: rig,
        preset: presetWithOverride,
      ),
    ));
    await tester.pumpAndSettle();

    final dot = find.descendant(
      of: find.byKey(const Key('live-param-dist-gain_db')),
      matching: find.byKey(const Key('live-param-override-dot')),
    );
    expect(dot, findsOneWidget);

    final slider = tester.widget<Slider>(find.descendant(
      of: find.byKey(const Key('live-param-dist-gain_db')),
      matching: find.byType(Slider),
    ));
    expect(slider.value, 9.0);
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
    // Live controls sit below the default 800x600 viewport, and ListView
    // only builds on-screen children.
    tester.view.physicalSize = const Size(800, 2400);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(MaterialApp(
      home: PresetEditorScreen(
        controller: controller,
        rig: rigSvt,
        preset: presetDrive,
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
    expect(sent.presetId, 'preset-b');
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
    final rigWithPlugin = rigSvt.copyWith(chain: [
      ...svtChain,
      const EffectBlock(id: 'fx1', type: 'vst3', assetId: 'plug-1'),
    ]);
    final stateWithPlugin = DaemonState(
      rigs: [rigWithPlugin, rigOrange],
      assets: {...sampleState.assets, 'plug-1': vst3Asset},
      footswitchMapping: sampleState.footswitchMapping,
      activeRigIndex: sampleState.activeRigIndex,
      activePresetIndex: sampleState.activePresetIndex,
      activeRigId: sampleState.activeRigId,
      activePresetId: sampleState.activePresetId,
      bypass: sampleState.bypass,
      tempoBpm: sampleState.tempoBpm,
    );
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
