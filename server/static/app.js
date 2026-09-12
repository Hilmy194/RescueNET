// RescueNet Dashboard - polling API setiap 4 detik (sesuai target <10 detik pada proposal)
const REFRESH_MS = 4000;

async function fetchReports() {
  try {
    const res = await fetch("/api/reports");
    const data = await res.json();
    setConnStatus(true);
    renderReports(data);
    renderMap(data);
  } catch (e) {
    setConnStatus(false);
  }
}

async function fetchNodes() {
  try {
    const res = await fetch("/api/nodes");
    const data = await res.json();
    const active = data.filter(n => n.online).length;
    document.getElementById("statNodes").innerText = active;
  } catch (e) { /* diamkan, akan dicoba lagi siklus berikutnya */ }
}

function setConnStatus(ok) {
  const el = document.getElementById("connStatus");
  el.className = "badge " + (ok ? "online" : "offline");
  el.innerText = ok ? "Terhubung ke server lokal" : "Terputus dari server";
}

function timeAgo(ts) {
  const d = new Date(ts * 1000);
  return d.toLocaleTimeString("id-ID");
}

function renderReports(data) {
  const tbody = document.getElementById("reportBody");
  tbody.innerHTML = "";

  let sosCount = 0, baruCount = 0;

  data.forEach(r => {
    if (r.sos) sosCount++;
    if (r.status === "BARU") baruCount++;

    const tr = document.createElement("tr");
    if (r.sos) tr.className = "sos";

    const lokasi = r.has_gps ? `${r.lat.toFixed(5)}, ${r.lon.toFixed(5)}` : "GPS tidak aktif";

    tr.innerHTML = `
      <td>${timeAgo(r.received_at)}</td>
      <td>Node ${r.src_id}</td>
      <td>${r.hop}</td>
      <td><span class="tag ${r.kondisi}">${r.sos ? "SOS" : r.kondisi}</span></td>
      <td>${r.jumlah}</td>
      <td>${lokasi}</td>
      <td>${r.pesan}</td>
      <td>${r.rssi} dBm</td>
      <td>
        <select class="status-select" data-id="${r.id}">
          <option value="BARU" ${r.status === "BARU" ? "selected" : ""}>Baru</option>
          <option value="DITANGANI" ${r.status === "DITANGANI" ? "selected" : ""}>Ditangani</option>
          <option value="SELESAI" ${r.status === "SELESAI" ? "selected" : ""}>Selesai</option>
        </select>
      </td>
      <td></td>
    `;
    tbody.appendChild(tr);
  });

  document.getElementById("statTotal").innerText = data.length;
  document.getElementById("statSOS").innerText = sosCount;
  document.getElementById("statBaru").innerText = baruCount;

  document.querySelectorAll(".status-select").forEach(sel => {
    sel.addEventListener("change", async (e) => {
      const id = e.target.dataset.id;
      await fetch(`/api/reports/${id}/status`, {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ status: e.target.value })
      });
    });
  });
}

// Scatter plot sederhana berbasis SVG (tanpa perlu peta/tile - cocok untuk kondisi offline)
function renderMap(data) {
  const svg = document.getElementById("mapSvg");
  svg.innerHTML = "";
  const points = data.filter(r => r.has_gps);
  if (points.length === 0) {
    svg.innerHTML = `<text x="200" y="200" fill="#6a6a90" font-size="13" text-anchor="middle">Belum ada data GPS</text>`;
    return;
  }

  const lats = points.map(p => p.lat), lons = points.map(p => p.lon);
  const minLat = Math.min(...lats), maxLat = Math.max(...lats);
  const minLon = Math.min(...lons), maxLon = Math.max(...lons);
  const rangeLat = (maxLat - minLat) || 0.001;
  const rangeLon = (maxLon - minLon) || 0.001;

  points.forEach(p => {
    const x = 30 + ((p.lon - minLon) / rangeLon) * 340;
    const y = 370 - ((p.lat - minLat) / rangeLat) * 340;
    const color = p.sos ? "#e94560" : "#4fd1c5";
    const circle = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    circle.setAttribute("cx", x);
    circle.setAttribute("cy", y);
    circle.setAttribute("r", p.sos ? 7 : 5);
    circle.setAttribute("fill", color);
    circle.setAttribute("opacity", "0.85");
    const title = document.createElementNS("http://www.w3.org/2000/svg", "title");
    title.textContent = `Node ${p.src_id} - ${p.kondisi} (${p.lat.toFixed(4)}, ${p.lon.toFixed(4)})`;
    circle.appendChild(title);
    svg.appendChild(circle);
  });
}

fetchReports();
fetchNodes();
setInterval(fetchReports, REFRESH_MS);
setInterval(fetchNodes, REFRESH_MS);
