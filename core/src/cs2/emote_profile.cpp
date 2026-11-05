#include "nyan/cs2/emote_profile.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "nyan/common/sjis.hpp"
#include "nyan/common/xml.hpp"
#include "nyan/cs2/asset_index.hpp"

namespace nyan::cs2 {
namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::vector<std::string> splitArgs(const std::string& spec) {
    std::vector<std::string> out;
    size_t s = 0;
    for (size_t c; (c = spec.find(',', s)) != std::string::npos; s = c + 1) out.push_back(spec.substr(s, c - s));
    out.push_back(spec.substr(s));
    return out;
}

}  // namespace

const EmoteProfiles::Sections* EmoteProfiles::file(const std::string& name) const {
    auto it = files_.find(name);
    if (it != files_.end()) return it->second ? &*it->second : nullptr;
    std::optional<Sections> parsed;
    if (assets_ && assets_->find(name)) {
        // "#section" lines, then whitespace-separated rows; `//` starts a comment.
        Bytes b = assets_->read(name);
        std::string text = sjisToUtf8(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
        Sections sec;
        std::string current;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            if (size_t c = line.find("//"); c != std::string::npos) line.resize(c);
            std::istringstream ls(line);
            std::vector<std::string> row;
            for (std::string t; ls >> t;) row.push_back(t);
            if (row.empty()) continue;
            if (row[0][0] == '#') {
                current = row[0].substr(1);
                sec[current];
                continue;
            }
            if (row[0][0] == '$' && row.size() == 1) {  // a `$name` chain table (kept as "$name")
                current = row[0];
                sec[current];
                continue;
            }
            sec[current].push_back(std::move(row));
        }
        parsed = std::move(sec);
    }
    auto& slot = files_[name] = std::move(parsed);
    return slot ? &*slot : nullptr;
}

EmoteProfiles EmoteProfiles::load(const AssetIndex& assets) {
    EmoteProfiles p;
    p.assets_ = &assets;
    // sndanm.xml: which voices move which characters' mouths (sscript 0x0fc260 reads it: up to 32
    // ch%02d entries; <mode> MPL makes it the E-mote banks', otherwise the images').
    if (auto ref = assets.find("sndanm.xml")) {
        Bytes b = assets.read("sndanm.xml");
        auto root = parseXml(decodeGameText(std::string_view(reinterpret_cast<const char*>(b.data()), b.size())));
        if (const XmlNode* doc = root->child("document")) {
            p.lipMpl_ = lower(doc->value("mode").value_or("")) == "mpl";
            for (const auto& ch : doc->children) {
                if (!ch->name.starts_with("ch")) continue;
                LipChannel lc;
                lc.image = lower(ch->value("image").value_or(""));
                lc.sound = lower(ch->value("sound").value_or(""));
                // An entry counts only with a limit above 0 (a missing one reads as 0).
                lc.limit = std::atoi(ch->value("limit").value_or("0").c_str());
                if (lc.limit > 0 && !lc.image.empty() && !lc.sound.empty()) p.lipChannels_.push_back(std::move(lc));
            }
        }
    }
    const Sections* view = p.file("m2view.cnf");
    if (!view) return p;
    auto sec = [&](const char* name) -> const std::vector<std::vector<std::string>>* {
        auto it = view->find(name);
        return it == view->end() ? nullptr : &it->second;
    };
    if (auto* rows = sec("profile")) {
        for (const auto& r : *rows) {
            if (r.size() >= 2) p.profiles_[lower(r[0])] = r[1];
        }
    }
    // #position is m2view.exe's (and the HG fallback's, cs2.exe 0x5777f0); E-mote models are placed
    // from the centre of the view (0x574070).
    if (auto* rows = sec("fade"); rows && !rows->empty()) p.fade_ = std::atoi(rows->front()[0].c_str());
    return p;
}

const EmoteProfiles::LipChannel* EmoteProfiles::lipChannel(const std::string& voice) const {
    if (!lipMpl_) return nullptr;
    std::string v = lower(voice);
    for (const auto& lc : lipChannels_) {
        if (v.starts_with(lc.sound)) return &lc;
    }
    return nullptr;
}

