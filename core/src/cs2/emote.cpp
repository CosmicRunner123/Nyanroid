#include "nyan/cs2/emote.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>

#include "nyan/common/error.hpp"
#include "nyan/cs2/psb.hpp"

namespace nyan::cs2 {
namespace {

constexpr std::array<float, 32> identityMesh() {
    std::array<float, 32> m{};
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            m[(j * 4 + i) * 2] = static_cast<float>(i) / 3.0f;
            m[(j * 4 + i) * 2 + 1] = static_cast<float>(j) / 3.0f;
        }
    }
    return m;
}
constexpr std::array<float, 32> kIdentity = identityMesh();

void bernstein(float t, float b[4]) {
    float s = 1 - t;
    b[0] = s * s * s, b[1] = 3 * s * s * t, b[2] = 3 * s * t * t, b[3] = t * t * t;
}

// A point of the unit square through a 4x4 Bezier patch.
void patch(const std::array<float, 32>& P, float u, float v, float& x, float& y) {
    float bu[4], bv[4];
    bernstein(u, bu);
    bernstein(v, bv);
    x = y = 0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            float w = bu[i] * bv[j];
            x += w * P[(j * 4 + i) * 2];
            y += w * P[(j * 4 + i) * 2 + 1];
        }
    }
}

}  // namespace

// One step of the transform from a layer's space to the model's: an affine map, or a mesh
// layer's bending of its rectangle (points in the rectangle's normalised space go through the patch).
struct EmoteModel::Step {
    bool ffd = false;
    // A mesh above a layer that doesn't take its parents' bending: it moves the layer by where the
    // layer's origin goes, without bending it (emotedriver 0x1003f530).
    bool rigid = false;
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;  // x' = a x + b y + tx, y' = c x + d y + ty
    float w = 1, h = 1, ox = 0, oy = 0;
    std::array<float, 32> mesh{};
};

struct EmoteModel::Eval {
    const std::map<std::string, float>& vars;
    // Positions only: anchor layers named here are recorded (first seen wins), nothing is drawn.
    const std::set<std::string>* want = nullptr;
    std::map<std::string, std::array<float, 2>>* found = nullptr;
    // The stencils in force (type-12 layers being walked), outermost first.
    std::vector<Draw::Stencil> stencil;
};

namespace {

template <typename Steps>
void applySteps(const Steps& steps, float& x, float& y) {
    // The layer's origin (0, 0 in its own space) goes along: a rigid mesh step moves the point by
    // the origin's displacement through that mesh.
    float qx = 0, qy = 0;
    auto bend = [](const auto& s, float& px0, float& py0) {
        float u = (px0 + s.ox) / s.w, v = (py0 + s.oy) / s.h, px, py;
        patch(s.mesh, u, v, px, py);
        px0 = px * s.w - s.ox;
        py0 = py * s.h - s.oy;
    };
    for (const auto& s : steps) {
        if (s.ffd) {
            float nqx = qx, nqy = qy;
            bend(s, nqx, nqy);
            if (s.rigid) {
                x += nqx - qx;
                y += nqy - qy;
            } else {
                bend(s, x, y);
            }
            qx = nqx, qy = nqy;
        } else {
            float nx = s.a * x + s.b * y + s.tx;
            float ny = s.c * x + s.d * y + s.ty;
            x = nx, y = ny;
            nx = s.a * qx + s.b * qy + s.tx;
            ny = s.c * qx + s.d * qy + s.ty;
            qx = nx, qy = ny;
        }
    }
}

bool parseRect(const std::string& icon, float& w, float& h, float& ox, float& oy) {
    // Blank layers give their rectangle as "w:h:originX:originY".
    if (std::count(icon.begin(), icon.end(), ':') != 3) return false;
    char* e = nullptr;
    const char* p = icon.c_str();
    w = std::strtof(p, &e);
    h = std::strtof(e + 1, &e);
    ox = std::strtof(e + 1, &e);
    oy = std::strtof(e + 1, &e);
    return w > 0 && h > 0;
}

}  // namespace

