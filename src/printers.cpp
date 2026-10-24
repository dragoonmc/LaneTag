#include "printers.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <math.h>
#include <mbedtls/base64.h>

#include "anycubic.h"
#include "bambu.h"
#include "creality.h"
#include "decoders.h"

namespace {

constexpr uint32_t TIMEOUT = 8000;
// The plugin may look the tag up in Spoolman before it answers.
constexpr uint32_t SCAN_TIMEOUT = 25000;
// Larger "spool IDs" on tags are not Spoolman's (phone apps write
// timestamps there).
constexpr int MAX_SPOOLMAN_ID = 10000000;

// --- HTTP --------------------------------------------------------------------

struct HttpResult {
    int code = 0;
    String body;
    bool ok() const { return code >= 200 && code < 300; }
};

HttpResult request(const String &url, const char *method, const String &body, const String &apiKey = "",
                   uint32_t timeout = TIMEOUT) {
    HttpResult result;
    if (WiFi.status() != WL_CONNECTED) {
        result.code = -1;
        result.body = "Wi-Fi is not connected";
        return result;
    }
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(timeout);
    if (!http.begin(client, url)) {
        result.code = -1;
        result.body = "bad address " + url;
        return result;
    }
    if (body.length()) http.addHeader("Content-Type", "application/json");
    if (apiKey.length()) http.addHeader("X-Api-Key", apiKey);
    result.code = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());
    result.body = result.code > 0 ? http.getString() : HTTPClient::errorToString(result.code);
    http.end();
    return result;
}

String describeFailure(const HttpResult &result, const JsonDocument &doc) {
    if (result.code <= 0) return result.body;
    const char *error = doc["error"]["message"] | (doc["error"] | (const char *)nullptr);
    if (error) return error;
    return "HTTP " + String(result.code);
}

String base64(const uint8_t *data, size_t len) {
    if (!len) return "";
    size_t outLen = 0;
    std::vector<uint8_t> out(4 * ((len + 2) / 3) + 1);
    mbedtls_base64_encode(out.data(), out.size(), &outLen, data, len);
    return String((const char *)out.data(), outLen);
}

// --- Materials and colors -----------------------------------------------------

String upper(String text) {
    text.trim();
    text.toUpperCase();
    return text;
}

// Zmod material type for a tag's material: exact match, then the base
// without filler or modifiers, then the fallbacks zmod_nfc uses.
String resolveMaterial(const String &input, JsonArrayConst validTypes) {
    String material = upper(input);
    std::vector<String> valid;
    for (JsonVariantConst t : validTypes) {
        String type = upper(t.as<String>());
        if (type.length() && type != "NONE" && type != "N/A" && type != "?") valid.push_back(type);
    }
    if (valid.empty()) return material;
    auto has = [&](const String &m) {
        for (auto &v : valid)
            if (v == m) return true;
        return false;
    };
    int dash = material.indexOf('-'), space = material.indexOf(' ');
    String candidates[] = {material, dash > 0 ? material.substring(0, dash) : material,
                           space > 0 ? material.substring(0, space) : material};
    for (auto &c : candidates)
        if (has(c)) return c;
    String fallback;
    if (material.startsWith("ASA")) fallback = "ABS";
    else if (material == "COPE" || material == "CPE" || material.indexOf("PLA") >= 0) fallback = "PLA";
    else if (material.startsWith("PCTG") || material.startsWith("PET")) fallback = "PETG";
    return has(fallback) ? fallback : "";
}

void rgbOf(uint32_t color, long &r, long &g, long &b) {
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
}

// Closest palette entry by the "redmean" weighted RGB distance (as zmod_nfc
// and zmod_afclite snap colors to Zmod's palette).
String nearestPalette(uint32_t color, JsonArrayConst palette) {
    String best;
    double bestDistance = 1e18;
    long r1, g1, b1;
    rgbOf(color, r1, g1, b1);
    for (JsonVariantConst entry : palette) {
        int32_t c = parseColor(entry.as<String>());
        if (c < 0) continue;
        long r2, g2, b2;
        rgbOf(c, r2, g2, b2);
        double mean = (r1 + r2) / 2.0;
        double d = (2 + mean / 256) * (r1 - r2) * (r1 - r2) + 4 * (g1 - g2) * (g1 - g2) +
                   (2 + (255 - mean) / 256) * (b1 - b2) * (b1 - b2);
        if (d < bestDistance) {
            bestDistance = d;
            char hex[12];
            snprintf(hex, sizeof(hex), "%06X", (unsigned)(c & 0xFFFFFF));
            best = hex;
        }
    }
    return best;
}

// The stock screens draw a slot's color only when it is one of their 24
// palette entries, byte for byte as "#RRGGBB"; any other color is stored
// but not shown. Each model has its own palette.
constexpr size_t PALETTE_SIZE = 24;
const char *const C5_PALETTE[PALETTE_SIZE] = {
    "#FFFFFF", "#FFF245", "#DEF578", "#21CC3D", "#167A4B", "#156682", "#24E4A0", "#7BD9F0",
    "#4CAAF8", "#2E54DD", "#48358C", "#A341F7", "#F435F6", "#D5B4DE", "#FA6173", "#F82D29",
    "#805003", "#F9903B", "#FCEBD7", "#D5C5A1", "#B17C38", "#8C8C89", "#BEBEBE", "#1B1B1B",
};
// From the AD5X's stock firmwareExe (3.1.7), as its screen stores them.
const char *const AD5X_PALETTE[PALETTE_SIZE] = {
    "#FFFFFF", "#FEF043", "#DCF478", "#0ACC38", "#067749", "#0C6283", "#0DE2A0", "#75D9F3",
    "#45A8F9", "#2750E0", "#46328E", "#A03CF7", "#F330F9", "#D4B0DC", "#F95D73", "#F72224",
    "#7C4B00", "#F98D33", "#FDEBD5", "#D3C4A3", "#AF7836", "#898989", "#BCBCBC", "#161616",
};
const char *const C5_MATERIALS[] = {
    "PLA", "PETG", "PLA-CF", "PETG-CF", "ABS", "ASA", "SILK", "PET-CF", "PAHT-CF", "S-PAHT",
    "S-Multi", "PA-CF", "HIPS", "PVA", "TPU-90A", "TPU-95A", "TPU-64D", "PC", "PA", "PC-ABS", "PPS-CF",
};

void toLab(uint32_t color, double lab[3]) {
    double c[3];
    for (int i = 0; i < 3; i++) {
        double v = ((color >> (16 - 8 * i)) & 0xFF) / 255.0;
        c[i] = v > 0.04045 ? pow((v + 0.055) / 1.055, 2.4) : v / 12.92;
    }
    double x = (c[0] * 0.4124 + c[1] * 0.3576 + c[2] * 0.1805) / 0.95047;
    double y = c[0] * 0.2126 + c[1] * 0.7152 + c[2] * 0.0722;
    double z = (c[0] * 0.0193 + c[1] * 0.1192 + c[2] * 0.9505) / 1.08883;
    auto f = [](double v) { return v > 0.008856 ? cbrt(v) : 7.787 * v + 16.0 / 116.0; };
    lab[0] = 116 * f(y) - 16;
    lab[1] = 500 * (f(x) - f(y));
    lab[2] = 200 * (f(y) - f(z));
}

// CIEDE2000 color difference of two CIELAB colors. Plain CIELAB distance
// sends saturated blues to purple (#0022FF became "bright purple" on the
// AD5X palette); CIEDE2000 corrects the hue in that region.
double deltaE2000(const double lab1[3], const double lab2[3]) {
    const double deg = M_PI / 180;
    double c1 = hypot(lab1[1], lab1[2]), c2 = hypot(lab2[1], lab2[2]);
    double cBar7 = pow((c1 + c2) / 2, 7);
    double g = 0.5 * (1 - sqrt(cBar7 / (cBar7 + 6103515625.0)));    // 25^7
    double a1 = (1 + g) * lab1[1], a2 = (1 + g) * lab2[1];
    double c1p = hypot(a1, lab1[2]), c2p = hypot(a2, lab2[2]);
    double h1 = fmod(atan2(lab1[2], a1) / deg + 360, 360);
    double h2 = fmod(atan2(lab2[2], a2) / deg + 360, 360);
    double dL = lab2[0] - lab1[0], dC = c2p - c1p, dh = 0;
    if (c1p * c2p != 0) {
        dh = h2 - h1;
        if (dh > 180) dh -= 360;
        else if (dh < -180) dh += 360;
    }
    double dH = 2 * sqrt(c1p * c2p) * sin(dh / 2 * deg);
    double lBar = (lab1[0] + lab2[0]) / 2, cBarP = (c1p + c2p) / 2, hBar = h1 + h2;
    if (c1p * c2p != 0) {
        if (fabs(h1 - h2) <= 180) hBar = (h1 + h2) / 2;
        else hBar = (h1 + h2 < 360 ? h1 + h2 + 360 : h1 + h2 - 360) / 2;
    }
    double t = 1 - 0.17 * cos((hBar - 30) * deg) + 0.24 * cos(2 * hBar * deg) +
               0.32 * cos((3 * hBar + 6) * deg) - 0.20 * cos((4 * hBar - 63) * deg);
    double sl = 1 + 0.015 * (lBar - 50) * (lBar - 50) / sqrt(20 + (lBar - 50) * (lBar - 50));
    double sc = 1 + 0.045 * cBarP, sh = 1 + 0.015 * cBarP * t;
    double cBarP7 = pow(cBarP, 7);
    double rt = -2 * sqrt(cBarP7 / (cBarP7 + 6103515625.0)) *
                sin(60 * exp(-pow((hBar - 275) / 25, 2)) * deg);
    double l = dL / sl, c = dC / sc, h = dH / sh;
    return sqrt(l * l + c * c + h * h + rt * c * h);
}

