/* Banana Pocket web installer: flashes OpenABL onto the Retroid Pocket Classic over WebUSB fastboot.
 * No libraries. The fastboot protocol is short ASCII commands on a bulk OUT endpoint, answered on bulk IN
 * with OKAY / FAIL / INFO / TEXT / DATA<8 hex>, and a download is DATA then the raw bytes then OKAY. */
(function () {
  "use strict";

  /* ---- what may be flashed, and onto what ---- */
  var ABL = {
    name: "abl_signed-SM4450.elf",
    version: "OpenABL v1.2",
    size: 532480,
    sha256: "8d73232614bdf50d602dfb934157328b95fc90f1ea22ba71dbc8836bc7aa8a3b",
    url: "https://github.com/OpenHandheldCollective/OpenABL/releases/download/v1.2/abl_signed-SM4450.elf",
    local: "abl/abl_signed-SM4450.elf"   // used when the site hosts its own copy; otherwise the visitor picks the file
  };
  /* What a Retroid Pocket Classic says in fastboot. Fastboot has no chipset variable, so the device must match all of
   * these; anything else is refused, since an ABL for the wrong chipset is the one way this can brick a handheld.
   * Measured on a Rev Q unit, with the stock ABL (product parrot, empty version-bootloader) and with OpenABL 1.2
   * (product OpenABL): both say variant "SGP EMMC", hw-revision 10000, abl_a/abl_b 1 MiB, unlocked, secure no. */
  var DEVICE = {
    products: ["parrot", "OpenABL"],
    variant: "SGP EMMC",
    hwRevision: "10000",
    ablSize: 0x100000
  };

  var FB_CLASS = 0xff, FB_SUBCLASS = 0x42, FB_PROTOCOL = 0x03;
  var CHUNK = 16384;

  var $ = function (s) { return document.querySelector(s); };
  var logEl = $("#log");
  var dev = null, epIn = 0, epOut = 0;
  var image = null;           // Uint8Array of the verified ABL
  var checked = false;        // device passed the checks
  var mode = "";              // "bootloader" (OpenABL) or "fastbootd": where the flash happens

  function log(msg, cls) {
    var line = document.createElement("div");
    if (cls) line.className = cls;
    line.textContent = msg;
    logEl.appendChild(line);
    logEl.scrollTop = logEl.scrollHeight;
  }
  function setStep(id, state) { var el = $(id); if (el) el.setAttribute("data-state", state); }
  function refresh() {
    $("#flashBtn").disabled = !(image && checked && dev);
  }

  /* ---- sha256 of the ABL, against the pinned hash ---- */
  function hex(buf) {
    return Array.prototype.map.call(new Uint8Array(buf), function (b) { return ("0" + b.toString(16)).slice(-2); }).join("");
  }
  function takeImage(bytes, from) {
    if (bytes.length !== ABL.size) {
      log(from + ": " + bytes.length + " bytes, not the " + ABL.size + " of " + ABL.version + "'s " + ABL.name, "bad");
      return Promise.resolve(false);
    }
    return crypto.subtle.digest("SHA-256", bytes).then(function (d) {
      var h = hex(d);
      if (h !== ABL.sha256) {
        log(from + ": sha256 " + h + " is not " + ABL.version + "'s. Not using it.", "bad");
        return false;
      }
      image = bytes;
      log(from + ": " + ABL.name + " verified (sha256 matches " + ABL.version + ")", "ok");
      setStep("#stepFile", "done");
      $("#fileState").textContent = "Verified: " + ABL.version;
      refresh();
      return true;
    });
  }
  function tryLocal() {
    return fetch(ABL.local, { cache: "no-store" }).then(function (r) {
      if (!r.ok) throw new Error(r.status);
      return r.arrayBuffer();
    }).then(function (b) { return takeImage(new Uint8Array(b), "site copy"); })
      .catch(function () { $("#filePick").hidden = false; return false; });
  }
  $("#ablFile").addEventListener("change", function (e) {
    var f = e.target.files && e.target.files[0];
    if (!f) return;
    f.arrayBuffer().then(function (b) { return takeImage(new Uint8Array(b), f.name); });
  });

  /* ---- fastboot over WebUSB ---- */
  var enc = new TextEncoder(), dec = new TextDecoder();

  function readReply() {
    return dev.transferIn(epIn, 256).then(function (r) {
      if (r.status !== "ok") throw new Error("USB read: " + r.status);
      var s = dec.decode(r.data);
      var kind = s.slice(0, 4), rest = s.slice(4);
      if (kind === "INFO" || kind === "TEXT") { if (rest) log("  " + rest.trim(), "dim"); return readReply(); }
      if (kind === "OKAY") return { ok: true, value: rest };
      if (kind === "DATA") return { ok: true, data: parseInt(rest, 16) };
      if (kind === "FAIL") return { ok: false, value: rest };
      throw new Error("unexpected reply: " + JSON.stringify(s));
    });
  }
  function command(cmd) {
    return dev.transferOut(epOut, enc.encode(cmd)).then(function (r) {
      if (r.status !== "ok") throw new Error("USB write: " + r.status);
      return readReply();
    });
  }
  function getvar(name) {
    return command("getvar:" + name).then(function (r) { return r.ok ? r.value.trim() : null; });
  }
  function must(r, what) {
    if (!r.ok) throw new Error(what + " refused: " + (r.value || "FAIL"));
    return r;
  }
  function download(bytes) {
    var len = ("00000000" + bytes.length.toString(16)).slice(-8);
    return command("download:" + len).then(function (r) {
      must(r, "download");
      if (r.data !== bytes.length) throw new Error("device asked for " + r.data + " bytes, not " + bytes.length);
      var off = 0;
      function next() {
        if (off >= bytes.length) return readReply();
        var part = bytes.subarray(off, Math.min(off + CHUNK, bytes.length));
        return dev.transferOut(epOut, part).then(function (w) {
          if (w.status !== "ok") throw new Error("USB write: " + w.status);
          off += part.length;
          return next();
        });
      }
      return next();
    }).then(function (r) { return must(r, "download"); });
  }

  function findEndpoints(d) {
    var conf = d.configuration;
    for (var i = 0; i < conf.interfaces.length; i++) {
      var itf = conf.interfaces[i];
      for (var j = 0; j < itf.alternates.length; j++) {
        var alt = itf.alternates[j];
        if (alt.interfaceClass === FB_CLASS && alt.interfaceSubclass === FB_SUBCLASS && alt.interfaceProtocol === FB_PROTOCOL) {
          var inp = 0, out = 0;
          alt.endpoints.forEach(function (ep) {
            if (ep.type !== "bulk") return;
            if (ep.direction === "in") inp = ep.endpointNumber; else out = ep.endpointNumber;
          });
          if (inp && out) return { itf: itf.interfaceNumber, alt: alt.alternateSetting, inp: inp, out: out };
        }
      }
    }
    return null;
  }

  function connect() {
    if (!navigator.usb) { log("This browser has no WebUSB. Use Chrome, Edge or another Chromium browser on a computer.", "bad"); return; }
    checked = false; refresh();
    navigator.usb.requestDevice({ filters: [{ classCode: FB_CLASS, subclassCode: FB_SUBCLASS, protocolCode: FB_PROTOCOL }] })
      .then(function (d) {
        dev = d;
        return d.open().then(function () { return d.configuration ? null : d.selectConfiguration(1); }).then(function () {
          var e = findEndpoints(d);
          if (!e) throw new Error("this device has no fastboot interface");
          epIn = e.inp; epOut = e.out;
          return d.claimInterface(e.itf).then(function () { return e.alt ? d.selectAlternateInterface(e.itf, e.alt) : null; });
        });
      })
      .then(function () { log("connected: " + (dev.productName || "fastboot device")); return check(); })
      .catch(function (err) {
        if (err && err.name === "NotFoundError") { log("no device picked", "dim"); return; }
        log(String(err && err.message || err), "bad");
        if (err && err.name === "SecurityError") log("The page can't open the device. On Windows, install the Google USB driver; on Linux, add the udev rule below.", "bad");
        setStep("#stepDevice", "bad");
      });
  }

  /* The stock Retroid bootloader has no `flash` command at all (it answers "unknown command"), so a stock unit is
   * identified in the bootloader, where variant and hw-revision are real, then restarted into fastbootd (Android
   * recovery's fastboot, which does flash), and accepted there only with the same serial number. */
  var SERIAL_KEY = "bp-install-serial";
  function verifiedSerial(v) {
    try { if (v === undefined) return sessionStorage.getItem(SERIAL_KEY); sessionStorage.setItem(SERIAL_KEY, v); }
    catch (e) { /* storage off: the in-memory copy below still works while the page stays open */ }
    if (v !== undefined) verifiedSerial.mem = v;
    return verifiedSerial.mem || null;
  }
  function refuse(why) {
    why.forEach(function (w) { log("refusing: " + w, "bad"); });
    $("#devState").textContent = "Not flashing this device";
    setStep("#stepDevice", "bad");
  }
  function ablSizesOk(info, why) {
    ["abl_a", "abl_b"].forEach(function (p) {
      if (parseInt(info["partition-size:" + p], 16) !== DEVICE.ablSize) why.push(p + " is not the 1 MiB partition this device has");
    });
  }

  function check() {
    var info = {};
    var names = ["product", "variant", "hw-revision", "version-bootloader", "unlocked", "slot-count", "current-slot",
                 "max-download-size", "partition-size:abl_a", "partition-size:abl_b", "is-userspace", "serialno",
                 "is-logical:abl_a", "is-logical:abl_b"];
    $("#fbdBtn").hidden = true;
    return names.reduce(function (p, n) {
      return p.then(function () { return getvar(n).then(function (v) { info[n] = v; }); });
    }, Promise.resolve()).then(function () {
      var userspace = info["is-userspace"] === "yes";
      log((userspace ? "fastbootd: " : "bootloader: ") + "product " + info.product + ", variant " + info.variant + ", hw " +
          info["hw-revision"] + ", bootloader " + info["version-bootloader"] + ", unlocked " + info.unlocked +
          ", slot " + info["current-slot"]);
      var why = [];
      if (userspace) {
        var known = verifiedSerial();
        if (!known) why.push("this is fastbootd: connect in the bootloader first (power off, hold Volume Down, press Power) so the page can check the handheld");
        else if (info.serialno !== known) why.push("this isn't the handheld the page checked in the bootloader");
        if (info.product !== "parrot") why.push("product \"" + info.product + "\" in fastbootd is not a Retroid Pocket Classic's");
        ablSizesOk(info, why);
        if (info["is-logical:abl_a"] !== "no" || info["is-logical:abl_b"] !== "no") why.push("abl_a / abl_b aren't plain partitions here");
        if (info.unlocked !== "yes") why.push("the bootloader reports locked");
        if (why.length) { refuse(why); return; }
        mode = "fastbootd";
        checked = true;
        $("#devState").textContent = "Retroid Pocket Classic in fastbootd: ready to flash";
        log("same handheld, fastbootd checks passed", "ok");
        setStep("#stepDevice", "done");
        refresh();
        return;
      }
      if (DEVICE.products.indexOf(info.product) < 0) why.push("product \"" + info.product + "\" is not one this page knows");
      if (info.variant !== DEVICE.variant) why.push("variant \"" + info.variant + "\" is not a Retroid Pocket Classic's");
      if (info["hw-revision"] !== DEVICE.hwRevision) why.push("hw-revision \"" + info["hw-revision"] + "\" is not a Retroid Pocket Classic's");
      ablSizesOk(info, why);
      if (info.unlocked !== "yes") why.push("the bootloader reports locked");
      if (!info.serialno) why.push("the handheld reports no serial number");
      var max = parseInt(info["max-download-size"], 10) || 0;
      if (max && max < ABL.size) why.push("download size limit is too small");
      if (why.length) { refuse(why); return; }
      verifiedSerial(info.serialno);
      if (info.product === "parrot") {
        $("#devState").textContent = "Stock bootloader: restart into fastbootd to flash";
        log("Retroid Pocket Classic checks passed. Its stock bootloader can't flash, so the next step restarts it into fastbootd.", "ok");
        $("#fbdBtn").hidden = false;
        setStep("#stepDevice", "busy");
        return;
      }
      if (!why.length && info.product === "OpenABL" && info["version-bootloader"] === "1.2") {
        $("#devState").textContent = "OpenABL 1.2 is already installed: go to step 2";
        log("OpenABL " + info["version-bootloader"] + " is already on this handheld, nothing to flash", "ok");
        setStep("#stepDevice", "done"); setStep("#stepFlash", "done");
        $("#next").scrollIntoView({ behavior: "smooth" });
        return;
      }
      mode = "bootloader";   // OpenABL, an older version: its own fastboot flashes
      checked = true;
      $("#devState").textContent = "Retroid Pocket Classic, " + info.product;
      log("device checks passed", "ok");
      setStep("#stepDevice", "done");
      refresh();
    });
  }

  function flash() {
    if (!(image && checked && dev)) return;
    $("#flashBtn").disabled = true;
    $("#connectBtn").disabled = true;
    setStep("#stepFlash", "busy");
    var slots = ["abl_a", "abl_b"];
    slots.reduce(function (p, part) {
      return p.then(function () {
        log("sending " + ABL.name + " (" + image.length + " bytes)");
        return download(image).then(function () {
          log("writing " + part);
          return command("flash:" + part);
        }).then(function (r) { must(r, "flash " + part); log(part + " written", "ok"); });
      });
    }, Promise.resolve())
      .then(function () {
        log("restarting into the OpenABL menu");
        return command("reboot-bootloader").catch(function () { /* it may drop off USB before answering */ });
      })
      .then(function () {
        setStep("#stepFlash", "done");
        $("#flashState").textContent = "Done: OpenABL is on both slots";
        log("Done. The handheld restarts into the OpenABL menu. Next: write the card with the flasher (step 2).", "ok");
        $("#next").scrollIntoView({ behavior: "smooth" });
      })
      .catch(function (err) {
        setStep("#stepFlash", "bad");
        log(String(err && err.message || err), "bad");
        log("Stop here: don't unplug or reboot. If abl_a was written and abl_b failed, the handheld still boots from the slot in use; ask on Discord with this log.", "bad");
        $("#connectBtn").disabled = false;
      });
  }

  if (navigator.usb) {
    navigator.usb.addEventListener("disconnect", function (e) {
      if (dev && e.device === dev) { dev = null; checked = false; refresh(); log("device disconnected", "dim"); }
    });
  } else {
    $("#noUsb").hidden = false;
    $("#connectBtn").disabled = true;
  }
  function toFastbootd() {
    if (!dev) return;
    $("#fbdBtn").hidden = true;
    log("restarting into fastbootd (Android recovery's fastboot); this takes about 20 seconds");
    command("reboot-fastboot").catch(function () { /* drops off USB as it restarts */ }).then(function () {
      $("#devState").textContent = "Wait for the fastbootd screen, then press Connect again";
      log("When the handheld shows fastbootd, press Connect again and pick it (it has a new USB name).", "ok");
    });
  }
  $("#fbdBtn").addEventListener("click", toFastbootd);
  $("#connectBtn").addEventListener("click", connect);
  $("#flashBtn").addEventListener("click", flash);
  $("#ablLink").href = ABL.url;
  tryLocal();
  refresh();
})();
