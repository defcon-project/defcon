#!/usr/bin/env python3
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Bind a complete depends prefix to its inputs and producing workflow run."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

SCHEMA = 1
HOST = 'x86_64-pc-linux-gnu'
TARGET = 'linux64'
ACTIONS = {
    'checkout': '11d5960a326750d5838078e36cf38b85af677262',
    'cache': '0057852bfaa89a56745cba8c7296529d2fc39830',
    'upload': 'ea165f8d65b6e75b540449e92b4886f43607fa02',
    'download': 'd3f86a106a0bac45b974a628896c90dbdf5c8093',
}
CONFIG = (
    'ci/dash/depends_prefix.py', 'ci/dash/matrix.sh', 'ci/dash/build_src.sh',
    'ci/test/00_setup_env.sh', 'ci/test/00_setup_env_native_qt5.sh',
    'contrib/containers/ci/Dockerfile.linux64',
)
# This first implementation deliberately supports only the existing HOST-only recipe.
# Environment overrides must not silently create a different dependency configuration.
OVERRIDES = (
    'HOST BUILD DEBUG LTO MULTIPROCESS ALLOW_HOST_PACKAGES '
    'NO_QT NO_QR NO_BDB NO_SQLITE NO_WALLET NO_ZMQ NO_UPNP NO_USDT NO_NATPMP '
    'BUILD_ID_SALT HOST_ID_SALT GUIX_ENVIRONMENT CC CXX AR RANLIB STRIP NM '
    'OBJCOPY OBJDUMP CFLAGS CXXFLAGS CPPFLAGS LDFLAGS C_STANDARD CXX_STANDARD '
    'MAKEFLAGS MFLAGS MAKEOVERRIDES MAKEFILES SDK_PATH BASE_CACHE SOURCES_PATH '
    'WORK_PATH CONFIG_SITE CONFIG_SHELL DEP_OPTS SOURCE_DATE_EPOCH '
    'SDKROOT CMAKE_TOOLCHAIN_FILE PKG_CONFIG_PATH PKG_CONFIG_LIBDIR LD AS'
).split()
MAX_ARCHIVE = 1024 ** 3
MAX_PAYLOAD = 4 * 1024 ** 3
MAX_ENTRIES = 100000
MAX_JSON = 32 * 1024 ** 2
META_MARKER = 'DEFCON_CACHE_PROBE_META_V1'
PATCH_MARKER = 'DEFCON_CACHE_PROBE_PATCH_V1'
FIXTURES = {
    'C3': ('depends/hosts/linux.mk', None, '\nlinux_release_CFLAGS += -D' + META_MARKER + '=1\n'),
    'C4': ('depends/patches/boost/process_macos_sdk.patch',
           '+    auto my_fd = dirfd(dir.get());',
           '+    auto my_fd = dirfd(dir.get()); // ' + PATCH_MARKER),
}


class ContractError(ValueError):
    """The producer/consumer contract was not satisfied."""


def require(condition, message):
    if not condition:
        raise ContractError(message)


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'),
                       ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def write_json(path, value):
    Path(path).write_bytes(canonical(value))


def read_json(path):
    path = Path(path)
    require(path.is_file() and not path.is_symlink(), 'missing regular JSON file: ' + str(path))
    require(path.stat().st_size <= MAX_JSON, 'JSON size limit exceeded')
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'duplicate JSON key')
            result[key] = value
        return result
    def constant(_value):
        raise ContractError('non-finite JSON value')
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=pairs, parse_constant=constant)


def command(args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, stderr=subprocess.PIPE, timeout=30).decode().strip()


def git(root, *args):
    return command(['git', '--no-optional-locks', '-C', str(root), *args])


def changed_fixture(original, case):
    path, old, new = FIXTURES[case]
    if old is None:
        return original + new.encode()
    require(original.count(old.encode()) == 1, 'fixture replacement is not unique: ' + path)
    return original.replace(old.encode(), new.encode(), 1)


