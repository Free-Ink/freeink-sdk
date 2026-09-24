"use strict";

const devices = [
  { id: "x4", label: "X4 800x480", width: 800, height: 480 },
  { id: "x3", label: "X3 792x528", width: 792, height: 528 },
  { id: "papercolor", label: "PaperColor 400x600", width: 400, height: 600 },
  { id: "murphy", label: "Murphy 240x416", width: 240, height: 416 },
  { id: "delink", label: "de-link 800x480", width: 800, height: 480 },
  { id: "sticky", label: "Sticky 800x480", width: 800, height: 480 },
  { id: "m5paper", label: "M5Paper 540x960", width: 540, height: 960 },
  { id: "lilygo", label: "LilyGo 960x540", width: 960, height: 540 },
  { id: "papermono", label: "PaperMono 480x800", width: 480, height: 800 },
];

const supported = new Set([
  "header",
  "footer",
  "list",
  "button",
  "settingRow",
  "toggleRow",
  "stepperRow",
  "checkbox",
  "slider",
  "dropdown",
  "table",
  "radioGroup",
  "spacer",
  "popup",
  "optionDialog",
  "qwertyKeyboard",
  "statusBar",
  "bookCard",
  "textArea",
]);

const state = {
  gallery: null,
  palette: [],
  selectedIndex: -1,
  draggingType: null,
  device: devices[0],
  orientation: "portrait",
  renderSeq: 0,
  previewObjectUrl: "",
  schema: {
    screen: "settings",
    maxInteractions: 32,
    children: [
      { type: "header", title: "Settings", rightLabel: "Back", anchor: "top" },
      { type: "toggleRow", label: "Dark mode", checked: false, action: "toggleDarkMode" },
      { type: "stepperRow", label: "Font size", value: "12", decrement: "fontSmaller", increment: "fontLarger" },
      { type: "qwertyKeyboard", layout: "qwerty_en", action: "keyboardKey", shiftAction: "keyboardShift", deleteAction: "keyboardDelete", okAction: "keyboardOk", anchor: "bottom" },
    ],
  },
};

const el = {
  paletteList: document.getElementById("paletteList"),
  paletteFilter: document.getElementById("paletteFilter"),
  deviceSelect: document.getElementById("deviceSelect"),
  orientationSelect: document.getElementById("orientationSelect"),
  devicePreview: document.getElementById("devicePreview"),
  screenCanvas: document.getElementById("screenCanvas"),
  screenName: document.getElementById("screenName"),
  safeAreaTop: document.getElementById("safeAreaTop"),
  safeAreaRight: document.getElementById("safeAreaRight"),
  safeAreaBottom: document.getElementById("safeAreaBottom"),
  safeAreaLeft: document.getElementById("safeAreaLeft"),
  contentMarginTop: document.getElementById("contentMarginTop"),
  contentMarginRight: document.getElementById("contentMarginRight"),
  contentMarginBottom: document.getElementById("contentMarginBottom"),
  contentMarginLeft: document.getElementById("contentMarginLeft"),
  inspectorFields: document.getElementById("inspectorFields"),
  schemaText: document.getElementById("schemaText"),
  cppOutput: document.getElementById("cppOutput"),
  toast: document.getElementById("toast"),
  deleteBtn: document.getElementById("deleteBtn"),
  applyJsonBtn: document.getElementById("applyJsonBtn"),
  generateBtn: document.getElementById("generateBtn"),
  exportBtn: document.getElementById("exportBtn"),
  importBtn: document.getElementById("importBtn"),
  fileInput: document.getElementById("fileInput"),
  copyCppBtn: document.getElementById("copyCppBtn"),
};

function titleCase(value) {
  return String(value || "")
    .replace(/([a-z])([A-Z])/g, "$1 $2")
    .replace(/[-_]/g, " ")
    .replace(/\b\w/g, (m) => m.toUpperCase());
}

function slugToType(slug) {
  return slug.replace(/-([a-z])/g, (_, c) => c.toUpperCase());
}

function emptyInsets() {
  return { top: 0, right: 0, bottom: 0, left: 0 };
}

