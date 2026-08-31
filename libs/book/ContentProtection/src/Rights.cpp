#include "Rights.h"

#include "Util.h"
#include "Xml.h"

namespace freeink {
namespace content {

namespace {

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

}  // namespace

bool parseRightsXml(const std::string& xml, Rights* out) {
  // Upfront heap gate for the parse below: the scanner and field extraction
  // materialize substrings proportional to the document (throwing allocations;
  // -fno-exceptions turns a failed grow into abort()). One probe here fails
  // the parse readably instead of aborting mid-extraction on a tight heap.
  if (!heapProbe(xml.size() + 4096)) return false;
  XmlScanner scanner(xml);
  std::string lastElement;
  bool inDisplay = false;
  int64_t durationSecs = -1;
  int64_t fulfillmentEpoch = 0;

  for (;;) {
    XmlScanner::Event ev = scanner.nextEvent();
    if (ev.type == XmlScanner::Event::DONE) break;
    if (ev.type == XmlScanner::Event::ERROR) return false;

    if (ev.type == XmlScanner::Event::START) {
      lastElement = ev.name;
      if (XmlScanner::named(ev.name, "display")) inDisplay = true;
      continue;
    }
    if (ev.type == XmlScanner::Event::END) {
      if (XmlScanner::named(ev.name, "display")) inDisplay = false;
      continue;
    }
    if (ev.type != XmlScanner::Event::TEXT) continue;

    const std::string text = trim(ev.text);
    if (text.empty()) continue;

    if (XmlScanner::named(lastElement, "encryptedKey")) {
      out->encryptedKey = text;
    } else if (XmlScanner::named(lastElement, "user")) {
      out->user = text;
    } else if (XmlScanner::named(lastElement, "device")) {
      out->device = text;
    } else if (XmlScanner::named(lastElement, "fulfillment")) {
      // <fulfillment> holds an id; a sibling <date> may hold the timestamp.
      out->fulfillment = text;
    } else if (XmlScanner::named(lastElement, "voucher")) {
      out->voucher = text;
    } else if (inDisplay && XmlScanner::named(lastElement, "until")) {
      int64_t epoch;
      if (isoToEpoch(text, &epoch)) out->expiresAt = epoch;
    } else if (inDisplay && XmlScanner::named(lastElement, "duration")) {
      durationSecs = strtoll(text.c_str(), nullptr, 10);
    } else if (XmlScanner::named(lastElement, "date")) {
      int64_t epoch;
      if (isoToEpoch(text, &epoch)) fulfillmentEpoch = epoch;
    }
  }

  // keyType rides on the encryptedKey element as an attribute; grab it with a
  // targeted second pass (it's rare and we keep the scanner stateless).
  const size_t ekPos = xml.find("encryptedKey");
  if (ekPos != std::string::npos) {
    const size_t ktPos = xml.find("keyType", ekPos > 16 ? ekPos - 16 : 0);
    if (ktPos != std::string::npos && ktPos < ekPos + 64) {
      const size_t q1 = xml.find('"', ktPos);
      const size_t q2 = q1 == std::string::npos ? q1 : xml.find('"', q1 + 1);
      if (q1 != std::string::npos && q2 != std::string::npos) {
        out->keyType = xml.substr(q1 + 1, q2 - q1 - 1);
      }
    }
  }

  if (out->expiresAt == 0 && durationSecs > 0 && fulfillmentEpoch > 0) {
    out->expiresAt = fulfillmentEpoch + durationSecs;
  }

  return !out->encryptedKey.empty();
}

}  // namespace content
}  // namespace freeink
