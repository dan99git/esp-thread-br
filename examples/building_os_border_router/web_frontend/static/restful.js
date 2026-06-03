const SITE_SERVER_STORAGE_KEY = "bos.br.siteServerBase";

const state = {
  selectedNetwork: null,
  selectedTorrentId: null,
  torrentFilter: "all",
  torrentDetailTab: "general",
  bosBase: location.origin,
  siteServerBase: loadSetting(SITE_SERVER_STORAGE_KEY),
  siteApiKey: "",
  adminPassword: null,
  last: {
    status: null,
    ledger: null,
    torrent: null,
    convergence: null,
    peers: null,
    thread: null,
    siteEstate: null,
  },
};

const $ = (id) => document.getElementById(id);

function toast(message) {
  const el = $("toast");
  el.textContent = message;
  el.classList.add("show");
  clearTimeout(el._timer);
  el._timer = setTimeout(() => el.classList.remove("show"), 2800);
}

function adminHeaders() {
  if (!state.adminPassword) {
    state.adminPassword = window.prompt("BR admin password");
  }
  if (!state.adminPassword) throw new Error("Admin password required");
  return { "X-BOS-Admin-Password": state.adminPassword };
}

async function api(path, options = {}, admin = false) {
  const headers = {
    "Content-Type": "application/json",
    ...(options.headers || {}),
    ...(admin ? adminHeaders() : {}),
  };
  const res = await fetch(path, {
    ...options,
    headers,
  });
  if (!res.ok && admin && res.status === 401) state.adminPassword = null;
  if (!res.ok) throw new Error(`${path} returned ${res.status}`);
  return res.json();
}

async function bos(path) {
  const res = await fetch(`${state.bosBase}${path}`);
  if (!res.ok) throw new Error(`${path} returned ${res.status}`);
  return res.json();
}

function loadSetting(key) {
  try {
    return window.localStorage.getItem(key) || "";
  } catch {
    return "";
  }
}

function saveSetting(key, value) {
  try {
    if (value) window.localStorage.setItem(key, value);
    else window.localStorage.removeItem(key);
  } catch {
    // Ignore storage failures; the live read path still works for this session.
  }
}

function normaliseBaseUrl(value) {
  const raw = String(value ?? "").trim();
  if (!raw) return "";
  const withScheme = /^https?:\/\//i.test(raw) ? raw : `http://${raw}`;
  return new URL(withScheme).origin;
}

function siteHeaders() {
  return state.siteApiKey ? { "x-api-key": state.siteApiKey } : {};
}

async function siteFetchResult(path) {
  if (!state.siteServerBase) {
    return { ok: false, url: path, error: "site server not configured" };
  }
  const url = `${state.siteServerBase}${path}`;
  const headers = siteHeaders();
  const options = Object.keys(headers).length > 0 ? { headers } : {};
  try {
    const res = await fetch(url, options);
    if (!res.ok) return { ok: false, url, status: res.status, error: `HTTP ${res.status}` };
    return { ok: true, url, data: await res.json() };
  } catch (err) {
    return { ok: false, url, error: err instanceof Error ? err.message : String(err) };
  }
}

function escapeHtml(value) {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function setText(id, value) {
  const el = $(id);
  if (el) el.textContent = value ?? "unknown";
}

function tableCell(value) {
  return `<td>${escapeHtml(value ?? "unknown")}</td>`;
}

function numeric(value, fallback = 0) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function percent(value) {
  return Math.max(0, Math.min(100, numeric(value)));
}

function formatBytes(value) {
  const n = numeric(value);
  if (n <= 0) return "0 b";
  const units = ["b", "KB", "MB", "GB"];
  let size = n;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size >= 10 || unit === 0 ? size.toFixed(0) : size.toFixed(1)} ${units[unit]}`;
}

function formatRate(value) {
  const n = numeric(value);
  return n <= 0 ? "0.0 KB/s" : `${formatBytes(n)}/s`;
}

function formatEta(value) {
  if (value == null || !Number.isFinite(Number(value))) return "-";
  const seconds = Math.max(0, Math.round(Number(value) / 1000));
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  const rem = seconds % 60;
  if (minutes < 60) return `${minutes}m ${rem}s`;
  const hours = Math.floor(minutes / 60);
  return `${hours}h ${minutes % 60}m`;
}

function formatDuration(value) {
  if (value == null || !Number.isFinite(Number(value))) return "-";
  const seconds = Math.max(0, Math.round(Number(value) / 1000));
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ${seconds % 60}s`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `${hours}h ${minutes % 60}m`;
  const days = Math.floor(hours / 24);
  return `${days}d ${hours % 24}h`;
}

function titleCase(value) {
  return String(value ?? "")
    .replace(/[-_]/g, " ")
    .replace(/\b[a-z]/g, (char) => char.toUpperCase());
}

function shortDigest(value) {
  const digest = String(value ?? "");
  if (!digest || digest === "null") return "no digest";
  if (digest.length <= 22) return digest;
  return `${digest.slice(0, 12)}...${digest.slice(-6)}`;
}

function progressCell(value) {
  const pct = percent(value);
  return `<td class="progress-cell"><div class="progress-track mini"><div class="progress-fill" style="width:${pct}%"></div><span>${pct.toFixed(1)}%</span></div></td>`;
}

function emptyRow(target, colspan, message) {
  const el = $(target);
  if (el) el.innerHTML = `<tr><td colspan="${colspan}">${escapeHtml(message)}</td></tr>`;
}

function formJson(form) {
  const out = {};
  new FormData(form).forEach((value, key) => { out[key] = value; });
  if ("defaultRoute" in out) out.defaultRoute = 1;
  else out.defaultRoute = 0;
  if ("channel" in out) out.channel = Number(out.channel);
  return out;
}

function routeForTab(name) {
  return name === "developer" ? "docs/developer" : name;
}