function normalizeInsets(value) {
  if (typeof value === "number") return { top: value, right: value, bottom: value, left: value };
  if (Array.isArray(value)) {
    if (value.length === 2) return { top: Number(value[0] || 0), right: Number(value[1] || 0), bottom: Number(value[0] || 0), left: Number(value[1] || 0) };
    if (value.length === 4) return { top: Number(value[0] || 0), right: Number(value[1] || 0), bottom: Number(value[2] || 0), left: Number(value[3] || 0) };
  }
  if (value && typeof value === "object") {
    return {
      top: Number(value.top || 0),
      right: Number(value.right || 0),
      bottom: Number(value.bottom || 0),
      left: Number(value.left || 0),
    };
  }
  return emptyInsets();
}

function hasInsets(value) {
  const insets = normalizeInsets(value);
  return Boolean(insets.top || insets.right || insets.bottom || insets.left);
}

function readInsets(prefix) {
  return {
    top: Number(el[`${prefix}Top`].value || 0),
    right: Number(el[`${prefix}Right`].value || 0),
    bottom: Number(el[`${prefix}Bottom`].value || 0),
    left: Number(el[`${prefix}Left`].value || 0),
  };
}

function writeInsets(prefix, value) {
  const insets = normalizeInsets(value);
  el[`${prefix}Top`].value = insets.top || "";
  el[`${prefix}Right`].value = insets.right || "";
  el[`${prefix}Bottom`].value = insets.bottom || "";
  el[`${prefix}Left`].value = insets.left || "";
}

function setSchemaInsets(name, value) {
  if (hasInsets(value)) state.schema[name] = normalizeInsets(value);
  else delete state.schema[name];
}

function previewDimensions() {
  const shortSide = Math.min(state.device.width, state.device.height);
  const longSide = Math.max(state.device.width, state.device.height);
  return state.orientation === "portrait"
    ? { width: shortSide, height: longSide }
    : { width: longSide, height: shortSide };
}

function defaultKeyboardHeight(child) {
  const dims = previewDimensions();
  const safe = normalizeInsets(state.schema.safeArea);
  const width = dims.width - safe.left - safe.right;
  const safeHeight = dims.height - safe.top - safe.bottom;
  const padding = child.padding ? normalizeInsets(child.padding) : { top: 5, bottom: 5 };
  const gap = Math.max(0, child.rowGap ?? 6);
  const rows = child.numberRow && !child.symbols ? 5 : 4;
  const height = Math.max(64, Math.floor(width / 6)) * rows + gap * (rows - 1)
    + padding.top + padding.bottom;
  return Math.max(0, Math.min(height, Math.floor(safeHeight * 0.5) + Math.max(0, gap - 2) * (rows - 1)));
}

function rowHeight(child) {
  if (child.height) return Number(child.height);
  if (child.type === "qwertyKeyboard") return defaultKeyboardHeight(child);
  if (child.type === "table") return 96;
  if (child.type === "list") return 132;
  if (child.type === "footer") return 44;
  if (child.type === "header") return 40;
  if (child.type === "statusBar") return 40;
  if (child.type === "bookCard") return 88;
  if (child.type === "textArea") return 132;
  return 44;
}

function modalSize(child, scale) {
  const dims = previewDimensions();
  const maxWidth = Number(child.width || child.maxWidth || Math.round(dims.width * 0.75));
  if (child.type === "optionDialog") {
    return {
      width: Math.round(Math.min(maxWidth, dims.width * 0.9) * scale),
      height: Math.round(Math.min(220, dims.height * 0.55) * scale),
    };
  }
  const padding = normalizeInsets(child.padding);
  const text = String(child.message || "Popup");
  const textWidth = Math.min(maxWidth - padding.left - padding.right, Math.max(64, text.length * 7));
  const lines = Math.max(1, Math.ceil((text.length * 7) / Math.max(64, textWidth)));
  return {
    width: Math.round((textWidth + padding.left + padding.right) * scale),
    height: Math.round((lines * 18 + padding.top + padding.bottom) * scale),
  };
}