def input_identity(root, image, workflow, case='C1'):
    root = Path(root).resolve()
    require(platform.system() == 'Linux' and platform.machine() == 'x86_64', 'only native Linux amd64 is supported')
    require(re.fullmatch(r'ghcr\.io/[a-z0-9_./-]+@sha256:[0-9a-f]{64}', image), 'image must be pinned by digest')
    require(workflow in ('.github/workflows/build.yml', '.github/workflows/depends-cache-probe.yml'), 'unsupported workflow')
    require(case in ('C1', 'C2', 'C3', 'C4'), 'unsupported case')
    overrides = {name: os.environ.get(name, '') for name in OVERRIDES}
    require(not any(overrides.values()), 'unsupported depends environment override')
    # Other make-variable prefixes may override the HOST-only recipe as well.
    require(not any(value and (name.startswith(('build_', 'host_', 'linux_', 'x86_64_', 'i686_')))
                    for name, value in os.environ.items()), 'unsupported make-variable override')
    raw = subprocess.check_output(['git', '--no-optional-locks', '-C', str(root),
                                   'ls-tree', '-rz', 'HEAD', '--', 'depends'])
    sources = []
    for entry in raw.split(b'\0'):
        if not entry:
            continue
        meta, name = entry.split(b'\t', 1)
        mode, kind, blob = meta.decode().split()
        name = name.decode('utf-8')
        path = root / name
        require(kind == 'blob' and mode in ('100644', '100755'), 'unsupported depends source type')
        require(path.is_file() and not path.is_symlink(), 'missing depends source: ' + name)
        require(bool(path.stat().st_mode & 0o111) == (mode == '100755'), 'source executable mode mismatch: ' + name)
        data = path.read_bytes()
        expected = subprocess.check_output(['git', '--no-optional-locks', '-C', str(root), 'cat-file', 'blob', blob])
        if case in FIXTURES and name == FIXTURES[case][0]:
            expected = changed_fixture(expected, case)
        require(data == expected, 'unexpected depends source delta: ' + name)
        sources.append({'path': name, 'mode': mode, 'sha256': digest(data)})
    require(sources, 'empty tracked depends inventory')
    configs = []
    for name in (*CONFIG, workflow):
        path = root / name
        require(path.is_file() and not path.is_symlink(), 'missing config: ' + name)
        configs.append({'path': name, 'mode': stat.S_IMODE(path.stat().st_mode), 'sha256': file_digest(path)})
    tools = {}
    for name in ('gcc', 'g++', 'ar', 'ranlib', 'strip', 'nm', 'objcopy', 'objdump',
                 'make', 'cmake', 'tar', 'git', 'python3', 'sha256sum', 'pkg-config', 'zstd'):
        executable = shutil.which(name)
        if executable is None and name == 'zstd':
            tools[name] = None
            continue
        require(executable is not None, 'missing tool: ' + name)
        tools[name] = {'path': str(Path(executable).resolve()),
                       'version': command([executable, '--version'])}
    build = command([str(root / 'depends/config.guess')], cwd=root / 'depends')
    require(build == HOST, 'unexpected build triplet: ' + build)
    return {'schema': SCHEMA, 'sources': sources, 'configs': configs,
            'image': image, 'platform': 'linux/amd64', 'actions': ACTIONS,
            'tools': tools, 'build': build, 'host': HOST, 'target': TARGET,
            'root': str(root), 'prefix': str(root / 'depends' / HOST),
            'recipe': ['make', '-j4', '-C', 'depends', 'HOST=' + HOST],
            'environment': overrides}


def binding(root, identity, producer_job):
    keys = ('GITHUB_REPOSITORY', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT',
            'GITHUB_WORKFLOW_REF', 'GITHUB_WORKFLOW_SHA', 'GITHUB_SHA')
    context = {name: os.environ.get(name, '') for name in keys}
    require(all(context.values()), 'missing workflow binding context')
    require(context['GITHUB_RUN_ID'].isdigit() and context['GITHUB_RUN_ATTEMPT'].isdigit(), 'invalid run binding')
    require(re.fullmatch(r'[0-9a-f]{40}', context['GITHUB_SHA']) and
            re.fullmatch(r'[0-9a-f]{40}', context['GITHUB_WORKFLOW_SHA']), 'invalid source binding')
    require(git(root, 'rev-parse', 'HEAD') == context['GITHUB_SHA'], 'checkout HEAD does not match workflow SHA')
    require(re.fullmatch(r'[a-zA-Z0-9_-]+', producer_job), 'invalid producer job')
    return {'schema': SCHEMA, 'context': context, 'producer_job': producer_job,
            'source_tree': git(root, 'rev-parse', 'HEAD^{tree}'),
            'host': HOST, 'target': TARGET, 'input_sha256': digest(canonical(identity))}


