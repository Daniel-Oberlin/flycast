"""Desktop viewer for the 2-player stream (core/ht4q/stream.cpp): shows player 2's view with ffplay.

    python3 -I stream_view.py [HOST[:PORT]] [--save FILE.h264] [--seconds N] [--gas] [--steer X] [--yaw DEG]
                              [--xr DEG]

Prints frames/s, Mb/s and, when the host is this machine, how old each frame is on arrival.
--gas holds player 2's throttle, --steer X (-1..1) its stick, --yaw turns player 2's head (degrees, + = left).
--xr DEG pretends to be a headset turned DEG to the left: the host then streams both eyes (side by side).
Without ffplay (or with --save only) the stream is just counted or saved; --save F.h264 writes one file per
image size (F-WxH.h264), as the host restarts the encoder when it changes (mono -> stereo).
"""
import argparse, math, os, shutil, socket, struct, subprocess, sys, time

BTN_A = 1 << 2
# the host's clock, std::chrono::steady_clock (CLOCK_MONOTONIC_RAW on macOS, CLOCK_MONOTONIC elsewhere)
host_clock_us = lambda: time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW if sys.platform == 'darwin'
                                              else time.CLOCK_MONOTONIC) / 1000


def recv_all(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError('host closed the stream')
        buf += chunk
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host', nargs='?', default='127.0.0.1:3266')
    ap.add_argument('--save')
    ap.add_argument('--seconds', type=float, default=0)
    ap.add_argument('--gas', action='store_true')
    ap.add_argument('--steer', type=float, default=0)
    ap.add_argument('--yaw', type=float)
    ap.add_argument('--xr', type=float)
    ap.add_argument('--no-window', action='store_true')
    a = ap.parse_args()
    host, _, port = a.host.partition(':')
    s = socket.create_connection((host, int(port or 3266)), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if a.gas or a.steer:
        pressed = BTN_A if a.gas else 0
        s.sendall(b'I' + struct.pack('<IHHhhI', pressed, 0, 0xffff if a.gas else 0, int(a.steer * 32767), 0, 0))
    if a.yaw is not None:
        s.sendall(b'H' + struct.pack('<I6f', 1, 0, 0, 0, a.yaw, 0, 0))
    if a.xr is not None:
        half = math.radians(a.xr) / 2   # rotation about +y (left), LOCAL space, head at the origin
        s.sendall(b'X' + struct.pack('<8f', 0, math.sin(half), 0, math.cos(half), 0, 0, 0, 0.064))

    player = None
    if not a.no_window and shutil.which('ffplay'):
        player = subprocess.Popen(['ffplay', '-loglevel', 'error', '-fflags', 'nobuffer', '-flags', 'low_delay',
                                   '-framedrop', '-probesize', '32', '-sync', 'ext', '-window_title', 'player 2', '-'],
                                  stdin=subprocess.PIPE)
    save, save_size = None, None
    start = last = time.monotonic()
    frames = nbytes = 0
    ages = []
    try:
        while True:
            hdr = recv_all(s, 64)
            magic, size, frame, flags, t_us, pose, w, h = struct.unpack_from('<4sIIIQIHH', hdr)
            head_q, head_p, zscreen = struct.unpack_from('<4f', hdr, 32), struct.unpack_from('<3f', hdr, 48), \
                struct.unpack_from('<f', hdr, 60)[0]
            if magic != b'HT4V':
                raise ValueError('not an HT4V stream')
            data = recv_all(s, size)
            ages.append(host_clock_us() - t_us)
            frames += 1
            nbytes += size
            if player:
                player.stdin.write(data)
                player.stdin.flush()
            if a.save and (w, h) != save_size:
                if save:
                    save.close()
                base, ext = os.path.splitext(a.save)
                save, save_size = open(f'{base}-{w}x{h}{ext or ".h264"}', 'wb'), (w, h)
            if save:
                save.write(data)
            now = time.monotonic()
            if now - last >= 2:
                ages.sort()
                age = ages[len(ages) // 2] / 1000
                print(f'{w}x{h}{" stereo" if flags & 2 else ""}  {frames / (now - last):5.1f} frames/s  {nbytes * 8 / (now - last) / 1e6:5.2f} Mb/s'
                      f'  median age {age:6.1f} ms (same machine only)  frame {frame}{" key" if flags & 1 else ""}'
                      f'  fov {2 * math.degrees(math.atan(320 / zscreen)):.0f}' + (f'  head q {tuple(round(v, 3) for v in head_q)}' if flags & 2 else ''),
                      flush=True)
                last, frames, nbytes, ages = now, 0, 0, []
            if a.seconds and now - start >= a.seconds:
                break
    except (ConnectionError, BrokenPipeError) as e:
        print(e)
    finally:
        s.close()
        if save:
            save.close()
        if player:
            player.stdin.close()
            player.terminate()


if __name__ == '__main__':
    main()
