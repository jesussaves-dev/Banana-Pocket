/* Banana Pocket site: palettes, opt-in sound, the Sound Test and the Blue Banana. */
(function () {
  "use strict";
  var root = document.documentElement;
  var $ = function (s) { return document.querySelector(s); };
  var $$ = function (s) { return Array.prototype.slice.call(document.querySelectorAll(s)); };
  var reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  function store(k, v) {
    try { if (v === undefined) return localStorage.getItem("bp-" + k); localStorage.setItem("bp-" + k, v); }
    catch (e) { return null; }
  }

  /* ---------- sound (off until the visitor turns it on) ---------- */
  var soundOn = false;
  var cache = {};
  var bgm = null;
  var lastBlip = 0;
  var sndBtn = $("#sndToggle");

  function sfx(name, force) {
    if (!soundOn && !force) return;
    try {
      if (!cache[name]) { cache[name] = new Audio("assets/sfx/" + name + ".mp3"); cache[name].preload = "auto"; }
      var a = cache[name].paused ? cache[name] : cache[name].cloneNode();
      a.volume = 0.55; a.currentTime = 0;
      var p = a.play(); if (p && p.catch) p.catch(function () {});
    } catch (e) { /* audio unavailable */ }
  }
  function setSound(on) {
    soundOn = on;
    sndBtn.setAttribute("aria-pressed", String(on));
    if (!on && bgm) { bgm.pause(); $("#stBgm").classList.remove("on"); }
    if (on) sfx("ready");
  }
  sndBtn.addEventListener("click", function () { setSound(!soundOn); });

  document.addEventListener("pointerover", function (e) {
    var t = e.target.closest && e.target.closest(".sfx, .pal, .maker, .board, .st-arrow, .links a");
    if (!t || (e.relatedTarget && t.contains(e.relatedTarget))) return;
    var now = performance.now();
    if (now - lastBlip > 70) { lastBlip = now; sfx("scroll"); }
  });
  document.addEventListener("click", function (e) {
    if (e.target.closest && e.target.closest(".sfx")) sfx("select");
  });

  /* ---------- CRT ---------- */
  var crtBtn = $("#crtToggle");
  function setCrt(on) { root.classList.toggle("crt-on", on); crtBtn.setAttribute("aria-pressed", String(on)); store("crt", on ? "1" : "0"); }
  crtBtn.addEventListener("click", function () { setCrt(!root.classList.contains("crt-on")); sfx("select"); });
  if (store("crt") === "1") setCrt(true);

  /* ---------- clock in the HUD ---------- */
  function tickClock() {
    var d = new Date(), h = d.getHours(), m = d.getMinutes();
    var ap = h >= 12 ? "PM" : "AM"; h = h % 12 || 12;
    $("#clock").textContent = (h < 10 ? "0" : "") + h + ":" + (m < 10 ? "0" : "") + m + " " + ap;
  }
  tickClock(); setInterval(tickClock, 15000);

  /* ---------- the rider ---------- */
  var rider = $("#rider"), riderImg = $("#riderImg"), frame = 0, blue = false;
  function riderFrame() {
    frame ^= 1;
    riderImg.src = "assets/img/" + (blue ? "blue-" : "") + "ride" + frame + ".png";
  }
  ["ride0", "ride1", "blue-ride0", "blue-ride1"].forEach(function (n) { new Image().src = "assets/img/" + n + ".png"; });
  if (!reduced) setInterval(riderFrame, 160);

  function boost() {
    rider.classList.remove("boost"); void rider.offsetWidth; rider.classList.add("boost");
    sfx(blue ? "rev" : "boost");
  }
  var riderClicks = 0, riderTimer = 0;
  rider.addEventListener("click", function () {
    boost();
    riderClicks++; clearTimeout(riderTimer);
    riderTimer = setTimeout(function () { riderClicks = 0; }, 1500);
    if (riderClicks >= 5) { riderClicks = 0; blueBanana(); }
  });

  /* ---------- palettes ---------- */
  var shot = $("#palShot");
  var alts = {
    night: "Banana Blast, Night Drive palette: a magenta floor, cyan rails and a violet city",
    acid: "Banana Blast, Acid Mall palette: a lime floor and pink rails",
    arcade: "Banana Blast, Hyper Arcade palette: red and blue"
  };
  function setPalette(p, quiet) {
    if (!alts[p]) p = "night";
    root.setAttribute("data-palette", p);
    $$(".pal").forEach(function (b) { b.setAttribute("aria-selected", String(b.dataset.pal === p)); });
    shot.classList.add("swap");
    setTimeout(function () {
      shot.src = "assets/img/blast-" + p + ".png"; shot.alt = alts[p]; shot.classList.remove("swap");
    }, quiet ? 0 : 120);
    store("pal", p);
    if (!quiet) boost();
  }
  $$(".pal").forEach(function (b) { b.addEventListener("click", function () { setPalette(b.dataset.pal); }); });
  setPalette(store("pal") || "night", true);

  /* ---------- systems: L / R boost between makers ---------- */
  var makers = $$(".maker"), makerIdx = 0, makerRow = $("#makers");
  function focusMaker(i) {
    makerIdx = (i + makers.length) % makers.length;
    makers.forEach(function (m, j) { m.classList.toggle("on", j === makerIdx); });
    var m = makers[makerIdx];
    makerRow.scrollTo({ left: m.offsetLeft - makerRow.offsetLeft, behavior: reduced ? "auto" : "smooth" });
    sfx("boost");
  }

  /* ---------- Sound Test ---------- */
  var tests = [
    ["scroll", "SCROLL"], ["select", "SELECT"], ["back", "BACK"], ["boost", "BOOST"],
    ["insert", "INSERT"], ["ready", "READY"]
  ];
  for (var i = 0; i < 16; i++) tests.push(["num" + i, "VOICE " + (i < 10 ? "0" : "") + i]);
  var stIdx = 0;
  var stNum = $("#stNum"), stName = $("#stName");
  // stepping left from 00 lands on 99, which isn't in the list
  function showTest() {
    if (stIdx === -1) { stNum.textContent = "99"; stName.textContent = "???"; return; }
    stNum.textContent = (stIdx < 10 ? "0" : "") + stIdx;
    stName.textContent = tests[stIdx][1];
  }
  $("#stPrev").addEventListener("click", function () {
    stIdx = stIdx === -1 ? tests.length - 1 : stIdx - 1;
    showTest(); sfx("scroll");
  });
  $("#stNext").addEventListener("click", function () {
    stIdx = stIdx >= tests.length - 1 ? -1 : stIdx + 1;
    showTest(); sfx("scroll");
  });
  $("#stPlay").addEventListener("click", function () {
    if (!soundOn) setSound(true);
    if (stIdx === -1) { blueBanana(); return; }
    sfx(tests[stIdx][0]);
  });
  $("#stBgm").addEventListener("click", function () {
    if (!soundOn) setSound(true);
    try {
      if (!bgm) { bgm = new Audio("assets/sfx/bgm.mp3"); bgm.loop = true; bgm.volume = 0.4; }
      if (bgm.paused) { var p = bgm.play(); if (p && p.catch) p.catch(function () {}); this.classList.add("on"); }
      else { bgm.pause(); this.classList.remove("on"); }
    } catch (e) { /* audio unavailable */ }
  });
  showTest();

  /* ---------- Ko-fi credits ---------- */
  var credits = 0, creditEl = $("#credits");
  function addCredit() {
    credits = Math.min(99, credits + 1);
    creditEl.textContent = (credits < 10 ? "0" : "") + credits;
    sfx("insert");
  }
  $("#kofiBtn").addEventListener("pointerenter", addCredit);
  $("#kofiBtn").addEventListener("focus", addCredit);

  /* ---------- the Blue Banana ---------- */
  var toastEl = $("#toast"), toastTimer = 0;
  function toast(msg) {
    toastEl.textContent = msg; toastEl.classList.add("show");
    clearTimeout(toastTimer); toastTimer = setTimeout(function () { toastEl.classList.remove("show"); }, 3200);
  }
  function blueBanana() {
    blue = !blue;
    root.classList.toggle("blue", blue);
    var f = $("#flash"); f.classList.remove("go"); void f.offsetWidth; if (!reduced) f.classList.add("go");
    sfx("secret");
    setTimeout(boost, 400);
    riderFrame();
    toast(blue ? "★ BLUE BANANA ★" : "BANANA RESTORED");
  }

  var code = ["arrowup", "arrowup", "arrowdown", "arrowdown", "arrowleft", "arrowright", "arrowleft", "arrowright", "b", "a"];
  var pos = 0;
  document.addEventListener("keydown", function (e) {
    if (e.target.closest && e.target.closest("input, textarea")) return;
    var k = (e.key || "").toLowerCase();
    pos = k === code[pos] ? pos + 1 : (k === "arrowup" && pos >= 2 ? 2 : (k === code[0] ? 1 : 0));
    if (pos === code.length) { pos = 0; blueBanana(); return; }
    if (k === "l" || k === "r") {
      var sys = $("#systems").getBoundingClientRect();
      if (sys.top < window.innerHeight * 0.6 && sys.bottom > 0) focusMaker(makerIdx + (k === "r" ? 1 : -1));
    }
  });

  /* ---------- reveal on scroll ---------- */
  var rev = $$(".reveal");
  if ("IntersectionObserver" in window && !reduced) {
    var io = new IntersectionObserver(function (es) {
      es.forEach(function (en) { if (en.isIntersecting) { en.target.classList.add("in"); io.unobserve(en.target); } });
    }, { rootMargin: "0px 0px -10% 0px" });
    rev.forEach(function (el) { io.observe(el); });
  } else {
    rev.forEach(function (el) { el.classList.add("in"); });
  }
})();
