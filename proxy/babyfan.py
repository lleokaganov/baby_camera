#!/usr/bin/env python3
"""
babyfan — one connection to the camera, many viewers.

The ESP32-CAM serves each stream from a single-threaded HTTP server, so a
second viewer does not get a second stream: the two tear the first one apart.
This sits in front of it and holds exactly one upstream connection per stream,
fanning the bytes out to whoever is listening.

The one thing it optimises: nothing is read from the camera while nobody is
watching. The camera runs off a power bank and is unwatched ~99% of the time,
so the upstream connection opens on the first viewer and closes after the last
one leaves. Everything else is deliberately left simple.

stdlib only — no packages to install on the Pi.
"""

import asyncio
import logging
import os
import sys

CAMERA_HOST = os.environ.get("BABYFAN_CAMERA", "10.1.1.25")
LISTEN_HOST = os.environ.get("BABYFAN_HOST", "127.0.0.1")
LISTEN_PORT = int(os.environ.get("BABYFAN_PORT", "8090"))

# Seconds to keep the camera connection after the last viewer disappears.
# Phones lock their screen and drop the socket constantly; without this the
# link would be torn down and re-established every few seconds, which costs
# the camera more than simply staying connected for a moment longer.
GRACE = float(os.environ.get("BABYFAN_GRACE", "8"))

# A viewer whose socket has backed up this far is not keeping up and is
# dropped. Without a limit one stalled phone would grow the buffer without
# bound and take the Pi's memory with it.
MAX_BACKLOG = 4 * 1024 * 1024

log = logging.getLogger("babyfan")


class Fanout:
    """One upstream stream from the camera, shared by N viewers."""

    def __init__(self, name, port, path, content_type, boundary=None):
        self.name = name
        self.port = port
        self.path = path
        self.content_type = content_type
        # MJPEG only: a viewer joining mid-frame would be fed half a JPEG, so
        # it waits for the next part boundary before it gets anything.
        self.boundary = boundary
        self.viewers = []
        self.preamble = b""      # for WAV: the header every new viewer needs
        self.upstream = None
        self.closer = None

    # ---------------------------------------------------------------- viewers
    async def add(self, writer):
        if self.closer:
            self.closer.cancel()
            self.closer = None
        client = {"w": writer, "waiting": self.boundary is not None}
        self.viewers.append(client)
        if self.upstream is None:
            log.info("%s: first viewer, opening camera", self.name)
            self.upstream = asyncio.create_task(self._pump())
        return client

    def remove(self, client):
        try:
            self.viewers.remove(client)
        except ValueError:
            return
        if not self.viewers and self.upstream and not self.closer:
            self.closer = asyncio.create_task(self._close_later())

    async def _close_later(self):
        try:
            await asyncio.sleep(GRACE)
        except asyncio.CancelledError:
            return
        if self.viewers:
            return
        log.info("%s: no viewers, closing camera", self.name)
        if self.upstream:
            self.upstream.cancel()
            self.upstream = None
        self.preamble = b""
        self.closer = None

    # --------------------------------------------------------------- upstream
    async def _pump(self):
        try:
            r, w = await asyncio.open_connection(CAMERA_HOST, self.port)
            w.write(
                f"GET {self.path} HTTP/1.1\r\nHost: {CAMERA_HOST}\r\n"
                f"Connection: close\r\n\r\n".encode()
            )
            await w.drain()

            # Read the camera's own HTTP headers. Ours are already sent, so
            # these are only inspected, never forwarded -- but they matter:
            # esp_http_server streams with Transfer-Encoding: chunked, and the
            # hex length markers would otherwise land inside the audio.
            chunked = False
            while True:
                line = await r.readline()
                if not line or line in (b"\r\n", b"\n"):
                    break
                if line.lower().startswith(b"transfer-encoding:") and b"chunked" in line.lower():
                    chunked = True

            first = True
            while True:
                if chunked:
                    size_line = await r.readline()
                    if not size_line:
                        break
                    try:
                        size = int(size_line.split(b";")[0].strip(), 16)
                    except ValueError:
                        log.warning("%s: bad chunk size %r", self.name, size_line[:32])
                        break
                    if size == 0:
                        break
                    chunk = await r.readexactly(size)
                    await r.readexactly(2)          # the CRLF after the chunk
                else:
                    chunk = await r.read(16384)
                if not chunk:
                    log.warning("%s: camera closed the stream", self.name)
                    break
                if first and self.boundary is None:
                    # WAV header, replayed to everyone who joins later.
                    self.preamble = chunk[:44]
                    first = False
                self._broadcast(chunk)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            log.warning("%s: upstream failed: %s", self.name, e)
        finally:
            try:
                w.close()
            except Exception:
                pass
            self.upstream = None
            # Drop every viewer. When the camera goes away -- it reboots, the
            # power bank hiccups, WiFi drops -- the bytes simply stop. A viewer
            # left holding an open socket sees no error at all: the browser
            # keeps showing the last frame and waits forever. Closing them is
            # what turns a silent freeze into a reconnect.
            if self.viewers:
                log.info("%s: upstream gone, dropping %d viewer(s)",
                         self.name, len(self.viewers))
            for client in list(self.viewers):
                try:
                    client["w"].close()
                except Exception:
                    pass
            self.viewers.clear()
            self.preamble = b""

    def _broadcast(self, chunk):
        for client in list(self.viewers):
            data = chunk
            if client["waiting"]:
                # Start this viewer at a part boundary, not mid-JPEG.
                i = chunk.find(self.boundary)
                if i < 0:
                    continue
                data = chunk[i:]
                client["waiting"] = False
            w = client["w"]
            try:
                if w.transport.get_write_buffer_size() > MAX_BACKLOG:
                    log.info("%s: viewer too slow, dropping", self.name)
                    w.close()
                    self.remove(client)
                    continue
                w.write(data)
            except Exception:
                self.remove(client)


