"""Independently inspect the native-generated complete evidence package."""
# build/ 内的输入由 workspacetest 测试生成，可删除；清理后先运行 make check，再单独执行本脚本。
import hashlib
import json
import zipfile
from pathlib import Path

path = Path('build/tests/unit/workspace-package.zip')
with zipfile.ZipFile(path) as archive:
    assert archive.testzip() is None
    index = json.loads(archive.read('package-index.json'))
    assert index['schema'] == 2 and index['tool'] == 'dialLog'
    assert set(index['files']) == set(archive.namelist()) - {'package-index.json'}
    for name, digest in index['files'].items():
        assert hashlib.sha256(archive.read(name)).hexdigest() == digest, name
    originals = index['originals']
    assert len(originals) == 2
    identities = {source['identity'] for source in originals}
    assert len(identities) == len(originals)
    a, b = (archive.read(source['path']) for source in originals)
    assert a.startswith(b'\xef\xbb\xbf') and a.count(b'\r\n') == 3
    assert b.count(b'\n') == 3 and b'\r' not in b
    for source in originals:
        assert hashlib.sha256(archive.read(source['path'])).hexdigest() == source['sha256']
    workspace = json.loads(archive.read('workspace.json'))
    assert set(workspace['devices']) <= identities
    assert index['selected_source'] in identities
    assert index['selected_device'] == '设备一'
    assert workspace['reviews'][0]['status'] == 1
    assert workspace['reviews'][0]['note'] == '人工复核\n<script>"note"'
    bookmark = index['bookmarks'][0]
    assert bookmark['source_hash'] == originals[0]['identity']
    assert bookmark['source_line'] == 2 and bookmark['text'] == '现场备注'
    assert json.loads(archive.read('manifest.json'))['original_bytes_included'] is True
print('PASS independent ZIP inventory/SHA-256, original BOM/CRLF, source identity, notes and bookmark checks')
