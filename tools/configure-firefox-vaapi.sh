#!/bin/sh
# Current-user launcher/desktop integration and one explicit root-owned policy.
# No profile rewrites, daemon checks, or sandbox-disabling defaults.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 - "$script_dir/../vaapi-driver/tools/firefox-hwdec" "$@" <<'PY'
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import stat
import sys
import tempfile

TEMPLATE = Path(sys.argv.pop(1))
PREF = 'dom.ipc.forkserver.enable'
VALUE = {'Value': False, 'Status': 'locked'}
TAG = '# Managed by configure-firefox-vaapi.sh (explicit RDD KGSL compatibility policy)'

def fail(message):
    raise RuntimeError(message)

def load_json(path, default=None):
    if path.is_symlink():
        fail(f'Refusing symlink: {path}')
    if not path.exists():
        return default
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        fail(f'Expected a JSON object: {path}')
    return value

def atomic(path, data, mode=0o644):
    if path.is_symlink():
        fail(f'Refusing symlink: {path}')
    path.parent.mkdir(parents=True, exist_ok=True)
    old = path.stat() if path.exists() else None
    fd, temp = tempfile.mkstemp(prefix='.' + path.name + '.', dir=path.parent)
    try:
        os.fchmod(fd, stat.S_IMODE(old.st_mode) if old else mode)
        if old and os.geteuid() == 0:
            os.fchown(fd, old.st_uid, old.st_gid)
        with os.fdopen(fd, 'wb') as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)

