import 'package:flutter/material.dart';

import 'asset.dart';

/// Generic category icons for the signal-chain UI (`ChainStageCard` and the
/// assets list). Deliberately plain `IconData` constants rather than real
/// product photos or logos of amps/cabs/plugins -- a stage card just needs to
/// read at a glance as "this slot is an amp model" / "this one's a cab" /
/// "this one's a plugin", and using stand-ins for actual gear avoids any
/// trademark/rights question a photo-realistic icon would raise.
IconData assetKindIcon(AssetKind kind) {
  switch (kind) {
    case AssetKind.nam:
      // A captured/neural amp model -- "memory" reads as a captured
      // snapshot of a physical amp, not a generic file icon.
      return Icons.memory;
    case AssetKind.ir:
      // A cabinet/speaker impulse response.
      return Icons.graphic_eq;
    case AssetKind.vst3:
      // A hosted plugin -- the universal "plug-in" glyph.
      return Icons.extension;
  }
}

/// Icon for a chain block by its `EffectBlock.type` string, used by
/// `ChainStageCard` for every slot in the horizontal signal chain --
/// asset-backed types (nam/ir/vst3) get the same icon as their asset kind so
/// a slot looks the same whether you're looking at the rig chain or the
/// assets list; native types neither app has a `.type` catalog for anywhere
/// else get a generic fallback rather than crashing on an unknown type.
IconData blockTypeIcon(String type) {
  switch (type) {
    case 'nam':
      return assetKindIcon(AssetKind.nam);
    case 'ir':
      return assetKindIcon(AssetKind.ir);
    case 'vst3':
      return assetKindIcon(AssetKind.vst3);
    case 'gain':
      return Icons.tune;
    case 'volume':
      return Icons.volume_up;
    case 'tone_stack':
    case 'eq':
      return Icons.equalizer;
    case 'delay':
      return Icons.blur_circular;
    case 'reverb':
      return Icons.waves;
    // The modelled pedals. Distinct glyphs matter more here than literal ones:
    // these sit side by side in the chain, and three identical fallback icons
    // make a row of stage cards unreadable at a glance.
    case 'big_muff':
      return Icons.local_fire_department;
    case 'tube_screamer':
      return Icons.bolt;
    case 'noise_gate':
      return Icons.noise_control_off;
    default:
      return Icons.settings_input_component;
  }
}
