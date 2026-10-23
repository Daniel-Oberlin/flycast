"""HQ textures for Hydro Thunder: make your own AI-upscaled texture pack from your own game.

The textures come from the game disc, so they can't be shared - this tool rebuilds them on each
player's machine. Steps:

    python3 -I hqtex.py setup                 download the upscaler (Real-ESRGAN, ncnn/Vulkan build)
    python3 -I hqtex.py dump-quest on         make the Quest app save every texture the game loads
        ... play: every track you want in HQ, menus included (textures are saved as they appear) ...
    python3 -I hqtex.py dump-quest off
    python3 -I hqtex.py pull-quest DUMP_DIR   copy the saved textures from the Quest
    python3 -I hqtex.py upscale DUMP_DIR HQ_DIR [--scale 2|4]
    python3 -I hqtex.py install-quest HQ_DIR  put them in the Quest app (then Settings > Textures > HQ)

Or dump on the desktop, automatically (macOS/Linux, needs this fork's Flycast built - see CLAUDE.md):
    python3 -I hqtex.py dump-desktop DUMP_DIR [--tracks thunder,lost,arctic] [--game FILE] [--flycast PATH]
                                              [--minutes 5]
        races each track with the camera riding an AI boat round the whole course (your checkpoint
        timer kept full), then quits. Tracks must be unlocked in your save; Rev 1 image only.
    then `upscale DUMP_DIR HQ_DIR` as above, and `install-quest` or `install-desktop HQ_DIR`
    (<Flycast data>/textures/T-9702N/, with config:rend.CustomTextures=yes).

Needs Python 3 with numpy and Pillow (pip install numpy pillow) for `upscale`, adb for the Quest
commands, and a Vulkan- or Metal-capable GPU. --scale 2 (default) suits the Quest's memory; 4 for
the desktop. Run with the game set to Original textures while dumping.
Upscaler location: --esrgan DIR or $HT4Q_ESRGAN, default ~/.cache/ht4q/realesrgan.
"""
import hashlib, io, os, platform, shutil, struct, subprocess, sys, tempfile, urllib.request, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PKG = 'com.flycast.emulator.ht4q'
EXT = f'/sdcard/Android/data/{PKG}/files'   # the app's external files: emu.cfg, texture dumps
PRIV = f'/data/user/0/{PKG}/files'          # private: HQ textures go in textures/T-9702N
GAME_ID = 'T-9702N'
RELEASE = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/'
PACKAGES = {    # platform -> (zip, sha256 or None)
    'Darwin': ('realesrgan-ncnn-vulkan-20220424-macos.zip', 'e0ad05580abfeb25f8d8fb55aaf7bedf552c375b5b4d9bd3c8d59764d2cc333a'),
    'Linux': ('realesrgan-ncnn-vulkan-20220424-ubuntu.zip', None),
    'Windows': ('realesrgan-ncnn-vulkan-20220424-windows.zip', None),
}


def die(msg):
    sys.exit(f'error: {msg}')


def option(args, name, default=None):
    if name in args:
        i = args.index(name)
        value = args[i + 1]
        del args[i:i + 2]
        return value
    return default


def esrgan_dir(args):
    return os.path.expanduser(option(args, '--esrgan', os.environ.get('HT4Q_ESRGAN', '~/.cache/ht4q/realesrgan')))


def esrgan_exe(d):
    return os.path.join(d, 'realesrgan-ncnn-vulkan' + ('.exe' if platform.system() == 'Windows' else ''))


def adb(*args, check=True, capture=False):
    if shutil.which('adb') is None:
        die('adb not found: install Android platform-tools')
    r = subprocess.run(['adb', *args], check=False, text=True, capture_output=True)
    if check and r.returncode != 0:
        die(f'adb {" ".join(args)}: {r.stderr.strip() or r.stdout.strip()}')
    return r.stdout if capture else r


