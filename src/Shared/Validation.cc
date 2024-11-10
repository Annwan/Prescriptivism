module;
#include <algorithm>
#include <array>
#include <ranges>
module pr.validation;
import pr.utils;
import pr.cards;

namespace pr {
auto IsConsonant(CardId id) -> bool {
    return CardDatabase[+id].is_consonant();
}
}

auto pr::validation::ValidateInitialWord(constants::Word word, constants::Word original)
    -> InitialWordValidationResult {
    // The word is a permutation of the original word
    rgs::sort(original);
    constants::Word w2 = word;
    rgs::sort(w2);
    if (w2 != original) return InitialWordValidationResult::NotAPermutation;

    // No cluster or hiatus shall be longer than 2 sounds
    if (rgs::any_of( // clang-format off
        vws::chunk_by(word, [](auto a, auto b) {
            return IsConsonant(a) == IsConsonant(b);
        }),
        [](auto x) { return rgs::distance(x) > 2; })
    ) return InitialWordValidationResult::ClusterTooLong; // clang-format on

    // M1 and M2 CANNOT start a consonant cluster word-initially.
    if (IsConsonant(word[0]) and IsConsonant(word[1]) and CardDatabase[+word[0]].manner_or_height <= 2)
        return InitialWordValidationResult::BadInitialClusterManner;

    // Two consonants with the same coordinates CANNOT cluster word-initially.
    if (
        IsConsonant(word[0]) and IsConsonant(word[1]) and
        CardDatabase[+word[0]].manner_or_height == CardDatabase[+word[1]].manner_or_height and
        CardDatabase[+word[0]].place_or_frontness == CardDatabase[+word[1]].place_or_frontness
    ) return InitialWordValidationResult::BadInitialClusterCoordinates;

    // Else all seems good
    return InitialWordValidationResult::Valid;
}

auto pr::validation::ValidatePlaySoundCard(CardId played, std::span<CardId> on, usz at) -> PlaySoundCardValidationResult {
    // Is this played on a /h/ or a /ə/ and the played sound is adjacent? If so yes
    if (
        (on[at] == CardId::C_h or on[at] == CardId::V_ə) and
        ((at > 0 and on[at - 1] == played) or (at < on.size() - 1 and on[at + 1] == played))
    ) return PlaySoundCardValidationResult::Valid;

    // Is this a special sound change? If so yes
    for (auto& c : CardDatabase[+on[at]].converts_to)
        if (played == c[0]) return c.size() > 1 ? PlaySoundCardValidationResult::NeedsOtherCard : PlaySoundCardValidationResult::Valid;

    // Is this an adjacent phoneme or a different phoneme with the same coordinates? If so yes
    if (
        IsConsonant(played) == IsConsonant(on[at]) and
        std::abs(CardDatabase[+played].place_or_frontness - CardDatabase[+on[at]].place_or_frontness) +
            std::abs(CardDatabase[+played].manner_or_height - CardDatabase[+on[at]].manner_or_height) < 2 and
        played != on[at]
    ) return PlaySoundCardValidationResult::Valid;

    // Otherwise, no
    return PlaySoundCardValidationResult::Invalid;
}
