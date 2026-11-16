#include <StreamingJsonParser.h>

#include <cassert>
#include <string>

struct Capture {
  int keys = 0, strings = 0, prefixes = 0;
  std::string value;
};
void key(void* context, const char*, size_t) { ++static_cast<Capture*>(context)->keys; }
void string(void* context, const char* value, size_t size) {
  auto& capture = *static_cast<Capture*>(context);
  ++capture.strings;
  capture.value.assign(value, size);
}
void prefix(void* context, const char* value, size_t size) {
  auto& capture = *static_cast<Capture*>(context);
  ++capture.prefixes;
  capture.value.assign(value, size);
}
int main() {
  const std::string longText(6000, 'x');
  for (const bool optIn : {false, true}) {
    Capture capture;
    JsonCallbacks callbacks{&capture, key, string, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    if (optIn) callbacks.onStringPrefix = prefix;
    StreamingJsonParser parser(callbacks);
    const std::string document = "{\"description\":\"" + longText + "\",\"next\":\"short\"}";
    for (char byte : document) parser.feed(&byte, 1);
    assert(!parser.hasError());
    assert(capture.keys == 2 && capture.strings == 1 && capture.value == "short");
    assert(capture.prefixes == (optIn ? 1 : 0));

    capture = {};
    parser.reset();
    const std::string value = "{\"description\":\"" + longText + "\"}";
    parser.feed(value.data(), value.size());
    assert(!parser.hasError() && capture.strings == 0);
    if (optIn)
      assert(capture.value == longText.substr(0, StreamingJsonParser::TOKEN_BUF_SIZE - 1));
    else
      assert(capture.value.empty());

    // Long keys remain rejected even when previews are enabled.
    capture = {};
    parser.reset();
    const std::string longKey = "{\"" + longText + "\":\"ok\"}";
    parser.feed(longKey.data(), longKey.size());
    assert(!parser.hasError() && capture.keys == 0 && capture.prefixes == 0 && capture.strings == 1);
  }
}
