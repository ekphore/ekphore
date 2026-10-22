#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Bundle and verify runtime dependencies in release artifacts."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def command(*args):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('LD_', 'DYLD_'))}
    return subprocess.check_output([str(arg) for arg in args], text=True, env=env)


def pe_imports(image):
    # /imports includes delay-loaded DLLs too, unlike a startup-only check
    lines = command('dumpbin', '/nologo', '/imports', image).splitlines()
    return sorted({line.strip().lower() for line in lines
                   if re.fullmatch(r'\s+.+\.dll\s*', line, re.IGNORECASE)})


def windows_system_library(name):
    libraries = {
        'advapi32.dll', 'avrt.dll', 'bcrypt.dll', 'bcryptprimitives.dll',
        'cfgmgr32.dll', 'comctl32.dll', 'comdlg32.dll', 'crypt32.dll',
        'cryptbase.dll', 'cryptsp.dll', 'd2d1.dll', 'd3d11.dll', 'd3d12.dll',
        'd3d9.dll', 'd3dcompiler_47.dll', 'dbghelp.dll', 'dinput8.dll',
        'dnsapi.dll', 'dsound.dll', 'dwmapi.dll', 'dwrite.dll', 'dxgi.dll',
        'gdi32.dll', 'hid.dll', 'imm32.dll', 'iphlpapi.dll', 'kernel32.dll',
        'kernelbase.dll', 'mf.dll', 'mfplat.dll', 'mfreadwrite.dll', 'mfuuid.dll',
        'mmdevapi.dll', 'msacm32.dll', 'msimg32.dll', 'msvcrt.dll', 'ncrypt.dll',
        'normaliz.dll', 'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'opengl32.dll',
        'powrprof.dll', 'propsys.dll', 'psapi.dll', 'rpcrt4.dll', 'secur32.dll',
        'setupapi.dll', 'shell32.dll', 'shlwapi.dll', 'ucrtbase.dll', 'user32.dll',
        'userenv.dll', 'usp10.dll', 'uxtheme.dll', 'version.dll',
        'windowscodecs.dll', 'winhttp.dll', 'wininet.dll', 'winmm.dll',
        'wintrust.dll', 'winusb.dll',
        'ws2_32.dll', 'wtsapi32.dll', 'xinput1_4.dll',
    }
    return name in libraries or name.startswith(('api-ms-win-', 'ext-ms-win-'))


def windows_verify(output):
    count = 0
    for image in output.rglob('*'):
        if not image.is_file() or image.is_symlink():
            continue
        with image.open('rb') as stream:
            if stream.read(2) != b'MZ':
                continue
        count += 1
        for name in pe_imports(image):
            if not windows_system_library(name):
                raise RuntimeError(f'Non-system DLL imported by {image}: {name}. '
                                   'Windows releases must link dependencies and the C++ runtime statically.')
    if count == 0:
        raise RuntimeError(f'No Windows images found in {output}')
    print(f'Verified {count} Windows images; only Windows system DLLs are imported')


def windows_smoke(output):
    with tempfile.TemporaryDirectory(prefix='ekphore-windows-smoke-') as temp:
        directory = Path(temp)
        env = {key.upper(): value for key, value in os.environ.items()}
        env.update(PATH=str(Path(env['SYSTEMROOT']) / 'System32'),
                   HOME=temp, USERPROFILE=temp, APPDATA=temp, LOCALAPPDATA=temp,
                   LIBNKS_RUNTIME_DIR=str(directory / 'nks'))
        for name in ('ekphore-ui.exe', 'nksd.exe'):
            isolated = directory / Path(name).stem
            isolated.mkdir()
            shutil.copy2(output / name, isolated / name)
            subprocess.run([str(isolated / name), '--help'], cwd=isolated,
                           env=env, check=True, timeout=30)
        for suffix in ('.clap', '.vst3'):
            plugins = list(output.glob('*' + suffix))
            if len(plugins) != 1:
                raise RuntimeError(f'Expected one {suffix} plugin, found {plugins}')
            isolated = directory / suffix[1:]
            isolated.mkdir()
            destination = isolated / plugins[0].name
            if plugins[0].is_dir():
                shutil.copytree(plugins[0], destination)
                images = list(destination.glob('Contents/x86_64-win/*.vst3'))
            else:
                shutil.copy2(plugins[0], destination)
                images = [destination]
            if len(images) != 1:
                raise RuntimeError(f'Expected one plugin binary in {destination}')
            subprocess.run([sys.executable, '-c',
                            'import ctypes, sys; ctypes.WinDLL(sys.argv[1], winmode=0x900)',
                            str(images[0])], cwd=isolated, env=env, check=True, timeout=30)
        print('Isolated Windows standalone, daemon and plugin loading passed')