function defaultChild(type) {
  const action = type.replace(/Row$/, "");
  const defaults = {
    header: { type, title: "Screen", rightLabel: "Back", anchor: "top", borderEdges: "bottom" },
    footer: { type, anchor: "bottom", sidePadding: 8, gap: 4, buttonBorderEdges: "top", buttons: [{ label: "Back", action: "back" }, { label: "Apply", action: "apply" }] },
    button: { type, label: "Button", action: "buttonPressed" },
    settingRow: { type, label: "Wi-Fi", value: "On", action: "openWifi" },
    toggleRow: { type, label: "Dark mode", checked: false, action: "toggleDarkMode" },
    stepperRow: { type, label: "Font size", value: "12", decrement: "fontSmaller", increment: "fontLarger" },
    checkbox: { type, label: "Enable sync", checked: true, action: "toggleSync" },
    slider: { type, value: 42, max: 100, action: "setValue", height: 44 },
    dropdown: { type, label: "Sort", value: "Recent", action: "openSort", indicatorSize: 8, indicatorStroke: 1 },
    radioGroup: { type, selectedValue: 2, action: "selectSize", options: [{ label: "Small", value: 1 }, { label: "Medium", value: 2 }, { label: "Large", value: 3 }] },
    list: { type, action: "selectItem", selectedIndex: 0, items: [{ label: "Library", value: "24" }, { label: "Settings", value: "" }, { label: "About", value: "" }] },
    table: { type, headerRow: true, rows: [["Metric", "Value"], ["Books", "24"], ["Battery", "82%"]], height: 96 },
    spacer: { type, height: 12 },
    popup: { type, message: "Saved", align: "center", maxWidth: 320 },
    optionDialog: {
      type,
      title: "Confirm",
      headline: "Apply changes?",
      message: "This will update the current reader settings.",
      options: [{ label: "Cancel", action: "cancel" }, { label: "Apply", action: "apply" }],
      width: 320,
    },
    qwertyKeyboard: { type, anchor: "bottom", layout: "qwerty_en", action: "keyboardKey", shiftAction: "keyboardShift", modeAction: "keyboardMode", deleteAction: "keyboardDelete", okAction: "keyboardOk" },
    statusBar: { type, title: "Chapter 3", leading: "12:04", trailing: "42 / 180", anchor: "top" },
    bookCard: { type, title: "The Time Machine", author: "H. G. Wells", meta: "EPUB - 214 KB", progress: 42, progressMax: 100, action: "openBook" },
    textArea: { type, text: "Write here...", showCaret: true, height: 132 },
  };
  return defaults[type] ? structuredClone(defaults[type]) : null;
}

function normalizeSchemaDefaults() {
  if (!Array.isArray(state.schema.children)) state.schema.children = [];
  for (const child of state.schema.children) {
    if (child.type === "qwertyKeyboard" && Number(child.height) === 144) delete child.height;
  }
}

function componentSummary(child) {
  if (child.type === "qwertyKeyboard") return "QWERTY keyboard";
  if (child.type === "table") return `${(child.rows || []).length} rows`;
  if (child.type === "list") return `${(child.items || []).length} items`;
  if (child.type === "radioGroup") return `${(child.options || []).length} options`;
  if (child.type === "optionDialog") return `${(child.options || []).length} options`;
  if (child.type === "footer") return `${(child.buttons || []).length} actions`;
  if (child.type === "bookCard") return child.title || child.author || "";
  if (child.type === "statusBar") return child.title || child.leading || "";
  if (child.type === "textArea") return child.text || "";
  return child.value || child.author || child.message || child.action || "";
}

function addChild(type, index = state.schema.children.length) {
  if (!supported.has(type)) {
    showToast(`${type} is not editable in the builder yet`);
    return;
  }
  const child = defaultChild(type);
  if (!child) {
    showToast(`${type} is not editable in the builder yet`);
    return;
  }
  state.selectedIndex = index;
  state.schema.children.splice(index, 0, child);
  renderAll();
}

function moveChild(from, to) {
  if (from === to || from < 0 || to < 0) return;
  const [child] = state.schema.children.splice(from, 1);
  state.schema.children.splice(to, 0, child);
  state.selectedIndex = to;
  renderAll();
}

function syncSchemaMeta() {
  state.schema.screen = el.screenName.value.trim() || "generated";
  if (!state.schema.maxInteractions) state.schema.maxInteractions = 32;
  setSchemaInsets("safeArea", readInsets("safeArea"));
  setSchemaInsets("contentMargin", readInsets("contentMargin"));
  renderSchema();
  renderCanvas();
}

