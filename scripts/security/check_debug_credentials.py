"""Reject private debug material in files destined for GitHub. Never echo values."""
import pathlib
import re
import subprocess
import sys

PRIVATE_PATH = re.compile(
    r"(?i)(?:^(?:local|build|release|reports|runtime-signaling[^/]*)/|"
    r"\.(?:dpapi|key|pfx|p12|reg)$|(?:^|/)(?:credentials?[^/]*\.json|auth\.json|\.env(?:\..*)?)$)"
)
FIXED_CODE = re.compile(
    r"(?i:\b\w*(?:session_code|SessionCode)\b)\s*(?:=\s*|\s+)['\"`]?([A-Z0-9]{8})(?![A-Za-z0-9])"
)
PASSWORD = re.compile(
    r"(?i)(?:\$(?:script:)?\w*(?:password|passphrase|pairing_secret)\w*\s*=|"
    r"[\"'](?:password|passphrase|pairing_secret)[\"']\s*:)\s*['\"]([^'\"]+)['\"]"
)


def inspect_file(path, data):
    """Return location/reason tuples, with no candidate value or file contents."""
    path = path.replace('\\', '/')
    if PRIVATE_PATH.search(path) or path.split('/')[0] in ('local', 'build', 'release', 'reports'):
        return [(path, 0, 'private credential/configuration file must remain local')]
    # Deterministic isolated fixtures must not be reused as live debug credentials.
    if path.startswith('tests/') or pathlib.PurePosixPath(path).name.startswith(('test-', 'test_')):
        return []
    if b'\0' in data[:8000]:
        return []
    issues = []
    for number, line in enumerate(data.decode('utf-8-sig', 'replace').splitlines(), 1):
        if FIXED_CODE.search(line):
            issues.append((path, number, 'fixed debug session code; use a private local value'))
        if any(not match[1].startswith(('$', '<')) for match in PASSWORD.finditer(line)):
            issues.append((path, number, 'literal password/passphrase; use protected local credentials'))
    return issues


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    files = subprocess.check_output(
        ['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=root
    ).decode('utf-8').split('\0')
    issues = []
    for relative in sorted(set(filter(None, files))):
        path = root / relative
        if path.is_file():
            issues.extend(inspect_file(relative, path.read_bytes()))
    for path, line, reason in issues:
        print(f'{path}:{line}: {reason}')
    if issues:
        print(f'Credential check failed: {len(issues)} finding(s); values withheld.')
        return 1
    print('Credential check passed: no private files or fixed live debug credentials found.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
