#include "../analysis/generic_static_analysis.h"
#include "../analysis/query.h"
#include "../parameter.h"
#include "../pir/pir_impl.h"
#include "../transform/bb.h"
#include "../transform/replace.h"
#include "pass_definitions.h"
#include "utils/Map.h"
#include "utils/Set.h"

namespace {

using namespace rir::pir;

/* This optimization removes redundant force instructions:
 *
 * b = force(a)
 * c = force(b)
 *
 * For that we need to compute a dominance graph of forces.
 *
 * Additionally, if we know the promise being forced, we try to inline it. For
 * example:
 *
 * a = mkArg(prom(0))
 * b = force(a)
 *
 * will be translated to:
 *
 * b = <inlined prom(0)>
 *
 * But, in the case of promises with side-effects we can only inline them iff
 * there is a unique dominating force instruction.
 *
 * For example in the following case:
 *
 *      Branch
 *   /          \
 * force         |
 *   \         /
 *     \     /
 *        |
 *      force
 *
 * we don't know at the second force if the promise was forced (by the left
 * branch) or not. Thus we cannot inline it.
 */

struct ForcedBy {
    std::unordered_map<Value*, Force*> forcedBy;
    rir::SmallSet<Value*> inScope;
    rir::SmallSet<Value*> escaped;

    std::vector<size_t> argumentForceOrder;
    bool ambiguousForceOrder = false;

    static Force* ambiguous() {
        static Force f(Nil::instance(), Env::nil());
        return &f;
    }

    bool declare(Value* arg) {
        bool changed = false;
        if (!inScope.count(arg)) {
            inScope.insert(arg);
            changed = true;
        }
        auto f = forcedBy.find(arg);
        if (f != forcedBy.end()) {
            forcedBy.erase(f);
            changed = true;
        }
        auto e = escaped.find(arg);
        if (e != escaped.end()) {
            escaped.erase(e);
            changed = true;
        }
        return changed;
    }

    bool sideeffect() {
        bool changed = false;
        // when we execute an instruction that could force promises as a
        // sideeffect, we have to assume that all escaped promises might have
        // been forced

        for (auto& e : escaped) {
            if (!forcedBy.count(e)) {
                forcedBy[e] = ambiguous();
                changed = true;
            }
        }

        return changed;
    }

    bool forcedAt(Value* val, Force* force) {
        if (!forcedBy.count(val)) {
            forcedBy[val] = force;
            return true;
        }
        return false;
    }

    bool escape(Value* val) {
        if (!forcedBy.count(val) && !escaped.count(val)) {
            escaped.insert(val);
            return true;
        }

        return false;
    }

    AbstractResult mergeExit(const ForcedBy& other) {
        AbstractResult res;

        for (auto& e : forcedBy) {
            auto v = e.first;
            auto f = e.second;
            if (other.forcedBy.count(v)) {
                if (f != other.forcedBy.at(v)) {
                    if (e.second != ambiguous()) {
                        e.second = ambiguous();
                        res.lostPrecision();
                    }
                }
            }
        }
        for (auto& e : other.forcedBy) {
            if (!forcedBy.count(e.first)) {
                if (inScope.count(e.first)) {
                    forcedBy.emplace(e);
                    res.update();
                } else {
                    inScope.insert(e.first);
                    forcedBy[e.first] = e.second;
                    res.update();
                }
            }
        }
        for (auto& e : other.escaped) {
            if (!escaped.count(e)) {
                escaped.insert(e);
                res.update();
            }
        }

        if (!ambiguousForceOrder && other.ambiguousForceOrder) {
            ambiguousForceOrder = true;
            res.update();
        }

        if (argumentForceOrder != other.argumentForceOrder) {
            auto mySize = argumentForceOrder.size();
            auto otherSize = other.argumentForceOrder.size();
            auto common = mySize;

            if (mySize > otherSize) {
                argumentForceOrder.resize(otherSize);
                ambiguousForceOrder = true;
                common = otherSize;
                res.update();
            } else if (!ambiguousForceOrder && otherSize > mySize) {
                ambiguousForceOrder = true;
                res.update();
            }

            for (size_t i = 0; i < common; ++i) {
                if (argumentForceOrder[i] != other.argumentForceOrder[i]) {
                    argumentForceOrder.resize(i);
                    ambiguousForceOrder = true;
                    res.update();
                    break;
                }
            }
        }

        return res;
    }