// Closest palette entry by CIEDE2000.
String nearestStockColor(uint32_t color, const char *const *palette) {
    double source[3];
    toLab(color, source);
    const char *best = palette[0];
    double bestDistance = 1e18;
    for (size_t n = 0; n < PALETTE_SIZE; n++) {
        double lab[3];
        toLab(strtol(palette[n] + 1, nullptr, 16), lab);
        double d = deltaE2000(source, lab);
        if (d < bestDistance) {
            bestDistance = d;
            best = palette[n];
        }
    }
    return best;
}

String c5Material(const Filament &f) {
    String material = upper(f.material);
    String mods = upper(f.modifiers);
    if (material == "PLA" && mods.indexOf("SILK") >= 0) return "SILK";
    if (material == "TPU") return mods.indexOf("90A") >= 0 ? "TPU-90A" : "TPU-95A";
    int dash = material.indexOf('-');
    String candidates[] = {material, dash > 0 ? material.substring(0, dash) : material};
    for (auto &c : candidates)
        for (const char *known : C5_MATERIALS)
            if (c.equalsIgnoreCase(known)) return known;
    if (material.startsWith("ASA")) return "ASA";
    if (material.indexOf("PLA") >= 0) return "PLA";
    return "";
}

String slotLabel(bool toolchanger, int slot) {
    return toolchanger ? "T" + String(slot - 1) : "Slot " + String(slot);
}

// --- zmod_nfc ------------------------------------------------------------------
//
// zmod_nfc registers Klipper endpoints (zmod_nfc/scan, ...), which Moonraker
// publishes on its HTTP API; no extra port on the printer.

HttpResult moonraker(const PrinterConfig &printer, const String &path, const char *method,
                     const String &body = "", uint32_t timeout = TIMEOUT) {
    return request(printer.baseUrl() + path, method, body, printer.apiKey, timeout);
}

String moonrakerFailure(const HttpResult &result, const JsonDocument &doc) {
    if (result.code == 401 || result.code == 403)
        return "Moonraker refused the request: add this reader to [authorization] trusted_clients, "
               "or enter a Moonraker API key";
    return describeFailure(result, doc);
}

const char *const NO_ZMOD_NFC = "zmod_nfc is not installed on the printer, or too old for network readers";

String scanBody(const Tag &tag, const String &readerName, int slot) {
    JsonDocument doc;
    doc["reader"] = readerName;
    if (slot > 0) doc["slot"] = slot;
    doc["uid"] = tag.uidHex();
    doc["kind"] = tagKindName(tag.kind);
    if (tag.kind == TagKind::Type5) {
        doc["block_size"] = tag.blockSize;
    } else {
        doc["sak"] = tag.sak;
        doc["atqa"] = toHex(tag.atqa, 2);
    }
    doc["memory"] = base64(tag.memory.data(), tag.memory.size());
    if (tag.kind == TagKind::Type4) doc["ndef"] = base64(tag.ndef.data(), tag.ndef.size());
    if (tag.kind == TagKind::MifareClassic) {
        JsonObject keys = doc["sector_keys"].to<JsonObject>();
        for (int s = 0; s < MAX_SECTORS; s++)
            if (tag.sectorRead(s)) keys[String(s)] = tag.sectorKey[s];
    }
    doc["incomplete"] = tag.incomplete;
    String body;
    serializeJson(doc, body);
    return body;
}

void describeOutcome(PrinterReply &reply, JsonObjectConst remote) {
    reply.ok = true;
    reply.outcome = remote["outcome"] | "";
    reply.title = remote["scan"]["title"] | "";
    reply.spoolId = remote["scan"]["spool_id"] | 0;
    String spoolman = remote["spoolman"] | "";
    if (reply.outcome == "prompt")
        reply.message = "Pick the slot on the printer's screen (or below)";
    else if (reply.outcome == "assigned")
        reply.message = remote["message"] | "Assigned to the printer's only slot";
    else if (reply.outcome == "no_slot")
        reply.ok = false, reply.message = remote["message"] | "The printer has no such slot";
    else if (reply.outcome == "disabled")
        reply.ok = false, reply.message = "zmod_nfc is disabled on the printer (NFC_ENABLE turns it on)";
    else if (reply.outcome == "printing")
        reply.ok = false, reply.message = "The printer is printing; zmod_nfc ignores tags while printing";
    else if (reply.outcome == "no_data")
        reply.ok = false, reply.message = "The printer found no filament data on this tag";
    else if (reply.outcome == "incomplete")
        reply.ok = false, reply.message = "The tag left the reader too soon; hold it still and scan again";
    else if (reply.outcome == "error")
        reply.ok = false, reply.message = String("zmod_nfc: ") + (remote["error"] | "scan failed");
    else
        reply.message = reply.outcome;
    if (spoolman.length() && reply.ok) reply.message += " (Spoolman: " + spoolman + ")";
}

PrinterReply zmodNfcScan(const PrinterConfig &printer, const Tag &tag, const String &readerName, int slot) {
    PrinterReply reply;
    HttpResult result = moonraker(printer, "/printer/zmod_nfc/scan", "POST", scanBody(tag, readerName, slot));
    JsonDocument doc;
    deserializeJson(doc, result.body);
    if (!result.ok()) {
        reply.message = result.code == 404 ? String(NO_ZMOD_NFC) : moonrakerFailure(result, doc);
        return reply;
    }
    JsonObjectConst accepted = doc["result"];
    if (!(accepted["accepted"] | false)) {
        describeOutcome(reply, accepted);
        return reply;
    }
    // Decoding and the Spoolman lookup run in the background on the
    // printer; the outcome shows up in printer.zmod_nfc.remote.
    String id = accepted["id"] | "";
    uint32_t deadline = millis() + SCAN_TIMEOUT;
    while ((int32_t)(millis() - deadline) < 0) {
        delay(300);
        HttpResult poll = moonraker(printer, "/printer/objects/query?zmod_nfc=remote", "GET");
        JsonDocument status;
        if (!poll.ok() || deserializeJson(status, poll.body)) continue;
        JsonObjectConst remote = status["result"]["status"]["zmod_nfc"]["remote"];
        if (id == (remote["id"] | "")) {
            describeOutcome(reply, remote);
            return reply;
        }
    }
    reply.message = "The printer took the tag but did not report back in time";
    return reply;
}

PrinterReply zmodNfcAssign(const PrinterConfig &printer, const Tag &tag, int slot, const String &readerName) {
    PrinterReply reply;
    String body = "{\"slot\":" + String(slot) + "}";
    for (int attempt = 0; attempt < 2; attempt++) {
        HttpResult result = moonraker(printer, "/printer/zmod_nfc/assign", "POST", body);
        JsonDocument doc;
        deserializeJson(doc, result.body);
        if (result.ok()) {
            reply.ok = true;
            reply.message = doc["result"]["message"] | "Assigned";
            reply.title = doc["result"]["scan"]["title"] | "";
            return reply;
        }
        reply.message = moonrakerFailure(result, doc);
        // The printer's prompt timed out or was dismissed: send the tag again.
        if (attempt == 0 && reply.message.indexOf("no scanned tag") >= 0) {
            PrinterReply scan = zmodNfcScan(printer, tag, readerName, 0);
            if (scan.ok && scan.outcome == "prompt") continue;
            return scan;
        }
        break;
    }
    return reply;
}

SlotList zmodNfcSlots(const PrinterConfig &printer) {
    SlotList list;
    HttpResult result = moonraker(printer, "/printer/zmod_nfc/info", "GET");
    JsonDocument doc;
    deserializeJson(doc, result.body);
    if (!result.ok()) {
        list.error = result.code == 404 ? String(NO_ZMOD_NFC) : moonrakerFailure(result, doc);
        return list;
    }
    for (JsonObjectConst s : doc["result"]["slots"].as<JsonArrayConst>()) {
        SlotInfo info;
        info.slot = s["slot"] | 0;
        info.label = s["label"] | "";
        info.material = s["material"] | "";
        info.color = s["color"] | "";
        info.loaded = s["loaded"] | false;
        list.slots.push_back(info);
    }
    list.ok = true;
    return list;
}

