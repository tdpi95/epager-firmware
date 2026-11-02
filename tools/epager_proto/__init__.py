"""Python reference implementation of the ePager contract (contracts/protocol.md).

Used to generate and check test vectors and by the Python tools. The firmware uses the C
library in components/protocol; both must agree on every vector.
"""
from . import ancs, assets, codec, config, registry, segments, text, transfer  # noqa: F401
