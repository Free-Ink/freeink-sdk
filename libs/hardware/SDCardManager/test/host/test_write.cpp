#include <SDCardManager.h>

#include <cassert>
#include <cstdio>
#include <string>

class Sink : public Print {
 public:
  size_t write(const uint8_t* bytes, size_t count) override {
    data.append(reinterpret_cast<const char*>(bytes), count);
    return count;
  }

  std::string data;
};

void reset() {
  fakeSd::filesMode = true;
  fakeSd::files = {{"/state", "previous"}, {"/unrelated", "untouched"}};
  fakeSd::mutation = fakeSd::cutAfter = fakeSd::failMutation = fakeSd::openWriters = 0;
  fakeSd::fileSyncFails = fakeSd::fileCloseFails = false;
  fakeSd::maxPrint = std::numeric_limits<size_t>::max();
  fakeSd::renameFailures.clear();
  fakeSd::removeFailures.clear();
}
int main() {
  SDCardManager card;
  assert(card.begin());
  reset();
  assert(card.writeFile("/state", "replacement"));
  const auto operations = fakeSd::mutation;
  assert(fakeSd::files.at("/state") == "replacement");
  assert(!fakeSd::files.count("/state.bak"));
  for (unsigned cut = 1; cut <= operations; ++cut) {
    reset();
    fakeSd::cutAfter = cut;
    try {
      card.writeFile("/state", "replacement");
      assert(false);
    } catch (unsigned) {
    }
    fakeSd::cutAfter = 0;
    fakeSd::openWriters = 0;  // Reboot releases the interrupted process's handles.
    assert(card.recoverWrittenFile("/state"));
    const auto recovered = card.readFile("/state");
    assert(recovered == (cut == operations ? "replacement" : "previous"));
    assert(fakeSd::files.at("/unrelated") == "untouched");
    assert(card.writeFile("/state", "retry"));
    assert(card.readFile("/state") == "retry");
  }
  for (unsigned fail = 1; fail <= operations; ++fail) {
    reset();
    fakeSd::failMutation = fail;
    assert(!card.writeFile("/state", "replacement"));
    fakeSd::failMutation = 0;
    assert(card.recoverWrittenFile("/state"));
    assert(card.readFile("/state") == "previous");
    assert(fakeSd::files.at("/unrelated") == "untouched");
  }
  for (int failure = 0; failure < 3; ++failure) {
    reset();
    fakeSd::fileCloseFails = failure == 0;
    fakeSd::fileSyncFails = failure == 1;
    if (failure == 2) fakeSd::maxPrint = 3;
    assert(!card.writeFile("/state", "replacement"));
    assert(fakeSd::openWriters == 0);
    assert(fakeSd::files.at("/state") == "previous");
  }
  reset();
  fakeSd::files.erase("/state");
  fakeSd::files["/state.bak"] = "previous";
  fakeSd::failMutation = 1;
  assert(!card.writeFile("/state", "replacement"));
  assert(fakeSd::files.at("/state.bak") == "previous");
  fakeSd::failMutation = 0;
  assert(card.recoverWrittenFile("/state"));
  assert(card.readFile("/state") == "previous");
  reset();
  fakeSd::renameFailures = {{"/state.tmp", "/state"}, {"/state.bak", "/state"}};
  assert(!card.writeFile("/state", "replacement"));
  assert(!fakeSd::files.count("/state"));
  assert(!fakeSd::files.count("/state.tmp"));
  assert(fakeSd::files.at("/state.bak") == "previous");

  reset();
  fakeSd::renameFailures = {{"/state.tmp", "/state"}, {"/state.bak", "/state"}};
  fakeSd::removeFailures = {"/state.tmp"};
  assert(!card.writeFile("/state", "replacement"));
  assert(!fakeSd::files.count("/state"));
  assert(fakeSd::files.at("/state.tmp") == "replacement");
  assert(fakeSd::files.at("/state.bak") == "previous");

  reset();
  fakeSd::files["/state.bak"] = "unrelated user backup";
  const auto originalFiles = fakeSd::files;
  assert(card.readFile("/state") == "previous");
  assert(fakeSd::files == originalFiles);  // Generic reads cannot claim sidecar ownership.

  reset();
  fakeSd::files.erase("/state");
  fakeSd::files["/state.bak"] = "recovered";
  const auto fallbackFiles = fakeSd::files;
  const auto mutationsBeforeFallbackReads = fakeSd::mutation;
  assert(card.readFile("/state") == "recovered");
  char buffer[32];
  assert(card.readFileToBuffer("/state", buffer, sizeof(buffer)) == 9);
  assert(std::string(buffer) == "recovered");
  Sink sink;
  assert(card.readFileToStream("/state", sink));
  assert(sink.data == "recovered");
  assert(fakeSd::mutation == mutationsBeforeFallbackReads);
  assert(fakeSd::files == fallbackFiles);

  reset();
  fakeSd::files["/state.bak"] = "older";
  const auto mainPreferredFiles = fakeSd::files;
  const auto mutationsBeforeMainReads = fakeSd::mutation;
  assert(card.readFile("/state") == "previous");
  assert(card.readFileToBuffer("/state", buffer, sizeof(buffer)) == 8);
  assert(std::string(buffer) == "previous");
  sink.data.clear();
  assert(card.readFileToStream("/state", sink));
  assert(sink.data == "previous");
  assert(fakeSd::mutation == mutationsBeforeMainReads);
  assert(fakeSd::files == mainPreferredFiles);

  reset();
  fakeSd::files.erase("/state");
  fakeSd::files["/state.bak"] = "recovered";
  FsFile exactPath;
  assert(!card.openFileForRead("test", "/state", exactPath));
  assert(!exactPath);
  assert(fakeSd::files.at("/state.bak") == "recovered");
  assert(fakeSd::mutation == 0);

  std::puts(
      "PASS: interrupted saves at every mutation, short writes, sync/close/rename/remove failures, recovery and retry");
}
