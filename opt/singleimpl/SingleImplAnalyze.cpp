/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <stdio.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "SingleImplDefs.h"
#include "SingleImpl.h"
#include "SingleImplUtil.h"
#include "Debug.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Trace.h"
#include "Walkers.h"

struct AnalysisImpl : SingleImplAnalysis {
  AnalysisImpl(const Scope& scope, DexClasses& primary_dex)
      : SingleImplAnalysis(), scope(scope) {
    for (const auto& cls : primary_dex) {
      primary.insert(cls->get_type());
    }
  }

  void create_single_impl(const TypeMap& single_impl,
                          const TypeSet& intfs,
                          const SingleImplConfig& config);
  void collect_field_defs();
  void collect_method_defs();
  void analyze_opcodes();
  void escape_not_in_primary();
  void remove_escaped();

 private:
  DexType* get_and_check_single_impl(DexType* type);
  void collect_children(const TypeSet& intfs);
  void check_impl_hierarchy();
  void escape_with_clinit();
  void escape_with_sfields();
  void filter_single_impl(const SingleImplConfig& config);
  void filter_do_not_strip();
  void filter_list(const std::vector<std::string>& list, bool keep_match);

 private:
  const Scope& scope;
  std::unordered_set<DexType*> primary;
};

/**
 * Get the single impl if the type is a single impl or an array of it.
 * When an array mark the single impl as having an array type.
 * Return nullptr otherwise.
 */
DexType* AnalysisImpl::get_and_check_single_impl(DexType* type) {
  if (exists(single_impls, type)) return type;
  if (is_array(type)) {
    auto array_type = get_array_type(type);
    assert(array_type);
    const auto sit = single_impls.find(array_type);
    if (sit != single_impls.end()) {
      escape_interface(sit->first, HAS_ARRAY_TYPE);
      return sit->first;
    }
  }
  return nullptr;
}

/**
 * Find all single implemented interfaces.
 */
void AnalysisImpl::create_single_impl(const TypeMap& single_impl,
                                      const TypeSet& intfs,
                                      const SingleImplConfig& config) {
  for (const auto intf_it : single_impl) {
    auto intf = intf_it.first;
    auto intf_cls = type_class(intf);
    always_assert(intf_cls && !intf_cls->is_external());
    if (is_annotation(intf_cls)) continue;
    auto impl = intf_it.second;
    auto impl_cls = type_class(impl);
    always_assert(impl_cls && !impl_cls->is_external());
    if (is_annotation(impl_cls)) continue;
    single_impls[intf].cls = impl;
  }
  collect_children(intfs);
  check_impl_hierarchy();
  escape_with_clinit();
  escape_with_sfields();
  filter_single_impl(config);
  filter_do_not_strip();
}

/**
 * Filter common function for both white and black list.
 */
void AnalysisImpl::filter_list(
  const std::vector<std::string>& list,
  bool keep_match
) {
  if (list.empty()) return;

  auto find_in_list = [&](const std::string& name) {
    for (const std::string& el_name : list) {
      if (name == el_name) {
        return true;
      }
    }
    return false;
  };

  for (const auto intf_it : single_impls) {
    const auto intf = intf_it.first;
    const auto intf_cls = type_class(intf);
    const std::string& intf_name = intf_cls->get_deobfuscated_name();
    bool match = find_in_list(intf_name);
    if (match && keep_match) continue;
    if (!match && !keep_match) continue;
    escape_interface(intf, FILTERED);
  }
}

/**
 * Apply filters to the set of single impl found.
 * White lists come first, then black lists.
 */
void AnalysisImpl::filter_single_impl(const SingleImplConfig& config) {
  filter_list(config.white_list, true);
  filter_list(config.package_white_list, true);
  filter_list(config.black_list, false);
  filter_list(config.package_black_list, false);
}

/**
 * Do not optimize DoNotStrip interfaces.
 */
void AnalysisImpl::filter_do_not_strip() {
  for (const auto intf_it : single_impls) {
    if (!can_delete(type_class(intf_it.first))) {
      escape_interface(intf_it.first, DO_NOT_STRIP);
    }
  }
}

/**
 * Collect direct children of interfaces.
 */
void AnalysisImpl::collect_children(const TypeSet& intfs) {
  for (auto& intf : intfs) {
    auto supers = type_class(intf)->get_interfaces();
    for (auto super : supers->get_type_list()) {
      auto super_it = single_impls.find(super);
      if (super_it != single_impls.end()) {
        super_it->second.children.insert(intf);
      }
    }
  }
}

/**
 * Escape if any parent is not known to redex.
 */
void AnalysisImpl::check_impl_hierarchy() {
  for (auto& intf_it : single_impls) {
    if (!has_hierarchy_in_scope(type_class(intf_it.second.cls))) {
      escape_interface(intf_it.first, IMPL_PARENT_ESCAPED);
    }
  }
}

/**
 * Escape interfaces with static initializer.
 */