function renderPalette() {
  const filter = el.paletteFilter.value.trim().toLowerCase();
  const grouped = new Map();
  for (const item of state.palette) {
    const type = item.component;
    if (!supported.has(type)) continue;
    if (filter && !type.toLowerCase().includes(filter) && !item.category.toLowerCase().includes(filter)) continue;
    if (!grouped.has(item.category)) grouped.set(item.category, []);
    grouped.get(item.category).push(item);
  }

  el.paletteList.innerHTML = "";
  for (const [category, items] of grouped.entries()) {
    const group = document.createElement("section");
    group.className = "category";
    const heading = document.createElement("h3");
    heading.textContent = titleCase(category);
    group.appendChild(heading);
    for (const item of items) {
      const card = document.createElement("button");
      card.className = "palette-card";
      card.type = "button";
      card.draggable = true;
      card.dataset.type = item.component;
      card.innerHTML = `<img alt="" src="/images/${item.file}"><div><strong>${item.component}</strong><span>${titleCase(item.category)}</span></div>`;
      card.addEventListener("click", () => addChild(item.component));
      card.addEventListener("dragstart", (event) => {
        state.draggingType = item.component;
        event.dataTransfer.setData("text/plain", item.component);
      });
      group.appendChild(card);
    }
    el.paletteList.appendChild(group);
  }
}

function renderDevice() {
  const dims = previewDimensions();
  const maxW = Math.min(460, window.innerWidth - 460);
  const maxH = Math.max(240, window.innerHeight - 220);
  const scale = Math.min(maxW / dims.width, maxH / dims.height, 0.78);
  el.devicePreview.style.width = `${Math.round(dims.width * scale + 30)}px`;
  el.screenCanvas.style.width = `${Math.round(dims.width * scale)}px`;
  el.screenCanvas.style.height = `${Math.round(dims.height * scale)}px`;
  el.screenCanvas.dataset.scale = String(scale);
}

function renderPayloadDimensions() {
  return previewDimensions();
}

function renderCanvas() {
  renderDevice();
  el.screenCanvas.innerHTML = "";
  const scale = Number(el.screenCanvas.dataset.scale || 1);
  const children = state.schema.children || [];
  if (!children.length) {
    const empty = document.createElement("div");
    empty.className = "drop-hint";
    empty.textContent = "Drop components here";
    el.screenCanvas.appendChild(empty);
  }

  const preview = document.createElement("img");
  preview.className = "screen-preview-img is-loading";
  preview.alt = "Rendered FreeInkUI screen preview";
  el.screenCanvas.appendChild(preview);

  const loader = document.createElement("div");
  loader.className = "preview-loader";
  loader.setAttribute("role", "status");
  loader.setAttribute("aria-live", "polite");
  loader.innerHTML = `<span class="preview-loader__spinner" aria-hidden="true"></span><span>Rendering preview</span>`;
  el.screenCanvas.appendChild(loader);

  const topChildren = [];
  const bottomChildren = [];
  const overlayChildren = [];
  children.forEach((child, index) => {
    if (child.type === "popup" || child.type === "optionDialog") overlayChildren.push([child, index]);
    else if ((child.anchor || "").toLowerCase() === "bottom") bottomChildren.push([child, index]);
    else topChildren.push([child, index]);
  });

  const renderItem = (child, index, overlay = false) => {
    const item = document.createElement("div");
    item.className = `screen-item ${child.type}`;
    if (index === state.selectedIndex) item.classList.add("selected");
    if (overlay) {
      const size = modalSize(child, scale);
      item.classList.add("screen-item--overlay");
      item.style.width = `${size.width}px`;
      item.style.height = `${size.height}px`;
      item.style.left = `calc(50% - ${Math.round(size.width / 2)}px)`;
      item.style.top = `calc(50% - ${Math.round(size.height / 2)}px)`;
    } else {
      item.style.height = `${Math.round(rowHeight(child) * scale)}px`;
    }
    item.draggable = true;
    item.dataset.index = String(index);
    item.dataset.label = `${child.type}${componentSummary(child) ? `: ${componentSummary(child)}` : ""}`;
    item.title = `${child.type}${child.anchor ? ` (${child.anchor})` : ""}`;
    item.addEventListener("click", () => {
      state.selectedIndex = index;
      renderAll();
    });
    item.addEventListener("dragstart", (event) => {
      event.dataTransfer.setData("application/x-freeink-index", String(index));
    });
    item.addEventListener("dragover", (event) => {
      event.preventDefault();
      item.classList.add("drag-over");
    });
    item.addEventListener("dragleave", () => item.classList.remove("drag-over"));
    item.addEventListener("drop", (event) => {
      event.preventDefault();
      item.classList.remove("drag-over");
      const from = Number(event.dataTransfer.getData("application/x-freeink-index"));
      const type = event.dataTransfer.getData("text/plain") || state.draggingType;
      if (Number.isFinite(from) && event.dataTransfer.types.includes("application/x-freeink-index")) {
        moveChild(from, index);
      } else if (type) {
        addChild(type, index);
      }
    });
    return item;
  };

  const topRegion = document.createElement("div");
  topRegion.className = "screen-region screen-region--top";
  for (const [child, index] of topChildren) topRegion.appendChild(renderItem(child, index));

  const bottomRegion = document.createElement("div");
  bottomRegion.className = "screen-region screen-region--bottom";
  for (const [child, index] of bottomChildren) {
    bottomRegion.appendChild(renderItem(child, index));
  }

  el.screenCanvas.appendChild(topRegion);
  el.screenCanvas.appendChild(bottomRegion);

  const overlayRegion = document.createElement("div");
  overlayRegion.className = "screen-region screen-region--overlay";
  for (const [child, index] of overlayChildren) overlayRegion.appendChild(renderItem(child, index, true));
  el.screenCanvas.appendChild(overlayRegion);

  el.screenCanvas.addEventListener("dragover", (event) => event.preventDefault(), { once: true });
  updateRenderedPreview(preview, loader);
}