    AbstractResult merge(const ForcedBy& other) {
        AbstractResult res;

        // Those are the cases where we merge two branches where one branch has
        // the promise evaluated and the other not. For exits we don't care
        // about this case.
        for (auto& e : forcedBy) {
            auto v = e.first;
            if (!other.forcedBy.count(v)) {
                if (other.inScope.count(v)) {
                    if (e.second != ambiguous()) {
                        e.second = ambiguous();
                        res.lostPrecision();
                    }
                }
            }
        }
        for (auto& e : other.forcedBy) {
            if (!forcedBy.count(e.first)) {
                if (inScope.count(e.first)) {
                    forcedBy[e.first] = ambiguous();
                    res.lostPrecision();
                }
            }
        }

        res.max(mergeExit(other));

        return res;
    }

    bool maybeForced(size_t i) const {
        // Scan the list of unambiguously forced arguments to see if we know if
        // this one was forced
        for (auto f : argumentForceOrder) {
            if (f == i)
                return true;
        }
        return ambiguousForceOrder;
    }

    bool eagerLikeFunction(ClosureVersion* fun) const {
        if (ambiguousForceOrder ||
            argumentForceOrder.size() < fun->effectiveNArgs())
            return false;
        for (size_t i = 0; i < fun->effectiveNArgs(); ++i)
            if (argumentForceOrder[i] != i)
                return false;
        return true;
    }

    bool isDominatingForce(Force* f) const {
        return f == getDominatingForce(f);
    }

    Force* getDominatingForce(Force* f) const {
        auto a = f->arg<0>().val()->followCasts();
        if (!forcedBy.count(a)) {
            return nullptr;
        }
        auto res = forcedBy.at(a);
        if (res == ambiguous()) {
            return nullptr;
        }
        return res;
    }

    enum PromiseInlineable {
        SafeToInline,
        SafeToInlineWithUpdate,
        NotSafeToInline
    };

    PromiseInlineable isSafeToInline(MkArg* a) const {
        // To inline promises with a deopt instruction we need to be able to
        // synthesize promises and promise call frames.
        auto prom = a->prom();
        if (hasDeopt.count(prom)) {
            if (hasDeopt.at(prom))
                return NotSafeToInline;
        } else {
            auto deopt = !Query::noDeopt(prom);
            const_cast<ForcedBy*>(this)->hasDeopt[prom] = deopt;
            if (deopt)
                return NotSafeToInline;
        }
        return escaped.count(a) ? SafeToInlineWithUpdate : SafeToInline;
    }
    std::unordered_map<Promise*, bool> hasDeopt;

    void print(std::ostream& out, bool tty) {
        out << "Known proms: ";
        for (auto& p : inScope) {
            p->printRef(out);
            out << " ";
        }
        out << "\n";
        out << "Escaped proms: ";
        for (auto& p : escaped) {
            p->printRef(out);
            out << " ";
        }
        out << "\n";
        for (auto& e : forcedBy) {
            e.first->printRef(out);
            if (e.second == ambiguous()) {
                out << " force is ambiguous\n";
            } else {
                out << " is forced by ";
                e.second->printRef(out);
                out << "\n";
            }
        }
    }
};

//, DummyState, AnalysisDebugLevel::Taint
class ForceDominanceAnalysis : public StaticAnalysis<ForcedBy> {
  public:
    using StaticAnalysis::PositioningStyle;
    const CFG cfg;
    explicit ForceDominanceAnalysis(ClosureVersion* cls, Code* code,
                                    LogStream& log)
        : StaticAnalysis("ForceDominance", cls, code, log), cfg(code) {}

