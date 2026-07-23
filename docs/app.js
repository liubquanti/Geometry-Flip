const ORANGE = '#ff8200';
const BLACK = '#000000';
const GRID = 8;
const MAX_OBJECTS = 512;
const MUSIC_NOTES_MAX = 900; // must match MUSIC_NOTES_MAX in lib/geoflip.c
const AUTO_LENGTH_PADDING = 10;
const PANELS = {
  top: 136,
  bottom: 96,
};

const TYPES = [
  { type: 'BLOCK', label: 'BLOCK', hint: 'Solid' },
  { type: 'SPIKE', label: 'SPIKE', hint: 'Kill' },
  { type: 'MINI_SPIKE', label: 'MINI SPIKE', hint: 'Half' },
  { type: 'MINI_BLOCK', label: 'MINI BLOCK', hint: 'Half' },
  { type: 'JUMPER', label: 'JUMPER', hint: 'Auto jump' },
  { type: 'SPHERE', label: 'SPHERE', hint: 'Manual jump' },
  { type: 'ERASE', label: 'ERASE', hint: 'Delete' },
];

const el = {
  canvas: document.getElementById('editorCanvas'),
  levelName: document.getElementById('levelName'),
  bgStyle: document.getElementById('bgStyle'),
  difficulty: document.getElementById('difficulty'),
  levelLength: document.getElementById('levelLength'),
  cameraX: document.getElementById('cameraX'),
  loadBtn: document.getElementById('loadBtn'),
  saveBtn: document.getElementById('saveBtn'),
  fileInput: document.getElementById('fileInput'),
  palette: document.getElementById('palette'),
  selectionInfo: document.getElementById('selectionInfo'),
  startupOverlay: document.getElementById('startupOverlay'),
  startupMessage: document.getElementById('startupMessage'),
  startupActions: document.querySelector('.startup-actions'),
  startupTitle: document.getElementById('startupTitle'),
  connectBtn: document.getElementById('connectBtn'),
  offlineBtn: document.getElementById('offlineBtn'),
  levelsPanel: document.getElementById('levelsPanel'),
  levelsList: document.getElementById('levelsList'),
  saveIcon: document.getElementById('saveIcon'),
  exitBtn: document.getElementById('exitBtn'),
  iconEditorBtn: document.getElementById('iconEditorBtn'),
  iconEditorPanel: document.getElementById('iconEditorPanel'),
  iconCanvas: document.getElementById('iconCanvas'),
  iconClear: document.getElementById('iconClear'),
  iconInvert: document.getElementById('iconInvert'),
  iconCopy: document.getElementById('iconCopy'),
  iconCode: document.getElementById('iconCode'),
  iconClose: document.getElementById('iconClose'),
  musicBtn: document.getElementById('musicBtn'),
  musicEditor: document.getElementById('musicEditor'),
  musicBpm: document.getElementById('musicBpm'),
  musicDuration: document.getElementById('musicDuration'),
  musicOctave: document.getElementById('musicOctave'),
  musicNotes: document.getElementById('musicNotes'),
  musicPlayBtn: document.getElementById('musicPlayBtn'),
  musicStopBtn: document.getElementById('musicStopBtn'),
  musicCloseBtn: document.getElementById('musicCloseBtn'),
  musicImportMidiBtn: document.getElementById('musicImportMidiBtn'),
  midiFileInput: document.getElementById('midiFileInput'),
  midiImportPanel: document.getElementById('midiImportPanel'),
  midiImportFileName: document.getElementById('midiImportFileName'),
  midiTrackSelect: document.getElementById('midiTrackSelect'),
  midiChannelSelect: document.getElementById('midiChannelSelect'),
  midiSkipDrums: document.getElementById('midiSkipDrums'),
  midiImportStats: document.getElementById('midiImportStats'),
};

const ctx = el.canvas.getContext('2d');
ctx.imageSmoothingEnabled = false;

if (el.levelLength) {
  el.levelLength.disabled = true;
  el.levelLength.title = 'Auto (last object + 10)';
}

const state = {
  tool: 'BLOCK',
  objectRotation: 0,
  cameraX: 0,
  cameraY: 0,
  dragging: false,
  dragButton: 0,
  zoom: 2.0,
  panning: false,
  panStartX: 0,
  panStartY: 0,
  panStartCameraX: 0,
  panStartCameraY: 0,
  hover: null,
  level: blankLevel(),
  dirty: false,
  fileName: 'untitled.gdlvl',
  undoStack: [],
  redoStack: [],
  autosaveTimer: null,
  isConnected: false,
  hasFlipperFile: false,
  lastLimitNotice: 0,
};

function normalizeRotation(raw) {
  let r = Number(raw);
  if (!Number.isFinite(r)) return 0;
  if (Number.isInteger(r) && Math.abs(r) <= 3) r *= 90;
  r = ((r % 360) + 360) % 360;
  const snapped = Math.round(r / 90) * 90;
  return ((snapped % 360) + 360) % 360;
}

function rotate90(value) {
  return (normalizeRotation(value) + 90) % 360;
}

function getObjectAt(gx, gy) {
  return state.level.objects.find((obj) => obj.gx === gx && obj.gy === gy);
}

function computeAutoLength(level) {
  let maxGx = -1;
  for (const obj of level.objects) {
    if (Number.isFinite(obj.gx) && obj.gx > maxGx) maxGx = obj.gx;
  }
  const cells = Math.max(0, maxGx + AUTO_LENGTH_PADDING);
  return Math.max(64, cells * GRID);
}

function applyAutoLength(updateCamera = true) {
  state.level.length = computeAutoLength(state.level);
  if (el.levelLength) {
    el.levelLength.value = String(state.level.length);
  }
  if (updateCamera) {
    el.cameraX.max = String(Math.max(0, state.level.length));
    state.cameraX = clamp(state.cameraX, 0, Number(el.cameraX.max));
    el.cameraX.value = String(state.cameraX);
  }
}

function notifyObjectLimit() {
  const now = Date.now();
  if (now - state.lastLimitNotice < 800) return;
  state.lastLimitNotice = now;
  if (el.selectionInfo) el.selectionInfo.textContent = `LIMIT ${MAX_OBJECTS}`;
}

function cell() { return GRID * (state.zoom || 1); }

function blankLevel() {
  return {
    name: 'Untitled Level',
    speed: 2,
    gravityPct: 100,
    difficulty: 'Easy',
    bgStyle: '0',
    length: 2000,
    objects: [],
    decorations: [],
    musicBpm: 0,
    musicDuration: 8,
    musicOctave: 5,
    musicNotes: '',
  };
}

function resizeCanvas() {
  const rect = el.canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  el.canvas.width = Math.max(1, Math.floor(rect.width * dpr));
  el.canvas.height = Math.max(1, Math.floor(rect.height * dpr));
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  render();
}

function setDirty(value = true) {
  state.dirty = value;
  document.title = `${value ? '* ' : ''}Geometry Flip Level Editor`;
  if (value) requestAutoSave();
  if (!value && state.autosaveTimer) {
    clearTimeout(state.autosaveTimer);
    state.autosaveTimer = null;
  }
}

function requestAutoSave() {
  if (!state.isConnected) return;
  setSavingIcon(true);
  if (el.exitBtn) el.exitBtn.disabled = true;
  if (state.autosaveTimer) clearTimeout(state.autosaveTimer);
  state.autosaveTimer = setTimeout(() => {
    if (state.dirty) saveToFlipper();
  }, 1000);
}

function updateSaveUI() {
  if (state.isConnected) {
    el.saveBtn.hidden = true;
  } else {
    el.saveBtn.hidden = false;
  }
}

function setSavingIcon(visible) {
  if (!el.saveIcon) return;
  el.saveIcon.hidden = !visible;
}

function clamp(v, min, max) {
  return Math.max(min, Math.min(max, v));
}

function pushHistory() {
  // Save current level state to undo stack BEFORE it changes, and clear redo stack
  const snapshot = JSON.parse(JSON.stringify(state.level));
  state.undoStack.push(snapshot);
  state.redoStack = [];
  // Keep history size reasonable (limit to 50 states)
  if (state.undoStack.length > 50) state.undoStack.shift();
}

function undo() {
  if (state.undoStack.length === 0) return;
  // Save current state to redo
  const snapshot = JSON.parse(JSON.stringify(state.level));
  state.redoStack.push(snapshot);
  // Restore previous state
  state.level = state.undoStack.pop();
  applyAutoLength(false);
  setDirty(true);
  applyLevelToUI();
  render();
}

function redo() {
  if (state.redoStack.length === 0) return;
  // Save current state to undo
  const snapshot = JSON.parse(JSON.stringify(state.level));
  state.undoStack.push(snapshot);
  // Restore next state
  state.level = state.redoStack.pop();
  applyAutoLength(false);
  setDirty(true);
  applyLevelToUI();
  render();
}

function gridToScreenX(gx) {
  return gx * cell() - state.cameraX;
}

function gridToScreenY(gy) {
  const h = el.canvas.clientHeight;
  return h - cell() - gy * cell() + state.cameraY;
}

function screenToGrid(clientX, clientY) {
  const rect = el.canvas.getBoundingClientRect();
  const x = clientX - rect.left;
  const y = clientY - rect.top;
  const s = cell();
  const gx = Math.floor((x + state.cameraX) / s);
  const gy = Math.floor((hVisible() - s + state.cameraY - y) / s);
  return { gx, gy: (gy + 1) }; // +1 because screenToGrid is used for hover and we want it to snap to the current cell until you move into the next one
}