function setPreviewLoaderError(loader, message) {
  loader.classList.add("is-error");
  const text = document.createElement("span");
  text.textContent = message;
  loader.replaceChildren(text);
}

async function updateRenderedPreview(preview, loader) {
  const seq = ++state.renderSeq;
  const dims = renderPayloadDimensions();
  try {
    const response = await fetch("/api/render", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ schema: state.schema, width: dims.width, height: dims.height }),
    });
    const svg = await response.text();
    if (seq !== state.renderSeq) return;
    if (!response.ok) {
      preview.removeAttribute("src");
      preview.classList.add("is-loading");
      setPreviewLoaderError(loader, "Preview render failed");
      showToast("Preview render failed");
      return;
    }
    const url = URL.createObjectURL(new Blob([svg], { type: "image/svg+xml" }));
    preview.onload = () => {
      if (seq !== state.renderSeq) {
        URL.revokeObjectURL(url);
        return;
      }
      if (state.previewObjectUrl) URL.revokeObjectURL(state.previewObjectUrl);
      state.previewObjectUrl = url;
      preview.classList.remove("is-loading");
      loader.remove();
    };
    preview.onerror = () => {
      URL.revokeObjectURL(url);
      if (seq !== state.renderSeq) return;
      preview.removeAttribute("src");
      preview.classList.add("is-loading");
      setPreviewLoaderError(loader, "Preview image failed");
      showToast("Preview image failed");
    };
    preview.src = url;
  } catch (error) {
    if (seq === state.renderSeq) {
      preview.classList.add("is-loading");
      setPreviewLoaderError(loader, error.message);
      showToast(error.message);
    }
  }
}

