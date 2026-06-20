#include "Deinflector.h"

#include <algorithm>
#include <cstring>

namespace {

struct Rule {
  const char* from;
  const char* to;
  WordCondition condIn;
  WordCondition condOut;
};

// Japanese deinflection rules — linguistic facts covering common
// verb/adjective conjugation patterns.  Organized by conjugation type.
// Rules are tried against the END of the input string; if "from" matches
// the suffix, it's replaced by "to" and the candidate gets condOut.
//
// condIn gates which prior condition allows this rule to fire (DICT
// means "accept from raw surface form — no prior rule needed").
// condOut is what the output candidate is tagged as for further chaining
// or for final dictionary lookup.

// clang-format off
static constexpr Rule kRules[] = {
    // ── Ichidan (v1) verbs: dictionary form ends in -る ──────────
    // Negative
    {"\xe3\x81\xaa\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ない→る
    // Past
    {"\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // た→る
    // Te-form
    {"\xe3\x81\xa6", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // て→る
    // Polite
    {"\xe3\x81\xbe\xe3\x81\x99", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ます→る
    {"\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ません→る
    // Passive/potential
    {"\xe3\x82\x89\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // られる→る
    // Causative
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // させる→る
    // Volitional
    {"\xe3\x82\x88\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // よう→る
    // Conditional
    {"\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // れば→る
    // Desire
    {"\xe3\x81\x9f\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // たい→る
    // Progressive て+いる contracted
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ている→る

    // ── Godan (v5) verbs: dictionary form ends in う-row kana ────
    // Past/te-form: consonant-stem euphonic changes
    // K-column: く→いた/いて
    {"\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いた→く
    {"\xe3\x81\x84\xe3\x81\xa6", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いて→く
    // G-column: ぐ→いだ/いで
    {"\xe3\x81\x84\xe3\x81\xa0", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // いだ→ぐ
    {"\xe3\x81\x84\xe3\x81\xa7", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // いで→ぐ
    // S-column: す→した/して
    {"\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // した→す
    {"\xe3\x81\x97\xe3\x81\xa6", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // して→す
    // T-column: つ→った/って
    {"\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // った→つ
    {"\xe3\x81\xa3\xe3\x81\xa6", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // って→つ
    // N-column: ぬ→んだ/んで
    {"\xe3\x82\x93\xe3\x81\xa0", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // んだ→ぬ
    {"\xe3\x82\x93\xe3\x81\xa7", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // んで→ぬ
    // B-column: ぶ→んだ/んで
    {"\xe3\x82\x93\xe3\x81\xa0", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // んだ→ぶ
    {"\xe3\x82\x93\xe3\x81\xa7", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // んで→ぶ
    // M-column: む→んだ/んで
    {"\xe3\x82\x93\xe3\x81\xa0", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んだ→む
    {"\xe3\x82\x93\xe3\x81\xa7", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んで→む
    // R-column: る→った/って (godan る, not ichidan)
    {"\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // った→る
    {"\xe3\x81\xa3\xe3\x81\xa6", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // って→る
    // U-column: う→った/って
    {"\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // った→う
    {"\xe3\x81\xa3\xe3\x81\xa6", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // って→う

    // Godan negative: replace あ-row ending + ない
    {"\xe3\x81\x8b\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かない→く
    {"\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がない→ぐ
    {"\xe3\x81\x95\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // さない→す
    {"\xe3\x81\x9f\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たない→つ
    {"\xe3\x81\xaa\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なない→ぬ
    {"\xe3\x81\xb0\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばない→ぶ
    {"\xe3\x81\xbe\xe3\x81\xaa\xe3\x81\x84", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まない→む
    {"\xe3\x82\x89\xe3\x81\xaa\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らない→る
    {"\xe3\x82\x8f\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わない→う

    // Godan polite -ます: replace い-row + ます
    {"\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // きます→く
    {"\xe3\x81\x8e\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ぎます→ぐ
    {"\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // します→す
    {"\xe3\x81\xa1\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ちます→つ
    {"\xe3\x81\xab\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // にます→ぬ
    {"\xe3\x81\xb3\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // びます→ぶ
    {"\xe3\x81\xbf\xe3\x81\xbe\xe3\x81\x99", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // みます→む
    {"\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x99", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // ります→る
    {"\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // います→う

    // Godan masu-stem (連用形): bare い-row ending (used for compound verbs and nominal forms)
    {"\xe3\x81\x8d", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // き→く
    {"\xe3\x81\x8e", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ぎ→ぐ
    {"\xe3\x81\x97", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // し→す
    {"\xe3\x81\xa1", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ち→つ
    {"\xe3\x81\xab", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // に→ぬ
    {"\xe3\x81\xb3", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // び→ぶ
    {"\xe3\x81\xbf", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // み→む
    {"\xe3\x82\x8a", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // り→る
    {"\xe3\x81\x84", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // い→う

    // Godan passive: replace あ-row + れる
    {"\xe3\x81\x8b\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かれる→く
    {"\xe3\x81\x8c\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がれる→ぐ
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // される→す
    {"\xe3\x81\x9f\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たれる→つ
    {"\xe3\x81\xaa\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なれる→ぬ
    {"\xe3\x81\xb0\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばれる→ぶ
    {"\xe3\x81\xbe\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まれる→む
    {"\xe3\x82\x89\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // られる→る
    {"\xe3\x82\x8f\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // われる→う

    // Godan causative: replace あ-row + せる
    {"\xe3\x81\x8b\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かせる→く
    {"\xe3\x81\x8c\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がせる→ぐ
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // させる→す
    {"\xe3\x81\x9f\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たせる→つ
    {"\xe3\x81\xaa\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なせる→ぬ
    {"\xe3\x81\xb0\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばせる→ぶ
    {"\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // ませる→む
    {"\xe3\x82\x89\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らせる→る
    {"\xe3\x82\x8f\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わせる→う

    // Godan potential: replace え-row + る
    {"\xe3\x81\x91\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // ける→く
    {"\xe3\x81\x92\xe3\x82\x8b", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // げる→ぐ
    {"\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // せる→す
    {"\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // てる→つ
    {"\xe3\x81\xad\xe3\x82\x8b", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // ねる→ぬ
    {"\xe3\x81\xb9\xe3\x82\x8b", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // べる→ぶ
    {"\xe3\x82\x81\xe3\x82\x8b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // める→む
    {"\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // れる→る
    {"\xe3\x81\x88\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // える→う

    // ── I-adjective (adj-i): dictionary form ends in -い ─────────
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // くない→い (negative)
    {"\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // かった→い (past)
    {"\xe3\x81\x8f\xe3\x81\xa6", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // くて→い (te-form)
    {"\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // ければ→い (conditional)
    {"\xe3\x81\x8f", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // く→い (adverbial)
    {"\xe3\x81\x95", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // さ→い (nominal)

    // ── Suru (する) irregular verb ───────────────────────────────
    {"\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // した→する
    {"\xe3\x81\x97\xe3\x81\xa6", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // して→する
    {"\xe3\x81\x97\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // しない→する
    {"\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // します→する
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // される→する
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // させる→する
    {"\xe3\x81\xa7\xe3\x81\x8d\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // できる→する
    {"\xe3\x81\x97\xe3\x82\x88\xe3\x81\x86", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // しよう→する
    {"\xe3\x81\x99\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // すれば→する

    // ── Kuru (来る/くる) irregular verb ──────────────────────────
    {"\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // きた→くる
    {"\xe3\x81\x8d\xe3\x81\xa6", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // きて→くる
    {"\xe3\x81\x93\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // こない→くる
    {"\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // きます→くる
    {"\xe3\x81\x93\xe3\x82\x89\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // こられる→くる
    {"\xe3\x81\x93\xe3\x82\x88\xe3\x81\x86", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // こよう→くる
    {"\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // くれば→くる

    // ── Iku (行く) irregular te/ta forms ─────────────────────────
    {"\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // った→く (行った→行く, duplicates godan る but also matches く)
    {"\xe3\x81\xa3\xe3\x81\xa6", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // って→く (行って→行く)
};
// clang-format on

static constexpr size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

bool endsWith(const std::string& str, const char* suffix, size_t suffixLen) {
  if (str.size() < suffixLen) return false;
  return std::memcmp(str.data() + str.size() - suffixLen, suffix, suffixLen) == 0;
}

}  // namespace

std::vector<DeinflectionCandidate> Deinflector::deinflect(const std::string& surface) {
  std::vector<DeinflectionCandidate> results;
  if (surface.empty()) return results;

  // The surface form itself is always a candidate (it may be a dictionary form).
  results.push_back({surface, WordCondition::DICT});

  // Apply rules iteratively — each new candidate can be further deinflected
  // (enabling rule chaining like causative-passive-past). Limit depth to
  // prevent runaway on pathological input.
  constexpr size_t kMaxCandidates = 64;

  for (size_t i = 0; i < results.size() && results.size() < kMaxCandidates; i++) {
    // Copy by value — push_back below can reallocate the vector,
    // invalidating any reference into results[].
    const std::string text = results[i].text;
    const WordCondition cond = results[i].condition;

    for (size_t r = 0; r < kRuleCount; r++) {
      const Rule& rule = kRules[r];

      // Condition check: DICT matches any input; otherwise must match exactly.
      if (rule.condIn != WordCondition::DICT && rule.condIn != cond) continue;

      const size_t fromLen = std::strlen(rule.from);
      if (!endsWith(text, rule.from, fromLen)) continue;

      if (text.size() < fromLen) continue;

      std::string newText = text.substr(0, text.size() - fromLen) + rule.to;

      // Avoid duplicates.
      bool found = false;
      for (const auto& existing : results) {
        if (existing.text == newText) {
          found = true;
          break;
        }
      }
      if (!found) {
        results.push_back({std::move(newText), rule.condOut});
      }
    }
  }

  return results;
}