function hVisible() {
  return el.canvas.clientHeight;
}

function addOrReplaceObject(type, gx, gy) {
  if (type === 'ERASE') {
    const existing = state.level.objects.some((obj) => obj.gx === gx && obj.gy === gy);
    if (existing) {
      pushHistory();
      state.level.objects = state.level.objects.filter((obj) => !(obj.gx === gx && obj.gy === gy));
      applyAutoLength(false);
      setDirty();
    }
    return;
  }

  // Check if object already exists at this position with same type
  const existing = state.level.objects.find((obj) => obj.gx === gx && obj.gy === gy);
  const rot = normalizeRotation(state.objectRotation);
  const isSameType = existing && existing.type === type;
  const isSameRot = existing && normalizeRotation(existing.rot) === rot;

  if (!existing && state.level.objects.length >= MAX_OBJECTS) {
    notifyObjectLimit();
    return;
  }
  
  // Only make changes if we're actually changing something
  if (!isSameType || !isSameRot) {
    pushHistory();
    state.level.objects = state.level.objects.filter((obj) => !(obj.gx === gx && obj.gy === gy));
    state.level.objects.push({ type, gx, gy, rot });
    state.level.objects.sort((a, b) => a.gx - b.gx || a.gy - b.gy || a.type.localeCompare(b.type) || normalizeRotation(a.rot) - normalizeRotation(b.rot));
    applyAutoLength(false);
    setDirty();
  }
}

function placeFromPointer(ev) {
  const { gx, gy } = screenToGrid(ev.clientX, ev.clientY);
  if (gx < 0 || gy < 0) return;
  addOrReplaceObject(state.tool, gx, gy);
  updateSelectionInfo(gx, gy);
}

function updateSelectionInfo(gx = null, gy = null) {
  if (state.tool === 'ERASE') {
    el.selectionInfo.textContent = 'ERASE';
    return;
  }
  const hoverText = gx === null ? '' : ` @ ${gx},${gy}`;
  let rotValue = null;
  if (gx !== null && gy !== null) {
    const hovered = getObjectAt(gx, gy);
    if (hovered && (hovered.type === 'SPIKE' || hovered.type === 'MINI_SPIKE' || hovered.type === 'BLOCK' || hovered.type === 'MINI_BLOCK')) {
      rotValue = normalizeRotation(hovered.rot || 0);
    }
  }
  if (rotValue === null && (state.tool === 'SPIKE' || state.tool === 'MINI_SPIKE' || state.tool === 'BLOCK' || state.tool === 'MINI_BLOCK')) {
    rotValue = normalizeRotation(state.objectRotation);
  }
  const rotText = (rotValue !== null) ? ` rot ${rotValue}` : '';
  el.selectionInfo.textContent = `${state.tool}${rotText}${hoverText}`;
}

function drawGrid(width, height) {
  const s = cell();
  ctx.fillStyle = BLACK;
  ctx.fillRect(0, 0, width, height);

  ctx.strokeStyle = ORANGE;
  ctx.lineWidth = 1;
  ctx.beginPath();

  const startX = -((state.cameraX % s) + s) % s;
  for (let x = startX; x < width; x += s) {
    ctx.moveTo(Math.floor(x) + 0.5, 0);
    ctx.lineTo(Math.floor(x) + 0.5, height);
  }

  const startY = ((height + state.cameraY) % s + s) % s;
  for (let y = startY; y < height; y += s) {
    ctx.moveTo(0, Math.floor(y) + 0.5);
    ctx.lineTo(width, Math.floor(y) + 0.5);
  }

  ctx.stroke();
}

function drawRoundedRect(x, y, w, h, radius, fill = true) {
  ctx.beginPath();
  ctx.moveTo(x + radius, y);
  ctx.arcTo(x + w, y, x + w, y + h, radius);
  ctx.arcTo(x + w, y + h, x, y + h, radius);
  ctx.arcTo(x, y + h, x, y, radius);
  ctx.arcTo(x, y, x + w, y, radius);
  ctx.closePath();
  if (fill) ctx.fill();
  else ctx.stroke();
}

function drawBlock(x, y, w, h) {
  ctx.fillStyle = ORANGE;
  ctx.fillRect(x, y, w, h);
}

function drawSpike(x, y, w, h) {
  ctx.fillStyle = ORANGE;
  ctx.beginPath();
  ctx.moveTo(x, y + h);
  ctx.lineTo(x + w / 2, y);
  ctx.lineTo(x + w, y + h);
  ctx.closePath();
  ctx.fill();
}

function drawSpikeRotated(x, y, w, h, rot) {
  const r = normalizeRotation(rot);
  if (r === 0) {
    drawSpike(x, y, w, h);
    return;
  }
  ctx.fillStyle = ORANGE;
  ctx.beginPath();
  if (r === 90) {
    ctx.moveTo(x, y);
    ctx.lineTo(x + w, y + h / 2);
    ctx.lineTo(x, y + h);
  } else if (r === 180) {
    ctx.moveTo(x, y);
    ctx.lineTo(x + w / 2, y + h);
    ctx.lineTo(x + w, y);
  } else {
    ctx.moveTo(x + w, y);
    ctx.lineTo(x, y + h / 2);
    ctx.lineTo(x + w, y + h);
  }
  ctx.closePath();
  ctx.fill();
}

function drawMiniSpike(x, y, w, h) {
  const midY = y + h;
  ctx.fillStyle = ORANGE;
  ctx.beginPath();
  ctx.moveTo(x, midY);
  ctx.lineTo(x + w / 2, y);
  ctx.lineTo(x + w, midY);
  ctx.closePath();
  ctx.fill();
}

function drawMiniSpikeRotated(x, y, w, h, rot) {
  const r = normalizeRotation(rot);
  const halfH = h / 2;
  const halfW = w / 2;
  if (r === 0) {
    drawMiniSpike(x, y + halfH, w, halfH);
    return;
  }
  ctx.fillStyle = ORANGE;
  ctx.beginPath();
  if (r === 180) {
    ctx.moveTo(x, y);
    ctx.lineTo(x + w / 2, y + halfH);
    ctx.lineTo(x + w, y);
  } else if (r === 90) {
    const x0 = x;
    ctx.moveTo(x0, y);
    ctx.lineTo(x0 + halfW, y + h / 2);
    ctx.lineTo(x0, y + h);
  } else {
    const x0 = x + halfW;
    ctx.moveTo(x0 + halfW, y);
    ctx.lineTo(x0, y + h / 2);
    ctx.lineTo(x0 + halfW, y + h);
  }
  ctx.closePath();
  ctx.fill();
}

function drawMiniBlock(x, y, w, h) {
  ctx.fillStyle = ORANGE;
  ctx.fillRect(x, y, w, h);
}

function drawMiniBlockRotated(x, y, w, h, rot) {
  const r = normalizeRotation(rot);
  const halfW = w / 2;
  const halfH = h / 2;
  if (r === 0) {
    drawMiniBlock(x, y + halfH, w, halfH);
  } else if (r === 180) {
    drawMiniBlock(x, y, w, halfH);
  } else if (r === 90) {
    drawMiniBlock(x, y, halfW, h);
  } else {
    drawMiniBlock(x + halfW, y, halfW, h);
  }
}

function drawJumper(x, y, w, h) {
  ctx.fillStyle = ORANGE;
  ctx.fillRect(x, y + h - 2, w, 2);
  // simple filled jumper bar in editor (no black outlines)
}

function drawSphere(x, y, w, h) {
  ctx.strokeStyle = ORANGE;
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(x + w / 2, y + h / 2, Math.max(2, (w / 2) - 1), 0, Math.PI * 2);
  ctx.stroke();
}

function drawObject(obj) {
  const s = cell();
  const x = gridToScreenX(obj.gx);
  const y = gridToScreenY(obj.gy);
  const visible = x > -s * 2 && x < el.canvas.clientWidth + s * 2;
  if (!visible) return;

  switch (obj.type) {
    case 'BLOCK':
      drawBlock(x, y, s, s);
      break;
    case 'SPIKE':
      drawSpikeRotated(x, y, s, s, obj.rot || 0);
      break;
    case 'MINI_SPIKE':
      drawMiniSpikeRotated(x, y, s, s, obj.rot || 0);
      break;
    case 'MINI_BLOCK':
      drawMiniBlockRotated(x, y, s, s, obj.rot || 0);
      break;
    case 'JUMPER':
      drawJumper(x, y, s, s);
      break;
    case 'SPHERE':
      drawSphere(x, y, s, s);
      break;
    default:
      break;
  }
}

function drawDecoration(dec) {
  const x = dec.x - state.cameraX / 2;
  const y = dec.y;
  if (x < -20 || x > el.canvas.clientWidth + 20) return;
  ctx.strokeStyle = ORANGE;
  ctx.lineWidth = 1;
  if (dec.type === 'STAR') {
    ctx.beginPath();
    ctx.moveTo(x, y);
    ctx.lineTo(x + 2, y);
    ctx.moveTo(x + 1, y - 1);
    ctx.lineTo(x + 1, y + 1);
    ctx.stroke();
  } else if (dec.type === 'CLOUD') {
    ctx.strokeRect(x, y, 16, 8);
  } else if (dec.type === 'PILLAR') {
    ctx.strokeRect(x, 40, 4, Math.max(4, hVisible() - 40));
  }
}