def relative_link(name, target, logical_prefix):
    require(target and '\x00' not in target, 'invalid symlink target')
    if target.startswith('/'):
        logical = str(logical_prefix).rstrip('/') + '/'
        require(target == str(logical_prefix) or target.startswith(logical), 'symlink escapes prefix')
        value = target[len(logical):] if target.startswith(logical) else '.'
    else:
        value = str(PurePosixPath(name).parent / target)
    return value


def validate_entries(entries, logical_prefix):
    require(isinstance(entries, list) and 0 < len(entries) <= MAX_ENTRIES, 'invalid prefix entry count')
    by_name = {}
    total = 0
    for row in entries:
        name = row['path']
        require(isinstance(name, str) and name not in by_name and '\x00' not in name, 'duplicate or invalid prefix path')
        require(name == '.' or (not name.startswith('/') and '..' not in PurePosixPath(name).parts
                                and str(PurePosixPath(name)) == name), 'unsafe prefix path')
        require(type(row['mode']) is int and 0 <= row['mode'] <= 0o777, 'unsafe permission mode')
        require(row['type'] in ('directory', 'file', 'symlink'), 'unsupported entry type')
        if row['type'] == 'file':
            require(type(row['bytes']) is int and row['bytes'] >= 0, 'invalid file size')
            require(re.fullmatch(r'[0-9a-f]{64}', row['sha256']), 'invalid file hash')
            total += row['bytes']
        by_name[name] = row
    require(total <= MAX_PAYLOAD, 'unpacked payload exceeds limit')
    require(by_name.get('.', {}).get('type') == 'directory', 'missing prefix root')
    for name, row in by_name.items():
        if name == '.':
            continue
        require(by_name.get(str(PurePosixPath(name).parent), {}).get('type') == 'directory', 'non-directory prefix parent')
        if row['type'] == 'symlink':
            # Preserve '..' until AFTER expanding each preceding symlink. Lexical
            # normpath first would not implement the filesystem's link semantics.
            pending = list(PurePosixPath(relative_link(name, row['target'], logical_prefix)).parts)
            resolved = []
            links = 0
            while pending:
                part = pending.pop(0)
                if part == '.':
                    continue
                if part == '..':
                    require(resolved, 'symlink escapes prefix')
                    resolved.pop()
                    continue
                target = '/'.join(resolved + [part])
                ref = by_name.get(target)
                require(ref is not None, 'dangling symlink')
                if ref['type'] == 'symlink':
                    links += 1
                    require(links <= 40, 'cyclic symlink')
                    expansion = relative_link(target, ref['target'], logical_prefix)
                    pending = list(PurePosixPath(expansion).parts) + pending
                    resolved = []
                else:
                    require(not pending or ref['type'] == 'directory', 'symlink traverses non-directory')
                    resolved.append(part)
    require(by_name.get('share/config.site', {}).get('type') == 'file', 'missing config.site')
    require(any(name.startswith('.stamp_') and '/' not in name and row['type'] == 'file'
                for name, row in by_name.items()), 'missing prefix stamp')
    require([row['path'] for row in entries] == sorted(by_name), 'prefix entries must be sorted')
    return by_name


def prefix_manifest(prefix, logical_prefix=None):
    prefix = Path(prefix)
    logical_prefix = logical_prefix or prefix
    require(prefix.is_dir() and not prefix.is_symlink(), 'missing regular prefix directory')
    entries = []
    def visit(path, name):
        st = path.lstat()
        row = {'path': name, 'mode': stat.S_IMODE(st.st_mode)}
        if stat.S_ISLNK(st.st_mode):
            row.update(type='symlink', target=os.readlink(path))
        elif stat.S_ISDIR(st.st_mode):
            row['type'] = 'directory'
        elif stat.S_ISREG(st.st_mode):
            row.update(type='file', bytes=st.st_size, sha256=file_digest(path))
        else:
            raise ContractError('unsupported prefix file type: ' + name)
        entries.append(row)
        if row['type'] == 'directory':
            for child in sorted(path.iterdir()):
                visit(child, child.name if name == '.' else name + '/' + child.name)
    visit(prefix, '.')
    entries.sort(key=lambda row: row['path'])
    validate_entries(entries, logical_prefix)
    return {'schema': SCHEMA, 'entries': entries}