std::vector<std::pair<std::string, float>> EmoteProfiles::bodyPattern(const std::string& dif, int32_t digit,
                                                                      const std::string& value) const {
    std::vector<std::pair<std::string, float>> out;
    const Sections* s = file(dif);
    if (!s) return out;
    auto it = s->find("pattern");
    if (it == s->end()) return out;
    const std::string d = std::to_string(digit);
    bool on = false;
    for (const auto& row : it->second) {
        if (row[0][0] == '%') {
            on = row[0].substr(1) == d && row.size() >= 2 && row[1] == value;
        } else if (on && row.size() >= 2) {
            out.emplace_back(row[0], std::strtof(row[1].c_str(), nullptr));
        }
    }
    return out;
}

std::pair<int32_t, int32_t> EmoteProfiles::bodyChain(const std::string& dif, int32_t digit) const {
    const Sections* s = file(dif);
    if (!s) return {0, 0};
    auto it = s->find("chain");
    if (it == s->end()) return {0, 0};
    static const char* kTypes[] = {"fix", "linear", "sin_fs", "sin_sf", "sin_ss", "fade", "nomove"};
    for (const auto& row : it->second) {
        if (row.size() < 3 || std::atoi(row[0].c_str()) != digit) continue;
        int32_t type = 0;
        for (int32_t k = 0; k < 7; ++k) {
            if (lower(row[1]) == kTypes[k]) type = k;
        }
        return {type, std::atoi(row[2].c_str())};
    }
    return {0, 0};
}

bool EmoteProfiles::sameGroup(const std::string& dif, const std::string& a, const std::string& b) const {
    const Sections* s = file(dif);
    if (!s) return false;
    auto it = s->find("group");
    if (it == s->end()) return false;
    // The rows' names map to their row's number, later rows replacing earlier ones (0x57a1d0).
    std::map<std::string, size_t> group;
    for (size_t r = 0; r < it->second.size(); ++r) {
        for (const auto& name : it->second[r]) group[name] = r;
    }
    auto x = group.find(a), y = group.find(b);
    return x != group.end() && y != group.end() && x->second == y->second;
}

std::vector<std::string> EmoteProfiles::groupInertia(const std::string& dif) const {
    std::vector<std::string> out;
    const Sections* s = file(dif);
    if (!s) return out;
    if (auto it = s->find("group_inertia"); it != s->end()) {
        for (const auto& row : it->second) out.push_back(row[0]);
    }
    return out;
}

std::vector<std::string> EmoteProfiles::bodyDefaults(const std::string& dif) const {
    std::vector<std::string> out;
    const Sections* s = file(dif);
    if (!s) return out;
    auto difs = s->find("dif");
    auto pattern = s->find("pattern");
    if (difs == s->end()) return out;
    for (const auto& d : difs->second) {
        std::string first, marked;
        if (pattern != s->end()) {
            for (const auto& row : pattern->second) {
                if (row[0][0] != '%' || row[0].substr(1) != d[0] || row.size() < 2) continue;
                if (first.empty()) first = row[1];
                if (marked.empty() && row.size() >= 4 && lower(row[3]) == "default") marked = row[1];
            }
        }
        out.push_back(!marked.empty() ? marked : !first.empty() ? first : "0");
    }
    return out;
}

std::pair<int32_t, int32_t> EmoteProfiles::Resolved::argChain(int32_t n, const std::string& from, const std::string& to) const {
    if (auto t = chainTables.find(n); t != chainTables.end()) {
        auto m = t->second.moves.find({lower(from), lower(to)});
        return m != t->second.moves.end() ? m->second : std::pair<int32_t, int32_t>{t->second.type, t->second.frames};
    }
    auto c = chains.find(n);
    return c != chains.end() ? c->second : std::pair<int32_t, int32_t>{0, 0};
}

int32_t EmoteProfiles::chainFade(const Resolved& to, const std::string& fromSpec, const std::string& toSpec) {
    // cs2.exe 0x4d3430 walks the arguments after the costume, comparing old and new (0x4d9880), and
    // asks each changed one's #chain (0x4ed580): fade sets the crossfade and its frames (0x9c / 0xa0),
    // the eased types tween the variables instead (none in Amakano 2; instant here).
    auto a = splitArgs(fromSpec), b = splitArgs(toSpec);
    int32_t frames = 0;
    for (size_t n = 2; n < std::max(a.size(), b.size()); ++n) {
        std::string x = n < a.size() ? lower(a[n]) : std::string(), y = n < b.size() ? lower(b[n]) : std::string();
        if (x == y) continue;
        auto c = to.argChain(static_cast<int32_t>(n - 1), x, y);
        if (c.first == 5) frames = c.second;
    }
    return frames;
}

