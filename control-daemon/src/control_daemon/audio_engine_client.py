"""Abstraction over the (not-yet-built) JUCE/C++ audio engine.

The daemon calls an ``AudioEngineClient`` on every operation that should
affect what's actually playing (loading a preset, toggling bypass, tempo
updates). Wiring in the real engine later -- most likely over a local
IPC/socket to the JUCE host process -- is then a drop-in swap for
``NullAudioEngineClient``; nothing else in the daemon needs to change.
"""

from __future__ import annotations

import logging
from abc import ABC, abstractmethod

from .models import Preset

logger = logging.getLogger("control_daemon.audio_engine")


class AudioEngineClient(ABC):
    @abstractmethod
    def load_preset(self, preset: Preset) -> None:
        """Load and apply the given preset's full signal chain."""

    @abstractmethod
    def set_bypass(self, bypass: bool) -> None:
        """Engage/disengage global bypass."""

    @abstractmethod
    def set_tempo(self, bpm: float) -> None:
        """Update the engine's tempo (e.g. for tempo-synced effects)."""


class NullAudioEngineClient(AudioEngineClient):
    """No-op engine client used until the real audio engine exists.

    Every call is logged at INFO level so behavior is observable in dev and
    in tests without requiring an actual engine process.
    """

    def load_preset(self, preset: Preset) -> None:
        logger.info("load_preset(id=%s, name=%r)", preset.id, preset.name)

    def set_bypass(self, bypass: bool) -> None:
        logger.info("set_bypass(%s)", bypass)

    def set_tempo(self, bpm: float) -> None:
        logger.info("set_tempo(%.2f bpm)", bpm)