function fieldSpec(child) {
  const common = [
    ["type", "text", true],
    ["anchor", "select"],
    ["label", "text"],
    ["title", "text"],
    ["subtitle", "text"],
    ["value", "text"],
    ["action", "text"],
    ["height", "number"],
  ];
  const specs = {
    header: [["type", "text", true], ["anchor", "select"], ["title", "text"], ["subtitle", "text"], ["rightLabel", "text"], ["centered", "checkbox"], ["borderEdges", "edgeMask"]],
    footer: [["type", "text", true], ["anchor", "select"], ["sidePadding", "number"], ["gap", "number"], ["buttonBorderEdges", "edgeMask"], ["buttons", "json"]],
    button: [["type", "text", true], ["anchor", "select"], ["label", "text"], ["action", "text"], ["value", "number"], ["radius", "number"]],
    settingRow: [["type", "text", true], ["anchor", "select"], ["label", "text"], ["subtitle", "text"], ["value", "text"], ["action", "text"], ["actionValue", "number"], ["chevron", "checkbox"], ["radius", "number"], ["sidePadding", "number"], ["textGap", "number"]],
    toggleRow: [["type", "text", true], ["anchor", "select"], ["label", "text"], ["subtitle", "text"], ["checked", "checkbox"], ["action", "text"], ["value", "number"], ["radius", "number"], ["knobRadius", "number"], ["rowRadius", "number"], ["sidePadding", "number"], ["textGap", "number"]],
    stepperRow: [["type", "text", true], ["anchor", "select"], ["label", "text"], ["value", "text"], ["decrement", "text"], ["increment", "text"], ["controlSize", "number"], ["controlStroke", "number"], ["buttonRadius", "number"], ["rowRadius", "number"], ["sidePadding", "number"], ["textGap", "number"], ["gap", "number"]],
    checkbox: [["type", "text", true], ["anchor", "select"], ["label", "text"], ["checked", "checkbox"], ["action", "text"], ["value", "number"], ["radius", "number"], ["sidePadding", "number"], ["gap", "number"]],
    slider: [["type", "text", true], ["anchor", "select"], ["value", "number"], ["max", "number"], ["action", "text"], ["radius", "number"], ["height", "number"]],
    dropdown: [["type", "text", true], ["anchor", "select"], ["label", "text"], ["value", "text"], ["action", "text"], ["radius", "number"], ["indicatorWidth", "number"], ["indicatorSize", "number"], ["indicatorStroke", "number"], ["padding", "insets"]],
    radioGroup: [["type", "text", true], ["anchor", "select"], ["selectedValue", "number"], ["action", "text"], ["radius", "number"], ["options", "json"]],
    list: [["type", "text", true], ["anchor", "select"], ["height", "number"], ["action", "text"], ["selectedIndex", "number"], ["topIndex", "number"], ["selectionMarker", "selectionMarker"], ["rowRadius", "number"], ["sidePadding", "number"], ["rowGap", "number"], ["items", "json"]],
    table: [["type", "text", true], ["anchor", "select"], ["height", "number"], ["rowHeight", "number"], ["headerRow", "checkbox"], ["cellRadius", "number"], ["padding", "number"], ["rows", "json"]],
    popup: [["type", "text", true], ["message", "text"], ["align", "textAlign"], ["maxWidth", "number"], ["showProgress", "checkbox"], ["progress", "number"], ["progressMax", "number"], ["progressHeight", "number"], ["padding", "insets"]],
    optionDialog: [["type", "text", true], ["title", "text"], ["headline", "text"], ["message", "text"], ["width", "number"], ["buttonHeight", "number"], ["gap", "number"], ["verticalOptions", "checkbox"], ["dimBackground", "checkbox"], ["padding", "insets"], ["options", "json"]],
    spacer: [["type", "text", true], ["anchor", "select"], ["height", "number"]],
    qwertyKeyboard: [["type", "text", true], ["anchor", "select"], ["layout", "keyboardLayout"], ["action", "text"], ["shiftAction", "text"], ["modeAction", "text"], ["deleteAction", "text"], ["okAction", "text"], ["selectedIndex", "number"], ["shifted", "checkbox"], ["symbols", "checkbox"], ["keyRadius", "number"], ["gap", "number"], ["rowGap", "number"], ["padding", "insets"], ["height", "number"]],
    statusBar: [["type", "text", true], ["anchor", "select"], ["title", "text"], ["leading", "text"], ["leadingSecondary", "text"], ["trailing", "text"], ["trailingSecondary", "text"], ["horizontalPadding", "number"], ["gap", "number"], ["showProgress", "checkbox"], ["progress", "number"], ["progressMax", "number"], ["progressHeight", "number"]],
    bookCard: [["type", "text", true], ["anchor", "select"], ["title", "text"], ["author", "text"], ["meta", "text"], ["progress", "number"], ["progressMax", "number"], ["action", "text"], ["value", "number"], ["gap", "number"], ["textGap", "number"], ["padding", "insets"], ["height", "number"]],
    textArea: [["type", "text", true], ["anchor", "select"], ["text", "text"], ["cursor", "number"], ["topLine", "number"], ["showCaret", "checkbox"], ["selStart", "number"], ["selEnd", "number"], ["height", "number"]],
  };
  return specs[child.type] || common;
}