std::optional<EmoteProfiles::Resolved> EmoteProfiles::resolve(const std::string& spec) const {
    // cs2.exe 0x57e560: what follows the first `:` is the motion ("0" if none); the rest is split at
    // the commas.
    std::string main = spec, motion = "0";
    if (size_t colon = spec.find(':'); colon != std::string::npos) {
        main = spec.substr(0, colon);
        motion = spec.substr(colon + 1);
        if (size_t more = motion.find(':'); more != std::string::npos) motion.resize(more);
        if (motion.empty()) motion = "0";
    }
    auto args = splitArgs(main);
    auto prof = profiles_.find(lower(args[0]));
    if (prof == profiles_.end()) return std::nullopt;
    args.erase(args.begin());
    const std::string costume = args.empty() ? std::string() : args[0];
    const Sections* pfl = file(prof->second);
    if (!pfl) return std::nullopt;

    Resolved r;
    r.args = args;
    std::string dif;
    auto find = [](const Sections* s, const char* name) -> const std::vector<std::vector<std::string>>* {
        if (!s) return nullptr;
        auto it = s->find(name);
        return it == s->end() ? nullptr : &it->second;
    };
    if (auto* rows = find(pfl, "charaname")) {
        for (const auto& row : *rows) {
            if (row.size() >= 3 && row[0] == costume) r.model = row[2] + ".psz";
        }
        if (r.model.empty() && !rows->empty() && rows->front().size() >= 3) r.model = rows->front()[2] + ".psz";
    }
    if (auto* rows = find(pfl, "motname")) {
        // The display profiles come from the body's motion "0" row (0x59d5c0 / 0x59d7b0 with the
        // spec's third entry, always "0"), the idle set from the spec's motion's row (0x59da80).
        const std::vector<std::string>* first = nullptr;  // the body's first row, if neither matches
        bool idleFound = false;
        for (const auto& row : *rows) {
            if (row.size() < 4 || row[0] != costume) continue;
            if (!first) first = &row;
            if (row[1] == "0" && dif.empty()) {
                dif = row[3];
                if (row.size() >= 5) r.bodyDif = row[4];
            }
            if (row[1] == motion && !idleFound) r.idle = row[2], idleFound = true;
        }
        if (dif.empty() && first) {
            dif = (*first)[3];
            if (first->size() >= 5) r.bodyDif = (*first)[4];
        }
        if (dif.empty() && !rows->empty() && rows->front().size() >= 4) dif = rows->front()[3];
        if (!idleFound && first) r.idle = (*first)[2];
    }
    if (r.model.empty()) return std::nullopt;

    // The display profile and the files it includes; later files' settings are applied after.
    std::vector<const Sections*> chain;
    if (const Sections* d = file(dif)) {
        if (auto* inc = find(d, "include")) {
            for (const auto& row : *inc) {
                if (const Sections* f = file(row[0])) chain.push_back(f);
            }
        }
        chain.push_back(d);
    }
    for (const Sections* s : chain) {
        if (auto* rows = find(s, "position"); rows && !rows->empty() && rows->front().size() >= 2) {
            r.posX = std::atoi(rows->front()[0].c_str());
            r.posY = std::atoi(rows->front()[1].c_str());
        }
        if (auto* rows = find(s, "scale"); rows && !rows->empty() && !rows->front().empty()) {
            r.scale = std::strtof(rows->front()[0].c_str(), nullptr);  // only x is used (0x574070)
        }
        if (auto* rows = find(s, "scaleoffset"); rows && !rows->empty() && !rows->front().empty()) {
            r.scaleOffsetX = std::strtof(rows->front()[0].c_str(), nullptr);
            if (rows->front().size() >= 2) r.scaleOffsetY = std::strtof(rows->front()[1].c_str(), nullptr);
        }
        if (auto* rows = find(s, "attribute")) {
            for (const auto& row : *rows) {
                if (row.empty()) continue;
                std::array<int32_t, 3> v{0, 0, 0};
                for (size_t c = 1; c < row.size() && c <= 3; ++c) v[c - 1] = std::atoi(row[c].c_str());
                r.attributes.emplace(std::atoi(row[0].c_str()), v);  // a number already read stays
            }
        }
        if (auto* rows = find(s, "defval")) {
            for (const auto& row : *rows) {
                if (row.size() >= 2) r.vars[row[0]] = std::strtof(row[1].c_str(), nullptr);
            }
        }
    }
    // Each argument named in #dif picks a #pattern block ("%n value"), whose rows set variables.
    for (const Sections* s : chain) {
        auto* difs = find(s, "dif");
        auto* pattern = find(s, "pattern");
        if (!difs || !pattern) continue;
        for (const auto& d : *difs) {
            size_t n = static_cast<size_t>(std::atoi(d[0].c_str()));
            if (n >= args.size()) continue;
            const std::string& value = args[n];
            bool on = false;
            for (const auto& row : *pattern) {
                if (row[0][0] == '%') {
                    on = row[0].substr(1) == d[0] && row.size() >= 2 && row[1] == value;
                } else if (on && row.size() >= 2) {
                    r.vars[row[0]] = std::strtof(row[1].c_str(), nullptr);
                    r.varArg[row[0]] = static_cast<int32_t>(n);
                }
            }
        }
    }
    // #chain (0x4f1df0): "<argument> <type> <frames>", later files' rows replacing earlier ones;
    // "<argument> $name" takes the `$name` table (0x597120) from the files.
    static const char* kTypes[] = {"fix", "linear", "sin_fs", "sin_sf", "sin_ss", "fade", "nomove"};
    auto typeOf = [&](const std::string& w) {
        for (int32_t k = 0; k < 7; ++k) {
            if (lower(w) == kTypes[k]) return k;
        }
        return 0;
    };
    for (const Sections* s : chain) {
        if (auto* rows = find(s, "chain")) {
            for (const auto& row : *rows) {
                const int32_t n = std::atoi(row[0].c_str());
                if (row.size() >= 2 && row[1][0] == '$') {
                    Resolved::ChainTable t;
                    for (const Sections* ts : chain) {
                        auto it = ts->find(row[1]);
                        if (it == ts->end()) continue;
                        t = Resolved::ChainTable{};
                        for (const auto& tr : it->second) {
                            if (lower(tr[0]) == "default" && tr.size() >= 2) t.type = typeOf(tr[1]);
                            else if (lower(tr[0]) == "frame" && tr.size() >= 2) t.frames = std::atoi(tr[1].c_str());
                            else if (lower(tr[0]) == "fix" || lower(tr[0]) == "fade") continue;
                            else if (tr.size() >= 4) t.moves[{lower(tr[0]), lower(tr[1])}] = {typeOf(tr[2]), std::atoi(tr[3].c_str())};
                        }
                    }
                    r.chainTables[n] = std::move(t);
                    r.chains.erase(n);
                    continue;
                }
                if (row.size() < 3) continue;
                r.chains[n] = {typeOf(row[1]), std::atoi(row[2].c_str())};
                r.chainTables.erase(n);
            }
        }
    }
    // #blink (0x4f21b0 / 0x4d7610): per argument, the row for its value (the first such row); the
    // variable and rest value from that value's #pattern block (its first row).
    {
        std::map<int32_t, Resolved::Blink> blinks;
        for (const Sections* s : chain) {
            auto* rows = find(s, "blink");
            if (!rows) continue;
            for (const auto& row : *rows) {
                if (row.size() < 7) continue;
                int32_t n = std::atoi(row[0].c_str());
                if (n < 0 || static_cast<size_t>(n) >= args.size() || args[static_cast<size_t>(n)] != row[1] || blinks.count(n)) continue;
                Resolved::Blink b;
                for (const Sections* ps : chain) {
                    auto* pattern = find(ps, "pattern");
                    if (!pattern) continue;
                    bool on = false;
                    for (const auto& prow : *pattern) {
                        if (prow[0][0] == '%') {
                            on = prow[0].substr(1) == row[0] && prow.size() >= 2 && prow[1] == row[1];
                        } else if (on && prow.size() >= 2 && b.variable.empty()) {
                            b.variable = prow[0];
                            b.rest = std::strtof(prow[1].c_str(), nullptr);
                        }
                    }
                }
                if (b.variable.empty()) continue;
                b.start = std::strtof(row[2].c_str(), nullptr);
                b.end = std::strtof(row[3].c_str(), nullptr);
                b.frames = std::atoi(row[4].c_str());
                b.min = std::atoi(row[5].c_str());
                b.range = std::atoi(row[6].c_str()) - b.min;
                if (b.range == 0) b.range = 1;
                blinks[n] = b;
            }
        }
        for (auto& [n, b] : blinks) r.blinks.push_back(std::move(b));
    }
    // cs2.exe's springs (0x5a75e0): later files in the chain add to and replace earlier ones.
    for (const Sections* sec : chain) {
        auto materials = [&](const char* name, std::map<std::string, Resolved::Material>& out) {
            if (auto* rows = find(sec, name)) {
                for (const auto& row : *rows) {
                    if (row.size() < 9) continue;  // the reader wants all nine columns
                    Resolved::Material m;
                    m.mass = std::strtof(row[1].c_str(), nullptr);
                    m.spring = std::strtof(row[2].c_str(), nullptr);
                    m.resistance = std::strtof(row[3].c_str(), nullptr);
                    m.dt = std::strtof(row[4].c_str(), nullptr);
                    m.maxMove = std::strtof(row[5].c_str(), nullptr);
                    m.invert = std::atoi(row[6].c_str()) != 0;
                    m.scale = std::strtof(row[7].c_str(), nullptr);
                    m.shift = std::strtof(row[8].c_str(), nullptr);
                    out[row[0]] = m;
                }
            }
        };
        materials("material", r.materials);
        materials("material_min", r.materialMin);
        materials("material_max", r.materialMax);
        auto names = [&](const char* name, std::vector<std::string>& out) {
            if (auto* rows = find(sec, name)) {
                for (const auto& row : *rows) {
                    if (!row.empty()) out.push_back(row[0]);
                }
            }
        };
        names("inertia_ud", r.inertiaUD);
        names("inertia_lr", r.inertiaLR);
        auto groups = [&](const char* name, std::map<std::string, std::string>& out) {
            if (auto* rows = find(sec, name)) {
                std::string group;
                for (const auto& row : *rows) {
                    if (row.empty()) continue;
                    if (row[0][0] == '%') group = row[0].substr(1);
                    else out[row[0]] = group;
                }
            }
        };
        groups("inertia_lv", r.levelGroup);
        groups("inertia_enable", r.enableGroup);
        if (auto* rows = find(sec, "inertia_dif")) {
            for (const auto& row : *rows) {
                if (row.size() < 4) continue;
                r.inertiaDifs.push_back({row[0], row[1], std::strtof(row[2].c_str(), nullptr), std::strtof(row[3].c_str(), nullptr)});
            }
        }
    }
    // Lip sync: the rows of #lip_variable / #lip_table / #clipper for the spec's own arguments
    // ("<argument> <value> ..."); #clipper_default otherwise.
    for (const Sections* sec : chain) {
        auto matches = [&](const std::vector<std::string>& row) {
            size_t n = static_cast<size_t>(std::atoi(row[0].c_str()));
            return row.size() >= 2 && n < args.size() && args[n] == row[1];
        };
        if (auto* rows = find(sec, "clipper_default"); rows && !rows->empty() && rows->front().size() >= 8) {
            const auto& row = rows->front();
            for (int k = 0; k < 6; ++k) r.lipWeights[static_cast<size_t>(k)] = std::strtof(row[static_cast<size_t>(k)].c_str(), nullptr);
            r.lipLatency = std::atoi(row[6].c_str());
            r.lipSmooth = std::atoi(row[7].c_str());
        }
        if (auto* rows = find(sec, "lip_variable")) {
            for (const auto& row : *rows) {
                if (row.size() >= 3 && matches(row)) r.lipVariable = row[2];
            }
        }
        if (auto* rows = find(sec, "lip_table")) {
            // "<argument> <value> <interpolation> <level min> <level max> <pattern 0> <pattern 1> ..."
            for (const auto& row : *rows) {
                if (row.size() < 6 || !matches(row)) continue;
                r.lipRest = std::strtof(row[5].c_str(), nullptr);
                r.lipInterp = lower(row[2]) == "linear" ? 1 : 0;
                r.lipLevelMin = std::strtof(row[3].c_str(), nullptr);
                r.lipPatterns.clear();
                for (size_t k = 5; k < row.size(); ++k) r.lipPatterns.push_back(std::strtof(row[k].c_str(), nullptr));
            }
        }
        if (auto* rows = find(sec, "lip_flow")) {
            // "<argument> <value> <pattern>,<frames>,<end check> ..."
            for (const auto& row : *rows) {
                if (row.size() < 3 || !matches(row)) continue;
                r.lipFlow.clear();
                for (size_t k = 2; k < row.size(); ++k) {
                    Resolved::LipStep st;
                    if (std::sscanf(row[k].c_str(), "%d,%d,%d", &st.pattern, &st.frames, &st.check) >= 2) r.lipFlow.push_back(st);
                }
            }
        }
    }
    for (const Sections* sec : chain) {
        if (auto* rows = find(sec, "clipper")) {
            for (const auto& row : *rows) {
                size_t n = row.empty() ? 0 : static_cast<size_t>(std::atoi(row[0].c_str()));
                if (row.size() < 10 || n >= args.size() || args[n] != row[1]) continue;
                for (int k = 0; k < 6; ++k) r.lipWeights[static_cast<size_t>(k)] = std::strtof(row[static_cast<size_t>(k + 2)].c_str(), nullptr);
                r.lipLatency = std::atoi(row[8].c_str());
                r.lipSmooth = std::atoi(row[9].c_str());
            }
        }
    }
    // Touch mode: #hittest and #touch, a later file's section replacing an earlier one's; a
    // `$table` is looked up in every file, the last that has it winning.
    for (const Sections* s : chain) {
        if (auto* rows = find(s, "hittest")) {
            r.hitTest.clear();
            for (const auto& row : *rows) r.hitTest.push_back(row[0]);
        }
        if (auto* rows = find(s, "touch")) {
            r.touch.clear();
            for (const auto& row : *rows) {
                if (row.size() < 2) continue;
                auto& tables = r.touch[static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 0))];
                tables.clear();
                for (size_t k = 1; k < row.size(); ++k) {
                    std::vector<Resolved::TouchRow> table;
                    for (const Sections* ts : chain) {
                        auto it = ts->find(row[k]);
                        if (it == ts->end()) continue;
                        table.clear();
                        for (const auto& tr : it->second) table.push_back({tr[0], tr.size() >= 2 ? tr[1] : std::string()});
                    }
                    tables.push_back(std::move(table));
                }
            }
        }
    }
    return r;
}