// --- Zmod through Moonraker ------------------------------------------------------

bool zmodColor(const PrinterConfig &printer, JsonDocument &status, String &error) {
    HttpResult result = moonraker(printer, "/printer/objects/query?zmod_color", "GET");
    JsonDocument doc;
    if (deserializeJson(doc, result.body) || !result.ok()) {
        error = moonrakerFailure(result, doc);
        return false;
    }
    JsonObjectConst zmod = doc["result"]["status"]["zmod_color"];
    if (zmod.isNull() || zmod.size() == 0) {
        error = "Zmod's zmod_color is not loaded on this printer";
        return false;
    }
    status.set(zmod);
    return true;
}

SlotList basicSlots(const PrinterConfig &printer) {
    SlotList list;
    JsonDocument status;
    if (!zmodColor(printer, status, list.error)) return list;
    bool toolchanger = (status["total_tools"] | 1) > 1;
    int count = constrain(status["color_limit"] | 4, 1, 16);
    for (int n = 1; n <= count; n++) {
        SlotInfo info;
        info.slot = n;
        info.label = slotLabel(toolchanger, n);
        for (JsonObjectConst s : status["slots"].as<JsonArrayConst>()) {
            if ((s["ID"] | 0) != n) continue;
            info.material = s["Material"] | "";
            String hex = s["HEX"] | "";
            hex.replace("#", "");
            info.color = hex.substring(0, 6);
            info.loaded = s["hasFilament"] | false;
        }
        list.slots.push_back(info);
    }
    list.ok = true;
    return list;
}

// Moonraker lane_data, read by OrcaSlicer: the entry of the 0-based lane
// (found by its "lane" field, as keys differ between printers) gets the
// spool's details; other writers' keys stay.
String writeLaneData(const PrinterConfig &printer, int index, const Tag &tag, const Filament &f,
                     const String &material, const String &color) {
    HttpResult current = moonraker(printer, "/server/database/item?namespace=lane_data", "GET");
    JsonDocument entries;
    if (current.ok()) deserializeJson(entries, current.body);
    JsonObjectConst value = entries["result"]["value"];
    String lane = String(index), key = "lane" + String(index + 1);
    for (JsonPairConst entry : value) {
        if (entry.value()["lane"].as<String>() == lane) {
            key = entry.key().c_str();
            break;
        }
    }
    JsonDocument doc;
    doc["namespace"] = "lane_data";
    doc["key"] = key;
    JsonObject out = doc["value"].to<JsonObject>();
    if (value[key].is<JsonObjectConst>()) out.set(value[key].as<JsonObjectConst>());
    out["lane"] = lane;
    out["material"] = material;
    out["color"] = "#" + color;
    if (f.spoolId > 0 && f.spoolId < MAX_SPOOLMAN_ID) out["spool_id"] = f.spoolId;
    else out.remove("spool_id");
    if (f.brand.length()) out["vendor"] = f.brand, out["vendor_name"] = f.brand;
    else out.remove("vendor"), out.remove("vendor_name");
    if (f.nozzleTemp()) out["nozzle_temp"] = f.nozzleTemp();
    else out.remove("nozzle_temp");
    if (f.bedTemp) out["bed_temp"] = f.bedTemp;
    else out.remove("bed_temp");
    out["zmod_nfc_name"] = f.title();
    out["zmod_nfc_uid"] = tag.uidHex();
    String body;
    serializeJson(doc, body);
    HttpResult result = moonraker(printer, "/server/database/item", "POST", body);
    return result.ok() ? "" : "lane_data not written (" + String(result.code) + ")";
}

PrinterReply basicAssign(const PrinterConfig &printer, const Tag &tag, const Filament &f, int slot) {
    PrinterReply reply;
    if (!f.hasFilamentData()) {
        reply.message = f.spoolId ? "The tag only names Spoolman spool " + String(f.spoolId) +
                                        "; the zmod_nfc method looks those up"
                                  : "No filament data on this tag";
        return reply;
    }
    JsonDocument status;
    if (!zmodColor(printer, status, reply.message)) return reply;
    String label = slotLabel((status["total_tools"] | 1) > 1, slot);
    String current, currentColor = "FFFFFF";
    for (JsonObjectConst s : status["slots"].as<JsonArrayConst>())
        if ((s["ID"] | 0) == slot) {
            current = s["Material"] | "";
            String hex = s["HEX"] | "FFFFFF";
            hex.replace("#", "");
            currentColor = hex.substring(0, 6);
        }
    String material = resolveMaterial(f.material, status["valid_types"]);
    String notes;
    if (!material.length()) {
        if (!current.length() || current == "?" || current == "NONE") {
            reply.message = f.material + " is not a material this printer knows";
            return reply;
        }
        material = current;
        notes += f.material + " is unknown to the printer; kept " + current + ". ";
    }
    String color = f.color >= 0 ? f.colorHex() : currentColor;
    JsonArrayConst palette = status["palette"];
    if (palette.size()) {
        String snapped = nearestPalette(strtol(color.c_str(), nullptr, 16), palette);
        if (snapped.length() && snapped != color) {
            notes += "#" + color + " stored as palette color #" + snapped + ". ";
            color = snapped;
        }
    }
    JsonDocument cmd;
    cmd["script"] = "CHANGE_ZCOLOR SLOT=" + String(slot) + " HEX=" + color + " TYPE=" + material + " SILENT=1";
    String body;
    serializeJson(cmd, body);
    HttpResult result = moonraker(printer, "/printer/gcode/script", "POST", body, 15000);
    if (!result.ok()) {
        JsonDocument doc;
        deserializeJson(doc, result.body);
        reply.message = moonrakerFailure(result, doc);
        return reply;
    }
    String lane = writeLaneData(printer, slot - 1, tag, f, material, color);
    reply.ok = true;
    reply.message = label + " is now " + material + " #" + color + ". " + notes + lane;
    reply.message.trim();
    return reply;
}

// --- Stock Flashforge firmware -------------------------------------------------

void stockAuth(const PrinterConfig &printer, JsonDocument &doc) {
    doc["serialNumber"] = printer.serial;
    doc["checkCode"] = printer.checkCode;
}

bool stockCall(const PrinterConfig &printer, const char *path, JsonDocument &body, JsonDocument &reply,
               String &error) {
    String text;
    serializeJson(body, text);
    HttpResult result = request(printer.baseUrl() + path, "POST", text);
    if (deserializeJson(reply, result.body) || !result.ok()) {
        error = describeFailure(result, reply);
        return false;
    }
    int code = reply["code"] | -1;
    if (code != 0) {
        error = reply["message"] | ("the printer answered code " + String(code)).c_str();
        if (error.indexOf("heck") >= 0 || code == 1) error += " (check the serial number and check code)";
        return false;
    }
    return true;
}

SlotList stockSlots(const PrinterConfig &printer) {
    SlotList list;
    JsonDocument body, reply;
    stockAuth(printer, body);
    if (!stockCall(printer, "/detail", body, reply, list.error)) return list;
    JsonObjectConst station = reply["detail"]["matlStationInfo"];
    for (JsonObjectConst s : station["slotInfos"].as<JsonArrayConst>()) {
        SlotInfo info;
        info.slot = s["slotId"] | 0;
        info.label = "Slot " + String(info.slot);
        info.material = s["materialName"] | "";
        String color = s["materialColor"] | "";
        color.replace("#", "");
        info.color = color;
        info.loaded = s["hasFilament"] | false;
        list.slots.push_back(info);
    }
    if (list.slots.empty())
        for (int n = 1; n <= 4; n++) {
            SlotInfo info;
            info.slot = n;
            info.label = "Slot " + String(n);
            list.slots.push_back(info);
        }
    list.ok = true;
    return list;
}

PrinterReply stockAssign(const PrinterConfig &printer, const Filament &f, int slot) {
    PrinterReply reply;
    if (!f.hasFilamentData()) {
        reply.message = f.spoolId ? "The tag only names Spoolman spool " + String(f.spoolId) +
                                        "; stock firmware cannot look it up"
                                  : "No filament data on this tag";
        return reply;
    }
    String material;
    bool c5 = printer.model == Model::Creator5;
    if (c5) {
        material = c5Material(f);
        if (!material.length()) {
            reply.message = f.material + " is not a Creator 5 material";
            return reply;
        }
    } else {
        // The AD5X shows any material name.
        material = upper(f.material.length() ? f.material : "PLA");
    }
    uint32_t color = f.color >= 0 ? f.color : 0xFFFFFF;
    String rgb = nearestStockColor(color, c5 ? C5_PALETTE : AD5X_PALETTE);
    JsonDocument body, response;
    stockAuth(printer, body);
    body["payload"]["cmd"] = "msConfig_cmd";
    JsonObject args = body["payload"]["args"].to<JsonObject>();
    args["slot"] = slot;
    args["mt"] = material;
    args["rgb"] = rgb;
    if (!stockCall(printer, "/control", body, response, reply.message)) return reply;
    reply.ok = true;
    reply.message = "Slot " + String(slot) + " is now " + material + " " + rgb;
    if (f.color >= 0 && rgb.substring(1) != f.colorHex())
        reply.message += " (the screen's closest color to #" + f.colorHex() + ")";
    return reply;
}