    AbstractResult apply(ForcedBy& state, Instruction* i) const override {
        AbstractResult res;
        auto apply = [&](Instruction* i) {
            i->eachArg([&](Value* v) {
                v = v->followCasts();
                auto instruction = Instruction::Cast(v);
                if (MkArg::Cast(v) || LdArg::Cast(v) ||
                    (instruction && instruction->type.maybeLazy()))
                    if (state.escape(instruction))
                        res.update();
            });
        };
        if (auto f = Force::Cast(i)) {
            if (LdArg* arg = LdArg::Cast(f->arg<0>().val()->followCasts())) {
                if (arg->type.maybeLazy()) {
                    if (state.forcedAt(arg, f))
                        res.update();
                    if (!state.ambiguousForceOrder &&
                        !state.maybeForced(arg->id)) {
                        state.argumentForceOrder.push_back(arg->id);
                        res.update();
                    }
                }
            } else {
                auto instruction =
                    Instruction::Cast(f->arg<0>().val()->followCasts());
                if (MkArg::Cast(f->arg<0>().val()->followCasts()) ||
                    (instruction && instruction->type.maybeLazy())) {
                    if (state.forcedAt(instruction, f))
                        res.update();
                }
            }
        } else if (auto mk = MkArg::Cast(i)) {
            if (state.declare(mk))
                res.update();
        } else if (auto e = MkEnv::Cast(i)) {
            if (!e->stub)
                apply(e);
        } else if (CastType::Cast(i) || Deopt::Cast(i)) { /* do nothing */ 
        } else {
            if (i->type.maybeLazy()) {
                if (state.declare(i))
                    res.update();
            }
            apply(i);

            if (i->effects.contains(Effect::Force)) {
                if (state.sideeffect())
                    res.taint();
            }

            if (i->effects.includes(Effect::Force) &&
                !state.ambiguousForceOrder &&
                state.argumentForceOrder.size() < closure->effectiveNArgs()) {
                // After the first effect we give up on recording force order,
                // since we can't use it to turn the arguments into eager ones
                // anyway. Otherwise we would reorder effects.
                state.ambiguousForceOrder = true;
                res.taint();
            }
        }
        return res;
    }
};

} // namespace

