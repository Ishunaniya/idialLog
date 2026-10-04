"""Offline WebKit report layout/DOM smoke; run under Xvfb with system Python."""
import json
import sys
from pathlib import Path
import gi

gi.require_version('Gtk', '3.0')
gi.require_version('WebKit2', '4.1')
from gi.repository import Gtk, WebKit2, GLib

path = Path(sys.argv[1]).resolve()
width = int(sys.argv[2]) if len(sys.argv) > 2 else 1280
window = Gtk.Window()
window.set_default_size(width, 1000)
web = WebKit2.WebView()
web.get_settings().set_hardware_acceleration_policy(WebKit2.HardwareAccelerationPolicy.NEVER)
window.add(web)
window.show_all()
failed = True

def stop(message):
    print(message, flush=True)
    Gtk.main_quit()

def snapshot_done(view, result, _):
    global failed
    try:
        surface = view.get_snapshot_finish(result)
        surface.write_to_png(str(path.with_suffix(f'.{width}.png')))
        failed = False
        stop('PASS offline browser DOM, point counts, shared axes, responsive layout and screenshot')
    except Exception as exc:
        stop(f'FAIL screenshot: {exc}')

def checked(view, result, _):
    try:
        value = json.loads(view.evaluate_javascript_finish(result).to_string())
        print(json.dumps(value, ensure_ascii=False), flush=True)
        assert value['version'] and value['charts'] == 5
        assert value['counts'] == value['markers']
        assert value['axesSame'] and value['noBodyOverflow'] and value['externalResources'] == 0
        if '.range.' in path.name:
            assert value['counts'] == [67, 30, 30, 30, 30]
        else:
            assert value['counts'] == [1359, 563, 563, 516, 516]
        assert value['chartScroll'] == (width < 1000)
        view.get_snapshot(WebKit2.SnapshotRegion.VISIBLE, WebKit2.SnapshotOptions.NONE, None, snapshot_done, None)
    except Exception as exc:
        stop(f'FAIL report DOM/layout: {exc}')

def loaded(view, event):
    if event != WebKit2.LoadEvent.FINISHED:
        return
    # All assertions inspect the actual exported HTML and browser-computed layout.
    source = '''(() => {
      const charts = [...document.querySelectorAll('.signal-chart')];
      const axes = charts.map(c => [...c.querySelectorAll('svg > text.chart-label')].filter(t => t.querySelector('tspan')).map(t => t.textContent).join('|'));
      window.scrollTo(0, charts[0].getBoundingClientRect().top + window.scrollY - 16);
      return JSON.stringify({version:document.body.textContent.includes('dialLog v1.12.0'),charts:charts.length,
        counts:charts.map(c=>Number(c.dataset.drawn)),markers:charts.map(c=>c.querySelectorAll('circle.sample').length),
        axesSame:axes.every(a=>a===axes[0]),noBodyOverflow:document.documentElement.scrollWidth<=innerWidth,
        viewport:innerWidth,documentWidth:document.documentElement.scrollWidth,
        overflowing:[...document.querySelectorAll('body *')].filter(e=>e.getBoundingClientRect().right>innerWidth+1 &&
          !e.closest('.chart-scroll') && !e.closest('table')).slice(0,12).map(e=>({tag:e.tagName,cls:e.className,
          right:Math.round(e.getBoundingClientRect().right),text:e.textContent.slice(0,100)})),
        textOverflow:[...document.querySelectorAll('.kpi b,.finding h3,.finding p,header,.card > p')]
          .filter(e=>e.scrollWidth>e.clientWidth+1).map(e=>({tag:e.tagName,cls:e.className,width:e.clientWidth,
            scroll:e.scrollWidth,text:e.textContent.slice(0,120)})),
        externalResources:document.querySelectorAll('script[src],link[href],img[src]').length,
        chartScroll:charts[0].querySelector('.chart-scroll').scrollWidth>charts[0].querySelector('.chart-scroll').clientWidth});
    })()'''
    GLib.timeout_add(250, lambda: (view.evaluate_javascript(source, -1, None, None, None, checked, None), False)[1])

web.connect('load-changed', loaded)
web.connect('load-failed', lambda *_: (stop('FAIL load'), True)[1])
GLib.timeout_add_seconds(60, lambda: (stop('FAIL browser timeout'), False)[1])
web.load_uri(path.as_uri())
Gtk.main()
sys.exit(1 if failed else 0)
