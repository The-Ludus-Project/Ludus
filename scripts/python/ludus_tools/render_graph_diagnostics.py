"""Bounded validation batches; callers stop between dependent validation phases."""
from contextlib import contextmanager

from .errors import ToolingError

MAX_DIAGNOSTICS = 32
MAX_DETAIL_CHARACTERS = 512
MAX_MESSAGE_BYTES = 4096
TRUNCATION_SUFFIX = b'... (truncated)'


def make_error(code, where, message, *, port=None, direction=None, field=None,
               expected=None, actual=None, repair=None):
    diagnostic = f'{where}: {message}'.encode('utf-8', errors='backslashreplace')
    if len(diagnostic) > MAX_MESSAGE_BYTES:
        diagnostic = diagnostic[:MAX_MESSAGE_BYTES - len(TRUNCATION_SUFFIX)].decode('utf-8', errors='ignore').encode() + TRUNCATION_SUFFIX
    error = ToolingError('RenderGraph' + code, diagnostic.decode('utf-8'))
    error.where = where
    error.target = {key: value for key, value in
                    (('port', port), ('direction', direction), ('field', field)) if value is not None}
    error.details = {key: str(value)[:MAX_DETAIL_CHARACTERS] for key, value in
                     (('expected', expected), ('actual', actual), ('repair', repair)) if value is not None}
    return error


class DiagnosticBatch:
    """Collect independent failures in authored order without continuing unsafe work.

    The first error retains the stable legacy code/message. Structured diagnostics
    include it once; consumers can replace their legacy summary row with that list.
    One slot is reserved for an explicit limit notice when errors are omitted.
    """
    def __init__(self):
        self.errors = []
        self.is_truncated = False

    def add(self, error):
        for issue in getattr(error, 'issues', [error]):
            if len(self.errors) < MAX_DIAGNOSTICS - 1:
                self.errors.append(issue)
            else:
                self.is_truncated = True

    @contextmanager
    def capture(self):
        try:
            yield
        except ToolingError as error:
            self.add(error)

    def raise_if_any(self):
        if not self.errors:
            return
        if self.is_truncated:
            notice = make_error('DiagnosticLimit', self.errors[0].where,
                f'Further errors omitted after {MAX_DIAGNOSTICS - 1} diagnostics. Repair these issues and validate again.')
            notice.severity = 'note'
            self.errors.append(notice)
        first = self.errors[0]
        first.issues = self.errors.copy()
        raise first