std::optional<EmoteModel> EmoteModel::load(ByteSpan psz, uint32_t key) {
    auto psb = Psb::load(psz, key);
    if (!psb) return std::nullopt;
    const PsbValue& root = psb->root();
    EmoteModel m;

    // The textures: source/<name>/texture (RGBA8 stored as BGRA) and their icons (sprite
    // rectangles), which image layers name as src <name>, icon <icon>. Version 3 models (Amakano 1)
    // split the parts over several sources (tex#000, tex#001); later ones have one, `tex`. They are
    // stacked into one atlas, each icon moved down by its texture's place in it.
    struct Source {
        const std::string* name;
        const PsbValue* icons;
        int w, h;
        ByteSpan px;
    };
    std::vector<Source> sources;
    constexpr int kGap = 2;  // clear rows between stacked textures, so filtering never mixes them
    if (const PsbValue* source = root.get("source"); source && source->type == PsbValue::Type::Object) {
        for (const auto& [name, src] : source->object) {
            const PsbValue& tex = src["texture"];
            if (tex.isNull()) continue;
            int w = static_cast<int>(tex.num("width")), h = static_cast<int>(tex.num("height"));
            int th = static_cast<int>(tex.num("truncated_height", h));
            ByteSpan px = psb->resource(static_cast<size_t>(tex["pixel"].i));
            if (tex["type"].str() != "RGBA8" || w <= 0 || th <= 0 || px.size() < static_cast<size_t>(w) * th * 4) continue;
            sources.push_back({&name, &src["icon"], w, th, px});
            m.texW_ = std::max(m.texW_, w);
            m.texH_ += th + (sources.size() > 1 ? kGap : 0);
        }
    }
    if (sources.empty()) return std::nullopt;
    m.rgba_.assign(static_cast<size_t>(m.texW_) * m.texH_ * 4, 0);
    int top = 0;
    for (const auto& s : sources) {
        for (int y = 0; y < s.h; ++y) {
            const uint8_t* from = s.px.data() + static_cast<size_t>(y) * s.w * 4;
            uint8_t* to = m.rgba_.data() + (static_cast<size_t>(top + y) * m.texW_) * 4;
            for (int x = 0; x < s.w; ++x) {
                to[x * 4] = from[x * 4 + 2], to[x * 4 + 1] = from[x * 4 + 1];
                to[x * 4 + 2] = from[x * 4], to[x * 4 + 3] = from[x * 4 + 3];
            }
        }
        for (const auto& [iname, ic] : s.icons->object) {
            m.icons_[*s.name + '/' + iname] =
                Icon{static_cast<float>(ic.num("left")), static_cast<float>(ic.num("top") + top), static_cast<float>(ic.num("width")),
                     static_cast<float>(ic.num("height")), static_cast<float>(ic.num("originX")), static_cast<float>(ic.num("originY"))};
        }
        top += s.h + kGap;
    }

    const PsbValue& meta = root["metadata"];
    m.baseChara_ = meta["base"]["chara"].str();
    m.baseMotion_ = meta["base"]["motion"].str();
    for (const auto& v : meta["variableList"].list) m.variables_.push_back(v["label"].str());
    for (const auto& v : meta["instantVariableList"].list) m.instant_.insert(v.str());
    for (const auto& tv : meta["timelineControl"].list) {
        Timeline tl;
        tl.loopBegin = static_cast<float>(tv.num("loopBegin", -1));
        tl.loopEnd = static_cast<float>(tv.num("loopEnd", -1));
        tl.lastTime = static_cast<float>(tv.num("lastTime", -1));
        for (const auto& vv : tv["variableList"].list) {
            std::vector<Timeline::Key> keys;
            Timeline::Key k;
            for (const auto& f : vv["frameList"].list) {
                k.time = static_cast<float>(f.num("time"));
                k.end = f.num("type") == 0;
                if (!k.end && f["content"].type == PsbValue::Type::Object) {
                    k.value = static_cast<float>(f["content"].num("value"));
                    k.easing = static_cast<float>(f["content"].num("easing"));
                }
                tl.maxTime = std::max(tl.maxTime, k.time);
                keys.push_back(k);
            }
            tl.variables.push_back(vv["label"].str());
            if (!keys.empty()) tl.tracks.emplace_back(vv["label"].str(), std::move(keys));
        }
        m.timelines_[tv["label"].str()] = std::move(tl);
    }
    for (const auto& ev : meta["eyeControl"].list) {
        if (ev.num("enabled", 1) == 0 || m.blink_) continue;
        Blink b;
        b.variable = ev["label"].str();
        b.beginFrame = static_cast<int>(ev.num("beginFrame"));
        b.endFrame = static_cast<int>(ev.num("endFrame"));
        b.frames = static_cast<float>(ev.num("blinkFrameCount", 16));
        b.intervalMin = static_cast<float>(ev.num("blinkIntervalMin", 30));
        b.intervalMax = static_cast<float>(ev.num("blinkIntervalMax", 180));
        b.enabled = ev.num("blinkEnabled") != 0;
        m.blink_ = std::move(b);
    }
    for (const auto& mv : meta["mouthControl"].list) {
        if (m.talk_.empty()) m.talk_ = mv["talkLabel"].str();
    }
    // Physics (emotedriver.dll 0x1000e3d0 / 0x1000e060 / 0x1000e230 for the bust, 0x1000f790 /
    // 0x1000f260 / 0x1000ee80 for hair and parts): enabled entries only.
    auto vec3 = [](const PsbValue& v) {
        return std::array<float, 3>{static_cast<float>(v.num("x")), static_cast<float>(v.num("y")), static_cast<float>(v.num("z"))};
    };
    auto pair = [](const PsbValue& v, float* out) {
        auto n = v.numbers();
        for (size_t k = 0; k < 2 && k < n.size(); ++k) out[k] = static_cast<float>(n[k]);
    };
    for (const auto& bv : meta["bustControl"].list) {
        if (bv.num("enabled", 1) == 0) continue;
        Bust b;
        b.layer = bv["baseLayer"].str(), b.varLR = bv["var_lr"].str(), b.varUD = bv["var_ud"].str();
        b.gravity = static_cast<float>(bv.num("gravity")), b.spring = static_cast<float>(bv.num("spring"));
        b.friction = static_cast<float>(bv.num("friction"));
        b.scaleX = static_cast<float>(bv.num("scale_x", 1)), b.scaleY = static_cast<float>(bv.num("scale_y", 1));
        const PsbValue& pr = bv["param"];
        b.op = vec3(pr["op"]), b.p = vec3(pr["p"]), b.pv = vec3(pr["pv"]);
        b.ofs = static_cast<float>(pr.num("ofs"));
        m.busts_.push_back(std::move(b));
    }
    auto pends = [&](const PsbValue& list, std::vector<Pend>& out) {
        for (const auto& hv : list.list) {
            if (hv.num("enabled", 1) == 0) continue;
            Pend h;
            h.layer = hv["baseLayer"].str(), h.varLR = hv["var_lr"].str(), h.varLRM = hv["var_lrm"].str();
            h.varUD = hv["var_ud"].str();
            h.gravity = static_cast<float>(hv.num("gravity"));
            h.frictionX = static_cast<float>(hv.num("friction_x")), h.frictionY = static_cast<float>(hv.num("friction_y"));
            h.bRate = static_cast<float>(hv.num("b_rate")), h.vBound = static_cast<float>(hv.num("v_bound"));
            h.udEft = static_cast<int>(hv.num("ud_eft"));
            pair(hv["length"], h.length);
            pair(hv["scale_x"], h.scaleX);
            pair(hv["scale_y"], h.scaleY);
            h.bendSpd = static_cast<float>(hv.num("bend_spd")), h.bendVol = static_cast<float>(hv.num("bend_vol"));
            const PsbValue& pr = hv["param"];
            h.op = vec3(pr["op"]);
            for (size_t k = 0; k < 2; ++k) {
                if (k < pr["bp"].list.size()) h.bp[k] = vec3(pr["bp"].list[k]);
                if (k < pr["p"].list.size()) h.p[k] = vec3(pr["p"].list[k]);
                if (k < pr["pv"].list.size()) h.pv[k] = vec3(pr["pv"].list[k]);
            }
            h.ofs = static_cast<float>(pr.num("ofs"));
            h.bendR = static_cast<float>(pr.num("bendR")), h.bendS = static_cast<float>(pr.num("bendS"));
            out.push_back(std::move(h));
        }
    };
    // Clamps (loader 0x10015850): enabled pairs.
    for (const auto& cv : meta["clampControl"].list) {
        if (cv.num("enabled", 1) == 0) continue;
        m.clamps_.push_back({static_cast<int>(cv.num("type")), static_cast<float>(cv.num("min")),
                             static_cast<float>(cv.num("max")), cv["var_lr"].str(), cv["var_ud"].str()});
    }
    for (const auto& sv : meta["selectorControl"].list) {
        if (sv.num("enabled", 1) == 0) continue;
        Selector s;
        s.label = sv["label"].str();
        for (const auto& ov : sv["optionList"].list) {
            s.options.push_back({ov["label"].str(), static_cast<float>(ov.num("offValue", 1)),
                                 static_cast<float>(ov.num("onValue", 0))});
        }
        m.selectors_.push_back(std::move(s));
    }
    pends(meta["hairControl"], m.hairs_);
    pends(meta["partsControl"], m.parts_);

    for (const auto& [oname, obj] : root["object"].object) {
        for (const auto& [mname, mv] : obj["motion"].object) {
            Motion mo;
            if (const PsbValue* p = mv.get("parameterize"); p && p->isNumber()) mo.parameterize = static_cast<int>(p->number());
            for (const auto& pv : mv["parameter"].list) {
                mo.params.push_back({pv["id"].str(), static_cast<float>(pv.num("rangeBegin")),
                                     static_cast<float>(pv.num("rangeEnd")), static_cast<float>(pv.num("division"))});
            }
            for (const auto& pf : mv["priority"].list) {
                std::vector<int> order;
                for (double v : pf["content"].numbers()) order.push_back(static_cast<int>(v));
                mo.priority.emplace_back(static_cast<float>(pf.num("time")), std::move(order));
            }
            m.addLayers(mo, mv["layer"], mo.roots);
            std::map<std::string, int> indexMap;
            for (const auto& [label, ix] : mv["layerIndexMap"].object) indexMap[label] = static_cast<int>(ix.number());
            for (auto& L : mo.layers) {
                auto it = indexMap.find(L.label);
                if (it != indexMap.end()) L.index = it->second;
            }
            m.objects_[oname][mname] = std::move(mo);
        }
    }
    return m;
}

