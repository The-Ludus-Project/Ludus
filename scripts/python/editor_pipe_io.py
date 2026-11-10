"""Byte I/O shared by the editor adapter and live Play supervisor."""

import os
import selectors

if os.name == "nt":
    from editor_process_windows import PipeSelector as pipe_selector, read_pipe, set_nonblocking, write_pipe
else:
    pipe_selector = selectors.DefaultSelector
    read_pipe = os.read
    write_pipe = os.write

    def set_nonblocking(fd, *, write=False):
        os.set_blocking(fd, False)
