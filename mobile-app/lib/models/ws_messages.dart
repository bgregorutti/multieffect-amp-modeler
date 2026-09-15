/// WebSocket message envelopes for the control-daemon protocol.
///
/// Mirrors `control_daemon.ws_protocol`. This app always connects with
/// `role: "app"`, so only the app-only commands are modeled here (not
/// `footswitch_press`, which only a footswitch-role client may send).
library;

import 'effect_block.dart';
import 'footswitch_action.dart';
import 'preset.dart';

// ---------------------------------------------------------------------------
// Client -> server
// ---------------------------------------------------------------------------

/// Must be the first message sent on every connection.
class HelloMessage {
  final String role;
  final String? clientName;

  const HelloMessage({this.role = 'app', this.clientName});

  Map<String, dynamic> toJson() => {
        'type': 'hello',
        'role': role,
        if (clientName != null) 'client_name': clientName,
      };
}

/// Base type for every app-only command this client can send. Each has a
/// stable `type` string used both on the wire and to match a `command_ok`
/// reply back to its caller.
abstract class DaemonCommand {
  String get type;
  Map<String, dynamic> toJson();
}

/// Omitting [chain] (the default) makes the daemon seed the standard
/// gain/amp/cab/tone-stack/volume skeleton (see control-daemon's
/// `models.default_rig_chain`) rather than an empty rig; pass an explicit
/// `[]` for a deliberately empty one instead.
class CreateRigCommand implements DaemonCommand {
  @override
  String get type => 'create_rig';

  final String name;
  final List<EffectBlock>? chain;

  const CreateRigCommand({required this.name, this.chain});

  @override
  Map<String, dynamic> toJson() => {
        'type': type,
        'name': name,
        if (chain != null) 'chain': chain!.map((b) => b.toJson()).toList(),
      };
}

/// Any field left `null` (the default) is left unchanged by the daemon.
/// Sending `chain` replaces the whole chain, so send the full desired order.
class UpdateRigCommand implements DaemonCommand {
  @override
  String get type => 'update_rig';

  final String rigId;
  final String? name;
  final List<EffectBlock>? chain;

  const UpdateRigCommand({required this.rigId, this.name, this.chain});

  @override
  Map<String, dynamic> toJson() {
    final json = <String, dynamic>{'type': type, 'rig_id': rigId};
    if (name != null) json['name'] = name;
    if (chain != null) {
      json['chain'] = chain!.map((b) => b.toJson()).toList();
    }
    return json;
  }
}

class DeleteRigCommand implements DaemonCommand {
  @override
  String get type => 'delete_rig';

  final String rigId;

  const DeleteRigCommand({required this.rigId});

  @override
  Map<String, dynamic> toJson() => {'type': type, 'rig_id': rigId};
}

class ReorderRigsCommand implements DaemonCommand {
  @override
  String get type => 'reorder_rigs';

  final List<String> rigIds;

  const ReorderRigsCommand({required this.rigIds});

  @override
  Map<String, dynamic> toJson() => {'type': type, 'rig_ids': rigIds};
}

class CreatePresetCommand implements DaemonCommand {
  @override
  String get type => 'create_preset';

  final String rigId;
  final String name;
  final Map<String, PresetBlockState> blockStates;

  const CreatePresetCommand({
    required this.rigId,
    required this.name,
    this.blockStates = const {},
  });

  @override
  Map<String, dynamic> toJson() => {
        'type': type,
        'rig_id': rigId,
        'name': name,
        'block_states':
            blockStates.map((id, s) => MapEntry(id, s.toJson())),
      };
}

/// Any field left `null` (the default) is left unchanged by the daemon.
class UpdatePresetCommand implements DaemonCommand {
  @override
  String get type => 'update_preset';

  final String rigId;
  final String presetId;
  final String? name;
  final Map<String, PresetBlockState>? blockStates;

  const UpdatePresetCommand({
    required this.rigId,
    required this.presetId,
    this.name,
    this.blockStates,
  });

  @override
  Map<String, dynamic> toJson() {
    final json = <String, dynamic>{
      'type': type,
      'rig_id': rigId,
      'preset_id': presetId,
    };
    if (name != null) json['name'] = name;
    if (blockStates != null) {
      json['block_states'] =
          blockStates!.map((id, s) => MapEntry(id, s.toJson()));
    }
    return json;
  }
}

