const $ = id => document.getElementById(id);

async function refreshStatus() {
  const r = await fetch('/api/status');
  $('status').textContent = JSON.stringify(await r.json(), null, 2);
}
$('refresh').onclick = refreshStatus;
refreshStatus();

// Server-Sent Events
const es = new EventSource('/events');
es.addEventListener('tick', e => {
  const d = JSON.parse(e.data);
  $('tick').textContent = d.tick;
  $('uptime').textContent = d.uptime;
});

// WebSocket chat
const ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
const say = t => { const b = $('chat'); b.textContent += t + '\n'; b.scrollTop = b.scrollHeight; };
ws.onopen = () => say('* connected');
ws.onclose = () => say('* disconnected');
ws.onmessage = e => say(e.data);
$('chatForm').onsubmit = e => { e.preventDefault(); if ($('msg').value) ws.send($('msg').value); $('msg').value = ''; };

// url-encoded form POST
$('echoForm').onsubmit = async e => {
  e.preventDefault();
  const r = await fetch('/api/echo', { method: 'POST', body: new URLSearchParams(new FormData(e.target)) });
  $('echoOut').textContent = await r.text();
};

// multipart upload
async function listFiles() {
  const files = await (await fetch('/api/files')).json();
  $('files').innerHTML = files.map(f => `<li><a href="/uploads/${encodeURIComponent(f.name)}">${f.name}</a> (${f.size} bytes)</li>`).join('');
}
$('uploadForm').onsubmit = async e => {
  e.preventDefault();
  const r = await fetch('/upload', { method: 'POST', body: new FormData(e.target) });
  $('uploadOut').textContent = await r.text();
  listFiles();
};
listFiles();