def elf(path):
    with path.open('rb') as stream:
        return stream.read(4) == b'\x7fELF'


def host_library(name):
    return bool(re.fullmatch(
        r'(lib(c|m|dl|pthread|rt|resolv|util|anl|BrokenLocale|nss_[\w]+)\.so\.[\d.]+|ld-linux[^/]*\.so\.[\d.]+)', name))


def loaded(image):
    paths = {}
    for line in command('ldd', image).splitlines():
        if '=> not found' in line:
            raise RuntimeError(f'Unresolved dependency of {image}: {line.strip()}')
        if ' => ' not in line:
            continue
        name, path = line.strip().split(' => ', 1)
        path = path.rsplit(' (', 1)[0]
        if not path.startswith('/'):
            raise RuntimeError(f'Unexpected ldd dependency: {line}')
        paths[name] = Path(path).resolve()
    return paths


def elf_notices(source, output):
    candidates = [source]
    if str(source).startswith('/usr/lib/'):
        candidates.append(Path(str(source)[4:]))
    for path in candidates:
        result = subprocess.run(['dpkg-query', '-S', str(path)],
                                text=True, capture_output=True)
        if result.returncode:
            continue
        package = result.stdout.split(': ', 1)[0].split(':', 1)[0]
        copyright = Path('/usr/share/doc') / package / 'copyright'
        if copyright.is_file():
            destination = output / 'licenses/debian' / package
            destination.mkdir(parents=True, exist_ok=True)
            shutil.copy2(copyright, destination / 'copyright')
            return
    raise RuntimeError(f'Cannot find package copyright for {source}')


def elf_bundle(output):
    clap = output / 'ekphore.clap'
    if clap.is_file():
        directory = output / 'ekphore-clap'
        directory.mkdir()
        clap.rename(directory / clap.name)
    roots = [path for path in output.rglob('*')
             if path.is_file() and not path.is_symlink() and elf(path)]
    copied = {}
    edited = set()

    def visit(source, destination, directory, paths):
        if destination in edited:
            return
        edited.add(destination)
        for name in command('patchelf', '--print-needed', source).splitlines():
            if host_library(name):
                continue
            library = paths.get(name)
            if library is None:
                raise RuntimeError(f'Cannot resolve {name} needed by {source}')
            if '/' in name:
                raise RuntimeError(f'Absolute DT_NEEDED is not supported: {name}')
            target = directory / name
            if target in copied and copied[target] != library:
                raise RuntimeError(f'Conflicting libraries for {target}')
            if target not in copied:
                copied[target] = library
                directory.mkdir(parents=True, exist_ok=True)
                if target.resolve() != library:
                    shutil.copy2(library, target)
                    target.chmod(target.stat().st_mode | 0o200)
                elf_notices(library, output)
                visit(library, target, directory, paths)
        relative = os.path.relpath(directory, destination.parent)
        search = '$ORIGIN' if relative == '.' else '$ORIGIN/' + relative
        subprocess.run(['patchelf', '--set-rpath', search, str(destination)], check=True)

    for image in roots:
        visit(image, image, image.parent / 'lib', loaded(image))
    if copied:
        destination = output / 'licenses/debian/common-licenses'
        shutil.copytree('/usr/share/common-licenses', destination, dirs_exist_ok=True)


def elf_verify(output):
    count = 0
    for image in output.rglob('*'):
        if not image.is_file() or image.is_symlink() or not elf(image):
            continue
        count += 1
        search = command('patchelf', '--print-rpath', image).strip()
        if search not in ('$ORIGIN', '$ORIGIN/lib'):
            raise RuntimeError(f'Unexpected runtime search path in {image}: {search}')
        directory = image.parent if search == '$ORIGIN' else image.parent / 'lib'
        for name in command('patchelf', '--print-needed', image).splitlines():
            if host_library(name):
                continue
            target = (directory / name).resolve()
            if '/' in name or not target.is_relative_to(output) or not target.is_file():
                raise RuntimeError(f'Missing or escaping dependency of {image}: {name}')
        for name, path in loaded(image).items():
            if not host_library(name) and not path.is_relative_to(output):
                raise RuntimeError(f'External dependency of {image}: {name} => {path}')
    if count == 0:
        raise RuntimeError(f'No ELF images found in {output}')
    print(f'Verified {count} ELF files; only glibc remains a host dependency in {output}')