const EmoteProfiles::Action* EmoteProfiles::action(const std::string& name) const {
    auto it = actions_.find(name);
    if (it != actions_.end()) return it->second ? &*it->second : nullptr;
    std::optional<Action> parsed;
    const Sections* def = file("action.def");
    auto sec = def ? def->find("$" + name) : Sections::const_iterator{};
    if (def && sec != def->end()) {
        // 0x5a9da0: the rows up to the next `#` or `$` line. A step is kept once something set it
        // (a `frame`, `easing`, `var`, `wait` or `restore`); `frame` and `wait` close the one
        // before. Unknown words are skipped (action.def's one `/* ... */` block, after an action's
        // last step, adds variables to that `restore` step, which doesn't use its own).
        Action a;
        Action::Step step;
        bool open = false;
        auto close = [&] {
            if (open) a.steps.push_back(std::move(step));
            step = Action::Step{};
            open = false;
        };
        for (const auto& row : sec->second) {
            const std::string& w = row[0];
            if (w == "frame") {
                close();
                if (row.size() < 2) continue;
                step.frames = std::strtof(row[1].c_str(), nullptr);
                open = true;
            } else if (w == "easing") {
                if (row.size() < 2) continue;
                step.easing = std::strtof(row[1].c_str(), nullptr);
                open = true;
            } else if (w == "var") {
                if (row.size() < 3) continue;
                step.vars.emplace_back(row[1], std::strtof(row[2].c_str(), nullptr));
                if (std::find(a.vars.begin(), a.vars.end(), row[1]) == a.vars.end()) a.vars.push_back(row[1]);
                open = true;
            } else if (w == "wait") {
                close();
                if (row.size() < 2) continue;
                step.wait = std::atoi(row[1].c_str());
                open = true;
            } else if (w == "restore") {
                step.restore = true;
                open = true;
            }
        }
        close();
        parsed = std::move(a);
    }
    auto& slot = actions_[name] = std::move(parsed);
    return slot ? &*slot : nullptr;
}

}  // namespace nyan::cs2