// --- Snapmaker U1 (paxx12's Extended firmware) ----------------------------------
//
// On the Extended firmware OpenRFID hands each head's tag reads to Klipper's
// filament_detect/set endpoint; this reader does the same for its head, so
// the screen, the slicer and SpoolLink treat the scan as the head's own read.
// SpoolLink (when Spoolman is set up) then looks the card UID up in Spoolman
// by itself and, if found, replaces the head's filament with the spool's.

constexpr int U1_HEADS = 4;
// How long SpoolLink gets to find the spool in Spoolman.
constexpr uint32_t SPOOLLINK_WAIT = 4000;

bool u1Status(const PrinterConfig &printer, JsonDocument &status, String &error, bool sensors = false) {
    String path = "/printer/objects/query?print_stats=state&filament_detect=state&print_task_config="
                  "filament_vendor,filament_type,filament_sub_type,filament_color_rgba,filament_spool_id";
    if (sensors)
        for (int n = 0; n < U1_HEADS; n++) path += "&filament_motion_sensor%20e" + String(n) + "_filament";
    HttpResult result = moonraker(printer, path, "GET");
    JsonDocument doc;
    if (deserializeJson(doc, result.body) || !result.ok()) {
        error = moonrakerFailure(result, doc);
        return false;
    }
    status.set(doc["result"]["status"]);
    if (status["filament_detect"].isNull() || status["print_task_config"].isNull()) {
        error = "No Snapmaker filament_detect on this printer (is it a Snapmaker U1?)";
        return false;
    }
    return true;
}

bool u1HasSpoolLink(const PrinterConfig &printer) {
    HttpResult result = moonraker(printer, "/server/info", "GET");
    JsonDocument doc;
    if (!result.ok() || deserializeJson(doc, result.body)) return false;
    for (JsonVariantConst c : doc["result"]["components"].as<JsonArrayConst>())
        if (c.as<String>() == "spoollink") return true;
    return false;
}

// What a head holds, from print_task_config: "Sunlu PLA Matte" and its color.
SlotInfo u1Head(const JsonDocument &status, int head) {
    JsonObjectConst ptc = status["print_task_config"];
    SlotInfo info;
    info.slot = head;
    info.label = "Head " + String(head);
    String vendor = ptc["filament_vendor"][head - 1] | "NONE";
    String type = ptc["filament_type"][head - 1] | "NONE";
    String sub = ptc["filament_sub_type"][head - 1] | "";
    if (type.length() && type != "NONE") {
        info.material = (vendor != "NONE" && vendor != "Generic" ? vendor + " " : String()) + type;
        if (sub.length() && sub != "NONE") info.material += " " + sub;
        String rgba = ptc["filament_color_rgba"][head - 1] | "";
        info.color = rgba.substring(0, 6);
    }
    return info;
}

SlotList u1Slots(const PrinterConfig &printer) {
    SlotList list;
    JsonDocument status;
    if (!u1Status(printer, status, list.error, true)) return list;
    for (int head = 1; head <= U1_HEADS; head++) {
        SlotInfo info = u1Head(status, head);
        String sensor = "filament_motion_sensor e" + String(head - 1) + "_filament";
        info.loaded = status[sensor]["filament_detected"] | false;
        list.slots.push_back(info);
    }
    list.ok = true;
    return list;
}

const char *u1CardType(TagKind kind) {
    switch (kind) {
    case TagKind::Type2: return "NTAG";
    case TagKind::MifareClassic: return "M1";
    default: return tagKindName(kind);
    }
}

PrinterReply u1Assign(const PrinterConfig &printer, const Tag &tag, const Filament &f, int head) {
    PrinterReply reply;
    if (head < 1 || head > U1_HEADS) {
        reply.message = "The U1 has heads 1 to " + String(U1_HEADS);
        return reply;
    }
    JsonDocument status;
    if (!u1Status(printer, status, reply.message)) return reply;
    String state = status["print_stats"]["state"] | "";
    if (state == "printing" || state == "paused") {
        reply.message = "The printer is printing; heads are not changed while it prints";
        return reply;
    }
    bool spoolLink = u1HasSpoolLink(printer);
    int spoolId = f.spoolId > 0 && f.spoolId < MAX_SPOOLMAN_ID ? f.spoolId : 0;
    // Without filament data only Spoolman can tell what the spool is.
    if (!f.hasFilamentData() && !spoolLink) {
        reply.message = spoolId ? "The tag only names Spoolman spool " + String(spoolId) +
                                      "; turn on Spoolman in the Extended firmware's settings to look it up"
                                : "No filament data on this tag, and Spoolman is not set up on the printer";
        return reply;
    }

    // As OpenRFID sends a read: brand "Generic" and no subtype unless it is
    // Snapmaker's own filament, so the slicer finds a matching profile.
    JsonDocument body;
    body["channel"] = head - 1;
    JsonObject info = body["info"].to<JsonObject>();
    JsonArray uid = info["CARD_UID"].to<JsonArray>();
    for (int n = 0; n < tag.uidLen; n++) uid.add(tag.uid[n]);
    info["CARD_TYPE"] = u1CardType(tag.kind);
    if (f.hasFilamentData()) {
        bool snapmaker = f.brand.equalsIgnoreCase("Snapmaker");
        info["VENDOR"] = snapmaker ? "Snapmaker" : "Generic";
        info["MAIN_TYPE"] = upper(f.material.length() ? f.material : String("PLA"));
        info["SUB_TYPE"] = snapmaker ? f.modifiers : String();
        if (f.nozzleMin) info["HOTEND_MIN_TEMP"] = f.nozzleMin;
        if (f.nozzleMax) info["HOTEND_MAX_TEMP"] = f.nozzleMax;
        if (f.bedTemp) info["BED_TEMP"] = f.bedTemp;
        info["ALPHA"] = f.alpha;
        info["RGB_1"] = f.color >= 0 ? f.color : 0xFFFFFF;
    }
    String text;
    serializeJson(body, text);
    HttpResult result = moonraker(printer, "/printer/filament_detect/set", "POST", text);
    if (!result.ok()) {
        JsonDocument doc;
        deserializeJson(doc, result.body);
        reply.message = result.code == 404 ? String("No filament_detect/set endpoint: this needs the Extended firmware "
                                                    "(pick the U1's Stock firmware method without it)")
                                           : moonrakerFailure(result, doc);
        return reply;
    }

    String notes;
    if (spoolId && spoolLink) {
        // Binds the card to the tag's spool in Spoolman, then loads that spool.
        JsonDocument cmd;
        cmd["script"] = "SET_SPOOL_ID CHANNEL=" + String(head - 1) + " SPOOL_ID=" + String(spoolId);
        String script;
        serializeJson(cmd, script);
        HttpResult set = moonraker(printer, "/printer/gcode/script", "POST", script, 15000);
        if (!set.ok()) notes = " Spoolman spool " + String(spoolId) + " was not set (" + String(set.code) + ").";
    }
    String error;
    if (spoolLink) {
        uint32_t deadline = millis() + SPOOLLINK_WAIT;
        do {
            delay(400);
            if (!u1Status(printer, status, error)) break;
        } while ((status["print_task_config"]["filament_spool_id"][head - 1] | 0) == 0 &&
                 (int32_t)(millis() - deadline) < 0);
    } else {
        u1Status(printer, status, error);
    }
    SlotInfo now = u1Head(status, head);
    int spool = status["print_task_config"]["filament_spool_id"][head - 1] | 0;
    if (!f.hasFilamentData() && !spool) {
        reply.message = "SpoolLink found no Spoolman spool for this tag, and the tag has no filament data";
        return reply;
    }
    reply.ok = true;
    reply.title = now.material;
    reply.spoolId = spool;
    reply.message = now.label + " is now " + now.material + " #" + now.color;
    if (spool) reply.message += " (Spoolman spool " + String(spool) + ")";
    else if (spoolLink) reply.message += " (no Spoolman spool for this tag)";
    reply.message += notes;
    return reply;
}

// --- Klipper: Happy Hare, AFC and plain lane_data ---------------------------------
//
// Each reader covers four gates, lanes or tools of the printer
// (PrinterConfig::unit). Spoolman, when the printer has it, is reached
// through the MMU software's own commands, so its gate map stays in charge.

// A G-code parameter value: Klipper splits parameters shell-style and cuts
// comments at '#' and ';'.
String gcodeValue(String text) {
    for (const char *bad : {"\"", "#", ";", "\n", "\r"}) text.replace(bad, "");
    text.trim();
    return "\"" + text + "\"";
}