function tabFromLocation() {
  const path = location.pathname.replace(/\/+$/, "");
  if (path === "/docs/developer") return "developer";
  const hash = location.hash.replace(/^#/, "");
  if (hash === "docs/developer") return "developer";
  return hash || "overview";
}

function showTab(name, updateHash = true) {
  const target = $(`tab-${name}`) ? name : "overview";
  document.querySelectorAll(".nav-item").forEach((button) => {
    button.classList.toggle("active", button.dataset.tab === target);
  });
  document.querySelectorAll(".tab").forEach((tab) => {
    tab.classList.toggle("active", tab.id === `tab-${target}`);
  });
  if (updateHash) history.replaceState(null, "", `#${routeForTab(target)}`);
}

function renderKv(target, entries) {
  const el = $(target);
  if (!el) return;
  el.innerHTML = entries.map(([label, value]) =>
    `<div class="kv"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value ?? "unknown")}</strong></div>`
  ).join("");
}

function decodeThreadProperties(payload) {
  const r = payload?.result ?? {};
  state.last.thread = r;
  setText("metric-role", r["RCP:State"] ?? "unknown");
  setText("metric-network", r["Network:Name"] ?? "network pending");
  renderKv("thread-summary", [
    ["Link local", r["IPv6:LinkLocalAddress"]],
    ["Routing local", r["IPv6:RoutingLocalAddress"]],
    ["Mesh local", r["IPv6:MeshLocalAddress"]],
    ["Network", r["Network:Name"]],
    ["PAN ID", r["Network:PANID"]],
    ["Partition", r["Network:PartitionID"]],
    ["XPAN ID", r["Network:XPANID"]],
    ["RCP channel", r["RCP:Channel"]],
    ["RCP version", r["RCP:Version"]],
  ]);
  renderNetworkSummary();
}

async function refreshThread(options = {}) {
  const data = await api("/get_properties");
  decodeThreadProperties(data);
  if (options.notify) toast("Thread status refreshed");
  return data;
}

function normalisePeer(peer) {
  const p = peer ?? {};
  const testPeer = isTmfsTestPeer(p);
  return {
    raw: p,
    hardware: p.hardware_id ?? p.eui64 ?? p.transport_id ?? p.node_id ?? "unknown",
    modelId: p.model_id ?? p.modelId ?? p.product_model_id ?? "",
    transport: p.address ?? p.thread_address ?? p.ipv6 ?? p.ip ?? p.rloc16 ?? "unknown",
    state: p.commissioning_state ?? p.phase ?? p.state ?? (p.commissioned ? "registered" : "floating"),
    source: p.source ?? "srp",
    plannedMatch: p.planned_match ?? p.expected_position ?? p.spatial_id ?? "",
    ready: p.pairing_ready === true || p.match_state === "pairing-ready" || p.match === "exact",
    commissioned: p.commissioned === true || p.registered === true,
    testPeer,
    holdbackReason: p.holdback_reason ?? p.reason ?? "",
  };
}

function peerHoldbackReason(peer) {
  if (peer.ready || peer.commissioned) return "";
  if (peer.testPeer) return "torrent test peer";
  if (!peer.modelId) return "missing model_id";
  return peer.holdbackReason || "not matched to active site package by this BR firmware";
}

function isRecord(value) {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function records(value) {
  return Array.isArray(value) ? value.filter(isRecord) : [];
}

function resultRecords(result, key = null) {
  if (!result?.ok) return [];
  if (!key) return records(result.data);
  return isRecord(result.data) ? records(result.data[key]) : [];
}

function resultRecord(result, key = null) {
  if (!result?.ok || !isRecord(result.data)) return null;
  if (!key) return result.data;
  return isRecord(result.data[key]) ? result.data[key] : null;
}

function stringValue(value) {
  return typeof value === "string" && value !== "" ? value : null;
}

function firstString(...values) {
  for (const value of values) {
    const raw = stringValue(value);
    if (raw) return raw;
  }
  return null;
}

function hasToken(value, token) {
  return String(value ?? "")
    .split(",")
    .map((part) => part.trim().toLowerCase())
    .includes(token);
}

function isTmfsTestPeer(peer) {
  const p = peer?.raw ?? peer ?? {};
  if (p.test_peer === true || p.testPeer === true) return true;
  const caps = firstString(p.tmfs_caps, p.caps);
  const service = isRecord(p.service) ? p.service : {};
  const hasTmfsSignal = p.tmfs_caps != null || service.tmfs === true || firstString(p.tmfs_catalog, p.cat) != null;
  return hasTmfsSignal && hasToken(caps, "bare-test");
}

function lowerKey(value) {
  const raw = firstString(value);
  return raw ? raw.toLowerCase() : null;
}

function brDeviceId(status = state.last.status) {
  return firstString(
    status?.device_id,
    status?.registration?.device_id,
    status?.border_router?.device_id,
    status?.device?.id
  );
}

function propsOf(record) {
  return isRecord(record?.properties) ? record.properties : {};
}

function peerKeys(peer) {
  return [
    peer?.id,
    peer?.transport_id,
    peer?.node_id,
    peer?.hardware_id,
    peer?.eui64,
    peer?.ml_eid,
    peer?.thread_address,
    peer?.address,
    peer?.hostname,
    peer?.name,
  ].map(lowerKey).filter(Boolean);
}

function deviceKeys(device) {
  const props = propsOf(device);
  return [
    device?.id,
    device?.device_id,
    device?.hardware_id,
    props.device_id,
    props.hardware_id,
    props.eui64,
    props.thread_address,
    props.address,
    props.hostname,
  ].map(lowerKey).filter(Boolean);
}

function discoveredKeys(device) {
  return [
    device?.transport_id,
    device?.node_id,
    device?.eui64,
    device?.hardware_id,
    device?.id,
    device?.hostname,
    device?.address,
    device?.thread_address,
    device?.name,
  ].map(lowerKey).filter(Boolean);
}

function indexByKeys(items, keyFn) {
  const index = new Map();
  records(items).forEach((item) => {
    keyFn(item).forEach((key) => {
      if (!index.has(key)) index.set(key, item);
    });
  });
  return index;
}

function matchByKeys(index, keys) {
  for (const key of keys) {
    const found = index.get(key);
    if (found) return found;
  }
  return null;
}

function catalogParts(source, fallback) {
  const sourceProps = propsOf(source);
  const fallbackProps = propsOf(fallback);
  return {
    manufacturer: firstString(
      source?.manufacturer,
      sourceProps.manufacturer,
      fallback?.manufacturer,
      fallbackProps.manufacturer
    ),
    product: firstString(
      source?.product_name,
      source?.product,
      source?.model,
      sourceProps.product_name,
      sourceProps.product,
      sourceProps.model,
      fallback?.product_name,
      fallback?.product,
      fallback?.model,
      fallbackProps.product_name,
      fallbackProps.product,
      fallbackProps.model
    ),
  };
}

function buildSiteEstateIndex(siteEstate) {
  const devices = records(siteEstate?.devices);
  const discovered = records(siteEstate?.discovered);
  return {
    devices,
    discovered,
    devicesByKey: indexByKeys(devices, deviceKeys),
    discoveredByKey: indexByKeys(discovered, discoveredKeys),
  };
}

function estateMatchForPeer(peer, siteEstateIndex) {
  const keys = peerKeys(peer);
  return {
    device: matchByKeys(siteEstateIndex.devicesByKey, keys),
    discovered: matchByKeys(siteEstateIndex.discoveredByKey, keys),
  };
}

function enrichPeerWithSite(peer, siteEstateIndex) {
  const p = peer ?? {};
  const match = estateMatchForPeer(p, siteEstateIndex);
  const device = match.device;
  const discovered = match.discovered;
  const deviceProps = propsOf(device);
  const catalog = catalogParts(discovered ?? p, device ?? p);
  const registered = device != null || p.registered === true || p.commissioned === true;
  const spatialId = registered
    ? firstString(device?.spatial_id, deviceProps.spatial_id, p.spatial_id, p.spatialId)
    : null;

  return {
    ...p,
    site_device_id: firstString(device?.id, device?.device_id),
    site_source: device ? "server-devices" : discovered ? "server-discovered" : null,
    hardware_id: firstString(
      p.hardware_id,
      device?.hardware_id,
      deviceProps.hardware_id,
      discovered?.hardware_id,
      discovered?.eui64,
      p.eui64
    ),
    transport_id: firstString(
      p.transport_id,
      p.node_id,
      p.nid,
      p.id,
      p.ml_eid,
      p.thread_address,
      p.address,
      p.hostname,
      p.name
    ),
    node_id: firstString(p.node_id, p.nid, p.id, p.ml_eid),
    manufacturer: firstString(p.manufacturer, catalog.manufacturer),
    product_name: firstString(p.product_name, catalog.product),
    model_id: firstString(p.model_id, discovered?.model_id, device?.model_id, deviceProps.model_id),
    spatial_id: spatialId,
    spatial_label: registered
      ? firstString(p.spatial_label, device?.name, deviceProps.spatial_label, deviceProps.space, spatialId)
      : null,
    planned_position: firstString(p.planned_position, discovered?.planned_position, discovered?.expected_position),
    planned_match: firstString(p.planned_match, discovered?.planned_match, discovered?.expected_position),
    match_state: firstString(p.match_state, discovered?.match_state, p.match, discovered?.match),
    pairing_ready: p.pairing_ready === true || discovered?.pairing_ready === true ||
      p.match_state === "pairing-ready" || discovered?.match_state === "pairing-ready" ||
      p.match === "exact" || discovered?.match === "exact",
    holdback_reason: firstString(p.holdback_reason, discovered?.holdback_reason, p.reason, discovered?.reason),
    estate_state: registered
      ? "registered"
      : firstString(p.estate_state, p.registration_state, p.device_state, discovered?.estate_state),
    registered,
    commissioned: registered,
    source: firstString(p.source, device ? "server-devices" : discovered ? "server-discovered" : "srp"),
  };
}

function siteOnlyPeers(siteEstateIndex, rawPeers) {
  const seen = indexByKeys(records(rawPeers), peerKeys);
  return siteEstateIndex.discovered
    .filter((device) => !matchByKeys(seen, discoveredKeys(device)))
    .map((device) => ({
      id: firstString(device.id, device.eui64, device.hardware_id, device.hostname, device.name),
      hardware_id: firstString(device.hardware_id, device.eui64),
      transport_id: firstString(device.transport_id, device.eui64, device.hostname, device.address, device.thread_address),
      model_id: firstString(device.model_id),
      manufacturer: firstString(device.manufacturer),
      product_name: firstString(device.product_name, device.product, device.model),
      address: firstString(device.address, device.thread_address),
      hostname: firstString(device.hostname),
      state: firstString(device.state, device.commissioning_state, "floating"),
      source: "site-server",
      pairing_ready: device.pairing_ready === true,
      match_state: firstString(device.match_state, device.match),
      planned_match: firstString(device.planned_match, device.expected_position),
      holdback_reason: firstString(device.holdback_reason, device.reason),
    }));
}

function siteEstateStateClass(siteEstate) {
  if (!siteEstate?.configured) return "warn";
  return siteEstate.state === "connected" ? "ok" : "warn";
}

function siteInfrastructure(siteEstate) {
  return resultRecord(siteEstate?.results?.serverPlacement, "infrastructure");
}

function siteServerConvergence(siteEstate) {
  return resultRecord(siteEstate?.results?.serverConvergence);
}

function brPlacementSummary(infrastructure) {
  if (!isRecord(infrastructure)) return "";
  return [
    firstString(infrastructure.site_id),
    firstString(infrastructure.building_id),
    firstString(infrastructure.floor_id),
    firstString(infrastructure.network_segment),
    firstString(infrastructure.cabinet),
  ].filter(Boolean).join(" / ");
}

function setBadge(id, value, stateClass = "") {
  const el = $(id);
  if (!el) return;
  el.textContent = value ?? "unknown";
  el.className = ["badge", stateClass].filter(Boolean).join(" ");
}

function renderSiteEstateState(siteEstate) {
  const configured = siteEstate?.configured === true;
  const deviceResult = siteEstate?.results?.devices;
  const discoveredResult = siteEstate?.results?.discovered;
  const placementResult = siteEstate?.results?.serverPlacement;
  const convergenceResult = siteEstate?.results?.serverConvergence;
  const placement = siteInfrastructure(siteEstate);
  const brId = siteEstate?.brDeviceId || brDeviceId();
  const registryState = !configured
    ? "not configured"
    : deviceResult?.ok
      ? `${siteEstate.devices.length} registered`
      : `unavailable: ${deviceResult?.error ?? "read failed"}`;
  const discoveredState = !configured
    ? "not configured"
    : discoveredResult?.ok
      ? `${siteEstate.discovered.length} discovered`
      : `unavailable: ${discoveredResult?.error ?? "read failed"}`;
  const placementState = !configured
    ? "not configured"
    : !brId
      ? "BR device id unavailable"
      : placementResult?.ok
        ? (brPlacementSummary(placement) || "placement present")
        : `unavailable: ${placementResult?.error ?? "read failed"}`;
  const convergenceState = !configured
    ? "not configured"
    : !brId
      ? "BR device id unavailable"
      : convergenceResult?.ok
        ? (siteServerConvergence(siteEstate)?.freshness?.state ?? siteServerConvergence(siteEstate)?.state ?? "stored")
        : `unavailable: ${convergenceResult?.error ?? "read failed"}`;
  const source = configured ? `${siteEstate.serverBase} ${siteEstate.state}` : "not configured";

  setBadge("site-estate-state", configured ? siteEstate.state : "not configured", siteEstateStateClass(siteEstate));
  setText("devices-registry-state", registryState);
  setText("devices-discovered-state", discoveredState);
  setText("devices-br-placement-state", placementState);
  setText("devices-br-convergence-state", convergenceState);
  setText("site-estate-source", source);
  setText("torrent-site-source", configured ? `site ${siteEstate.state}${brId ? ` | ${brId}` : ""}` : "site server not configured");
}

async function refreshSiteEstate(status = state.last.status) {
  const brId = brDeviceId(status);
  if (!state.siteServerBase) {
    const unconfigured = {
      configured: false,
      serverBase: "",
      state: "not configured",
      brDeviceId: brId,
      devices: [],
      discovered: [],
      results: {},
    };
    state.last.siteEstate = unconfigured;
    renderSiteEstateState(unconfigured);
    return unconfigured;
  }

  const noBrId = { ok: false, url: "", error: "BR device id unavailable" };
  const [devices, discovered, serverPlacement, serverConvergence] = await Promise.all([
    siteFetchResult("/api/devices"),
    siteFetchResult("/api/commissioning/discovered"),
    brId ? siteFetchResult(`/api/border-routers/${encodeURIComponent(brId)}/infrastructure`) : noBrId,
    brId ? siteFetchResult(`/api/border-routers/${encodeURIComponent(brId)}/convergence`) : noBrId,
  ]);
  const errors = [devices, discovered, serverPlacement, serverConvergence].filter((result) => !result.ok);
  const siteEstate = {
    configured: true,
    serverBase: state.siteServerBase,
    brDeviceId: brId,
    state: errors.length === 0 ? "connected" : errors.length === 4 ? "unavailable" : "partial",
    devices: resultRecords(devices),
    discovered: resultRecords(discovered, "devices"),
    results: { devices, discovered, serverPlacement, serverConvergence },
    error: errors.map((result) => result.error).filter(Boolean).join("; "),
  };
  state.last.siteEstate = siteEstate;
  renderSiteEstateState(siteEstate);
  return siteEstate;
}

function renderCommissioning(peersPayload, siteEstate = state.last.siteEstate) {
  const siteEstateIndex = buildSiteEstateIndex(siteEstate);
  const rawPeers = records(peersPayload?.peers);
  const peers = rawPeers
    .concat(siteOnlyPeers(siteEstateIndex, rawPeers))
    .map((peer) => normalisePeer(enrichPeerWithSite(peer, siteEstateIndex)));
  const ready = peers.filter((p) => p.ready && !p.commissioned);
  const floating = peers.filter((p) => !p.commissioned);
  const heldBack = floating.filter((p) => !p.ready && !p.testPeer);
  const source = peersPayload?.source ?? "srp";
  const feedState = peersPayload?.state ?? (peersPayload?.error ? "unavailable" : "pending");
  const note = peersPayload?.note ?? peersPayload?.error ?? "SRP peer feed pending";
  const siteNote = siteEstate?.configured
    ? `${siteEstate.devices.length} registered, ${siteEstate.discovered.length} discovered from ${siteEstate.serverBase}`
    : "Set a site server on Devices to enrich registered and floating estate identity";

  setText("site-package-state", siteEstate?.configured ? siteEstate.state : "not configured");
  setText("site-package-detail", siteEstate?.error || siteNote);
  setText("pairing-ready-count", String(ready.length));
  setText("floating-count", String(floating.length));
  setText("floating-source", siteEstate?.configured ? `${source} ${feedState} + site estate` : `${source} ${feedState}`);
  setText("holdback-count", String(heldBack.length));
  setText("metric-floating", String(floating.length));
  setText("metric-floating-detail", note);
  setText("peers-source-badge", `/${source} ${feedState}`);
  setText("devices-peer-state", `${feedState}${peersPayload?.error ? " error" : ""}`);
  renderSiteEstateState(siteEstate);

  if (ready.length === 0) {
    emptyRow("commissioning-ready", 5, "No pairing-ready candidates reported.");
  } else {
    $("commissioning-ready").innerHTML = ready.map((p) =>
      `<tr>${tableCell(p.hardware)}${tableCell(p.modelId)}${tableCell(p.plannedMatch || "exact match reported")}${tableCell("identify then bind")}${tableCell(p.source)}</tr>`
    ).join("");
  }

  if (floating.length === 0) {
    emptyRow("floating-devices", 5, note);
  } else {
    $("floating-devices").innerHTML = floating.map((p) =>
      `<tr>${tableCell(p.hardware)}${tableCell(p.modelId || "missing")}${tableCell(p.transport)}${tableCell(p.state)}${tableCell(peerHoldbackReason(p) || "pairing-ready")}</tr>`
    ).join("");
  }

  if (heldBack.length === 0) {
    emptyRow("holdback-devices", 4, "No holdbacks reported.");
  } else {
    $("holdback-devices").innerHTML = heldBack.map((p) =>
      `<tr>${tableCell(p.hardware)}${tableCell(p.modelId || "missing")}${tableCell(peerHoldbackReason(p))}${tableCell(p.modelId ? "activate package match or inspect surplus hardware" : "fix device package identity before pairing")}</tr>`
    ).join("");
  }

  renderKv("device-feed-summary", [
    ["Feed", "/bos/peers"],
    ["Source", source],
    ["State", feedState],
    ["Site server", siteEstate?.configured ? siteEstate.serverBase : "not configured"],
    ["Registered registry", siteEstate?.configured ? siteEstate.devices.length : 0],
    ["Site discovered", siteEstate?.configured ? siteEstate.discovered.length : 0],
    ["Floating count", floating.length],
    ["Pairing-ready count", ready.length],
    ["Note", note],
  ]);
}

function renderNetworkSummary() {
  const status = state.last.status;
  const thread = state.last.thread ?? {};
  renderKv("network-summary", [
    ["Backbone", status ? (status.backbone?.connected ? "online" : "waiting") : "unknown"],
    ["Interface", status?.backbone?.interface ?? "unknown"],
    ["IPv4", status?.backbone?.ipv4 || "none"],
    ["IPv6", status?.backbone?.ipv6 || "none"],
    ["Thread network", thread["Network:Name"] ?? "unknown"],
    ["Thread role", thread["RCP:State"] ?? status?.thread?.role ?? "unknown"],
    ["Channel", thread["RCP:Channel"] ?? "unknown"],
    ["PAN ID", thread["Network:PANID"] ?? "unknown"],
    ["XPAN ID", thread["Network:XPANID"] ?? "unknown"],
  ]);
}

function normaliseTorrentRows(torrent, ledger, convergence, siteEstate = state.last.siteEstate) {
  const active = !ledger?.error && ledger?.present;
  const target = torrent?.target || convergence?.target || {};
  const siteEstateIndex = buildSiteEstateIndex(siteEstate);
  const placement = siteInfrastructure(siteEstate);
  const serverConvergence = siteServerConvergence(siteEstate);
  const placementSummary = brPlacementSummary(placement);
  const knownBrId = siteEstate?.brDeviceId || brDeviceId();
  const rawSelf = torrent?.self || {
    id: "border-router",
    name: active ? `bos-ledger-v${ledger.ledger_version}` : "border-router",
    estate_state: "infrastructure",
    role: "seed",
    state: active ? "seedable" : "waiting",
    ledger_version: active ? ledger.ledger_version : null,
    manifest_digest: active ? ledger.manifest_digest : null,
    chunks_have: active ? (ledger.chunk_count ?? 0) : 0,
    chunks_total: active ? (ledger.chunk_count ?? 0) : 0,
    progress_pct: active ? 100 : 0,
    rate_bps: 0,
    eta_ms: null,
  };
  const self = {
    ...rawSelf,
    id: knownBrId || rawSelf.id || "border-router",
    hardware_id: knownBrId || rawSelf.hardware_id || rawSelf.id || "border-router",
    name: rawSelf.name === "border-router" || !rawSelf.name ? "border-router" : rawSelf.name,
    estate_state: "infrastructure",
    infrastructure_summary: placementSummary,
    site_id: placement?.site_id ?? rawSelf.site_id ?? null,
    building_id: placement?.building_id ?? rawSelf.building_id ?? null,
    floor_id: placement?.floor_id ?? rawSelf.floor_id ?? null,
    upstream: placement?.upstream ?? rawSelf.upstream ?? null,
    network_segment: placement?.network_segment ?? rawSelf.network_segment ?? null,
    cabinet: placement?.cabinet ?? rawSelf.cabinet ?? null,
    control_port: placement?.control_port ?? rawSelf.control_port ?? 80,
    diagnostics_port: placement?.diagnostics_port ?? rawSelf.diagnostics_port ?? null,
    site_convergence_state: serverConvergence?.freshness?.state ?? serverConvergence?.state ?? null,
    site_convergence_updated_at: serverConvergence?.updated_at ?? serverConvergence?.convergence?.updated_at ?? null,
  };
  const rawPeers = Array.isArray(torrent?.peers) ? torrent.peers : records(convergence?.peers);
  const peers = rawPeers.concat(siteOnlyPeers(siteEstateIndex, rawPeers));
  return [
    {
      ...self,
      id: self.id || "border-router",
      name: self.name || self.id || "border-router",
      estate_state: self.estate_state || "infrastructure",
      queue_position: 0,
      priority: "normal",
    },
    ...peers.map((peer, index) => {
      const p = enrichPeerWithSite(peer, siteEstateIndex);
      const testPeer = isTmfsTestPeer(p);
      const transportId = firstString(
        p.transport_id,
        p.node_id,
        p.ml_eid,
        p.thread_address,
        p.address,
        p.hostname
      );
      const rowKey = firstString(p.site_device_id, p.id, p.hardware_id, transportId) || `transport-row-${index + 1}`;
      return {
        ...p,
        id: rowKey,
        transport_id: transportId,
        name: firstString(p.name, p.product_name, p.spatial_label, p.hardware_id, testPeer ? "TMFS test peer" : null, transportId),
        hardware_id: p.hardware_id ?? null,
        test_peer: p.test_peer === true || testPeer,
        tmfs_catalog: firstString(p.tmfs_catalog, p.cat),
        tmfs_caps: firstString(p.tmfs_caps, p.caps),
        spatial_id: p.spatial_id ?? null,
        spatial_label: p.spatial_label ?? p.site_label ?? p.position_label ?? p.space_label ?? null,
        planned_position: p.planned_position ?? p.expected_position ?? null,
        planned_match: p.planned_match ?? p.expected_position ?? p.match_position ?? null,
        match_state: p.match_state ?? p.match ?? null,
        pairing_ready: p.pairing_ready === true || p.match_state === "pairing-ready" || p.match === "exact",
        holdback_reason: p.holdback_reason ?? p.reason ?? null,
        estate_state: p.estate_state ?? p.registration_state ?? p.device_state ?? null,
        commissioned: p.commissioned === true || p.registered === true,
        role: p.role || "peer",
        state: p.transfer_state || p.state || "unknown",
        queue_position: index + 1,
        priority: p.priority || "normal",
        progress_pct: p.progress_pct ?? (
          numeric(p.chunks_total) > 0 ? (numeric(p.chunks_have) / numeric(p.chunks_total)) * 100 : 0
        ),
      };
    }),
  ].map((row) => ({
    ...row,
    bytes_total: row.bytes_total ?? target.bytes_total ?? ledger?.size_bytes ?? 0,
    chunk_size: row.chunk_size ?? target.chunk_size ?? ledger?.chunk_size ?? 0,
    chunks_total: row.chunks_total ?? target.chunks_total ?? ledger?.chunk_count ?? 0,
    manifest_digest: row.manifest_digest ?? target.manifest_digest ?? ledger?.manifest_digest ?? null,
    ledger_version: row.ledger_version ?? target.ledger_version ?? ledger?.ledger_version ?? null,
  }));
}

function torrentEstateState(row) {
  const explicit = String(row.estate_state ?? "").toLowerCase();
  if (explicit === "infrastructure" || row.id === "border-router" || row.role === "border-router") return "infrastructure";
  if (explicit === "registered" || explicit === "paired" || row.commissioned === true || row.registered === true || row.spatial_id) return "registered";
  return "floating";
}

function torrentCatalogLabel(row) {
  if (isTmfsTestPeer(row)) return "TMFS test peer";
  const manufacturer = row.manufacturer || row.make || "";
  const product = row.product_name || row.product || row.model || row.model_id || "";
  const label = [manufacturer, product].filter(Boolean).join(" ");
  if (label) return label;
  if (row.id === "border-router" || torrentEstateState(row) === "infrastructure") return row.name || "border-router";
  return "";
}

function torrentHoldbackReason(row) {
  const estateState = torrentEstateState(row);
  if (estateState !== "floating") return "";
  if (isTmfsTestPeer(row)) return "";
  const explicit = row.holdback_reason || row.reason;
  if (explicit) return explicit;
  const matchState = String(row.match_state ?? "").toLowerCase();
  if (["mismatch", "missing", "surplus", "ineligible", "blocked", "double-bound", "class-mismatch"].includes(matchState)) {
    return matchState;
  }
  if (!torrentCatalogLabel(row)) return "missing_catalog_identity";
  if (!row.model_id) return "missing_model_id";
  if (row.pairing_ready !== true) return "not matched to an unpaired planned fitting";
  return "";
}

function torrentPairingState(row) {
  const estateState = torrentEstateState(row);
  if (estateState === "infrastructure") return "infrastructure";
  if (estateState === "registered") return "registered";
  if (isTmfsTestPeer(row)) return "floating";
  if (torrentHoldbackReason(row)) return "ineligible";
  if (row.pairing_ready === true) return "pairing-ready";
  return "floating";
}

function torrentRowKind(row) {
  const estateState = torrentEstateState(row);
  if (estateState === "infrastructure") return "br_seed";
  if (estateState === "registered") return "registered_device";
  if (torrentPairingState(row) === "ineligible") return "ineligible_device";
  return "floating_device";
}

function torrentRowName(row) {
  const estateState = torrentEstateState(row);
  const catalog = torrentCatalogLabel(row);
  if (catalog) return catalog;
  if (torrentPairingState(row) === "ineligible") return titleCase(torrentHoldbackReason(row));
  if (estateState === "registered") {
    return row.name || row.spatial_label || row.hardware_id || row.transport_id || "registered device";
  }
  if (estateState === "infrastructure") {
    return row.name || "border-router";
  }
  return row.hardware_id || row.transport_id || row.name || "floating device";
}

function torrentRowSubtitle(row) {
  const estateState = torrentEstateState(row);
  const digest = shortDigest(row.manifest_digest);
  if (estateState === "registered") {
    const hardware = row.hardware_id || row.transport_id || "hardware unknown";
    const spatial = row.spatial_label || row.spatial_id || "spatial linked";
    return `registered | ${spatial} | ${hardware} | v${row.ledger_version ?? "-"} ${digest}`;
  }
  if (estateState === "infrastructure") {
    const placement = row.infrastructure_summary ? `${row.infrastructure_summary} | ` : "";
    return `infrastructure | ${placement}v${row.ledger_version ?? "-"} ${digest}`;
  }
  const pairingState = torrentPairingState(row);
  const peerHandle = row.hardware_id || row.transport_id || "transport identity unknown";
  if (isTmfsTestPeer(row)) {
    return `torrent test peer | ${peerHandle} | v${row.ledger_version ?? "-"} ${digest}`;
  }
  if (pairingState === "ineligible") {
    return `ineligible: ${torrentHoldbackReason(row)} | ${peerHandle}`;
  }
  if (pairingState === "pairing-ready") {
    return `pairing-ready | ${row.planned_match || "planned match"} | ${peerHandle}`;
  }
  return `floating | ${peerHandle} | v${row.ledger_version ?? "-"} ${digest}`;
}

function torrentRowDone(row) {
  if (row.progress_pct != null) return percent(row.progress_pct);
  const total = numeric(row.chunks_total);
  return total > 0 ? percent((numeric(row.chunks_have) / total) * 100) : 0;
}

function torrentRowStatus(row) {
  const raw = String(row.transfer_state ?? row.state ?? row.status ?? row.role ?? "").toLowerCase();
  const done = torrentRowDone(row);
  if (raw.includes("fail") || raw.includes("error")) return "Error";
  if (raw.includes("offline")) return "Offline";
  if (raw.includes("stale")) return "Stale";
  if (raw.includes("leech") || raw.includes("download")) return "Leeching";
  if (raw.includes("seed") || raw === "current" || done >= 100) return "Seeding";
  if (raw.includes("wait") || raw.includes("initial")) return "Waiting";
  return raw ? titleCase(raw) : "Unknown";
}

function torrentRowClass(row) {
  const status = torrentRowStatus(row).toLowerCase();
  if (status === "error") return "error";
  if (status === "offline") return "offline";
  if (status === "stale") return "stale";
  if (status === "leeching") return "leeching";
  if (status === "seeding") return "seeding";
  if (status === "waiting") return "waiting";
  return "unknown";
}

function torrentBuckets(row) {
  const buckets = new Set(["all"]);
  const statusClass = torrentRowClass(row);
  const done = torrentRowDone(row);
  const estateState = torrentEstateState(row);
  const pairingState = torrentPairingState(row);
  buckets.add(estateState);
  buckets.add(pairingState);
  if (pairingState === "ineligible") buckets.add("holdback");
  if (statusClass === "leeching") buckets.add("downloading");
  if (statusClass === "seeding" || done >= 100) buckets.add("completed");
  if (statusClass === "stale") buckets.add("stale");
  if (statusClass === "offline") buckets.add("offline");
  if (statusClass === "error") buckets.add("error");
  return buckets;
}

function countTorrentBuckets(rows) {
  const counts = {
    all: rows.length,
    infrastructure: 0,
    registered: 0,
    floating: 0,
    "pairing-ready": 0,
    holdback: 0,
    ineligible: 0,
    downloading: 0,
    completed: 0,
    stale: 0,
    offline: 0,
    error: 0,
  };
  rows.forEach((row) => {
    torrentBuckets(row).forEach((bucket) => {
      if (bucket !== "all") counts[bucket] = (counts[bucket] ?? 0) + 1;
    });
  });
  return counts;
}

function updateTorrentFilterChrome(counts) {
  document.querySelectorAll("[data-torrent-count]").forEach((el) => {
    el.textContent = String(counts[el.dataset.torrentCount] ?? 0);
  });
  document.querySelectorAll("[data-torrent-filter]").forEach((el) => {
    el.classList.toggle("active", el.dataset.torrentFilter === state.torrentFilter);
  });
}

function filteredTorrentRows(rows) {
  if (state.torrentFilter === "all") return rows;
  return rows.filter((row) => torrentBuckets(row).has(state.torrentFilter));
}

function torrentDetailPair([label, value]) {
  return `<div class="torrent-detail-pair"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value ?? "unknown")}</strong></div>`;
}

function torrentDetailSection(title, entries) {
  return `<section class="torrent-detail-section"><h3>${escapeHtml(title)}</h3><div class="torrent-detail-grid">${entries.map(torrentDetailPair).join("")}</div></section>`;
}

function renderTorrentDetails(selected, rowsData, torrent, ledger, active, digest) {
  const done = torrentRowDone(selected);
  const total = numeric(selected.bytes_total);
  const downloaded = Math.round((done / 100) * total);
  const remaining = Math.max(0, total - downloaded);
  const peerRows = Math.max(0, rowsData.length - 1);
  const counts = torrent?.counts || countTorrentBuckets(rowsData);
  const selectedName = torrentRowName(selected);
  const status = torrentRowStatus(selected);
  const rateDown = numeric(selected.down_bps ?? selected.rate_bps);
  const rateUp = numeric(selected.up_bps);
  const body = $("torrent-detail-body");
  const selectedEstateState = torrentEstateState(selected);
  const selectedBrId = state.last.siteEstate?.brDeviceId || brDeviceId() || selected.id;

  setText("torrent-selected-name", selectedName);
  const selectedProgress = $("torrent-selected-progress");
  if (selectedProgress) selectedProgress.style.width = `${done}%`;
  setText("torrent-selected-percent", `${done.toFixed(1)}%`);
  document.querySelectorAll("[data-torrent-detail]").forEach((button) => {
    button.classList.toggle("active", button.dataset.torrentDetail === state.torrentDetailTab);
  });

  const commonTransfer = [
    ["Status", status],
    ["Downloaded", formatBytes(downloaded)],
    ["Download speed", formatRate(rateDown)],
    ["Down limit", "-"],
    ["Seeds", counts.seeders ?? 0],
    ["Tracker", torrent?.source ?? "br-ledger"],
    ["Uploaded", formatBytes(selected.uploaded_bytes ?? 0)],
    ["Upload speed", formatRate(rateUp)],
    ["Up limit", "-"],
    ["Peers", peerRows],
    ["Remaining", formatBytes(remaining)],
    ["Share ratio", selected.ratio ?? (selected.role === "seed" && active ? "seed" : "-")],
  ];

  const ledgerInfo = [
    ["Name", selectedName],
    ["Total size", formatBytes(total)],
    ["Version", selected.ledger_version != null ? `v${selected.ledger_version}` : "none"],
    ["Digest", selected.manifest_digest ?? digest],
    ["Chunks", `${selected.chunks_have ?? 0}/${selected.chunks_total ?? 0}`],
    ["Chunk size", formatBytes(selected.chunk_size ?? torrent?.target?.chunk_size ?? 0)],
    ["Queue position", selected.queue_position ?? "-"],
    ["Priority", selected.priority ?? "normal"],
  ];

  const trackerInfo = [
    ["Primary tracker", "/bos/ledger/torrent"],
    ["Convergence feed", "/bos/convergence"],
    ["Peer browse", "/bos/peers"],
    ["Site registry", state.last.siteEstate?.configured ? "/api/devices" : "not configured"],
    ["Site discovery", state.last.siteEstate?.configured ? "/api/commissioning/discovered" : "not configured"],
    ["BR placement", state.last.siteEstate?.configured && selectedBrId ? `/api/border-routers/${selectedBrId}/infrastructure` : "not configured"],
    ["BR server convergence", state.last.siteEstate?.configured && selectedBrId ? `/api/border-routers/${selectedBrId}/convergence` : "not configured"],
    ["SRP service", selected.service?.tmfs ? "_tmfs._udp" : "_mesh._udp"],
    ["Source", selected.source ?? torrent?.source ?? "br-ledger"],
    ["Address", selected.address ?? selected.thread_address ?? "unknown"],
    ["Service port", selected.service?.port ?? "-"],
    ["Last active", selected.last_seen_ms != null ? `BR uptime +${formatDuration(selected.last_seen_ms)}` : "-"],
  ];

  const peerInfo = [
    ["Selected node", selectedName],
    ["Estate state", titleCase(selectedEstateState)],
    ["Row kind", torrentRowKind(selected)],
    ["Pairing state", titleCase(torrentPairingState(selected))],
    ["Holdback reason", torrentHoldbackReason(selected) || "-"],
    ["Planned match", selected.planned_match ?? selected.planned_position ?? "-"],
    ["Spatial ID", selected.spatial_id ?? "-"],
    ["Spatial label", selected.spatial_label ?? selected.planned_position ?? "-"],
    ["Hardware ID", selected.hardware_id ?? "unknown"],
    ["Transport ID", selected.transport_id ?? selected.node_id ?? "unknown"],
    ["Site source", selected.site_source ?? "-"],
    ["Role", selected.role ?? "peer"],
    ["Transfer state", status],
    ["Model", selected.model_id ?? selected.model ?? "-"],
    ["Device class", selected.device_class ?? "-"],
    ["Online", counts.online ?? "-"],
    ["Offline", counts.offline ?? "-"],
    ["Stale", counts.stale ?? "-"],
    ["Failed", counts.failed ?? 0],
  ];

  const infrastructureInfo = [
    ["BR device id", selectedBrId ?? "unknown"],
    ["Placement", selected.infrastructure_summary || "-"],
    ["Site", selected.site_id ?? "-"],
    ["Building", selected.building_id ?? "-"],
    ["Floor", selected.floor_id ?? "-"],
    ["Upstream", selected.upstream ?? "-"],
    ["Network segment", selected.network_segment ?? "-"],
    ["Cabinet", selected.cabinet ?? "-"],
    ["Control port", selected.control_port ?? 80],
    ["Diagnostics port", selected.diagnostics_port ?? "none"],
    ["Site convergence", selected.site_convergence_state ?? "-"],
    ["Site update", selected.site_convergence_updated_at ?? "-"],
  ];

  const fileInfo = [
    ["Manifest digest", selected.manifest_digest ?? digest],
    ["Ledger version", selected.ledger_version != null ? `v${selected.ledger_version}` : "none"],
    ["Chunks have", selected.chunks_have ?? 0],
    ["Chunks total", selected.chunks_total ?? 0],
    ["Size left", formatBytes(remaining)],
    ["Bytes total", formatBytes(total)],
    ["Ledger key", `v${selected.ledger_version ?? "none"}:${shortDigest(selected.manifest_digest ?? digest)}`],
  ];

  const statistics = [
    ["Total nodes", rowsData.length],
    ["Infrastructure", countTorrentBuckets(rowsData).infrastructure],
    ["Registered", countTorrentBuckets(rowsData).registered],
    ["Floating", countTorrentBuckets(rowsData).floating],
    ["Ineligible", countTorrentBuckets(rowsData).ineligible],
    ["Completed", countTorrentBuckets(rowsData).completed],
    ["Leeching", countTorrentBuckets(rowsData).downloading],
    ["Down speed", formatRate(rowsData.reduce((sum, row) => sum + numeric(row.down_bps ?? row.rate_bps), 0))],
    ["Up speed", formatRate(rowsData.reduce((sum, row) => sum + numeric(row.up_bps), 0))],
    ["Target state", ledger?.state || torrent?.state || "none"],
  ];

  if (!body) return;
  if (state.torrentDetailTab === "trackers") {
    body.innerHTML = torrentDetailSection("Trackers", trackerInfo);
  } else if (state.torrentDetailTab === "peers") {
    body.innerHTML = torrentDetailSection("Peers", peerInfo);
  } else if (state.torrentDetailTab === "files") {
    body.innerHTML = torrentDetailSection("Files", fileInfo);
  } else if (state.torrentDetailTab === "statistics") {
    body.innerHTML = torrentDetailSection("Statistics", statistics);
  } else {
    const sections = [
      torrentDetailSection("Transfer", commonTransfer),
      torrentDetailSection("Ledger", ledgerInfo),
    ];
    if (selectedEstateState === "infrastructure") {
      sections.push(torrentDetailSection("Infrastructure", infrastructureInfo));
    }
    body.innerHTML = sections.join("");
  }

  setText("torrent-status-download", `D: ${formatRate(rowsData.reduce((sum, row) => sum + numeric(row.down_bps ?? row.rate_bps), 0))}`);
  setText("torrent-status-upload", `U: ${formatRate(rowsData.reduce((sum, row) => sum + numeric(row.up_bps), 0))}`);
  setText("torrent-status-selected", `Selected: ${selectedName}`);
  setText("torrent-status-done", `Done: ${formatBytes(downloaded)}`);
  setText("torrent-status-remaining", `Remaining: ${formatBytes(remaining)}`);
}

function renderLedger(ledger, convergence, torrent, siteEstate = state.last.siteEstate) {
  const active = !ledger?.error && ledger?.present;
  const digest = ledger?.manifest_digest || "no digest";
  setText("metric-ledger", active ? `v${ledger.ledger_version}` : "none");
  setText("metric-ledger-detail", ledger?.error ?? digest);
  setText("torrent-version", active ? `v${ledger.ledger_version}` : "none");
  setText("torrent-digest", ledger?.error ?? digest);
  setText("torrent-state", ledger?.state || "none");
  setText("torrent-seeder-state", active ? "seedable" : "no active ledger");

  const rowsData = normaliseTorrentRows(torrent, ledger, convergence, siteEstate);
  const buckets = countTorrentBuckets(rowsData);
  updateTorrentFilterChrome(buckets);
  const visibleRows = filteredTorrentRows(rowsData);
  const peerRows = Math.max(0, rowsData.length - 1);
  setText("torrent-peer-count", `${peerRows} ${peerRows === 1 ? "peer" : "peers"}`);

  const overall = rowsData.length > 0
    ? rowsData.reduce((sum, row) => sum + torrentRowDone(row), 0) / rowsData.length
    : 0;
  const overallProgress = $("torrent-overall-progress");
  if (overallProgress) overallProgress.style.width = `${overall}%`;
  setText("torrent-overall-percent", `${overall.toFixed(1)}%`);

  let selected = visibleRows.find((row) => String(row.id) === String(state.selectedTorrentId));
  if (!selected && visibleRows.length > 0) {
    selected = visibleRows[0];
    state.selectedTorrentId = selected.id || null;
  }
  if (!selected) {
    selected = rowsData.find((row) => String(row.id) === String(state.selectedTorrentId));
  }
  if (!selected) {
    selected = visibleRows[0] || rowsData[0] || {};
    state.selectedTorrentId = selected.id || null;
  }

  const rows = visibleRows.map((row) => {
    const total = numeric(row.bytes_total);
    const done = torrentRowDone(row);
    const left = Math.max(0, total - Math.round((done / 100) * total));
    const seeds = row.role === "seed" ? "1" : String(row.seeds ?? 0);
    const peers = row.role === "seed" ? String(peerRows) : String(row.peers ?? "");
    const status = torrentRowStatus(row);
    const rowClass = torrentRowClass(row);
    const selectedClass = String(row.id) === String(selected.id) ? " class=\"selected\"" : "";
    return `<tr${selectedClass} data-torrent-id="${escapeHtml(row.id)}">
      <td class="torrent-name-cell"><span class="torrent-state-dot ${rowClass}"></span><span><strong>${escapeHtml(torrentRowName(row))}</strong><small>${escapeHtml(torrentRowSubtitle(row))}</small></span></td>
      ${tableCell(formatBytes(total))}
      ${progressCell(done)}
      <td><span class="torrent-status ${rowClass}">${escapeHtml(status)}</span></td>
      ${tableCell(formatRate(row.down_bps ?? row.rate_bps ?? 0))}
      ${tableCell(formatRate(row.up_bps ?? 0))}
      ${tableCell(seeds)}
      ${tableCell(peers)}
      ${tableCell(formatEta(row.eta_ms))}
      ${tableCell(row.ratio ?? (row.role === "seed" && active ? "seed" : "-"))}
      ${tableCell(row.priority)}
      ${tableCell(row.queue_position ?? "")}
      ${tableCell(formatBytes(left))}
    </tr>`;
  });
  const target = $("torrent-peers");
  if (target) {
    target.innerHTML = rows.length
      ? rows.join("")
      : `<tr><td colspan="13">No nodes match the ${escapeHtml(state.torrentFilter)} filter.</td></tr>`;
  }
  document.querySelectorAll("[data-torrent-id]").forEach((row) => {
    row.addEventListener("click", () => {
      state.selectedTorrentId = row.dataset.torrentId;
      renderLedger(state.last.ledger, state.last.convergence, state.last.torrent, state.last.siteEstate);
    });
  });
  renderTorrentDetails(selected, rowsData, torrent, ledger, active, digest);
}

async function refreshBos() {
  const [status, ledger, torrent, convergence, peers] = await Promise.all([
    bos("/bos/status").catch((e) => ({ error: e.message })),
    bos("/bos/ledger/active").catch((e) => ({ error: e.message })),
    bos("/bos/ledger/torrent").catch((e) => ({ error: e.message, peers: [] })),
    bos("/bos/convergence").catch((e) => ({ error: e.message, peers: [] })),
    bos("/bos/peers").catch((e) => ({ error: e.message, peers: [], source: "srp", state: "unavailable" })),
  ]);

  state.last.status = status.error ? null : status;
  state.last.ledger = ledger;
  state.last.torrent = torrent;
  state.last.convergence = convergence;
  state.last.peers = peers;
  if (status.site_server_url && !state.siteServerBase) {
    try {
      state.siteServerBase = normaliseBaseUrl(status.site_server_url);
      saveSetting(SITE_SERVER_STORAGE_KEY, state.siteServerBase);
      initSiteEstateInputs();
    } catch {
      state.siteServerBase = "";
    }
  }
  const siteEstate = await refreshSiteEstate(status.error ? null : status);

  if (status.error) {
    setText("metric-backbone", "unavailable");
    setText("metric-backbone-detail", status.error);
    setText("devices-diag-state", "unavailable");
    $("overall-status").textContent = "BOS diagnostics offline";
    $("overall-status").className = "status-pill warn";
  } else {
    setText("metric-backbone", status.backbone?.connected ? "online" : "waiting");
    setText("metric-backbone-detail", `${status.backbone?.ipv4 || "no IPv4"} ${status.backbone?.ipv6 || ""}`);
    setText("metric-registered", status.registered ? "registered" : "pending");
    setText("metric-firmware", status.firmware);
    setText("devices-diag-state", "available");
    $("overall-status").textContent = status.backbone?.connected ? "online" : "waiting";
    $("overall-status").className = `status-pill ${status.backbone?.connected ? "ok" : "warn"}`;
  }

  renderLedger(ledger, convergence, torrent, siteEstate);
  renderCommissioning(peers, siteEstate);
  renderNetworkSummary();
}

async function refreshAll() {
  await Promise.all([
    refreshThread().catch((e) => toast(e.message)),
    refreshBos().catch((e) => toast(e.message)),
  ]);
}

function networkField(network, names, fallback = "") {
  for (const name of names) {
    if (network[name] != null) return network[name];
  }
  return fallback;
}

function networkIndex(network, index) {
  const explicit = networkField(network, ["No", "Index", "index", "id"], null);
  if (explicit != null && !Number.isNaN(Number(explicit))) return Number(explicit);
  const first = Object.values(network)[0];
  if (first != null && !Number.isNaN(Number(first))) return Number(first);
  return index;
}

async function scanNetworks() {
  $("available-networks").innerHTML = `<tr><td colspan="9">Scanning...</td></tr>`;
  const data = await api("/available_network");
  const networks = data.result || [];
  if (!networks.length) {
    emptyRow("available-networks", 9, "No Thread networks found.");
    return;
  }
  $("available-networks").innerHTML = networks.map((n, index) => {
    const row = [
      networkIndex(n, index),
      networkField(n, ["NetworkName", "networkName", "Name", "name"], ""),
      networkField(n, ["ExtendedPanId", "ExtPanId", "XPANID", "xpanid"], ""),
      networkField(n, ["PanId", "PANID", "panId"], ""),
      networkField(n, ["MacAddress", "MAC", "mac"], ""),
      networkField(n, ["Channel", "channel"], ""),
      networkField(n, ["Rssi", "RSSI", "dBm", "dbm"], ""),
      networkField(n, ["Lqi", "LQI", "lqi"], ""),
    ].map(tableCell).join("");
    return `<tr>${row}<td><button class="ghost" data-join="${index}">Dev join</button></td></tr>`;
  }).join("");
  document.querySelectorAll("[data-join]").forEach((button) => {
    button.addEventListener("click", () => openJoin(networks[Number(button.dataset.join)]));
  });
}

function openJoin(network) {
  state.selectedNetwork = network;
  $("join-network-name").textContent = networkField(network, ["NetworkName", "networkName", "Name", "name"], "Selected Thread network");
  $("join-dialog").showModal();
}

async function joinSelected(event) {
  event.preventDefault();
  if (!state.selectedNetwork) return;
  const payload = formJson($("join-form"));
  payload.index = networkIndex(state.selectedNetwork, 0);
  await api("/join_network", { method: "POST", body: JSON.stringify(payload) }, true);
  $("join-dialog").close();
  toast("Dev join request submitted");
}

async function formNetwork() {
  const payload = formJson($("network-form"));
  await api("/form_network", { method: "POST", body: JSON.stringify(payload) }, true);
  toast("Dev form network request submitted");
}

async function addPrefix() {
  await api("/add_prefix", { method: "POST", body: JSON.stringify(formJson($("prefix-form"))) }, true);
  toast("Prefix add submitted");
}

async function deletePrefix() {
  await api("/delete_prefix", { method: "POST", body: JSON.stringify(formJson($("prefix-form"))) }, true);
  toast("Prefix delete submitted");
}

async function loadTopology() {
  const [node, topology] = await Promise.all([api("/node_information"), api("/topology")]);
  const nodeInfo = node.result || {};
  const diag = topology.result || [];
  setText("topology-network", nodeInfo.NetworkName);
  setText("topology-leader", nodeInfo.LeaderData?.LeaderRouterId != null ? `0x${nodeInfo.LeaderData.LeaderRouterId.toString(16)}` : "unknown");
  setText("topology-router-count", String(diag.filter((item) => item.ChildTable).length));
  $("topology-json").textContent = JSON.stringify({ node: nodeInfo, diagnostics: diag }, null, 2);
}

function initLinks() {
  $("diagnostics-link").href = state.bosBase + "/";
  $("device-endpoint").textContent = `${location.hostname || "border-router"} port 80`;
  setText("diagnostics-endpoint", `${location.hostname || "border-router"} port 80`);
}

function initSiteEstateInputs() {
  const serverInput = $("site-server-url");
  const apiKeyInput = $("site-api-key");
  if (serverInput) serverInput.value = state.siteServerBase;
  if (apiKeyInput) apiKeyInput.value = state.siteApiKey;
  renderSiteEstateState(state.last.siteEstate);
}

async function configureSiteEstate(event) {
  event.preventDefault();
  const serverInput = $("site-server-url");
  const apiKeyInput = $("site-api-key");
  state.siteServerBase = normaliseBaseUrl(serverInput?.value ?? "");
  state.siteApiKey = String(apiKeyInput?.value ?? "").trim();
  saveSetting(SITE_SERVER_STORAGE_KEY, state.siteServerBase);
  if (serverInput) serverInput.value = state.siteServerBase;
  await refreshBos();
  toast(state.siteServerBase ? "Site estate read configured" : "Site estate read cleared");
}

async function clearSiteEstate() {
  state.siteServerBase = "";
  state.siteApiKey = "";
  saveSetting(SITE_SERVER_STORAGE_KEY, "");
  initSiteEstateInputs();
  await refreshBos();
  toast("Site estate read cleared");
}

function bind(id, event, handler) {
  const el = $(id);
  if (el) el.addEventListener(event, handler);
}

document.addEventListener("DOMContentLoaded", () => {
  initLinks();
  initSiteEstateInputs();
  document.querySelectorAll(".nav-item").forEach((button) => button.addEventListener("click", () => showTab(button.dataset.tab)));
  document.querySelectorAll("[data-torrent-filter]").forEach((button) => {
    button.addEventListener("click", () => {
      state.torrentFilter = button.dataset.torrentFilter || "all";
      renderLedger(state.last.ledger, state.last.convergence, state.last.torrent, state.last.siteEstate);
    });
  });
  document.querySelectorAll("[data-torrent-detail]").forEach((button) => {
    button.addEventListener("click", () => {
      state.torrentDetailTab = button.dataset.torrentDetail || "general";
      renderLedger(state.last.ledger, state.last.convergence, state.last.torrent, state.last.siteEstate);
    });
  });
  showTab(tabFromLocation(), false);
  bind("refresh-all", "click", () => refreshAll());
  bind("refresh-thread", "click", () => refreshThread({ notify: true }).catch((e) => toast(e.message)));
  bind("refresh-commissioning", "click", () => refreshBos().catch((e) => toast(e.message)));
  bind("refresh-devices", "click", () => refreshBos().catch((e) => toast(e.message)));
  bind("refresh-ledger", "click", () => refreshBos().catch((e) => toast(e.message)));
  bind("scan-networks", "click", () => scanNetworks().catch((e) => toast(e.message)));
  bind("form-network", "click", () => formNetwork().catch((e) => toast(e.message)));
  bind("add-prefix", "click", () => addPrefix().catch((e) => toast(e.message)));
  bind("delete-prefix", "click", () => deletePrefix().catch((e) => toast(e.message)));
  bind("join-submit", "click", joinSelected);
  bind("load-topology", "click", () => loadTopology().catch((e) => toast(e.message)));
  bind("site-estate-form", "submit", (event) => configureSiteEstate(event).catch((e) => toast(e.message)));
  bind("site-estate-clear", "click", () => clearSiteEstate().catch((e) => toast(e.message)));
  refreshAll();
  setInterval(refreshBos, 5000);
});
