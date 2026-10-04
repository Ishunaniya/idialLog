"""Validate standalone vector charts with an independent XML parser."""
from html.parser import HTMLParser
from pathlib import Path
import xml.etree.ElementTree as ET

class Charts(HTMLParser):
    def __init__(self):
        super().__init__()
        self.counts = []
    def handle_starttag(self, tag, attrs):
        if tag == 'figure':
            attrs = dict(attrs)
            self.counts.append((int(attrs['data-samples']), int(attrs['data-drawn'])))

for name in ('small', 'gaps', 'dense', 'real'):
    text = Path(f'build/tests/unit/report-chart-{name}.html').read_text()
    parser = Charts()
    parser.feed(text)
    svg = text[text.index('<svg '):text.index('</svg>') + 6]
    root = ET.fromstring(svg)
    points = root.findall('.//circle')
    assert len(points) == parser.counts[0][1], name
    assert all(point.find('title') is not None for point in points), name
    path = root.find('.//path').attrib['d']
    assert path.count('M') == (2 if name == 'gaps' else 1), name
    assert 'nan' not in path and 'inf' not in path
    if name == 'dense':
        assert parser.counts[0][0] == 1_000_000 and len(points) <= 8194
    else:
        assert parser.counts[0][0] == len(points)
print('report_chart: XML, exact point counts, gap paths, sample titles and dense budget verified')