namespace rir {
namespace pir {

void ForceDominance::apply(RirCompiler&, ClosureVersion* code,
                           LogStream& log) const {
    SmallSet<Force*> toInline;
    SmallSet<Force*> needsUpdate;
    SmallMap<Force*, Force*> dominatedBy;

    bool isHuge = code->size() > Parameter::PROMISE_INLINER_MAX_SIZE;
    {
        ForceDominanceAnalysis analysis(code, code, log);
        analysis();

        auto result = analysis.result();
        if (result.eagerLikeFunction(code))
            code->properties.set(ClosureVersion::Property::IsEager);
        code->properties.argumentForceOrder = result.argumentForceOrder;

        VisitorNoDeoptBranch::run(code->entry, [&](BB* bb) {
            auto ip = bb->begin();
            while (ip != bb->end()) {
                auto next = ip + 1;
                auto i = *ip;

                if (auto f = Force::Cast(i)) {
                    auto a = analysis.resultIgnoringUnreachableExits(
                        i, analysis.cfg);
                    if (a.isDominatingForce(f)) {
                        f->strict = true;
                        if (auto mk = MkArg::Cast(f->followCastsAndForce())) {
                            if (!mk->isEager()) {
                                if (!isHuge || mk->prom()->size() < 10) {
                                    auto query = analysis.after(i);
                                    auto inl = query.isSafeToInline(mk);
                                    if (inl != ForcedBy::NotSafeToInline) {
                                        toInline.insert(f);
                                        if (inl ==
                                            ForcedBy::SafeToInlineWithUpdate)
                                            needsUpdate.insert(f);
                                    }
                                }
                            }
                        }
                    } else if (auto dom = a.getDominatingForce(f)) {
                        if (f != dom)
                            dominatedBy[f] = dom;
                    }
                } else if (auto u = UpdatePromise::Cast(i)) {
                    if (auto mkarg = MkArg::Cast(u->arg(0).val())) {
                        if (!analysis.before(i).escaped.count(mkarg))
                            next = bb->remove(ip);
                    }
                }

                ip = next;
            }
        });
    }

    std::unordered_map<Force*, Value*> inlinedPromise;
    std::unordered_map<Instruction*, MkArg*> forcedMkArg;

    // 1. Inline dominating promises
    Visitor::runPostChange(code->entry, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            auto next = ip + 1;
            if (auto f = Force::Cast(*ip)) {
                if (auto mkarg = MkArg::Cast(f->followCastsAndForce())) {
                    if (mkarg->isEager()) {
                        Value* eager = mkarg->eagerArg();
                        f->replaceUsesWith(eager);
                        next = bb->remove(ip);
                    } else if (toInline.count(f)) {
                        Promise* prom = mkarg->prom();
                        BB* split =
                            BBTransform::split(code->nextBBId++, bb, ip, code);
                        BB* prom_copy =
                            BBTransform::clone(prom->entry, code, code);
                        bb->overrideNext(prom_copy);

                        // For now we assume every promise starts with a
                        // LdFunctionEnv instruction. We replace it's
                        // usages with the caller environment.
                        LdFunctionEnv* e =
                            LdFunctionEnv::Cast(*prom_copy->begin());
                        assert(e);
                        Replace::usesOfValue(prom_copy, e, mkarg->promEnv());
                        prom_copy->remove(prom_copy->begin());

                        // Update environment dependency of inlined forces:
                        // the inlined forces can see local env of this
                        // function if it is stored on the context.
                        if (auto mkenv = MkEnv::Cast(f->env())) {
                            if (mkenv->context) {
                                Visitor::run(prom_copy, [&](Instruction* i) {
                                    if (auto fi = Force::Cast(i)) {
                                        if (fi->hasEnv()) {
                                            fi->env(f->env());
                                        }
                                    }
                                });
                            }
                        }

                        // Create a return value phi of the promise
                        Value* promRes =
                            BBTransform::forInline(prom_copy, split).first;

                        assert(!promRes->type.maybePromiseWrapped());
                        f = Force::Cast(*split->begin());
                        assert(f);
                        f->replaceUsesWith(promRes);
                        split->remove(split->begin());

                        MkArg* fixedMkArg =
                            new MkArg(mkarg->prom(), promRes, mkarg->promEnv());
                        next = split->insert(split->begin(), fixedMkArg);
                        forcedMkArg[mkarg] = fixedMkArg;

                        inlinedPromise[f] = promRes;
                        if (needsUpdate.count(f))
                            next = split->insert(
                                next, new UpdatePromise(mkarg, promRes));

                        break;
                    }
                }
            } else if (auto cast = CastType::Cast(*ip)) {
                if (auto mk = MkArg::Cast(cast->arg<0>().val())) {
                    if (mk->isEager()) {
                        auto eager = mk->eagerArg();
                        cast->replaceUsesWith(eager);
                        next = bb->remove(ip);
                    }
                }
            }
            ip = next;
        }
    });

    // 2. replace dominated promises
    Visitor::run(code->entry, [&](BB* bb) {
        auto ip = bb->begin();
        while (ip != bb->end()) {
            auto f = Force::Cast(*ip);
            auto next = ip + 1;
            if (f) {
                // If this force instruction is dominated by another force
                // we can replace it with the dominating instruction
                auto dom = dominatedBy.find(f);
                if (dom != dominatedBy.end()) {
                    assert(f != dom->second);
                    if (inlinedPromise.count(dom->second))
                        f->replaceUsesWith(inlinedPromise.at(dom->second));
                    else
                        f->replaceUsesWith(dom->second);
                    next = bb->remove(ip);
                }
            }
            ip = next;
        }
    });

    // 3. replace remaining uses of the mkarg itself
    for (auto m : forcedMkArg) {
        m.first->replaceDominatedUses(m.second);
    }
}

size_t Parameter::PROMISE_INLINER_MAX_SIZE =
    getenv("PIR_PROMISE_INLINER_MAX_SIZE")
        ? atoi(getenv("PIR_PROMISE_INLINER_MAX_SIZE"))
        : 3000;
} // namespace pir
} // namespace rir
