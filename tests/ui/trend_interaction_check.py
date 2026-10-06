"""Check aggregate-period tooltip semantics in the actual offline WebKit page."""
import json
import sys
from pathlib import Path
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('WebKit2', '4.1')
from gi.repository import Gtk, WebKit2, GLib

window = Gtk.Window()
window.set_default_size(1000, 900)
view = WebKit2.WebView()
view.get_settings().set_hardware_acceleration_policy(WebKit2.HardwareAccelerationPolicy.NEVER)
window.add(view)
window.show_all()
passed = False
script = r'''(() => {
 const figures=[...document.querySelectorAll('.signal-chart')];
 const first=figures.find(f=>f.querySelector('svg'));
 const svg=first.querySelector('svg'), d=JSON.parse(first.querySelector('.chart-data').textContent);
 const tooltip=document.querySelector('.chart-hover');
 function hover(period){const p=svg.createSVGPoint();p.x=64+((period[0]+period[1])/2-d.start)/Math.max(1,d.end-d.start)*750;p.y=120;
  const screen=p.matrixTransform(svg.getScreenCTM());svg.dispatchEvent(new PointerEvent('pointermove',{bubbles:true,clientX:screen.x,clientY:screen.y}));return tooltip.textContent;}
 const period=d.periods.find(p=>p[2]>0), empty=d.periods.find(p=>p[2]===0);
 if(!period||!empty)throw Error('Fixture needs an observed and an empty period');
 const text=hover(period), missing=hover(empty);
 const start=new Date(period[0]*1000).toISOString().slice(0,19).replace('T',' ');
 const end=new Date(period[1]*1000).toISOString().slice(0,19).replace('T',' ');
 const result={periodMedian:text.includes(String(period[3]/d.scale)),sampleCount:text.includes('n='+period[2]),interval:text.includes(start)&&text.includes(end),p50:text.includes('P50'),allMetrics:figures.every(f=>text.includes(f.querySelector('strong').textContent)),missing:missing.includes('无采样')||missing.includes('No samples'),external:document.querySelectorAll('script[src],link[href],img[src]').length,text,emptyTooltip:missing};
 return JSON.stringify(result);
})()'''
def checked(web, result, _):
    global passed
    try:
        data = json.loads(web.evaluate_javascript_finish(result).to_string())
        print(json.dumps(data, ensure_ascii=False), flush=True)
        assert all(data[key] for key in ('periodMedian','sampleCount','interval','p50','allMetrics','missing'))
        assert data['external'] == 0
        passed = True
        print('PASS period P50 uses interval and sample counts; empty periods stay missing', flush=True)
    except Exception as error:
        print(f'FAIL aggregate tooltip: {error}', flush=True)
    Gtk.main_quit()
def loaded(web, event):
    if event == WebKit2.LoadEvent.FINISHED:
        GLib.timeout_add(250, lambda: (web.evaluate_javascript(script,-1,None,None,None,checked,None),False)[1])
view.connect('load-changed',loaded)
GLib.timeout_add_seconds(60,lambda: (print('FAIL browser timeout',flush=True),Gtk.main_quit(),False)[2])
view.load_uri(Path(sys.argv[1]).resolve().as_uri())
Gtk.main()
sys.exit(0 if passed else 1)
