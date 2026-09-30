(() => {
  "use strict";

  document.documentElement.classList.toggle("outerloop-host", /(^|\.)outerlooplocal$/i.test(window.location.hostname));

  const decoder = new TextDecoder();
  const elements = {
    shell: document.querySelector("#app"),
    edit: document.querySelector("#edit-button"),
    urlsPage: document.querySelector("#urls-page"),
    directory: document.querySelector("#url-directory"),
    sections: document.querySelector("#app-sections"),
    empty: document.querySelector("#empty-state"),
    summary: document.querySelector("#app-summary"),
    status: document.querySelector("#status-banner"),
    search: document.querySelector("#search-input"),
    add: document.querySelector("#add-button"),
    refresh: document.querySelector("#refresh-button"),
    dialogLayer: document.querySelector("#dialog-layer"),
    dialogTemplate: document.querySelector("#dialog-template"),
    toasts: document.querySelector("#toast-region")
  };

  const state = {
    backends: [],
    bookmarks: null,
    bookmarkOrder: [],
    folderPositions: {},
    endpointNames: {},
    folderWidths: {},
    folderResize: null,
    bookmarkStorageFailed: false,
    bookmarksMarkup: "",
    containerFolders: {},
    directoryMarkup: "",
    safeSpaces: [],
    providers: [],
    containerDownloadURL: null,
    loading: true,
    safeSpacesLoading: true,
    safeSpacesRefreshing: false,
    backendError: "",
    safeSpacesError: "",
    safeSpaceBusy: new Set(),
    safeSpacesTimer: null,
    busy: false,
    query: "",
    editing: false,
    addTab: "catalog",
    backendsVersion: 0n,
    logVersion: 0n,
    logSelection: null,
    eventAbort: null,
    stopped: false,
    suspended: document.hidden,
    refreshAbort: {},
    refreshErrorTimers: {},
    longPress: null,
    suppressLaunchUntil: 0,
    pageScrollY: null,
    pressFeedback: null,
    appDrag: null
  };

  class PayloadReader {
    constructor(value) {
      this.bytes = value instanceof Uint8Array ? value : new Uint8Array(value);
      this.view = new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
    }

    require(offset, length) {
      if (offset < 0 || length < 0 || offset + length > this.bytes.byteLength) {
        throw new Error("Outer Shell returned an invalid binary payload.");
      }
    }

    u32(offset) {
      this.require(offset, 4);
      return this.view.getUint32(offset, true);
    }

    u64(offset) {
      this.require(offset, 8);
      if (typeof this.view.getBigUint64 === "function") return this.view.getBigUint64(offset, true);
      return BigInt(this.u32(offset)) | (BigInt(this.u32(offset + 4)) << 32n);
    }

    f64(offset) {
      this.require(offset, 8);
      return this.view.getFloat64(offset, true);
    }

    bytesRef(offset) {
      const start = this.u32(offset);
      const length = this.u32(offset + 4);
      this.require(start, length);
      return this.bytes.subarray(start, start + length);
    }

    stringRef(offset) {
      const bytes = this.bytesRef(offset);
      return bytes.length ? decoder.decode(bytes) : "";
    }

    child(offset) {
      return new PayloadReader(this.bytesRef(offset));
    }

    payloadArray() {
      const count = this.u32(0);
      const result = [];
      for (let index = 0; index < count; index += 1) result.push(this.child(4 + index * 8));
      return result;
    }
  }

  function decodeBackends(buffer) {
    const reader = new PayloadReader(buffer);
    const count = reader.u32(8);
    const backends = [];
    for (let index = 0; index < count; index += 1) {
      backends.push(decodeBackend(reader.child(12 + index * 8)));
    }
    return { error: reader.stringRef(0), backends };
  }

  function decodeBackend(reader) {
    const flags = reader.u32(64);
    const value = {
      serviceID: reader.stringRef(0),
      displayName: reader.stringRef(8),
      serviceUnit: reader.stringRef(16),
      serviceUnitPath: reader.stringRef(24),
      serviceScope: reader.stringRef(32),
      status: reader.stringRef(40),
      iconSymbolName: reader.stringRef(48),
      launchdPlistPath: reader.stringRef(56),
      canControl: Boolean(flags & 0x01),
      canUninstall: Boolean(flags & 0x02),
      isBundled: Boolean(flags & 0x04),
      isInstalled: Boolean(flags & 0x08),
      isMigration: Boolean(flags & 0x10),
      ownsLaunchdPlist: Boolean(flags & 0x20),
      supportsRoot: Boolean(flags & 0x40),
      rootOnly: Boolean(flags & 0x80),
      hasRootSupport: Boolean(flags & 0x100),
      menuBarVisibilityEnabled: Boolean(flags & 0x200),
      menuBarVisibilityAvailable: Boolean(flags & 0x400),
      frontends: reader.child(68).payloadArray().map(decodeFrontend),
      logFiles: reader.child(76).payloadArray().map(decodeLogFile),
      installedVersion: "",
      availableVersion: "",
      scriptPath: "",
      publicBaseURL: ""
    };
    if (reader.bytes.length >= 92) value.installedVersion = reader.stringRef(84);
    if (reader.bytes.length >= 100) value.availableVersion = reader.stringRef(92);
    if (reader.bytes.length >= 108) value.scriptPath = reader.stringRef(100);
    if (reader.bytes.length >= 116) value.publicBaseURL = reader.stringRef(108);
    return value;
  }

  function decodeFrontend(reader) {
    const flags = reader.bytes.length >= 64 ? reader.u32(60) : 1;
    return {
      name: reader.stringRef(0),
      url: reader.stringRef(8),
      socketPath: reader.stringRef(16),
      iconPath: reader.stringRef(24),
      iconData: reader.bytesRef(32),
      list: reader.stringRef(40),
      port: reader.u32(48),
      id: reader.bytes.length >= 60 ? reader.stringRef(52) : "",
      iconObservationToken: reader.bytes.length >= 72 ? reader.stringRef(64) : "",
      isRunning: Boolean(flags & 0x01)
    };
  }

  function decodeLogFile(reader) {
    return {
      identifier: reader.stringRef(0),
      displayName: reader.stringRef(8),
      path: reader.stringRef(16),
      size: Number(reader.u64(24)),
      modified: reader.f64(32),
      readable: Boolean(reader.u32(40) & 0x01)
    };
  }

  function decodeAction(buffer) {
    const reader = new PayloadReader(buffer);
    const flags = reader.u32(0);
    return {
      ok: Boolean(flags & 0x01),
      needsPassword: Boolean(flags & 0x02),
      updateAvailable: Boolean(flags & 0x04),
      message: reader.stringRef(4),
      installedVersion: reader.bytes.length >= 20 ? reader.stringRef(12) : "",
      availableVersion: reader.bytes.length >= 28 ? reader.stringRef(20) : ""
    };
  }

  function decodeLog(buffer) {
    const reader = new PayloadReader(buffer);
    return {
      serviceID: reader.stringRef(0),
      path: reader.stringRef(8),
      contents: reader.stringRef(16),
      truncated: Boolean(reader.u32(24) & 0x01),
      fileSize: Number(reader.u64(28)),
      modified: reader.f64(36),
      error: reader.stringRef(44)
    };
  }

  function decodeEvents(buffer) {
    const reader = new PayloadReader(buffer);
    const flags = reader.u32(0);
    return {
      backendsChanged: Boolean(flags & 0x01),
      logChanged: Boolean(flags & 0x02),
      timedOut: Boolean(flags & 0x04),
      backendsVersion: reader.u64(8),
      logVersion: reader.u64(16)
    };
  }

  const escapeHTML = value => String(value ?? "").replace(/[&<>'"]/g, character => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;"
  })[character]);

  function backendKey(backend) {
    return [backend.serviceID, backend.serviceScope, backend.serviceUnitPath || backend.serviceUnit].join("\u001f");
  }

  function findBackend(key) {
    return state.backends.find(backend => backendKey(backend) === key);
  }

  function initials(name) {
    const parts = String(name || "App").trim().split(/\s+/).filter(Boolean);
    return parts.slice(0, 2).map(part => part[0]).join("").toUpperCase() || "A";
  }

  function dataURL(bytes) {
    if (!bytes || !bytes.length) return "";
    let binary = "";
    for (let offset = 0; offset < bytes.length; offset += 0x8000) {
      binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
    }
    return `data:image/png;base64,${btoa(binary)}`;
  }

  function iconHTML(item, extraClass = "") {
    const source = dataURL(item.frontend?.iconData);
    const content = source
      ? `<img src="${source}" alt="">`
      : `<span>${escapeHTML(initials(item.displayName || item.backend.displayName))}</span>`;
    return `<span class="app-icon ${extraClass}" aria-hidden="true">${content}</span>`;
  }

  function launcherIconHTML(item) {
    const source = item.app?.iconData
      ? `data:image/png;base64,${escapeHTML(item.app.iconData)}`
      : dataURL(item.frontend?.iconData);
    const content = source
      ? `<img src="${source}" alt="">`
      : `<span class="launcher-fallback" aria-hidden="true">${escapeHTML(initials(item.displayName).slice(0, 1))}</span>`;
    return `<span class="launcher-icon" aria-hidden="true">${content}</span>`;
  }

  function listIconHTML(item) {
    const source = item.app?.iconData
      ? `data:image/png;base64,${escapeHTML(item.app.iconData)}`
      : dataURL(item.frontend?.iconData);
    const content = source
      ? `<img src="${source}" alt="">`
      : `<span>${escapeHTML(initials(item.displayName).slice(0, 1))}</span>`;
    return `<span class="list-icon${source ? " has-image" : ""}" aria-hidden="true">${content}</span>`;
  }

  function safeSpaceAppIconHTML(app) {
    const source = app.iconData ? `data:image/png;base64,${app.iconData}` : "";
    const content = source
      ? `<img src="${source}" alt="">`
      : `<span>${escapeHTML(initials(app.displayName || app.serviceID).slice(0, 1))}</span>`;
    return `<span class="safe-space-app-icon${source ? " has-image" : ""}" aria-hidden="true">${content}</span>`;
  }


  function editButtonHTML(item) {
    return `<button class="bookmark-menu" type="button" data-action="edit-bookmark" data-app-key="${escapeHTML(item.identity)}" aria-label="Edit ${escapeHTML(item.displayName)}" aria-haspopup="dialog" ${state.editing ? "" : "hidden"}>•••</button>`;
  }

  function normalizedPath(value) {
    const trimmed = String(value || "").trim();
    if (!trimmed) return "/";
    if (trimmed.startsWith("/")) return trimmed;
    if (trimmed.startsWith("?")) return `/${trimmed}`;
    return `/${trimmed}`;
  }

  function pathAndQuery(frontend) {
    const raw = String(frontend.url || "").trim();
    const socket = String(frontend.socketPath || "").trim();
    if (socket && raw === socket) return "/";
    if (socket && raw.startsWith(socket)) return normalizedPath(raw.slice(socket.length));
    if (raw.toLowerCase().startsWith("http+unix://")) {
      const rest = raw.slice("http+unix://".length);
      const index = rest.search(/[/?#]/);
      return index < 0 ? "/" : normalizedPath(rest.slice(index));
    }
    try {
      const parsed = new URL(raw);
      return `${parsed.pathname || "/"}${parsed.search}`;
    } catch (_) {
      const slash = raw.indexOf("/");
      return slash < 0 ? "/" : normalizedPath(raw.slice(slash));
    }
  }

  function navigationURL(frontend) {
    const socket = String(frontend.socketPath || "").trim();
    let target;
    if (socket) target = `http+unix://${encodeURIComponent(socket)}${pathAndQuery(frontend)}`;
    else if (frontend.port > 0) target = `http://127.0.0.1:${frontend.port}${pathAndQuery(frontend)}`;
    else target = String(frontend.url || "").trim() || "#";

    const token = String(frontend.iconObservationToken || "");
    if (window.outerLoopPageIconObservationSupported === true && !frontend.iconData?.length && token) {
      const callback = new URL("/api/icon-observation", window.location.href);
      callback.search = new URLSearchParams({ token }).toString();
      const observation = new URL("outerloop://observe-page-icon");
      observation.search = new URLSearchParams({ url: target, callback: callback.href }).toString();
      return observation.href;
    }
    return target;
  }

  function endpointRunning(endpoint) {
    const hasEndpoint = Boolean(endpoint.frontend.socketPath || endpoint.frontend.port > 0 || /^[a-z][a-z0-9+.-]*:/i.test(endpoint.frontend.url || ""));
    return hasEndpoint ? endpoint.frontend.isRunning : endpoint.frontend.isRunning || endpoint.backend.status === "running";
  }

  function endpointReady(endpoint) {
    const frontend = endpoint.frontend;
    const hasEndpoint = Boolean(frontend.socketPath || frontend.port > 0 || /^[a-z][a-z0-9+.-]*:/i.test(frontend.url || ""));
    return endpointRunning(endpoint) || (["available", "awaiting"].includes(endpoint.backend.status) && hasEndpoint);
  }

  function launcherItems(includeShell = false) {
    const endpoints = state.backends.flatMap(backend => {
      if ((!includeShell && backend.serviceID === "org.outershell.OuterShell") || !backend.isInstalled) return [];
      return backend.frontends.map(frontend => ({ backend, frontend }));
    });
    return endpoints.map(endpoint => {
      const root = endpoint.backend.serviceScope === "system";
      const name = endpoint.frontend.name.trim() || endpoint.backend.displayName.trim() || "Untitled";
      return {
        identity: [backendKey(endpoint.backend), endpoint.frontend.id || endpoint.frontend.url].join("\u001f"),
        primary: endpoint,
        backend: endpoint.backend,
        frontend: endpoint.frontend,
        displayName: state.endpointNames[hostBookmarkKey(endpoint)] || name,
        bookmarkContext: root ? "root" : "",
        subtitle: endpoint.backend.displayName.trim() || endpoint.backend.serviceID
      };
    }).sort((left, right) => left.displayName.localeCompare(right.displayName, undefined, { sensitivity: "base" }));
  }

  const bookmarkStorageKey = "outer-shell.home-bookmarks.v1";

  function hostBookmarkKey(item) {
    if (item.workspace) return containerBookmarkKey(item.workspace, item.app);
    return JSON.stringify(["host", item.backend.serviceScope, item.backend.serviceID, item.frontend.id || navigationURL(item.frontend)]);
  }

  function containerFrontendKey(app) {
    return JSON.stringify([app.frontendID || "", app.socketPath || "", app.url || "", app.port || 0]);
  }

  function findSafeSpaceApp(workspace, serviceID, frontendKey = "") {
    return workspace?.apps?.find(app => app.serviceID === serviceID && (!frontendKey || containerFrontendKey(app) === frontendKey));
  }

  function containerBookmarkKey(workspace, app) {
    return JSON.stringify(["container", workspace.id, app.serviceID, containerFrontendKey(app)]);
  }

  function loadBookmarks() {
    try {
      const names = JSON.parse(localStorage.getItem("outer-shell.endpoint-names.v1") || "{}");
      if (names && typeof names === "object" && !Array.isArray(names)) state.endpointNames = Object.fromEntries(Object.entries(names).filter(([, name]) => typeof name === "string" && name.trim()));
    } catch (error) { toast("Could not read endpoint names.", true); }
    try {
      const widths = JSON.parse(localStorage.getItem("outer-shell.folder-widths.v1") || "{}");
      if (widths && typeof widths === "object" && !Array.isArray(widths)) {
        state.folderWidths = Object.fromEntries(Object.entries(widths).filter(([, width]) => Number.isFinite(width) && width >= 160));
      }
    } catch (error) { toast("Could not read folder widths.", true); }
    try {
      const positions = JSON.parse(localStorage.getItem("outer-shell.folder-positions.v1") || "{}");
      if (positions && typeof positions === "object" && !Array.isArray(positions)) state.folderPositions = positions;
    } catch (error) { toast("Could not read folder positions.", true); }
    try {
      const order = JSON.parse(window.localStorage.getItem("outer-shell.bookmark-order.v1") || "[]");
      if (!Array.isArray(order) || order.some(key => typeof key !== "string")) throw new Error("Invalid bookmark order");
      state.bookmarkOrder = [...new Set(order)];
    } catch (error) { toast("Could not read bookmark order.", true); }
    try {
      const folders = JSON.parse(window.localStorage.getItem("outer-shell.container-folders.v1") || "{}");
      if (folders && typeof folders === "object" && !Array.isArray(folders)) state.containerFolders = folders;
      const saved = window.localStorage.getItem(bookmarkStorageKey);
      if (saved !== null) {
        const values = JSON.parse(saved);
        if (!Array.isArray(values) || values.some(value => typeof value !== "string")) throw new Error("Invalid bookmark data");
        state.bookmarks = new Set(values);
      }
    } catch (error) {
      state.bookmarkStorageFailed = true;
      toast("Could not read saved Bookmarks. Your saved choices have not been changed.", true);
    }
  }

  function initializeBookmarks() {
    if (state.bookmarks !== null) return;
    const keys = launcherItems().map(hostBookmarkKey);
    state.bookmarks = new Set(keys);
    if (state.bookmarkStorageFailed) return;
    try { window.localStorage.setItem(bookmarkStorageKey, JSON.stringify(keys)); }
    catch (error) { state.bookmarkStorageFailed = true; toast("Bookmarks cannot be saved in this browser.", true); }
  }

  function isBookmarked(key) {
    return state.bookmarks?.has(key) || false;
  }

  function setBookmark(key, included) {
    if (state.bookmarkStorageFailed || state.bookmarks === null) {
      toast("Bookmarks are unavailable. Enable browser storage and reload to try again.", true);
      return false;
    }
    const next = new Set(state.bookmarks);
    if (included) next.add(key);
    else next.delete(key);
    try { window.localStorage.setItem(bookmarkStorageKey, JSON.stringify([...next])); }
    catch (error) { toast("Could not save this bookmark. Bookmarks have not changed.", true); return false; }
    state.bookmarks = next;
    return true;
  }

  function bookmarkMenuHTML(key) {
    return `<button class="context-menu-item" type="button" data-action="toggle-bookmark" data-bookmark-key="${escapeHTML(key)}" role="menuitem">${contextMenuGlyph(isBookmarked(key) ? "−" : "☆")}<span>${isBookmarked(key) ? "Remove bookmark" : "Add bookmark"}</span></button>`;
  }

  function updatePage() {
    document.title = "Outer Shell";
    renderDirectory();
  }

  function addressKind(frontend) {
    const socket = String(frontend.socketPath || "");
    if (!socket) return Number(frontend.port) > 0 ? "Ports" : "Other endpoints";
    if (/^\/(?:var\/)?run\/user\/0(?:\/|$)/.test(socket)) return "Root sockets";
    if (/^\/(?:var\/)?run\/user\/\d+\//.test(socket)) return "User sockets";
    if (/^\/(?:var\/)?run\//.test(socket)) return "Root sockets";
    return "Custom sockets";
  }

  function registeredURL(frontend) {
    const socket = String(frontend.socketPath || "").trim();
    if (socket) return `http+unix://${encodeURIComponent(socket)}${pathAndQuery(frontend)}`;
    if (Number(frontend.port) > 0) return `http://127.0.0.1:${Number(frontend.port)}${pathAndQuery(frontend)}`;
    return String(frontend.url || "");
  }

  function registeredAddresses() {
    const host = launcherItems(true).map(item => ({
      key: hostBookmarkKey(item), name: item.displayName, item,
      kind: addressKind(item.frontend), address: registeredURL(item.frontend),
      socket: item.frontend.socketPath || "", running: endpointRunning(item.primary),
      serviceID: item.backend.serviceID, groupID: "host", groupName: "This server"
    }));
    const containers = state.safeSpaces.flatMap(workspace => (workspace.apps || []).map(app => {
      const frontend = { ...app, port: app.port || 0 };
      return {
        key: containerBookmarkKey(workspace, app), name: state.endpointNames[containerBookmarkKey(workspace, app)] || app.displayName || app.serviceID,
        workspace, app, kind: addressKind(frontend), address: registeredURL(frontend),
        socket: app.socketPath || "", running: safeSpaceAppIsReady(workspace, app),
        serviceID: app.serviceID, groupID: workspace.id, groupName: workspace.name || "Container"
      };
    }));
    return [...host, ...containers];
  }

  function directoryRowHTML(entry) {
    const launchAttributes = entry.item
      ? `data-action="launch" data-app-key="${escapeHTML(entry.item.identity)}"`
      : `data-action="launch-safe-space-app" data-safe-space-id="${escapeHTML(entry.workspace.id)}" data-service-id="${escapeHTML(entry.app.serviceID)}" data-frontend-key="${escapeHTML(containerFrontendKey(entry.app))}"`;
    const target = entry.item ? navigationURL(entry.item.frontend) : safeSpaceAppNavigationURL(entry.workspace, entry.app);
    const saved = isBookmarked(entry.key);
    const route = entry.workspace && safeSpaceAppURL(entry.app) !== "#" ? safeSpaceAppURL(entry.app) : "";
    const displayAddress = entry.socket || entry.address || "Address pending";
    const path = entry.socket ? pathAndQuery(entry.item?.frontend || entry.app) : "";
    const busy = entry.workspace && state.safeSpaceBusy.has(`${entry.workspace.id}\u001f${entry.app.serviceID}`);
    const menuAttributes = entry.workspace
      ? `data-action="edit-container-bookmark" data-safe-space-id="${escapeHTML(entry.workspace.id)}" data-service-id="${escapeHTML(entry.app.serviceID)}" data-frontend-key="${escapeHTML(containerFrontendKey(entry.app))}"`
      : `data-action="edit-bookmark" data-app-key="${escapeHTML(entry.item.identity)}"`;
    return `<article class="directory-row" aria-busy="${Boolean(busy)}">
      <a class="directory-name" href="${escapeHTML(target)}" ${launchAttributes} title="Open ${escapeHTML(entry.name)}"><span class="address-status${entry.running ? " is-running" : ""}" aria-label="${entry.running ? "Running" : "Not running"}"></span>${escapeHTML(entry.name)}</a>
      <div class="directory-address">
        <a class="address-link" href="${escapeHTML(target)}" ${launchAttributes} aria-label="Open endpoint for ${escapeHTML(entry.name)}" title="${escapeHTML(entry.address)}"><code>${escapeHTML(displayAddress)}${path && path !== "/" ? `<span class="address-path"> ${escapeHTML(path)}</span>` : ""}</code></a>
        ${route ? `<details class="connection-details"><summary>↳ host</summary><a class="address-link" href="${escapeHTML(target)}" ${launchAttributes} title="Open host connection URL"><code>${escapeHTML(route)}</code></a></details>` : ""}
      </div>
      <div class="directory-actions"><button class="bookmark-toggle${saved ? " is-saved" : ""}" type="button" data-action="toggle-bookmark" data-bookmark-key="${escapeHTML(entry.key)}" aria-pressed="${saved}" aria-label="${saved ? "Remove" : "Add"} ${escapeHTML(entry.name)} ${saved ? "from" : "to"} Bookmarks" title="${saved ? "Remove bookmark" : "Add bookmark"}">${saved ? "★" : "☆"}</button><button class="endpoint-menu" type="button" ${menuAttributes} aria-label="Actions for ${escapeHTML(entry.name)}" aria-haspopup="dialog">•••</button></div>
    </article>`;
  }

  function matchesSearch(entry, query) {
    const folder = entry.item?.frontend.list || state.containerFolders[entry.key] || "";
    return `${entry.name} ${entry.address} ${entry.socket} ${entry.kind} ${entry.groupName} ${entry.serviceID} ${entry.item?.bookmarkContext || ""} ${folder}`.toLocaleLowerCase().includes(query);
  }

  function renderDirectory() {
    const all = registeredAddresses();
    const query = state.query.trim().toLocaleLowerCase();
    const filtered = all.filter(entry => matchesSearch(entry, query));
    const groups = [{ id: "host", name: "This server" }, ...state.safeSpaces.map(workspace => ({ id: workspace.id, name: workspace.name || "Container", workspace }))];
    let markup = groups.map(group => {
      const entries = filtered.filter(entry => entry.groupID === group.id);
      if (!entries.length && query && !`${group.name} ${group.workspace ? safeSpaceRuntimeDescription(group.workspace) : ""}`.toLocaleLowerCase().includes(query)) return "";
      const categories = ["Ports", "Root sockets", "User sockets", "Custom sockets", "Other endpoints"].map(kind => {
        const rows = entries.filter(entry => entry.kind === kind);
        return rows.length ? `<section class="address-category"><h4>${kind}<span>${rows.length}</span></h4>${rows.map(directoryRowHTML).join("")}</section>` : "";
      }).join("");
      const note = group.workspace ? `Container · ${safeSpaceState(group.workspace)}${group.workspace.recipe?.needsRebuild ? " · rebuild needed" : ""}` : "Host";
      const loading = group.workspace ? state.safeSpacesLoading : state.loading;
      const workspace = group.workspace;
      const busy = workspace && containerPreparing(workspace);
      const controls = workspace ? `${!safeSpaceIsRunning(workspace) ? `<button class="directory-control" type="button" data-action="start-safe-space" data-safe-space-id="${escapeHTML(workspace.id)}" ${busy ? "disabled" : ""}>${busy ? "Starting…" : "Start"}</button>` : ""}<button class="endpoint-menu" type="button" data-action="edit-container" data-safe-space-id="${escapeHTML(workspace.id)}" aria-label="Actions for ${escapeHTML(group.name)}" aria-haspopup="dialog">•••</button>` : "";
      return `<section class="directory-group"><header><div><h3>${escapeHTML(group.name)}</h3><p>${escapeHTML(note)}</p></div><div class="directory-group-actions"><span class="directory-total">${entries.length} ${entries.length === 1 ? "endpoint" : "endpoints"}</span>${controls}</div></header>${workspace?.buildProgress ? `<details class="container-build-progress" ${workspace.buildProgress.phase === "failed" ? "open" : ""}><summary>${escapeHTML(workspace.buildProgress.detail || workspace.buildProgress.phase)}</summary><pre>${escapeHTML(workspace.buildProgress.log || "")}</pre></details>` : ""}${categories || `<p class="directory-empty">${loading ? "Loading addresses…" : "No registered endpoints here yet."}</p>`}</section>`;
    }).join("");
    if (!markup) markup = `<p class="directory-empty">No registered endpoints match your search.</p>`;
    if (state.directoryMarkup !== markup) {
      elements.directory.innerHTML = markup;
      state.directoryMarkup = markup;
    }
  }

  function allBookmarkItems() {
    const containers = state.safeSpaces.flatMap(workspace => (workspace.apps || []).map(app => ({
      identity: containerBookmarkKey(workspace, app), workspace, app,
      displayName: state.endpointNames[containerBookmarkKey(workspace, app)] || app.displayName || app.serviceID,
      subtitle: workspace.name || "Container",
      backend: { serviceID: app.serviceID },
      frontend: { list: state.containerFolders[containerBookmarkKey(workspace, app)] || "" }
    })));
    const ranks = new Map(state.bookmarkOrder.map((key, index) => [key, index]));
    return [...launcherItems(true), ...containers].sort((a, b) => {
      const rank = (ranks.get(hostBookmarkKey(a)) ?? Infinity) - (ranks.get(hostBookmarkKey(b)) ?? Infinity);
      return rank || a.displayName.localeCompare(b.displayName, undefined, { sensitivity: "base" });
    });
  }

  function render() {
    renderSafeSpaces();
  }

  function renderBookmarks() {
    if (state.appDrag || state.folderResize) return;
    elements.shell.setAttribute("aria-busy", state.loading ? "true" : "false");
    if (state.loading && !state.backends.length) {
      state.bookmarksMarkup = "";
      elements.sections.innerHTML = `<div class="loading-grid">${"<div class=\"skeleton\"></div>".repeat(6)}</div>`;
      elements.empty.hidden = true;
      return;
    }
    const items = allBookmarkItems().filter(item => isBookmarked(hostBookmarkKey(item)));
    const query = state.query.trim().toLocaleLowerCase();
    const matchingKeys = new Set(registeredAddresses().filter(entry => matchesSearch(entry, query)).map(entry => entry.key));
    const visible = query ? items.filter(item => matchingKeys.has(hostBookmarkKey(item))) : items;
    const bookmarkCount = items.length;
    elements.summary.textContent = bookmarkCount === 1 ? "1 bookmark." : `${bookmarkCount} bookmarks.`;
    elements.empty.hidden = visible.length > 0 || state.loading || state.safeSpacesLoading;
    if (!visible.length) {
      elements.sections.innerHTML = "";
      state.bookmarksMarkup = "";
      if (query) {
        elements.empty.querySelector("h2").textContent = "No matching bookmarks";
        elements.empty.querySelector("p").textContent = "Try a different name or identifier.";
        elements.empty.querySelector("button").hidden = true;
      } else {
        elements.empty.querySelector("h2").textContent = "Your bookmarks";
        elements.empty.querySelector("p").textContent = "Bookmark an endpoint to get started.";
        elements.empty.querySelector("button").hidden = true;
      }
      if (query) return;
    }

    const iconItems = visible.filter(item => !item.frontend.list?.trim());
    const listSections = new Map();
    visible.filter(item => item.frontend.list?.trim()).forEach(item => {
      const name = item.frontend.list.trim();
      if (!listSections.has(name)) listSections.set(name, []);
      listSections.get(name).push(item);
    });
    const orderedLists = [...listSections.entries()].sort(([left], [right]) => left.localeCompare(right, undefined, { sensitivity: "base" }));
    const addTile = query ? "" : `<article class="launcher-tile add-app-tile">
      <button class="launcher-link" type="button" data-action="browse-urls" aria-label="Add bookmark"><span class="add-app-icon" aria-hidden="true"><span></span></span></button>
      <span class="launcher-name">Add bookmark</span>
    </article>`;
    elements.sections.classList.toggle("single-column", orderedLists.length === 0);
    const markup = `
      <section class="launcher-column" data-drop-list="" aria-label="Shortcuts">
        <div class="launcher-grid">${iconItems.map(renderLauncherTile).join("")}${addTile}</div>
      </section>
      ${orderedLists.length ? `<section class="list-column" aria-label="Folders">${orderedLists.map(renderListGroup).join("")}</section>` : ""}`;
    if (state.bookmarksMarkup !== markup) {
      elements.sections.innerHTML = markup;
      state.bookmarksMarkup = markup;
      layoutBookmarks();
    }
  }

  function layoutBookmarks() {
    const grid = elements.sections.closest(".dashboard");
    if (!grid || !grid.clientWidth) return;
    const columns = window.innerWidth <= 680 ? 3 : Math.max(3, Math.floor((grid.clientWidth + 12) / 102));
    grid.style.gridTemplateColumns = `repeat(${columns}, minmax(0, 1fr))`;
    const folders = [...elements.sections.querySelectorAll(".list-group")];
    const tiles = [...elements.sections.querySelectorAll(".launcher-tile")];
    const occupied = new Set();
    const rowHeight = Math.ceil(Math.max(112, ...tiles.map(tile => tile.getBoundingClientRect().height)));
    const rowGap = 20;
    grid.style.gridAutoRows = `${rowHeight}px`;
    const span = element => Math.max(1, Math.ceil((element.getBoundingClientRect().height + rowGap) / (rowHeight + rowGap)));
    tiles.forEach(tile => { tile.style.gridRow = "span 1"; });
    folders.forEach(folder => {
      const position = (state.appDrag?.folder === folder.dataset.folder && state.appDrag.folderPosition) || state.folderPositions[folder.dataset.folder] || "top-right";
      const gap = parseFloat(getComputedStyle(grid).columnGap) || 0;
      const columnWidth = (grid.clientWidth - gap * (columns - 1)) / columns;
      const minimum = state.folderResize?.name === folder.dataset.folder ? state.folderResize.width : state.folderWidths[folder.dataset.folder];
      const width = Math.min(columns, minimum ? Math.max(1, Math.ceil((minimum + gap) / (columnWidth + gap))) : 3);
      folder.querySelectorAll("[data-folder-resize]").forEach(handle => {
        handle.setAttribute("aria-valuenow", String(Math.round(minimum || columnWidth * width + gap * (width - 1))));
        handle.setAttribute("aria-valuetext", `${handle.getAttribute("aria-valuenow")} pixels minimum`);
      });
      const column = position === "top-left" ? 1 : position === "middle" ? Math.floor((columns - width) / 2) + 1 : columns - width + 1;
      folder.style.gridColumn = `${column} / span ${width}`;
      const height = span(folder);
      let row = position === "middle" && columns > width && tiles.length > columns
        ? Math.max(1, Math.floor(tiles.length / columns / 2) + 1) : 1;
      const cells = start => Array.from({ length: height }, (_, y) =>
        Array.from({ length: width }, (_, x) => `${start + y}:${column + x}`)).flat();
      while (cells(row).some(cell => occupied.has(cell))) row++;
      cells(row).forEach(cell => occupied.add(cell));
      folder.style.gridRow = `${row} / span ${height}`;
    });
  }

  function safeSpaceState(workspace) {
    return String(workspace.state || "stopped").toLocaleLowerCase();
  }

  function safeSpaceIsRunning(workspace) {
    return safeSpaceState(workspace) === "running";
  }

  function safeSpaceRuntimeDescription(workspace) {
    const parts = [workspace.runtime?.providerName || workspace.runtime?.isolationName];
    const system = [workspace.runtime?.operatingSystemName, workspace.runtime?.operatingSystemVersion]
      .filter(Boolean).join(" ");
    if (system) parts.push(system);
    if (workspace.runtime?.architecture) parts.push(workspace.runtime.architecture);
    const description = parts.filter(Boolean).join(" · ") || "Container";
    return workspace.ownsContainer === false || workspace.managementKind === "attached"
      ? `Attached · ${description}`
      : description;
  }

  function safeSpacePathAndQuery(app) {
    return pathAndQuery({ url: app.url || "", socketPath: app.socketPath || "" });
  }

  function safeSpaceAppURL(app) {
    const path = safeSpacePathAndQuery(app);
    if (Number(app.publishedPort) > 0) return `http://127.0.0.1:${Number(app.publishedPort)}${path}`;
    const socket = String(app.externalSocketPath || "").trim();
    return socket ? `http+unix://${encodeURIComponent(socket)}${path}` : "#";
  }

  function safeSpaceAppNavigationURL(workspace, app) {
    const target = safeSpaceAppURL(app);
    if (target === "#") return target;
    if (window.outerLoopPageIconObservationSupported === true && !app.iconData && app.iconObservationToken) {
      const callback = new URL("/api/icon-observation", window.location.href);
      callback.search = new URLSearchParams({ token: app.iconObservationToken }).toString();
      return `outerloop://observe-page-icon?${new URLSearchParams({ url: target, callback: callback.href })}`;
    }
    if (window.outerLoopFriendlyNavigationSupported !== true) return target;
    const name = app.displayName || app.serviceID || "App";
    const user = String(app.socketPath || "").startsWith("/run/user/0/") ? "root" : "workspace";
    const parameters = [
      `url=${encodeURIComponent(target)}`,
      `display=${encodeURIComponent(`${workspace.name || "Container"} / ${user} / ${name}`)}`
    ].join("&");
    return `outerloop://navigate?${parameters}`;
  }

  function safeSpaceAppIsReady(workspace, app) {
    return safeSpaceIsRunning(workspace) && app.isRunning === true && safeSpaceAppURL(app) !== "#";
  }

  function renderSafeSpaces() {
    renderDirectory();
    renderBookmarks();
  }

  function slug(value) {
    return String(value).toLocaleLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "apps";
  }

  function renderLauncherTile(item) {
    if (item.workspace) return renderContainerBookmark(item);
    const readyURL = navigationURL(item.frontend);
    return `<article class="launcher-tile" data-drag-app-key="${escapeHTML(item.identity)}">
      <a class="launcher-link bookmark-link" href="${escapeHTML(readyURL)}" data-action="launch" data-app-key="${escapeHTML(item.identity)}" aria-label="Open ${escapeHTML(item.displayName)}${item.bookmarkContext ? ` ${escapeHTML(item.bookmarkContext)}` : ""}" aria-keyshortcuts="Shift+F10"><span class="launcher-icon-row">${launcherIconHTML(item)}</span>
      <h2 class="launcher-name">${escapeHTML(item.displayName)}${item.bookmarkContext ? `<span class="bookmark-context">${escapeHTML(item.bookmarkContext)}</span>` : ""}</h2></a>${editButtonHTML(item)}
    </article>`;
  }

  function renderContainerBookmark(item, row = false) {
    const { workspace, app } = item;
    const attributes = `data-app-key="${escapeHTML(item.identity)}" data-safe-space-id="${escapeHTML(workspace.id)}" data-service-id="${escapeHTML(app.serviceID)}" data-frontend-key="${escapeHTML(containerFrontendKey(app))}"`;
    const name = `<span class="${row ? "container-bookmark-name" : "launcher-name"}">${escapeHTML(item.displayName)}<span class="bookmark-context">${escapeHTML(item.subtitle)}</span></span>`;
    return `<article class="${row ? "list-row" : "launcher-tile"}" data-drag-app-key="${escapeHTML(item.identity)}">
      <a class="${row ? "list-link" : "launcher-link bookmark-link"}" href="${escapeHTML(safeSpaceAppNavigationURL(workspace, app))}" data-action="launch-safe-space-app" ${attributes} aria-label="Open ${escapeHTML(item.displayName)} in ${escapeHTML(item.subtitle)}">${row ? listIconHTML(item) : `<span class="launcher-icon-row">${launcherIconHTML(item)}</span>`}${name}</a>
      ${editButtonHTML(item)}
    </article>`;
  }

  function renderListGroup([name, items]) {
    return `<section class="list-group" data-folder="${escapeHTML(name)}" aria-label="${escapeHTML(name)}">
      ${["left", "right"].map(edge => `<span class="folder-resize folder-resize-${edge}" data-folder-resize="${edge}" role="separator" tabindex="0" aria-orientation="vertical" aria-label="Resize ${escapeHTML(name)}" aria-valuemin="160"></span>`).join("")}
      <header class="folder-heading" data-folder-handle="${escapeHTML(name)}" data-drop-list="${escapeHTML(name)}"><span>${escapeHTML(name)}</span><span class="folder-count">${items.length}</span></header>
      <div class="list-widget" data-drop-list="${escapeHTML(name)}">${items.map(renderListRow).join("")}</div>
    </section>`;
  }

  function saveFolderWidth(name, width) {
    const widths = { ...state.folderWidths, [name]: Math.max(160, Math.round(width)) };
    try { localStorage.setItem("outer-shell.folder-widths.v1", JSON.stringify(widths)); }
    catch (error) { toast("Could not save folder width.", true); return false; }
    state.folderWidths = widths;
    return true;
  }

  function beginFolderResize(event) {
    const handle = event.target.closest("[data-folder-resize]");
    if (!handle || !event.isPrimary || event.button !== 0 || elements.dialogLayer.childElementCount) return;
    event.preventDefault();
    cancelAppDrag();
    cancelLongPress();
    const folder = handle.closest("[data-folder]");
    const width = folder.getBoundingClientRect().width;
    state.folderResize = { name: folder.dataset.folder, handle, pointerID: event.pointerId, startX: event.clientX, startWidth: width, width, direction: handle.dataset.folderResize === "left" ? -1 : 1 };
    try { handle.setPointerCapture(event.pointerId); } catch (_) {}
    document.body.classList.add("folder-resizing");
  }

  function moveFolderResize(event) {
    const resize = state.folderResize;
    if (!resize || resize.pointerID !== event.pointerId) return;
    if (event.cancelable) event.preventDefault();
    resize.width = Math.max(160, Math.round(resize.startWidth + (event.clientX - resize.startX) * resize.direction));
    layoutBookmarks();
  }

  function endFolderResize(event, save) {
    const resize = state.folderResize;
    if (!resize || resize.pointerID !== event.pointerId) return;
    if (save) { moveFolderResize(event); saveFolderWidth(resize.name, resize.width); }
    state.folderResize = null;
    document.body.classList.remove("folder-resizing");
    state.suppressLaunchUntil = Date.now() + 300;
    layoutBookmarks();
    renderBookmarks();
  }

  function renameEndpointMenuHTML(item) {
    return `<button class="context-menu-item" type="button" data-action="rename-endpoint" data-app-key="${escapeHTML(item.identity)}" role="menuitem">${contextMenuGlyph("✎")}<span>Rename endpoint…</span></button>`;
  }

  function openRenameEndpoint(item) {
    const key = hostBookmarkKey(item);
    const dialog = openDialog(`${containerDialogHeader("Rename endpoint")}<form><div class="dialog-body"><label class="field">Name<input name="name" required value="${escapeHTML(item.displayName)}" autocomplete="off"></label><p class="field-note">Saved in this browser. Used in bookmarks and All endpoints.</p><p class="container-message" role="status"></p></div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Cancel</button><button type="submit" class="primary-button">Rename</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.elements.name.select();
    form.addEventListener("submit", event => {
      event.preventDefault();
      const name = form.elements.name.value.trim();
      const message = form.querySelector(".container-message");
      if (!name) { message.textContent = "Enter a name."; return; }
      const names = { ...state.endpointNames, [key]: name };
      try { localStorage.setItem("outer-shell.endpoint-names.v1", JSON.stringify(names)); }
      catch (error) { message.textContent = "Could not save the name. Please try again."; return; }
      state.endpointNames = names;
      closeDialog();
      render();
    });
  }

  function openFolderMenu(name, point = null) {
    showContextMenu(`<button class="context-menu-item" type="button" data-action="rename-folder" data-folder-name="${escapeHTML(name)}" role="menuitem">${contextMenuGlyph("✎")}<span>Rename folder…</span></button>`, point, `${name} actions`);
  }

  function openRenameFolder(name) {
    const dialog = openDialog(`${containerDialogHeader("Rename folder")}<form><div class="dialog-body"><label class="field">Folder name<input name="folder" required value="${escapeHTML(name)}" autocomplete="off"></label><p class="container-message" role="status"></p></div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Cancel</button><button type="submit" class="primary-button">Rename</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.elements.folder.select();
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const next = form.elements.folder.value.trim();
      const message = form.querySelector(".container-message");
      if (!next) { message.textContent = "Enter a folder name."; return; }
      if (next === name) { closeDialog(); return; }
      const items = allBookmarkItems();
      if (items.some(item => item.frontend.list?.trim() === next)) { message.textContent = "A folder with that name already exists."; return; }
      const button = form.querySelector('[type="submit"]');
      button.disabled = true;
      form.elements.folder.disabled = true;
      let failed = false;
      for (const item of items.filter(item => item.frontend.list?.trim() === name)) {
        if (!await setFrontendList(item, next)) { failed = true; break; }
      }
      const positions = { ...state.folderPositions, [next]: state.folderPositions[name] || "top-right" };
      if (!failed) delete positions[name];
      try { localStorage.setItem("outer-shell.folder-positions.v1", JSON.stringify(positions)); state.folderPositions = positions; }
      catch (error) { toast("Could not save folder position.", true); }
      if (Object.hasOwn(state.folderWidths, name)) {
        const widths = { ...state.folderWidths, [next]: state.folderWidths[name] };
        if (!failed) delete widths[name];
        try { localStorage.setItem("outer-shell.folder-widths.v1", JSON.stringify(widths)); state.folderWidths = widths; }
        catch (error) { toast("Could not save folder width.", true); }
      }
      render();
      if (!failed) closeDialog();
      else { message.textContent = "Some bookmarks could not be moved. Both folders have been kept; move the remaining bookmarks when the connection is restored."; button.disabled = false; form.elements.folder.disabled = false; }
    });
  }

  function renderListRow(item) {
    if (item.workspace) return renderContainerBookmark(item, true);
    const readyURL = navigationURL(item.frontend);
    return `<article class="list-row" data-drag-app-key="${escapeHTML(item.identity)}">
      <a class="list-link" href="${escapeHTML(readyURL)}" data-action="launch" data-app-key="${escapeHTML(item.identity)}" aria-label="Open ${escapeHTML(item.displayName)}${item.bookmarkContext ? ` ${escapeHTML(item.bookmarkContext)}` : ""}" aria-keyshortcuts="Shift+F10">${listIconHTML(item)}<h3 class="list-name">${escapeHTML(item.displayName)}${item.bookmarkContext ? `<span class="bookmark-context">${escapeHTML(item.bookmarkContext)}</span>` : ""}</h3></a>
      ${editButtonHTML(item)}
    </article>`;
  }

  async function requestBuffer(url, options = {}, acceptErrors = false) {
    const response = await fetch(url, { cache: "no-store", ...options });
    const buffer = await response.arrayBuffer();
    if (!response.ok && !acceptErrors) {
      const text = decoder.decode(buffer).trim();
      throw new Error(text || `Outer Shell returned HTTP ${response.status}.`);
    }
    return { response, buffer };
  }

  function makeRequestID() {
    if (typeof globalThis.crypto?.randomUUID === "function") {
      return globalThis.crypto.randomUUID();
    }

    const bytes = new Uint8Array(16);
    if (typeof globalThis.crypto?.getRandomValues === "function") {
      globalThis.crypto.getRandomValues(bytes);
    } else {
      for (let index = 0; index < bytes.length; index += 1) {
        bytes[index] = Math.floor(Math.random() * 256);
      }
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const hex = Array.from(bytes, value => value.toString(16).padStart(2, "0"));
    return `${hex[0]}${hex[1]}${hex[2]}${hex[3]}-${hex[4]}${hex[5]}-${hex[6]}${hex[7]}-${hex[8]}${hex[9]}-${hex[10]}${hex[11]}${hex[12]}${hex[13]}${hex[14]}${hex[15]}`;
  }

  async function safeSpaceRequest(operation, values = {}, signal) {
    const requestID = makeRequestID();
    const response = await fetch("/api/safe-spaces", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      signal,
      body: JSON.stringify({ requestID, operation, ...values })
    });
    const result = await response.json();
    if (!response.ok || result.error) throw new Error(result.error || `Outer Shell returned HTTP ${response.status}.`);
    if (!signal?.aborted && Array.isArray(result.providers)) state.providers = result.providers;
    return result;
  }

  async function refreshSafeSpaces({ quiet = false } = {}) {
    if (state.stopped || state.suspended || state.safeSpacesRefreshing) return;
    const controller = new AbortController();
    state.refreshAbort.safeSpacesError = controller;
    state.safeSpacesRefreshing = true;
    if (!quiet) {
      state.safeSpacesLoading = true;
      renderSafeSpaces();
    }
    try {
      const result = await safeSpaceRequest("list", {}, controller.signal);
      if (state.refreshAbort.safeSpacesError !== controller) return;
      state.safeSpaces = Array.isArray(result.workspaces) ? result.workspaces : [];
      clearRefreshFailure("safeSpacesError");
      state.safeSpacesError = "";
    } catch (error) {
      if (state.refreshAbort.safeSpacesError === controller) reportRefreshFailure("safeSpacesError", error, quiet);
    } finally {
      if (state.refreshAbort.safeSpacesError !== controller) return;
      delete state.refreshAbort.safeSpacesError;
      state.safeSpacesRefreshing = false;
      state.safeSpacesLoading = false;
      if (!state.stopped) { updateStatus(); renderSafeSpaces(); }
    }
  }

  function findSafeSpace(id) {
    return state.safeSpaces.find(workspace => workspace.id === id);
  }

  async function waitForSafeSpaceApp(workspaceID, serviceID, frontendKey) {
    for (let attempt = 0; attempt < 40; attempt += 1) {
      await refreshSafeSpaces({ quiet: true });
      const workspace = findSafeSpace(workspaceID);
      const app = findSafeSpaceApp(workspace, serviceID, frontendKey);
      if (workspace && app && safeSpaceAppIsReady(workspace, app)) return { workspace, app };
      await delay(250);
    }
    throw new Error("The app started, but its address is not ready yet.");
  }

  async function startSafeSpace(workspaceID) {
    state.safeSpaceBusy.add(workspaceID);
    renderSafeSpaces();
    try {
      const result = await safeSpaceRequest("start", { workspaceID });
      state.safeSpaces = Array.isArray(result.workspaces) ? result.workspaces : state.safeSpaces;
    } finally {
      state.safeSpaceBusy.delete(workspaceID);
      renderSafeSpaces();
    }
  }

  async function launchSafeSpaceApp(workspaceID, serviceID, frontendKey) {
    let workspace = findSafeSpace(workspaceID);
    let app = findSafeSpaceApp(workspace, serviceID, frontendKey);
    if (!workspace || !app) throw new Error("The selected container app no longer exists.");
    if (safeSpaceAppIsReady(workspace, app)) {
      window.location.assign(safeSpaceAppNavigationURL(workspace, app));
      return;
    }
    const key = `${workspaceID}\u001f${serviceID}`;
    state.safeSpaceBusy.add(key);
    renderSafeSpaces();
    try {
      if (!safeSpaceIsRunning(workspace)) {
        const result = await safeSpaceRequest("start", { workspaceID });
        state.safeSpaces = Array.isArray(result.workspaces) ? result.workspaces : state.safeSpaces;
        workspace = findSafeSpace(workspaceID) || workspace;
        app = findSafeSpaceApp(workspace, serviceID, frontendKey) || app;
      }
      if (app.isRunning !== true) await safeSpaceRequest("startApp", { workspaceID, serviceID });
      const ready = await waitForSafeSpaceApp(workspaceID, serviceID, frontendKey);
      window.location.assign(safeSpaceAppNavigationURL(ready.workspace, ready.app));
    } finally {
      state.safeSpaceBusy.delete(key);
      renderSafeSpaces();
    }
  }

  async function refreshBackends({ quiet = false } = {}) {
    if (state.stopped || state.suspended || state.refreshAbort.backendError) return;
    const controller = new AbortController();
    state.refreshAbort.backendError = controller;
    if (!quiet) {
      state.loading = true;
      render();
    }
    try {
      const { buffer } = await requestBuffer("/api/backends", { signal: controller.signal });
      if (state.refreshAbort.backendError !== controller) return;
      const result = decodeBackends(buffer);
      state.backends = result.backends;
      if (!result.error) initializeBookmarks();
      clearRefreshFailure("backendError");
      state.backendError = result.error;
      updateStatus();
    } catch (error) {
      if (state.refreshAbort.backendError === controller) reportRefreshFailure("backendError", error, quiet);
    } finally {
      if (state.refreshAbort.backendError !== controller) return;
      delete state.refreshAbort.backendError;
      state.loading = false;
      if (!state.stopped) render();
    }
  }

  function clearRefreshFailure(key) {
    window.clearTimeout(state.refreshErrorTimers[key]);
    delete state.refreshErrorTimers[key];
  }

  function reportRefreshFailure(key, error) {
    if (state.stopped || state.suspended || document.hidden || error.name === "AbortError" || state.refreshErrorTimers[key]) return;
    state.refreshErrorTimers[key] = window.setTimeout(() => {
      delete state.refreshErrorTimers[key];
      if (state.stopped || state.suspended || document.hidden) return;
      state[key] = "Unable to refresh right now. Reconnecting…";
      updateStatus();
    }, 10000);
  }

  function suspendRefreshes() {
    state.suspended = true;
    state.eventAbort?.abort();
    for (const key of ["backendError", "safeSpacesError"]) {
      state.refreshAbort[key]?.abort();
      delete state.refreshAbort[key];
      clearRefreshFailure(key);
      state[key] = "";
    }
    state.safeSpacesRefreshing = false;
    updateStatus();
  }

  function resumeRefreshes() {
    if (document.hidden || !state.suspended || state.stopped) return;
    state.suspended = false;
    refreshBackends({ quiet: true });
    refreshSafeSpaces({ quiet: true });
  }

  function showStatus(message = "") {
    elements.status.textContent = message;
    elements.status.hidden = !message;
  }

  function updateStatus() {
    if (!state.stopped) showStatus([...new Set([state.backendError, state.safeSpacesError].filter(Boolean))].join("\n"));
  }

  function toast(message, isError = false) {
    if (!message || state.stopped) return;
    const node = document.createElement("div");
    node.className = `toast${isError ? " error" : ""}`;
    node.textContent = message;
    elements.toasts.append(node);
    window.setTimeout(() => node.remove(), 4200);
  }

  function findItem(identity) {
    return allBookmarkItems().find(item => item.identity === identity);
  }

  function itemIdentityFromTarget(target) {
    if (!(target instanceof Element)) return "";
    return target.closest("[data-app-key]")?.dataset.appKey
      || target.closest("[data-drag-app-key]")?.dataset.dragAppKey || "";
  }

  function cancelLongPress() {
    if (!state.longPress) return;
    window.clearTimeout(state.longPress.timer);
    state.longPress = null;
  }

  function clearTextSelection() {
    window.getSelection()?.removeAllRanges();
  }

  function beginLongPress(event) {
    if (event.pointerType !== "touch" || !event.isPrimary || elements.dialogLayer.childElementCount) return;
    const identity = itemIdentityFromTarget(event.target);
    if (event.target.closest("[data-folder-resize]")) return;
    const folder = event.target.closest("[data-folder]")?.dataset.folder;
    if (!identity && folder === undefined) return;
    cancelLongPress();
    const press = {
      identity,
      pointerID: event.pointerId,
      x: event.clientX,
      y: event.clientY,
      timer: 0
    };
    press.timer = window.setTimeout(() => {
      if (state.longPress !== press) return;
      state.longPress = null;
      cancelAppDrag();
      clearPressFeedback();
      state.suppressLaunchUntil = Date.now() + 900;
      navigator.vibrate?.(8);
      if (identity) openAppMenu(identity);
      else openFolderMenu(folder);
      clearTextSelection();
      window.requestAnimationFrame(clearTextSelection);
    }, 520);
    state.longPress = press;
  }

  function moveLongPress(event) {
    const press = state.longPress;
    if (!press || event.pointerId !== press.pointerID) return;
    if (Math.hypot(event.clientX - press.x, event.clientY - press.y) > 10) cancelLongPress();
  }

  function beginPressFeedback(event) {
    if (!event.isPrimary || event.button !== 0 || event.ctrlKey || !(event.target instanceof Element)) return;
    const target = event.target.closest(".launcher-link, .list-link, .running-badge-button");
    if (!target) return;
    clearPressFeedback();
    target.classList.add("pressed");
    state.pressFeedback = { target, pointerID: event.pointerId, x: event.clientX, y: event.clientY };
  }

  function movePressFeedback(event) {
    const feedback = state.pressFeedback;
    if (!feedback || event.pointerId !== feedback.pointerID) return;
    if (Math.hypot(event.clientX - feedback.x, event.clientY - feedback.y) > 10) clearPressFeedback();
  }

  function clearPressFeedback() {
    state.pressFeedback?.target.classList.remove("pressed");
    state.pressFeedback = null;
  }

  function beginAppDrag(event) {
    if (!event.isPrimary || event.button !== 0 || event.ctrlKey || elements.dialogLayer.childElementCount || !(event.target instanceof Element)) return;
    const source = event.target.closest(".launcher-link[data-app-key], .list-link[data-app-key], [data-folder-handle]");
    if (!source || source.closest(".context-menu")) return;
    if (event.target.closest("button")) return;
    const folder = source.dataset.folderHandle;
    const item = folder === undefined ? findItem(source.dataset.appKey) : { identity: folder, displayName: folder, frontend: {} };
    if (!item) return;
    cancelAppDrag();
    const drag = {
      identity: item.identity,
      folder,
      item,
      source,
      sourceContainer: source.closest("[data-drag-app-key], [data-folder]"),
      pointerID: event.pointerId,
      pointerType: event.pointerType,
      startX: event.clientX,
      startY: event.clientY,
      x: event.clientX,
      y: event.clientY,
      currentList: String(item.frontend.list || "").trim(),
      currentDropList: null,
      dropTarget: null,
      preview: null,
      isDragging: false,
      touchArmed: event.pointerType !== "touch" || state.editing,
      armTimer: 0,
      scrollVelocity: 0,
      scrollFrame: 0
    };
    if (event.pointerType === "touch" && !state.editing) {
      drag.armTimer = window.setTimeout(() => {
        if (state.appDrag === drag) drag.touchArmed = true;
      }, 260);
    }
    state.appDrag = drag;
  }

  function startAppDrag(drag, event) {
    if (drag.isDragging) return;
    drag.isDragging = true;
    window.clearTimeout(drag.armTimer);
    drag.armTimer = 0;
    cancelLongPress();
    clearPressFeedback();
    clearTextSelection();
    state.suppressLaunchUntil = Date.now() + 900;
    drag.source.setAttribute("aria-grabbed", "true");
    drag.sourceContainer?.classList.add("app-drag-source");
    document.body.classList.add("app-dragging");
    const preview = document.createElement("div");
    preview.className = "app-drag-preview";
    preview.setAttribute("aria-hidden", "true");
    preview.innerHTML = `${drag.folder !== undefined ? "▤" : drag.item.workspace ? safeSpaceAppIconHTML(drag.item.app) : launcherIconHTML(drag.item)}<span>${escapeHTML(drag.item.displayName)}</span>`;
    document.body.append(preview);
    drag.preview = preview;
    try { drag.source.setPointerCapture(event.pointerId); } catch (_) {}
    updateAppDrag(drag, event.clientX, event.clientY);
  }

  function dropTargetAt(x, y) {
    const node = document.elementFromPoint(x, y);
    const target = node instanceof Element ? node.closest("[data-drop-list]") : null;
    if (!target) return null;
    const bookmark = node.closest("[data-drag-app-key]");
    const bounds = bookmark?.getBoundingClientRect();
    const after = bounds ? (bookmark.classList.contains("list-row") ? y > bounds.top + bounds.height / 2 : x > bounds.left + bounds.width / 2) : true;
    return { element: bookmark || target, list: target.dataset.dropList || "", identity: bookmark?.dataset.dragAppKey, after };
  }

  function updateAppDropTarget(drag) {
    if (drag.folder !== undefined) {
      const bounds = elements.sections.closest(".dashboard").getBoundingClientRect();
      const fraction = (drag.x - bounds.left) / bounds.width;
      drag.folderPosition = fraction < 1 / 3 ? "top-left" : fraction > 2 / 3 ? "top-right" : "middle";
      layoutBookmarks();
      return;
    }
    const target = dropTargetAt(drag.x, drag.y);
    drag.dropTarget?.classList.remove("app-drop-target");
    drag.dropTarget?.removeAttribute("data-drop-position");
    drag.dropTarget = null;
    drag.currentDropList = target?.list ?? null;
    drag.targetIdentity = target?.identity || null;
    drag.dropAfter = target?.after ?? true;
    if (!target || target.identity === drag.identity) return;
    drag.dropTarget = target.element;
    if (target.identity) target.element.dataset.dropPosition = target.after ? "after" : "before";
    else target.element.classList.add("app-drop-target");
  }

  function stepAppDragScroll() {
    const drag = state.appDrag;
    if (!drag?.isDragging || !drag.scrollVelocity) return;
    window.scrollBy(0, drag.scrollVelocity);
    updateAppDropTarget(drag);
    drag.scrollFrame = window.requestAnimationFrame(stepAppDragScroll);
  }

  function updateAppDragScroll(drag) {
    const edge = Math.min(84, Math.max(54, window.innerHeight * 0.12));
    let velocity = 0;
    if (drag.y < edge) velocity = -Math.max(2, (edge - drag.y) * 0.12);
    else if (drag.y > window.innerHeight - edge) velocity = Math.max(2, (drag.y - (window.innerHeight - edge)) * 0.12);
    drag.scrollVelocity = Math.max(-13, Math.min(13, velocity));
    if (drag.scrollVelocity && !drag.scrollFrame) drag.scrollFrame = window.requestAnimationFrame(stepAppDragScroll);
    if (!drag.scrollVelocity && drag.scrollFrame) {
      window.cancelAnimationFrame(drag.scrollFrame);
      drag.scrollFrame = 0;
    }
  }

  function updateAppDrag(drag, x, y) {
    drag.x = x;
    drag.y = y;
    if (drag.preview) {
      drag.preview.style.left = `${x}px`;
      drag.preview.style.top = `${y}px`;
    }
    updateAppDropTarget(drag);
    updateAppDragScroll(drag);
  }

  function moveAppDrag(event) {
    const drag = state.appDrag;
    if (!drag || event.pointerId !== drag.pointerID) return;
    const distance = Math.hypot(event.clientX - drag.startX, event.clientY - drag.startY);
    if (!drag.isDragging) {
      if (drag.pointerType === "touch" && !drag.touchArmed) {
        if (distance > 10) cancelAppDrag();
        return;
      }
      if (distance < 4) return;
      startAppDrag(drag, event);
    }
    if (event.cancelable) event.preventDefault();
    updateAppDrag(drag, event.clientX, event.clientY);
  }

  function cancelAppDrag() {
    const drag = state.appDrag;
    if (!drag) return;
    window.clearTimeout(drag.armTimer);
    if (drag.scrollFrame) window.cancelAnimationFrame(drag.scrollFrame);
    drag.dropTarget?.classList.remove("app-drop-target");
    drag.dropTarget?.removeAttribute("data-drop-position");
    drag.sourceContainer?.classList.remove("app-drag-source");
    drag.source.removeAttribute("aria-grabbed");
    drag.preview?.remove();
    document.body.classList.remove("app-dragging");
    state.appDrag = null;
    if (drag.folder !== undefined) layoutBookmarks();
  }

  async function setFrontendList(item, listName) {
    if (item.workspace) {
      const folders = { ...state.containerFolders, [item.identity]: listName };
      try { window.localStorage.setItem("outer-shell.container-folders.v1", JSON.stringify(folders)); }
      catch (error) { toast("Could not save the bookmark folder.", true); return false; }
      state.containerFolders = folders;
      render();
      return true;
    }
    const serviceID = item.backend.serviceID;
    const frontendID = item.frontend.id;
    const frontendURL = item.frontend.url;
    state.backends.forEach(backend => {
      if (backend.serviceID !== serviceID) return;
      backend.frontends.forEach(frontend => {
        const matches = frontendID ? frontend.id === frontendID : frontend.url === frontendURL;
        if (matches) frontend.list = listName;
      });
    });
    render();
    try {
      const result = await control(item.backend, "setFrontendList", {
        frontendID,
        frontendURL,
        list: listName
      });
      if (!result.ok) throw new Error(result.message || "Could not update the app list.");
      return true;
    } catch (error) {
      await refreshBackends({ quiet: true });
      toast(error.message || String(error), true);
      return false;
    }
  }

  function openBookmarkFolderDialog(item) {
    const folders = [...new Set(allBookmarkItems().map(value => value.frontend.list?.trim()).filter(Boolean))]
      .sort((a, b) => a.localeCompare(b, undefined, { sensitivity: "base" }));
    const current = item.frontend.list?.trim() || "";
    const dialog = openDialog(`${containerDialogHeader("Move bookmark to folder")}
      <form class="bookmark-folder-form"><div class="dialog-body">
        <p>${escapeHTML(item.displayName)}</p>
        <label class="field">Existing folder<select name="existing"><option value="">Choose a folder…</option><option value="top">No folder</option>${folders.map((name, index) => `<option value="${index}" ${name === current ? "selected" : ""}>${escapeHTML(name)}</option>`).join("")}</select></label>
        <label class="field">Folder name<input name="folder" value="${escapeHTML(current)}" placeholder="Enter a new folder name" autocomplete="off"></label>
        <p class="field-note">Choose an existing folder or type a new name. Leave the name empty to move the bookmark out of its folder.</p>
        <p class="container-message" role="status"></p>
      </div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="submit">Move</button></footer></form>`);
    const form = dialog.querySelector("form");
    const input = form.elements.folder;
    const select = form.elements.existing;
    select.addEventListener("change", () => {
      if (select.value !== "") input.value = select.value === "top" ? "" : folders[Number(select.value)];
    });
    input.addEventListener("input", () => {
      const index = folders.indexOf(input.value.trim());
      select.value = !input.value.trim() ? "top" : index >= 0 ? String(index) : "";
    });
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const name = input.value.trim();
      const button = form.querySelector('[type="submit"]');
      const message = form.querySelector(".container-message");
      button.disabled = true;
      input.disabled = true;
      select.disabled = true;
      message.textContent = "Moving…";
      const moved = await setFrontendList(item, name);
      if (moved) {
        render();
        if (dialog.isConnected) closeDialog();
      } else {
        message.textContent = "Could not move the bookmark. Your chosen folder is still here; you can try again.";
        button.disabled = false;
        input.disabled = false;
        select.disabled = false;
      }
    });
  }

  function reorderBookmark(item, target, after = false, listName = item.frontend.list?.trim() || "") {
    const key = hostBookmarkKey(item);
    if (target && hostBookmarkKey(target) === key) return true;
    const items = allBookmarkItems().filter(value => isBookmarked(hostBookmarkKey(value)));
    const order = [...new Set([...items.map(hostBookmarkKey), ...state.bookmarkOrder])].filter(value => value !== key);
    let index;
    if (target) index = order.indexOf(hostBookmarkKey(target)) + (after ? 1 : 0);
    else {
      const siblings = items.filter(value => hostBookmarkKey(value) !== key && (value.frontend.list?.trim() || "") === listName);
      const last = siblings.at(-1);
      index = last ? order.indexOf(hostBookmarkKey(last)) + 1 : order.length;
    }
    order.splice(Math.max(0, index), 0, key);
    try { window.localStorage.setItem("outer-shell.bookmark-order.v1", JSON.stringify(order)); }
    catch (error) { toast("Could not save bookmark order.", true); return false; }
    state.bookmarkOrder = order;
    return true;
  }

  function reorderMenuHTML(item) {
    if (!isBookmarked(hostBookmarkKey(item))) return "";
    const siblings = allBookmarkItems().filter(value => isBookmarked(hostBookmarkKey(value)) && (value.frontend.list?.trim() || "") === (item.frontend.list?.trim() || ""));
    const index = siblings.findIndex(value => value.identity === item.identity);
    return [[-1, "Move earlier", "↑"], [1, "Move later", "↓"]].map(([direction, label, icon]) => `<button class="context-menu-item" type="button" data-action="reorder-bookmark" data-app-key="${escapeHTML(item.identity)}" data-direction="${direction}" role="menuitem" ${index + direction < 0 || index + direction >= siblings.length ? "disabled" : ""}>${contextMenuGlyph(icon)}<span>${label}</span></button>`).join("");
  }

  function finishAppDrag(event) {
    const drag = state.appDrag;
    if (!drag || event.pointerId !== drag.pointerID) return;
    const wasDragging = drag.isDragging;
    if (wasDragging) {
      updateAppDropTarget(drag);
      if (event.cancelable) event.preventDefault();
      state.suppressLaunchUntil = Date.now() + 900;
    }
    if (drag.folder !== undefined) {
      if (wasDragging) {
        const positions = { ...state.folderPositions, [drag.folder]: drag.folderPosition };
        try { localStorage.setItem("outer-shell.folder-positions.v1", JSON.stringify(positions)); state.folderPositions = positions; }
        catch (error) { toast("Could not save folder position.", true); }
      }
      cancelAppDrag();
      return;
    }
    const listName = drag.currentDropList;
    const target = drag.targetIdentity ? findItem(drag.targetIdentity) : null;
    cancelAppDrag();
    if (wasDragging && listName !== null && reorderBookmark(drag.item, target, drag.dropAfter, listName)) {
      if (listName !== drag.currentList) setFrontendList(drag.item, listName);
      else render();
    }
  }

  async function launch(identity, scope = "primary") {
    let item = findItem(identity);
    if (!item) return;
    let endpoint = item[scope];
    if (!endpoint) throw new Error(`${item.displayName} is not available for this account.`);
    if (!endpointReady(endpoint)) {
      toast(`Starting ${item.displayName}…`);
      const action = await control(endpoint.backend, "start");
      if (!action.ok) throw new Error(action.message || `Could not start ${item.displayName}.`);
      for (let attempt = 0; attempt < 30; attempt += 1) {
        await delay(500);
        await refreshBackends({ quiet: true });
        item = findItem(identity);
        endpoint = item?.[scope];
        if (endpoint && endpointReady(endpoint)) break;
      }
    }
    if (!endpoint || !endpointReady(endpoint)) throw new Error(`Timed out waiting for ${item?.displayName || "the app"}.`);
    window.location.assign(navigationURL(endpoint.frontend));
  }

  const delay = milliseconds => new Promise(resolve => window.setTimeout(resolve, milliseconds));

  async function control(backend, operation, values = {}, allowPassword = true) {
    const query = new URLSearchParams({ serviceID: backend.serviceID, scope: backend.serviceScope, operation });
    const body = new URLSearchParams(values);
    const { buffer } = await requestBuffer(`/api/control?${query}`, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded; charset=utf-8" },
      body
    }, true);
    let result;
    try {
      result = decodeAction(buffer);
    } catch (_) {
      throw new Error(decoder.decode(buffer).trim() || "Outer Shell could not complete the request.");
    }
    if (!result.ok && result.needsPassword && allowPassword) {
      const password = await requestPassword(backend.displayName, result.message);
      if (password === null) return result;
      return control(backend, operation, { ...values, sudoPassword: password }, false);
    }
    return result;
  }

  function openDialog(bodyHTML, className = "") {
    lockPageScroll();
    elements.dialogLayer.replaceChildren();
    const fragment = elements.dialogTemplate.content.cloneNode(true);
    const backdrop = fragment.querySelector(".dialog-backdrop");
    const dialog = fragment.querySelector(".dialog");
    dialog.className = `dialog ${className}`.trim();
    if (dialog.classList.contains("context-menu")) backdrop.classList.add("context-menu-backdrop");
    dialog.innerHTML = bodyHTML;
    backdrop.addEventListener("click", event => {
      if (event.target === backdrop) closeDialog();
    });
    elements.dialogLayer.append(fragment);
    window.setTimeout(() => {
      if (dialog.classList.contains("context-menu")) {
        dialog.tabIndex = -1;
        dialog.focus({ preventScroll: true });
      } else {
        dialog.querySelector("button, a, input, select, textarea")?.focus();
      }
    }, 0);
    return dialog;
  }

  function positionContextMenu(dialog, point) {
    if (!point || window.matchMedia("(max-width: 680px), (min-width: 681px) and (max-width: 950px) and (orientation: landscape)").matches) return;
    dialog.dataset.anchored = "true";
    dialog.style.visibility = "hidden";
    const bounds = dialog.getBoundingClientRect();
    const inset = 9;
    const left = Math.min(Math.max(point.x, inset), window.innerWidth - bounds.width - inset);
    const top = Math.min(Math.max(point.y, inset), window.innerHeight - bounds.height - inset);
    dialog.style.left = `${left}px`;
    dialog.style.top = `${top}px`;
    dialog.style.visibility = "visible";
  }

  function closeDialog() {
    elements.dialogLayer.replaceChildren();
    unlockPageScroll();
    state.logSelection = null;
    restartEventWatch();
  }

  function lockPageScroll() {
    if (state.pageScrollY !== null) return;
    state.pageScrollY = window.scrollY;
    document.body.style.top = `-${state.pageScrollY}px`;
    document.body.classList.add("dialog-open");
  }

  function unlockPageScroll() {
    if (state.pageScrollY === null) return;
    const scrollY = state.pageScrollY;
    state.pageScrollY = null;
    document.body.classList.remove("dialog-open");
    document.body.style.top = "";
    window.scrollTo(0, scrollY);
  }

  function requestPassword(name, message) {
    return new Promise(resolve => {
      const dialog = openDialog(`
        <header class="dialog-header">
          <div class="dialog-title-wrap"><h2 id="dialog-title">Administrator password</h2><p>${escapeHTML(name)}</p></div>
          <button class="dialog-close" type="button" data-password-cancel aria-label="Cancel">×</button>
        </header>
        <form id="password-form">
          <div class="dialog-body">
            <p>${escapeHTML(message || "Administrator password required.")}</p>
            <div class="field"><label for="sudo-password">Password</label><input id="sudo-password" name="password" type="password" autocomplete="current-password" required></div>
          </div>
          <footer class="dialog-footer"><button class="secondary-button" type="button" data-password-cancel>Cancel</button><button class="primary-button" type="submit">Continue</button></footer>
        </form>`);
      const finish = value => {
        elements.dialogLayer.replaceChildren();
        unlockPageScroll();
        resolve(value);
      };
      dialog.querySelectorAll("[data-password-cancel]").forEach(button => button.addEventListener("click", () => finish(null)));
      dialog.querySelector("#password-form").addEventListener("submit", event => {
        event.preventDefault();
        finish(new FormData(event.currentTarget).get("password"));
      });
      dialog.querySelector("#sudo-password")?.focus();
    });
  }

  function openAddDialog(tab = state.addTab) {
    state.addTab = tab;
    const available = state.backends.filter(backend => backend.isBundled && !backend.isInstalled && !backend.isMigration);
    const catalog = available.length ? `<div class="catalog-list">${available.map(backend => {
      const item = { backend, frontend: null, displayName: backend.displayName };
      return `<article class="catalog-item">
        ${iconHTML(item)}
        <div><h3>${escapeHTML(backend.displayName)}</h3><p>${escapeHTML(backend.rootOnly ? "Runs with full system access" : "Bundled with Outer Shell")}</p></div>
        <button class="primary-button" type="button" data-action="install" data-backend-key="${escapeHTML(backendKey(backend))}">${backend.rootOnly ? "Install as root" : "Install"}</button>
      </article>`;
    }).join("")}</div>` : `<div class="empty-state"><div class="empty-glyph">✓</div><h2>Bundled apps installed</h2><p>There are no additional bundled apps available.</p></div>`;
    const bash = `<form id="bash-form">
      <div class="form-grid">
        <div class="field"><label for="bash-name">Display name</label><input id="bash-name" name="name" placeholder="My server" required></div>
        <div class="field"><label for="bash-id">Identifier</label><input id="bash-id" class="mono" name="identifier" placeholder="my-server" required><span class="field-note">Used for the service and files.</span></div>
        <div class="field full"><label for="bash-command">Bash commands</label><textarea id="bash-command" name="command" placeholder="cd ~/dev/my-app&#10;npm start" required></textarea></div>
        <div class="field"><label for="bash-transport">Connection</label><select id="bash-transport" name="frontendTransport"><option value="port">TCP port</option><option value="unixSocket">Unix socket</option></select></div>
        <div class="field" data-transport="port"><label for="bash-port">Port</label><input id="bash-port" class="mono" name="port" inputmode="numeric" placeholder="4000" required></div>
        <div class="field" data-transport="unixSocket" hidden><label for="bash-socket">Socket path</label><input id="bash-socket" class="mono" name="socketPath" placeholder="/run/user/1000/my-app.sock"></div>
        <div class="field full"><label for="bash-icon">Icon path <span class="field-note">(optional)</span></label><input id="bash-icon" class="mono" name="iconPath" placeholder="~/Pictures/my-app.png"></div>
      </div>
      <p id="bash-message" class="form-message" role="status"></p>
      <footer class="dialog-footer"><button class="secondary-button" type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="submit">Create app</button></footer>
    </form>`;
    const dialog = openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">Add apps</h2><p>Install a bundled app or run your own command.</p></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="dialog-body">
        <nav class="tabs" aria-label="Add app type"><button class="tab ${tab === "catalog" ? "active" : ""}" type="button" data-action="add-tab" data-tab="catalog">Install apps</button><button class="tab ${tab === "bash" ? "active" : ""}" type="button" data-action="add-tab" data-tab="bash">Bash command</button></nav>
        ${tab === "catalog" ? catalog : bash}
      </div>`, tab === "catalog" ? "" : "wide");
    if (tab === "bash") wireBashForm(dialog);
  }

  function wireBashForm(dialog) {
    const name = dialog.querySelector("#bash-name");
    const identifier = dialog.querySelector("#bash-id");
    let identifierWasEdited = false;
    identifier.addEventListener("input", () => { identifierWasEdited = true; });
    name.addEventListener("input", () => {
      if (!identifierWasEdited) identifier.value = slug(name.value);
    });
    const transport = dialog.querySelector("#bash-transport");
    const updateTransport = () => {
      dialog.querySelectorAll("[data-transport]").forEach(field => {
        const active = field.dataset.transport === transport.value;
        field.hidden = !active;
        field.querySelector("input").required = active;
      });
    };
    transport.addEventListener("change", updateTransport);
    updateTransport();
    dialog.querySelector("#bash-form").addEventListener("submit", submitBashForm);
  }

  async function submitBashForm(event) {
    event.preventDefault();
    const form = event.currentTarget;
    const message = form.querySelector("#bash-message");
    const submit = form.querySelector("button[type=submit]");
    const values = Object.fromEntries(new FormData(form));
    const query = new URLSearchParams({
      recipe: "command-port",
      command: values.command,
      workdir: "~",
      frontendTransport: values.frontendTransport,
      name: values.name.trim(),
      identifier: values.identifier.trim()
    });
    if (values.frontendTransport === "unixSocket") query.set("socketPath", values.socketPath.trim());
    else query.set("port", values.port.trim());
    if (values.iconPath.trim()) query.set("iconPath", values.iconPath.trim());
    submit.disabled = true;
    submit.textContent = "Creating…";
    message.textContent = "";
    try {
      const { buffer } = await requestBuffer(`/api/create?${query}`, { method: "POST" }, true);
      const result = decodeAction(buffer);
      if (!result.ok) throw new Error(result.message || "Could not create the app.");
      closeDialog();
      toast(result.message || `Created ${values.name}.`);
      await refreshBackends({ quiet: true });
    } catch (error) {
      message.textContent = error.message || String(error);
      submit.disabled = false;
      submit.textContent = "Create app";
    }
  }

  function contextMenuGlyph(value) {
    return `<span class="context-menu-glyph" aria-hidden="true">${value}</span>`;
  }

  function endpointContextMenuHTML(item, scope, title) {
    const endpoint = item[scope];
    if (!endpoint) return "";
    const backend = endpoint.backend;
    const running = endpointRunning(endpoint);
    const showControl = backend.canControl && (running || !endpointReady(endpoint));
    const key = escapeHTML(backendKey(backend));
    const script = String(backend.scriptPath || "").trim();
    return `<section class="context-menu-section">
      <h3>${escapeHTML(title)}</h3>
      <a class="context-menu-item" href="${escapeHTML(navigationURL(endpoint.frontend))}" data-action="launch-endpoint" data-app-key="${escapeHTML(item.identity)}" data-app-scope="${scope}" role="menuitem">${contextMenuGlyph("↗")}<span>Open</span></a>
      ${showControl ? `<button class="context-menu-item" type="button" data-action="control" data-operation="${running ? "stop" : "start"}" data-backend-key="${key}" role="menuitem">${contextMenuGlyph(running ? "■" : "▶")}<span>${running ? "Stop" : "Start"}</span></button>` : ""}
      ${backend.logFiles.length ? `<button class="context-menu-item" type="button" data-action="logs" data-backend-key="${key}" role="menuitem">${contextMenuGlyph("≡")}<span>View Logs</span></button>` : ""}
      ${script ? `<button class="context-menu-item" type="button" data-action="copy-script" data-script="${escapeHTML(script)}" role="menuitem">${contextMenuGlyph("⧉")}<span>Copy Script Path</span></button>` : ""}
    </section>`;
  }

  function showContextMenu(contents, point = null, label = "Actions") {
    const dialog = openDialog(`<h2 id="dialog-title" class="context-menu-title">${escapeHTML(label)}</h2><div class="context-menu-scroll" role="menu">${contents}</div>`, "context-menu");
    dialog.setAttribute("aria-label", label);
    positionContextMenu(dialog, point);
    return dialog;
  }

  function openAppMenu(identity, point = null) {
    const item = findItem(identity);
    if (!item) return;
    if (item.workspace) { openContainerMenu(item.workspace.id, item.app.serviceID, containerFrontendKey(item.app)); return; }
    const backend = item.backend;
    const sections = [endpointContextMenuHTML(item, "primary", backend.serviceScope === "system" ? "Root" : "User")];
    const management = [
      bookmarkMenuHTML(hostBookmarkKey(item)),
      renameEndpointMenuHTML(item),
      reorderMenuHTML(item),
      `<button class="context-menu-item" type="button" data-action="copy-url" data-app-key="${escapeHTML(item.identity)}" role="menuitem">${contextMenuGlyph("⧉")}<span>Copy URL</span></button>`,
      `<button class="context-menu-item" type="button" data-action="move-bookmark" data-app-key="${escapeHTML(item.identity)}" role="menuitem">${contextMenuGlyph("▤")}<span>Move bookmark to folder…</span></button>`
    ];
    if (backend.supportsRoot && backend.serviceScope !== "system" && !backend.rootOnly) {
      management.push(`<button class="context-menu-item" type="button" data-action="control" data-operation="${backend.hasRootSupport ? "removeRootSupport" : "addRootSupport"}" data-backend-key="${escapeHTML(backendKey(backend))}" role="menuitem">${contextMenuGlyph("◇")}<span>${backend.hasRootSupport ? "Remove root support" : "Add root shortcut"}</span></button>`);
    }
    if (backend.canUninstall) {
      management.push(`<button class="context-menu-item danger" type="button" data-action="uninstall" data-backend-key="${escapeHTML(backendKey(backend))}" role="menuitem">${contextMenuGlyph("−")}<span>Uninstall</span></button>`);
    }
    if (management.length) sections.push(`<section class="context-menu-section context-menu-management">${management.join("")}</section>`);
    showContextMenu(`<div class="context-menu-app-heading">${launcherIconHTML(item)}<strong>${escapeHTML(item.displayName)}</strong></div>${sections.filter(Boolean).join("")}`, point, `${item.displayName} actions`);
  }

  function managedContainer(workspace) {
    return workspace.ownsContainer !== false && workspace.managementKind !== "attached";
  }

  function containerPreparing(workspace) {
    return state.safeSpaceBusy.has(workspace.id) || ["creating", "starting", "rebuilding"].includes(safeSpaceState(workspace));
  }

  function containerDialogHeader(title) {
    return `<header class="dialog-header"><h2 id="dialog-title">${escapeHTML(title)}</h2><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>`;
  }

  function runtimeOptions(selected = "") {
    return state.providers.map(provider => `<option value="${escapeHTML(provider.id)}" ${provider.id === selected ? "selected" : ""} ${provider.isAvailable ? "" : "disabled"}>${escapeHTML(provider.name)}${provider.isAvailable ? "" : " (unavailable)"}</option>`).join("");
  }

  async function containerOperation(operation, values) {
    const id = values.workspaceID;
    if (id) state.safeSpaceBusy.add(id);
    render();
    try {
      const result = await safeSpaceRequest(operation, values);
      if (Array.isArray(result.workspaces)) state.safeSpaces = result.workspaces;
      return result;
    } finally {
      if (id) state.safeSpaceBusy.delete(id);
      render();
    }
  }

  function openCreateContainer() {
    const provider = state.providers.find(value => value.isAvailable);
    const dialog = openDialog(`${containerDialogHeader("New container")}
      <form class="container-form"><div class="dialog-body form-grid">
        <label class="field full">Name<input name="name" required autocomplete="off"></label>
        <label class="field full">Runtime<select name="runtimeProviderID">${runtimeOptions(provider?.id)}</select></label>
        <label class="field full">Base image<input name="baseImage" value="${escapeHTML(provider?.defaultBaseImage || "")}" required spellcheck="false"></label>
        <label class="field">Max CPUs<input name="cpus" type="number" min="1" step="1" value="4" required></label>
        <label class="field">Max Memory (GB)<input name="memoryInGB" type="number" min="1" step="1" value="8" required></label>
        <p class="field-note full">The container is built on this server. You can edit its Dockerfile and runtime configuration after creation.</p>
        <p class="container-message full" role="status">${provider ? "" : "No container runtime is available on this server."}</p>
      </div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="submit" ${provider ? "" : "disabled"}>Create</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const data = new FormData(form);
      const button = form.querySelector('[type="submit"]');
      button.disabled = true;
      const message = form.querySelector(".container-message");
      message.textContent = "Creating container…";
      try {
        await containerOperation("create", { name: String(data.get("name")).trim(), runtimeProviderID: data.get("runtimeProviderID"), baseImage: String(data.get("baseImage")).trim(), cpus: Number(data.get("cpus")), memoryInGB: Number(data.get("memoryInGB")) });
        if (dialog.isConnected) closeDialog();
        toast("Container creation requested. Its status appears under All endpoints.");
      } catch (error) { message.textContent = error.message; button.disabled = false; }
    });
  }

  function configurationRow(kind, value = {}) {
    const input = (name, label, content, type = "text") => `<label class="field">${label}<input data-column="${name}" type="${type}" value="${escapeHTML(content ?? "")}" ${type === "number" ? 'min="1" max="65535" step="1"' : 'spellcheck="false"'}></label>`;
    let fields;
    if (kind === "mounts") {
      fields = input("name", "Name", value.name) + input("hostPath", "Server folder", value.hostPath) + input("guestPath", "Container path", value.guestPath) + `<label class="container-check"><input data-column="isReadOnly" type="checkbox" ${value.isReadOnly ? "checked" : ""}>Read only</label>`;
    } else if (kind === "environment") {
      fields = input("name", "Name", value.name) + input("value", "Value", value.value);
    } else {
      fields = input("hostPort", "Server port", value.hostPort, "number") + input("containerPort", "Container port", value.containerPort, "number");
    }
    return `<div class="configuration-row" data-row="${kind}" data-id="${escapeHTML(value.id || makeRequestID())}">${fields}<button type="button" data-remove-row aria-label="Remove ${kind === "mounts" ? "mount" : kind === "environment" ? "variable" : "port"}">×</button></div>`;
  }

  function configurationValues(form, kind) {
    return [...form.querySelectorAll(`[data-row="${kind}"]`)].map(row => {
      const value = kind === "mounts" ? { id: row.dataset.id } : {};
      row.querySelectorAll("[data-column]").forEach(input => {
        value[input.dataset.column] = input.type === "checkbox" ? input.checked : input.type === "number" ? Number(input.value) : input.value;
      });
      return value;
    });
  }

  function validateConfiguration(value) {
    if (!value.dockerfile.trim()) throw new Error("The Dockerfile cannot be empty.");
    const paths = new Set();
    for (const mount of value.mounts) {
      mount.name = mount.name.trim(); mount.hostPath = mount.hostPath.trim(); mount.guestPath = mount.guestPath.trim();
      if (!mount.hostPath.startsWith("/") || !mount.guestPath.startsWith("/") || mount.guestPath.includes(":") || mount.guestPath === "/var/lib/outershell/project" || paths.has(mount.guestPath)) throw new Error("Mounts need an absolute server path and a unique absolute container path.");
      paths.add(mount.guestPath);
    }
    const names = new Set();
    for (const variable of value.environment) {
      variable.name = variable.name.trim();
      if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(variable.name) || names.has(variable.name) || variable.value.includes("\0")) throw new Error("Environment names must be unique shell identifiers.");
      names.add(variable.name);
    }
    const ports = new Set();
    for (const port of value.publishedPorts) {
      if (![port.hostPort, port.containerPort].every(number => Number.isInteger(number) && number >= 1 && number <= 65535) || ports.has(port.hostPort)) throw new Error("Ports must be between 1 and 65535, with no repeated server ports.");
      ports.add(port.hostPort);
    }
  }

  function openContainerConfiguration(id) {
    const workspace = findSafeSpace(id);
    if (!workspace || !managedContainer(workspace)) return;
    const recipe = workspace.recipe;
    if (!recipe || typeof recipe.containerfile !== "string") { toast("The container configuration is not available yet.", true); return; }
    const infrastructure = (workspace.mounts || []).filter(mount => mount.isInfrastructure);
    const tabs = [["dockerfile", "Dockerfile"], ["mounts", "Folder mounts"], ["environment", "Environment"], ["ports", "Ports"], ["runtime", "Runtime"]];
    const dialog = openDialog(`${containerDialogHeader(`Edit ${workspace.name}`)}
      <form class="container-form" novalidate><div class="dialog-body">
        <label class="field">Name<input name="name" value="${escapeHTML(workspace.name)}" required></label>
        <div class="configuration-tabs" role="tablist" aria-label="Container configuration">${tabs.map(([key, label], index) => `<button type="button" role="tab" id="config-tab-${key}" aria-controls="config-panel-${key}" aria-selected="${index === 0}" data-config-tab="${key}">${label}</button>`).join("")}</div>
        <section role="tabpanel" id="config-panel-dockerfile" aria-labelledby="config-tab-dockerfile" data-config-panel="dockerfile"><label class="field">Dockerfile<textarea name="dockerfile" class="dockerfile-editor" spellcheck="false" autocapitalize="off" autocorrect="off">${escapeHTML(recipe.containerfile)}</textarea></label><p class="field-note">Save stores the definition. Save &amp; rebuild applies it to the container.</p></section>
        <section role="tabpanel" id="config-panel-mounts" aria-labelledby="config-tab-mounts" data-config-panel="mounts" hidden><p class="field-note">Server folders are paths on the machine running Outer Shell. They must already exist.</p><div data-rows="mounts">${(workspace.mounts || []).filter(mount => !mount.isInfrastructure).map(mount => configurationRow("mounts", mount)).join("")}</div><button type="button" data-add-row="mounts">Add folder mount</button>${infrastructure.length ? `<details class="managed-mounts"><summary>Managed mounts (${infrastructure.length})</summary>${infrastructure.map(mount => `<p><code>${escapeHTML(mount.hostPath)} → ${escapeHTML(mount.guestPath)}</code></p>`).join("")}</details>` : ""}</section>
        <section role="tabpanel" id="config-panel-environment" aria-labelledby="config-tab-environment" data-config-panel="environment" hidden><div data-rows="environment">${(recipe.environment || []).map(value => configurationRow("environment", value)).join("")}</div><button type="button" data-add-row="environment">Add variable</button></section>
        <section role="tabpanel" id="config-panel-ports" aria-labelledby="config-tab-ports" data-config-panel="ports" hidden><p class="field-note">Publish a container TCP port on this server.</p><div data-rows="ports">${(recipe.publishedPorts || []).map(value => configurationRow("ports", value)).join("")}</div><button type="button" data-add-row="ports">Add port</button></section>
        <section role="tabpanel" id="config-panel-runtime" aria-labelledby="config-tab-runtime" data-config-panel="runtime" hidden><p>${escapeHTML(safeSpaceRuntimeDescription(workspace))} · ${escapeHTML(workspace.cpus)} CPUs · ${escapeHTML(workspace.memoryInGB)} GB</p><label class="field">Runtime<select name="runtimeProviderID">${runtimeOptions(workspace.runtime?.providerID)}</select></label><p class="field-note">Changing runtime saves the configuration and rebuilds the container with the selected provider.</p><button type="submit" value="runtime">Save &amp; change runtime</button></section>
        <p class="container-message" role="status">${recipe.needsRebuild ? "Saved changes need a rebuild." : ""}</p>
        <p class="field-note">Rebuilding restarts the container. Changes outside its Dockerfile, persistent data, and mounted folders are not retained.</p>
      </div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Close</button><button type="submit" value="save">Save</button><button class="primary-button" type="submit" value="rebuild">Save &amp; rebuild</button></footer></form>`, "wide container-editor");
    const form = dialog.querySelector("form");
    form.addEventListener("click", event => {
      const tab = event.target.closest("[data-config-tab]");
      if (tab) {
        form.querySelectorAll("[data-config-tab]").forEach(button => button.setAttribute("aria-selected", String(button === tab)));
        form.querySelectorAll("[data-config-panel]").forEach(panel => { panel.hidden = panel.dataset.configPanel !== tab.dataset.configTab; });
      }
      const add = event.target.closest("[data-add-row]");
      if (add) form.querySelector(`[data-rows="${add.dataset.addRow}"]`).insertAdjacentHTML("beforeend", configurationRow(add.dataset.addRow));
      event.target.closest("[data-remove-row]")?.closest("[data-row]").remove();
    });
    let savedDockerfile = recipe.containerfile;
    let savedName = workspace.name;
    let currentProvider = workspace.runtime?.providerID;
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const action = event.submitter?.value || "save";
      const message = form.querySelector(".container-message");
      const values = { workspaceID: id, dockerfile: form.elements.dockerfile.value, mounts: configurationValues(form, "mounts"), environment: configurationValues(form, "environment"), publishedPorts: configurationValues(form, "ports") };
      const name = form.elements.name.value.trim();
      try { if (!name) throw new Error("Enter a container name."); validateConfiguration(values); }
      catch (error) { message.textContent = error.message; return; }
      const provider = form.elements.runtimeProviderID.value;
      const controls = [...form.querySelectorAll("input, select, textarea, button")];
      controls.forEach(control => { control.disabled = true; });
      let saved = false;
      message.textContent = "Saving…";
      try {
        if (values.dockerfile !== savedDockerfile) {
          // macOS reads command; the Linux provider reads dockerfile.
          await containerOperation("updateDockerfile", { workspaceID: id, command: values.dockerfile, dockerfile: values.dockerfile });
          savedDockerfile = values.dockerfile;
          saved = true;
        }
        await containerOperation("updateContainerConfiguration", values);
        saved = true;
        if (name !== savedName) { await containerOperation("rename", { workspaceID: id, name }); savedName = name; }
        if (action === "runtime" && provider !== currentProvider) {
          message.textContent = "Changing runtime…";
          await containerOperation("changeRuntime", { workspaceID: id, runtimeProviderID: provider });
          currentProvider = provider;
        } else if (action === "rebuild") {
          message.textContent = "Rebuilding…";
          await containerOperation("rebuildRecipe", { workspaceID: id });
        }
        message.textContent = action === "save" ? "Saved. Rebuild to apply configuration changes." : "Request completed. Build status appears under All endpoints.";
      } catch (error) { message.textContent = `${saved ? "Some changes were saved. " : ""}${error.message}`; }
      finally { controls.forEach(control => { control.disabled = false; }); }
    });
  }

  function containerTransferRequest(transferID, offset, length) {
    if (!/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(transferID)) throw new Error("Invalid container transfer identifier.");
    const bytes = new Uint8Array(48);
    const view = new DataView(bytes.buffer);
    bytes.set([0x4f, 0x53, 0x43, 0x54]);
    view.setUint16(4, 1, true); view.setUint16(6, 1, true);
    bytes.set(transferID.replaceAll("-", "").match(/../g).map(value => parseInt(value, 16)), 8);
    view.setBigUint64(24, BigInt(offset), true); view.setBigUint64(32, BigInt(length), true);
    return bytes;
  }

  function decodeContainerTransfer(buffer, offset, total) {
    if (buffer.byteLength < 40) throw new Error("Incomplete container transfer response.");
    const reader = new PayloadReader(buffer);
    if (reader.u32(0) !== 0x5443534f || reader.view.getUint16(4, true) !== 1) throw new Error("Invalid container transfer response.");
    if (reader.view.getUint16(6, true)) throw new Error(reader.stringRef(32) || "Container download failed.");
    const bytes = reader.bytesRef(24);
    const next = Number(reader.u64(8));
    if (Number(reader.u64(16)) !== total || next !== offset + bytes.length || next > total || (next < total && next <= offset)) throw new Error("Container download size changed or transfer made no progress.");
    return { bytes, next };
  }

  function openShareContainer(id) {
    const workspace = findSafeSpace(id);
    if (!workspace || !managedContainer(workspace)) return;
    const dialog = openDialog(`${containerDialogHeader("Share Container")}
      <form class="container-form"><div class="dialog-body">
        <p>Create a portable archive of ${escapeHTML(workspace.name)} with its Dockerfile and build-context files.</p>
        <label class="container-check"><input name="persistent" type="checkbox">Include persistent data</label>
        <label class="container-check"><input name="mounts" type="checkbox">Include mounted folders from this server</label>
        <p class="field-note">The archive includes configured environment values. Only include data you intend to share. Without a browser file picker, the download is assembled in browser memory.</p>
        <p class="container-message" role="status"></p><div class="container-download"></div>
      </div><footer class="dialog-footer"><button type="button" data-action="close-dialog">Close</button><button class="primary-button" type="submit">Prepare archive</button></footer></form>`);
    const form = dialog.querySelector("form");
    form.addEventListener("submit", async event => {
      event.preventDefault();
      const button = form.querySelector('[type="submit"]');
      const message = form.querySelector(".container-message");
      const includePersistentData = form.elements.persistent.checked;
      const includeMountedFolders = form.elements.mounts.checked;
      button.disabled = true;
      let writer;
      try {
        if (typeof window.showSaveFilePicker === "function") {
          const handle = await window.showSaveFilePicker({ suggestedName: `${workspace.name.replace(/[^a-zA-Z0-9_-]/g, "-") || "Container"}.outershell-container` });
          writer = await handle.createWritable();
        }
        message.textContent = "Preparing archive…";
        const result = await containerOperation("prepareShare", { workspaceID: id, includePersistentData, includeMountedFolders });
        const total = Number(result.byteCount);
        if (!Number.isSafeInteger(total) || total <= 0) throw new Error("Invalid shared container size.");
        const parts = [];
        let offset = 0;
        while (offset < total) {
          const response = await fetch("/api/safe-spaces", { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: containerTransferRequest(result.transferID, offset, 4 * 1024 * 1024) });
          if (!response.ok) throw new Error(`Container download returned HTTP ${response.status}.`);
          const chunk = decodeContainerTransfer(await response.arrayBuffer(), offset, total);
          if (writer) await writer.write(chunk.bytes);
          else parts.push(chunk.bytes);
          offset = chunk.next;
          message.textContent = `Downloading archive… ${Math.round(offset / total * 100)}%`;
        }
        if (writer) { await writer.close(); writer = null; message.textContent = "Container archive saved."; }
        else {
          const file = new File(parts, result.fileName || "Container.outershell-container", { type: "application/octet-stream" });
          if (state.containerDownloadURL) URL.revokeObjectURL(state.containerDownloadURL);
          state.containerDownloadURL = URL.createObjectURL(file);
          const output = form.querySelector(".container-download");
          output.replaceChildren();
          const link = document.createElement("a"); link.href = state.containerDownloadURL; link.download = file.name; link.textContent = `Download ${file.name}`; output.append(link);
          if (typeof navigator.canShare === "function" && navigator.canShare({ files: [file] })) {
            const share = document.createElement("button"); share.type = "button"; share.textContent = "Share…";
            share.addEventListener("click", async () => {
              try { await navigator.share({ files: [file], title: workspace.name }); }
              catch (error) { if (error.name !== "AbortError") message.textContent = error.message; }
            });
            output.append(share);
          }
          message.textContent = "Archive ready.";
        }
      } catch (error) {
        if (writer) { try { await writer.abort(); } catch {} }
        message.textContent = error.name === "AbortError" ? "Cancelled." : error.message;
      } finally { button.disabled = false; }
    });
  }

  function openContainerMenu(id, serviceID, frontendKey) {
    const workspace = findSafeSpace(id);
    if (!workspace) return;
    const app = serviceID ? findSafeSpaceApp(workspace, serviceID, frontendKey) : null;
    if (serviceID && !app) return;
    const name = app ? state.endpointNames[containerBookmarkKey(workspace, app)] || app.displayName || app.serviceID : workspace.name || "Container";
    const running = app ? app.isRunning : safeSpaceIsRunning(workspace);
    const preparing = containerPreparing(workspace);
    const operation = app ? (running ? "stopApp" : "startApp") : (running ? "stop" : "start");
    const attributes = `data-safe-space-id="${escapeHTML(id)}" data-service-id="${escapeHTML(serviceID || "")}" data-frontend-key="${escapeHTML(frontendKey || "")}"`;
    const url = app ? safeSpaceAppNavigationURL(workspace, app) : "";
    showContextMenu(`<div class="context-menu-app-heading"><strong>${escapeHTML(name)}</strong></div>
      <section class="context-menu-section">
        ${!app && managedContainer(workspace) ? `<button class="context-menu-item" type="button" data-action="configure-container" data-safe-space-id="${escapeHTML(id)}" role="menuitem" ${preparing ? "disabled" : ""}>Edit container…</button><button class="context-menu-item" type="button" data-action="share-container" data-safe-space-id="${escapeHTML(id)}" role="menuitem" ${preparing ? "disabled" : ""}>Share Container…</button>` : ""}
        ${app ? bookmarkMenuHTML(containerBookmarkKey(workspace, app)) + renameEndpointMenuHTML(findItem(containerBookmarkKey(workspace, app))) : ""}
        ${app ? reorderMenuHTML(findItem(containerBookmarkKey(workspace, app))) : ""}
        ${app ? `<button class="context-menu-item" type="button" data-action="move-bookmark" data-app-key="${escapeHTML(containerBookmarkKey(workspace, app))}" role="menuitem">Move bookmark to folder…</button>` : ""}
        ${app ? `<a class="context-menu-item" href="${escapeHTML(url)}" data-action="launch-safe-space-app" ${attributes} role="menuitem">Open</a>` : `<p class="container-details">${escapeHTML(safeSpaceRuntimeDescription(workspace))}</p>`}
        <button class="context-menu-item" type="button" data-action="container-control" data-operation="${operation}" ${attributes} role="menuitem" ${preparing ? "disabled" : ""}>${preparing ? "Preparing…" : running ? "Stop" : "Start"}</button>
        ${app && url !== "#" ? `<button class="context-menu-item" type="button" data-action="copy-container-url" ${attributes} role="menuitem">Copy URL</button>` : ""}
      </section>`, null, `${name} actions`);
  }

  function openHomeMenu(anchor) {
    const outerShell = state.backends.find(backend => backend.serviceID === "org.outershell.OuterShell" && backend.serviceScope !== "system")
      || state.backends.find(backend => backend.serviceID === "org.outershell.OuterShell");
    const actions = [
      `<button class="context-menu-item plain" type="button" data-action="create-container" role="menuitem">New container…</button>`
    ];
    if (!state.editing) actions.unshift(`<button class="context-menu-item plain" type="button" data-action="toggle-edit" role="menuitem">Edit</button>`);
    if (outerShell) {
      const key = escapeHTML(backendKey(outerShell));
      actions.push(
        `<button class="context-menu-item plain" type="button" data-action="about-outer-shell" data-backend-key="${key}" role="menuitem">About Outer Shell</button>`,
        `<button class="context-menu-item plain" type="button" data-action="logs" data-backend-key="${key}" role="menuitem">View Logs for Outer Shell</button>`
      );
      if (outerShell.menuBarVisibilityAvailable) {
        actions.push(`<button class="context-menu-item menu-toggle" type="button" data-action="toggle-menu-bar" data-backend-key="${key}" data-enabled="${outerShell.menuBarVisibilityEnabled ? "true" : "false"}" role="menuitemcheckbox" aria-checked="${outerShell.menuBarVisibilityEnabled ? "true" : "false"}">${contextMenuGlyph(outerShell.menuBarVisibilityEnabled ? "✓" : "")}<span>Show in macOS menu bar when backends are running</span></button>`);
      }
      actions.push(`<button class="context-menu-item plain" type="button" data-action="check-outer-shell-update" data-backend-key="${key}" role="menuitem">Check for Updates</button>`);
      actions.push(`<button class="context-menu-item plain" type="button" data-action="uninstall-outer-shell" data-backend-key="${key}" role="menuitem">Uninstall Outer Shell</button>`);
    }
    const bounds = anchor.getBoundingClientRect();
    const point = { x: Math.max(9, bounds.right - 280), y: bounds.bottom + 5 };
    showContextMenu(`<section class="context-menu-section">${actions.join("")}</section>`, point, "Outer Shell");
  }

  function openOuterShellAbout(key) {
    const backend = findBackend(key);
    if (!backend) return;
    const version = String(backend.installedVersion || "").trim() || "unknown";
    const updateSource = String(backend.publicBaseURL || "").trim() || "unknown";
    const text = [
      "Outer Shell",
      `Version: ${version}`,
      `Service ID: ${backend.serviceID}`,
      `Scope: ${backend.serviceScope}`,
      `Status: ${backend.status}`,
      `Update source: ${updateSource}`
    ].join("\n");
    openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">About Outer Shell</h2></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="dialog-body"><pre class="about-output">${escapeHTML(text)}</pre></div>
      <footer class="dialog-footer"><button class="primary-button" type="button" data-action="close-dialog">OK</button></footer>`);
  }

  function openOuterShellUpdate(backend, result) {
    const installed = String(result.installedVersion || backend.installedVersion || "").trim() || "unknown";
    const available = String(result.availableVersion || backend.availableVersion || "").trim() || "the latest version";
    openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">Update Outer Shell</h2><p>${escapeHTML(result.message || `Outer Shell ${available} is available.`)}</p></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="dialog-body"><p>Update Outer Shell from version <strong>${escapeHTML(installed)}</strong> to <strong>${escapeHTML(available)}</strong>?</p></div>
      <footer class="dialog-footer"><button class="secondary-button" type="button" data-action="close-dialog">Cancel</button><button class="primary-button" type="button" data-action="update-outer-shell" data-backend-key="${escapeHTML(backendKey(backend))}">Update</button></footer>`);
  }

  async function checkOuterShellUpdate(key, button) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    const original = button.textContent;
    button.textContent = "Checking for Updates…";
    try {
      const result = await control(backend, "checkUpdate");
      if (!result.ok) throw new Error(result.message || "Could not check for updates.");
      if (result.updateAvailable) {
        openOuterShellUpdate(backend, result);
      } else {
        closeDialog();
        toast(result.message || "Outer Shell is up to date.");
      }
    } catch (error) {
      toast(error.message || String(error), true);
      button.disabled = false;
      button.textContent = original;
    }
  }

  async function performOuterShellOperation(key, operation, button, reloadAfter = false) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    const original = button.textContent;
    button.textContent = `${original}…`;
    try {
      const result = await control(backend, operation);
      if (!result.ok) throw new Error(result.message || "The action failed.");
      closeDialog();
      toast(result.message);
      if (reloadAfter) window.setTimeout(() => window.location.reload(), 1250);
    } catch (error) {
      toast(error.message || String(error), true);
      button.disabled = false;
      button.textContent = original;
    }
  }

  async function uninstallOuterShell(key, button) {
    if (!window.confirm("Uninstall Outer Shell?")) return;
    await performOuterShellOperation(key, "uninstallOuterShell", button);
  }

  async function performControl(key, operation, button) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    const original = button.textContent;
    button.textContent = `${original}…`;
    try {
      const result = await control(backend, operation);
      if (!result.ok) throw new Error(result.message || "The action failed.");
      closeDialog();
      toast(result.message);
      await refreshBackends({ quiet: true });
    } catch (error) {
      toast(error.message || String(error), true);
      button.disabled = false;
      button.textContent = original;
    }
  }

  async function installBackend(key, button) {
    const backend = findBackend(key);
    if (!backend) return;
    button.disabled = true;
    button.textContent = "Installing…";
    try {
      const operation = backend.rootOnly ? "runRoot" : "run";
      const result = await control(backend, operation);
      if (!result.ok) throw new Error(result.message || `Could not install ${backend.displayName}.`);
      closeDialog();
      toast(result.message || `Installed ${backend.displayName}.`);
      await refreshBackends({ quiet: true });
    } catch (error) {
      toast(error.message || String(error), true);
      button.disabled = false;
      button.textContent = backend.rootOnly ? "Install as root" : "Install";
    }
  }

  async function uninstallBackend(key, button) {
    const backend = findBackend(key);
    if (!backend || !window.confirm(`Uninstall ${backend.displayName}?`)) return;
    await performControl(key, "uninstall", button);
  }

  async function copyText(text) {
    try {
      await navigator.clipboard.writeText(text);
      toast("Copied to the clipboard.");
    } catch (_) {
      const area = document.createElement("textarea");
      area.value = text;
      document.body.append(area);
      area.select();
      document.execCommand("copy");
      area.remove();
      toast("Copied to the clipboard.");
    }
  }

  function openLogs(key, selectedIndex = 0) {
    const backend = findBackend(key);
    if (!backend) return;
    if (!backend.logFiles.length) {
      state.logSelection = null;
      openDialog(`
        <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">${escapeHTML(backend.displayName)} logs</h2><p>No registered log file.</p></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
        <pre class="log-output">No registered log file.</pre>`, "log-dialog");
      restartEventWatch();
      return;
    }
    state.logSelection = { backendKey: key, index: Math.min(selectedIndex, backend.logFiles.length - 1) };
    openDialog(`
      <header class="dialog-header"><div class="dialog-title-wrap"><h2 id="dialog-title">${escapeHTML(backend.displayName)} logs</h2><p>Updates automatically while this window is open.</p></div><button class="dialog-close" type="button" data-action="close-dialog" aria-label="Close">×</button></header>
      <div class="log-toolbar">
        <select id="log-select" aria-label="Log file">${backend.logFiles.map((log, index) => `<option value="${index}" ${index === state.logSelection.index ? "selected" : ""}>${escapeHTML(log.displayName || log.path)}</option>`).join("")}</select>
        <span id="log-meta" class="log-meta">Loading…</span>
        <button class="secondary-button" type="button" data-action="refresh-log">Refresh</button>
      </div>
      <pre id="log-output" class="log-output">Loading logs…</pre>`, "log-dialog");
    restartEventWatch();
    fetchSelectedLog();
  }

  async function fetchSelectedLog() {
    const selection = state.logSelection;
    if (!selection) return;
    const backend = findBackend(selection.backendKey);
    if (!backend) return;
    const output = elements.dialogLayer.querySelector("#log-output");
    const meta = elements.dialogLayer.querySelector("#log-meta");
    try {
      const query = new URLSearchParams({ serviceID: backend.serviceID, logIndex: String(selection.index), bytes: String(512 * 1024) });
      const { buffer } = await requestBuffer(`/api/logs?${query}`);
      const log = decodeLog(buffer);
      if (!state.logSelection || state.logSelection.backendKey !== selection.backendKey || state.logSelection.index !== selection.index) return;
      if (output) output.textContent = log.error || log.contents || "No logs yet.";
      if (meta) meta.textContent = `${log.path || "No registered log"}${log.truncated ? " · showing latest output" : ""} · ${formatBytes(log.fileSize)}`;
      if (output && output.scrollHeight - output.scrollTop - output.clientHeight < 90) output.scrollTop = output.scrollHeight;
    } catch (error) {
      if (output) output.textContent = error.message || String(error);
      if (meta) meta.textContent = "Could not read logs";
    }
  }

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
    const units = ["B", "KB", "MB", "GB"];
    const unit = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
    const value = bytes / (1024 ** unit);
    return `${value >= 10 || unit === 0 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
  }

  function restartEventWatch() {
    state.eventAbort?.abort();
  }

  async function watchEvents() {
    while (!state.stopped) {
      if (state.suspended) { await delay(1000); continue; }
      const controller = new AbortController();
      state.eventAbort = controller;
      const query = new URLSearchParams({
        sinceBackends: state.backendsVersion.toString(),
        sinceLog: state.logVersion.toString()
      });
      const selection = state.logSelection;
      if (selection) {
        const backend = findBackend(selection.backendKey);
        if (backend) {
          query.set("serviceID", backend.serviceID);
          query.set("logIndex", String(selection.index));
        }
      }
      try {
        const { buffer } = await requestBuffer(`/api/events?${query}`, { signal: controller.signal });
        if (controller.signal.aborted || state.suspended) continue;
        const event = decodeEvents(buffer);
        state.backendsVersion = event.backendsVersion;
        state.logVersion = event.logVersion;
        if (event.backendsChanged) await refreshBackends({ quiet: true });
        if (event.logChanged) await fetchSelectedLog();
      } catch (error) {
        if (error.name !== "AbortError") await delay(1200);
      }
    }
  }

  loadBookmarks();
  updatePage();
  let layoutFrame = 0;
  const scheduleBookmarkLayout = () => {
    cancelAnimationFrame(layoutFrame);
    layoutFrame = requestAnimationFrame(layoutBookmarks);
  };
  let bookmarkLayoutWidth = "";
  const scheduleBookmarkWidthChange = () => {
    const grid = elements.sections.closest(".dashboard");
    const width = `${grid.clientWidth}:${window.innerWidth <= 680}`;
    if (width === bookmarkLayoutWidth) return;
    bookmarkLayoutWidth = width;
    scheduleBookmarkLayout();
  };
  new ResizeObserver(scheduleBookmarkWidthChange).observe(elements.sections.closest(".dashboard"));
  window.addEventListener("resize", scheduleBookmarkWidthChange);
  elements.sections.addEventListener("load", scheduleBookmarkLayout, true);
  window.addEventListener("hashchange", updatePage);
  elements.search.addEventListener("input", event => {
    state.query = event.target.value;
    render();
  });
  elements.add.addEventListener("click", () => openAddDialog());
  elements.refresh.addEventListener("click", event => openHomeMenu(event.currentTarget));
  document.body.addEventListener("contextmenu", event => {
    const identity = itemIdentityFromTarget(event.target);
    const folder = event.target.closest("[data-folder]")?.dataset.folder;
    if (!identity && folder === undefined) return;
    event.preventDefault();
    cancelLongPress();
    cancelAppDrag();
    clearPressFeedback();
    if (Date.now() < state.suppressLaunchUntil && elements.dialogLayer.childElementCount) return;
    if (identity) openAppMenu(identity, { x: event.clientX, y: event.clientY });
    else openFolderMenu(folder, { x: event.clientX, y: event.clientY });
  });
  document.body.addEventListener("pointerdown", beginFolderResize);
  document.body.addEventListener("pointermove", moveFolderResize, { passive: false });
  document.body.addEventListener("pointerup", event => endFolderResize(event, true));
  document.body.addEventListener("pointercancel", event => endFolderResize(event, false));
  document.body.addEventListener("lostpointercapture", event => endFolderResize(event, false));
  elements.sections.addEventListener("keydown", event => {
    const handle = event.target.closest("[data-folder-resize]");
    if (!handle || !["ArrowLeft", "ArrowRight"].includes(event.key)) return;
    event.preventDefault();
    const folder = handle.closest("[data-folder]");
    const width = state.folderWidths[folder.dataset.folder] || folder.getBoundingClientRect().width;
    const direction = handle.dataset.folderResize === "left" ? -1 : 1;
    saveFolderWidth(folder.dataset.folder, width + (event.key === "ArrowRight" ? 32 : -32) * direction);
    layoutBookmarks();
  });
  document.body.addEventListener("pointerdown", beginLongPress);
  document.body.addEventListener("pointerdown", beginPressFeedback);
  document.body.addEventListener("pointerdown", beginAppDrag);
  document.body.addEventListener("dragstart", event => {
    if (event.target.closest("[data-drag-app-key]")) event.preventDefault();
  });
  document.body.addEventListener("pointermove", moveLongPress);
  document.body.addEventListener("pointermove", movePressFeedback);
  document.body.addEventListener("pointermove", moveAppDrag, { passive: false });
  document.body.addEventListener("touchmove", event => {
    if (state.appDrag?.touchArmed || state.appDrag?.isDragging) event.preventDefault();
  }, { passive: false });
  document.body.addEventListener("pointerup", event => { finishAppDrag(event); cancelLongPress(); clearPressFeedback(); });
  document.body.addEventListener("pointercancel", () => { cancelAppDrag(); cancelLongPress(); clearPressFeedback(); });
  document.body.addEventListener("lostpointercapture", () => { cancelAppDrag(); cancelLongPress(); clearPressFeedback(); });
  document.body.addEventListener("selectstart", event => {
    if (itemIdentityFromTarget(event.target) || Date.now() < state.suppressLaunchUntil) event.preventDefault();
  });
  document.body.addEventListener("click", async event => {
    const actionElement = event.target.closest("[data-action]");
    if (!actionElement) return;
    if (actionElement.classList.contains("dialog-backdrop") && event.target !== actionElement) return;
    const action = actionElement.dataset.action;
    if (Date.now() < state.suppressLaunchUntil && actionElement.closest(".app-sections")) {
      event.preventDefault();
      return;
    }
    if (action === "rename-endpoint") { const item = findItem(actionElement.dataset.appKey); if (item) openRenameEndpoint(item); return; }
    if (action === "rename-folder") { openRenameFolder(actionElement.dataset.folderName); return; }
    if (action === "create-container") { openCreateContainer(); return; }
    if (action === "configure-container") { openContainerConfiguration(actionElement.dataset.safeSpaceId); return; }
    if (action === "share-container") { openShareContainer(actionElement.dataset.safeSpaceId); return; }
    if (action === "toggle-edit") {
      state.editing = !state.editing;
      elements.shell.classList.toggle("is-editing", state.editing);
      elements.edit.hidden = !state.editing;
      if (actionElement.closest(".context-menu")) closeDialog();
      render();
      return;
    }
    if (action === "browse-urls") {
      window.location.hash = "bookmarks";
      updatePage();
      elements.urlsPage.scrollIntoView({ block: "start" });
      return;
    }
    if (action === "toggle-bookmark") {
      const key = actionElement.dataset.bookmarkKey;
      const added = !isBookmarked(key);
      if (setBookmark(key, added)) {
        if (actionElement.closest(".context-menu")) closeDialog();
        render();
        toast(added ? "Bookmark added." : "Bookmark removed. The endpoint is still registered.");
        const button = [...elements.directory.querySelectorAll("[data-bookmark-key]")].find(value => value.dataset.bookmarkKey === key);
        button?.focus({ preventScroll: true });
      }
      return;
    }
    if (action === "edit-bookmark") { openAppMenu(actionElement.dataset.appKey); return; }
    if (action === "copy-url") {
      const item = findItem(actionElement.dataset.appKey);
      if (item) { closeDialog(); await copyText(navigationURL(item.frontend)); }
      return;
    }
    if (action === "reorder-bookmark") {
      const item = findItem(actionElement.dataset.appKey);
      if (!item) return;
      const direction = Number(actionElement.dataset.direction);
      const siblings = allBookmarkItems().filter(value => isBookmarked(hostBookmarkKey(value)) && (value.frontend.list?.trim() || "") === (item.frontend.list?.trim() || ""));
      const target = siblings[siblings.findIndex(value => value.identity === item.identity) + direction];
      if (target && reorderBookmark(item, target, direction > 0)) { closeDialog(); render(); }
      return;
    }
    if (action === "move-bookmark") {
      const item = findItem(actionElement.dataset.appKey);
      if (!item) return;
      openBookmarkFolderDialog(item);
      return;
    }
    if (action === "edit-container" || action === "edit-container-bookmark") {
      openContainerMenu(actionElement.dataset.safeSpaceId, actionElement.dataset.serviceId, actionElement.dataset.frontendKey);
      return;
    }
    if (action === "copy-container-url") {
      const workspace = findSafeSpace(actionElement.dataset.safeSpaceId);
      const app = findSafeSpaceApp(workspace, actionElement.dataset.serviceId, actionElement.dataset.frontendKey);
      if (app) { closeDialog(); await copyText(safeSpaceAppNavigationURL(workspace, app)); }
      return;
    }
    if (action === "container-control") {
      actionElement.disabled = true;
      try {
        await safeSpaceRequest(actionElement.dataset.operation, { workspaceID: actionElement.dataset.safeSpaceId, serviceID: actionElement.dataset.serviceId });
        closeDialog();
        await refreshSafeSpaces({ quiet: true });
      } catch (error) { toast(error.message || String(error), true); actionElement.disabled = false; }
      return;
    }
    if (action === "close-dialog") { closeDialog(); return; }
    if (action === "open-add") { openAddDialog(); return; }
    if (action === "start-safe-space") {
      try { await startSafeSpace(actionElement.dataset.safeSpaceId); }
      catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "launch-safe-space-app") {
      const workspace = findSafeSpace(actionElement.dataset.safeSpaceId);
      const app = findSafeSpaceApp(workspace, actionElement.dataset.serviceId, actionElement.dataset.frontendKey);
      if (workspace && app && safeSpaceAppIsReady(workspace, app)) return;
      event.preventDefault();
      try { await launchSafeSpaceApp(actionElement.dataset.safeSpaceId, actionElement.dataset.serviceId, actionElement.dataset.frontendKey); }
      catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "add-tab") { openAddDialog(actionElement.dataset.tab); return; }
    if (action === "about-outer-shell") { openOuterShellAbout(actionElement.dataset.backendKey); return; }
    if (action === "check-outer-shell-update") { await checkOuterShellUpdate(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "update-outer-shell") { await performOuterShellOperation(actionElement.dataset.backendKey, "update", actionElement, true); return; }
    if (action === "uninstall-outer-shell") { await uninstallOuterShell(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "toggle-menu-bar") {
      const operation = actionElement.dataset.enabled === "true" ? "hideMenuBarWhenRunning" : "showMenuBarWhenRunning";
      await performControl(actionElement.dataset.backendKey, operation, actionElement);
      return;
    }
    if (action === "launch-endpoint") {
      if (Date.now() < state.suppressLaunchUntil && !actionElement.closest(".context-menu")) {
        event.preventDefault();
        return;
      }
      const item = findItem(actionElement.dataset.appKey);
      const scope = actionElement.dataset.appScope;
      const endpoint = item?.[scope];
      if (endpoint && endpointReady(endpoint)) return;
      event.preventDefault();
      try { await launch(actionElement.dataset.appKey, scope); } catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "launch") {
      if (Date.now() < state.suppressLaunchUntil && !actionElement.closest(".context-menu")) {
        event.preventDefault();
        return;
      }
      const item = findItem(actionElement.dataset.appKey);
      if (item && endpointReady(item.primary)) return;
      event.preventDefault();
      try { await launch(actionElement.dataset.appKey); } catch (error) { toast(error.message || String(error), true); }
      return;
    }
    if (action === "install") { await installBackend(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "control") { await performControl(actionElement.dataset.backendKey, actionElement.dataset.operation, actionElement); return; }
    if (action === "uninstall") { await uninstallBackend(actionElement.dataset.backendKey, actionElement); return; }
    if (action === "copy-script") { const script = actionElement.dataset.script; closeDialog(); await copyText(script); return; }
    if (action === "logs") { openLogs(actionElement.dataset.backendKey); return; }
    if (action === "refresh-log") { await fetchSelectedLog(); }
  });
  document.body.addEventListener("change", event => {
    if (event.target.id !== "log-select" || !state.logSelection) return;
    state.logSelection.index = Number(event.target.value) || 0;
    state.logVersion = 0n;
    restartEventWatch();
    fetchSelectedLog();
  });
  document.addEventListener("keydown", event => {
    if (event.key === "Escape" && elements.dialogLayer.childElementCount) closeDialog();
    if (event.key === "ContextMenu" || (event.shiftKey && event.key === "F10")) {
      const identity = itemIdentityFromTarget(event.target);
      if (!identity) return;
      event.preventDefault();
      const bounds = event.target.getBoundingClientRect();
      openAppMenu(identity, { x: bounds.left, y: bounds.bottom });
    }
  });
  window.addEventListener("beforeunload", suspendRefreshes);
  window.addEventListener("pagehide", () => { cancelAppDrag(); suspendRefreshes(); });
  window.addEventListener("pageshow", resumeRefreshes);
  window.addEventListener("online", () => { suspendRefreshes(); resumeRefreshes(); });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) suspendRefreshes();
    else resumeRefreshes();
  });
  state.safeSpacesTimer = window.setInterval(() => {
    if (document.hidden || state.suspended) return;
    if (!state.safeSpaceBusy.size) refreshSafeSpaces({ quiet: true });
    if (state.backendError || state.refreshErrorTimers.backendError) refreshBackends({ quiet: true });
  }, 2000);

  Promise.all([refreshBackends(), refreshSafeSpaces()]).then(watchEvents);
})();
