#include "DictIndex.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

bool DictIndex::isAvailable() {
  return Storage.exists(IDX_PATH) && Storage.exists(DAT_PATH);
}

bool DictIndex::lookupInFile(const char* headword, const char* idxPath, const char* datPath, DictEntry& out) {
  HalFile idxFile;
  if (!Storage.openFileForRead("DICT", idxPath, idxFile)) {
    return false;
  }

  const size_t fileSize = idxFile.size();
  if (fileSize < sizeof(DictIndexRecord)) {
    idxFile.close();
    return false;
  }

  const size_t recordCount = fileSize / sizeof(DictIndexRecord);

  char key[DictIndexRecord::HEADWORD_SIZE];
  std::memset(key, 0, sizeof(key));
  const size_t len = std::strlen(headword);
  if (len >= sizeof(key)) {
    idxFile.close();
    return false;
  }
  std::memcpy(key, headword, len);

  size_t lo = 0;
  size_t hi = recordCount;
  DictIndexRecord rec;

  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    idxFile.seek(mid * sizeof(DictIndexRecord));
    if (idxFile.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec)) {
      break;
    }

    const int cmp = std::memcmp(key, rec.headword, DictIndexRecord::HEADWORD_SIZE);
    if (cmp < 0) {
      hi = mid;
    } else if (cmp > 0) {
      lo = mid + 1;
    } else {
      // Found a match — scan backwards to find the first record with this headword
      size_t first = mid;
      while (first > 0) {
        idxFile.seek((first - 1) * sizeof(DictIndexRecord));
        DictIndexRecord prevRec;
        if (idxFile.read(reinterpret_cast<uint8_t*>(&prevRec), sizeof(prevRec)) != sizeof(prevRec)) break;
        if (std::memcmp(key, prevRec.headword, DictIndexRecord::HEADWORD_SIZE) != 0) break;
        first--;
      }

      // Collect all entries with this headword, pick highest priority and merge
      HalFile datFile;
      if (!Storage.openFileForRead("DICT", datPath, datFile)) {
        idxFile.close();
        return false;
      }

      struct Entry { std::string def; uint8_t priority; };
      std::vector<Entry> entries;
      constexpr int kMaxEntries = 5;

      for (size_t idx = first; idx < recordCount && static_cast<int>(entries.size()) < kMaxEntries; idx++) {
        idxFile.seek(idx * sizeof(DictIndexRecord));
        DictIndexRecord r;
        if (idxFile.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != sizeof(r)) break;
        if (std::memcmp(key, r.headword, DictIndexRecord::HEADWORD_SIZE) != 0) break;

        datFile.seek(r.offset);
        std::string def;
        def.resize(r.length);
        if (datFile.read(reinterpret_cast<uint8_t*>(def.data()), r.length) != static_cast<int>(r.length)) continue;

        entries.push_back({std::move(def), r.priority});
      }

      datFile.close();
      idxFile.close();

      if (entries.empty()) return false;

      // Sort by priority (highest first)
      for (size_t a = 0; a < entries.size(); a++) {
        for (size_t b = a + 1; b < entries.size(); b++) {
          if (entries[b].priority > entries[a].priority) {
            std::swap(entries[a], entries[b]);
          }
        }
      }

      out.headword = headword;
      out.priority = entries[0].priority;
      if (entries.size() == 1) {
        out.definition = std::move(entries[0].def);
      } else {
        // Merge: show best entry, then separator + other entries
        out.definition = entries[0].def;
        for (size_t e = 1; e < entries.size(); e++) {
          out.definition += "\n\n---\n" + entries[e].def;
        }
      }
      return true;
    }
  }

  idxFile.close();
  return false;
}

bool DictIndex::lookupExact(const char* headword, DictEntry& out) {
  if (lookupInFile(headword, IDX_PATH, DAT_PATH, out)) return true;
  if (Storage.exists(GRAMMAR_IDX_PATH)) {
    if (lookupInFile(headword, GRAMMAR_IDX_PATH, GRAMMAR_DAT_PATH, out)) return true;
  }
  if (Storage.exists(NAMES_IDX_PATH)) {
    return lookupInFile(headword, NAMES_IDX_PATH, NAMES_DAT_PATH, out);
  }
  return false;
}