PrinterReply runGcode(const PrinterConfig &printer, const String &script) {
    PrinterReply reply;
    JsonDocument cmd;
    cmd["script"] = script;
    String body;
    serializeJson(cmd, body);
    HttpResult result = moonraker(printer, "/printer/gcode/script", "POST", body, 20000);
    if (result.ok()) {
        reply.ok = true;
        return reply;
    }
    JsonDocument doc;
    deserializeJson(doc, result.body);
    reply.message = moonrakerFailure(result, doc);
    return reply;
}

bool queryObjects(const PrinterConfig &printer, const String &query, JsonDocument &status, String &error) {
    HttpResult result = moonraker(printer, "/printer/objects/query?" + query, "GET");
    JsonDocument doc;
    if (deserializeJson(doc, result.body) || !result.ok()) {
        error = moonrakerFailure(result, doc);
        return false;
    }
    status.set(doc["result"]["status"]);
    return true;
}

bool hasComponent(const PrinterConfig &printer, const char *name) {
    HttpResult result = moonraker(printer, "/server/info", "GET");
    JsonDocument doc;
    if (!result.ok() || deserializeJson(doc, result.body)) return false;
    for (JsonVariantConst c : doc["result"]["components"].as<JsonArrayConst>())
        if (c.as<String>() == name) return true;
    return false;
}

int tagSpoolId(const Filament &f) { return f.spoolId > 0 && f.spoolId < MAX_SPOOLMAN_ID ? f.spoolId : 0; }

// "RRGGBB" from "#rrggbb", "rrggbbaa" and the like; "" for color names.
String hexColor(String color) {
    color.replace("#", "");
    if (color.length() < 6) return "";
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char)color[i])) return "";
    color = color.substring(0, 6);
    color.toUpperCase();
    return color;
}

String filamentName(const Filament &f) {
    if (f.name.length()) return f.name;
    String name = upper(f.material);
    if (f.modifiers.length()) name += " " + f.modifiers;
    return name;
}

String describeSlot(const SlotInfo &s, int spoolId) {
    String text = s.label + " is now " + (s.material.length() ? s.material : String("?"));
    if (s.color.length()) text += " #" + s.color;
    if (spoolId > 0) text += " (Spoolman spool " + String(spoolId) + ")";
    return text;
}

// Happy Hare --------------------------------------------------------------------

const char *const HH_FIELDS = "mmu=num_gates,gate_status,gate_material,gate_color,gate_color_rgb,"
                              "gate_filament_name,gate_spool_id,spoolman_support";

bool hhStatus(const PrinterConfig &printer, JsonDocument &mmu, String &error) {
    JsonDocument status;
    if (!queryObjects(printer, HH_FIELDS, status, error)) return false;
    mmu.set(status["mmu"]);
    if (mmu.isNull() || !(mmu["num_gates"] | 0)) {
        error = "Happy Hare is not installed on this printer (no mmu object)";
        return false;
    }
    return true;
}

SlotInfo hhGate(const PrinterConfig &printer, const JsonDocument &mmu, int slot) {
    int gate = printer.slotIndex(slot);
    SlotInfo info;
    info.slot = slot;
    info.label = "Gate " + String(gate);
    info.material = mmu["gate_material"][gate] | "";
    info.color = hexColor(mmu["gate_color"][gate] | "");
    // Colors given by name ("red") are only in gate_color_rgb.
    JsonArrayConst rgb = mmu["gate_color_rgb"][gate];
    if (!info.color.length() && rgb.size() >= 3) {
        char hex[8];
        snprintf(hex, sizeof(hex), "%02X%02X%02X", (int)(rgb[0].as<float>() * 255 + 0.5f),
                 (int)(rgb[1].as<float>() * 255 + 0.5f), (int)(rgb[2].as<float>() * 255 + 0.5f));
        info.color = hex;
    }
    info.loaded = (mmu["gate_status"][gate] | -1) > 0;
    return info;
}

String hhNoGates(const PrinterConfig &printer, int gates) {
    return "Happy Hare has " + String(gates) + " gates; this reader starts at gate " +
           String(printer.slotIndex(1));
}

SlotList hhSlots(const PrinterConfig &printer) {
    SlotList list;
    JsonDocument mmu;
    if (!hhStatus(printer, mmu, list.error)) return list;
    int gates = mmu["num_gates"] | 0;
    for (int slot = 1; slot <= SLOT_COUNT && printer.slotIndex(slot) < gates; slot++)
        list.slots.push_back(hhGate(printer, mmu, slot));
    if (list.slots.empty()) {
        list.error = hhNoGates(printer, gates);
        return list;
    }
    list.ok = true;
    return list;
}

PrinterReply hhAssign(const PrinterConfig &printer, const Tag &tag, const Filament &f, int slot) {
    PrinterReply reply;
    JsonDocument mmu;
    if (!hhStatus(printer, mmu, reply.message)) return reply;
    int gate = printer.slotIndex(slot), gates = mmu["num_gates"] | 0;
    if (gate >= gates) {
        reply.message = "Happy Hare has gates 0 to " + String(gates - 1) + "; there is no gate " + String(gate);
        return reply;
    }
    String spoolman = mmu["spoolman_support"] | "off";
    int spoolId = spoolman != "off" ? tagSpoolId(f) : 0;
    String script;
    if (spoolman == "pull") {
        // The gate map comes from Spoolman: only a spool can be assigned.
        if (!spoolId) {
            reply.message = "Happy Hare takes its gate map from Spoolman (spoolman_support: pull); "
                            "only tags naming a Spoolman spool can change a gate";
            return reply;
        }
        script = "MMU_SPOOLMAN GATE=" + String(gate) + " SPOOLID=" + String(spoolId) + " QUIET=1";
    } else {
        if (!f.hasFilamentData() && !spoolId) {
            reply.message = tagSpoolId(f) ? "The tag only names a Spoolman spool, and Happy Hare has no Spoolman"
                                          : "No filament data on this tag";
            return reply;
        }
        script = "MMU_GATE_MAP GATE=" + String(gate) + " QUIET=1";
        if (f.material.length()) script += " MATERIAL=" + gcodeValue(upper(f.material));
        if (f.color >= 0) script += " COLOR=" + f.colorHex();
        if (f.hasFilamentData()) script += " NAME=" + gcodeValue(filamentName(f));
        if (f.brand.length()) script += " VENDOR=" + gcodeValue(f.brand);
        if (f.nozzleTemp()) script += " TEMP=" + String(f.nozzleTemp());
        // Without a spool on the tag, the gate's old spool no longer applies.
        if (spoolman != "off") script += " SPOOLID=" + String(spoolId ? spoolId : -1);
        // Kept with the gate by Happy Hare 4 (older versions ignore it).
        script += " RFID=" + tag.uidHex();
    }
    PrinterReply run = runGcode(printer, script);
    if (!run.ok) return run;
    if (!hhStatus(printer, mmu, reply.message)) return reply;
    SlotInfo now = hhGate(printer, mmu, slot);
    reply.ok = true;
    reply.spoolId = mmu["gate_spool_id"][gate] | 0;
    reply.title = mmu["gate_filament_name"][gate] | "";
    reply.message = describeSlot(now, reply.spoolId);
    if (spoolId && reply.spoolId != spoolId)
        reply.ok = false, reply.message += "; Happy Hare did not take Spoolman spool " + String(spoolId);
    return reply;
}

// AFC --------------------------------------------------------------------------------

struct AfcLane {
    String name;
    String object;      // Klipper object: "AFC_stepper lane1", "AFC_lane lane1", ...
};

// The printer's lanes in AFC's order, and whether AFC has Spoolman.
bool afcLanes(const PrinterConfig &printer, std::vector<AfcLane> &lanes, bool &spoolman, String &error) {
    JsonDocument status;
    if (!queryObjects(printer, "AFC=lanes,spoolman", status, error)) return false;
    JsonArrayConst names = status["AFC"]["lanes"];
    if (!names.size()) {
        error = "AFC is not installed on this printer, or has no lanes";
        return false;
    }
    String url = status["AFC"]["spoolman"] | "";
    spoolman = url.length() > 0;
    HttpResult result = moonraker(printer, "/printer/objects/list", "GET");
    JsonDocument list;
    if (deserializeJson(list, result.body) || !result.ok()) {
        error = moonrakerFailure(result, list);
        return false;
    }
    lanes.clear();
    for (JsonVariantConst n : names) {
        AfcLane lane;
        lane.name = n.as<String>();
        for (JsonVariantConst o : list["result"]["objects"].as<JsonArrayConst>()) {
            String object = o.as<String>();
            if (object.startsWith("AFC_") && object.endsWith(" " + lane.name)) {
                lane.object = object;
                break;
            }
        }
        lanes.push_back(lane);
    }
    return true;
}