function render() {
  const width = el.canvas.clientWidth;
  const height = el.canvas.clientHeight;
  if (width === 0 || height === 0) return;

  ctx.save();
  ctx.globalAlpha = 0.25;
  drawGrid(width, height);
  ctx.globalAlpha = 1.0;

  // draw ground level line: snap to grid and offset one cell down
  const groundY = Math.floor(gridToScreenY(0) + cell()) + 0.5;
  ctx.strokeStyle = ORANGE;
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.moveTo(0, groundY);
  ctx.lineTo(width, groundY);
  ctx.stroke();

  ctx.strokeStyle = ORANGE;
  ctx.fillStyle = ORANGE;

  state.level.decorations.forEach(drawDecoration);
  state.level.objects.forEach(drawObject);

  if (state.hover) {
    const { gx, gy } = state.hover;
    const x = gridToScreenX(gx);
    const y = gridToScreenY(gy);
    ctx.strokeStyle = ORANGE;
    ctx.lineWidth = 2;
    ctx.strokeRect(x + 1, y + 1, cell() - 2, cell() - 2);
  }

  ctx.restore();
}

function renderLoop() {
  render();
  requestAnimationFrame(renderLoop);
}

function applyLevelToUI() {
  el.levelName.value = state.level.name;
  el.bgStyle.value = state.level.bgStyle;
  el.difficulty.value = state.level.difficulty || 'Easy';
  if (el.musicBpm) el.musicBpm.value = state.level.musicBpm > 0 ? state.level.musicBpm : 120;
  if (el.musicDuration) el.musicDuration.value = String(state.level.musicDuration || 8);
  if (el.musicOctave) el.musicOctave.value = state.level.musicOctave || 5;
  if (el.musicNotes) el.musicNotes.value = state.level.musicNotes || '';
  midiParsed = null;
  if (el.midiImportPanel) el.midiImportPanel.hidden = true;
  applyAutoLength(true);
  updateSelectionInfo();
  render();
}

function setLevel(level, fileName = 'untitled.gdlvl') {
  state.level = level;
  state.fileName = fileName;
  state.hasFlipperFile = false;
  state.cameraX = 0;
  state.cameraY = 0;
  state.dirty = false;
  applyAutoLength(false);
  applyLevelToUI();
  setDirty(false);
}

function parseLevel(text) {
  const level = blankLevel();
  const lines = text.split(/\r?\n/);
  let overflowed = false;

  for (const raw of lines) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;

    const parts = line.split(/\s+/);
    const key = parts[0].toUpperCase();

    if (key === 'NAME' && parts.length >= 2) {
      level.name = raw.slice(raw.indexOf(' ') + 1).trim();
    } else if (key === 'BG' && parts[1]) {
      level.bgStyle = parts[1];
    } else if (key === 'DIFICULTY' && parts[1]) {
      level.difficulty = parts[1];
    } else if (key === 'LENGTH' && parts[1]) {
      // Length is auto-computed in the editor.
    } else if (key === 'SPEED' && parts[1]) {
      level.speed = Number(parts[1]) || level.speed;
    } else if (key === 'GRAVITY' && parts[1]) {
      level.gravityPct = Number(parts[1]) || level.gravityPct;
    } else if (key === 'MUSIC') {
      const body = raw.slice(raw.indexOf(' ') + 1);
      const bpmMatch = body.match(/BPM=(-?\d+)/);
      const durMatch = body.match(/DURATION=(-?\d+)/);
      const octMatch = body.match(/OCTAVE=(-?\d+)/);
      if (bpmMatch) level.musicBpm = Number(bpmMatch[1]) || 0;
      if (durMatch) level.musicDuration = Number(durMatch[1]) || level.musicDuration;
      if (octMatch) level.musicOctave = Number(octMatch[1]) || level.musicOctave;
    } else if (key === 'NOTE') {
      const chunk = raw.slice(raw.indexOf(' ') + 1).trim();
      if (chunk) level.musicNotes = level.musicNotes ? `${level.musicNotes}, ${chunk}` : chunk;
    } else if (key === 'OBJ' && parts.length >= 4) {
      const type = parts[1].toUpperCase();
      const gx = Number(parts[2]);
      const gy = Number(parts[3]);
      const rot = parts.length >= 5 ? normalizeRotation(parts[4]) : 0;
      if (!Number.isFinite(gx) || !Number.isFinite(gy)) continue;
      if (level.objects.length >= MAX_OBJECTS) {
        overflowed = true;
        continue;
      }
      level.objects.push({ type, gx, gy, rot });
    } else if (key === 'DEC' && parts.length >= 4) {
      const type = parts[1].toUpperCase();
      const x = Number(parts[2]);
      const y = Number(parts[3]);
      if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
      level.decorations.push({ type, x, y });
    }
  }

  level.objects.sort((a, b) => a.gx - b.gx || a.gy - b.gy || a.type.localeCompare(b.type) || normalizeRotation(a.rot) - normalizeRotation(b.rot));
  if (overflowed) console.warn(`Object limit reached (${MAX_OBJECTS}); extra objects ignored.`);
  return level;
}

/* Splits a comma-separated note list into several short lines (each safely
   under the firmware parser's per-line buffer) that get re-joined into one
   note list on load via consecutive NOTE directives. */
function chunkNotes(notesStr, maxLen = 100) {
  const tokens = notesStr.split(',').map((s) => s.trim()).filter(Boolean);
  const lines = [];
  let cur = '';
  for (const tok of tokens) {
    const candidate = cur ? `${cur}, ${tok}` : tok;
    if (cur && candidate.length > maxLen) {
      lines.push(cur);
      cur = tok;
    } else {
      cur = candidate;
    }
  }
  if (cur) lines.push(cur);
  return lines;
}

function serializeLevel(level) {
  const autoLength = computeAutoLength(level);
  const lines = [
    '# Geometry Flip level generated by web editor',
    `NAME ${level.name}`,
    `BG ${level.bgStyle}`,
    `DIFICULTY ${level.difficulty || 'Easy'}`,
    `LENGTH ${autoLength}`,
  ];

  if (level.musicNotes && level.musicNotes.trim()) {
    const bpm = level.musicBpm > 0 ? level.musicBpm : 120;
    const dur = level.musicDuration || 8;
    const oct = level.musicOctave || 5;
    lines.push('', '# MUSIC BPM=<n> DURATION=<denominator> OCTAVE=<n>', `MUSIC BPM=${bpm} DURATION=${dur} OCTAVE=${oct}`);
    for (const noteLine of chunkNotes(level.musicNotes)) {
      lines.push(`NOTE ${noteLine}`);
    }
  }

  lines.push('', '# OBJ <TYPE> <GX> <GY> [ROT]');

  for (const obj of level.objects) {
    const rot = normalizeRotation(obj.rot || 0);
    const rotPart = rot ? ` ${rot}` : '';
    lines.push(`OBJ ${obj.type} ${obj.gx} ${obj.gy}${rotPart}`);
  }

  if (level.decorations.length) {
    lines.push('', '# DEC <TYPE> <X> <Y>');
    for (const dec of level.decorations) {
      lines.push(`DEC ${dec.type} ${dec.x} ${dec.y}`);
    }
  }

  return lines.join('\n') + '\n';
}

function buildFileName(name) {
  const base = (name || 'untitled').trim() || 'untitled';
  return `${base.replace(/[^a-z0-9_\- ]/gi, '_')}.gdlvl`;
}

let flipperPort = null;
let flipperWriter = null;
let flipperReader = null;

async function connectFlipper() {
  if (flipperPort) return;

  flipperPort = await navigator.serial.requestPort({
    filters: [
      { usbVendorId: 0x0483 }
    ]
  });

  await flipperPort.open({
    baudRate: 230400,
  });

  flipperWriter =
    flipperPort.writable.getWriter();

  flipperReader =
    flipperPort.readable.getReader();

  await sleep(1000);

  await flushSerial();
  state.isConnected = true;
  updateSaveUI();
  setSavingIcon(false);
}

async function disconnectFlipper() {
  try {
    if (flipperReader) {
      await flipperReader.cancel();
      flipperReader.releaseLock();
      flipperReader = null;
    }

    if (flipperWriter) {
      flipperWriter.releaseLock();
      flipperWriter = null;
    }

    if (flipperPort) {
      await flipperPort.close();
      flipperPort = null;
    }
  } catch (e) {
    console.error(e);
  }
  state.isConnected = false;
  updateSaveUI();
  setSavingIcon(false);
  if (el.exitBtn) el.exitBtn.disabled = false;
}

function sleep(ms) {
  return new Promise(r => setTimeout(r, ms));
}

async function flushSerial() {
  const start = Date.now();

  while (Date.now() - start < 300) {
    const result = await Promise.race([
      flipperReader.read(),
      sleep(50).then(() => null)
    ]);

    if (!result) break;
  }
}

async function writeSerial(text) {
  const encoder = new TextEncoder();

  await flipperWriter.write(
    encoder.encode(text)
  );
}