AUDIO = Fanout("audio", 82, "/audio", "audio/wav")
VIDEO = Fanout("video", 81, "/stream",
               "multipart/x-mixed-replace;boundary=frame", b"\r\n--frame\r\n")


async def handle(reader, writer):
    try:
        request = await asyncio.wait_for(reader.readline(), 10)
    except asyncio.TimeoutError:
        writer.close()
        return
    if not request:
        writer.close()
        return
    try:
        _, target, _ = request.decode("latin1").split(" ", 2)
    except ValueError:
        writer.close()
        return
    path = target.split("?", 1)[0]

    while True:                                   # drain request headers
        line = await reader.readline()
        if not line or line in (b"\r\n", b"\n"):
            break

    fan = {"/audio": AUDIO, "/stream": VIDEO}.get(path)
    if fan is None:
        writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        await writer.drain()
        writer.close()
        return

    writer.write(
        f"HTTP/1.1 200 OK\r\nContent-Type: {fan.content_type}\r\n"
        f"Cache-Control: no-store\r\nConnection: close\r\n\r\n".encode()
    )
    if fan.preamble:
        writer.write(fan.preamble)

    client = await fan.add(writer)
    peer = writer.get_extra_info("peername")
    log.info("%s: viewer %s joined (%d total)", fan.name, peer, len(fan.viewers))
    try:
        # Nothing more arrives from the viewer; this just waits for it to go.
        while True:
            if await reader.read(1024) == b"":
                break
    except Exception:
        pass
    finally:
        fan.remove(client)
        try:
            writer.close()
        except Exception:
            pass
        log.info("%s: viewer %s left (%d left)", fan.name, peer, len(fan.viewers))


async def main():
    logging.basicConfig(
        level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
        stream=sys.stdout,
    )
    server = await asyncio.start_server(handle, LISTEN_HOST, LISTEN_PORT)
    log.info("babyfan on %s:%d, camera %s", LISTEN_HOST, LISTEN_PORT, CAMERA_HOST)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
