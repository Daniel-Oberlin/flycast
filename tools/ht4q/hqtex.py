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

Desktop Flycast instead of the Quest: dump with
    Flycast -config config:rend.DumpTextures=yes -config config:Dreamcast.TextureDumpPath=DUMP_DIR <game.cue>
and use HQ_DIR as <Flycast data>/textures/T-9702N/ with config:rend.CustomTextures=yes
(`install-desktop HQ_DIR` copies it there).

Needs Python 3 with numpy and Pillow (pip install numpy pillow) for `upscale`, adb for the Quest
commands, and a Vulkan- or Metal-capable GPU. --scale 2 (default) suits the Quest's memory; 4 for
the desktop. Run with the game set to Original textures while dumping.
Upscaler location: --esrgan DIR or $HT4Q_ESRGAN, default ~/.cache/ht4q/realesrgan.
"""
import hashlib, io, os, platform, shutil, subprocess, sys, tempfile, urllib.request, zipfile

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
    commands = {'setup': setup, 'dump-quest': dump_quest, 'pull-quest': pull_quest, 'upscale': upscale,
                'install-quest': install_quest, 'install-desktop': install_desktop}
    if not args or args[0] not in commands:
        print(__doc__)
        sys.exit(1)
    commands[args[0]](args[1:])


if __name__ == '__main__':
    main()