function renderInspector() {
  const child = state.schema.children[state.selectedIndex];
  el.deleteBtn.disabled = !child;
  el.inspectorFields.innerHTML = "";
  if (!child) {
    el.inspectorFields.textContent = "Select a component.";
    return;
  }
  for (const [name, kind, readonly] of fieldSpec(child)) {
    const wrap = document.createElement("div");
    wrap.className = "field";
    const label = document.createElement("label");
    label.textContent = titleCase(name);
    wrap.appendChild(label);
    let input;
    if (kind === "json") {
      input = document.createElement("textarea");
      input.value = JSON.stringify(child[name] ?? [], null, 2);
    } else if (kind === "insets") {
      input = document.createElement("div");
      input.className = "inset-grid";
      const values = normalizeInsets(child[name]);
      for (const edge of ["top", "right", "bottom", "left"]) {
        const edgeInput = document.createElement("input");
        edgeInput.type = "number";
        edgeInput.min = "0";
        edgeInput.placeholder = edge[0].toUpperCase();
        edgeInput.ariaLabel = `${titleCase(name)} ${edge}`;
        edgeInput.value = values[edge] || "";
        edgeInput.dataset.edge = edge;
        input.appendChild(edgeInput);
      }
    } else if (kind === "select" && name === "anchor") {
      input = document.createElement("select");
      for (const value of ["", "top", "bottom"]) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = value || "default";
        input.appendChild(option);
      }
      input.value = child[name] || "";
    } else if (kind === "keyboardLayout") {
      input = document.createElement("select");
      for (const [value, label] of [
        ["qwerty_en", "QWERTY English"],
        ["azerty_fr", "AZERTY French"],
        ["qwertz_de", "QWERTZ German"],
        ["spanish_es", "Spanish"],
      ]) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = label;
        input.appendChild(option);
      }
      input.value = child[name] || "qwerty_en";
    } else if (kind === "edgeMask") {
      input = document.createElement("select");
      for (const [value, label] of [
        ["none", "None"],
        ["top", "Top"],
        ["bottom", "Bottom"],
        ["topBottom", "Top + bottom"],
        ["leftRight", "Left + right"],
        ["all", "All"],
      ]) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = label;
        input.appendChild(option);
      }
      input.value = child[name] || "top";
    } else if (kind === "selectionMarker") {
      input = document.createElement("select");
      for (const [value, label] of [["none", "None"], ["underline", "Underline"], ["triangle", "Triangle"]]) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = label;
        input.appendChild(option);
      }
      input.value = child[name] || "none";
    } else if (kind === "textAlign") {
      input = document.createElement("select");
      for (const [value, label] of [["left", "Left"], ["center", "Center"], ["right", "Right"]]) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = label;
        input.appendChild(option);
      }
      input.value = child[name] || "left";
    } else {
      input = document.createElement("input");
      input.type = kind;
      if (kind === "checkbox") input.checked = Boolean(child[name]);
      else input.value = child[name] ?? "";
    }
    if (kind !== "insets") input.disabled = Boolean(readonly);
    input.addEventListener("input", (event) => {
      try {
        if (kind === "json") child[name] = JSON.parse(input.value || "null");
        else if (kind === "insets") {
          const next = normalizeInsets(child[name]);
          next[event.target.dataset.edge] = Number(event.target.value || 0);
          if (hasInsets(next)) child[name] = next;
          else delete child[name];
        }
        else if (kind === "checkbox") child[name] = input.checked;
        else if (kind === "number") child[name] = input.value === "" ? undefined : Number(input.value);
        else if (kind === "select" && input.value === "") delete child[name];
        else child[name] = input.value;
        renderSchema();
        renderCanvas();
      } catch {
        input.style.borderColor = "#a33120";
      }
    });
    input.addEventListener("change", () => {
      input.style.borderColor = "";
      renderAll();
    });
    wrap.appendChild(input);
    el.inspectorFields.appendChild(wrap);
  }
}