void EmoteModel::addLayers(Motion& m, const PsbValue& list, std::vector<int>& out) {
    for (const auto& lv : list.list) {
        Layer L;
        L.label = lv["label"].str();
        L.type = static_cast<int>(lv.num("type"));
        if (const PsbValue* p = lv.get("parameterize"); p && p->isNumber()) L.parameterize = static_cast<int>(p->number());
        L.meshTransform = lv.num("meshTransform") != 0;
        if (const PsbValue* im = lv.get("inheritMask"); im && im->isNumber()) L.inheritMask = static_cast<int>(im->number());
        if (const PsbValue* ms = lv.get("meshSyncChildMask"); ms && ms->isNumber()) L.meshSyncChildMask = static_cast<int>(ms->number());
        L.stencilType = static_cast<int>(lv.num("stencilType"));
        for (const auto& sv : lv["stencilCompositeMaskLayerList"].list) L.stencilMasks.push_back(sv.str());
        for (const auto& fv : lv["frameList"].list) {
            Frame f;
            f.time = static_cast<float>(fv.num("time"));
            f.type = static_cast<uint8_t>(fv.num("type"));
            const PsbValue& c = fv["content"];
            if (c.type == PsbValue::Type::Object) {
                f.content = true;
                f.src = c["src"].str();
                f.icon = c["icon"].str();
                if (const PsbValue* co = c.get("coord")) {
                    auto v = co->numbers();
                    f.hasCoord = true;
                    f.x = v.size() > 0 ? static_cast<float>(v[0]) : 0;
                    f.y = v.size() > 1 ? static_cast<float>(v[1]) : 0;
                    f.z = v.size() > 2 ? static_cast<float>(v[2]) : 0;
                }
                if (const PsbValue* a = c.get("angle")) f.hasAngle = true, f.angle = static_cast<float>(a->number());
                if (const PsbValue* o = c.get("opa")) f.hasOpa = true, f.opa = static_cast<float>(o->number());
                if (const PsbValue* me = c.get("mesh")) {
                    f.hasMesh = true;
                    auto bp = (*me)["bp"].numbers();
                    if (bp.size() == 32) {
                        for (int k = 0; k < 32; ++k) f.mesh[k] = static_cast<float>(bp[k]);
                    } else {
                        f.mesh = kIdentity;  // no points: the undeformed rectangle
                    }
                }
                f.timeOffset = static_cast<float>(c["motion"].num("timeOffset"));
                f.bm = static_cast<int>(c.num("bm"));
            }
            L.frames.push_back(std::move(f));
        }
        int index = static_cast<int>(m.layers.size());
        m.layers.push_back(std::move(L));
        out.push_back(index);
        std::vector<int> children;
        addLayers(m, lv["children"], children);
        m.layers[static_cast<size_t>(index)].children = std::move(children);
    }
}