bool afcLaneStatus(const PrinterConfig &printer, const AfcLane &lane, JsonDocument &out, String &error) {
    if (!lane.object.length()) {
        error = "no Klipper object for AFC lane " + lane.name;
        return false;
    }
    String object = lane.object;
    object.replace(" ", "%20");
    JsonDocument status;
    if (!queryObjects(printer, object + "=material,color,spool_id,load,prep,filament_name", status, error))
        return false;
    out.set(status[lane.object]);
    return true;
}

SlotInfo afcSlot(int slot, const AfcLane &lane, const JsonDocument &status) {
    SlotInfo info;
    info.slot = slot;
    info.label = lane.name;
    info.material = status["material"] | "";
    info.color = hexColor(status["color"] | "");
    info.loaded = (status["load"] | false) || (status["prep"] | false);
    return info;
}

String afcNoLanes(const PrinterConfig &printer, size_t count) {
    return "AFC has " + String(count) + " lanes; this reader starts at lane " + String(printer.slotIndex(1) + 1);
}

SlotList afcSlots(const PrinterConfig &printer) {
    SlotList list;
    std::vector<AfcLane> lanes;
    bool spoolman;
    if (!afcLanes(printer, lanes, spoolman, list.error)) return list;
    for (int slot = 1; slot <= SLOT_COUNT && printer.slotIndex(slot) < (int)lanes.size(); slot++) {
        const AfcLane &lane = lanes[printer.slotIndex(slot)];
        JsonDocument status;
        if (!afcLaneStatus(printer, lane, status, list.error)) {
            list.slots.clear();
            return list;
        }
        list.slots.push_back(afcSlot(slot, lane, status));
    }
    if (list.slots.empty()) {
        if (!list.error.length()) list.error = afcNoLanes(printer, lanes.size());
        return list;
    }
    list.ok = true;
    return list;
}

PrinterReply afcAssign(const PrinterConfig &printer, const Filament &f, int slot) {
    PrinterReply reply;
    std::vector<AfcLane> lanes;
    bool spoolman;
    if (!afcLanes(printer, lanes, spoolman, reply.message)) return reply;
    int index = printer.slotIndex(slot);
    if (index >= (int)lanes.size()) {
        reply.message = "AFC has " + String(lanes.size()) + " lanes; there is no lane " + String(index + 1);
        return reply;
    }
    const AfcLane &lane = lanes[index];
    JsonDocument status;
    if (!afcLaneStatus(printer, lane, status, reply.message)) return reply;
    int spoolId = spoolman ? tagSpoolId(f) : 0;
    String laneArg = " LANE=" + lane.name;
    String script;
    if (spoolId) {
        // AFC's staged next-load hook consumes next_spool_id when the lane loads.
        // Use the dedicated command rather than directly overwriting the lane.
        script = "SET_NEXT_SPOOL_ID SPOOL_ID=" + String(spoolId);
    } else {
        if (!f.hasFilamentData()) {
            reply.message = tagSpoolId(f) ? "The tag only names a Spoolman spool, and AFC has no Spoolman"
                                          : "No filament data on this tag";
            return reply;
        }
        // The lane's old Spoolman spool no longer applies; clearing it also
        // clears the lane's values, so it goes first.
        if (spoolman && (status["spool_id"] | 0)) script += "SET_SPOOL_ID" + laneArg + " SPOOL_ID=\n";
        script += "SET_MATERIAL" + laneArg + " MATERIAL=" +
                  gcodeValue(upper(f.material.length() ? f.material : String("PLA")));
        if (f.color >= 0) script += "\nSET_COLOR" + laneArg + " COLOR=" + f.colorHex();
        if (f.nozzleTemp() || f.bedTemp) {
            script += "\nAFC_SET_SPOOL_TEMP" + laneArg;
            if (f.nozzleTemp()) script += " EXTRUDER_TEMP=" + String(f.nozzleTemp());
            if (f.bedTemp) script += " BED_TEMP=" + String(f.bedTemp);
        }
    }
    PrinterReply run = runGcode(printer, script);
    if (!run.ok) return run;
    if (spoolId) {
        reply.ok = true;
        reply.spoolId = spoolId;
        reply.title = "Spoolman spool " + String(spoolId);
        reply.message = "Spoolman spool " + String(spoolId) + " is staged for the next AFC load on " + lane.name;
        return reply;
    }
    if (!afcLaneStatus(printer, lane, status, reply.message)) return reply;
    SlotInfo now = afcSlot(slot, lane, status);
    reply.ok = true;
    reply.spoolId = status["spool_id"] | 0;
    reply.title = status["filament_name"] | "";
    reply.message = describeSlot(now, reply.spoolId);
    // AFC reports refusals (spool already on another lane, no weight left)
    // in its console, not as errors.
    if (spoolId && reply.spoolId != spoolId)
        reply.ok = false, reply.message += "; AFC did not take Spoolman spool " + String(spoolId) + " (see its console)";
    return reply;
}

// Snapmaker U1, stock firmware ---------------------------------------------------------
//
// Without the Extended firmware there is no filament_detect/set endpoint, but
// Snapmaker's own SET_PRINT_FILAMENT_CONFIG sets a head's filament the way an
// edit on the screen does (vendor, type, subtype, color), through Moonraker.

PrinterReply u1StockAssign(const PrinterConfig &printer, const Filament &f, int head) {
    PrinterReply reply;
    if (head < 1 || head > U1_HEADS) {
        reply.message = "The U1 has heads 1 to " + String(U1_HEADS);
        return reply;
    }
    if (!f.hasFilamentData()) {
        reply.message = f.spoolId ? "The tag only names Spoolman spool " + String(f.spoolId) +
                                        "; the U1's stock firmware cannot look it up"
                                  : "No filament data on this tag";
        return reply;
    }
    JsonDocument status;
    if (!u1Status(printer, status, reply.message)) return reply;
    String state = status["print_stats"]["state"] | "";
    if (state == "printing" || state == "paused") {
        reply.message = "The printer is printing; heads are not changed while it prints";
        return reply;
    }
    // As the Extended method names a read: Snapmaker's own filament with its
    // subtype, anything else as Generic, so the slicer finds a profile.
    bool snapmaker = f.brand.equalsIgnoreCase("Snapmaker");
    String type = upper(f.material.length() ? f.material : String("PLA"));
    char rgba[12];
    snprintf(rgba, sizeof(rgba), "%06X%02X", (unsigned)(f.color >= 0 ? f.color & 0xFFFFFF : 0xFFFFFF), f.alpha);
    String script = "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=" + String(head - 1) +
                    " VENDOR=" + gcodeValue(snapmaker ? "Snapmaker" : "Generic") +
                    " FILAMENT_TYPE=" + gcodeValue(type) +
                    " FILAMENT_SUBTYPE=" + gcodeValue(snapmaker ? f.modifiers : String()) +
                    " FILAMENT_COLOR_RGBA=" + rgba;
    PrinterReply run = runGcode(printer, script);
    if (!run.ok) {
        if (run.message.indexOf("Unknown command") >= 0)
            run.message = "this printer has no SET_PRINT_FILAMENT_CONFIG (is it a Snapmaker U1?)";
        return run;
    }
    if (!u1Status(printer, status, reply.message)) return reply;
    SlotInfo now = u1Head(status, head);
    String want = String(rgba).substring(0, 6);
    String nowType = status["print_task_config"]["filament_type"][head - 1] | "";
    if (!upper(nowType).equals(type) || !now.color.equalsIgnoreCase(want)) {
        reply.message = now.label + " is still " + (now.material.length() ? now.material + " #" + now.color : "empty") +
                        "; the printer did not take the change";
        return reply;
    }
    reply.ok = true;
    reply.title = now.material;
    reply.message = now.label + " is now " + now.material + " #" + now.color;
    return reply;
}

// Qidi Box (Plus4 / Q2 / Max4, stock firmware) ---------------------------------------
//
// The Box keeps each slot in Klipper's save_variables as three row ids into
// the printer's officiall_filas_list.cfg (Qidi's spelling): filament_slotN
// ([filaN] profiles), color_slotN ([colordict]) and vendor_slotN
// ([vendor_list]); N is 0-15 over up to four Boxes. Its RFID reader, Qidi
// Studio and the screen write those same variables, so a scan does too, with
// stock SAVE_VARIABLE (as qidi-spoolman-sync and HelixScreen do).

// Whether any word (2+ letters) of words appears in text, ignoring case.
bool hasWord(const String &text, const String &words) {
    String h = upper(text), w = upper(words);
    int start = 0;
    while (start < (int)w.length()) {
        int end = w.indexOf(' ', start);
        if (end < 0) end = w.length();
        String word = w.substring(start, end);
        if (word.length() > 1 && h.indexOf(word) >= 0) return true;
        start = end + 1;
    }
    return false;
}

struct QidiFila {
    int id;
    String name;    // "PLA Rapido"
    String type;    // "PLA"
};

