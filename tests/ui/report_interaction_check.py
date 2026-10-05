"""Exercise offline chart interactions in the actual WebKit engine."""
import json
import sys
from pathlib import Path
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('WebKit2', '4.1')
from gi.repository import Gtk, WebKit2, GLib
path = Path(sys.argv[1]).resolve()
window = Gtk.Window()
window.set_default_size(1280, 1000)
view = WebKit2.WebView()
view.get_settings().set_hardware_acceleration_policy(WebKit2.HardwareAccelerationPolicy.NEVER)
window.add(view)
window.show_all()
passed = False
script = r'''(() => {
  const figures=[...document.querySelectorAll('.signal-chart')];
  const c=figures.find(f=>f.querySelector('svg')&&f.querySelector('.chart-data'));
  const svg=c.querySelector('svg'), data=JSON.parse(c.querySelector('.chart-data').textContent);
  const tools=document.querySelector('.chart-tools'), label=tools.querySelector('span');
  const initial=label.textContent;
  const singleton=data.start===data.end;
  function screen(x,y){const p=svg.createSVGPoint();p.x=x;p.y=y;return p.matrixTransform(svg.getScreenCTM());}
  function pointer(type,x,y,button=0){const p=screen(x,y);svg.dispatchEvent(new PointerEvent(type,{bubbles:true,clientX:p.x,clientY:p.y,button,pointerId:3}));}
  let point=screen(440,120);
  svg.dispatchEvent(new WheelEvent('wheel',{bubbles:true,cancelable:true,clientX:point.x,clientY:point.y,deltaY:-120}));
  const zoomed=(singleton?label.textContent===initial:label.textContent!==initial)&&svg.querySelector('.interactive-curves')!==null;
  const axes=[...document.querySelectorAll('.interactive-ticks')].map(g=>g.textContent);
  const shared=axes.length===figures.filter(f=>f.querySelector('svg')).length&&axes.every(a=>a===axes[0]);
  const zoomLabel=label.textContent;
  svg.dispatchEvent(new WheelEvent('wheel',{bubbles:true,cancelable:true,clientX:point.x,clientY:point.y,deltaY:120,shiftKey:true}));
  const panned=singleton?label.textContent===zoomLabel:label.textContent!==zoomLabel;
  tools.querySelector('button').click();
  pointer('pointerdown',210,120);pointer('pointermove',670,120);pointer('pointerup',670,120);
  const selected=singleton?label.textContent===initial:label.textContent!==initial;
  const eventRows=[...document.querySelectorAll('tr[data-event-start]')];
  const eventFiltered=!eventRows.length||(eventRows.some(r=>r.hidden)&&eventRows.some(r=>!r.hidden));
  tools.querySelector('button').click();
  const reset=label.textContent===initial;
  const sample=data.points[Math.floor(data.points.length/2)];
  const x=64+(sample[0]-data.start)/Math.max(1,data.end-data.start)*750;
  pointer('pointermove',x,120);
  const tooltip=document.querySelector('.chart-hover').textContent;
  const exactSample=tooltip.includes(String(sample[1]/data.scale));
  const nearAll=figures.every(f=>tooltip.includes(f.querySelector('strong').textContent));
  const relativeDates=!data.relative||tooltip.includes(new Date((data.originalStart+sample[0])*1000).toISOString().slice(0,19).replace('T',' '));
  const missing=figures.every(f=>JSON.parse(f.querySelector('.chart-data').textContent).points.length || tooltip.includes('附近无采样') || tooltip.includes('No nearby sample'));
  const band=svg.querySelector('.outage-band');
  if(band)band.dispatchEvent(new KeyboardEvent('keydown',{bubbles:true,key:'Enter'}));
  const eventLinked=!band||eventRows.some(r=>r.classList.contains('selected-event')&&r.dataset.eventStart===band.dataset.eventStart);
  const beforeMiddle=label.textContent;
  svg.dispatchEvent(new WheelEvent('wheel',{bubbles:true,cancelable:true,clientX:point.x,clientY:point.y,deltaY:-120}));
  const middleStart=label.textContent;pointer('pointerdown',400,120,1);pointer('pointermove',480,120,1);pointer('pointerup',480,120,1);
  const middlePan=singleton?label.textContent===middleStart:label.textContent!==middleStart;
  document.dispatchEvent(new KeyboardEvent('keydown',{bubbles:true,key:'0',ctrlKey:true}));
  const keyboardReset=label.textContent===initial;
  const external=document.querySelectorAll('script[src],link[href],img[src]').length;
  return JSON.stringify({zoomed,shared,panned,selected,reset,exactSample,nearAll,relativeDates,missing,eventFiltered,eventLinked,eventCount:eventRows.length,singleton,middlePan,keyboardReset,external,tooltip});
})()'''
def checked(web, result, _):
    global passed
    try:
        value = json.loads(web.evaluate_javascript_finish(result).to_string())
        print(json.dumps(value, ensure_ascii=False), flush=True)
        assert all(value[key] for key in ('zoomed', 'shared', 'panned', 'selected', 'reset',
                                          'exactSample', 'nearAll', 'relativeDates', 'missing', 'eventFiltered', 'eventLinked', 'middlePan', 'keyboardReset'))
        assert value['external'] == 0
        passed = True
        print('PASS offline wheel/pan/selection/reset, shared axes and exact all-metric hover', flush=True)
    except Exception as error:
        print(f'FAIL interaction: {error}', flush=True)
    Gtk.main_quit()
def loaded(web, event):
    if event == WebKit2.LoadEvent.FINISHED:
        GLib.timeout_add(250, lambda: (web.evaluate_javascript(script, -1, None, None, None, checked, None), False)[1])
view.connect('load-changed', loaded)
GLib.timeout_add_seconds(60, lambda: (print('FAIL browser timeout', flush=True), Gtk.main_quit(), False)[2])
view.load_uri(path.as_uri())
Gtk.main()
sys.exit(0 if passed else 1)
