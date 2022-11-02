/*
Copyright Contributors to the libdnf project.

This file is part of libdnf: https://github.com/rpm-software-management/libdnf/

Libdnf is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 2.1 of the License, or
(at your option) any later version.

Libdnf is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with libdnf.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "module/module_goal_private.hpp"

#include "module/module_sack_impl.hpp"
#include "rpm/solv/goal_private.hpp"
#include "utils/bgettext/bgettext-mark-domain.h"

#include "libdnf/base/goal_elements.hpp"
#include "libdnf/base/transaction.hpp"
#include "libdnf/module/module_sack.hpp"
#include "libdnf/module/module_sack_weak.hpp"

extern "C" {
#include <solv/solver.h>
}

namespace libdnf::module {


void ModuleGoalPrivate::add_provide_install(Id reldepid, bool strict, bool best) {
    staging.push_back(
        SOLVER_INSTALL | SOLVER_SOLVABLE_PROVIDES | SOLVER_SETARCH | SOLVER_SETEVR | (strict ? 0 : SOLVER_WEAK) |
            (best ? SOLVER_FORCEBEST : 0),
        reldepid);
}


void init_solver(Pool * pool, Solver ** solver) {
    if (*solver) {
        solver_free(*solver);
    }

    *solver = solver_create(pool);
}


libdnf::GoalProblem ModuleGoalPrivate::resolve() {
    Pool * pool = module_sack->p_impl->pool;
    libdnf::solv::IdQueue job(staging);

    if (libsolv_transaction) {
        transaction_free(libsolv_transaction);
        libsolv_transaction = NULL;
    }

    init_solver(pool, &libsolv_solver);

    if (solver_solve(libsolv_solver, &job.get_queue())) {
        return libdnf::GoalProblem::SOLVER_ERROR;
    }

    libsolv_transaction = solver_create_transaction(libsolv_solver);

    return libdnf::GoalProblem::NO_PROBLEM;
}


libdnf::solv::IdQueue ModuleGoalPrivate::list_results(Id type_filter1, Id type_filter2) {
    /* no transaction */
    if (!libsolv_transaction) {
        libdnf_assert_goal_resolved();

        // TODO(pkratoch) replace with a proper and descriptive exception
        throw RuntimeError(M_("no solution possible"));
    }

    libdnf::solv::IdQueue result_ids;
    const int common_mode = SOLVER_TRANSACTION_SHOW_OBSOLETES | SOLVER_TRANSACTION_CHANGE_IS_REINSTALL;

    for (int i = 0; i < libsolv_transaction->steps.count; ++i) {
        Id p = libsolv_transaction->steps.elements[i];
        Id type;

        switch (type_filter1) {
            case SOLVER_TRANSACTION_OBSOLETED:
                type = transaction_type(libsolv_transaction, p, common_mode);
                break;
            default:
                type = transaction_type(
                    libsolv_transaction, p, common_mode | SOLVER_TRANSACTION_SHOW_ACTIVE | SOLVER_TRANSACTION_SHOW_ALL);
                break;
        }

        if (type == type_filter1 || (type_filter2 && type == type_filter2)) {
            result_ids.push_back(p);
        }
    }
    return result_ids;
}


libdnf::solv::IdQueue ModuleGoalPrivate::list_installs() {
    return list_results(SOLVER_TRANSACTION_INSTALL, SOLVER_TRANSACTION_OBSOLETES);
}


size_t ModuleGoalPrivate::count_solver_problems() {
    libdnf_assert_goal_resolved();

    return solver_problem_count(libsolv_solver);
}