function renderSchema() {
  el.schemaText.value = JSON.stringify(state.schema, null, 2);
}

function renderMeta() {
  el.screenName.value = state.schema.screen || "generated";
  if (!state.schema.maxInteractions) state.schema.maxInteractions = 32;
  writeInsets("safeArea", state.schema.safeArea);
  writeInsets("contentMargin", state.schema.contentMargin);
}

function renderAll() {
  normalizeSchemaDefaults();
  renderMeta();
  renderCanvas();
  renderInspector();
  renderSchema();
}

function showToast(message) {
  el.toast.textContent = message;
  el.toast.classList.add("visible");
  setTimeout(() => el.toast.classList.remove("visible"), 1800);
}

function download(filename, text, type) {
  const link = document.createElement("a");
  link.href = URL.createObjectURL(new Blob([text], { type }));
  link.download = filename;
  link.click();
  URL.revokeObjectURL(link.href);
}

async function generateCpp() {
  syncSchemaMeta();
  const response = await fetch("/api/generate", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(state.schema),
  });
  const text = await response.text();
  if (!response.ok) {
    el.cppOutput.textContent = text;
    showToast("Generation failed");
    return;
  }
  el.cppOutput.textContent = text;
  showToast("Generated C++");
}

async function init() {
  const gallery = await fetch("/api/gallery").then((response) => response.json());
  state.gallery = gallery;
  state.palette = gallery.palette.map((item) => ({
    ...item,
    component: item.component || slugToType(item.file.replace(/^.*\/|\.svg$/g, "")),
  }));

  for (const device of devices) {
    const option = document.createElement("option");
    option.value = device.id;
    option.textContent = device.label;
    el.deviceSelect.appendChild(option);
  }

  el.paletteFilter.addEventListener("input", renderPalette);
  el.deviceSelect.addEventListener("change", () => {
    state.device = devices.find((device) => device.id === el.deviceSelect.value) || devices[0];
    renderCanvas();
  });
  el.orientationSelect.addEventListener("change", () => {
    state.orientation = el.orientationSelect.value;
    renderCanvas();
  });
  el.screenName.addEventListener("input", syncSchemaMeta);
  for (const prefix of ["safeArea", "contentMargin"]) {
    for (const edge of ["Top", "Right", "Bottom", "Left"]) {
      el[`${prefix}${edge}`].addEventListener("input", syncSchemaMeta);
    }
  }
  el.deleteBtn.addEventListener("click", () => {
    if (state.selectedIndex < 0) return;
    state.schema.children.splice(state.selectedIndex, 1);
    state.selectedIndex = Math.min(state.selectedIndex, state.schema.children.length - 1);
    renderAll();
  });
  el.applyJsonBtn.addEventListener("click", () => {
    try {
      state.schema = JSON.parse(el.schemaText.value);
      state.selectedIndex = -1;
      renderAll();
      showToast("Schema applied");
    } catch (error) {
      showToast(error.message);
    }
  });
  el.generateBtn.addEventListener("click", generateCpp);
  el.exportBtn.addEventListener("click", () => {
    syncSchemaMeta();
    download(`${state.schema.screen || "screen"}.freeinkui.json`, JSON.stringify(state.schema, null, 2), "application/json");
  });
  el.importBtn.addEventListener("click", () => el.fileInput.click());
  el.fileInput.addEventListener("change", async () => {
    const file = el.fileInput.files[0];
    if (!file) return;
    state.schema = JSON.parse(await file.text());
    state.selectedIndex = -1;
    renderAll();
  });
  el.copyCppBtn.addEventListener("click", async () => {
    await navigator.clipboard.writeText(el.cppOutput.textContent);
    showToast("Copied");
  });
  el.screenCanvas.addEventListener("dragover", (event) => event.preventDefault());
  el.screenCanvas.addEventListener("drop", (event) => {
    event.preventDefault();
    const type = event.dataTransfer.getData("text/plain") || state.draggingType;
    if (type) addChild(type);
  });
  window.addEventListener("resize", renderCanvas);

  renderPalette();
  renderAll();
}

init().catch((error) => {
  console.error(error);
  showToast(error.message);
});
