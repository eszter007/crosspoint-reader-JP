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
    // Progressive て+いる and compound auxiliaries
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ている→る
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ていた→る
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xaa\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ていない→る
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x99", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ています→る
    {"\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てきた→る (came to do)
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てくる→る
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x8f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ていく→る (going to do)
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ていった→る
    {"\xe3\x81\xa6\xe3\x81\x8a\xe3\x81\x8f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ておく→る (do in advance)
    {"\xe3\x81\xa6\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てしまう→る
    {"\xe3\x81\xa6\xe3\x81\x97\xe3\x81\xbe\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てしまった→る
    {"\xe3\x81\xa6\xe3\x81\xbf\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てみる→る (try doing)
    {"\xe3\x81\xa6\xe3\x81\xbf\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てみた→る
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てある→る (state result)
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てあった→る
    {"\xe3\x81\xa6\xe3\x82\x82\xe3\x82\x89\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てもらう→る (receive favor)
    {"\xe3\x81\xa6\xe3\x82\x82\xe3\x82\x89\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てもらった→る
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x81\x92\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てあげる→る (give favor)
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てくれる→る (receive favor)
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てくれた→る
    {"\xe3\x81\xa6\xe3\x81\xbb\xe3\x81\x97\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てほしい→る (want someone to do)

    // Godan て-form compound auxiliaries (って+aux for godan verbs)
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // っている→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // っていた→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // ってきた→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // ってくる→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // っている→つ
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // っていた→つ
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ってきた→つ
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // っている→る (godan る)
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // っていた→る
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // ってきた→る
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x84\xe3\x82\x8b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んでいる→む
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x84\xe3\x81\x9f", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んでいた→む
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んできた→む
    {"\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いている→く
    {"\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いていた→く
    {"\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いてきた→く
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // している→す
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // していた→す
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // してきた→す

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

    // Godan polite negative -ません: replace い-row + ません → う-row
    // (ありません→ある). Without these, the ichidan ません→る rule wrongly
    // produces ありる.
    {"\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // きません→く
    {"\xe3\x81\x8e\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ぎません→ぐ
    {"\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // しません→す
    {"\xe3\x81\xa1\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ちません→つ
    {"\xe3\x81\xab\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // にません→ぬ
    {"\xe3\x81\xb3\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // びません→ぶ
    {"\xe3\x81\xbf\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // みません→む
    {"\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // りません→る
    {"\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // いません→う

    // Godan polite negative PAST -ませんでした: replace い-row + ませんでした → う-row
    // (ありませんでした→ある). Must precede the shorter ません rules in coverage.
    {"\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // きませんでした→く
    {"\xe3\x81\x8e\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ぎませんでした→ぐ
    {"\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // しませんでした→す
    {"\xe3\x81\xa1\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ちませんでした→つ
    {"\xe3\x81\xab\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // にませんでした→ぬ
    {"\xe3\x81\xb3\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // びませんでした→ぶ
    {"\xe3\x81\xbf\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // みませんでした→む
    {"\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // りませんでした→る
    {"\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // いませんでした→う

    // Ichidan polite negative past: ませんでした→る (食べませんでした→食べる)
    {"\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ませんでした→る

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

    // Godan volitional: replace お-row + う → dictionary う-row ending
    {"\xe3\x81\x93\xe3\x81\x86", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // こう→く
    {"\xe3\x81\x94\xe3\x81\x86", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ごう→ぐ
    {"\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // そう→す
    {"\xe3\x81\xa8\xe3\x81\x86", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // とう→つ
    {"\xe3\x81\xae\xe3\x81\x86", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // のう→ぬ
    {"\xe3\x81\xbc\xe3\x81\x86", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ぼう→ぶ
    {"\xe3\x82\x82\xe3\x81\x86", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // もう→む
    {"\xe3\x82\x8d\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // ろう→る
    {"\xe3\x81\x8a\xe3\x81\x86", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // おう→う

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

    // Godan passive masu-stem: replace あ-row + れ (continuative before comma, conjunctive)
    {"\xe3\x81\x8b\xe3\x82\x8c", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かれ→く
    {"\xe3\x81\x8c\xe3\x82\x8c", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がれ→ぐ
    {"\xe3\x81\x95\xe3\x82\x8c", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // され→す
    {"\xe3\x81\x9f\xe3\x82\x8c", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たれ→つ
    {"\xe3\x81\xaa\xe3\x82\x8c", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なれ→ぬ
    {"\xe3\x81\xb0\xe3\x82\x8c", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばれ→ぶ
    {"\xe3\x81\xbe\xe3\x82\x8c", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まれ→む
    {"\xe3\x82\x89\xe3\x82\x8c", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // られ→る
    {"\xe3\x82\x8f\xe3\x82\x8c", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // われ→う

    // Godan causative masu-stem: replace あ-row + せ
    {"\xe3\x81\x8b\xe3\x81\x9b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かせ→く
    {"\xe3\x81\x8c\xe3\x81\x9b", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がせ→ぐ
    {"\xe3\x81\x95\xe3\x81\x9b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // させ→す
    {"\xe3\x81\x9f\xe3\x81\x9b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たせ→つ
    {"\xe3\x81\xaa\xe3\x81\x9b", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なせ→ぬ
    {"\xe3\x81\xb0\xe3\x81\x9b", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばせ→ぶ
    {"\xe3\x81\xbe\xe3\x81\x9b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // ませ→む
    {"\xe3\x82\x89\xe3\x81\x9b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らせ→る
    {"\xe3\x82\x8f\xe3\x81\x9b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わせ→う

    // Ichidan passive/potential and causative masu-stem
    {"\xe3\x82\x89\xe3\x82\x8c", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // られ→る
    {"\xe3\x81\x95\xe3\x81\x9b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // させ→る

    // Suru passive/causative masu-stem
    {"\xe3\x81\x95\xe3\x82\x8c", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // され→する
    {"\xe3\x81\x95\xe3\x81\x9b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // させ→する

    // Ichidan masu-stem (連用形): stem appears before commas, auxiliaries.
    // Add る to try the dictionary form. These え-row endings are common
    // ichidan stems: め→める, べ→べる, け→ける, etc.
    {"\xe3\x82\x81", "\xe3\x82\x81\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // め→める
    {"\xe3\x81\xb9", "\xe3\x81\xb9\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // べ→べる
    {"\xe3\x81\x91", "\xe3\x81\x91\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // け→ける
    {"\xe3\x81\x9b", "\xe3\x81\x9b\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // せ→せる
    {"\xe3\x81\xa6", "\xe3\x81\xa6\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // て→てる
    {"\xe3\x81\xad", "\xe3\x81\xad\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ね→ねる
    {"\xe3\x81\xb8", "\xe3\x81\xb8\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // へ→へる
    {"\xe3\x82\x8c", "\xe3\x82\x8c\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // れ→れる
    {"\xe3\x81\x88", "\xe3\x81\x88\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // え→える
    {"\xe3\x81\x92", "\xe3\x81\x92\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // げ→げる
    {"\xe3\x81\xa7", "\xe3\x81\xa7\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // で→でる
    {"\xe3\x81\xbe", "\xe3\x81\xbe\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ま→まる (not ideal but catches some)

    // Godan passive past: replace あ-row + れた
    {"\xe3\x81\x8b\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かれた→く
    {"\xe3\x81\x8c\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がれた→ぐ
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // された→す
    {"\xe3\x81\x9f\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たれた→つ
    {"\xe3\x81\xaa\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なれた→ぬ
    {"\xe3\x81\xb0\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばれた→ぶ
    {"\xe3\x81\xbe\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まれた→む
    {"\xe3\x82\x89\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // られた→る
    {"\xe3\x82\x8f\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // われた→う
    // Godan passive te-form: replace あ-row + れて
    {"\xe3\x81\x8b\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かれて→く
    {"\xe3\x81\x8c\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がれて→ぐ
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // されて→す
    {"\xe3\x81\x9f\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たれて→つ
    {"\xe3\x81\xaa\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なれて→ぬ
    {"\xe3\x81\xb0\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばれて→ぶ
    {"\xe3\x81\xbe\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まれて→む
    {"\xe3\x82\x89\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // られて→る
    {"\xe3\x82\x8f\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // われて→う

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

    // Godan causative past: replace あ-row + せた
    {"\xe3\x81\x8b\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かせた→く
    {"\xe3\x81\x8c\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がせた→ぐ
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // させた→す
    {"\xe3\x81\x9f\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たせた→つ
    {"\xe3\x82\x89\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らせた→る
    {"\xe3\x82\x8f\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わせた→う

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
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // された→する
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // されて→する
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // させる→する
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // させた→する
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x81\xa6", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // させて→する
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
