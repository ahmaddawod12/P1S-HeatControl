const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../../web/printer.js'),'utf8');
const html=fs.readFileSync(path.join(__dirname,'../../web/index.html'),'utf8');
const flush=async()=>{for(let i=0;i<6;++i)await new Promise(setImmediate);};
function browser(initial={}){
  const nodes=new Map(),timers=new Map(),calls=[],events=[],docEvents={};let seq=0,offline=false,lose=false,reply=202,hold=false,pending=[],holdDelete=false,pendingDeletes=[];
  let status={configured:true,enabled:true,access_code_set:true,ipv4:'192.168.1.20',serial:'SERIAL123',ntp_host:'pool.ntp.org',applying:false,config_result:'saved',
    connected:true,fresh:true,baseline_valid:true,printer_state:'paused',job_label:'fixture.3mf',progress:0,remaining_minutes:null,can_arm:true,
    arm_token:'0123456789abcdef0123456789abcdef',csrf_token:'csrf-one',reason:'ready',cycle:{id:'0',state:'ready',outcome:'none',target_c:60},...initial};
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
    click(id){element(id).listeners.click();},confirm(){element('printer-confirm').checked=true;element('printer-confirm').listeners.change();},
    edit(id,value){element(id).value=value;element(id).listeners.input();},save(){element('printer-form').listeners.submit({preventDefault(){}});},
    visible(v){document.hidden=!v;docEvents.visibilitychange();},async poll(){const e=[...timers.entries()].at(-1);assert.ok(e,'poll scheduled');assert.equal(e[1].delay,1000);timers.delete(e[0]);e[1].fn();await flush();}};
}
test('local setup and explicit actions have accessible labels',()=>{
  for(const id of ['printer-ipv4','printer-serial','printer-access-code','printer-ntp'])assert.match(html,new RegExp('<label[^>]*for="'+id+'"'));
  assert.match(html,/<input[^>]*type="password"[^>]*id="printer-access-code"/);
  assert.doesNotMatch(html.match(/<input[^>]*id="printer-confirm"[^>]*>/)[0],/ checked/);
  assert.match(html,/Stopping chamber control does not cancel the print/);assert.doesNotMatch(source,/innerHTML/);
});
test('polling preserves edits and never fills the write-only access code',async()=>{
  const b=browser({access_code:'must-never-display'});await flush();assert.equal(b.element('printer-access-code').value,'');
  b.edit('printer-ipv4','192.168.1.21');await b.poll();assert.equal(b.element('printer-ipv4').value,'192.168.1.21');
  b.save();await flush();const save=b.calls.find(c=>c.options.method==='PUT');assert.ok(save);assert.equal('access_code' in JSON.parse(save.options.body),false);
});

