"""HQ textures: make your own AI-upscaled texture pack from your own game (Hydro Thunder, Crazy Taxi).

The textures come from the game disc, so they can't be shared - this tool rebuilds them on each
player's machine. Steps:

    python3 -I hqtex.py setup                 download the upscaler (Real-ESRGAN, ncnn/Vulkan build)
    python3 -I hqtex.py dump-quest on         make the Quest app save every texture the game loads
        ... play: every track you want in HQ, menus included (textures are saved as they appear) ...
    python3 -I hqtex.py dump-quest off
    python3 -I hqtex.py pull-quest DUMP_DIR   copy the saved textures from the Quest
    python3 -I hqtex.py upscale DUMP_DIR HQ_DIR [--scale 2|4]
    python3 -I hqtex.py install-quest HQ_DIR  put them in the Quest app (then Settings > Textures > HQ)

Textures are kept per game: pull-quest, install-quest and install-desktop take --game-id ID, the
game's disc ID (T-9702N Hydro Thunder Rev 1 - the default -, RDC-0102 Crazy Taxi ReviveDC; Flycast
logs it at boot, or tools/ht4q/discid.py IMAGE prints it). Keep each game's dumps and packs in
separate folders.

Or dump on the desktop, automatically (macOS/Linux, needs this fork's Flycast built - see CLAUDE.md):
    python3 -I hqtex.py dump-desktop DUMP_DIR [--tracks thunder,lost,...] [--game FILE] [--flycast PATH]
                                              [--minutes 5]
        races each track (default: all 13) with the camera riding an AI boat round the whole course
        (your checkpoint timer kept full), then quits. Locked tracks are opened for the run, on a
        copy of your save (yours is not changed); Hydro Thunder Rev 1 only.
    python3 -I hqtex.py dump-desktop DUMP_DIR --game "Crazy Taxi (USA).cdi" [--maps arcade,original]
        Crazy Taxi (ReviveDC, RDC-0102): starts each map's Arcade-rules game and moves the taxi across the
        whole city (the game time kept full), all four drivers; about 45 min per map.
    then `upscale DUMP_DIR HQ_DIR` as above, and `install-quest` or `install-desktop HQ_DIR`
    (<Flycast data>/textures/<game ID>/, with config:rend.CustomTextures=yes).

Needs Python 3 with numpy and Pillow (pip install numpy pillow) for `upscale`, adb for the Quest
commands, and a Vulkan- or Metal-capable GPU. --scale 2 (default) suits the Quest's memory; 4 for
the desktop. Run with the game set to Original textures while dumping.
Upscaler location: --esrgan DIR or $HT4Q_ESRGAN, default ~/.cache/ht4q/realesrgan.
"""
import hashlib, io, os, platform, shutil, struct, subprocess, sys, tempfile, urllib.request, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PKG = 'com.flycast.emulator.ht4q'
EXT = f'/sdcard/Android/data/{PKG}/files'   # the app's external files: emu.cfg, texture dumps
PRIV = f'/data/user/0/{PKG}/files'          # private: HQ textures go in textures/<game ID>
HT_ID = 'T-9702N'                           # Hydro Thunder (USA) (Rev 1)
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
# right presses on the track screen, with every track open (the cursor goes round clockwise)
TRACKS = {'thunder': 0, 'lost': 1, 'arctic': 2, 'graveyard': 3, 'venice': 4, 'newyork': 5, 'nile': 6,
          'speedway': 7, 'catacomb': 8, 'castle': 9, 'fareast': 10, 'powell': 11, 'greek': 12}
UNLOCK_ALL = 63		# ht4q.UnlockTracks: medium, hard and the four bonus tracks
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


def flycast_data_dir():
    home = os.path.expanduser('~')
    return {'Darwin': f'{home}/Library/Application Support/Flycast/data',
            'Windows': os.path.join(os.environ.get('APPDATA', home), 'flycast', 'data')}.get(
            platform.system(), os.path.join(os.environ.get('XDG_DATA_HOME', f'{home}/.local/share'), 'flycast'))