async function readUntilPrompt(timeout = 5000) {
  const decoder = new TextDecoder();

  let buffer = '';

  const start = Date.now();

  while (Date.now() - start < timeout) {
    const result = await Promise.race([
      flipperReader.read(),
      sleep(100).then(() => null)
    ]);

    if (!result) continue;

    const { value, done } = result;

    if (done) break;

    if (value) {
      buffer += decoder.decode(value);

      console.log(buffer);

      // REAL prompt detection
      if (
        buffer.includes('\r\n>: ') ||
        buffer.endsWith('>: ') ||
        buffer.endsWith('> ')
      ) {
        return buffer;
      }
    }
  }

  return buffer;
}

async function execCLI(command) {
  await flushSerial();

  console.log('CMD:', command);

  await writeSerial(command + '\r');

  const response =
    await readUntilPrompt();

  console.log('RESP:', response);

  return response;
}

function stringToHex(str) {
  return [...new TextEncoder().encode(str)]
    .map(b => b.toString(16).padStart(2, '0'))
    .join('');
}

async function saveToFlipper(showAlert = false) {
  try {
    if (el.exitBtn) el.exitBtn.disabled = true;
    await connectFlipper();
    setSavingIcon(true);

    const targetFileName = buildFileName(state.level.name);
    const targetPath = `/ext/geoflip/levels/${targetFileName}`;
    const content = serializeLevel(state.level);
    let renamed = false;

    await execCLI('storage mkdir /ext/geoflip');
    await execCLI('storage mkdir /ext/geoflip/levels');
    if (state.hasFlipperFile && state.fileName && state.fileName !== targetFileName) {
      const oldPath = `/ext/geoflip/levels/${state.fileName}`;
      await execCLI(`storage rename "${oldPath}" "${targetPath}"`);
      state.fileName = targetFileName;
      renamed = true;
    } else {
      state.fileName = targetFileName;
    }
    if (!renamed) {
      await execCLI(`storage remove "${targetPath}"`);
    }

    // Надсилаємо команду write (Flipper чекає дані після неї)
    await flushSerial();
    await writeSerial(`storage write "${targetPath}"\r`);

    // Чекаємо поки Flipper надрукує підказку (зазвичай просто чекає введення)
    await sleep(500);
    await flushSerial();

    // Надсилаємо вміст файлу, потім EOF (Ctrl+C або спеціальний символ)
    const encoder = new TextEncoder();
    await flipperWriter.write(encoder.encode(content));

    // Надсилаємо Ctrl+C щоб завершити запис
    await flipperWriter.write(new Uint8Array([0x03]));

    await readUntilPrompt(5000);

    if (showAlert) {
      alert(`Експортовано:\n${targetPath}`);
    }
    setDirty(false);
    state.hasFlipperFile = true;

  } catch (err) {
    console.error(err);
    if (showAlert) alert(err.message);
  } finally {
    if (showAlert) await disconnectFlipper();
    setSavingIcon(false);
    if (el.exitBtn) el.exitBtn.disabled = false;
  }
}

function stripPrompt(text) {
  return text
    .replace(/\r\n/g, '\n')
    .replace(/>:\s*$/m, '')
    .replace(/>\s*$/m, '')
    .trim();
}

function parseLevelList(text) {
  const cleaned = stripPrompt(text);
  const lines = cleaned.split('\n').map((line) => line.trim());
  const files = [];
  for (const line of lines) {
    if (!line) continue;
    const fileMatch = line.match(/\[F\]\s+(.+?\.gdlvl)\b/i);
    if (fileMatch) {
      files.push(fileMatch[1]);
      continue;
    }
    const looseMatch = line.match(/([^/\\]+?\.gdlvl)\b/i);
    if (looseMatch) files.push(looseMatch[1]);
  }
  return files;
}

async function getLevelDisplayName(fileName) {
  try {
    const response = await execCLI(`storage read "/ext/geoflip/levels/${fileName}"`);
    const cleaned = stripPrompt(response);
    const level = parseLevel(cleaned);
    return level.name || fileName.replace(/\.gdlvl$/i, '');
  } catch (err) {
    console.error(err);
    return fileName.replace(/\.gdlvl$/i, '');
  }
}

async function listLevels() {
  try {
    await connectFlipper();
    await execCLI('storage mkdir /ext/geoflip');
    await execCLI('storage mkdir /ext/geoflip/levels');
    const response = await execCLI('storage list /ext/geoflip/levels');
    const files = parseLevelList(response);
    const entries = [];
    for (const fileName of files) {
      const label = await getLevelDisplayName(fileName);
      entries.push({ fileName, label });
    }
    return entries;
  } catch (err) {
    console.error(err);
    return [];
  }
}

async function loadLevelFromFlipper(fileName) {
  try {
    await connectFlipper();
    const response = await execCLI(`storage read "/ext/geoflip/levels/${fileName}"`);
    const cleaned = stripPrompt(response);
    const level = parseLevel(cleaned);
    setLevel(level, fileName);
    state.hasFlipperFile = true;
    setSavingIcon(false);
    hideStartupOverlay();
  } catch (err) {
    console.error(err);
    alert(err.message);
  }
}

async function deleteLevelFromFlipper(fileName) {
  try {
    await connectFlipper();
    await execCLI(`storage remove "/ext/geoflip/levels/${fileName}"`);
    await showLevels();
  } catch (err) {
    console.error(err);
    alert(err.message);
  }
}

async function showLevels() {
  el.levelsList.innerHTML = '';
  const loading = document.createElement('div');
  loading.textContent = 'Loading levels...';
  el.levelsList.appendChild(loading);
  const levels = await listLevels();
  el.levelsList.innerHTML = '';
  const newRow = document.createElement('div');
  newRow.className = 'level-row';

  const newButton = document.createElement('button');
  newButton.type = 'button';
  newButton.className = 'level-name';
  newButton.textContent = 'NEW LEVEL';
  newButton.addEventListener('click', () => {
    setLevel(blankLevel(), 'untitled.gdlvl');
    setDirty(true);
    hideStartupOverlay();
  });
  newRow.appendChild(newButton);
  el.levelsList.appendChild(newRow);
  if (!levels.length) {
    const empty = document.createElement('div');
    empty.textContent = 'No levels found.';
    el.levelsList.appendChild(empty);
    return;
  }
  for (const entry of levels) {
    const row = document.createElement('div');
    row.className = 'level-row';

    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'level-name';
    button.textContent = entry.label;
    button.addEventListener('click', () => loadLevelFromFlipper(entry.fileName));

    const delButton = document.createElement('button');
    delButton.type = 'button';
    delButton.className = 'level-delete';
    delButton.textContent = 'DELETE';
    delButton.addEventListener('click', (event) => {
      event.stopPropagation();
      deleteLevelFromFlipper(entry.fileName);
    });

    row.appendChild(button);
    row.appendChild(delButton);
    el.levelsList.appendChild(row);
  }
}

function showStartupOverlay(message) {
  if (message) el.startupMessage.textContent = message;
  el.startupOverlay.classList.remove('hidden');
  el.startupMessage.hidden = false;
  el.levelsPanel.hidden = true;
  if (el.startupActions) el.startupActions.hidden = false;
}

function showLevelPicker() {
  el.startupMessage.hidden = true;
  el.levelsPanel.hidden = false;
  if (el.startupActions) el.startupActions.hidden = true;
  showLevels();
}

function hideStartupOverlay() {
  el.startupOverlay.classList.add('hidden');
}

