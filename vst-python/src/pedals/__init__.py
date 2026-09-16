"""DSP pedal models in Python, used as a reference for the C++ audio engine.

See ``vst-python/README.md`` for why these are hand-written DSP rather than
NAM captures, and for the workflow that ports one to the C++ engine.
"""

from .bigmuff import BigMuff, BigMuffParams
from .dsp import OnePole, Oversampler, db_to_gain, soft_clip
from .noisegate import NoiseGate, NoiseGateParams
from .tubescreamer import TubeScreamer, TubeScreamerParams

#: Every model, keyed by the ``TYPE`` the C++ block registry would use.
PEDALS = {
    BigMuff.TYPE: BigMuff,
    TubeScreamer.TYPE: TubeScreamer,
    NoiseGate.TYPE: NoiseGate,
}

__all__ = [
    "PEDALS",
    "BigMuff",
    "BigMuffParams",
    "NoiseGate",
    "NoiseGateParams",
    "OnePole",
    "Oversampler",
    "TubeScreamer",
    "TubeScreamerParams",
    "db_to_gain",
    "soft_clip",
]