void AnalysisImpl::escape_with_clinit() {
  for (auto& intf_it : single_impls) {
    // strictly speaking this is not checking for a clinit but it's all the
    // same. Interfaces should not have static methods and even if so we
    // just escape them. From our analysis it turns out there are few with
    // clinit only and as expected none with static methods.
    if (type_class(intf_it.first)->get_dmethods().size() > 0) {
      escape_interface(intf_it.first, CLINIT);
    }
  }
}

/**
 * Escape interfaces with static fields. Also escape the type of the field
 * if it is a single impl.
 * Interface fields may not be scoped to the interface itself and resolved
 * at runtime. So until we have an analysis that can spot those cases we
 * give up on interfaces with fields and the type of the field if it is
 * a single impl.
 */
void AnalysisImpl::escape_with_sfields() {
  for (auto const& intf_it : single_impls) {
    auto intf_cls = type_class(intf_it.first);
    assert(intf_cls->get_ifields().size() == 0);
    always_assert(!intf_cls->is_external());
    const auto& sfields = intf_cls->get_sfields();
    if (sfields.size() == 0) continue;
    escape_interface(intf_it.first, HAS_SFIELDS);
    for (auto sfield : sfields) {
      auto ftype = sfield->get_class();
      auto simpl = get_and_check_single_impl(ftype);
      if (simpl) {
        escape_interface(simpl, HAS_SFIELDS);
      }
    }
  }
}

/**
 * If an interface in primary brings a class not in primary drop the
 * optimization.
 */
void AnalysisImpl::escape_not_in_primary() {
  for (auto const& intf_it : single_impls) {
    if (primary.count(intf_it.first) == 0) continue;
    if (primary.count(intf_it.second.cls) == 0) {
      escape_interface(intf_it.first, NOT_IN_PRIMARY);
    }
  }
}

/**
 * Clean up the single impl map.
 */
void AnalysisImpl::remove_escaped() {
  auto it = single_impls.begin();
  while (it != single_impls.end()) {
    if (it->second.is_escaped()) {
      it = single_impls.erase(it);
    } else {
      ++it;
    }
  }
}

/**
 * Find all fields typed with the single impl interface.
 */
void AnalysisImpl::collect_field_defs() {
  walk_fields(scope,
              [&](DexField* field) {
                auto type = field->get_type();
                auto intf = get_and_check_single_impl(type);
                if (intf) {
                  single_impls[intf].fielddefs.push_back(field);
                }
              });
}

/**
 * Find all methods with a single impl interface in their signature.
 * Also if a method with the interface in the signature is native mark the
 * interface as "escaped".
 */
void AnalysisImpl::collect_method_defs() {

  auto check_method_arg = [&](DexType* type, DexMethod* method, bool native) {
    auto intf = get_and_check_single_impl(type);
    if (!intf) return;
    if (native) {
      escape_interface(intf, NATIVE_METHOD);
    }
    if (method->get_class() == intf) {
      escape_interface(intf, SELF_REFERENCE);
    }
    single_impls[intf].methoddefs.insert(method);
  };

  walk_methods(scope,
    [&](DexMethod* method) {
      auto proto = method->get_proto();
      bool native = is_native(method);
      check_method_arg(proto->get_rtype(), method, native);
      auto args = proto->get_args();
      for (const auto it : args->get_type_list()) {
        check_method_arg(it, method, native);
      }
    });
}

/**
 * Find all opcodes that reference a single implemented interface in a typeref,
 * fieldref or methodref.
 */
