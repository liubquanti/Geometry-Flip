const ORANGE = '#ff8200';
const BLACK = '#000000';
const GRID = 8;
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
  levelLength: document.getElementById('levelLength'),
  cameraX: document.getElementById('cameraX'),
  newLevelBtn: document.getElementById('newLevelBtn'),
  loadBtn: document.getElementById('loadBtn'),
  saveBtn: document.getElementById('saveBtn'),
  fileInput: document.getElementById('fileInput'),
  palette: document.getElementById('palette'),
  selectionInfo: document.getElementById('selectionInfo'),
};

const ctx = el.canvas.getContext('2d');
ctx.imageSmoothingEnabled = false;

const state = {
  tool: 'BLOCK',
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
};

function cell() { return GRID * (state.zoom || 1); }

function blankLevel() {
  return {
    name: 'Untitled Level',
    speed: 2,
    gravityPct: 100,
    bgStyle: '0',
    length: 2000,
    objects: [],
    decorations: [],
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
      setDirty();
    }
    return;
  }

  // Check if object already exists at this position with same type
  const existing = state.level.objects.find((obj) => obj.gx === gx && obj.gy === gy);
  const isSameType = existing && existing.type === type;
  
  // Only make changes if we're actually changing something
  if (!isSameType) {
    pushHistory();
    state.level.objects = state.level.objects.filter((obj) => !(obj.gx === gx && obj.gy === gy));
    state.level.objects.push({ type, gx, gy });
    state.level.objects.sort((a, b) => a.gx - b.gx || a.gy - b.gy || a.type.localeCompare(b.type));
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
  el.selectionInfo.textContent = `${state.tool}${hoverText}`;
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

function drawMiniBlock(x, y, w, h) {
  ctx.fillStyle = ORANGE;
  ctx.fillRect(x, y, w, h);
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
      drawSpike(x, y, s, s);
      break;
    case 'MINI_SPIKE':
      drawMiniSpike(x, y + s / 2, s, s / 2);
      break;
    case 'MINI_BLOCK':
      drawMiniBlock(x, y + s / 2, s, s / 2);
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
  el.levelLength.value = state.level.length;
  el.cameraX.max = String(Math.max(0, state.level.length));
  el.cameraX.value = String(clamp(state.cameraX, 0, Number(el.cameraX.max)));
  updateSelectionInfo();
  render();
}

function setLevel(level, fileName = 'untitled.gdlvl') {
  state.level = level;
  state.fileName = fileName;
  state.cameraX = 0;
  state.cameraY = 0;
  state.dirty = false;
  applyLevelToUI();
  setDirty(false);
}

function parseLevel(text) {
  const level = blankLevel();
  const lines = text.split(/\r?\n/);

  for (const raw of lines) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;

    const parts = line.split(/\s+/);
    const key = parts[0].toUpperCase();

    if (key === 'NAME' && parts.length >= 2) {
      level.name = raw.slice(raw.indexOf(' ') + 1).trim();
    } else if (key === 'BG' && parts[1]) {
      level.bgStyle = parts[1];
    } else if (key === 'LENGTH' && parts[1]) {
      level.length = Number(parts[1]) || level.length;
    } else if (key === 'SPEED' && parts[1]) {
      level.speed = Number(parts[1]) || level.speed;
    } else if (key === 'GRAVITY' && parts[1]) {
      level.gravityPct = Number(parts[1]) || level.gravityPct;
    } else if (key === 'OBJ' && parts.length >= 4) {
      const type = parts[1].toUpperCase();
      const gx = Number(parts[2]);
      const gy = Number(parts[3]);
      if (!Number.isFinite(gx) || !Number.isFinite(gy)) continue;
      level.objects.push({ type, gx, gy });
    } else if (key === 'DEC' && parts.length >= 4) {
      const type = parts[1].toUpperCase();
      const x = Number(parts[2]);
      const y = Number(parts[3]);
      if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
      level.decorations.push({ type, x, y });
    }
  }

  level.objects.sort((a, b) => a.gx - b.gx || a.gy - b.gy || a.type.localeCompare(b.type));
  return level;
}

function serializeLevel(level) {
  const lines = [
    '# Geometry Flip level generated by web editor',
    `NAME ${level.name}`,
    `BG ${level.bgStyle}`,
    `LENGTH ${level.length}`,
    '',
    '# OBJ <TYPE> <GX> <GY>',
  ];

  for (const obj of level.objects) {
    lines.push(`OBJ ${obj.type} ${obj.gx} ${obj.gy}`);
  }

  if (level.decorations.length) {
    lines.push('', '# DEC <TYPE> <X> <Y>');
    for (const dec of level.decorations) {
      lines.push(`DEC ${dec.type} ${dec.x} ${dec.y}`);
    }
  }

  return lines.join('\n') + '\n';
}

function downloadLevel() {
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
}

function syncInputsToLevel() {
  const oldName = state.level.name;
  const oldBgStyle = state.level.bgStyle;
  const oldLength = state.level.length;
  
  const newName = el.levelName.value.trim() || 'Untitled Level';
  const newBgStyle = el.bgStyle.value;
  const newLength = Math.max(64, Number(el.levelLength.value) || 2000);
  
  // Only push history and apply changes if something is about to change
  if (oldName !== newName || oldBgStyle !== newBgStyle || oldLength !== newLength) {
    pushHistory();
    state.level.name = newName;
    state.level.bgStyle = newBgStyle;
    state.level.length = newLength;
    setDirty(true);
  }
  
  el.cameraX.max = String(state.level.length);
  state.cameraX = clamp(Number(el.cameraX.value) || 0, 0, state.level.length);
  el.cameraX.value = String(state.cameraX);
  render();
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

el.newLevelBtn.addEventListener('click', () => {
  setLevel(blankLevel(), 'untitled.gdlvl');
});

el.loadBtn.addEventListener('click', () => {
  el.fileInput.value = '';
  el.fileInput.click();
});

el.fileInput.addEventListener('change', () => {
  const file = el.fileInput.files && el.fileInput.files[0];
  if (file) loadFromFile(file);
});

el.saveBtn.addEventListener('click', downloadLevel);

el.levelName.addEventListener('input', syncInputsToLevel);
el.bgStyle.addEventListener('change', syncInputsToLevel);
el.levelLength.addEventListener('input', syncInputsToLevel);
el.cameraX.addEventListener('input', () => {
  state.cameraX = clamp(Number(el.cameraX.value) || 0, 0, Number(el.cameraX.max) || 0);
  render();
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