struct QidiTables {
    std::vector<QidiFila> filas;
    std::vector<std::pair<int, uint32_t>> colors;   // colordict: id -> RRGGBB
    std::vector<std::pair<int, String>> vendors;    // vendor_list: id -> name
};

const char *const QIDI_TABLES = "officiall_filas_list.cfg";

bool qidiTables(const PrinterConfig &printer, QidiTables &t, String &error) {
    HttpResult result = moonraker(printer, "/server/files/config/" + String(QIDI_TABLES), "GET");
    if (!result.ok()) {
        error = result.code == 404 ? "the printer has no " + String(QIDI_TABLES) + " (is it a Qidi with a Box?)"
                                   : "could not read " + String(QIDI_TABLES) + " (" + String(result.code) + ")";
        return false;
    }
    String section;
    QidiFila fila{0, "", ""};
    auto flush = [&]() {
        if (fila.id > 0 && fila.name.length() && fila.type.length()) t.filas.push_back(fila);
        fila = QidiFila{0, "", ""};
    };
    int start = 0;
    const String &text = result.body;
    while (start < (int)text.length()) {
        int end = text.indexOf('\n', start);
        if (end < 0) end = text.length();
        String line = text.substring(start, end);
        start = end + 1;
        line.trim();
        if (!line.length() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            flush();
            section = line.substring(1, line.indexOf(']'));
            section.trim();
            if (section.startsWith("fila")) fila.id = section.substring(4).toInt();
            continue;
        }
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq), value = line.substring(eq + 1);
        key.trim();
        value.trim();
        if (fila.id > 0) {
            if (key == "filament") fila.name = value;
            else if (key == "type") fila.type = upper(value);
        } else if (section == "colordict") {
            int32_t c = parseColor(value);
            if (c >= 0) t.colors.push_back({key.toInt(), (uint32_t)c});
        } else if (section == "vendor_list") {
            t.vendors.push_back({key.toInt(), value});
        }
    }
    flush();
    if (t.filas.empty() || t.colors.empty()) {
        error = String(QIDI_TABLES) + " has no filament or color list";
        return false;
    }
    return true;
}

// The Box's filament for a tag: profiles of the tag's material (else its base
// material), preferring one named for the tag's modifiers ("PLA Silk"), then
// "Basic", then the bare material name, then the lowest id.
int qidiFila(const QidiTables &t, const Filament &f) {
    String material = upper(f.material.length() ? f.material : String("PLA"));
    if (material == "PLA+" || material == "PLA PLUS") material = "PLA";
    if (material == "PETG+") material = "PETG";
    int dash = material.indexOf('-');
    for (const String &type : {material, dash > 0 ? material.substring(0, dash) : material}) {
        const QidiFila *best = nullptr;
        int bestScore = -1;
        for (auto &fila : t.filas) {
            if (fila.type != type) continue;
            String name = upper(fila.name);
            int score = 0;
            if (f.modifiers.length() && hasWord(name, f.modifiers)) score += 4;
            if (name.indexOf("BASIC") >= 0) score += 2;
            if (name == type) score += 1;
            if (score > bestScore || (score == bestScore && fila.id < best->id)) best = &fila, bestScore = score;
        }
        if (best) return best->id;
    }
    return 0;
}

int qidiColor(const QidiTables &t, uint32_t color) {
    double source[3];
    toLab(color, source);
    int best = t.colors[0].first;
    double bestDistance = 1e18;
    for (auto &c : t.colors) {
        double lab[3];
        toLab(c.second, lab);
        double d = deltaE2000(source, lab);
        if (d < bestDistance) bestDistance = d, best = c.first;
    }
    return best;
}

// The tag's brand when the Box knows it, else Generic.
int qidiVendor(const QidiTables &t, const Filament &f) {
    int generic = 0;
    for (auto &v : t.vendors) {
        if (v.second.equalsIgnoreCase("Generic")) generic = v.first;
        if (f.brand.length() && (v.second.equalsIgnoreCase(f.brand) || upper(f.brand).startsWith(upper(v.second))))
            return v.first;
    }
    return generic;
}

const char *const QIDI_NEWER = "this Qidi firmware keeps Box slots in a newer format (filament codes such as "
                               "\"QFA00\") that LaneTag does not support yet";

bool qidiStatus(const PrinterConfig &printer, JsonDocument &vars, String &state, String &error) {
    JsonDocument status;
    if (!queryObjects(printer, "save_variables&print_stats=state", status, error)) return false;
    vars.set(status["save_variables"]["variables"]);
    state = status["print_stats"]["state"] | "";
    if (vars.isNull() || !(vars["box_count"] | 0)) {
        error = "the printer reports no Qidi Box (save_variables box_count)";
        return false;
    }
    int boxes = vars["box_count"] | 0;
    if (printer.unit > boxes) {
        error = "the printer has " + String(boxes) + (boxes == 1 ? " Box" : " Boxes") + "; there is no Box " +
                String(printer.unit);
        return false;
    }
    // Newer firmware stores codes and color lists instead of row ids.
    for (int slot = 1; slot <= SLOT_COUNT; slot++) {
        String n = String(printer.slotIndex(slot));
        if (vars["filament_slot" + n].is<const char *>() && !String(vars["filament_slot" + n] | "").toInt() &&
            String(vars["filament_slot" + n] | "") != "0") {
            error = QIDI_NEWER;
            return false;
        }
        if (vars["color_slot" + n].is<JsonArrayConst>()) {
            error = QIDI_NEWER;
            return false;
        }
    }
    return true;
}

// save_variables ids are ints; Qidi Studio writes them quoted ("5").
int qidiId(JsonVariantConst v) {
    if (v.is<int>()) return v.as<int>();
    if (v.is<const char *>()) return String(v.as<const char *>()).toInt();
    return 0;
}

SlotInfo qidiSlot(const PrinterConfig &printer, const QidiTables &t, const JsonDocument &vars, int slot) {
    String n = String(printer.slotIndex(slot));
    SlotInfo info;
    info.slot = slot;
    info.label = slotName(printer, slot);
    int fila = qidiId(vars["filament_slot" + n]), color = qidiId(vars["color_slot" + n]);
    for (auto &f : t.filas)
        if (f.id == fila) info.material = f.name;
    if (info.material.length()) {
        for (auto &c : t.colors) {
            if (c.first != color) continue;
            char hex[8];
            snprintf(hex, sizeof(hex), "%06X", (unsigned)c.second);
            info.color = hex;
        }
    }
    info.loaded = qidiId(vars["slot" + n]) > 0;
    return info;
}

SlotList qidiSlots(const PrinterConfig &printer) {
    SlotList list;
    QidiTables t;
    JsonDocument vars;
    String state;
    if (!qidiStatus(printer, vars, state, list.error) || !qidiTables(printer, t, list.error)) return list;
    for (int slot = 1; slot <= printer.slotCount(); slot++) list.slots.push_back(qidiSlot(printer, t, vars, slot));
    list.ok = true;
    return list;
}

PrinterReply qidiAssign(const PrinterConfig &printer, const Filament &f, int slot) {
    PrinterReply reply;
    if (slot < 1 || slot > printer.slotCount()) {
        reply.message = "no such slot";
        return reply;
    }
    if (!f.hasFilamentData()) {
        reply.message = f.spoolId ? "The tag only names Spoolman spool " + String(f.spoolId) +
                                        "; the Qidi Box cannot look it up"
                                  : "No filament data on this tag";
        return reply;
    }
    QidiTables t;
    JsonDocument vars;
    String state;
    if (!qidiStatus(printer, vars, state, reply.message) || !qidiTables(printer, t, reply.message)) return reply;
    if (state == "printing" || state == "paused") {
        reply.message = "The printer is printing; Box slots are not changed while it prints";
        return reply;
    }
    int fila = qidiFila(t, f);
    if (!fila) {
        reply.message = f.material + " is not a material in the printer's filament list";
        return reply;
    }
    uint32_t color = f.color >= 0 ? f.color & 0xFFFFFF : 0xFFFFFF;
    int colorId = qidiColor(t, color), vendor = qidiVendor(t, f);
    String n = String(printer.slotIndex(slot));
    String script = "SAVE_VARIABLE VARIABLE=filament_slot" + n + " VALUE=" + String(fila) +
                    "\nSAVE_VARIABLE VARIABLE=color_slot" + n + " VALUE=" + String(colorId) +
                    "\nSAVE_VARIABLE VARIABLE=vendor_slot" + n + " VALUE=" + String(vendor);
    PrinterReply run = runGcode(printer, script);
    if (!run.ok) return run;
    if (!qidiStatus(printer, vars, state, reply.message)) return reply;
    SlotInfo now = qidiSlot(printer, t, vars, slot);
    if (qidiId(vars["filament_slot" + n]) != fila || qidiId(vars["color_slot" + n]) != colorId) {
        reply.message = now.label + " did not take the change";
        return reply;
    }
    reply.ok = true;
    reply.title = now.material;
    reply.message = now.label + " is now " + now.material + " #" + now.color;
    char hex[8];
    snprintf(hex, sizeof(hex), "%06X", (unsigned)color);
    if (f.color >= 0 && now.color != hex) reply.message += " (the Box's closest color to #" + String(hex) + ")";
    return reply;
}