void AnalysisImpl::analyze_opcodes() {

  auto check_arg = [&](DexType* type, DexMethodRef* meth, IRInstruction* insn) {
    auto intf = get_and_check_single_impl(type);
    if (intf) {
      single_impls[intf].methodrefs[meth].insert(insn);
    }
  };

  auto check_sig = [&](DexMethodRef* meth, IRInstruction* insn) {
    // check the sig for single implemented interface
    const auto proto = meth->get_proto();
    check_arg(proto->get_rtype(), meth, insn);
    const auto args = proto->get_args();
    for (const auto arg : args->get_type_list()) {
      check_arg(arg, meth, insn);
    }
  };

  auto check_field = [&](DexFieldRef* field, IRInstruction* insn) {
    auto cls = field->get_class();
    cls = get_and_check_single_impl(cls);
    if (cls) {
      escape_interface(cls, HAS_FIELD_REF);
    }
    const auto type = field->get_type();
    auto intf = get_and_check_single_impl(type);
    if (intf) {
      single_impls[intf].fieldrefs[field].push_back(insn);
    }
  };

  walk_opcodes(scope,
               [](DexMethod* method) { return true; },
               [&](DexMethod* method, IRInstruction* insn) {
                 auto op = insn->opcode();
                 switch (op) {
                 // type ref
                 case OPCODE_CONST_CLASS: {
                   // const_class is problematic because DI can use it as a key
                   // to mark
                   // different instances to retrieve, so we simply drop all
                   // single impl
                   // that are used with const_class
                   const auto typeref = insn->get_type();
                   auto intf = get_and_check_single_impl(typeref);
                   if (intf) {
                     escape_interface(intf, CONST_CLASS);
                   }
                   return;
                 }
                 case OPCODE_CHECK_CAST:
                 case OPCODE_INSTANCE_OF:
                 case OPCODE_NEW_INSTANCE:
                 case OPCODE_NEW_ARRAY:
                 case OPCODE_FILLED_NEW_ARRAY:
                 case OPCODE_FILLED_NEW_ARRAY_RANGE: {
                   auto intf = get_and_check_single_impl(insn->get_type());
                   if (intf) {
                     single_impls[intf].typerefs.push_back(insn);
                   }
                   return;
                 }
                 // field ref
                 case OPCODE_IGET:
                 case OPCODE_IGET_WIDE:
                 case OPCODE_IGET_OBJECT:
                 case OPCODE_IPUT:
                 case OPCODE_IPUT_WIDE:
                 case OPCODE_IPUT_OBJECT: {
                   DexFieldRef* field =
                       resolve_field(insn->get_field(), FieldSearch::Instance);
                   if (field == nullptr) {
                     field = insn->get_field();
                   }
                   check_field(field, insn);
                   return;
                 }
                 case OPCODE_SGET:
                 case OPCODE_SGET_WIDE:
                 case OPCODE_SGET_OBJECT:
                 case OPCODE_SPUT:
                 case OPCODE_SPUT_WIDE:
                 case OPCODE_SPUT_OBJECT: {
                   DexFieldRef* field =
                       resolve_field(insn->get_field(), FieldSearch::Static);
                   if (field == nullptr) {
                     field = insn->get_field();
                   }
                   check_field(field, insn);
                   return;
                 }
                 // method ref
                 case OPCODE_INVOKE_INTERFACE:
                 case OPCODE_INVOKE_INTERFACE_RANGE: {
                   // if it is an invoke on the interface method, collect it as
                   // such
                   const auto meth = insn->get_method();
                   const auto owner = meth->get_class();
                   const auto intf = get_and_check_single_impl(owner);
                   if (intf) {
                     // if the method ref is not defined on the interface itself
                     // drop the optimization
                     const auto& meths = type_class(intf)->get_vmethods();
                     if (std::find(meths.begin(), meths.end(), meth) ==
                         meths.end()) {
                       escape_interface(intf, UNKNOWN_MREF);
                     } else {
                       single_impls[intf].intf_methodrefs[meth].insert(insn);
                     }
                   }
                   check_sig(meth, insn);
                   return;
                 }

                 case OPCODE_INVOKE_DIRECT:
                 case OPCODE_INVOKE_DIRECT_RANGE:
                 case OPCODE_INVOKE_STATIC:
                 case OPCODE_INVOKE_STATIC_RANGE:
                 case OPCODE_INVOKE_VIRTUAL:
                 case OPCODE_INVOKE_VIRTUAL_RANGE:
                 case OPCODE_INVOKE_SUPER:
                 case OPCODE_INVOKE_SUPER_RANGE: {
                   const auto meth = insn->get_method();
                   check_sig(meth, insn);
                   return;
                 }
                 default:
                   return;
                 }
               });
}

/**
 * Main analysis method
 */
std::unique_ptr<SingleImplAnalysis> SingleImplAnalysis::analyze(
    const Scope& scope, DexClasses& primary_dex, const TypeMap& single_impl,
    const TypeSet& intfs, const SingleImplConfig& config) {
  std::unique_ptr<AnalysisImpl> single_impls(
      new AnalysisImpl(scope, primary_dex));
  single_impls->create_single_impl(single_impl, intfs, config);
  single_impls->collect_field_defs();
  single_impls->collect_method_defs();
  single_impls->analyze_opcodes();
  single_impls->escape_not_in_primary();
  single_impls->remove_escaped();
  return std::move(single_impls);
}

void SingleImplAnalysis::escape_interface(DexType* intf, EscapeReason reason) {
  auto sit = single_impls.find(intf);
  if (sit == single_impls.end()) return;
  sit->second.escape |= reason;
  TRACE(INTF, 5, "(ESC) Escape %s => 0x%X\n", SHOW(intf), reason);
  const auto intf_cls = type_class(intf);
  if (intf_cls) {
    const auto super_intfs = intf_cls->get_interfaces();
    for (auto super_intf : super_intfs->get_type_list()) {
      escape_interface(super_intf, reason);
    }
  }
}

/**
 * Collect the interfaces to optimize for an optimization step.
 */
void SingleImplAnalysis::get_interfaces(TypeList& to_optimize) const {
  for (const auto& sit : single_impls) {
    auto& data = sit.second;
    assert(!data.is_escaped());
    if (data.children.empty()) {
      to_optimize.push_back(sit.first);
    }
  }
  // make the optimizable list stable. It's extremely useful for debugging
  // and also avoids to get different optimizations on different runs on the
  // same apk.
  // Sort by vtable size and name
  std::sort(to_optimize.begin(),
            to_optimize.end(),
            [](const DexType* type1, const DexType* type2) {
              auto size1 = type_class(type1)->get_vmethods().size();
              auto size2 = type_class(type2)->get_vmethods().size();
              return size1 == size2
                         ? strcmp(type1->get_name()->c_str(),
                                  type2->get_name()->c_str()) < 0
                         : size1 < size2;
            });
}
