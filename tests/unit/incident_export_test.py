"""Read exports with independent ZIP/JSON/CSV implementations."""
# build/ 内的输入由 incidenttest 测试生成，可删除；清理后先运行 make check，再单独执行本脚本。
import csv
import io
import json
import re
import zipfile
from pathlib import Path

version_header = (Path(__file__).resolve().parents[2] / "version.h").read_text()
expected_version = ".".join(re.search(r"^#define DL_VER_" + part + r"\s+(\d+)$", version_header, re.M).group(1)
                            for part in ("MAJOR", "MINOR", "PATCH"))

root = Path('build/tests/unit')
with zipfile.ZipFile(root / 'incident-fixture.zh.zip') as archive:
    assert archive.testzip() is None
    assert set(archive.namelist()) == {'manifest.json', 'report.md', 'events.csv', 'metrics.csv',
                                      'parsed-evidence.jsonl', 'bookmarks.jsonl'}
    manifest = json.loads(archive.read('manifest.json'))
    assert (manifest['start_line'], manifest['end_line'], manifest['rows'],
            manifest['events'], manifest['samples']) == (4, 6, 4, 3, 1)
    assert manifest['version'] == expected_version
    assert manifest['language'] == 'zh-CN' and manifest['original_bytes_included'] is False
    assert manifest['sources'] == [{'index': 1, 'label': 'A.log', 'raw_line_offset': 0}]
    raw_evidence = archive.read('parsed-evidence.jsonl')
    evidence = [json.loads(row) for row in raw_evidence.splitlines()]
    assert [row['merged_line'] for row in evidence] == [4, 5, 6, 7]
    assert evidence[1]['message'].endswith('\n=cmd|"test"\t')
    assert all(row['source_line'] == row['merged_line'] for row in evidence)
    bookmarks = [json.loads(row) for row in archive.read('bookmarks.jsonl').splitlines()]
    assert len(bookmarks) == 1 and bookmarks[0]['merged_line'] == 6
    events = list(csv.DictReader(io.StringIO(archive.read('events.csv').decode('utf-8-sig'))))
    metrics = list(csv.DictReader(io.StringIO(archive.read('metrics.csv').decode('utf-8-sig'))))
    assert [int(row['merged_line']) for row in events] == [4, 5, 6]
    assert events[1]['message'] == evidence[1]['message']
    assert len(metrics) == 1 and metrics[0]['csq'] == '20' and metrics[0]['merged_line'] == '7'
    assert len(metrics[0]) == 27 and None not in metrics[0]
with zipfile.ZipFile(root / 'incident-fixture.en.zip') as archive:
    assert json.loads(archive.read('manifest.json'))['language'] == 'en-US'
    assert 'Incident review' in archive.read('report.md').decode()
    assert archive.read('parsed-evidence.jsonl') == raw_evidence
print('incident_export: ZIP, JSON, CSV, bilingual reports and exact raw messages verified')