def pack(prefix, output, identity, expected_binding):
    prefix, output = Path(prefix), Path(output)
    require(str(prefix) == identity['prefix'], 'producer prefix path mismatch')
    manifest = prefix_manifest(prefix)
    output.mkdir(parents=True, exist_ok=False)
    for name, value in [('input.json', identity), ('binding.json', expected_binding),
                        ('prefix-manifest.json', manifest)]:
        write_json(output / name, value)
    with tarfile.open(output / 'prefix.tar.gz', 'w:gz', format=tarfile.PAX_FORMAT) as archive:
        for row in manifest['entries']:
            member = tarfile.TarInfo('prefix' if row['path'] == '.' else 'prefix/' + row['path'])
            member.mode = row['mode']
            if row['type'] == 'directory':
                member.type = tarfile.DIRTYPE
            elif row['type'] == 'symlink':
                member.type, member.linkname = tarfile.SYMTYPE, row['target']
            else:
                member.size = row['bytes']
            if row['type'] == 'file':
                with (prefix / row['path']).open('rb') as stream:
                    archive.addfile(member, stream)
            else:
                archive.addfile(member)
    require((output / 'prefix.tar.gz').stat().st_size <= MAX_ARCHIVE, 'archive exceeds size limit')
    require(prefix_manifest(prefix) == manifest, 'prefix changed during packaging')
    envelope = {'schema': SCHEMA, 'files': {name: {'sha256': file_digest(output / name),
                'bytes': (output / name).stat().st_size} for name in
                ('input.json', 'binding.json', 'prefix-manifest.json', 'prefix.tar.gz')}}
    write_json(output / 'envelope.json', envelope)
    return file_digest(output / 'envelope.json')