// Plain lane_data ---------------------------------------------------------------------

// The printer's tools: Klipper's extruder, extruder1, extruder2, ...
int klipperTools(const PrinterConfig &printer, String &error) {
    HttpResult result = moonraker(printer, "/printer/objects/list", "GET");
    JsonDocument doc;
    if (deserializeJson(doc, result.body) || !result.ok()) {
        error = moonrakerFailure(result, doc);
        return -1;
    }
    int tools = 0;
    for (JsonVariantConst o : doc["result"]["objects"].as<JsonArrayConst>()) {
        String name = o.as<String>();
        if (!name.startsWith("extruder")) continue;
        bool numbered = true;
        for (unsigned i = 8; i < name.length(); i++)
            if (!isdigit((unsigned char)name[i])) numbered = false;
        if (numbered) tools++;
    }
    if (!tools) error = "Klipper reports no extruder";
    return tools;
}

String noTool(const PrinterConfig &printer, int tools, int index) {
    return "the printer has " + String(tools) + (tools == 1 ? " tool (T0)" : " tools (T0-T" + String(tools - 1) + ")") +
           "; there is no T" + String(index);
}

// The printer's lanes: one per extruder, or as many as lane_data already
// describes when that is more (a filament changer such as the AD5X's IFS
// feeds four lanes into one extruder). Returns 0 on failure.
int laneCount(const PrinterConfig &printer, JsonDocument &doc, String &error) {
    int tools = klipperTools(printer, error);
    if (tools <= 0) return 0;
    HttpResult result = moonraker(printer, "/server/database/item?namespace=lane_data", "GET");
    deserializeJson(doc, result.body);
    // Moonraker answers 404 while nothing has written lane_data yet.
    if (!result.ok() && result.code != 404) {
        error = moonrakerFailure(result, doc);
        return 0;
    }
    int lanes = tools;
    for (JsonPairConst entry : doc["result"]["value"].as<JsonObjectConst>()) {
        String lane = entry.value()["lane"] | "";
        if (lane.length() && isDigit(lane[0])) lanes = max(lanes, (int)lane.toInt() + 1);
    }
    return lanes;
}

SlotList laneDataSlots(const PrinterConfig &printer) {
    SlotList list;
    JsonDocument doc;
    int lanes = laneCount(printer, doc, list.error);
    if (lanes <= 0) return list;
    if (printer.slotIndex(1) >= lanes) {
        list.error = noTool(printer, lanes, printer.slotIndex(1));
        return list;
    }
    for (int slot = 1; slot <= SLOT_COUNT && printer.slotIndex(slot) < lanes; slot++) {
        SlotInfo info;
        info.slot = slot;
        info.label = slotName(printer, slot);
        String lane = String(printer.slotIndex(slot));
        for (JsonPairConst entry : doc["result"]["value"].as<JsonObjectConst>()) {
            if (entry.value()["lane"].as<String>() != lane) continue;
            info.material = entry.value()["material"] | "";
            info.color = hexColor(entry.value()["color"] | "");
            info.loaded = info.material.length() > 0;
        }
        list.slots.push_back(info);
    }
    list.ok = true;
    return list;
}

PrinterReply laneDataAssign(const PrinterConfig &printer, const Tag &tag, const Filament &f, int slot) {
    PrinterReply reply;
    int index = printer.slotIndex(slot);
    JsonDocument doc;
    int lanes = laneCount(printer, doc, reply.message);
    if (lanes <= 0) return reply;
    if (index >= lanes) {
        reply.message = noTool(printer, lanes, index);
        return reply;
    }
    int spoolId = tagSpoolId(f);
    // A printer with one lane prints only from it: its spool is the one
    // Spoolman tracks. With more lanes, which one prints is not known here.
    bool active = spoolId && lanes == 1 && hasComponent(printer, "spoolman");
    if (!f.hasFilamentData() && !active) {
        reply.message = spoolId ? "The tag only names Spoolman spool " + String(spoolId) +
                                      ", which can only become the active spool of a single-tool printer "
                                      "(with Spoolman set up in Moonraker)"
                                : "No filament data on this tag";
        return reply;
    }
    String label = slotName(printer, slot);
    if (f.hasFilamentData()) {
        String material = upper(f.material.length() ? f.material : String("PLA"));
        String color = f.color >= 0 ? f.colorHex() : String("FFFFFF");
        String error = writeLaneData(printer, index, tag, f, material, color);
        if (error.length()) {
            reply.message = error;
            return reply;
        }
        reply.message = label + " is now " + material + " #" + color + " in lane_data";
    }
    if (active) {
        String body = "{\"spool_id\":" + String(spoolId) + "}";
        HttpResult result = moonraker(printer, "/server/spoolman/spool_id", "POST", body);
        String sep = reply.message.length() ? "; " : "";
        if (!result.ok()) {
            JsonDocument doc;
            deserializeJson(doc, result.body);
            reply.message += sep + "Spoolman spool not set: " + moonrakerFailure(result, doc);
            return reply;
        }
        reply.spoolId = spoolId;
        reply.message += sep + "Spoolman spool " + String(spoolId) + " is active";
    }
    reply.ok = true;
    reply.title = f.title();
    return reply;
}

}  // namespace

PrinterReply sendScan(const PrinterConfig &printer, const Tag &tag, const String &readerName, int slot) {
    if (printer.method == Method::ZmodNfc) return zmodNfcScan(printer, tag, readerName, slot);
    PrinterReply reply;
    reply.message = "only the zmod_nfc method takes whole scans";
    return reply;
}

PrinterReply assignSlot(const PrinterConfig &printer, const Tag &tag, const Filament &filament, int slot,
                        const String &readerName) {
    switch (printer.method) {
    case Method::ZmodNfc: return zmodNfcAssign(printer, tag, slot, readerName);
    case Method::ZmodBasic: return basicAssign(printer, tag, filament, slot);
    case Method::Snapmaker: return u1Assign(printer, tag, filament, slot);
    case Method::SnapmakerStock: return u1StockAssign(printer, filament, slot);
    case Method::Bambu: return bambuAssign(printer, filament, slot);
    case Method::HappyHare: return hhAssign(printer, tag, filament, slot);
    case Method::Afc: return afcAssign(printer, filament, slot);
    case Method::LaneData: return laneDataAssign(printer, tag, filament, slot);
    case Method::CrealityCfs: return crealityAssign(printer, filament, slot);
    case Method::AnycubicAce: return anycubicAssign(printer, filament, slot);
    case Method::QidiBox: return qidiAssign(printer, filament, slot);
    default: return stockAssign(printer, filament, slot);
    }
}

void cancelPrompt(const PrinterConfig &printer) {
    if (printer.method == Method::ZmodNfc) moonraker(printer, "/printer/zmod_nfc/cancel", "POST", "{}");
}

SlotList listSlots(const PrinterConfig &printer) {
    switch (printer.method) {
    case Method::ZmodNfc: return zmodNfcSlots(printer);
    case Method::ZmodBasic: return basicSlots(printer);
    case Method::Snapmaker:
    case Method::SnapmakerStock: return u1Slots(printer);
    case Method::Bambu: return bambuSlots(printer);
    case Method::HappyHare: return hhSlots(printer);
    case Method::Afc: return afcSlots(printer);
    case Method::LaneData: return laneDataSlots(printer);
    case Method::CrealityCfs: return crealitySlots(printer);
    case Method::AnycubicAce: return anycubicSlots(printer);
    case Method::QidiBox: return qidiSlots(printer);
    default: return stockSlots(printer);
    }
}

bool learnSlots(PrinterConfig &printer, const SlotList &list) {
    if (printer.model != Model::Klipper || !list.ok || list.slots.empty()) return false;
    int found = min((int)list.slots.size(), SLOT_COUNT);
    if (found == printer.slotsFound) return false;
    printer.slotsFound = found;
    printer.slot = constrain(printer.slot, printer.canAsk() ? 0 : 1, found);
    return true;
}

PrinterReply testReply(const SlotList &slots) {
    PrinterReply reply;
    reply.ok = slots.ok;
    size_t n = slots.slots.size();
    reply.message = slots.ok ? "Connected; " + String(n) + (n == 1 ? " slot" : " slots") : slots.error;
    return reply;
}

void slotListToJson(const SlotList &list, JsonObject out) {
    out["ok"] = list.ok;
    if (!list.ok) out["error"] = list.error;
    JsonArray slots = out["slots"].to<JsonArray>();
    for (auto &s : list.slots) {
        JsonObject o = slots.add<JsonObject>();
        o["slot"] = s.slot;
        o["label"] = s.label;
        o["material"] = s.material;
        o["color"] = s.color;
        o["loaded"] = s.loaded;
    }
}
