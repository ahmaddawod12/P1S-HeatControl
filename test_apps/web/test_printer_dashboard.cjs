const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../../web/printer.js'),'utf8');
const html=fs.readFileSync(path.join(__dirname,'../../web/index.html'),'utf8');
const appSource=fs.readFileSync(path.join(__dirname,'../../web/app.js'),'utf8');
const flush=async()=>{for(let i=0;i<6;++i)await new Promise(setImmediate);};
function browser(initial={}){
  const nodes=new Map(),timers=new Map(),calls=[],events=[],docEvents={};let seq=0,offline=false,lose=false,reply=202,hold=false,pending=[],holdDelete=false,pendingDeletes=[];
  let status={configured:true,enabled:true,access_code_set:true,ipv4:'192.168.1.20',serial:'SERIAL123',applying:false,config_result:'saved',
    connected:true,printer_state:'running',reconnects:0,can_monitor:true,csrf_token:'csrf-one',reason:'ready',
    monitor:{phase:'off',hold:'none',last_end:'none',wait_remaining_ms:0},...initial};
  const element=id=>{if(!nodes.has(id))nodes.set(id,{value:'',textContent:'',checked:false,disabled:false,hidden:false,listeners:{},
    setAttribute(k,v){this[k]=v;},removeAttribute(k){delete this[k];},addEventListener(k,fn){this.listeners[k]=fn;}});return nodes.get(id);};
  const document={hidden:false,getElementById:element,addEventListener(k,fn){docEvents[k]=fn;}};
  const context={document,console,TextEncoder,CustomEvent:class{constructor(type,o){this.type=type;this.detail=o.detail;}},
    dispatchEvent(e){events.push(e.detail);},setTimeout(fn,delay){const id=++seq;timers.set(id,{fn,delay});return id;},clearTimeout(id){timers.delete(id);},
    fetch:async(url,options={})=>{calls.push({url,options});if(options.method){if(lose)throw Error('lost response');const response={ok:reply>=200&&reply<300,status:reply,json:async()=>({error:'printer_conflict'})};
      if(holdDelete&&options.method==='DELETE')return new Promise(resolve=>pendingDeletes.push(()=>resolve(response)));return response;}
      if(offline)throw Error('offline');const snapshot=JSON.parse(JSON.stringify(status));if(hold)return new Promise(resolve=>pending.push(()=>resolve({ok:true,json:async()=>snapshot})));
      return {ok:true,json:async()=>snapshot};}};context.window=context;vm.runInNewContext(source,context,{filename:'printer.js'});
  return {element,calls,events,document,timers,set(v){status={...status,...v};},offline(v){offline=v;},lose(v){lose=v;},reply(v){reply=v;},hold(v){hold=v;},release(){pending.shift()();},
    holdDelete(v){holdDelete=v;},releaseDelete(){pendingDeletes.shift()();},
    click(id){element(id).listeners.click();},
    edit(id,value){element(id).value=value;element(id).listeners.input();},save(){element('printer-form').listeners.submit({preventDefault(){}});},
    visible(v){document.hidden=!v;docEvents.visibilitychange();},async poll(){const e=[...timers.entries()].at(-1);assert.ok(e,'poll scheduled');assert.equal(e[1].delay,1000);timers.delete(e[0]);e[1].fn();await flush();}};
}
const watching=(extra={})=>({can_monitor:false,reason:'monitoring',monitor:{phase:'watching',hold:'none',last_end:'none',wait_remaining_ms:0,...extra}});
test('local setup has accessible labels and no preheat, stop or job controls',()=>{
  for(const id of ['printer-ipv4','printer-serial','printer-access-code'])assert.match(html,new RegExp('<label[^>]*for="'+id+'"'));
  assert.doesNotMatch(html,/printer-ntp|Time server/);assert.doesNotMatch(source,/ntp/);
  assert.match(html,/<input[^>]*type="password"[^>]*id="printer-access-code"/);
  for(const id of ['printer-preheat','printer-stop','printer-job','printer-progress','printer-workflow'])assert.doesNotMatch(html,new RegExp('id="'+id+'"'));
  assert.doesNotMatch(html,/Stopping chamber control/);assert.doesNotMatch(source,/innerHTML/);
  assert.match(html,/<button type="button" id="monitor-button" aria-pressed="false" disabled>Monitor P1S<\/button>/);
  const enable=html.indexOf('id="enable-button"'),monitor=html.indexOf('id="monitor-button"'),disable=html.indexOf('id="disable-button"'),fan=html.indexOf('id="fan-toggle-button"');
  assert.ok(enable<monitor&&monitor<disable&&disable<fan,'heating row order');
  assert.match(html,/<p class="hint" id="monitor-status" role="status" aria-live="polite">/);
  assert.match(html,/id="printer-state"/);
  assert.doesNotMatch(appSource,/printerActive|p1s-printer-control/);
});
test('monitor button disabled until can_monitor',async()=>{
  const b=browser({can_monitor:false,reason:'heating_off'});await flush();
  assert.equal(b.element('monitor-button').disabled,true);
  assert.equal(b.element('monitor-status').textContent,'Enable heating to use Monitor P1S.');
  b.click('monitor-button');await flush();assert.equal(b.calls.filter(c=>c.options.method).length,0);
  b.set({can_monitor:true,reason:'ready'});await b.poll();
  assert.equal(b.element('monitor-button').disabled,false);
  assert.equal(b.element('monitor-status').textContent,'Press Monitor P1S to turn heating off automatically when the print ends.');
});
test('one click posts once to /api/printer/monitor with {}',async()=>{
  const b=browser();await flush();b.reply(204);
  b.click('monitor-button');b.click('monitor-button');await flush();
  const posts=b.calls.filter(c=>c.options.method==='POST');
  assert.equal(posts.length,1);assert.equal(posts[0].url,'/api/printer/monitor');
  assert.deepEqual(JSON.parse(posts[0].options.body),{});assert.equal(posts[0].options.headers['X-P1S-CSRF'],'csrf-one');
});
test('pressed state and stop',async()=>{
  const b=browser(watching());await flush();
  assert.equal(b.element('monitor-button')['aria-pressed'],'true');
  assert.equal(b.element('monitor-button').disabled,false);
  b.reply(204);b.click('monitor-button');await flush();
  const posts=b.calls.filter(c=>c.options.method==='POST');
  assert.equal(posts.length,1);assert.equal(posts[0].url,'/api/printer/monitor/stop');
  b.set({can_monitor:true,reason:'ready',monitor:{phase:'off',hold:'none',last_end:'stopped',wait_remaining_ms:0}});await b.poll();
  assert.equal(b.element('monitor-button')['aria-pressed'],'false');
});
test('status copy',async()=>{
  const off=(last_end,extra={})=>({reason:'ready',monitor:{phase:'off',hold:'none',last_end,wait_remaining_ms:0},...extra});
  const cases=[
    [watching(),'Monitoring the print. Heating turns off when it finishes.'],
    [watching({hold:'paused'}),'Heater paused: printer is paused.'],
    [watching({hold:'disconnected'}),'Heater paused: printer connection lost.'],
    [watching({hold:'unknown'}),'Heater paused: waiting for printer status.'],
    [{can_monitor:false,reason:'monitoring',monitor:{phase:'waiting',hold:'none',last_end:'none',wait_remaining_ms:2580000}},'Waiting for the print to start (stops in 43 min).'],
    [off('finished'),'Print finished: heating turned off.'],
    [off('failed'),'Print failed or was cancelled: heating turned off.'],
    [off('idle'),'Printer went idle: heating turned off.'],
    [off('no_print'),'No print started within 60 minutes: heating turned off.'],
    [off('stopped'),'Monitoring stopped. Heating continues.'],
    [off('heating_off',{reason:'heating_off',can_monitor:false}),'Monitoring ended because heating was turned off.'],
    [off('settings'),'Printer settings changed: heating turned off.'],
    [{reason:'disconnected',can_monitor:false},'Printer not connected.'],
    [{reason:'state_unknown',can_monitor:false},'Waiting for printer status.'],
    [{reason:'configuration_required',can_monitor:false},'Set up the printer connection below.'],
    [{reason:'printer_disabled',can_monitor:false},'Printer monitoring is disabled in printer settings.'],
  ];
  for(const [status,text] of cases){const b=browser(status);await flush();assert.equal(b.element('monitor-status').textContent,text,JSON.stringify(status));}
});
test('a rejected start explains itself and does not repeat',async()=>{
  const b=browser();await flush();b.reply(409);b.click('monitor-button');await flush();
  assert.equal(b.element('printer-message').textContent,'Monitor P1S is not available right now.');
  assert.equal(b.calls.filter(c=>c.options.method==='POST').length,1);
});
test('printer card shows connection and state',async()=>{
  const b=browser({printer_state:'paused'});await flush();
  assert.equal(b.element('printer-connection').textContent,'Connected');
  assert.equal(b.element('printer-state').textContent,'paused');
  b.set({connected:false});await b.poll();assert.equal(b.element('printer-connection').textContent,'Disconnected');
  b.offline(true);await b.poll();assert.equal(b.element('printer-connection').textContent,'Status unavailable');
  assert.equal(b.element('monitor-button').disabled,true);
});
test('polling preserves edits and never fills the write-only access code',async()=>{
  const b=browser({access_code:'must-never-display'});await flush();assert.equal(b.element('printer-access-code').value,'');
  b.edit('printer-ipv4','192.168.1.21');await b.poll();assert.equal(b.element('printer-ipv4').value,'192.168.1.21');
  b.save();await flush();const save=b.calls.find(c=>c.options.method==='PUT');assert.ok(save);assert.equal('access_code' in JSON.parse(save.options.body),false);
  assert.equal('ntp_host' in JSON.parse(save.options.body),false);
});