const EmoteModel::Timeline* EmoteModel::timeline(const std::string& label) const {
    auto it = timelines_.find(label);
    return it == timelines_.end() ? nullptr : &it->second;
}

std::vector<EmoteModel::Draw> EmoteModel::evaluate(const std::map<std::string, float>& vars, float time, int grid) const {
    Eval e{vars};
    std::vector<Draw> out;
    evalMotion(e, baseChara_, baseMotion_, time, {}, 0, 1, out, 0);
    for (auto& d : out) d.grid = grid;
    // Each stencilled image gets its masks' images (by layer label, as the pose has them).
    for (auto& d : out) {
        for (const auto& s : d.stencil) {
            std::vector<Draw> masks;
            for (const auto& m : out) {
                if (&m != &d && std::find(s.masks.begin(), s.masks.end(), m.label) != s.masks.end()) {
                    Draw c = m;
                    c.maskDraws.clear();
                    masks.push_back(std::move(c));
                }
            }
            d.maskDraws.push_back(std::move(masks));
        }
    }
    // Grid points of each image: the icon's rectangle, bent by its own mesh, then through its steps.
    // (Computed in evalMotion; here the draws are ordered back to front by depth.)
    std::stable_sort(out.begin(), out.end(), [](const Draw& a, const Draw& b) { return a.z < b.z; });
    return out;
}

