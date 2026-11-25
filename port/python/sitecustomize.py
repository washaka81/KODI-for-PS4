"""PS4 network defaults for Kodi's embedded Python.

Kodi's curl sockets are tuned in PS4OpenSocket, but Python add-ons use their
own sockets (requests/urllib3/http.client).  This sitecustomize applies the
same generic options so small HTTPS requests do not pay Nagle/delayed-ACK
latency and streams keep the same kernel buffers.

No host, IP, SSID or MTU is hardcoded.  TCP_MAXSEG is intentionally not set:
the Orbis kernel rejects it with EINVAL; let the kernel negotiate, and note
that a pre-connect getsockopt() can report 536 even when the path MTU is
larger.
"""

import socket

_original_socket = socket.socket

# 256 KiB is the stability default (flex stays ~83 MB and the GUI does not
# hit the flexible ceiling).  Larger windows are an opt-in tradeoff through
# /data/kodi-ps4/net-buffer-kb (64..2048 KiB), shared with Kodi's curl path.
_BUFFER_KB = 256
try:
    with open("/data/kodi-ps4/net-buffer-kb", encoding="ascii") as _f:
        _BUFFER_KB = int(_f.read().strip())
except (OSError, ValueError):
    pass
_BUFFER_KB = max(64, min(2048, _BUFFER_KB))
_RCVBUF = _BUFFER_KB * 1024
_SNDBUF = _BUFFER_KB * 1024


def _tune(sock):
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except (AttributeError, OSError):
        pass
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, _RCVBUF)
    except OSError:
        pass
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, _SNDBUF)
    except OSError:
        pass
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    except OSError:
        pass
    for name, value in (("TCP_KEEPIDLE", 30), ("TCP_KEEPINTVL", 10), ("TCP_KEEPCNT", 3)):
        option = getattr(socket, name, None)
        if option is None:
            continue
        try:
            sock.setsockopt(socket.IPPROTO_TCP, option, value)
        except OSError:
            pass


class _PS4TunedSocket(_original_socket):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        _tune(self)


socket.socket = _PS4TunedSocket
