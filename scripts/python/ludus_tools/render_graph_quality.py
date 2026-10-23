"""Stable processing-quality choices, matching RenderGraphFormat::ProcessingQuality.

These are runtime library policies, not shader permutations. Consumers explicitly
opt into this vocabulary; arbitrary strings and automatic hardware changes reject.
"""
QUALITY_CODES = {'Fast': 1, 'Balanced': 2, 'High': 3}
DEFAULT_QUALITY = 'Balanced'