test('an unreadable optional configuration can be deliberately forgotten',async()=>{
  const b=browser({configured:false,enabled:false,access_code_set:false,ipv4:'',serial:'',
    can_monitor:false,reason:'configuration_required',config_result:'save_failed'});
  await flush();
  assert.equal(b.element('printer-forget').disabled,false);
  b.reply(204);b.click('printer-forget');await flush();
  const clears=b.calls.filter(c=>c.url==='/api/printer/config'&&c.options.method==='DELETE');
  assert.equal(clears.length,1);
  b.set({applying:true});await b.poll();
  assert.equal(b.element('printer-forget').disabled,true);
});
test('Forget preserves replacement connection edits entered before its reply',async()=>{
  const b=browser();await flush();b.holdDelete(true);b.reply(204);b.click('printer-forget');
  b.edit('printer-ipv4','192.168.1.50');b.edit('printer-serial','REPLACEMENT123');
  b.edit('printer-access-code','replacement-code');
  b.set({configured:false,enabled:false,access_code_set:false,ipv4:'',serial:''});
  b.releaseDelete();await flush();
  assert.equal(b.element('printer-ipv4').value,'192.168.1.50');
  assert.equal(b.element('printer-serial').value,'REPLACEMENT123');
  assert.equal(b.element('printer-access-code').value,'replacement-code');
});
test('Forget preserves an access-code-only edit entered before its reply',async()=>{
  const b=browser();await flush();b.holdDelete(true);b.reply(204);b.click('printer-forget');
  b.edit('printer-access-code','new-code');
  b.set({configured:false,enabled:false,access_code_set:false,ipv4:'',serial:''});
  b.releaseDelete();await flush();
  assert.equal(b.element('printer-access-code').value,'new-code');
  assert.equal(b.element('printer-ipv4').value,'192.168.1.20');
});
test('Forget clears an unchanged draft after its reply',async()=>{
  const b=browser();await flush();b.edit('printer-access-code','old-draft');
  b.holdDelete(true);b.reply(204);b.click('printer-forget');
  b.set({configured:false,enabled:false,access_code_set:false,ipv4:'',serial:''});
  b.releaseDelete();await flush();
  assert.equal(b.element('printer-access-code').value,'');
  assert.equal(b.element('printer-ipv4').value,'');assert.equal(b.element('printer-serial').value,'');
});
