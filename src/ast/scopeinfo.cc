// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>

#include "src/ast/scopes.h"
#include "src/bootstrapper.h"

namespace v8 {
namespace internal {


Handle<ScopeInfo> ScopeInfo::Create(Isolate* isolate, Zone* zone,
                                    Scope* scope) {
  // Collect variables.
  ZoneList<Variable*> stack_locals(scope->StackLocalCount(), zone);
  ZoneList<Variable*> context_locals(scope->ContextLocalCount(), zone);
  ZoneList<Variable*> context_globals(scope->ContextGlobalCount(), zone);
  scope->CollectVariables(&stack_locals, &context_locals, &context_globals);
  const int stack_local_count = stack_locals.length();
  const int context_local_count = context_locals.length();
  const int context_global_count = context_globals.length();
  // Make sure we allocate the correct amount.
  DCHECK_EQ(scope->ContextLocalCount(), context_local_count);
  DCHECK_EQ(scope->ContextGlobalCount(), context_global_count);

  // Determine use and location of the "this" binding if it is present.
  VariableAllocationInfo receiver_info;
  if (scope->is_declaration_scope() &&
      scope->AsDeclarationScope()->has_this_declaration()) {
    Variable* var = scope->AsDeclarationScope()->receiver();
    if (!var->is_used()) {
      receiver_info = UNUSED;
    } else if (var->IsContextSlot()) {
      receiver_info = CONTEXT;
    } else {
      DCHECK(var->IsParameter());
      receiver_info = STACK;
    }
  } else {
    receiver_info = NONE;
  }

  bool has_new_target =
      scope->is_declaration_scope() &&
      scope->AsDeclarationScope()->new_target_var() != nullptr;

  // Determine use and location of the function variable if it is present.
  VariableAllocationInfo function_name_info;
  VariableMode function_variable_mode;
  if (scope->is_function_scope() &&
      scope->AsDeclarationScope()->function_var() != nullptr) {
    Variable* var = scope->AsDeclarationScope()->function_var();
    if (!var->is_used()) {
      function_name_info = UNUSED;
    } else if (var->IsContextSlot()) {
      function_name_info = CONTEXT;
    } else {
      DCHECK(var->IsStackLocal());
      function_name_info = STACK;
    }
    function_variable_mode = var->mode();
  } else {
    function_name_info = NONE;
    function_variable_mode = VAR;
  }
  DCHECK(context_global_count == 0 || scope->scope_type() == SCRIPT_SCOPE);

  const bool has_function_name = function_name_info != NONE;
  const bool has_receiver = receiver_info == STACK || receiver_info == CONTEXT;
  const int parameter_count = scope->num_parameters();
  const int length = kVariablePartIndex + parameter_count +
                     (1 + stack_local_count) + 2 * context_local_count +
                     2 * context_global_count +
                     (has_receiver ? 1 : 0) + (has_function_name ? 2 : 0);

  Factory* factory = isolate->factory();
  Handle<ScopeInfo> scope_info = factory->NewScopeInfo(length);

  bool has_simple_parameters = false;
  bool asm_module = false;
  bool asm_function = false;
  FunctionKind function_kind = kNormalFunction;
  if (scope->is_function_scope()) {
    DeclarationScope* function_scope = scope->AsDeclarationScope();
    has_simple_parameters = function_scope->has_simple_parameters();
    asm_module = function_scope->asm_module();
    asm_function = function_scope->asm_function();
    function_kind = function_scope->function_kind();
  }

  // Encode the flags.
  int flags = ScopeTypeField::encode(scope->scope_type()) |
              CallsEvalField::encode(scope->calls_eval()) |
              LanguageModeField::encode(scope->language_mode()) |
              DeclarationScopeField::encode(scope->is_declaration_scope()) |
              ReceiverVariableField::encode(receiver_info) |
              HasNewTargetField::encode(has_new_target) |
              FunctionVariableField::encode(function_name_info) |
              FunctionVariableMode::encode(function_variable_mode) |
              AsmModuleField::encode(asm_module) |
              AsmFunctionField::encode(asm_function) |
              HasSimpleParametersField::encode(has_simple_parameters) |
              FunctionKindField::encode(function_kind);
  scope_info->SetFlags(flags);
  scope_info->SetParameterCount(parameter_count);
  scope_info->SetStackLocalCount(stack_local_count);
  scope_info->SetContextLocalCount(context_local_count);
  scope_info->SetContextGlobalCount(context_global_count);

  int index = kVariablePartIndex;
  // Add parameters.
  DCHECK_EQ(index, scope_info->ParameterEntriesIndex());
  if (scope->is_declaration_scope()) {
    for (int i = 0; i < parameter_count; ++i) {
      scope_info->set(index++,
                      *scope->AsDeclarationScope()->parameter(i)->name());
    }
  }

  // Add stack locals' names. We are assuming that the stack locals'
  // slots are allocated in increasing order, so we can simply add
  // them to the ScopeInfo object.
  int first_slot_index;
  if (stack_local_count > 0) {
    first_slot_index = stack_locals[0]->index();
  } else {
    first_slot_index = 0;
  }
  DCHECK_EQ(index, scope_info->StackLocalFirstSlotIndex());
  scope_info->set(index++, Smi::FromInt(first_slot_index));
  DCHECK_EQ(index, scope_info->StackLocalEntriesIndex());
  for (int i = 0; i < stack_local_count; ++i) {
    DCHECK(stack_locals[i]->index() == first_slot_index + i);
    scope_info->set(index++, *stack_locals[i]->name());
  }

  // Add context locals' names and info. Info lies beyond context globals'
  // names.
  // Make sure to store them in the order that they appear in the context.
  DCHECK_EQ(index, scope_info->ContextLocalNameEntriesIndex());
  int info_index = index + context_local_count + context_global_count;
  DCHECK_EQ(info_index, scope_info->ContextLocalInfoEntriesIndex());
  for (int i = 0; i < context_local_count; ++i) {
    Variable* var = context_locals[i];
    int context_index = var->index() - Context::MIN_CONTEXT_SLOTS;
    uint32_t info = VariableModeField::encode(var->mode()) |
                    InitFlagField::encode(var->initialization_flag()) |
                    MaybeAssignedFlagField::encode(var->maybe_assigned());
    scope_info->set(index + context_index, *var->name());
    scope_info->set(info_index + context_index, Smi::FromInt(info));
  }

  index += context_local_count;

  // Add context globals' names and info. Info lies beyond context locals' info.
  DCHECK_EQ(index, scope_info->ContextGlobalNameEntriesIndex());
  info_index = index + context_global_count + context_local_count;
  DCHECK_EQ(info_index, scope_info->ContextGlobalInfoEntriesIndex());
  for (int i = 0; i < context_global_count; ++i) {
    Variable* var = context_globals[i];
    scope_info->set(index + i, *var->name());
    // TODO(ishell): do we need this kind of info for globals here?
    uint32_t info = VariableModeField::encode(var->mode()) |
                    InitFlagField::encode(var->initialization_flag()) |
                    MaybeAssignedFlagField::encode(var->maybe_assigned());
    scope_info->set(info_index + i, Smi::FromInt(info));
  }

  index += context_local_count + 2 * context_global_count;

  // If the receiver is allocated, add its index.
  DCHECK(index == scope_info->ReceiverEntryIndex());
  if (has_receiver) {
    int var_index = scope->AsDeclarationScope()->receiver()->index();
    scope_info->set(index++, Smi::FromInt(var_index));
    // ?? DCHECK(receiver_info != CONTEXT || var_index ==
    // scope_info->ContextLength() - 1);
  }

  // If present, add the function variable name and its index.
  DCHECK(index == scope_info->FunctionNameEntryIndex());
  if (has_function_name) {
    int var_index = scope->AsDeclarationScope()->function_var()->index();
    scope_info->set(index++,
                    *scope->AsDeclarationScope()->function_var()->name());
    scope_info->set(index++, Smi::FromInt(var_index));
    DCHECK(function_name_info != CONTEXT ||
           var_index == scope_info->ContextLength() - 1);
  }

  DCHECK(index == scope_info->length());
  DCHECK(scope->num_parameters() == scope_info->ParameterCount());
  DCHECK(scope->num_heap_slots() == scope_info->ContextLength());
  return scope_info;
}


Handle<ScopeInfo> ScopeInfo::CreateGlobalThisBinding(Isolate* isolate) {
  DCHECK(isolate->bootstrapper()->IsActive());

  const int stack_local_count = 0;
  const int context_local_count = 1;
  const int context_global_count = 0;
  const bool has_simple_parameters = true;
  const VariableAllocationInfo receiver_info = CONTEXT;
  const VariableAllocationInfo function_name_info = NONE;
  const VariableMode function_variable_mode = VAR;
  const bool has_function_name = false;
  const bool has_receiver = true;
  const int parameter_count = 0;
  const int length = kVariablePartIndex + parameter_count +
                     (1 + stack_local_count) + 2 * context_local_count +
                     2 * context_global_count +
                     (has_receiver ? 1 : 0) + (has_function_name ? 2 : 0);

  Factory* factory = isolate->factory();
  Handle<ScopeInfo> scope_info = factory->NewScopeInfo(length);

  // Encode the flags.
  int flags = ScopeTypeField::encode(SCRIPT_SCOPE) |
              CallsEvalField::encode(false) |
              LanguageModeField::encode(SLOPPY) |
              DeclarationScopeField::encode(true) |
              ReceiverVariableField::encode(receiver_info) |
              FunctionVariableField::encode(function_name_info) |
              FunctionVariableMode::encode(function_variable_mode) |
              AsmModuleField::encode(false) | AsmFunctionField::encode(false) |
              HasSimpleParametersField::encode(has_simple_parameters) |
              FunctionKindField::encode(FunctionKind::kNormalFunction);
  scope_info->SetFlags(flags);
  scope_info->SetParameterCount(parameter_count);
  scope_info->SetStackLocalCount(stack_local_count);
  scope_info->SetContextLocalCount(context_local_count);
  scope_info->SetContextGlobalCount(context_global_count);

  int index = kVariablePartIndex;
  const int first_slot_index = 0;
  DCHECK(index == scope_info->StackLocalFirstSlotIndex());
  scope_info->set(index++, Smi::FromInt(first_slot_index));
  DCHECK(index == scope_info->StackLocalEntriesIndex());

  // Here we add info for context-allocated "this".
  DCHECK(index == scope_info->ContextLocalNameEntriesIndex());
  scope_info->set(index++, *isolate->factory()->this_string());
  DCHECK(index == scope_info->ContextLocalInfoEntriesIndex());
  const uint32_t value = VariableModeField::encode(CONST) |
                         InitFlagField::encode(kCreatedInitialized) |
                         MaybeAssignedFlagField::encode(kNotAssigned);
  scope_info->set(index++, Smi::FromInt(value));

  // And here we record that this scopeinfo binds a receiver.
  DCHECK(index == scope_info->ReceiverEntryIndex());
  const int receiver_index = Context::MIN_CONTEXT_SLOTS + 0;
  scope_info->set(index++, Smi::FromInt(receiver_index));

  DCHECK(index == scope_info->FunctionNameEntryIndex());

  DCHECK_EQ(index, scope_info->length());
  DCHECK_EQ(scope_info->ParameterCount(), 0);
  DCHECK_EQ(scope_info->ContextLength(), Context::MIN_CONTEXT_SLOTS + 1);

  return scope_info;
}


ScopeInfo* ScopeInfo::Empty(Isolate* isolate) {
  return reinterpret_cast<ScopeInfo*>(isolate->heap()->empty_fixed_array());
}


ScopeType ScopeInfo::scope_type() {
  DCHECK(length() > 0);
  return ScopeTypeField::decode(Flags());
}


bool ScopeInfo::CallsEval() {
  return length() > 0 && CallsEvalField::decode(Flags());
}


LanguageMode ScopeInfo::language_mode() {
  return length() > 0 ? LanguageModeField::decode(Flags()) : SLOPPY;
}


bool ScopeInfo::is_declaration_scope() {
  return DeclarationScopeField::decode(Flags());
}


int ScopeInfo::LocalCount() {
  return StackLocalCount() + ContextLocalCount();
}


int ScopeInfo::StackSlotCount() {
  if (length() > 0) {
    bool function_name_stack_slot =
        FunctionVariableField::decode(Flags()) == STACK;
    return StackLocalCount() + (function_name_stack_slot ? 1 : 0);
  }
  return 0;
}


int ScopeInfo::ContextLength() {
  if (length() > 0) {
    int context_locals = ContextLocalCount();
    int context_globals = ContextGlobalCount();
    bool function_name_context_slot =
        FunctionVariableField::decode(Flags()) == CONTEXT;
    bool has_context = context_locals > 0 || context_globals > 0 ||
                       function_name_context_slot ||
                       scope_type() == WITH_SCOPE ||
                       (scope_type() == BLOCK_SCOPE && CallsSloppyEval() &&
                           is_declaration_scope()) ||
                       (scope_type() == FUNCTION_SCOPE && CallsSloppyEval()) ||
                       scope_type() == MODULE_SCOPE;

    if (has_context) {
      return Context::MIN_CONTEXT_SLOTS + context_locals + context_globals +
             (function_name_context_slot ? 1 : 0);
    }
  }
  return 0;
}


bool ScopeInfo::HasReceiver() {
  if (length() > 0) {
    return NONE != ReceiverVariableField::decode(Flags());
  } else {
    return false;
  }
}


bool ScopeInfo::HasAllocatedReceiver() {
  if (length() > 0) {
    VariableAllocationInfo allocation = ReceiverVariableField::decode(Flags());
    return allocation == STACK || allocation == CONTEXT;
  } else {
    return false;
  }
}


bool ScopeInfo::HasNewTarget() { return HasNewTargetField::decode(Flags()); }


bool ScopeInfo::HasFunctionName() {
  if (length() > 0) {
    return NONE != FunctionVariableField::decode(Flags());
  } else {
    return false;
  }
}


bool ScopeInfo::HasHeapAllocatedLocals() {
  if (length() > 0) {
    return ContextLocalCount() > 0;
  } else {
    return false;
  }
}


bool ScopeInfo::HasContext() {
  return ContextLength() > 0;
}


String* ScopeInfo::FunctionName() {
  DCHECK(HasFunctionName());
  return String::cast(get(FunctionNameEntryIndex()));
}


String* ScopeInfo::ParameterName(int var) {
  DCHECK(0 <= var && var < ParameterCount());
  int info_index = ParameterEntriesIndex() + var;
  return String::cast(get(info_index));
}


String* ScopeInfo::LocalName(int var) {
  DCHECK(0 <= var && var < LocalCount());
  DCHECK(StackLocalEntriesIndex() + StackLocalCount() ==
         ContextLocalNameEntriesIndex());
  int info_index = StackLocalEntriesIndex() + var;
  return String::cast(get(info_index));
}


String* ScopeInfo::StackLocalName(int var) {
  DCHECK(0 <= var && var < StackLocalCount());
  int info_index = StackLocalEntriesIndex() + var;
  return String::cast(get(info_index));
}


int ScopeInfo::StackLocalIndex(int var) {
  DCHECK(0 <= var && var < StackLocalCount());
  int first_slot_index = Smi::cast(get(StackLocalFirstSlotIndex()))->value();
  return first_slot_index + var;
}


String* ScopeInfo::ContextLocalName(int var) {
  DCHECK(0 <= var && var < ContextLocalCount() + ContextGlobalCount());
  int info_index = ContextLocalNameEntriesIndex() + var;
  return String::cast(get(info_index));
}


VariableMode ScopeInfo::ContextLocalMode(int var) {
  DCHECK(0 <= var && var < ContextLocalCount() + ContextGlobalCount());
  int info_index = ContextLocalInfoEntriesIndex() + var;
  int value = Smi::cast(get(info_index))->value();
  return VariableModeField::decode(value);
}


InitializationFlag ScopeInfo::ContextLocalInitFlag(int var) {
  DCHECK(0 <= var && var < ContextLocalCount() + ContextGlobalCount());
  int info_index = ContextLocalInfoEntriesIndex() + var;
  int value = Smi::cast(get(info_index))->value();
  return InitFlagField::decode(value);
}


MaybeAssignedFlag ScopeInfo::ContextLocalMaybeAssignedFlag(int var) {
  DCHECK(0 <= var && var < ContextLocalCount() + ContextGlobalCount());
  int info_index = ContextLocalInfoEntriesIndex() + var;
  int value = Smi::cast(get(info_index))->value();
  return MaybeAssignedFlagField::decode(value);
}

bool ScopeInfo::VariableIsSynthetic(String* name) {
  // There's currently no flag stored on the ScopeInfo to indicate that a
  // variable is a compiler-introduced temporary. However, to avoid conflict
  // with user declarations, the current temporaries like .generator_object and
  // .result start with a dot, so we can use that as a flag. It's a hack!
  return name->length() == 0 || name->Get(0) == '.' ||
         name->Equals(name->GetHeap()->this_string());
}


int ScopeInfo::StackSlotIndex(String* name) {
  DCHECK(name->IsInternalizedString());
  if (length() > 0) {
    int first_slot_index = Smi::cast(get(StackLocalFirstSlotIndex()))->value();
    int start = StackLocalEntriesIndex();
    int end = start + StackLocalCount();
    for (int i = start; i < end; ++i) {
      if (name == get(i)) {
        return i - start + first_slot_index;
      }
    }
  }
  return -1;
}


int ScopeInfo::ContextSlotIndex(Handle<ScopeInfo> scope_info,
                                Handle<String> name, VariableMode* mode,
                                InitializationFlag* init_flag,
                                MaybeAssignedFlag* maybe_assigned_flag) {
  DCHECK(name->IsInternalizedString());
  DCHECK_NOT_NULL(mode);
  DCHECK_NOT_NULL(init_flag);
  DCHECK_NOT_NULL(maybe_assigned_flag);

  if (scope_info->length() > 0) {
    int start = scope_info->ContextLocalNameEntriesIndex();
    int end = start + scope_info->ContextLocalCount();
    for (int i = start; i < end; ++i) {
      if (*name == scope_info->get(i)) {
        int var = i - start;
        *mode = scope_info->ContextLocalMode(var);
        *init_flag = scope_info->ContextLocalInitFlag(var);
        *maybe_assigned_flag = scope_info->ContextLocalMaybeAssignedFlag(var);
        int result = Context::MIN_CONTEXT_SLOTS + var;
        DCHECK(result < scope_info->ContextLength());
        return result;
      }
    }
  }

  return -1;
}

int ScopeInfo::ContextGlobalSlotIndex(Handle<ScopeInfo> scope_info,
                                      Handle<String> name, VariableMode* mode,
                                      InitializationFlag* init_flag,
                                      MaybeAssignedFlag* maybe_assigned_flag) {
  DCHECK(name->IsInternalizedString());
  DCHECK_NOT_NULL(mode);
  DCHECK_NOT_NULL(init_flag);
  DCHECK_NOT_NULL(maybe_assigned_flag);
  if (scope_info->length() > 0) {
    // This is to ensure that ContextLocalMode() and co. queries would work.
    DCHECK_EQ(scope_info->ContextGlobalNameEntriesIndex(),
              scope_info->ContextLocalNameEntriesIndex() +
                  scope_info->ContextLocalCount());
    int base = scope_info->ContextLocalNameEntriesIndex();
    int start = scope_info->ContextGlobalNameEntriesIndex();
    int end = start + scope_info->ContextGlobalCount();
    for (int i = start; i < end; ++i) {
      if (*name == scope_info->get(i)) {
        int var = i - base;
        *mode = scope_info->ContextLocalMode(var);
        *init_flag = scope_info->ContextLocalInitFlag(var);
        *maybe_assigned_flag = scope_info->ContextLocalMaybeAssignedFlag(var);
        int result = Context::MIN_CONTEXT_SLOTS + var;
        DCHECK(result < scope_info->ContextLength());
        return result;
      }
    }
  }
  return -1;
}


String* ScopeInfo::ContextSlotName(int slot_index) {
  int const var = slot_index - Context::MIN_CONTEXT_SLOTS;
  DCHECK_LE(0, var);
  DCHECK_LT(var, ContextLocalCount() + ContextGlobalCount());
  return ContextLocalName(var);
}


int ScopeInfo::ParameterIndex(String* name) {
  DCHECK(name->IsInternalizedString());
  if (length() > 0) {
    // We must read parameters from the end since for
    // multiply declared parameters the value of the
    // last declaration of that parameter is used
    // inside a function (and thus we need to look
    // at the last index). Was bug# 1110337.
    int start = ParameterEntriesIndex();
    int end = start + ParameterCount();
    for (int i = end - 1; i >= start; --i) {
      if (name == get(i)) {
        return i - start;
      }
    }
  }
  return -1;
}


int ScopeInfo::ReceiverContextSlotIndex() {
  if (length() > 0 && ReceiverVariableField::decode(Flags()) == CONTEXT)
    return Smi::cast(get(ReceiverEntryIndex()))->value();
  return -1;
}


int ScopeInfo::FunctionContextSlotIndex(String* name, VariableMode* mode) {
  DCHECK(name->IsInternalizedString());
  DCHECK(mode != NULL);
  if (length() > 0) {
    if (FunctionVariableField::decode(Flags()) == CONTEXT &&
        FunctionName() == name) {
      *mode = FunctionVariableMode::decode(Flags());
      return Smi::cast(get(FunctionNameEntryIndex() + 1))->value();
    }
  }
  return -1;
}


FunctionKind ScopeInfo::function_kind() {
  return FunctionKindField::decode(Flags());
}


int ScopeInfo::ParameterEntriesIndex() {
  DCHECK(length() > 0);
  return kVariablePartIndex;
}


int ScopeInfo::StackLocalFirstSlotIndex() {
  return ParameterEntriesIndex() + ParameterCount();
}


int ScopeInfo::StackLocalEntriesIndex() {
  return StackLocalFirstSlotIndex() + 1;
}


int ScopeInfo::ContextLocalNameEntriesIndex() {
  return StackLocalEntriesIndex() + StackLocalCount();
}


int ScopeInfo::ContextGlobalNameEntriesIndex() {
  return ContextLocalNameEntriesIndex() + ContextLocalCount();
}


int ScopeInfo::ContextLocalInfoEntriesIndex() {
  return ContextGlobalNameEntriesIndex() + ContextGlobalCount();
}


int ScopeInfo::ContextGlobalInfoEntriesIndex() {
  return ContextLocalInfoEntriesIndex() + ContextLocalCount();
}


int ScopeInfo::ReceiverEntryIndex() {
  return ContextGlobalInfoEntriesIndex() + ContextGlobalCount();
}


int ScopeInfo::FunctionNameEntryIndex() {
  return ReceiverEntryIndex() + (HasAllocatedReceiver() ? 1 : 0);
}

#ifdef DEBUG

static void PrintList(const char* list_name,
                      int nof_internal_slots,
                      int start,
                      int end,
                      ScopeInfo* scope_info) {
  if (start < end) {
    PrintF("\n  // %s\n", list_name);
    if (nof_internal_slots > 0) {
      PrintF("  %2d - %2d [internal slots]\n", 0 , nof_internal_slots - 1);
    }
    for (int i = nof_internal_slots; start < end; ++i, ++start) {
      PrintF("  %2d ", i);
      String::cast(scope_info->get(start))->ShortPrint();
      PrintF("\n");
    }
  }
}


void ScopeInfo::Print() {
  PrintF("ScopeInfo ");
  if (HasFunctionName()) {
    FunctionName()->ShortPrint();
  } else {
    PrintF("/* no function name */");
  }
  PrintF("{");

  if (length() > 0) {
    PrintList("parameters", 0, ParameterEntriesIndex(),
              ParameterEntriesIndex() + ParameterCount(), this);
    PrintList("stack slots", 0, StackLocalEntriesIndex(),
              StackLocalEntriesIndex() + StackLocalCount(), this);
    PrintList("context slots", Context::MIN_CONTEXT_SLOTS,
              ContextLocalNameEntriesIndex(),
              ContextLocalNameEntriesIndex() + ContextLocalCount(), this);
  }

  PrintF("}\n");
}
#endif  // DEBUG


}  // namespace internal
}  // namespace v8
