"""Independently validate the generated work-session ZIP and its restore state."""
# build/ is disposable; make check regenerates this input before this check.
import hashlib
import json
import zipfile
from pathlib import Path

with zipfile.ZipFile(Path('build/tests/unit/workspace-session.zip')) as archive:
    assert archive.testzip() is None
    index = json.loads(archive.read('package-index.json'))
    manifest = json.loads(archive.read('manifest.json'))
    state = json.loads(archive.read('session.json'))
    workspace = json.loads(archive.read('workspace.json'))
    assert manifest['kind'] == 'workspace-session' and manifest['original_bytes_included']
    assert set(index['files']) == set(archive.namelist()) - {'package-index.json'}
    for path, expected in index['files'].items():
        assert hashlib.sha256(archive.read(path)).hexdigest() == expected, path
    identities = {source['identity'] for source in index['originals']}
    assert len(identities) == 2 and len(index['originals']) == 2
    assert state['a'].removeprefix('source:') in identities
    assert state['b'] == state['board_scope'] == 'device:设备一'
    assert state['range_active'] and state['start'] == index['start'] and state['end'] == index['end']
    assert state['columns'] == 33 and state['cell'] == '01ABCDEF'
    assert state['has_deny'] and state['deny'] == 15
    assert state['metric'] == 4 and state['view_active'] and state['view_start'] == 1 and state['view_end'] == 3
    assert state['board_status'] == 2 and state['board_minimum'] == 2 and state['board_query'] == 'manual'
    assert len(workspace['reviews']) == 1 and workspace['reviews'][0]['note'].startswith('人工复核')
    assert index['bookmarks'][0]['source_line'] == 2
    assert archive.read(index['originals'][0]['path']).startswith(b'\xef\xbb\xbf')
print('PASS independent session ZIP inventory, originals, filters, scopes, plot view, notes and bookmarks')