std::vector<std::vector<std::tuple<ProblemRules, Id, Id, Id, std::string>>> ModuleGoalPrivate::get_problems() {
    auto & pool = module_sack->p_impl->pool;

    libdnf_assert_goal_resolved();

    auto count_problems = static_cast<int>(count_solver_problems());
    if (count_problems == 0) {
        return {};
    }
    // std::tuple<ProblemRules, Id source, Id dep, Id target, std::string>>
    std::vector<std::vector<std::tuple<ProblemRules, Id, Id, Id, std::string>>> problems;

    libdnf::solv::IdQueue problem_queue;
    libdnf::solv::IdQueue descriptions_queue;
    // libsolv counts problem from 1
    for (int i = 1; i <= count_problems; ++i) {
        solver_findallproblemrules(libsolv_solver, i, &problem_queue.get_queue());
        std::vector<std::tuple<ProblemRules, Id, Id, Id, std::string>> problem;
        for (int j = 0; j < problem_queue.size(); ++j) {
            Id rid = problem_queue[j];
            if (solver_allruleinfos(libsolv_solver, rid, &descriptions_queue.get_queue())) {
                for (int ir = 0; ir < descriptions_queue.size(); ir += 4) {
                    SolverRuleinfo type = static_cast<SolverRuleinfo>(descriptions_queue[ir]);
                    Id source = descriptions_queue[ir + 1];
                    Id target = descriptions_queue[ir + 2];
                    Id dep = descriptions_queue[ir + 3];
                    ProblemRules rule;
                    const char * solv_strig = nullptr;
                    switch (type) {
                        case SOLVER_RULE_DISTUPGRADE:
                            rule = ProblemRules::RULE_DISTUPGRADE;
                            break;
                        case SOLVER_RULE_INFARCH:
                            rule = ProblemRules::RULE_INFARCH;
                            break;
                        case SOLVER_RULE_UPDATE:
                            rule = ProblemRules::RULE_UPDATE;
                            break;
                        case SOLVER_RULE_JOB:
                            rule = ProblemRules::RULE_JOB;
                            break;
                        case SOLVER_RULE_JOB_UNSUPPORTED:
                            rule = ProblemRules::RULE_JOB_UNSUPPORTED;
                            break;
                        case SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP:
                            rule = ProblemRules::RULE_JOB_NOTHING_PROVIDES_DEP;
                            break;
                        case SOLVER_RULE_JOB_UNKNOWN_PACKAGE:
                            rule = ProblemRules::RULE_JOB_UNKNOWN_PACKAGE;
                            break;
                        case SOLVER_RULE_JOB_PROVIDED_BY_SYSTEM:
                            rule = ProblemRules::RULE_JOB_PROVIDED_BY_SYSTEM;
                            break;
                        case SOLVER_RULE_PKG:
                            rule = ProblemRules::RULE_PKG;
                            break;
                        case SOLVER_RULE_BEST:
                            if (source > 0) {
                                rule = ProblemRules::RULE_BEST_1;
                                break;
                            }
                            rule = ProblemRules::RULE_BEST_2;
                            break;
                        case SOLVER_RULE_PKG_NOT_INSTALLABLE: {
                            Solvable * solvable = pool_id2solvable(pool, source);
                            if (pool_disabled_solvable(pool, solvable)) {
                                // TODO(jmracek) RULE_PKG_NOT_INSTALLABLE_4 not handled
                                rule = ProblemRules::RULE_PKG_NOT_INSTALLABLE_1;
                                break;
                            }
                            if (solvable->arch && solvable->arch != ARCH_SRC && solvable->arch != ARCH_NOSRC &&
                                pool->id2arch && (solvable->arch > pool->lastarch || !pool->id2arch[solvable->arch])) {
                                rule = ProblemRules::RULE_PKG_NOT_INSTALLABLE_2;
                                break;
                            }
                            rule = ProblemRules::RULE_PKG_NOT_INSTALLABLE_3;
                        } break;
                        case SOLVER_RULE_PKG_NOTHING_PROVIDES_DEP:
                            rule = ProblemRules::RULE_PKG_NOTHING_PROVIDES_DEP;
                            break;
                        case SOLVER_RULE_PKG_SAME_NAME:
                            rule = ProblemRules::RULE_PKG_SAME_NAME;
                            break;
                        case SOLVER_RULE_PKG_CONFLICTS:
                            rule = ProblemRules::RULE_PKG_CONFLICTS;
                            break;
                        case SOLVER_RULE_PKG_OBSOLETES:
                            rule = ProblemRules::RULE_PKG_OBSOLETES;
                            break;
                        case SOLVER_RULE_PKG_INSTALLED_OBSOLETES:
                            rule = ProblemRules::RULE_PKG_INSTALLED_OBSOLETES;
                            break;
                        case SOLVER_RULE_PKG_IMPLICIT_OBSOLETES:
                            rule = ProblemRules::RULE_PKG_IMPLICIT_OBSOLETES;
                            break;
                        case SOLVER_RULE_PKG_REQUIRES:
                            rule = ProblemRules::RULE_PKG_REQUIRES;
                            break;
                        case SOLVER_RULE_PKG_SELF_CONFLICT:
                            rule = ProblemRules::RULE_PKG_SELF_CONFLICT;
                            break;
                        case SOLVER_RULE_YUMOBS:
                            rule = ProblemRules::RULE_YUMOBS;
                            break;
                        default:
                            rule = ProblemRules::RULE_UNKNOWN;
                            solv_strig = solver_problemruleinfo2str(libsolv_solver, type, source, target, dep);
                            break;
                    }
                    problem.emplace_back(std::make_tuple(
                        rule, source, dep, target, solv_strig ? std::string(solv_strig) : std::string()));
                }
            }
        }
        problems.push_back(std::move(problem));
    }
    return problems;
}


}  // namespace libdnf::module