function downloadLevel() {
  try {
    setSavingIcon(true);
    if (el.exitBtn) el.exitBtn.disabled = true;
    const fileNameBase = state.level.name.trim() || 'untitled';
    const fileName = `${fileNameBase.replace(/[^a-z0-9_\- ]/gi, '_')}.gdlvl`;
    const blob = new Blob([serializeLevel(state.level)], { type: 'text/plain;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = fileName;
    a.click();
    URL.revokeObjectURL(url);
    setDirty(false);
  } finally {
    setSavingIcon(false);
    if (el.exitBtn) el.exitBtn.disabled = false;
  }
}

function syncInputsToLevel() {
  const oldName = state.level.name;
  const oldBgStyle = state.level.bgStyle;
  const oldDifficulty = state.level.difficulty || 'Easy';
  
  const newName = el.levelName.value.trim() || 'Untitled Level';
  const newBgStyle = el.bgStyle.value;
  const newDifficulty = (el.difficulty && el.difficulty.value) ? el.difficulty.value : 'Easy';
  
  // Only push history and apply changes if something is about to change
  if (oldName !== newName || oldBgStyle !== newBgStyle || oldDifficulty !== newDifficulty) {
    pushHistory();
    state.level.name = newName;
    state.level.bgStyle = newBgStyle;
    state.level.difficulty = newDifficulty;
    setDirty(true);
  }
  applyAutoLength(true);
  render();
}

function syncMusicToLevel() {
  if (!el.musicBpm) return;
  const oldBpm = state.level.musicBpm || 0;
  const oldDur = state.level.musicDuration || 8;
  const oldOct = state.level.musicOctave || 5;
  const oldNotes = state.level.musicNotes || '';

  const notes = el.musicNotes.value.trim();
  const newBpm = notes ? (Number(el.musicBpm.value) || 120) : 0;
  const newDur = Number(el.musicDuration.value) || 8;
  const newOct = Number(el.musicOctave.value) || 5;

  if (oldBpm !== newBpm || oldDur !== newDur || oldOct !== newOct || oldNotes !== notes) {
    pushHistory();
    state.level.musicBpm = newBpm;
    state.level.musicDuration = newDur;
    state.level.musicOctave = newOct;
    state.level.musicNotes = notes;
    setDirty(true);
  }
}

function openMusicEditor() {
  if (!el.musicEditor) return;
  el.musicEditor.hidden = false;
}

function closeMusicEditor() {
  if (!el.musicEditor) return;
  stopMusicPreview();
  el.musicEditor.hidden = true;
}

/* ─── Music preview (Web Audio) ───────────────────────────────────────
   Mirrors the firmware's note-token grammar so what you hear here matches
   what will play on-device: [duration]NoteLetter[#|b][octave], or
   [duration]P for a rest. Missing duration/octave fall back to the
   BPM/Duration/Octave fields. Purely a client-side approximation (square
   wave) for authoring convenience — not a byte-exact emulation of the
   Flipper speaker. */
const NOTE_SEMITONE = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };
let previewAudioCtx = null;
let previewOscillators = [];
let previewStopTimer = null;

function parseNoteTokenForPreview(rawTok, defaultDur, defaultOctave) {
  const tok = rawTok.trim();
  if (!tok) return null;
  let i = 0;
  while (i < tok.length && tok[i] >= '0' && tok[i] <= '9') i++;
  let dur = i > 0 ? parseInt(tok.slice(0, i), 10) : defaultDur;
  if (!dur || dur <= 0) dur = defaultDur || 8;

  let rest = false;
  let freq = 0;

  if (tok[i] === 'P' || tok[i] === 'p') {
    rest = true;
    i++;
  } else {
    const letter = (tok[i] || '').toUpperCase();
    if (!(letter in NOTE_SEMITONE)) return null;
    i++;
    let accidental = 0;
    if (tok[i] === '#') { accidental = 1; i++; }
    else if (tok[i] === 'b') { accidental = -1; i++; }

    let octave = defaultOctave || 5;
    let j = i;
    while (j < tok.length && tok[j] >= '0' && tok[j] <= '9') j++;
    if (j > i) { octave = parseInt(tok.slice(i, j), 10); i = j; }

    const midi = (octave + 1) * 12 + NOTE_SEMITONE[letter] + accidental;
    freq = 440 * Math.pow(2, (midi - 69) / 12);
  }

  // FMF dotted notes: each trailing '.' multiplies the length by 1.5,
  // compounding (1.5^n), matching the format spec.
  let dots = 0;
  while (tok[i] === '.') { dots++; i++; }

  return { rest, dur, freq, dots };
}

function stopMusicPreview() {
  for (const osc of previewOscillators) {
    try { osc.stop(); } catch (e) { /* already stopped */ }
  }
  previewOscillators = [];
  if (previewStopTimer) { clearTimeout(previewStopTimer); previewStopTimer = null; }
  if (el.musicPlayBtn) el.musicPlayBtn.textContent = 'PLAY';
}

function playMusicPreview() {
  if (!el.musicNotes) return;
  stopMusicPreview();

  const tokens = el.musicNotes.value.split(',').map((s) => s.trim()).filter(Boolean);
  if (!tokens.length) return;

  if (!previewAudioCtx) previewAudioCtx = new (window.AudioContext || window.webkitAudioContext)();
  const ctx = previewAudioCtx;
  const bpm = Number(el.musicBpm.value) || 120;
  const defDur = Number(el.musicDuration.value) || 8;
  const defOct = Number(el.musicOctave.value) || 5;

  let t = ctx.currentTime + 0.05;
  for (const rawTok of tokens) {
    const note = parseNoteTokenForPreview(rawTok, defDur, defOct);
    if (!note) continue;
    const seconds = (240 / (bpm * note.dur)) * Math.pow(1.5, note.dots || 0);
    if (!note.rest && note.freq > 0) {
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.type = 'square';
      osc.frequency.setValueAtTime(note.freq, t);
      gain.gain.setValueAtTime(0.15, t);
      gain.gain.setValueAtTime(0.15, Math.max(t, t + seconds - 0.02));
      gain.gain.linearRampToValueAtTime(0.0001, t + seconds);
      osc.connect(gain).connect(ctx.destination);
      osc.start(t);
      osc.stop(t + seconds);
      previewOscillators.push(osc);
    }
    t += seconds;
  }

  el.musicPlayBtn.textContent = 'PLAYING…';
  const totalMs = Math.max(0, (t - ctx.currentTime) * 1000);
  previewStopTimer = setTimeout(() => {
    if (el.musicPlayBtn) el.musicPlayBtn.textContent = 'PLAY';
    previewOscillators = [];
  }, totalMs);
}

/* ─── MIDI import ──────────────────────────────────────────────────────
   Reads a standard MIDI file (SMF) and lets the user pick which track
   and channel to import (defaulting to the track with the most note-on
   events, with a "skip drum channel" option on by default), since a
   Flipper's beeper can only sound one pitch at a time and auto-guessing
   "the melody" from a multi-track score is unreliable. Chords within the
   selected track/channel are reduced to a single monophonic line with
   last-note-priority: a new note interrupts whatever was sounding, and
   when it ends playback resumes whatever note was interrupted (if still
   held) rather than silence — the same behavior as a synth's mono mode.
   Each resulting note/rest length is quantized to the nearest FMF
   duration+dots combination and emitted as an FMF-style note list
   compatible with the in-game player. */

class MidiReader {
  constructor(buf) {
    this.view = new DataView(buf);
    this.pos = 0;
  }
  u8() { return this.view.getUint8(this.pos++); }
  u16() { const v = this.view.getUint16(this.pos); this.pos += 2; return v; }
  u32() { const v = this.view.getUint32(this.pos); this.pos += 4; return v; }
  bytes(n) { const arr = new Uint8Array(this.view.buffer, this.view.byteOffset + this.pos, n); this.pos += n; return arr; }
  str(n) { let s = ''; for (let i = 0; i < n; i++) s += String.fromCharCode(this.u8()); return s; }
  varLen() {
    let value = 0;
    for (let i = 0; i < 4; i++) {
      const b = this.u8();
      value = (value << 7) | (b & 0x7f);
      if (!(b & 0x80)) break;
    }
    return value;
  }
  eof() { return this.pos >= this.view.byteLength; }
}

function parseMidiFile(buf) {
  const r = new MidiReader(buf);
  if (r.str(4) !== 'MThd') throw new Error('Not a MIDI file (missing MThd header).');
  const headerLen = r.u32();
  const format = r.u16();
  const ntrks = r.u16();
  const division = r.u16();
  if (division & 0x8000) throw new Error('SMPTE time-coded MIDI files are not supported.');
  const ticksPerQuarter = division || 96;
  r.pos += Math.max(0, headerLen - 6);

  const tracks = [];
  for (let t = 0; t < ntrks && !r.eof(); t++) {
    const id = r.str(4);
    const len = r.u32();
    const trackEnd = r.pos + len;
    if (id !== 'MTrk') { r.pos = trackEnd; continue; }

    const events = [];
    let tick = 0;
    let runningStatus = 0;
    let trackName = '';
    while (r.pos < trackEnd) {
      tick += r.varLen();
      let statusByte = r.u8();
      if (statusByte < 0x80) {
        r.pos--; // this byte is actually the first data byte of a running-status event
        statusByte = runningStatus;
      } else if (statusByte < 0xf0) {
        runningStatus = statusByte;
      }

      if (statusByte === 0xff) {
        const metaType = r.u8();
        const metaLen = r.varLen();
        const data = r.bytes(metaLen);
        if (metaType === 0x51 && metaLen === 3) {
          const usPerQuarter = (data[0] << 16) | (data[1] << 8) | data[2];
          events.push({ tick, type: 'tempo', usPerQuarter });
        } else if ((metaType === 0x03 || metaType === 0x04) && !trackName) {
          let s = '';
          for (let i = 0; i < data.length; i++) s += String.fromCharCode(data[i]);
          trackName = s.trim();
        }
      } else if (statusByte === 0xf0 || statusByte === 0xf7) {
        r.pos += r.varLen();
      } else if (statusByte >= 0x80 && statusByte < 0xf0) {
        const kind = statusByte & 0xf0;
        const channel = statusByte & 0x0f;
        if (kind === 0x90 || kind === 0x80) {
          const note = r.u8();
          const vel = r.u8();
          if (kind === 0x90 && vel > 0) events.push({ tick, type: 'on', note, channel });
          else events.push({ tick, type: 'off', note, channel });
        } else if (kind === 0xa0 || kind === 0xb0 || kind === 0xe0) {
          r.pos += 2;
        } else if (kind === 0xc0 || kind === 0xd0) {
          r.pos += 1;
        }
      } else {
        break; // unrecognized status byte — bail out of this track rather than desync
      }
    }
    r.pos = trackEnd;
    tracks.push({ events, name: trackName });
  }

  return { format, ticksPerQuarter, tracks };
}

/* Collapses one track's (already channel-filtered) note-on/off events
   into non-overlapping segments of a single active pitch, using
   last-note-priority: a new note-on interrupts and silences whatever was
   previously sounding (pushing it onto a stack), and when the current
   note's matching note-off arrives, playback resumes the most recently
   interrupted note that's still held (if any). Matches the mono-voice
   behavior of a real synthesizer far better than picking the single
   highest pitch on overlap would. Assumes `events` is already in
   chronological tick order, which a raw MIDI track stream always is.

   Exception: a genuine chord — several note-ons landing on the *exact
   same* tick — isn't a "new note interrupting the old one", it's several
   voices starting together. Feeding those straight into the interrupt
   logic above picks whichever one the source file happens to list last,
   which is arbitrary (export-order dependent) and often lands on an
   inner/bass voice instead of the melody. So simultaneous onsets are
   collapsed first, keeping only the highest pitch — the usual
   "melody is the top voice" convention — before the interrupt logic
   ever sees them. */
function reduceMonophonic(events) {
  const onsetsByTick = new Map();
  for (const e of events) {
    if (e.type !== 'on') continue;
    if (!onsetsByTick.has(e.tick)) onsetsByTick.set(e.tick, []);
    onsetsByTick.get(e.tick).push(e);
  }
  const dropped = new Set();
  for (const group of onsetsByTick.values()) {
    if (group.length < 2) continue;
    let top = group[0];
    for (const e of group) if (e.note > top.note) top = e;
    for (const e of group) if (e !== top) dropped.add(e);
  }
  if (dropped.size) events = events.filter((e) => !dropped.has(e));

  let current = null;
  const stack = [];
  const segments = [];

  for (const e of events) {
    if (e.type === 'on') {
      if (current) {
        segments.push({ note: current.note, start: current.start, end: e.tick });
        stack.push(current);
      }
      current = { note: e.note, start: e.tick, channel: e.channel };
    } else if (e.type === 'off') {
      if (current && current.note === e.note && current.channel === e.channel) {
        segments.push({ note: current.note, start: current.start, end: e.tick });
        current = stack.length ? stack.pop() : null;
        if (current) current.start = e.tick;
      } else {
        const idx = stack.findIndex((s) => s.note === e.note && s.channel === e.channel);
        if (idx >= 0) stack.splice(idx, 1);
      }
    }
  }
  if (current) {
    const lastTick = events.length ? events[events.length - 1].tick : current.start;
    segments.push({ note: current.note, start: current.start, end: Math.max(lastTick, current.start + 1) });
  }

  segments.sort((a, b) => a.start - b.start);
  return segments.filter((s) => s.end > s.start);
}

const MIDI_SEMITONE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

function midiNoteToName(midiNote) {
  const semitone = ((midiNote % 12) + 12) % 12;
  const octave = Math.floor(midiNote / 12) - 1;
  return { name: MIDI_SEMITONE_NAMES[semitone], octave };
}

const FMF_DURATIONS = [1, 2, 4, 8, 16, 32];
const FMF_DOT_OPTIONS = [0, 1, 2, 3];

/* Finds the [duration denominator, dot count] pair whose length (in
   quarter-note units) is closest to `quarterLen`. */
function quantizeDuration(quarterLen) {
  let best = { d: 8, dots: 0, diff: Infinity };
  for (const d of FMF_DURATIONS) {
    const base = 4 / d;
    for (const dots of FMF_DOT_OPTIONS) {
      const len = base * Math.pow(1.5, dots);
      const diff = Math.abs(len - quarterLen);
      if (diff < best.diff) best = { d, dots, diff };
    }
  }
  return best;
}

function trackNoteOnCount(track) {
  return track.events.reduce((n, e) => n + (e.type === 'on' ? 1 : 0), 0);
}

/* Scans every track for the first tempo meta-event (falling back to the
   MIDI spec default of 120 BPM) to seed the BPM field when a file is
   first imported. */
function midiDefaultBpm(midi) {
  let usPerQuarter = 500000;
  for (const t of midi.tracks) {
    const tempoEvent = t.events.find((e) => e.type === 'tempo');
    if (tempoEvent) { usPerQuarter = tempoEvent.usPerQuarter; break; }
  }
  return Math.max(20, Math.min(400, Math.round(60000000 / usPerQuarter)));
}

/* Converts one track (filtered to a channel selection) into { duration,
   octave, notes, noteCount, usedCount, truncated }, or null if nothing in
   that track/channel is usable. Silence gaps between notes (including
   before the first note) become explicit rest tokens. */
function buildFmfFromTrack(track, ticksPerQuarter, channelVal, skipDrums) {
  const filtered = track.events.filter((e) => {
    if (e.type !== 'on' && e.type !== 'off') return false;
    if (channelVal !== 'all' && e.channel !== Number(channelVal)) return false;
    if (skipDrums && e.channel === 9) return false;
    return true;
  });

  const segments = reduceMonophonic(filtered);
  if (!segments.length) return null;

  const raw = segments.map((s) => {
    const d = quantizeDuration(Math.max((s.end - s.start) / ticksPerQuarter, 4 / 32));
    const { name, octave } = midiNoteToName(s.note);
    return { start: s.start, end: s.end, d, name, octave };
  });

  // Use the most common duration/octave as the level defaults so most
  // tokens can omit them, keeping the exported note list compact.
  const durCounts = new Map();
  const octCounts = new Map();
  for (const r of raw) {
    durCounts.set(r.d.d, (durCounts.get(r.d.d) || 0) + 1);
    octCounts.set(r.octave, (octCounts.get(r.octave) || 0) + 1);
  }
  const pickMode = (counts, fallback) => {
    let bestKey = fallback, bestN = -1;
    for (const [key, n] of counts) if (n > bestN) { bestN = n; bestKey = key; }
    return bestKey;
  };
  const defaultDur = pickMode(durCounts, 8);
  const defaultOctave = pickMode(octCounts, 5);

  // minTicks: gaps shorter than a 1/32 note are rounding noise, not rests.
  const minTicks = Math.max(1, Math.round(ticksPerQuarter / 32));
  const tokens = [];
  let cursor = 0;
  for (const r of raw) {
    if (r.start - cursor > minTicks) {
      const rd = quantizeDuration(Math.max((r.start - cursor) / ticksPerQuarter, 4 / 32));
      const durStr = rd.d === defaultDur ? '' : String(rd.d);
      tokens.push(`${durStr}P${'.'.repeat(rd.dots)}`);
    }
    const durStr = r.d.d === defaultDur ? '' : String(r.d.d);
    const octStr = r.octave === defaultOctave ? '' : String(r.octave);
    tokens.push(`${durStr}${r.name}${octStr}${'.'.repeat(r.d.dots)}`);
    cursor = r.end;
  }

  let notes = tokens.join(', ');
  let truncated = false;
  let usedCount = tokens.length;
  if (notes.length > MUSIC_NOTES_MAX) {
    let acc = '';
    usedCount = 0;
    for (const tok of tokens) {
      const candidate = acc ? `${acc}, ${tok}` : tok;
      if (candidate.length > MUSIC_NOTES_MAX) break;
      acc = candidate;
      usedCount++;
    }
    notes = acc;
    truncated = usedCount < tokens.length;
  }

  return { duration: defaultDur, octave: defaultOctave, notes, noteCount: tokens.length, usedCount, truncated };
}

let midiParsed = null; // the currently-loaded MIDI file, or null

function populateMidiTrackSelect(midi) {
  el.midiTrackSelect.innerHTML = '';
  let bestIdx = 0, bestCount = -1;
  midi.tracks.forEach((t, i) => {
    const noteCount = trackNoteOnCount(t);
    const opt = document.createElement('option');
    opt.value = String(i);
    opt.textContent = `${i + 1}. ${t.name || `Track ${i + 1}`} (${noteCount} notes)`;
    el.midiTrackSelect.appendChild(opt);
    if (noteCount > bestCount) { bestCount = noteCount; bestIdx = i; }
  });
  el.midiTrackSelect.value = String(bestIdx);
}

function populateMidiChannelSelect(track) {
  const channels = new Set();
  for (const e of track.events) {
    if (e.type === 'on' || e.type === 'off') channels.add(e.channel);
  }
  el.midiChannelSelect.innerHTML = '';
  const allOpt = document.createElement('option');
  allOpt.value = 'all';
  allOpt.textContent = 'All channels';
  el.midiChannelSelect.appendChild(allOpt);
  [...channels].sort((a, b) => a - b).forEach((c) => {
    const opt = document.createElement('option');
    opt.value = String(c);
    opt.textContent = `Channel ${c + 1}${c === 9 ? ' (drums)' : ''}`;
    el.midiChannelSelect.appendChild(opt);
  });
  el.midiChannelSelect.value = 'all';
}

/* Re-runs the conversion for the currently selected track/channel/skip-
   drums options and writes the result straight into the music fields —
   the notes textarea doubles as the live preview, same as manual
   editing. Called on import and whenever an import option changes. */
function regenerateMidiImport() {
  if (!midiParsed || !el.midiTrackSelect) return;
  const track = midiParsed.tracks[Number(el.midiTrackSelect.value)];
  if (!track) return;
  const channelVal = el.midiChannelSelect.value;
  const skipDrums = el.midiSkipDrums.checked;

  const result = buildFmfFromTrack(track, midiParsed.ticksPerQuarter, channelVal, skipDrums);
  if (!result) {
    el.musicNotes.value = '';
    syncMusicToLevel();
    if (el.midiImportStats) el.midiImportStats.textContent = 'No notes in this track/channel.';
    return;
  }

  el.musicDuration.value = String(result.duration);
  el.musicOctave.value = String(result.octave);
  el.musicNotes.value = result.notes;
  syncMusicToLevel();

  if (el.midiImportStats) {
    let msg = `${result.usedCount} notes · default ${result.duration}/${result.octave}`;
    if (result.truncated) {
      msg += ` · trimmed from ${result.noteCount} notes (${MUSIC_NOTES_MAX}-char limit)`;
    }
    el.midiImportStats.textContent = msg;
  }
}

function importMidiFile(file) {
  const reader = new FileReader();
  reader.onload = () => {
    try {
      const midi = parseMidiFile(reader.result);
      const totalNotes = midi.tracks.reduce((n, t) => n + trackNoteOnCount(t), 0);
      if (totalNotes <= 0) throw new Error('No notes found in this MIDI file.');

      midiParsed = midi;
      el.musicBpm.value = String(midiDefaultBpm(midi));
      populateMidiTrackSelect(midi);
      populateMidiChannelSelect(midi.tracks[Number(el.midiTrackSelect.value)]);
      if (el.midiSkipDrums) el.midiSkipDrums.checked = true;
      if (el.midiImportFileName) el.midiImportFileName.textContent = file.name;
      if (el.midiImportPanel) el.midiImportPanel.hidden = false;
      regenerateMidiImport();
    } catch (error) {
      alert(`Failed to import MIDI: ${error.message}`);
    } finally {
      el.midiFileInput.value = '';
    }
  };
  reader.onerror = () => {
    alert('Failed to read the MIDI file.');
    el.midiFileInput.value = '';
  };
  reader.readAsArrayBuffer(file);
}

function createPalette() {
  el.palette.innerHTML = '';
  for (const item of TYPES) {
    const button = document.createElement('button');
    button.type = 'button';
    button.dataset.tool = item.type;
    button.innerHTML = `${item.label}<small>${item.hint}</small>`;
    button.addEventListener('click', () => {
      state.tool = item.type;
      updatePaletteActive();
      updateSelectionInfo();
    });
    el.palette.appendChild(button);
  }
  updatePaletteActive();
}

function updatePaletteActive() {
  for (const button of el.palette.querySelectorAll('button')) {
    button.classList.toggle('active', button.dataset.tool === state.tool);
  }
}

function loadFromFile(file) {
  const reader = new FileReader();
  reader.onload = () => {
    try {
      const level = parseLevel(String(reader.result || ''));
      setLevel(level, file.name || 'level.gdlvl');
    } catch (error) {
      alert(`Failed to load level: ${error.message}`);
    }
  };
  reader.readAsText(file);
}

function rotateHoveredOrTool() {
  if (state.hover) {
    const obj = getObjectAt(state.hover.gx, state.hover.gy);
    if (obj && (obj.type === 'SPIKE' || obj.type === 'MINI_SPIKE' || obj.type === 'BLOCK' || obj.type === 'MINI_BLOCK')) {
      pushHistory();
      obj.rot = rotate90(obj.rot || 0);
      setDirty(true);
      render();
      updateSelectionInfo(state.hover.gx, state.hover.gy);
      return;
    }
  }

  if (state.tool === 'SPIKE' || state.tool === 'MINI_SPIKE' || state.tool === 'BLOCK' || state.tool === 'MINI_BLOCK') {
    state.objectRotation = rotate90(state.objectRotation);
    updateSelectionInfo(state.hover?.gx ?? null, state.hover?.gy ?? null);
  }
}

if (el.exitBtn) el.exitBtn.addEventListener('click', () => {
  if (state.dirty) {
    if (!confirm('You have unsaved changes. Discard and return to level list?')) return;
  }
  if (state.isConnected) {
    showStartupOverlay('Select a level or start a new one.');
    showLevelPicker();
  } else {
    showStartupOverlay('Please connect your Flipper to load levels.');
  }
});

el.loadBtn.addEventListener('click', () => {
  el.fileInput.value = '';
  el.fileInput.click();
});

el.fileInput.addEventListener('change', () => {
  const file = el.fileInput.files && el.fileInput.files[0];
  if (file) loadFromFile(file);
});

el.connectBtn.addEventListener('click', async () => {
  el.startupMessage.textContent = 'Connecting to Flipper...';
  try {
    await connectFlipper();
    showLevelPicker();
  } catch (err) {
    console.error(err);
    el.startupMessage.textContent = 'Failed to connect. Please try again.';
  }
});

el.offlineBtn.addEventListener('click', () => {
  state.isConnected = false;
  updateSaveUI();
  hideStartupOverlay();
});


el.saveBtn.addEventListener('click', () => {
  if (!state.isConnected) downloadLevel();
});

el.levelName.addEventListener('input', syncInputsToLevel);
el.bgStyle.addEventListener('change', syncInputsToLevel);
if (el.difficulty) el.difficulty.addEventListener('change', syncInputsToLevel);
el.levelLength.addEventListener('input', syncInputsToLevel);

if (el.musicBtn) el.musicBtn.addEventListener('click', openMusicEditor);
if (el.musicCloseBtn) el.musicCloseBtn.addEventListener('click', closeMusicEditor);
if (el.musicPlayBtn) el.musicPlayBtn.addEventListener('click', playMusicPreview);
if (el.musicStopBtn) el.musicStopBtn.addEventListener('click', stopMusicPreview);
if (el.musicImportMidiBtn) el.musicImportMidiBtn.addEventListener('click', () => el.midiFileInput.click());
if (el.midiFileInput) el.midiFileInput.addEventListener('change', () => {
  const file = el.midiFileInput.files && el.midiFileInput.files[0];
  if (file) importMidiFile(file);
});
if (el.midiTrackSelect) el.midiTrackSelect.addEventListener('change', () => {
  populateMidiChannelSelect(midiParsed.tracks[Number(el.midiTrackSelect.value)]);
  regenerateMidiImport();
});
if (el.midiChannelSelect) el.midiChannelSelect.addEventListener('change', regenerateMidiImport);
if (el.midiSkipDrums) el.midiSkipDrums.addEventListener('change', regenerateMidiImport);
if (el.musicBpm) el.musicBpm.addEventListener('input', syncMusicToLevel);
if (el.musicDuration) el.musicDuration.addEventListener('change', syncMusicToLevel);
if (el.musicOctave) el.musicOctave.addEventListener('input', syncMusicToLevel);
if (el.musicNotes) el.musicNotes.addEventListener('input', syncMusicToLevel);
el.cameraX.addEventListener('input', () => {
  state.cameraX = clamp(Number(el.cameraX.value) || 0, 0, Number(el.cameraX.max) || 0);
  render();
});

window.addEventListener('keydown', (ev) => {
  if (ev.key !== 'r' && ev.key !== 'R') return;
  const tag = (ev.target && ev.target.tagName) ? ev.target.tagName.toLowerCase() : '';
  if (tag === 'input' || tag === 'textarea' || tag === 'select') return;
  ev.preventDefault();
  rotateHoveredOrTool();
});

el.canvas.addEventListener('contextmenu', (ev) => ev.preventDefault());
el.canvas.addEventListener('pointerdown', (ev) => {
  el.canvas.setPointerCapture(ev.pointerId);
  state.dragging = true;
  state.dragButton = ev.button;
  if (ev.button === 2) {
    state.tool = 'ERASE';
    updatePaletteActive();
  }
  if (ev.button === 1) {
    // middle mouse -> start panning
    state.panning = true;
    state.panStartX = ev.clientX;
    state.panStartY = ev.clientY;
    state.panStartCameraX = state.cameraX;
    state.panStartCameraY = state.cameraY;
    return;
  }
  placeFromPointer(ev);
});

el.canvas.addEventListener('pointermove', (ev) => {
  if (state.panning) {
    const dx = ev.clientX - state.panStartX;
    const dy = ev.clientY - state.panStartY;
    // mouse move: update camera. cameraX is in pixels.
    state.cameraX = clamp(state.panStartCameraX - dx, 0, Number(el.cameraX.max) || 0);
    state.cameraY = clamp(state.panStartCameraY + dy, -80, 5 * cell());
    el.cameraX.value = String(Math.round(state.cameraX));
    render();
    return;
  }

  const { gx, gy } = screenToGrid(ev.clientX, ev.clientY);
  state.hover = { gx, gy };
  updateSelectionInfo(gx, gy);
  if (state.dragging) placeFromPointer(ev);
});

el.canvas.addEventListener('pointerup', (ev) => {
  state.dragging = false;
  if (state.dragButton === 2) {
    state.tool = 'BLOCK';
    updatePaletteActive();
    updateSelectionInfo();
  }
  if (state.panning && ev.button === 1) {
    state.panning = false;
  }
  state.dragButton = 0;
  state.hover = null;
});

el.canvas.addEventListener('wheel', (ev) => {
  ev.preventDefault();
  const rect = el.canvas.getBoundingClientRect();
  // Ctrl+wheel -> zoom, otherwise horizontal pan by wheel
  if (ev.ctrlKey) {
    const px = ev.clientX - rect.left;
    const py = ev.clientY - rect.top;
    const beforeWorldX = (px + state.cameraX) / cell();
    const beforeWorldY = (hVisible() - py - state.cameraY) / cell();
    const factor = ev.deltaY > 0 ? 0.9 : 1.1;
    state.zoom = clamp(state.zoom * factor, 0.5, 6);
    const s = cell();
    state.cameraX = beforeWorldX * s - px;
    state.cameraY = clamp(hVisible() - beforeWorldY * s - py, 0, 5 * cell());
    el.cameraX.value = String(Math.round(state.cameraX));
  } else {
    const delta = Math.sign(ev.deltaY) * cell() * 3;
    state.cameraX = clamp(state.cameraX + delta, 0, Number(el.cameraX.max) || 0);
    el.cameraX.value = String(Math.round(state.cameraX));
  }
  render();
  // update hover under cursor because wheel panning/zoom doesn't move the pointer
  try {
    const rect = el.canvas.getBoundingClientRect();
    const cx = ev.clientX;
    const cy = ev.clientY;
    if (cx >= rect.left && cx <= rect.right && cy >= rect.top && cy <= rect.bottom) {
      const { gx, gy } = screenToGrid(cx, cy);
      state.hover = { gx, gy };
      updateSelectionInfo(gx, gy);
      render();
    }
  } catch (e) {
    // noop
  }
}, { passive: false });

window.addEventListener('keydown', (ev) => {
  // Undo/Redo (use ev.code to work on any keyboard layout)
  if (ev.ctrlKey && ev.code === 'KeyZ') {
    ev.preventDefault();
    undo();
    return;
  }
  if (ev.ctrlKey && ev.code === 'KeyY') {
    ev.preventDefault();
    redo();
    return;
  }

  const step = cell() * 2;
  if (ev.key === 'ArrowLeft') {
    state.cameraX = clamp(state.cameraX - step, 0, Number(el.cameraX.max) || 0);
    el.cameraX.value = String(state.cameraX);
    render();
  } else if (ev.key === 'ArrowRight') {
    state.cameraX = clamp(state.cameraX + step, 0, Number(el.cameraX.max) || 0);
    el.cameraX.value = String(state.cameraX);
    render();
  } else if (ev.key === '1') {
    state.tool = 'BLOCK';
    updatePaletteActive();
    updateSelectionInfo();
  } else if (ev.key === '2') {
    state.tool = 'SPIKE';
    updatePaletteActive();
    updateSelectionInfo();
  } else if (ev.key === '3') {
    state.tool = 'MINI_SPIKE';
    updatePaletteActive();
    updateSelectionInfo();
  } else if (ev.key === '4') {
    state.tool = 'MINI_BLOCK';
    updatePaletteActive();
    updateSelectionInfo();
  } else if (ev.key === '5') {
    state.tool = 'JUMPER';
    updatePaletteActive();
    updateSelectionInfo();
  } else if (ev.key === '6') {
    state.tool = 'SPHERE';
    updatePaletteActive();
    updateSelectionInfo();
  } else if (ev.key === '7') {
    state.tool = 'ERASE';
    updatePaletteActive();
    updateSelectionInfo();
  }
});

window.addEventListener('resize', resizeCanvas);

createPalette();
setLevel(blankLevel());
resizeCanvas();
requestAnimationFrame(renderLoop);
showStartupOverlay('Please connect your Flipper to load levels.');
updateSaveUI();
setSavingIcon(false);

/* ---------------- Icon editor ---------------- */
const iconCtx = el.iconCanvas && el.iconCanvas.getContext('2d');
const ICON_SIZE = 8;
const ICON_SCALE = Math.floor((el.iconCanvas ? el.iconCanvas.width : 128) / ICON_SIZE);
const ICON_GRID_COLOR = 'rgba(255, 130, 0, 0.35)';
const iconState = {
  pixels: new Array(ICON_SIZE).fill(0).map(() => new Array(ICON_SIZE).fill(0)),
  drawing: false,
  mode: 'paint',
  dragMode: 'paint',
  lastCellKey: '',
};

function iconCellSize() {
  if (!el.iconCanvas) return 1;
  const rect = el.iconCanvas.getBoundingClientRect();
  return Math.max(1, Math.floor(Math.min(rect.width, rect.height) / ICON_SIZE));
}

function renderIcon() {
  if (!iconCtx) return;
  iconCtx.fillStyle = BLACK;
  iconCtx.fillRect(0, 0, el.iconCanvas.width, el.iconCanvas.height);
  // draw pixels
  for (let y = 0; y < ICON_SIZE; y++) {
    for (let x = 0; x < ICON_SIZE; x++) {
      if (iconState.pixels[y][x]) {
        iconCtx.fillStyle = ORANGE;
        iconCtx.fillRect(x * ICON_SCALE, y * ICON_SCALE, ICON_SCALE, ICON_SCALE);
      } else {
        iconCtx.fillStyle = BLACK;
        iconCtx.fillRect(x * ICON_SCALE, y * ICON_SCALE, ICON_SCALE, ICON_SCALE);
      }
    }
  }
  // grid
  iconCtx.strokeStyle = ICON_GRID_COLOR;
  iconCtx.fillStyle = ICON_GRID_COLOR;
  iconCtx.lineWidth = 1;
  iconCtx.strokeRect(0.5, 0.5, el.iconCanvas.width - 1, el.iconCanvas.height - 1);
  for (let i = 1; i < ICON_SIZE; i++) {
    const p = i * ICON_SCALE;
    iconCtx.fillRect(p, 0, 1, el.iconCanvas.height);
    iconCtx.fillRect(0, p, el.iconCanvas.width, 1);
  }
}

function iconCanvasCoords(clientX, clientY) {
  const rect = el.iconCanvas.getBoundingClientRect();
  const scale = iconCellSize();
  const x = Math.floor((clientX - rect.left) / scale);
  const y = Math.floor((clientY - rect.top) / scale);
  return { x: clamp(x, 0, ICON_SIZE - 1), y: clamp(y, 0, ICON_SIZE - 1) };
}

function setIconPixelAt(clientX, clientY, mode) {
  const { x, y } = iconCanvasCoords(clientX, clientY);
  const cellKey = `${x},${y}`;
  if (iconState.lastCellKey === cellKey && iconState.mode === mode) return;
  iconState.lastCellKey = cellKey;
  iconState.mode = mode;

  const nextValue = mode === 'erase' ? 0 : mode === 'toggle' ? (iconState.pixels[y][x] ? 0 : 1) : 1;
  if (iconState.pixels[y][x] === nextValue) return;

  iconState.pixels[y][x] = nextValue;
  renderIcon();
  updateIconCode();
}

function clearIcon() {
  for (let y = 0; y < ICON_SIZE; y++) for (let x = 0; x < ICON_SIZE; x++) iconState.pixels[y][x] = 0;
  renderIcon();
  updateIconCode();
}

function invertIcon() {
  for (let y = 0; y < ICON_SIZE; y++) for (let x = 0; x < ICON_SIZE; x++) iconState.pixels[y][x] = iconState.pixels[y][x] ? 0 : 1;
  renderIcon();
  updateIconCode();
}

function updateIconCode() {
  // Each row -> byte, bit7 = leftmost
  const bytes = [];
  for (let y = 0; y < ICON_SIZE; y++) {
    let v = 0;
    for (let x = 0; x < ICON_SIZE; x++) {
      v = (v << 1) | (iconState.pixels[y][x] ? 1 : 0);
    }
    bytes.push('0x' + v.toString(16).toUpperCase().padStart(2, '0'));
  }
  const code = `{ ${bytes.join(', ')} }`;
  if (el.iconCode) el.iconCode.textContent = code;
  return code;
}

function showIconEditor() {
  el.iconEditorPanel.hidden = false;
  el.levelsPanel.hidden = true;
  if (el.startupActions) el.startupActions.hidden = true;
  if (el.startupTitle) el.startupTitle.textContent = 'ICON EDITOR';
  if (el.startupMessage) el.startupMessage.textContent = 'Here you can draw a game icon.';
  renderIcon();
  updateIconCode();
}

function closeIconEditor() {
  el.iconEditorPanel.hidden = true;
  if (el.startupActions) el.startupActions.hidden = false;
  if (el.startupTitle) el.startupTitle.textContent = 'LEVEL EDITOR';
  if (el.startupMessage) el.startupMessage.textContent = 'Please connect your Flipper to load levels.';
}

if (el.iconEditorBtn) el.iconEditorBtn.addEventListener('click', () => {
  showIconEditor();
});

if (el.iconClose) el.iconClose.addEventListener('click', () => {
  closeIconEditor();
});

if (el.iconCanvas) {
  el.iconCanvas.addEventListener('pointerdown', (ev) => {
    el.iconCanvas.setPointerCapture(ev.pointerId);
    iconState.drawing = true;
    iconState.lastCellKey = '';
    iconState.mode = ev.button === 2 ? 'erase' : 'toggle';
    iconState.dragMode = ev.button === 2 ? 'erase' : 'toggle';
    setIconPixelAt(ev.clientX, ev.clientY, iconState.mode);
  });
  el.iconCanvas.addEventListener('pointermove', (ev) => {
    if (!iconState.drawing) return;
    setIconPixelAt(ev.clientX, ev.clientY, iconState.dragMode);
  });
  el.iconCanvas.addEventListener('pointerup', (ev) => {
    iconState.drawing = false;
    iconState.lastCellKey = '';
    iconState.dragMode = 'paint';
  });
  el.iconCanvas.addEventListener('contextmenu', (ev) => ev.preventDefault());
}

if (el.iconClear) el.iconClear.addEventListener('click', clearIcon);
if (el.iconInvert) el.iconInvert.addEventListener('click', invertIcon);
if (el.iconCopy) el.iconCopy.addEventListener('click', async () => {
  const code = updateIconCode();
  try { await navigator.clipboard.writeText(code); } catch (e) { console.error(e); }
});

// initialize icon canvas
clearIcon();
