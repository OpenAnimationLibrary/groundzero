from .probe import Probe
from krita import Krita
Krita.instance().addExtension(Probe(Krita.instance()))