class DeletePresetCommand implements DaemonCommand {
  @override
  String get type => 'delete_preset';

  final String rigId;
  final String presetId;

  const DeletePresetCommand({required this.rigId, required this.presetId});

  @override
  Map<String, dynamic> toJson() =>
      {'type': type, 'rig_id': rigId, 'preset_id': presetId};
}

/// Moves the active position. Either index may be sent alone; omitting
/// `rigIndex` selects within the current rig.
class SelectPresetCommand implements DaemonCommand {
  @override
  String get type => 'select_preset';

  final int? rigIndex;
  final int? presetIndex;

  const SelectPresetCommand({this.rigIndex, this.presetIndex});

  @override
  Map<String, dynamic> toJson() => {
        'type': type,
        if (rigIndex != null) 'rig_index': rigIndex,
        if (presetIndex != null) 'preset_index': presetIndex,
      };
}

class SetBypassCommand implements DaemonCommand {
  @override
  String get type => 'set_bypass';

  final bool bypass;

  const SetBypassCommand({required this.bypass});

  @override
  Map<String, dynamic> toJson() => {'type': type, 'bypass': bypass};
}

/// Replaces the *entire* footswitch mapping (not a merge) -- the app must
/// always send its complete desired mapping.
class SetFootswitchMappingCommand implements DaemonCommand {
  @override
  String get type => 'set_footswitch_mapping';

  final Map<int, FootswitchAction> mapping;

  const SetFootswitchMappingCommand({required this.mapping});

  @override
  Map<String, dynamic> toJson() => {
        'type': type,
        'mapping':
            mapping.map((k, v) => MapEntry(k.toString(), v.toJson())),
      };
}

/// Registers metadata for a .nam/IR file already uploaded via the plain HTTP
/// `POST /assets/upload` endpoint -- see `asset_upload_service.dart`.
class RegisterAssetCommand implements DaemonCommand {
  @override
  String get type => 'register_asset';

  final String kind;
  final String filename;
  final String storedPath;
  final int sizeBytes;
  final String? sha256;

  const RegisterAssetCommand({
    required this.kind,
    required this.filename,
    required this.storedPath,
    this.sizeBytes = 0,
    this.sha256,
  });

  @override
  Map<String, dynamic> toJson() => {
        'type': type,
        'kind': kind,
        'filename': filename,
        'stored_path': storedPath,
        'size_bytes': sizeBytes,
        'sha256': sha256,
      };
}

// ---------------------------------------------------------------------------
// Server -> client
// ---------------------------------------------------------------------------

/// A parsed incoming server->client message. Use the `type` string (or the
/// concrete subtype) to dispatch.
sealed class ServerMessage {
  const ServerMessage();

  factory ServerMessage.fromJson(Map<String, dynamic> json) {
    final type = json['type'] as String?;
    switch (type) {
      case 'state_snapshot':
        return StateSnapshotMessage(state: json['state'] as Map<String, dynamic>);
      case 'state_changed':
        return StateChangedMessage(
          state: json['state'] as Map<String, dynamic>,
          reason: json['reason'] as String,
        );
      case 'command_ok':
        return CommandOkMessage(
          command: json['command'] as String,
          result: json['result'] as Map<String, dynamic>? ?? const {},
        );
      case 'error':
        return ErrorMessage(
          code: json['code'] as String,
          message: json['message'] as String,
          inReplyTo: json['in_reply_to'] as String?,
        );
      default:
        return UnknownMessage(raw: json);
    }
  }
}

class StateSnapshotMessage extends ServerMessage {
  final Map<String, dynamic> state;
  const StateSnapshotMessage({required this.state});
}

class StateChangedMessage extends ServerMessage {
  final Map<String, dynamic> state;
  final String reason;
  const StateChangedMessage({required this.state, required this.reason});
}

class CommandOkMessage extends ServerMessage {
  final String command;
  final Map<String, dynamic> result;
  const CommandOkMessage({required this.command, required this.result});
}

class ErrorMessage extends ServerMessage {
  final String code;
  final String message;
  final String? inReplyTo;
  const ErrorMessage({required this.code, required this.message, this.inReplyTo});
}

/// A message whose `type` this client version doesn't recognize. Kept
/// instead of throwing so a future daemon message type doesn't crash the
/// app; callers should ignore it.
class UnknownMessage extends ServerMessage {
  final Map<String, dynamic> raw;
  const UnknownMessage({required this.raw});
}
