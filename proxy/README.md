# babyfan — one camera connection, many viewers

The board serves each stream from a single-threaded `esp_http_server`. A second
viewer does not get a second stream — the two tear the first one apart. This is
not a bug in our firmware; it is what an ESP32-CAM is.

`babyfan` sits in front of it, holds **one** upstream connection per stream and
fans the bytes out to everyone connected. Python, standard library only,
nothing to install.

It optimises exactly one thing: **while nobody is watching, nothing is read
from the camera at all.** Ours runs off a power bank and is unwatched about 99%
of the time, so the upstream opens on the first viewer and closes after the
last one leaves. Everything else is deliberately plain.

## Running it

```bash
sudo mkdir -p /opt/babyfan
sudo install -m 755 babyfan.py /opt/babyfan/
sudo install -m 644 babyfan.service /etc/systemd/system/
sudoedit /etc/systemd/system/babyfan.service   # set BABYFAN_CAMERA
sudo systemctl enable --now babyfan
journalctl -u babyfan -f    # who connected, and when the camera was released
```

It listens on `127.0.0.1:8090` by default and serves `/audio` and `/stream`.
Put it behind whatever already terminates TLS for you:

```nginx
upstream baby_video { server 127.0.0.1:8090; }
upstream baby_audio { server 127.0.0.1:8090; }

location = /stream {
    proxy_pass http://baby_video; proxy_http_version 1.1;
    proxy_set_header Connection ""; proxy_buffering off;
    proxy_read_timeout 24h; proxy_send_timeout 24h;
}
location = /audio  { ...the same, to baby_audio... }
```

Leave `/level` and `/set` pointing straight at the board: they are short
requests, there is nothing to fan out.

| variable | default | |
|---|---|---|
| `BABYFAN_CAMERA` | `10.1.1.25` | the board's address |
| `BABYFAN_PORT` | `8090` | pick another if something already has it |
| `BABYFAN_GRACE` | `8` | seconds to hold the camera after the last viewer |

## The bug you will hit if you write your own

`esp_http_server` streams with **`Transfer-Encoding: chunked`**. Reverse proxies
handle that for you, so it is invisible until you read the socket yourself —
and then the hex length markers land *inside* the media. The audio becomes
noise while the file size still looks perfectly reasonable, and the video
frames break while a naive `FFD8` count still finds them all. `babyfan` decodes
chunked itself.

## Details worth knowing

**The grace period is not an optimisation.** Phones lock their screens and drop
sockets constantly; without a delay the camera link would be torn down and
rebuilt every few seconds, which costs the board more than staying connected.

**A viewer whose socket backs up past 4 MB is dropped.** One stalled phone
would otherwise grow the buffer until the Pi runs out of memory.

**Video viewers start at a part boundary.** Joining mid-frame would hand them
half a JPEG.

**Audio viewers get the WAV header replayed.** It is captured once from the
start of the upstream, so somebody joining an hour in still gets a valid file.

## Verified

Three simultaneous listeners, identical byte counts, **one** connection to the
camera. A listener joining five seconds in: valid WAV, 16 kHz / 16-bit / mono.
Two video viewers, intact frames. Ten seconds after everybody left: zero
connections to the camera.