def encoded(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode()

def policy_parts(data):
    policies = data.setdefault('policies', {})
    if not isinstance(policies, dict):
        fail('policies must be an object')
    prefs = policies.setdefault('Preferences', {})
    if not isinstance(prefs, dict):
        fail('policies.Preferences must be an object')
    return policies, prefs

def policy_ok(path):
    data = load_json(path, {})
    policies = data.get('policies', {})
    prefs = policies.get('Preferences', {}) if isinstance(policies, dict) else {}
    return isinstance(prefs, dict) and prefs.get(PREF) == VALUE

def manage_policy(path, remove):
    if os.geteuid() != 0:
        fail('System policy changes require root. Use --install-system-policy as root.')
    state_path = path.with_name(path.name + '.dmd-firefox-state.json')
    backup = path.with_name(path.name + '.dmd-firefox.bak')
    state = load_json(state_path)
    data = load_json(path, {})
    if remove:
        if state is None:
            print(f'No managed policy state; preserving {path}')
            return
        policies, prefs = policy_parts(data)
        if prefs.get(PREF) != VALUE:
            fail('Managed preference changed elsewhere; preserving the current policy and backup.')
        if state['had_pref']:
            prefs[PREF] = state['previous_pref']
        else:
            prefs.pop(PREF, None)
        if not prefs and not state['had_preferences']:
            policies.pop('Preferences', None)
        if not policies and not state['had_policies']:
            data.pop('policies', None)
        original = load_json(backup) if state['had_file'] else None
        if original is not None and data == original:
            atomic(path, backup.read_bytes())
        elif not state['had_file'] and not data:
            path.unlink(missing_ok=True)
        else:
            atomic(path, encoded(data))
        state_path.unlink()
        print(f'Removed only managed {PREF}; other policies preserved: {path}')
        return
    if state is not None:
        if not policy_ok(path):
            fail('Managed preference changed elsewhere; reconcile it before reinstalling.')
        print(f'Policy already installed: {path}')
        return
    had_policies = 'policies' in data
    had_preferences = isinstance(data.get('policies'), dict) and 'Preferences' in data['policies']
    policies, prefs = policy_parts(data)
    state = {'had_file': path.exists(), 'had_policies': had_policies,
             'had_preferences': had_preferences, 'had_pref': PREF in prefs,
             'previous_pref': prefs.get(PREF)}
    if path.exists():
        if backup.exists() or backup.is_symlink():
            fail(f'Archive the existing recovery backup before a new policy install: {backup}')
        atomic(backup, path.read_bytes(), 0o600)
    prefs[PREF] = VALUE
    atomic(state_path, encoded(state), 0o600)
    atomic(path, encoded(data))
    print(f'Installed locked {PREF}=false: {path}')

def desktop_quote(path):
    # Desktop Entry Exec quoting is not shell quoting.
    value = str(path).replace('%', '%%')
    return '"' + re.sub(r'([\\"`$])', r'\\\1', value) + '"'

def patch_desktop(data, wrapper):
    changed = 0
    names = {'firefox', 'firefox-esr', 'firefox-bin', 'firefox-hwdec', 'firefox-msm-vaapi'}
    token_re = re.compile(r'''(?:[^\s"'\\]|\\.|"(?:[^"\\]|\\.)*"|'[^']*')+''')
    def rewrite(line):
        nonlocal changed
        if not line.startswith('Exec='):
            return line
        command = line[5:]
        tokens = list(token_re.finditer(command))
        if not tokens:
            return line
        values = [shlex.split(token.group())[0] for token in tokens]
        index = 0
        if Path(values[0]).name == 'env':
            index = 1
            while index < len(values):
                value = values[index]
                if re.match(r'^[A-Za-z_][A-Za-z0-9_]*=', value):
                    index += 1
                elif value == '--':
                    index += 1
                    break
                else:
                    break
        if index < len(tokens) and Path(values[index]).name in names:
            token = tokens[index]
            changed += 1
            return 'Exec=' + command[:token.start()] + desktop_quote(wrapper) + command[token.end():]
        return line
    output = ''.join(rewrite(line) for line in data.splitlines(keepends=True))
    if not changed:
        fail('Desktop has no direct Firefox executable; refusing an unsafe rewrite.')
    return output.encode()

def file_hash(data):
    return hashlib.sha256(data).hexdigest()

def user_paths():
    if os.geteuid() == 0:
        fail('Run user integration as the desktop user. Root only uses --install-system-policy/--uninstall-system-policy.')
    home = Path.home()
    return home / '.local/bin/firefox-hwdec', home / '.local/share/applications', home / '.local/state/droidspaces-firefox/integration.json'

def install_user(args):
    wrapper, apps, state_path = user_paths()
    lib = Path(args.compat_lib)
    if not lib.is_absolute() or re.search(r'[\s:]', str(lib)) or not lib.is_file() or not os.access(lib, os.R_OK):
        fail('--compat-lib must be a readable absolute library path without whitespace/colons')
    if not policy_ok(args.policy_file):
        fail('Locked forkserver policy missing. First use --install-system-policy as root; RDD/content/GPU sandboxes stay enabled.')
    if not TEMPLATE.is_file():
        fail(f'Launcher template missing: {TEMPLATE}')
    body = TEMPLATE.read_text()
    launcher = ('#!/bin/sh\n' + TAG + '\nDMD_FIREFOX_HEAP_LIB=${DMD_FIREFOX_HEAP_LIB:-' + shlex.quote(str(lib)) + '}\n' + body.split('\n', 1)[1]).encode()
    plans = {wrapper: launcher}
    directories = [Path(x) for x in args.desktop_dir] if args.desktop_dir else [Path('/usr/share/applications'), Path('/usr/local/share/applications')]
    candidates = {}
    for directory in directories:
        for pattern in ('firefox*.desktop', 'org.mozilla.firefox*.desktop'):
            for source in sorted(directory.glob(pattern)):
                candidates.setdefault(source.name, source)
    for pattern in ('firefox*.desktop', 'org.mozilla.firefox*.desktop'):
        for source in sorted(apps.glob(pattern)):
            candidates[source.name] = source
    if not candidates:
        fail('No Firefox vendor .desktop found; specify --desktop-dir.')
    for name, source in candidates.items():
        plans[apps / name] = patch_desktop(source.read_text(), wrapper)
    state = load_json(state_path, {'files': {}})
    for dest, content in plans.items():
        key = str(dest)
        previous = state['files'].get(key)
        if previous and dest.exists() and file_hash(dest.read_bytes()) != previous['installed_sha256']:
            fail(f'Managed file changed elsewhere; preserving it: {dest}')
        if not previous:
            backup = dest.with_name(dest.name + '.dmd-firefox.bak')
            if backup.exists() or backup.is_symlink():
                fail(f'Archive the existing recovery backup first: {backup}')
            previous = {'had_file': dest.exists(), 'backup': str(backup)}
            if dest.exists():
                atomic(backup, dest.read_bytes(), stat.S_IMODE(dest.stat().st_mode))
            state['files'][key] = previous
        previous['installed_sha256'] = file_hash(content)
    state['compat_lib'] = str(lib)
    atomic(state_path, encoded(state), 0o600)
    for dest, content in plans.items():
        atomic(dest, content, 0o755 if dest == wrapper else 0o644)
        if dest == wrapper:
            dest.chmod(0o755)
        print(f'Installed: {dest}')
    print('Close all Firefox processes before launching. Existing profiles were not rewritten.')

def uninstall_user():
    wrapper, apps, state_path = user_paths()
    state = load_json(state_path)
    if state is None:
        print('No managed user integration; existing files preserved.')
        return
    records = sorted(state['files'].items(), key=lambda item: item[0] == str(wrapper))
    for name, record in records:
        path = Path(name)
        if path.exists() and file_hash(path.read_bytes()) != record['installed_sha256']:
            fail(f'Managed file changed elsewhere; preserving current file and backup: {path}')
    for name, record in records:
        path = Path(name)
        backup = Path(record['backup'])
        if record['had_file']:
            atomic(path, backup.read_bytes(), stat.S_IMODE(backup.stat().st_mode))
            path.chmod(stat.S_IMODE(backup.stat().st_mode))
        else:
            path.unlink(missing_ok=True)
        print(f'Restored/removed: {path}')
    state_path.unlink()
    print('System policy is separate: root may use --uninstall-system-policy.')

def verify(args):
    wrapper, apps, state_path = user_paths()
    state = load_json(state_path, {})
    ok = policy_ok(args.policy_file)
    print(f'Locked forkserver policy: {"OK" if ok else "MISSING"}: {args.policy_file}')
    files = state.get('files', {})
    ok = bool(files) and ok
    for name, record in files.items():
        path = Path(name)
        valid = path.is_file() and file_hash(path.read_bytes()) == record['installed_sha256']
        if path == wrapper:
            valid = valid and os.access(path, os.X_OK)
        ok = valid and ok
        print(f'{"OK" if valid else "CHANGED/MISSING"}: {path}')
    lib = Path(state.get('compat_lib', args.compat_lib))
    readable = lib.is_file() and os.access(lib, os.R_OK)
    print(f'RDD compatibility preload: {"OK" if readable else "MISSING"}: {lib}')
    if not (ok and readable):
        raise SystemExit(1)

def main():
    parser = argparse.ArgumentParser(description='Firefox 160 GNU/Linux AArch64: explicit RDD KGSL ioctl policy; Mozilla sandboxes are not disabled.')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--install', action='store_true', help='current-user launcher/desktops (default)')
    group.add_argument('--uninstall', action='store_true', help='restore only managed user files')
    group.add_argument('--verify', action='store_true')
    group.add_argument('--install-system-policy', action='store_true', help='root: merge the single locked forkserver preference')
    group.add_argument('--uninstall-system-policy', action='store_true', help='root: restore only that preference')
    parser.add_argument('--compat-lib', default=os.environ.get('DMD_FIREFOX_HEAP_LIB', '/usr/local/lib/droidspaces-firefox/libdmd-firefox-compat.so'))
    parser.add_argument('--policy-file', type=Path, default=Path('/usr/lib/firefox/distribution/policies.json'), help='policy path in this native Firefox installation')
    parser.add_argument('--desktop-dir', action='append', help='vendor applications directory (repeatable)')
    args = parser.parse_args()
    if not args.policy_file.is_absolute():
        fail('--policy-file must be absolute')
    if args.install_system_policy or args.uninstall_system_policy:
        manage_policy(args.policy_file, args.uninstall_system_policy)
    elif args.uninstall:
        uninstall_user()
    elif args.verify:
        verify(args)
    else:
        install_user(args)

try:
    main()
except (RuntimeError, OSError, ValueError) as exc:
    print(f'error: {exc}', file=sys.stderr)
    sys.exit(1)
PY