def system_library(name):
    return name.startswith(('/usr/lib/', '/System/Library/'))


def macho(path):
    with path.open('rb') as stream:
        return stream.read(4) in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe',
                                  b'\xfe\xed\xfa\xcf', b'\xfe\xed\xfa\xce',
                                  b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca')


def install_id(image):
    lines = command('otool', '-D', image).splitlines()
    return next((line.strip() for line in lines if not line.endswith(':')), None)


def dependencies(image):
    identity = install_id(image)
    names = [line.strip().split(' (compatibility version', 1)[0]
             for line in command('otool', '-L', image).splitlines()
             if line.startswith('\t')]
    return list(dict.fromkeys(name for name in names if name != identity))


def rpaths(image):
    paths = []
    pending = False
    for line in command('otool', '-l', image).splitlines():
        line = line.strip()
        if line.startswith('cmd '):
            pending = line == 'cmd LC_RPATH'
        elif pending and line.startswith('path '):
            paths.append(line[5:].rsplit(' (offset ', 1)[0])
            pending = False
    return list(dict.fromkeys(paths))


def expand(name, image, executable):
    return name.replace('@loader_path', str(image.parent)).replace(
        '@executable_path', str(executable.parent))


def resolve(name, image, executable, paths):
    if name.startswith('@rpath/'):
        candidates = [Path(path) / name[len('@rpath/'):] for path in paths]
    else:
        candidates = [Path(expand(name, image, executable))]
    for path in candidates:
        if path.is_absolute() and path.is_file():
            return path.resolve()
    raise RuntimeError(f'Cannot resolve {name} needed by {image}')


def macho_notices(source, output):
    for prefix in source.parents:
        if not (prefix / 'INSTALL_RECEIPT.json').is_file():
            continue
        for path in prefix.rglob('*'):
            if not path.is_file() or not path.name.upper().startswith(
                    ('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE')):
                continue
            destination = output / 'licenses/homebrew' / prefix.parent.name / path.relative_to(prefix)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        break