std::map<std::string, std::array<float, 2>> EmoteModel::layerPositions(const std::map<std::string, float>& vars,
                                                                     const std::set<std::string>& labels) const {
    std::map<std::string, std::array<float, 2>> found;
    Eval e{vars, &labels, &found};
    std::vector<Draw> none;
    evalMotion(e, baseChara_, baseMotion_, 0, {}, 0, 1, none, 0);
    return found;
}

void EmoteModel::evalMotion(Eval& e, const std::string& obj, const std::string& name, float t,
                            const std::vector<Step>& xf, float z0, float opa0, std::vector<Draw>& out, int depth) const {
    if (depth > 16) return;
    auto oit = objects_.find(obj);
    if (oit == objects_.end()) return;
    auto mit = oit->second.find(name);
    if (mit == oit->second.end()) return;
    const Motion& m = mit->second;

    auto paramTime = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(m.params.size())) return 0.0f;
        const Param& p = m.params[static_cast<size_t>(idx)];
        auto it = e.vars.find(p.id);
        float v = std::clamp(it == e.vars.end() ? 0.0f : it->second, std::min(p.begin, p.end), std::max(p.begin, p.end));
        return p.end != p.begin ? (v - p.begin) * p.division / (p.end - p.begin) : 0.0f;
    };
    if (m.parameterize >= 0) t = paramTime(m.parameterize);

    // Items in this motion, by layer index, for the priority order.
    std::vector<std::pair<int, std::vector<Draw>>> items;

    std::function<void(const std::vector<int>&, const std::vector<Step>&, float, float)> walk =
        [&](const std::vector<int>& layers, const std::vector<Step>& parent, float zp, float op) {
            for (int li : layers) {
                const Layer& L = m.layers[static_cast<size_t>(li)];
                float lt = L.parameterize >= 0 ? paramTime(L.parameterize) : t;
                // The key in force: the last at or before lt; type 3 tweens to the next key.
                const Frame* cur = nullptr;
                const Frame* nxt = nullptr;
                for (size_t k = 0; k < L.frames.size(); ++k) {
                    if (L.frames[k].time <= lt + 1e-4f) {
                        cur = &L.frames[k];
                        nxt = k + 1 < L.frames.size() ? &L.frames[k + 1] : nullptr;
                    } else {
                        break;
                    }
                }
                Frame f;
                if (cur && cur->type != 0 && cur->content) {
                    f = *cur;
                    if (cur->type == 3 && nxt && nxt->content && nxt->time > cur->time) {
                        float k = (lt - cur->time) / (nxt->time - cur->time);
                        auto mix = [k](float a, float b) { return a + (b - a) * k; };
                        if (cur->hasCoord || nxt->hasCoord) {
                            f.hasCoord = true;
                            f.x = mix(cur->x, nxt->x), f.y = mix(cur->y, nxt->y), f.z = mix(cur->z, nxt->z);
                        }
                        if (cur->hasAngle || nxt->hasAngle) f.hasAngle = true, f.angle = mix(cur->angle, nxt->angle);
                        if (cur->hasOpa || nxt->hasOpa) f.hasOpa = true, f.opa = mix(cur->opa, nxt->opa);
                        if (cur->hasMesh || nxt->hasMesh) {
                            const auto& ma = cur->hasMesh ? cur->mesh : kIdentity;
                            const auto& mb = nxt->hasMesh ? nxt->mesh : kIdentity;
                            f.hasMesh = true;
                            for (int q = 0; q < 32; ++q) f.mesh[q] = mix(ma[q], mb[q]);
                        }
                    }
                } else {
                    f.content = false;  // nothing of its own now; its children still draw
                }

                std::vector<Step> mine = parent;
                // Not taking the parents' bending (inheritMask without 0x2000000): the meshes above
                // only move this layer and what is under it (0x1003f530, +0x2c1).
                if (L.inheritMask >= 0 && (L.inheritMask & 0x2000000) == 0) {
                    for (auto& s : mine) {
                        if (s.ffd) s.rigid = true;
                    }
                }
                if (f.hasCoord || f.hasAngle) {
                    float a = f.angle * 3.14159265f / 180.0f;
                    Step s;
                    s.a = std::cos(a), s.b = -std::sin(a), s.tx = f.x;
                    s.c = std::sin(a), s.d = std::cos(a), s.ty = f.y;
                    mine.insert(mine.begin(), s);
                }
                float z = zp + f.z;
                float opa = op * std::clamp(f.opa / 255.0f, 0.0f, 1.0f);
                if (e.want && L.type == 1 && e.want->count(L.label) && !e.found->count(L.label)) {
                    // An anchor layer: its origin through its own and its parents' transforms.
                    float x = 0, y = 0;
                    applySteps(mine, x, y);
                    (*e.found)[L.label] = {x, y};
                }

                float rw = 0, rh = 0, rox = 0, roy = 0;
                const Icon* icon = nullptr;
                bool haveRect = false;
                if (f.content) {
                    if (auto it = icons_.find(f.src + '/' + f.icon); it != icons_.end()) {
                        icon = &it->second;
                        rw = icon->width, rh = icon->height, rox = icon->originX, roy = icon->originY;
                        haveRect = true;
                    } else {
                        haveRect = parseRect(f.icon, rw, rh, rox, roy);
                    }
                }

                std::vector<Draw> drawn;
                if (L.type == 0 && icon && opa > 0 && !e.want) {
                    Draw d;
                    d.icon = icon;
                    d.opacity = opa;
                    d.z = z;
                    d.blend = f.bm & 0xF;
                    d.label = L.label;
                    d.stencil = e.stencil;
                    const int g = 8;
                    d.grid = g;
                    d.xy.reserve(static_cast<size_t>((g + 1) * (g + 1) * 2));
                    for (int j = 0; j <= g; ++j) {
                        for (int i = 0; i <= g; ++i) {
                            float u = static_cast<float>(i) / g, v = static_cast<float>(j) / g, px = u, py = v;
                            if (f.hasMesh) patch(f.mesh, u, v, px, py);
                            float x = px * rw - rox, y = py * rh - roy;
                            applySteps(mine, x, y);
                            d.xy.push_back(x);
                            d.xy.push_back(y);
                        }
                    }
                    drawn.push_back(std::move(d));
                } else if (L.type == 3 && f.content && objects_.count(f.src)) {
                    evalMotion(e, f.src, f.icon, lt + f.timeOffset, mine, z, opa, drawn, depth + 1);
                }
                items.emplace_back(L.index, std::move(drawn));

                std::vector<Step> childSteps = mine;
                if (L.meshTransform && (L.meshSyncChildMask & 8) != 0 && f.hasMesh && haveRect) {
                    Step s;
                    s.ffd = true;
                    s.w = rw, s.h = rh, s.ox = rox, s.oy = roy;
                    s.mesh = f.mesh;
                    childSteps.insert(childSteps.begin(), s);
                }
                // A stencil layer (type 12) masks what is under it (0x1004958b copies stencilType to
                // each draw's chain, 0x10049287 links it to the stencil above).
                const bool stencil = L.type == 12 && (L.stencilType & 3) != 0 && (L.stencilType & 4) != 0;
                if (stencil) e.stencil.push_back({L.stencilType & 3, L.stencilMasks});
                walk(L.children, childSteps, z, opa);
                if (stencil) e.stencil.pop_back();
            }
        };
    walk(m.roots, xf, z0, opa0);

    // The priority list in force (front to back) orders this motion's items; depth sorting later
    // keeps this order between items at the same depth.
    const std::vector<int>* order = nullptr;
    for (const auto& [pt, list] : m.priority) {
        if (pt <= t + 1e-4f && !list.empty()) order = &list;
    }
    std::map<int, int> rank;
    if (order) {
        for (size_t n = 0; n < order->size(); ++n) rank[(*order)[n]] = static_cast<int>(n);
    }
    auto rankOf = [&](int ix) {
        auto it = rank.find(ix);
        return it == rank.end() ? -1 : it->second;
    };
    std::stable_sort(items.begin(), items.end(),
                     [&](const auto& a, const auto& b) { return rankOf(a.first) > rankOf(b.first); });
    for (auto& [ix, drawn] : items) {
        for (auto& d : drawn) out.push_back(std::move(d));
    }
}

}  // namespace nyan::cs2