test('an unreadable optional configuration can be deliberately forgotten',async()=>{
  const b=browser({configured:false,enabled:false,access_code_set:false,ipv4:'',serial:'',
    can_arm:false,arm_token:'',reason:'configuration_required',config_result:'save_failed'});
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
  b.edit('printer-access-code','replacement-code');b.edit('printer-ntp','ntp.lan');
  b.set({configured:false,enabled:false,access_code_set:false,ipv4:'',serial:'',ntp_host:''});
  b.releaseDelete();await flush();
  assert.equal(b.element('printer-ipv4').value,'192.168.1.50');
  assert.equal(b.element('printer-serial').value,'REPLACEMENT123');
  assert.equal(b.element('printer-access-code').value,'replacement-code');
  assert.equal(b.element('printer-ntp').value,'ntp.lan');
});
test('Forget preserves an access-code-only edit entered before its reply',async()=>{
  const b=browser();await flush();b.holdDelete(true);b.reply(204);b.click('printer-forget');
  b.edit('printer-access-code','new-code');
  b.set({configured:false,enabled:false,access_code_set:false,ipv4:'',serial:'',ntp_host:''});
  b.releaseDelete();await flush();
  assert.equal(b.element('printer-access-code').value,'new-code');
  assert.equal(b.element('printer-ipv4').value,'192.168.1.20');
});
test('Forget clears an unchanged draft after its reply',async()=>{
  const b=browser();await flush();b.edit('printer-access-code','old-draft');
  b.holdDelete(true);b.reply(204);b.click('printer-forget');
  b.set({configured:false,enabled:false,access_code_set:false,ipv4:'',serial:'',ntp_host:''});
  b.releaseDelete();await flush();
  assert.equal(b.element('printer-access-code').value,'');
  assert.equal(b.element('printer-ipv4').value,'');assert.equal(b.element('printer-serial').value,'');
});
test('arming requires confirmation and consumes one click even when its response is lost',async()=>{
  const b=browser();await flush();assert.equal(b.element('printer-preheat').textContent,'Preheat & Resume');assert.equal(b.element('printer-stop').textContent,'Stop chamber control');
  b.click('printer-preheat');assert.equal(b.calls.filter(c=>c.options.method==='POST').length,0);
  b.confirm();b.lose(true);b.click('printer-preheat');b.click('printer-preheat');await flush();await b.poll();await b.poll();
  const posts=b.calls.filter(c=>c.url==='/api/printer/cycle'&&c.options.method==='POST');assert.equal(posts.length,1);
  assert.deepEqual(JSON.parse(posts[0].options.body),{arm_token:'0123456789abcdef0123456789abcdef',initial_pause_confirmed:true});
  assert.match(b.element('printer-message').textContent,/not confirmed|uncertain/i);assert.doesNotMatch(b.element('printer-message').textContent,/started|success/i);
});
test('a stale GET cannot overwrite a later arm and only one GET runs at a time',async()=>{
  const b=browser();await flush();b.hold(true);await b.poll();b.visible(false);b.visible(true);
  const gets=b.calls.filter(c=>!c.options.method).length;assert.equal(gets,2);b.confirm();b.click('printer-preheat');await flush();
  b.release();await flush();assert.equal(b.element('printer-preheat').disabled,true);assert.equal(b.calls.filter(c=>!c.options.method).length,gets);
  b.hold(false);b.set({can_arm:false,arm_token:'',cycle:{id:'4',state:'preheating',outcome:'none',target_c:60}});await b.poll();
  assert.equal(b.events.at(-1).active,true);assert.match(b.element('printer-message').textContent,/Preheating/i);
});
test('hidden polling pauses and later pauses end automatic control even if printing resumes',async()=>{
  const b=browser({can_arm:false,cycle:{id:'2',state:'printing',outcome:'none',target_c:60},printer_state:'running',progress:100});await flush();
  assert.match(b.element('printer-message').textContent,/Printing/i);assert.doesNotMatch(b.element('printer-message').textContent,/completed/i);
  b.visible(false);assert.equal(b.timers.size,0);b.visible(true);await flush();
  b.set({cycle:{id:'2',state:'stopped',outcome:'later_pause',target_c:60}});await b.poll();assert.match(b.element('printer-message').textContent,/ended.*pause|pause.*ended/i);assert.equal(b.events.at(-1).active,false);
});
test('disconnect retains active ownership and Stop does not wait for an arm reply',async()=>{
  const b=browser({can_arm:false,cycle:{id:'2',state:'preheating',outcome:'none',target_c:60}});await flush();b.offline(true);await b.poll();
  assert.equal(b.events.at(-1).known,false);assert.equal(b.events.at(-1).active,true);
  b.reply(204);b.click('printer-stop');await flush();assert.equal(b.calls.filter(c=>c.url==='/api/printer/cycle/stop').length,1);
});
test('two tabs cannot reuse a consumed offered token',async()=>{
  const b=browser();await flush();b.confirm();b.reply(409);b.click('printer-preheat');await flush();await b.poll();b.confirm();b.click('printer-preheat');await flush();
  assert.equal(b.calls.filter(c=>c.url==='/api/printer/cycle').length,1);assert.match(b.element('printer-message').textContent,/another|changed|conflict/i);
});
test('a lost Stop reply retains uncertain ownership and allows another deliberate Stop',async()=>{
  const b=browser();await flush();b.confirm();b.lose(true);b.click('printer-preheat');await flush();
  b.click('printer-stop');await flush();assert.equal(b.events.at(-1).active,true);
  assert.match(b.element('printer-message').textContent,/Stop.*not confirmed/i);
  b.lose(false);b.reply(204);b.click('printer-stop');await flush();assert.equal(b.events.at(-1).active,false);
});