def setup(args):
    d = esrgan_dir(args)
    if os.path.exists(esrgan_exe(d)):
        print(f'upscaler already in {d}')
        return
    pkg = PACKAGES.get(platform.system())
    if pkg is None:
        die(f'no Real-ESRGAN build for {platform.system()}')
    name, sha = pkg
    print(f'downloading {name} (about 45 MB; BSD-3-Clause, models by Xintao Wang et al.)...')
    data = urllib.request.urlopen(RELEASE + name).read()
    if sha and hashlib.sha256(data).hexdigest() != sha:
        die('download checksum mismatch')
    os.makedirs(d, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for member in z.namelist():
            target = os.path.realpath(os.path.join(d, member))
            if not target.startswith(os.path.realpath(d) + os.sep):
                die(f'unexpected path in archive: {member}')
        z.extractall(d)
    os.chmod(esrgan_exe(d), 0o755)
    if platform.system() == 'Darwin':   # downloaded binaries are quarantined
        subprocess.run(['xattr', '-dr', 'com.apple.quarantine', d], check=False)
    print(f'upscaler in {d}')


# Desktop dumping. RAM addresses are for Hydro Thunder (USA) (Rev 1).
TRACKS = {'thunder': 0, 'lost': 1, 'arctic': 2}		# right presses on the track screen
ATTACHED_PHYS = 0x8c1fc988		# the boat the camera follows (0 outside a race)
AI_BOAT_PHYS = 0x8c256838		# the first AI boat's physics (the player's is 0x330 before it)
CHECKPOINT_TIME = 0x8c2597bc	# float seconds left
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_GAME = os.path.join(REPO, '..', 'gamefiles', 'Hydro Thunder (USA) (Rev 1)', 'Hydro Thunder (USA) (Rev 1).cue')


def default_flycast():
    for p in ('build/RelWithDebInfo/Flycast.app/Contents/MacOS/Flycast', 'build/flycast', 'build/Release/flycast'):
        if os.path.exists(os.path.join(REPO, p)):
            return os.path.join(REPO, p)
    return None


def dump_track(flycast, game, dump, track, minutes):
    import dcmem, socket, time
    env = dict(os.environ, HT4Q_MEMSERVER='3264')
    proc = subprocess.Popen([flycast, '-config', 'aica:Volume=0', '-config', 'config:rend.DumpTextures=yes',
                             '-config', f'config:Dreamcast.TextureDumpPath={dump}', '-config', 'config:rend.CustomTextures=no',
                             game], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        m = None
        for _ in range(60):
            try:
                m = dcmem.DcMem(timeout=10)
                break
            except OSError:
                time.sleep(1)
        if m is None:
            die('no connection to Flycast\'s memserver: is this the HydroThunder4Quest build?')

        def tap(button, wait):
            m.input(button); time.sleep(0.4); m.input(0); time.sleep(wait)
        time.sleep(10)	# boot to the title screen
        tap(dcmem.BTN_START, 3)
        tap(dcmem.BTN_A, 5)	# Game -> track screen
        for _ in range(TRACKS[track]):
            tap(dcmem.DPAD_RIGHT, 2)
        end = time.time() + 90	# then A through the boat screen until the race starts
        while time.time() < end and not 0x8c000000 <= m.read_u32(ATTACHED_PHYS) < 0x8d000000:
            tap(dcmem.BTN_A, 1.2)
        if not 0x8c000000 <= m.read_u32(ATTACHED_PHYS) < 0x8d000000:
            print(f'  {track}: no race started (track locked, or not the Rev 1 image?)')
            return
        print(f'  {track}: racing, the camera follows an AI boat round the course...')
        end = time.time() + minutes * 60
        while time.time() < end:
            att = m.read_u32(ATTACHED_PHYS)
            if not 0x8c000000 <= att < 0x8d000000:
                break	# race over (before the attract mode shows other tracks)
            if att != AI_BOAT_PHYS:
                m.write(ATTACHED_PHYS, struct.pack('<I', AI_BOAT_PHYS))	# the game re-attaches the player at the start
            if 0 < m.read_f32(CHECKPOINT_TIME) < 200:
                m.write_f32(CHECKPOINT_TIME, 60.0)
            time.sleep(1)
        print(f'  {track}: done')
    finally:
        proc.terminate()
        proc.wait(10)


def dump_desktop(args):
    tracks = option(args, '--tracks', 'thunder,lost,arctic').split(',')
    game = os.path.abspath(option(args, '--game', DEFAULT_GAME))
    flycast = option(args, '--flycast', None) or default_flycast()
    minutes = float(option(args, '--minutes', '5'))
    if len(args) != 1:
        die('dump-desktop DUMP_DIR [--tracks thunder,lost,arctic] [--game FILE] [--flycast PATH]')
    if flycast is None or not os.path.exists(flycast):
        die('Flycast (this fork) not found: build it (see CLAUDE.md) or pass --flycast PATH')
    if not os.path.exists(game):
        die(f'no game image {game}: pass --game FILE (Hydro Thunder (USA) (Rev 1) .cue/.gdi/.chd)')
    for t in tracks:
        if t not in TRACKS:
            die(f'unknown track {t} (thunder, lost, arctic)')
    work = os.path.abspath(args[0]) + '.raw'
    for t in tracks:
        dump_track(flycast, game, work, t, minutes)
    os.makedirs(args[0], exist_ok=True)
    n = 0
    for root, _, files in os.walk(work):
        for f in files:
            if f.endswith('.png'):
                shutil.copy2(os.path.join(root, f), os.path.join(args[0], f)); n += 1
    shutil.rmtree(work)
    print(f'{n} textures in {args[0]} (the minimap frames among them are skipped by `upscale`)')


def set_quest_config(key, value):
    """Sets key = value in the [config] section of the app's emu.cfg (rewritten in place)."""
    cfg = adb('shell', f'cat {EXT}/emu.cfg', check=False, capture=True)
    if 'No such file' in cfg or not cfg.strip():
        die('the app has no settings yet: start it once on the Quest, then try again')
    lines, section, done = cfg.splitlines(), None, False
    out = []
    for line in lines:
        s = line.strip()
        if s.startswith('['):
            if section == 'config' and not done:
                out.append(f'{key} = {value}'); done = True
            section = s[1:-1]
        elif section == 'config' and s.split('=')[0].strip() == key:
            line, done = f'{key} = {value}', True
        out.append(line)
    if not done:
        if section != 'config':
            out.append('[config]')
        out.append(f'{key} = {value}')
    with tempfile.NamedTemporaryFile('w', suffix='.cfg', delete=False) as f:
        f.write('\n'.join(out) + '\n')
    adb('push', f.name, '/data/local/tmp/ht4q_emu.cfg')
    os.unlink(f.name)
    # overwrite the content, keeping the app's file (a file pushed by adb may be invisible to the app)
    adb('shell', f'cat /data/local/tmp/ht4q_emu.cfg > {EXT}/emu.cfg && rm /data/local/tmp/ht4q_emu.cfg')


def dump_quest(args):
    if not args or args[0] not in ('on', 'off'):
        die('dump-quest on|off')
    on = args[0] == 'on'
    adb('shell', f'am force-stop {PKG}')
    set_quest_config('rend.DumpTextures', 'yes' if on else 'no')
    if on:
        print('texture dumping on: start the game on the Quest (Textures: Original) and play every track you want')
    else:
        print('texture dumping off')


def pull_quest(args):
    if len(args) != 1:
        die('pull-quest DUMP_DIR')
    dst = args[0]
    os.makedirs(dst, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        adb('pull', f'{EXT}/texdump', tmp)
        n = 0
        for root, _, files in os.walk(tmp):
            for f in files:
                if f.endswith('.png'):
                    shutil.copy2(os.path.join(root, f), os.path.join(dst, f))
                    n += 1
    print(f'{n} textures in {dst}')


def upscale(args):
    scale = option(args, '--scale', '2')
    model = option(args, '--model', 'realesrgan-x4plus')
    d = esrgan_dir(args)
    if len(args) != 2:
        die('upscale DUMP_DIR HQ_DIR [--scale 2|4]')
    if not os.path.exists(esrgan_exe(d)):
        die(f'no upscaler in {d}: run `hqtex.py setup` first')
    try:
        import texup
    except ImportError as e:
        die(f'{e}: pip install numpy pillow')
    sys.argv = ['texup.py', d, args[0], args[1], '--scale', scale, '--model', model]
    texup.main()


def install_quest(args):
    if len(args) != 1 or not os.path.isdir(args[0]):
        die('install-quest HQ_DIR')
    tmp = '/data/local/tmp/ht4q_tex'
    adb('shell', f'rm -rf {tmp} && mkdir -p {tmp}')
    print('copying...')
    adb('push', os.path.join(args[0], '.'), f'{tmp}/')
    r = adb('shell', f'run-as {PKG} rm -rf {PRIV}/textures/{GAME_ID} && run-as {PKG} mkdir -p {PRIV}/textures/{GAME_ID}'
            f' && run-as {PKG} cp -r {tmp}/. {PRIV}/textures/{GAME_ID}/ && rm -rf {tmp}', check=False)
    if r.returncode != 0 or r.stderr.strip():
        die(f'copying into the app failed (run-as needs the debug APK): {r.stderr.strip()}')
    print('installed: on the Quest choose Settings > Textures > HQ')


def install_desktop(args):
    if len(args) != 1 or not os.path.isdir(args[0]):
        die('install-desktop HQ_DIR')
    home = os.path.expanduser('~')
    data = {'Darwin': f'{home}/Library/Application Support/Flycast/data',
            'Windows': os.path.join(os.environ.get('APPDATA', home), 'flycast', 'data')}.get(
            platform.system(), os.path.join(os.environ.get('XDG_DATA_HOME', f'{home}/.local/share'), 'flycast'))
    dst = os.path.join(data, 'textures', GAME_ID)
    if os.path.islink(dst):
        os.unlink(dst)
    shutil.copytree(args[0], dst, dirs_exist_ok=True)
    print(f'installed in {dst}: run Flycast with -config config:rend.CustomTextures=yes (or enable it in its settings)')


def main():
    args = sys.argv[1:]
    commands = {'setup': setup, 'dump-quest': dump_quest, 'pull-quest': pull_quest, 'dump-desktop': dump_desktop, 'upscale': upscale,
                'install-quest': install_quest, 'install-desktop': install_desktop}
    if not args or args[0] not in commands:
        print(__doc__)
        sys.exit(1)
    commands[args[0]](args[1:])


if __name__ == '__main__':
    main()