def receive(bundle, destination, identity, expected_binding, envelope_sha256):
    bundle, destination = Path(bundle), Path(destination)
    require(str(destination) == identity['prefix'], 'consumer prefix path mismatch')
    require(not destination.exists() and not destination.is_symlink(), 'destination prefix already exists')
    require(re.fullmatch(r'[0-9a-f]{64}', envelope_sha256 or ''), 'missing producer envelope digest')
    envelope = read_json(bundle / 'envelope.json')
    require(file_digest(bundle / 'envelope.json') == envelope_sha256, 'producer envelope digest mismatch')
    names = {'input.json', 'binding.json', 'prefix-manifest.json', 'prefix.tar.gz'}
    require(type(envelope['schema']) is int and envelope['schema'] == SCHEMA
            and set(envelope['files']) == names, 'invalid envelope schema')
    for name, row in envelope['files'].items():
        path = bundle / name
        require(path.is_file() and not path.is_symlink(), 'missing regular payload: ' + name)
        require(path.stat().st_size == row['bytes'] and file_digest(path) == row['sha256'], 'payload digest mismatch: ' + name)
    require((bundle / 'prefix.tar.gz').stat().st_size <= MAX_ARCHIVE, 'archive exceeds size limit')
    require(canonical(read_json(bundle / 'input.json')) == canonical(identity), 'consumer input mismatch')
    require(canonical(read_json(bundle / 'binding.json')) == canonical(expected_binding), 'producer run binding mismatch')
    manifest = read_json(bundle / 'prefix-manifest.json')
    require(type(manifest['schema']) is int and manifest['schema'] == SCHEMA, 'unknown prefix manifest schema')
    rows = validate_entries(manifest['entries'], destination)
    with tarfile.open(bundle / 'prefix.tar.gz', 'r:gz') as archive:
        members = []
        seen = set()
        for member in archive:
            require(len(members) < MAX_ENTRIES, 'archive entry limit exceeded')
            name = member.name
            require(name == 'prefix' or name.startswith('prefix/'), 'unsafe archive root')
            relative = '.' if name == 'prefix' else name[len('prefix/'):]
            require(relative in rows and relative not in seen, 'unexpected or duplicate archive path')
            row = rows[relative]
            expected_type = {'file': tarfile.REGTYPE, 'directory': tarfile.DIRTYPE, 'symlink': tarfile.SYMTYPE}[row['type']]
            require(member.type == expected_type and member.mode == row['mode'], 'archive type/mode mismatch')
            require(member.size == row.get('bytes', 0), 'archive size mismatch')
            require(member.linkname == row.get('target', ''), 'archive link mismatch')
            members.append((member, relative))
            seen.add(relative)
        require(seen == set(rows), 'incomplete archive')
        stage = Path(tempfile.mkdtemp(prefix='.depends-receive-', dir=destination.parent))
        staged_prefix = stage / 'prefix'
        try:
            # Create all real directories before files, and links last. Never traverse a link.
            directories = sorted((row for row in rows.values() if row['type'] == 'directory'),
                                 key=lambda row: (row['path'].count('/'), row['path']))
            for row in directories:
                path = staged_prefix if row['path'] == '.' else staged_prefix / row['path']
                path.mkdir(mode=0o700)
            for member, relative in members:
                if rows[relative]['type'] == 'file':
                    path = staged_prefix / relative
                    with archive.extractfile(member) as source, path.open('xb') as target:
                        shutil.copyfileobj(source, target, 1024 * 1024)
                    path.chmod(rows[relative]['mode'])
            for row in rows.values():
                if row['type'] == 'symlink':
                    (staged_prefix / row['path']).symlink_to(row['target'])
            for row in reversed(directories):
                path = staged_prefix if row['path'] == '.' else staged_prefix / row['path']
                path.chmod(row['mode'])
            require(prefix_manifest(staged_prefix, destination) == manifest, 'extracted prefix digest mismatch')
            require(not destination.exists() and not destination.is_symlink(), 'destination appeared during verification')
            staged_prefix.rename(destination)
        finally:
            # Only this function's private staging directory is removed. Restore
            # owner access first if an untrusted manifest supplied read-only modes.
            for current, directories, _files in os.walk(stage, followlinks=False):
                Path(current).chmod(0o700)
                for directory in directories:
                    path = Path(current) / directory
                    if not path.is_symlink():
                        path.chmod(0o700)
            shutil.rmtree(stage)
    return digest(canonical(manifest))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('identity', 'pack', 'receive', 'measure'))
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--image', required=True)
    parser.add_argument('--workflow', default='.github/workflows/build.yml')
    parser.add_argument('--case', default='C1', choices=('C1', 'C2', 'C3', 'C4'))
    parser.add_argument('--identity', type=Path, required=True)
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--producer-job', default='build-depends')
    parser.add_argument('--envelope-sha256')
    args = parser.parse_args()
    identity = input_identity(args.root, args.image, args.workflow, args.case)
    if args.operation == 'identity':
        write_json(args.identity, identity)
        values = {'input_sha256': digest(canonical(identity))}
    else:
        require(read_json(args.identity) == identity, 'inputs changed after initial identity capture')
        require(args.bundle is not None, '--bundle is required')
        run_binding = binding(args.root, identity, args.producer_job)
        prefix = Path(identity['prefix'])
        if args.operation == 'pack':
            values = {'envelope_sha256': pack(prefix, args.bundle, identity, run_binding)}
        elif args.operation == 'receive':
            values = {'prefix_sha256': receive(args.bundle, prefix, identity, run_binding, args.envelope_sha256)}
        else:
            args.bundle.mkdir(parents=True, exist_ok=False)
            manifest = prefix_manifest(prefix)
            write_json(args.bundle / 'input.json', identity)
            write_json(args.bundle / 'binding.json', run_binding)
            write_json(args.bundle / 'prefix-manifest.json', manifest)
            values = {'prefix_sha256': digest(canonical(manifest))}
    output = os.environ.get('GITHUB_OUTPUT')
    if output:
        with Path(output).open('a', encoding='utf-8') as stream:
            for key, value in values.items():
                stream.write(key + '=' + value + '\n')
    print(json.dumps(values, sort_keys=True))


if __name__ == '__main__':
    try:
        main()
    except (ContractError, OSError, KeyError, TypeError, json.JSONDecodeError,
            tarfile.TarError, subprocess.SubprocessError) as error:
        print('depends prefix rejected: ' + str(error), file=sys.stderr)
        sys.exit(1)