def start_flycast(flycast, game, dump, vmu_dir, *options):
    """Flycast dumping textures into dump, with the save in vmu_dir, and a memserver connection to it."""
    import dcmem, time
    env = dict(os.environ, HT4Q_MEMSERVER='3264')
    config = [a for o in ('aica:Volume=0', 'config:rend.DumpTextures=yes', f'config:Dreamcast.TextureDumpPath={dump}',
                          'config:rend.CustomTextures=no', f'config:Dreamcast.VMUPath={vmu_dir}', *options) for a in ('-config', o)]
    proc = subprocess.Popen([flycast, *config, game], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        try:
            return proc, dcmem.DcMem(timeout=10)
        except OSError:
            time.sleep(1)
    proc.terminate()
    die('no connection to Flycast\'s memserver: is this the HydroThunder4Quest build?')


def dump_track(flycast, game, dump, vmu_dir, track, minutes):
    import dcmem, time
    # the game saves the opened tracks with its progress: it gets a copy of the save (vmu_dir)
    proc, m = start_flycast(flycast, game, dump, vmu_dir, f'ht4q:UnlockTracks={UNLOCK_ALL}')
    try:
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
            print(f'  {track}: no race started (not the Rev 1 image, or Flycast without the ht4q hooks?)')
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


# Crazy Taxi (ReviveDC, RDC-0102). The game draws a large part of the city round the taxi in every
# direction, so moving the taxi over a grid (dropped from above, it lands on whatever is below) loads
# every area's textures.
CT_ID = 'RDC-0102'
CT_TAXI_POS = 0x8c176ec4		# float x, y, z: the taxi (the other copies follow it)
CT_GAME_TIME = 0x8c1ec4b4		# s32 frames left (60 per second)
CT_CAM_MODE = 0x8c1775e0		# 0 while driving
CT_MAPS = {'arcade': 0, 'original': 1}	# DOWN presses on the mode screen
CT_AREA = (-24000, 24000, 1000)	# x and z: from, to, step (on both maps new textures turned up all over it)
CT_DRIVERS = 4


def dump_ct(flycast, game, dump, vmu_dir, map_name, driver, sweep):
    import dcmem, time
    proc, m = start_flycast(flycast, game, dump, vmu_dir)
    try:
        def tap(button, wait):	# short: a longer press repeats in the menus
            m.input(button); time.sleep(0.1); m.input(0); time.sleep(wait)
        time.sleep(20)	# boot to the title screen
        tap(dcmem.BTN_START, 4)
        tap(dcmem.BTN_START, 4)	# -> mode selection
        for _ in range(CT_MAPS[map_name]):
            tap(dcmem.DPAD_DOWN, 1.5)
        tap(dcmem.BTN_A, 4)	# mode -> rules ("play by arcade rules")
        tap(dcmem.BTN_A, 8)	# -> driver selection
        for _ in range(driver):
            tap(dcmem.DPAD_RIGHT, 1.5)
        tap(dcmem.BTN_A, 4)
        end = time.time() + 40	# A through the start fly-by (the game time is already set during it)
        while time.time() < end and not (m.read_u32(CT_CAM_MODE) == 0 and m.read_u32(CT_GAME_TIME) != 0):
            tap(dcmem.BTN_A, 2)
        time.sleep(3)
        if m.read_u32(CT_CAM_MODE) != 0 or m.read_u32(CT_GAME_TIME) == 0:
            print(f'  {map_name}, driver {driver + 1}: not driving (not the ReviveDC image, or Flycast without the ht4q hooks?)')
            return
        if not sweep:
            print(f'  {map_name}, driver {driver + 1}: done')
            return
        lo, hi, step = CT_AREA
        rows = range(lo, hi + 1, step)
        print(f'  {map_name}: moving the taxi across the city ({len(rows)} rows)...')
        for i, z in enumerate(rows):
            for x in range(lo, hi + 1, step):
                m.write(CT_GAME_TIME, struct.pack('<i', 99 * 60))
                m.write(CT_TAXI_POS, struct.pack('<3f', x, 4000, z))
                time.sleep(0.9)
            if i % 10 == 9:
                print(f'  {map_name}: row {i + 1} of {len(rows)}')
        print(f'  {map_name}: done')
    finally:
        proc.terminate()
        proc.wait(10)


def dump_desktop(args):
    game = os.path.abspath(option(args, '--game', DEFAULT_GAME))
    flycast = option(args, '--flycast', None) or default_flycast()
    tracks = option(args, '--tracks', ','.join(TRACKS)).split(',')
    minutes = float(option(args, '--minutes', '5'))
    maps = option(args, '--maps', ','.join(CT_MAPS)).split(',')
    if len(args) != 1:
        die('dump-desktop DUMP_DIR [--game FILE] [--tracks thunder,lost,...] [--maps arcade,original] [--flycast PATH]')
    if flycast is None or not os.path.exists(flycast):
        die('Flycast (this fork) not found: build it (see CLAUDE.md) or pass --flycast PATH')
    if not os.path.exists(game):
        die(f'no game image {game}: pass --game FILE')
    import discid
    game_id = discid.disc_id(game) or HT_ID	# a .chd isn't read: taken as Hydro Thunder
    if game_id not in (HT_ID, CT_ID):
        die(f'{game} is {game_id}: dump-desktop knows Hydro Thunder (USA) (Rev 1) ({HT_ID}) and Crazy Taxi ReviveDC ({CT_ID})')
    for t in tracks if game_id == HT_ID else maps:
        if t not in (TRACKS if game_id == HT_ID else CT_MAPS):
            die(f'unknown track or map {t}')
    work = os.path.abspath(args[0]) + '.raw'
    # the games save progress: they get a copy of the player's save
    vmu_dir = tempfile.mkdtemp(prefix='ht4q_vmu')
    save = os.path.join(flycast_data_dir(), f'{game_id}_vmu_save_A1.bin')
    if os.path.exists(save):
        shutil.copy2(save, vmu_dir)
    try:
        if game_id == HT_ID:
            for t in tracks:
                dump_track(flycast, game, work, vmu_dir, t, minutes)
        else:
            # each map swept once; every driver (their taxi and portrait) started once
            for i, map_name in enumerate(maps):
                dump_ct(flycast, game, work, vmu_dir, map_name, i % CT_DRIVERS, True)
            for d in range(len(maps), CT_DRIVERS):
                dump_ct(flycast, game, work, vmu_dir, maps[0], d, False)
    finally:
        shutil.rmtree(vmu_dir)
    os.makedirs(args[0], exist_ok=True)
    n = 0
    for root, _, files in os.walk(work):
        for f in files:
            if f.endswith('.png'):
                shutil.copy2(os.path.join(root, f), os.path.join(args[0], f)); n += 1
    shutil.rmtree(work)
    print(f'{n} textures in {args[0]}' + (' (the minimap frames among them are skipped by `upscale`)' if game_id == HT_ID else ''))


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
    game_id = option(args, '--game-id', HT_ID)
    if len(args) != 1:
        die('pull-quest DUMP_DIR [--game-id ID]')
    dst = args[0]
    os.makedirs(dst, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        # Flycast dumps into a folder per game ID: only this game's
        r = adb('pull', f'{EXT}/texdump/{game_id}', tmp, check=False)
        if r.returncode != 0:
            die(f'no dumped textures for {game_id} on the Quest')
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
    game_id = option(args, '--game-id', HT_ID)
    if len(args) != 1 or not os.path.isdir(args[0]):
        die('install-quest HQ_DIR [--game-id ID]')
    tmp = '/data/local/tmp/ht4q_tex'
    adb('shell', f'rm -rf {tmp} && mkdir -p {tmp}')
    print('copying...')
    adb('push', os.path.join(args[0], '.'), f'{tmp}/')
    r = adb('shell', f'run-as {PKG} rm -rf {PRIV}/textures/{game_id} && run-as {PKG} mkdir -p {PRIV}/textures/{game_id}'
            f' && run-as {PKG} cp -r {tmp}/. {PRIV}/textures/{game_id}/ && rm -rf {tmp}', check=False)
    if r.returncode != 0 or r.stderr.strip():
        die(f'copying into the app failed (run-as needs the debug APK): {r.stderr.strip()}')
    print('installed: on the Quest choose Settings > Textures > HQ')


def install_desktop(args):
    game_id = option(args, '--game-id', HT_ID)
    if len(args) != 1 or not os.path.isdir(args[0]):
        die('install-desktop HQ_DIR [--game-id ID]')
    dst = os.path.join(flycast_data_dir(), 'textures', game_id)
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