def macho_bundle(output):
    copied = {}
    edited = set()

    def visit(source, destination, directory, executable, inherited):
        if destination in edited:
            return
        edited.add(destination)
        paths = [expand(path, source, executable) for path in rpaths(source)] + inherited
        changes = []
        for name in dependencies(source):
            if system_library(name):
                continue
            library = resolve(name, source, executable, paths)
            if system_library(str(library)):
                changes.extend(('-change', name, str(library)))
                continue
            if library.suffix != '.dylib':
                raise RuntimeError(f'Unsupported external library bundle: {library}')
            target = directory / library.name
            if target in copied and copied[target] != library:
                raise RuntimeError(f'Conflicting libraries for {target}: {library}, {copied[target]}')
            if target not in copied:
                copied[target] = library
                directory.mkdir(parents=True, exist_ok=True)
                if target != library:
                    shutil.copy2(library, target)
                    target.chmod(target.stat().st_mode | 0o200)
                macho_notices(library, output)
                visit(library, target, directory, executable, paths)
            relative = os.path.relpath(target, destination.parent)
            changes.extend(('-change', name, '@loader_path/' + relative))
        for path in rpaths(destination):
            changes.extend(('-delete_rpath', path))
        if install_id(destination):
            changes.extend(('-id', '@rpath/' + destination.name))
        if changes:
            signed = subprocess.run(['codesign', '-d', str(destination)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if signed.returncode == 0:
                subprocess.run(['codesign', '--remove-signature', str(destination)], check=True)
            subprocess.run(['install_name_tool', *changes, str(destination)], check=True)

    for name in ('ekphore-ui', 'nksd'):
        image = output / name
        if image.is_file():
            visit(image, image, output / 'Frameworks', image, [])
    bundles = [path for path in output.iterdir()
               if path.suffix in ('.app', '.clap', '.component', '.vst3') and path.is_dir()]
    for path in bundles:
        for directory in ('MacOS', 'Helpers'):
            for image in (path / 'Contents' / directory).glob('*'):
                if image.is_file() and macho(image):
                    visit(image, image, path / 'Contents/Frameworks', image, [])
    # modifying load commands invalidates the old signatures; sign copies only
    for image in sorted(edited):
        subprocess.run(['codesign', '--force', '--sign', '-', str(image)], check=True)
    for path in bundles:
        subprocess.run(['codesign', '--force', '--sign', '-', str(path)], check=True)


def macho_verify(output):
    count = 0
    for image in output.rglob('*'):
        if not image.is_file() or image.is_symlink() or not macho(image):
            continue
        count += 1
        for name in dependencies(image):
            if system_library(name):
                continue
            if not name.startswith('@loader_path/'):
                raise RuntimeError(f'External dependency remains in {image}: {name}')
            target = Path(expand(name, image, image)).resolve()
            if not target.is_relative_to(output) or not target.is_file():
                raise RuntimeError(f'Missing or escaping dependency in {image}: {name}')
        if rpaths(image):
            raise RuntimeError(f'Unexpected runtime search path remains in {image}')
        subprocess.run(['codesign', '--verify', '--strict', str(image)], check=True)
    if count == 0:
        raise RuntimeError(f'No Mach-O files found in {output}')
    for path in output.iterdir():
        if path.suffix in ('.app', '.clap', '.component', '.vst3') and path.is_dir():
            subprocess.run(['codesign', '--verify', '--deep', '--strict', str(path)], check=True)
    print(f'Verified {count} self-contained Mach-O files in {output}')


def smoke(output):
    with tempfile.TemporaryDirectory(prefix='ekphore-macos-smoke-') as temp:
        env = {key: value for key, value in os.environ.items() if not key.startswith('DYLD_')}
        env.update(HOME=temp, LIBNKS_RUNTIME_DIR=str(Path(temp) / 'nks'),
                   EKPHORE_NKS_DIR=str(Path(temp) / 'disabled-nks'), EKPHORE_NKS_DIRECT='0',
                   DYLD_PRINT_LIBRARIES='1')
        app = output / 'Ekphore.app/Contents'
        executable = app / 'MacOS/ekphore-ui' if app.is_dir() else output / 'ekphore-ui'
        result = subprocess.run([str(executable), '--help'], env=env,
                                text=True, capture_output=True, check=True)
        if any(prefix in result.stderr for prefix in
               ('/opt/homebrew/', '/usr/local/Cellar/', '/usr/local/opt/')):
            raise RuntimeError('Standalone loaded a Homebrew library outside the artifact')
        for path in output.glob('*/Contents/MacOS/*'):
            if path == executable:
                continue
            if path.is_file() and macho(path):
                subprocess.run([sys.executable, '-c',
                                'import ctypes, sys; ctypes.CDLL(sys.argv[1])', str(path)],
                               env={key: value for key, value in env.items()
                                    if key != 'DYLD_PRINT_LIBRARIES'}, check=True)
        for path in output.glob('*/Contents/Helpers/nksd'):
            subprocess.run([str(path), '--help'],
                           env={key: value for key, value in env.items()
                                if key != 'DYLD_PRINT_LIBRARIES'}, check=True, timeout=30)
        print('Relocated standalone startup and plugin loading passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifacts', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--smoke', action='store_true',
                        help='Test isolated Windows/macOS standalone and plugin loading')
    args = parser.parse_args()
    output = args.artifacts.resolve()
    if sys.platform == 'darwin':
        bundle, verify = macho_bundle, macho_verify
    elif sys.platform == 'win32':
        bundle, verify = None, windows_verify
    elif sys.platform.startswith('linux'):
        if args.smoke:
            parser.error('Use linux-check.sh for Linux runtime smoke tests')
        bundle, verify = elf_bundle, elf_verify
    else:
        parser.error('Library bundling is supported on Linux, macOS and Windows only')
    if not args.verify_only and bundle:
        bundle(output)
    verify(output)
    if args.smoke:
        if sys.platform == 'win32':
            windows_smoke(output)
        else:
            smoke(output)
