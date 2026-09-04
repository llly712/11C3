#pragma once
// 11C3 Web 控制台 (WiFi AP 模式, http://192.168.4.1)
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>11C3 控制台</title>
<style>
body{font-family:system-ui,sans-serif;max-width:760px;margin:16px auto;padding:0 12px;background:#111;color:#ddd}
h1{font-size:20px;color:#7df}section{background:#1c1c1e;border:1px solid #333;border-radius:10px;padding:12px 16px;margin:12px 0}
button{background:#66CCFF;color:#14324A;border:0;border-radius:6px;padding:8px 14px;margin:2px;cursor:pointer;font-size:14px;font-weight:600}
button:active{opacity:.8}
.ch{display:flex;align-items:center;gap:10px;margin:6px 0;background:#26262a;border-radius:8px;padding:6px 10px}
.ch .idx{width:28px;font-weight:bold}
.ch input[type=color]{width:44px;height:30px;border:0;background:none;cursor:pointer}
.ch select{background:#333;color:#fff;border:1px solid #555;border-radius:5px;padding:5px}
.swatch{width:16px;height:16px;border-radius:50%;border:1px solid #666}
#status{background:#0a3d1f;border:1px solid #2e7d4f;border-radius:8px;padding:8px 12px;margin:10px 0;font-size:14px}
input[type=range]{width:180px;vertical-align:middle}
input[type=text]{background:#222;color:#fff;border:1px solid #555;border-radius:5px;padding:6px}
input[type=file]{color:#ccc}
h2{font-size:15px;margin:6px 0;color:#9cf}
small{color:#888}
</style>
</head>
<body>
<h1>11C3 洛天依应援棒发射器</h1>
<div id="status">连接中...</div>

<section><h2>播放控制</h2>
<button onclick="cmd('PLAY')">播放/暂停</button>
<button onclick="cmd('STOP')">停止</button>
<button onclick="cmd('NEXT')">下一节目</button><br>
亮度 <input type="range" id="bright" min="0" max="100" value="100" oninput="document.getElementById('brightv').textContent=this.value" onchange="cmd('BRIGHT:'+this.value)"><span id="brightv">100</span>%
</section>

<section><h2>节目</h2>
<select id="presets" style="background:#222;color:#fff;padding:6px;border-radius:5px;min-width:200px"></select>
<button onclick="loadSel()">播放选中</button>
<button onclick="delSel()">删除选中</button>
<button onclick="loadList()">刷新</button>
</section>

<section><h2>实时调色 (10 通道)</h2>
<div id="chs"></div>
<button onclick="sendColor()">发送调色</button>
<button onclick="cmd('STOP')">停播后看效果</button>
<small>调色是实时广播, 播节目时以节目数据为准</small>
</section>

<section><h2>上传配置 (CSV 灯光序列)</h2>
<input type="file" id="file" accept=".csv,text/csv"><br><br>
节目名: <input type="text" id="pname" placeholder="留空则用文件名"><br><br>
<button onclick="upload()">上传并保存</button>
</section>

<section><h2>WiFi 网络 (用于 UDP 下位机模式)</h2>
 路由器SSID: <input type="text" id="sta_ssid" style="width:150px">
 密码: <input type="text" id="sta_pass" style="width:130px">
 <button onclick="setWifi()">保存并重启</button><br>
 <small>配置后设备会连入路由器, 电脑上位机可通过 UDP:32712 连接本设备 IP</small>
</section>

<section><h2>433MHz 波形调参</h2>
 波特率 <input type="number" id="rf_baud" min="300" max="20000" style="width:90px">
 前导字节 <input type="number" id="rf_pre" min="0" max="32" style="width:70px">
 编码模式 <select id="rf_mode"><option value="0">NRZ (纯位流)</option><option value="1">UART (8N1)</option></select>
 电平反转 <select id="rf_inv"><option value="0">否</option><option value="1">是</option></select>
 <button onclick="saveRf()">保存</button>
 <small>当前: <span id="rf_cur">-</span></small><br><br>
 WiFi热点密码: <input type="text" id="ap_pass" style="width:130px">
 <button onclick="saveAp()">保存热点密码</button>
</section>

<script>
const $=id=>document.getElementById(id);
function cmd(c){fetch('/api/cmd',{method:'POST',body:c}).then(r=>r.text()).then(t=>{$('status').textContent=t;refresh();}).catch(e=>$('status').textContent='ERR '+e);}
function loadList(){
  fetch('/api/list').then(r=>r.text()).then(t=>{
    const names=t?t.split(','):[];
    const sel=$('presets');sel.innerHTML='';
    names.forEach(n=>{const o=document.createElement('option');o.value=n;o.textContent=n;sel.appendChild(o);});
  });
}
function loadSel(){const v=$('presets').value;if(v)cmd('LOAD:'+v);}
function delSel(){const v=$('presets').value;if(v)cmd('DELETE:'+v);}
function refresh(){
  fetch('/api/status').then(r=>r.text()).then(t=>{
    try{
      const s=JSON.parse(t);
      let txt='状态: '+s.state+' | 节目: '+(s.current||'-')+' | 亮度: '+s.bright+'% | 节目数: '+s.presets;
      if(s.ip)txt+=' | 路由器IP: '+s.ip;
      $('status').textContent=txt;
      $('bright').value=s.bright;$('brightv').textContent=s.bright;
      if(s.wifi_ssid){$('sta_ssid').value=s.wifi_ssid;}
    }catch(e){$('status').textContent=t;}
  }).catch(()=>{});
}
// 生成 10 通道控件
(function(){
  const chs=$('chs');
  const funcs=['常亮','1Hz闪','2Hz闪','4Hz闪'];
  for(let i=0;i<10;i++){
    const d=document.createElement('div');d.className='ch';
    d.innerHTML='<span class="idx">CH'+i+'</span>'+
      '<input type="color" id="col'+i+'" value="#ff0000">'+
      '<select id="fn'+i+'">'+funcs.map((f,j)=>'<option value="'+j+'">'+f+'</option>').join('')+'</select>'+
      '<span class="swatch" id="sw'+i+'"></span>';
    chs.appendChild(d);
    $('col'+i).addEventListener('input',()=>{$('sw'+i).style.background=$('col'+i).value;});
  }
})();
function hex2n(h){return Math.round(parseInt(h.slice(1),16)/255*15);}
function sendColor(){
  let s='COLOR:';
  for(let i=0;i<10;i++){
    const c=$('col'+i).value;
    s+=i+':'+$('fn'+i).value+','+hex2n(c)+','+hex2n(c.slice(0,4)+'00')+','+hex2n(c.slice(0,2)+'0000')+(i<9?';':'');
  }
  cmd(s);
}
function upload(){
  const f=$('file').files[0];
  if(!f){alert('请选择 CSV 文件');return;}
  const name=$('pname').value.trim()||f.name.replace(/\.csv$/i,'');
  const fd=new FormData();
  fd.append('file',f);fd.append('name',name);
  fetch('/api/upload',{method:'POST',body:fd}).then(r=>r.text()).then(t=>{
    $('status').textContent=t;loadList();refresh();
  }).catch(e=>$('status').textContent='上传失败 '+e);
}
function setWifi(){
  fetch('/api/wifi',{method:'POST',body:JSON.stringify({ssid:$('sta_ssid').value,pass:$('sta_pass').value})})
    .then(r=>r.text()).then(t=>{$('status').textContent=t;setTimeout(()=>{fetch('/api/reboot',{method:'POST'});},500);});
}
function saveRf(){
  const v=$('rf_baud').value;
  if(!v||v<300||v>20000){alert('波特率 300-20000');return;}
  const pre=$('rf_pre').value||0;
  if(pre<0||pre>32){alert('前导 0-32');return;}
  cmd('RFBAUD:'+v);
  cmd('RFPRE:'+pre);
  cmd('RFMODE:'+$('rf_mode').value);
  cmd('RFINV:'+$('rf_inv').value);
  loadRf();
}
function saveAp(){const p=$('ap_pass').value;if(p.length<8||p.length>32){alert('密码需 8-32 位');return;}cmd('SETAP:'+p);}
function fillRf(s){
  const m=/baud=(\d+);pre=(\d+);mode=(\d+);inv=(\d+)/.exec(s);
  if(!m)return;
  $('rf_baud').value=m[1];$('rf_pre').value=m[2];
  $('rf_mode').value=m[3];$('rf_inv').value=m[4];
  $('rf_cur').textContent='波特率'+m[1]+' 前导'+m[2]+' 模式'+(m[3]==='1'?'UART':'NRZ')+(m[4]==='1'?' 反转':'');
  var ap=/ap=([^;]*)/.exec(s); if(ap)$('ap_pass').value=ap[1];
}
setInterval(refresh,2500);
loadList();refresh();
fetch('/api/cmd',{method:'POST',body:'CFG'}).then(r=>r.text()).then(t=>{fillRf(t);}).catch(()=>{});
</script>
</body></html>
)rawliteral";
