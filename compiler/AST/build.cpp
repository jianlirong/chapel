#include "build.h"
#include "astutil.h"
#include "baseAST.h"
#include "expr.h"
#include "parser.h"
#include "stmt.h"
#include "stringutil.h"
#include "symbol.h"
#include "type.h"


static void
checkControlFlow(Expr* expr, const char* context) {
  Vec<const char*> labelSet; // all labels in expr argument
  Vec<BaseAST*> loopSet;     // all asts in a loop in expr argument
  Vec<BaseAST*> innerFnSet;  // all asts in a function in expr argument
  Vec<BaseAST*> asts;
  collect_asts(expr, asts);

  //
  // compute labelSet and loopSet
  //
  forv_Vec(BaseAST, ast, asts) {
    if (DefExpr* def = toDefExpr(ast)) {
      if (LabelSymbol* ls = toLabelSymbol(def->sym))
        labelSet.set_add(ls->name);
      else if (FnSymbol* fn = toFnSymbol(def->sym)) {
        if (!innerFnSet.set_in(fn)) {
          Vec<BaseAST*> innerAsts;
          collect_asts(fn, innerAsts);
          forv_Vec(BaseAST, ast, innerAsts) {
            innerFnSet.set_add(ast);
          }
        }
      }
    } else if (BlockStmt* block = toBlockStmt(ast)) {
      if (block->isLoop() && !loopSet.set_in(block)) {
        if (block->userLabel != NULL) {
          labelSet.set_add(block->userLabel);
        }
        Vec<BaseAST*> loopAsts;
        collect_asts(block, loopAsts);
        forv_Vec(BaseAST, ast, loopAsts) {
          loopSet.set_add(ast);
        }
      }
    }
  }

  //
  // check for illegal control flow
  //
  forv_Vec(BaseAST, ast, asts) {
    if (CallExpr* call = toCallExpr(ast)) {
      if (innerFnSet.set_in(call))
        continue; // yield or return is in nested function/iterator
      if (call->isPrimitive(PRIM_RETURN)) {
        USR_FATAL_CONT(call, "return is not allowed in %s", context);
      } else if (call->isPrimitive(PRIM_YIELD)) {
        if (!strcmp(context, "begin statement") ||
            !strcmp(context, "yield statement"))
          USR_FATAL_CONT(call, "yield is not allowed in %s", context);
      }
    } else if (GotoStmt* gs = toGotoStmt(ast)) {
      if (labelSet.set_in(gs->getName()))
        continue; // break or continue target is in scope
      if (toSymExpr(gs->label) && toSymExpr(gs->label)->var == gNil && loopSet.set_in(gs))
        continue; // break or continue loop is in scope
      if (!strcmp(context, "on statement")) {
        USR_PRINT(gs, "the following error is a current limitation");
      }
      if (gs->gotoTag == GOTO_BREAK) {
        USR_FATAL_CONT(gs, "break is not allowed in %s", context);
      } else if (gs->gotoTag == GOTO_CONTINUE) {
        USR_FATAL_CONT(gs, "continue is not allowed in %s", context);
      } else {
        USR_FATAL_CONT(gs, "illegal 'goto' usage; goto is deprecated anyway");
      }
    }
  }
}


BlockStmt* buildPragmaStmt(BlockStmt* block,
                           Vec<const char*>* pragmas,
                           BlockStmt* stmt) {
  if (DefExpr* def = toDefExpr(stmt->body.first()))
    def->sym->addFlags(pragmas);
  delete pragmas;
  block->insertAtTail(stmt);
  return block;
}


Expr* buildParenExpr(CallExpr* call) {
  if (call->numActuals() == 1)
    return call->get(1)->remove();
  else
    return new CallExpr("_build_tuple", call);
}


Expr* buildSquareCallExpr(Expr* base, CallExpr* args) {
  CallExpr* call = new CallExpr(base, args);
  call->square = true;
  return call;
}


Expr* buildNamedActual(const char* name, Expr* expr) {
  return new NamedExpr(name, expr);
}


Expr* buildNamedAliasActual(const char* name, Expr* expr) {
  return new CallExpr(PRIM_ACTUALS_LIST,
           new NamedExpr(name, expr),
           new NamedExpr(astr("chpl__aliasField_", name), new SymExpr(gTrue)));
}


Expr* buildFormalArrayType(Expr* iterator, Expr* eltType, Expr* index) {
  if (index) {
    CallExpr* indexCall = toCallExpr(index);
    INT_ASSERT(indexCall);
    if (indexCall->numActuals() != 1)
      USR_FATAL(iterator, "invalid index expression");
    return new CallExpr("chpl__buildArrayRuntimeType",
             new CallExpr("chpl__buildDomainExpr", iterator),
             eltType, indexCall->get(1)->remove(),
             new CallExpr("chpl__buildDomainExpr", iterator->copy()));
  } else {
    CallExpr* call = toCallExpr(iterator);
    if (call->numActuals() == 1 && isDefExpr(call->get(1))) {
      return new CallExpr("chpl__buildArrayRuntimeType", call->get(1)->remove(), eltType);
    } else
      return new CallExpr("chpl__buildArrayRuntimeType",
               new CallExpr("chpl__buildDomainExpr", iterator), eltType);
  }
}

Expr* buildIntLiteral(const char* pch) {
  uint64_t ull;
  if (!strncmp("0b", pch, 2))
    ull = binStr2uint64(pch);
  else if (!strncmp("0x", pch, 2))
    ull = hexStr2uint64(pch);
  else
    ull = str2uint64(pch);
  if (ull <= 2147483647ull)
    return new SymExpr(new_IntSymbol(ull, INT_SIZE_32));
  else if (ull <= 9223372036854775807ull)
    return new SymExpr(new_IntSymbol(ull, INT_SIZE_64));
  else
    return new SymExpr(new_UIntSymbol(ull, INT_SIZE_64));
}


Expr* buildRealLiteral(const char* pch) {
  return new SymExpr(new_RealSymbol(pch, strtod(pch, NULL)));
}


Expr* buildImagLiteral(const char* pch) {
  char* str = strdup(pch);
  str[strlen(pch)-1] = '\0';
  SymExpr* se = new SymExpr(new_ImagSymbol(str, strtod(str, NULL)));
  free(str);
  return se;
}


Expr* buildStringLiteral(const char* pch) {
  return new SymExpr(new_StringSymbol(pch));
}


Expr* buildDotExpr(BaseAST* base, const char* member) {
  if (!strcmp("uid", member))
    if (CallExpr* intToLocale = toCallExpr(base))
      if (intToLocale->isNamed("chpl_int_to_locale"))
        if (CallExpr* getLocale = toCallExpr(intToLocale->get(1)))
          if (getLocale->isPrimitive(PRIM_GET_LOCALEID))
            return getLocale->remove();
  if (!strcmp("locale", member))
    return new CallExpr("chpl_int_to_locale", 
                        new CallExpr(PRIM_GET_LOCALEID, base));
  else
    return new CallExpr(".", base, new_StringSymbol(member));
}


Expr* buildDotExpr(const char* base, const char* member) {
  return buildDotExpr(new UnresolvedSymExpr(base), member);
}


Expr* buildLogicalAndExpr(BaseAST* left, BaseAST* right) {
  VarSymbol* lvar = newTemp();
  lvar->addFlag(FLAG_MAYBE_PARAM);
  FnSymbol* ifFn = buildIfExpr(new CallExpr("isTrue", lvar),
                                 new CallExpr("isTrue", right),
                                 new SymExpr(gFalse));
  ifFn->insertAtHead(new CondStmt(new CallExpr("_cond_invalid", lvar), new CallExpr("compilerError", new_StringSymbol("cannot promote short-circuiting && operator"))));
  ifFn->insertAtHead(new CallExpr(PRIM_MOVE, lvar, left));
  ifFn->insertAtHead(new DefExpr(lvar));
  return new CallExpr(new DefExpr(ifFn));
}


Expr* buildLogicalOrExpr(BaseAST* left, BaseAST* right) {
  VarSymbol* lvar = newTemp();
  lvar->addFlag(FLAG_MAYBE_PARAM);
  FnSymbol* ifFn = buildIfExpr(new CallExpr("isTrue", lvar),
                                 new SymExpr(gTrue),
                                 new CallExpr("isTrue", right));
  ifFn->insertAtHead(new CondStmt(new CallExpr("_cond_invalid", lvar), new CallExpr("compilerError", new_StringSymbol("cannot promote short-circuiting || operator"))));
  ifFn->insertAtHead(new CallExpr(PRIM_MOVE, lvar, left));
  ifFn->insertAtHead(new DefExpr(lvar));
  return new CallExpr(new DefExpr(ifFn));
}


BlockStmt* buildChapelStmt(BaseAST* ast) {
  BlockStmt* block = NULL;
  if (!ast)
    block = new BlockStmt();
  else if (Expr* a = toExpr(ast))
    block = new BlockStmt(a);
  else
    INT_FATAL(ast, "Illegal argument to buildChapelStmt");
  block->blockTag = BLOCK_SCOPELESS;
  return block;
}


static void addModuleToSearchList(CallExpr* newUse, BaseAST* module) {
  UnresolvedSymExpr* modNameExpr = toUnresolvedSymExpr(module);
  if (modNameExpr) {
    addModuleToParseList(modNameExpr->unresolved, newUse);
  } else if (CallExpr* callExpr = toCallExpr(module)) {
    addModuleToSearchList(newUse, callExpr->argList.first());
  }
}


static BlockStmt* buildUseList(BaseAST* module, BlockStmt* list) {
  CallExpr* newUse = new CallExpr(PRIM_USE, module);
  addModuleToSearchList(newUse, module);
  if (list == NULL) {
    return buildChapelStmt(newUse);
  } else {
    list->insertAtTail(newUse);
    return list;
  }
}


BlockStmt* buildUseStmt(CallExpr* modules) {
  BlockStmt* list = NULL;
  for_actuals(expr, modules)
    list = buildUseList(expr->remove(), list);
  return list;
}


static void
buildTupleVarDeclHelp(Expr* base, BlockStmt* decls, Expr* insertPoint) {
  int count = 1;
  for_alist(expr, decls->body) {
    if (DefExpr* def = toDefExpr(expr)) {
      if (strcmp(def->sym->name, "chpl__tuple_blank")) {
        def->init = new CallExpr(base->copy(), new_IntSymbol(count));
        insertPoint->insertBefore(def->remove());
      } else {
        def->remove(); // unexecuted none/gasnet on 4/25/08
      }
    } else if (BlockStmt* blk = toBlockStmt(expr)) {
      buildTupleVarDeclHelp(new CallExpr(base, new_IntSymbol(count)),
                            blk, insertPoint);
    } else {
      INT_FATAL(expr, "unexpected expression in buildTupleVarDeclHelp");
    }
    count++;
  }
  decls->remove();
}


BlockStmt*
buildTupleVarDeclStmt(BlockStmt* tupleBlock, Expr* type, Expr* init) {
  VarSymbol* tmp = newTemp();
  int count = 1;
  for_alist(expr, tupleBlock->body) {
    if (DefExpr* def = toDefExpr(expr)) {
      if (strcmp(def->sym->name, "chpl__tuple_blank")) {
        def->init = new CallExpr(tmp, new_IntSymbol(count));
      } else {
        def->remove();
      }
    } else if (BlockStmt* blk = toBlockStmt(expr)) {
      buildTupleVarDeclHelp(new CallExpr(tmp, new_IntSymbol(count)), blk, expr);
    }
    count++;
  }
  tupleBlock->insertAtHead(new DefExpr(tmp, init, type));
  return tupleBlock;
}


BlockStmt*
buildLabelStmt(const char* name, Expr* stmt) {
  BlockStmt* block = toBlockStmt(stmt);
  if (block) {
    Expr* breakLabelStmt = block->body.tail;
    if (!isDefExpr(breakLabelStmt) && isDefExpr(breakLabelStmt->prev)) {
      // the last statement in the block could be a call to _freeIterator()
      breakLabelStmt = breakLabelStmt->prev;
    }
    BlockStmt* loop = toBlockStmt(breakLabelStmt->prev);
    if (loop && loop->isLoop() &&
         (loop->blockInfo->isPrimitive(PRIM_BLOCK_FOR_LOOP)     ||
          loop->blockInfo->isPrimitive(PRIM_BLOCK_WHILEDO_LOOP) ||
          loop->blockInfo->isPrimitive(PRIM_BLOCK_DOWHILE_LOOP))) {
      if (!loop->breakLabel || !loop->continueLabel) {
        USR_FATAL(stmt, "cannot label parallel loop");
      } else {
        loop->userLabel = astr(name);
      }
    } else {
      USR_FATAL(stmt, "cannot label non-loop statement");
    }
  } else {
    USR_FATAL(stmt, "cannot label non-loop statement");
  }
  return block;
}


BlockStmt*
buildIfStmt(Expr* condExpr, Expr* thenExpr, Expr* elseExpr) {
  return buildChapelStmt(new CondStmt(new CallExpr("_cond_test", condExpr), thenExpr, elseExpr));
}


void createInitFn(ModuleSymbol* mod) {
  SET_LINENO(mod);

  mod->initFn = new FnSymbol(astr("chpl__init_", mod->name));
  mod->initFn->retType = dtVoid;

  //
  // move module-level statements into module's init function
  //
  if (mod != theProgram) {
    for_alist(stmt, mod->block->body) {

      //
      // except for module definitions
      //
      if (BlockStmt* block = toBlockStmt(stmt))
        if (block->length() == 1)
          if (DefExpr* def = toDefExpr(block->body.only()))
            if (isModuleSymbol(def->sym))
              continue;

      mod->initFn->insertAtTail(stmt->remove());
    }
  }
  mod->block->insertAtHead(new DefExpr(mod->initFn));
}


ModuleSymbol* buildModule(const char* name, BlockStmt* block, const char* filename) {
  ModuleSymbol* mod = new ModuleSymbol(name, currentModuleType, block);
  mod->filename = astr(filename);
  createInitFn(mod);
  return mod;
}


CallExpr* buildPrimitiveExpr(CallExpr* exprs) {
  INT_ASSERT(exprs->isPrimitive(PRIM_ACTUALS_LIST));
  if (exprs->argList.length == 0)
    INT_FATAL("primitive has no name");
  Expr* expr = exprs->get(1);
  expr->remove();
  SymExpr* symExpr = toSymExpr(expr);
  if (!symExpr)
    INT_FATAL(expr, "primitive has no name");
  VarSymbol* var = toVarSymbol(symExpr->var);
  if (!var || !var->immediate || var->immediate->const_kind != CONST_KIND_STRING)
    INT_FATAL(expr, "primitive with non-literal string name");
  PrimitiveOp* prim = primitives_map.get(var->immediate->v_string);
  if (!prim)
    INT_FATAL(expr, "primitive not found '%s'", var->immediate->v_string);
  return new CallExpr(prim, exprs);
}


FnSymbol* buildIfExpr(Expr* e, Expr* e1, Expr* e2) {
  static int uid = 1;

  if (!e2)
    USR_FATAL("if-then expressions currently require an else-clause");

  FnSymbol* ifFn = new FnSymbol(astr("_if_fn", istr(uid++)));
  ifFn->addFlag(FLAG_INLINE);
  VarSymbol* tmp1 = newTemp();
  VarSymbol* tmp2 = newTemp();
  tmp1->addFlag(FLAG_MAYBE_PARAM);
  tmp2->addFlag(FLAG_MAYBE_TYPE);

  ifFn->addFlag(FLAG_MAYBE_PARAM);
  ifFn->addFlag(FLAG_MAYBE_TYPE);
  ifFn->insertAtHead(new DefExpr(tmp1));
  ifFn->insertAtHead(new DefExpr(tmp2));
  ifFn->insertAtTail(new CallExpr(PRIM_MOVE, new SymExpr(tmp1), new CallExpr("_cond_test", e)));
  ifFn->insertAtTail(new CondStmt(
    new SymExpr(tmp1),
    new CallExpr(PRIM_MOVE,
                 new SymExpr(tmp2),
                 new CallExpr(PRIM_LOGICAL_FOLDER,
                              new SymExpr(tmp1),
                              new CallExpr(PRIM_GET_REF, e1))),
    new CallExpr(PRIM_MOVE,
                 new SymExpr(tmp2),
                 new CallExpr(PRIM_LOGICAL_FOLDER,
                              new SymExpr(tmp1),
                              new CallExpr(PRIM_GET_REF, e2)))));
  ifFn->insertAtTail(new CallExpr(PRIM_RETURN, tmp2));
  return ifFn;
}


CallExpr* buildLetExpr(BlockStmt* decls, Expr* expr) {
  static int uid = 1;
  FnSymbol* fn = new FnSymbol(astr("_let_fn", istr(uid++)));
  fn->addFlag(FLAG_INLINE);
  fn->insertAtTail(decls);
  fn->insertAtTail(new CallExpr(PRIM_RETURN, expr));
  return new CallExpr(new DefExpr(fn));
}


BlockStmt* buildWhileDoLoopStmt(Expr* cond, BlockStmt* body) {
  cond = new CallExpr("_cond_test", cond);
  VarSymbol* condVar = newTemp();
  body = new BlockStmt(body);
  body->blockInfo = new CallExpr(PRIM_BLOCK_WHILEDO_LOOP, condVar);
  LabelSymbol* continueLabel = new LabelSymbol("_continueLabel");
  continueLabel->addFlag(FLAG_TEMP);
  continueLabel->addFlag(FLAG_LABEL_CONTINUE);
  body->continueLabel = continueLabel;
  LabelSymbol* breakLabel = new LabelSymbol("_breakLabel");
  breakLabel->addFlag(FLAG_TEMP);
  breakLabel->addFlag(FLAG_LABEL_BREAK);
  body->breakLabel = breakLabel;
  body->insertAtTail(new DefExpr(continueLabel));
  body->insertAtTail(new CallExpr(PRIM_MOVE, condVar, cond->copy()));
  BlockStmt* stmts = buildChapelStmt();
  stmts->insertAtTail(new DefExpr(condVar));
  stmts->insertAtTail(new CallExpr(PRIM_MOVE, condVar, cond->copy()));
  stmts->insertAtTail(body);
  stmts->insertAtTail(new DefExpr(breakLabel));
  return stmts;
}


BlockStmt* buildDoWhileLoopStmt(Expr* cond, BlockStmt* body) {
  cond = new CallExpr("_cond_test", cond);
  VarSymbol* condVar = newTemp();

  // make variables declared in the scope of the body visible to
  // expressions in the condition of a do..while block
  if (body->length() == 1 && toBlockStmt(body->body.only())) {
    body = toBlockStmt(body->body.only());
    body->remove();
  }

  LabelSymbol* continueLabel = new LabelSymbol("_continueLabel");
  continueLabel->addFlag(FLAG_TEMP);
  continueLabel->addFlag(FLAG_LABEL_CONTINUE);
  LabelSymbol* breakLabel = new LabelSymbol("_breakLabel");
  breakLabel->addFlag(FLAG_TEMP);
  breakLabel->addFlag(FLAG_LABEL_BREAK);
  BlockStmt* block = new BlockStmt(body);
  block->continueLabel = continueLabel;
  block->breakLabel = breakLabel;
  block->blockInfo = new CallExpr(PRIM_BLOCK_DOWHILE_LOOP, condVar);
  BlockStmt* stmts = buildChapelStmt();
  stmts->insertAtTail(new DefExpr(condVar));
  stmts->insertAtTail(block);
  body->insertAtTail(new DefExpr(continueLabel));
  body->insertAtTail(new CallExpr(PRIM_MOVE, condVar, cond->copy()));
  stmts->insertAtTail(new DefExpr(breakLabel));
  return stmts;
}


BlockStmt* buildSerialStmt(Expr* cond, BlockStmt* body) {
  cond = new CallExpr("_cond_test", cond);
  if (fSerial) {
    body->insertAtHead(cond);
    return body;
  } else {
    BlockStmt *sbody = new BlockStmt();
    VarSymbol *serial_state = newTemp();
    sbody->insertAtTail(new DefExpr(serial_state, new CallExpr(PRIM_GET_SERIAL)));
    sbody->insertAtTail(new CondStmt(cond, new CallExpr(PRIM_SET_SERIAL, gTrue)));
    sbody->insertAtTail(body);
    sbody->insertAtTail(new CallExpr(PRIM_SET_SERIAL, serial_state));
    return sbody;
  }
}


//
// check validity of indices in loops and expressions
//
static void
checkIndices(BaseAST* indices) {
  if (CallExpr* call = toCallExpr(indices)) {
    if (!call->isNamed("_build_tuple"))
      USR_FATAL(indices, "invalid index expression");
    for_actuals(actual, call)
      checkIndices(actual);
  } else if (!isSymExpr(indices) && !isUnresolvedSymExpr(indices))
    USR_FATAL(indices, "invalid index expression");
}


static void
destructureIndices(BlockStmt* block,
                   BaseAST* indices,
                   Expr* init,
                   bool coforall) {
  if (CallExpr* call = toCallExpr(indices)) {
    if (call->isNamed("_build_tuple")) {
      int i = 1;
      for_actuals(actual, call) {
        if (UnresolvedSymExpr* use = toUnresolvedSymExpr(actual)) {
          if (!strcmp(use->unresolved, "chpl__tuple_blank")) {
            i++;
            continue;
          }
        }
        destructureIndices(block, actual,
                           new CallExpr(init->copy(), new_IntSymbol(i)),
                           coforall);
        i++;
      }
    }
  } else if (UnresolvedSymExpr* sym = toUnresolvedSymExpr(indices)) {
    VarSymbol* var = new VarSymbol(sym->unresolved);
    block->insertAtHead(new CallExpr(PRIM_MOVE, var, init));
    block->insertAtHead(new DefExpr(var));
    var->addFlag(FLAG_INDEX_VAR);
    if (coforall)
      var->addFlag(FLAG_HEAP_ALLOCATE);
    var->addFlag(FLAG_INSERT_AUTO_DESTROY);
  } else if (SymExpr* sym = toSymExpr(indices)) {
    block->insertAtHead(new CallExpr(PRIM_MOVE, sym->var, init));
    sym->var->addFlag(FLAG_INDEX_VAR);
    if (coforall)
      sym->var->addFlag(FLAG_HEAP_ALLOCATE);
    sym->var->addFlag(FLAG_INSERT_AUTO_DESTROY);
  }
}


static BlockStmt*
handleArrayTypeCase(FnSymbol* fn, Expr* indices, Expr* iteratorExpr, Expr* expr) {
  BlockStmt* block = new BlockStmt();
  fn->addFlag(FLAG_MAYBE_TYPE);
  bool hasSpecifiedIndices = !!indices;
  if (!hasSpecifiedIndices)
    indices = new UnresolvedSymExpr("_elided_index");
  checkIndices(indices);

  //
  // nested function to compute isArrayType which is set to true if
  // the inner expression is a type and false otherwise
  //
  // this nested function is called in a type block so that it is
  // never executed; placing all this code in a separate function
  // inside the type block is essential for two reasons:
  //
  // first, so that the iterators in any nested parallel loop
  // expressions are not pulled all the way out during cleanup
  //
  // second, so that types and functions declared in this nested
  // function do not get removed from the IR when the type lbock gets
  // removed
  //
  FnSymbol* isArrayTypeFn = new FnSymbol("_isArrayTypeFn");
  isArrayTypeFn->addFlag(FLAG_INLINE);

  Symbol* isArrayType = newTemp("_isArrayType");
  isArrayType->addFlag(FLAG_MAYBE_PARAM);
  fn->insertAtTail(new DefExpr(isArrayType));

  VarSymbol* iteratorSym = newTemp("_iterator");
  isArrayTypeFn->insertAtTail(new DefExpr(iteratorSym));
  isArrayTypeFn->insertAtTail(new CallExpr(PRIM_MOVE, iteratorSym,
                                new CallExpr("_getIterator", iteratorExpr->copy())));
  VarSymbol* index = newTemp("_indexOfInterest");
  isArrayTypeFn->insertAtTail(new DefExpr(index));
  isArrayTypeFn->insertAtTail(new CallExpr(PRIM_MOVE, index,
                                new CallExpr("iteratorIndex", iteratorSym)));
  BlockStmt* indicesBlock = new BlockStmt();
  destructureIndices(indicesBlock, indices->copy(), new SymExpr(index), false);
  indicesBlock->blockTag = BLOCK_SCOPELESS;
  isArrayTypeFn->insertAtTail(indicesBlock);
  isArrayTypeFn->insertAtTail(new CondStmt(
                                new CallExpr("chpl__isType", expr->copy()),
                                new CallExpr(PRIM_MOVE, isArrayType, gTrue),
                                new CallExpr(PRIM_MOVE, isArrayType, gFalse)));
  fn->insertAtTail(new DefExpr(isArrayTypeFn));
  BlockStmt* typeBlock = new BlockStmt();
  typeBlock->blockTag = BLOCK_TYPE;
  typeBlock->insertAtTail(new CallExpr(isArrayTypeFn));
  fn->insertAtTail(typeBlock);

  Symbol* arrayType = newTemp("_arrayType");
  arrayType->addFlag(FLAG_EXPR_TEMP);
  arrayType->addFlag(FLAG_MAYBE_TYPE);
  BlockStmt* thenStmt = new BlockStmt();
  thenStmt->insertAtTail(new DefExpr(arrayType));
  Symbol* domain = newTemp("_domain");
  domain->addFlag(FLAG_EXPR_TEMP);
  thenStmt->insertAtTail(new DefExpr(domain));
  // note that we need the below autoCopy until we start reference
  // counting domains within runtime array types
  thenStmt->insertAtTail(new CallExpr(PRIM_MOVE, domain,
                           new CallExpr("chpl__autoCopy",
                             new CallExpr("chpl__buildDomainExpr",
                                          iteratorExpr->copy()))));
  if (hasSpecifiedIndices) {
    // we want to swap something like the below commented-out
    // statement with the compiler error statement but skyline
    // arrays are not yet supported...
    thenStmt->insertAtTail(new CallExpr(PRIM_MOVE, arrayType, new CallExpr("compilerError", new_StringSymbol("unimplemented feature: if you are attempting to use skyline arrays, they are not yet supported; if not, remove the index expression from this array type specification"))));
    //      thenStmt->insertAtTail(new CallExpr(PRIM_MOVE, arrayType,
    //                                          new CallExpr("chpl__buildArrayRuntimeType",
    //                                                       domain, expr->copy(),
    //                                                       indices->copy(), domain)));
  } else {
    thenStmt->insertAtTail(new CallExpr(PRIM_MOVE, arrayType,
                             new CallExpr("chpl__buildArrayRuntimeType",
                                          domain, expr->copy())));
  }
  thenStmt->insertAtTail(new CallExpr(PRIM_RETURN, arrayType));
  fn->insertAtTail(new CondStmt(new SymExpr(isArrayType), thenStmt, block));
  return block;
}


static int loopexpr_uid = 1;

// builds body of for expression iterator
CallExpr*
buildForLoopExpr(Expr* indices, Expr* iteratorExpr, Expr* expr, Expr* cond, bool maybeArrayType) {
  FnSymbol* fn = new FnSymbol(astr("_seqloopexpr", istr(loopexpr_uid++)));
  BlockStmt* block = fn->body;

  if (maybeArrayType) {
    INT_ASSERT(!cond);
    block = handleArrayTypeCase(fn, indices, iteratorExpr, expr);
  }

  VarSymbol* iterator = newTemp("_iterator");
  iterator->addFlag(FLAG_EXPR_TEMP);
  block->insertAtTail(new DefExpr(iterator));
  block->insertAtTail(new CallExpr(PRIM_MOVE, iterator, new CallExpr("_checkIterator", iteratorExpr)));
  const char* iteratorName = astr("_iterator_for_loopexpr", istr(loopexpr_uid-1));
  block->insertAtTail(new CallExpr(PRIM_RETURN, new CallExpr(iteratorName, iterator)));

  //
  // build serial iterator function
  //
  FnSymbol* sifn = new FnSymbol(iteratorName);
  ArgSymbol* sifnIterator = new ArgSymbol(INTENT_BLANK, "iterator", dtAny);
  sifn->insertFormalAtTail(sifnIterator);
  fn->insertAtHead(new DefExpr(sifn));
  Expr* stmt = new CallExpr(PRIM_YIELD, expr);
  if (cond)
    stmt = new CondStmt(new CallExpr("_cond_test", cond), stmt);
  sifn->insertAtTail(buildForLoopStmt(indices, new SymExpr(sifnIterator), new BlockStmt(stmt)));
  return new CallExpr(new DefExpr(fn));
}


CallExpr*
buildForallLoopExpr(Expr* indices, Expr* iteratorExpr, Expr* expr, Expr* cond, bool maybeArrayType) {
  if (fSerial || fSerialForall)
    return buildForLoopExpr(indices, iteratorExpr, expr, cond, maybeArrayType);

  FnSymbol* fn = new FnSymbol(astr("_parloopexpr", istr(loopexpr_uid++)));
  BlockStmt* block = fn->body;

  if (maybeArrayType) {
    INT_ASSERT(!cond);
    block = handleArrayTypeCase(fn, indices, iteratorExpr, expr);
  }

  VarSymbol* iterator = newTemp("_iterator");
  iterator->addFlag(FLAG_EXPR_TEMP);
  block->insertAtTail(new DefExpr(iterator));
  block->insertAtTail(new CallExpr(PRIM_MOVE, iterator, new CallExpr("_checkIterator", iteratorExpr)));
  const char* iteratorName = astr("_iterator_for_loopexpr", istr(loopexpr_uid-1));
  block->insertAtTail(new CallExpr(PRIM_RETURN, new CallExpr(iteratorName, iterator)));

  //
  // build serial iterator function
  //
  FnSymbol* sifn = new FnSymbol(iteratorName);
  ArgSymbol* sifnIterator = new ArgSymbol(INTENT_BLANK, "iterator", dtAny);
  sifn->insertFormalAtTail(sifnIterator);
  fn->insertAtHead(new DefExpr(sifn));
  Expr* stmt = new CallExpr(PRIM_YIELD, expr);
  if (cond)
    stmt = new CondStmt(new CallExpr("_cond_test", cond), stmt);
  sifn->insertAtTail(buildForLoopStmt(indices, new SymExpr(sifnIterator), new BlockStmt(stmt)));

  //
  // build leader iterator function
  //
  FnSymbol* lifn = new FnSymbol(iteratorName);
  ArgSymbol* lifnIterator = new ArgSymbol(INTENT_BLANK, "iterator", dtAny);
  lifn->insertFormalAtTail(lifnIterator);
  ArgSymbol* lifnTag = new ArgSymbol(INTENT_PARAM, "tag", gLeaderTag->type);
  lifn->insertFormalAtTail(lifnTag);
  lifn->where = new BlockStmt(new CallExpr("==", lifnTag, gLeaderTag));
  fn->insertAtHead(new DefExpr(lifn));
  VarSymbol* leaderIterator = newTemp("_leaderIterator");
  leaderIterator->addFlag(FLAG_EXPR_TEMP);
  lifn->insertAtTail(new DefExpr(leaderIterator));
  lifn->insertAtTail(new CallExpr(PRIM_MOVE, leaderIterator, new CallExpr("_toLeader", lifnIterator)));
  lifn->insertAtTail(new CallExpr(PRIM_RETURN, leaderIterator));

  //
  // build follower iterator function
  //
  FnSymbol* fifn = new FnSymbol(iteratorName);
  ArgSymbol* fifnIterator = new ArgSymbol(INTENT_BLANK, "iterator", dtAny);
  fifn->insertFormalAtTail(fifnIterator);
  ArgSymbol* fifnTag = new ArgSymbol(INTENT_PARAM, "tag", gFollowerTag->type);
  fifn->insertFormalAtTail(fifnTag);
  ArgSymbol* fifnFollower = new ArgSymbol(INTENT_BLANK, "follower", dtAny);
  fifn->insertFormalAtTail(fifnFollower);
  fifn->where = new BlockStmt(new CallExpr("==", fifnTag, gFollowerTag));
  fn->insertAtHead(new DefExpr(fifn));
  VarSymbol* followerIterator = newTemp("_followerIterator");
  followerIterator->addFlag(FLAG_EXPR_TEMP);
  fifn->insertAtTail(new DefExpr(followerIterator));
  fifn->insertAtTail(new CallExpr(PRIM_MOVE, followerIterator, new CallExpr("_toFollower", fifnIterator, fifnFollower)));
  // do we need to use this map since symbols have not been resolved?
  SymbolMap map;
  Expr* indicesCopy = (indices) ? indices->copy(&map) : NULL;
  Expr* bodyCopy = stmt->copy(&map);
  fifn->insertAtTail(buildForLoopStmt(indicesCopy, new SymExpr(followerIterator), new BlockStmt(bodyCopy)));
  return new CallExpr(new DefExpr(fn));
}


BlockStmt* buildForLoopStmt(Expr* indices,
                            Expr* iteratorExpr,
                            BlockStmt* body,
                            bool coforall) {
  //
  // insert temporary index when elided by user
  //
  if (!indices)
    indices = new UnresolvedSymExpr("_elided_index");

  checkIndices(indices);

  body = new BlockStmt(body);
  BlockStmt* stmts = buildChapelStmt();
  LabelSymbol* continueLabel = new LabelSymbol("_continueLabel");
  continueLabel->addFlag(FLAG_TEMP);
  continueLabel->addFlag(FLAG_LABEL_CONTINUE);
  body->continueLabel = continueLabel;
  LabelSymbol* breakLabel = new LabelSymbol("_breakLabel");
  breakLabel->addFlag(FLAG_TEMP);
  breakLabel->addFlag(FLAG_LABEL_BREAK);
  body->breakLabel = breakLabel;

  VarSymbol* iterator = newTemp("_iterator");
  iterator->addFlag(FLAG_EXPR_TEMP);
  stmts->insertAtTail(new DefExpr(iterator));
  stmts->insertAtTail(new CallExpr(PRIM_MOVE, iterator, new CallExpr("_getIterator", iteratorExpr)));
  VarSymbol* index = newTemp("_indexOfInterest");
  stmts->insertAtTail(new DefExpr(index));
  stmts->insertAtTail(new BlockStmt(
    new CallExpr(PRIM_MOVE, index,
      new CallExpr("iteratorIndex", iterator)),
    BLOCK_TYPE));
  destructureIndices(body, indices, new SymExpr(index), coforall);
  body->blockInfo = new CallExpr(PRIM_BLOCK_FOR_LOOP, index, iterator);

  body->insertAtTail(new DefExpr(continueLabel));
  stmts->insertAtTail(body);
  stmts->insertAtTail(new DefExpr(breakLabel));
  stmts->insertAtTail(new CallExpr("_freeIterator", iterator));
  return stmts;
}


BlockStmt* buildForallLoopStmt(Expr* indices,
                               Expr* iteratorExpr,
                               BlockStmt* body) {
  checkControlFlow(body, "forall statement");

  if (fSerial || fSerialForall)
    return buildForLoopStmt(indices, iteratorExpr, body);

  //
  // insert temporary index when elided by user
  //
  if (!indices)
    indices = new UnresolvedSymExpr("_elided_index");

  checkIndices(indices);

  BlockStmt* leaderBlock = buildChapelStmt();
  VarSymbol* iterator = newTemp("_iterator");
  iterator->addFlag(FLAG_EXPR_TEMP);
  leaderBlock->insertAtTail(new DefExpr(iterator));
  leaderBlock->insertAtTail(new CallExpr(PRIM_MOVE, iterator, new CallExpr("_checkIterator", iteratorExpr)));
  VarSymbol* leaderIndex = newTemp("_leaderIndex");
  leaderBlock->insertAtTail(new DefExpr(leaderIndex));
  VarSymbol* leaderIterator = newTemp("_leaderIterator");
  leaderBlock->insertAtTail(new DefExpr(leaderIterator));
  VarSymbol* leaderIndexCopy = newTemp("_leaderIndexCopy");
  leaderIndexCopy->addFlag(FLAG_INDEX_VAR);
  leaderIndexCopy->addFlag(FLAG_INSERT_AUTO_DESTROY);
  leaderBlock->insertAtTail(new CallExpr(PRIM_MOVE, leaderIterator, new CallExpr("_getIterator", new CallExpr("_toLeader", iterator))));
  BlockStmt* followerBlock = new BlockStmt();
  VarSymbol* followerIndex = newTemp("_followerIndex");
  followerBlock->insertAtTail(new DefExpr(followerIndex));
  VarSymbol* followerIterator = newTemp("_followerIterator");
  followerBlock->insertAtTail(new DefExpr(followerIterator));
  followerBlock->insertAtTail(new CallExpr(PRIM_MOVE, followerIterator, new CallExpr("_getIterator", new CallExpr("_toFollower", iterator, leaderIndexCopy))));
  followerBlock->insertAtTail(new BlockStmt(new CallExpr(PRIM_MOVE, followerIndex, new CallExpr("iteratorIndex", followerIterator)), BLOCK_TYPE));
  BlockStmt* followerBody = new BlockStmt(body);
  destructureIndices(followerBody, indices, new SymExpr(followerIndex), false);
  followerBody->blockInfo = new CallExpr(PRIM_BLOCK_FOR_LOOP, followerIndex, followerIterator);
  followerBlock->insertAtTail(followerBody);
  followerBlock->insertAtTail(new CallExpr("_freeIterator", followerIterator));

  BlockStmt* beginBlock = new BlockStmt();
  beginBlock->insertAtTail(new DefExpr(leaderIndexCopy));
  beginBlock->insertAtTail(new CallExpr(PRIM_MOVE, leaderIndexCopy, leaderIndex));
  beginBlock->insertAtTail(followerBlock);

  BlockStmt* leaderBody = new BlockStmt(beginBlock);
  leaderBlock->insertAtTail(new BlockStmt(new CallExpr(PRIM_MOVE, leaderIndex, new CallExpr("iteratorIndex", leaderIterator)), BLOCK_TYPE));
  leaderBody->blockInfo = new CallExpr(PRIM_BLOCK_FOR_LOOP, leaderIndex, leaderIterator);
  leaderBlock->insertAtTail(leaderBody);
  leaderBlock->insertAtTail(new CallExpr("_freeIterator", leaderIterator));

  return leaderBlock;
}


BlockStmt* buildCoforallLoopStmt(Expr* indices, Expr* iterator, BlockStmt* body) {
  checkControlFlow(body, "coforall statement");

  if (fSerial)
    return buildForLoopStmt(indices, iterator, body);

  //
  // insert temporary index when elided by user
  //
  if (!indices)
    indices = new UnresolvedSymExpr("_elided_index");

  checkIndices(indices);

  //
  // detect on-statement directly inside coforall-loop
  //
  BlockStmt* onBlock = NULL;
  BlockStmt* tmp = body;
  while (tmp) {
    if (BlockStmt* b = toBlockStmt(tmp->body.tail)) {
      if (b->blockInfo && b->blockInfo->isPrimitive(PRIM_BLOCK_ON)) {
        onBlock = b;
        break;
      }
    }
    if (tmp->body.tail == tmp->body.head) {
      tmp = toBlockStmt(tmp->body.tail);
      if (tmp && tmp->blockInfo)
        tmp = NULL;
    } else
      tmp = NULL;
  }

  if (onBlock) {
    //
    // optimization of on-statements directly inside coforall-loops
    //
    //   In this case, the on-statement is made into a non-blocking
    //   on-statement and the coforall is serialized (rather than
    //   wasting threads that would do nothing other than wait on the
    //   on-statement.
    //
    VarSymbol* coforallCount = newTemp("_coforallCount");
    BlockStmt* block = buildForLoopStmt(indices, iterator, body, true);
    block->insertAtHead(new CallExpr(PRIM_MOVE, coforallCount, new CallExpr("_endCountAlloc")));
    block->insertAtHead(new DefExpr(coforallCount));
    body->insertAtHead(new CallExpr("_upEndCount", coforallCount));
    block->insertAtTail(new CallExpr("_waitEndCount", coforallCount));
    block->insertAtTail(new CallExpr("_endCountFree", coforallCount));
    onBlock->blockInfo->primitive = primitives[PRIM_BLOCK_ON_NB];
    BlockStmt* innerOnBlock = new BlockStmt();
    for_alist(tmp, onBlock->body) {
      innerOnBlock->insertAtTail(tmp->remove());
    }
    onBlock->insertAtHead(innerOnBlock);
    onBlock->insertAtTail(new CallExpr("_downEndCount", coforallCount));
    return block;
  } else {
    VarSymbol* coforallCount = newTemp("_coforallCount");
    BlockStmt* beginBlk = new BlockStmt();
    beginBlk->blockInfo = new CallExpr(PRIM_BLOCK_COFORALL);
    beginBlk->insertAtHead(body);
    beginBlk->insertAtTail(new CallExpr("_downEndCount", coforallCount));
    BlockStmt* block = buildForLoopStmt(indices, iterator, beginBlk, true);
    block->insertAtHead(new CallExpr(PRIM_MOVE, coforallCount, new CallExpr("_endCountAlloc")));
    block->insertAtHead(new DefExpr(coforallCount));
    block->insertAtTail(new CallExpr(PRIM_PROCESS_TASK_LIST, coforallCount));
    beginBlk->insertBefore(new CallExpr("_upEndCount", coforallCount));
    block->insertAtTail(new CallExpr("_waitEndCount", coforallCount));
    block->insertAtTail(new CallExpr("_endCountFree", coforallCount));
    return block;
  }
}


static Symbol*
insertBeforeCompilerTemp(Expr* stmt, Expr* expr) {
  Symbol* expr_var = newTemp();
  expr_var->addFlag(FLAG_MAYBE_PARAM);
  stmt->insertBefore(new DefExpr(expr_var));
  stmt->insertBefore(new CallExpr(PRIM_MOVE, expr_var, expr));
  return expr_var;
}


BlockStmt* buildParamForLoopStmt(const char* index, Expr* range, BlockStmt* stmts) {
  BlockStmt* block = new BlockStmt(stmts);
  BlockStmt* outer = new BlockStmt(block);
  VarSymbol* indexVar = new VarSymbol(index);
  block->insertBefore(new DefExpr(indexVar, new_IntSymbol((int64_t)0)));
  Expr *low = NULL, *high = NULL, *stride;
  CallExpr* call = toCallExpr(range);
  if (call && call->isNamed("by")) {
    stride = call->get(2)->remove();
    call = toCallExpr(call->get(1));
  } else {
    stride = new SymExpr(new_IntSymbol(1));
  }
  if (call && call->isNamed("_build_range")) {
    low = call->get(1)->remove();
    high = call->get(1)->remove();
  } else
    USR_FATAL(range, "iterators for param-for-loops must be literal ranges");
  Symbol* lowVar = insertBeforeCompilerTemp(block, low);
  Symbol* highVar = insertBeforeCompilerTemp(block, high);
  Symbol* strideVar = insertBeforeCompilerTemp(block, stride);
  block->blockInfo = new CallExpr(PRIM_BLOCK_PARAM_LOOP, indexVar, lowVar, highVar, strideVar);
  return buildChapelStmt(outer);
}


BlockStmt*
buildAssignment(Expr* lhs, Expr* rhs, const char* op) {
  if (op == NULL)
    return buildChapelStmt(new CallExpr("=", lhs, rhs));

  BlockStmt* stmt = buildChapelStmt();

  VarSymbol* ltmp = newTemp();
  ltmp->addFlag(FLAG_MAYBE_PARAM);
  stmt->insertAtTail(new DefExpr(ltmp));
  stmt->insertAtTail(new CallExpr(PRIM_MOVE, ltmp,
                       new CallExpr(PRIM_SET_REF, lhs)));

  VarSymbol* rtmp = newTemp();
  rtmp->addFlag(FLAG_MAYBE_PARAM);
  rtmp->addFlag(FLAG_EXPR_TEMP);
  stmt->insertAtTail(new DefExpr(rtmp));
  stmt->insertAtTail(new CallExpr(PRIM_MOVE, rtmp, rhs));

  BlockStmt* cast =
    new BlockStmt(
      new CallExpr("=", ltmp,
        new CallExpr("_cast",
          new CallExpr(PRIM_TYPEOF, ltmp),
          new CallExpr(op,
            new CallExpr(PRIM_GET_REF, ltmp), rtmp))));

  if (strcmp(op, "<<") && strcmp(op, ">>"))
    cast->insertAtHead(
      new BlockStmt(new CallExpr("=", ltmp, rtmp), BLOCK_TYPE));

  CondStmt* inner =
    new CondStmt(
      new CallExpr("_isPrimitiveType",
        new CallExpr(PRIM_TYPEOF,
          new CallExpr(PRIM_GET_REF, ltmp))),
      cast,
      new CallExpr("=", ltmp,
        new CallExpr(op,
          new CallExpr(PRIM_GET_REF, ltmp), rtmp)));

  if (!strcmp(op, "+")) {
    stmt->insertAtTail(
      new CondStmt(
        new CallExpr("chpl__isDomain", ltmp),
        new CallExpr(
          new CallExpr(".", ltmp, new_StringSymbol("add")), rtmp),
        inner));
  } else if (!strcmp(op, "-")) {
    stmt->insertAtTail(
      new CondStmt(
        new CallExpr("chpl__isDomain", ltmp),
        new CallExpr(
          new CallExpr(".", ltmp, new_StringSymbol("remove")), rtmp),
        inner));
  } else {
    stmt->insertAtTail(inner);
  }

  return stmt;
}


BlockStmt* buildLAndAssignment(Expr* lhs, Expr* rhs) {
  BlockStmt* stmt = buildChapelStmt();
  VarSymbol* ltmp = newTemp();
  stmt->insertAtTail(new DefExpr(ltmp));
  stmt->insertAtTail(new CallExpr(PRIM_MOVE, ltmp, new CallExpr(PRIM_SET_REF, lhs)));
  stmt->insertAtTail(new CallExpr("=", ltmp, buildLogicalAndExpr(ltmp, rhs)));
  return stmt;
}


BlockStmt* buildLOrAssignment(Expr* lhs, Expr* rhs) {
  BlockStmt* stmt = buildChapelStmt();
  VarSymbol* ltmp = newTemp();
  stmt->insertAtTail(new DefExpr(ltmp));
  stmt->insertAtTail(new CallExpr(PRIM_MOVE, ltmp, new CallExpr(PRIM_SET_REF, lhs)));
  stmt->insertAtTail(new CallExpr("=", ltmp, buildLogicalOrExpr(ltmp, rhs)));
  return stmt;
}

BlockStmt* buildSwapStmt(Expr* lhs, Expr* rhs) {
  return buildChapelStmt(new CallExpr("_chpl_swap", lhs, rhs));
}

BlockStmt* buildSelectStmt(Expr* selectCond, BlockStmt* whenstmts) {
  CondStmt* otherwise = NULL;
  CondStmt* top = NULL;
  CondStmt* condStmt = NULL;

  for_alist(stmt, whenstmts->body) {
    CondStmt* when = toCondStmt(stmt);
    if (!when)
      INT_FATAL("error in buildSelectStmt");
    CallExpr* conds = toCallExpr(when->condExpr);
    if (!conds || !conds->isPrimitive(PRIM_WHEN))
      INT_FATAL("error in buildSelectStmt");
    if (conds->numActuals() == 0) {
      if (otherwise)
        USR_FATAL(selectCond, "Select has multiple otherwise clauses");
      otherwise = when;
    } else {
      Expr* expr = NULL;
      for_actuals(whenCond, conds) {
        whenCond->remove();
        if (!expr)
          expr = new CallExpr("==", selectCond->copy(), whenCond);
        else
          expr = buildLogicalOrExpr(expr, new CallExpr("==",
                                                   selectCond->copy(),
                                                   whenCond));
      }
      if (!condStmt) {
        condStmt = new CondStmt(new CallExpr("_cond_test", expr), when->thenStmt);
        top = condStmt;
      } else {
        CondStmt* next = new CondStmt(new CallExpr("_cond_test", expr), when->thenStmt);
        condStmt->elseStmt = new BlockStmt(next);
        condStmt = next;
      }
    }
  }
  if (otherwise) {
    if (!condStmt)
      USR_FATAL(selectCond, "Select has no when clauses");
    condStmt->elseStmt = otherwise->thenStmt;
  }
  return buildChapelStmt(top);
}


BlockStmt* buildTypeSelectStmt(CallExpr* exprs, BlockStmt* whenstmts) {
  static int uid = 1;
  int caseId = 1;
  FnSymbol* fn = NULL;
  BlockStmt* stmts = buildChapelStmt();
  BlockStmt* newWhenStmts = buildChapelStmt();
  bool has_otherwise = false;

  INT_ASSERT(exprs->isPrimitive(PRIM_ACTUALS_LIST));

  for_alist(stmt, whenstmts->body) {
    CondStmt* when = toCondStmt(stmt);
    if (!when)
      INT_FATAL("error in buildSelectStmt");
    CallExpr* conds = toCallExpr(when->condExpr);
    if (!conds || !conds->isPrimitive(PRIM_WHEN))
      INT_FATAL("error in buildSelectStmt");
    if (conds->numActuals() == 0) {
      if (has_otherwise)
        USR_FATAL(conds, "Type select statement has multiple otherwise clauses");
      has_otherwise = true;
      fn = new FnSymbol(astr("_typeselect", istr(uid)));
      int lid = 1;
      for_actuals(expr, exprs) {
        fn->insertFormalAtTail(
          new DefExpr(
            new ArgSymbol(INTENT_BLANK,
                          astr("_t", istr(lid++)),
                          dtAny)));
      }
      fn->retTag = RET_PARAM;
      fn->insertAtTail(new CallExpr(PRIM_RETURN, new_IntSymbol(caseId)));
      newWhenStmts->insertAtTail(
        new CondStmt(new CallExpr(PRIM_WHEN, new_IntSymbol(caseId++)),
        when->thenStmt->copy()));
      stmts->insertAtTail(new DefExpr(fn));
    } else {
      if (conds->numActuals() != exprs->argList.length)
        USR_FATAL(when, "Type select statement requires number of selectors to be equal to number of when conditions");
      fn = new FnSymbol(astr("_typeselect", istr(uid)));
      int lid = 1;
      for_actuals(expr, conds) {
        fn->insertFormalAtTail(
          new DefExpr(new ArgSymbol(INTENT_BLANK, astr("_t", istr(lid++)),
                                    dtUnknown, expr->copy())));
      }
      fn->retTag = RET_PARAM;
      fn->insertAtTail(new CallExpr(PRIM_RETURN, new_IntSymbol(caseId)));
      newWhenStmts->insertAtTail(
        new CondStmt(new CallExpr(PRIM_WHEN, new_IntSymbol(caseId++)),
        when->thenStmt->copy()));
      stmts->insertAtTail(new DefExpr(fn));
    }
  }
  VarSymbol* tmp = newTemp();
  tmp->addFlag(FLAG_MAYBE_PARAM);
  stmts->insertAtHead(new DefExpr(tmp));
  stmts->insertAtTail(new CallExpr(PRIM_MOVE,
                                   tmp,
                                   new CallExpr(fn->name, exprs)));
  stmts->insertAtTail(buildSelectStmt(new SymExpr(tmp), newWhenStmts));
  return stmts;
}


static CallExpr*
buildReduceScanExpr(Expr* op, Expr* dataExpr, bool isScan) {
  if (UnresolvedSymExpr* sym = toUnresolvedSymExpr(op)) {
    if (!strcmp(sym->unresolved, "max"))
      sym->unresolved = astr("MaxReduceScanOp");
    else if (!strcmp(sym->unresolved, "min"))
      sym->unresolved = astr("MinReduceScanOp");
  }
  static int uid = 1;
  FnSymbol* fn = new FnSymbol(astr("_reduce_scan", istr(uid++)));
  fn->addFlag(FLAG_DONT_DISABLE_REMOTE_VALUE_FORWARDING);
  fn->addFlag(FLAG_INLINE);
  VarSymbol* data = newTemp();
  data->addFlag(FLAG_EXPR_TEMP);
  fn->insertAtTail(new DefExpr(data));
  fn->insertAtTail(new CallExpr(PRIM_MOVE, data, dataExpr));
  VarSymbol* eltType = newTemp();
  eltType->addFlag(FLAG_MAYBE_TYPE);
  fn->insertAtTail(new DefExpr(eltType));
  fn->insertAtTail(
    new BlockStmt(
      new CallExpr(PRIM_MOVE, eltType,
        new CallExpr(PRIM_TYPEOF,
          new CallExpr("chpl__initCopy",
            new CallExpr("iteratorIndex",
              new CallExpr("_getIterator", data))))),
      BLOCK_TYPE));
  VarSymbol* globalOp = newTemp();
  fn->insertAtTail(new DefExpr(globalOp));
  fn->insertAtTail(
    new CallExpr(PRIM_MOVE, globalOp,
      new CallExpr(PRIM_NEW,
        new CallExpr(op,
          new NamedExpr("eltType", new SymExpr(eltType))))));
  if (isScan) {
    VarSymbol* tmp = newTemp();
    tmp->addFlag(FLAG_EXPR_TEMP);
    fn->insertAtTail(new DefExpr(tmp));
    fn->insertAtTail(new CallExpr(PRIM_MOVE, tmp, new CallExpr("_scan", globalOp, data)));
    fn->insertAtTail(new CallExpr(PRIM_DELETE, globalOp));
    fn->insertAtTail(new CallExpr(PRIM_RETURN, tmp));
  } else {
    if (fSerial || fSerialForall) {
      VarSymbol* index = newTemp("_index");
      fn->insertAtTail(new DefExpr(index));
      fn->insertAtTail(buildForLoopStmt(new SymExpr(index), new SymExpr(data), new BlockStmt(new CallExpr(new CallExpr(".", globalOp, new_StringSymbol("accumulate")), index))));
    } else {

      BlockStmt* serialBlock = buildChapelStmt();
      VarSymbol* index = newTemp("_index");
      serialBlock->insertAtTail(new DefExpr(index));
      serialBlock->insertAtTail(buildForLoopStmt(new SymExpr(index), new SymExpr(data), new BlockStmt(new CallExpr(new CallExpr(".", globalOp, new_StringSymbol("accumulate")), index))));

      BlockStmt* leaderBlock = buildChapelStmt();
      VarSymbol* leaderIndex = newTemp("_leaderIndex");
      leaderBlock->insertAtTail(new DefExpr(leaderIndex));
      VarSymbol* leaderIterator = newTemp("_leaderIterator");
      leaderIterator->addFlag(FLAG_EXPR_TEMP);
      leaderBlock->insertAtTail(new DefExpr(leaderIterator));

      VarSymbol* leaderIndexCopy = newTemp("_leaderIndexCopy");
      leaderIndexCopy->addFlag(FLAG_INDEX_VAR);
      leaderIndexCopy->addFlag(FLAG_INSERT_AUTO_DESTROY);

      leaderBlock->insertAtTail(new CallExpr(PRIM_MOVE, leaderIterator, new CallExpr("_getIterator", new CallExpr("_toLeader", data))));

      BlockStmt* followerBlock = new BlockStmt();
      VarSymbol* followerIndex = newTemp("_followerIndex");
      followerBlock->insertAtTail(new DefExpr(followerIndex));
      VarSymbol* followerIterator = newTemp("_followerIterator");
      followerIterator->addFlag(FLAG_EXPR_TEMP);
      followerBlock->insertAtTail(new DefExpr(followerIterator));

      followerBlock->insertAtTail(new CallExpr(PRIM_MOVE, followerIterator, new CallExpr("_getIterator", new CallExpr("_toFollower", data, leaderIndexCopy))));

      followerBlock->insertAtTail(new BlockStmt(new CallExpr(PRIM_MOVE, followerIndex, new CallExpr("iteratorIndex", followerIterator)), BLOCK_TYPE));

      VarSymbol* localOp = newTemp();
      followerBlock->insertAtTail(new DefExpr(localOp));
      followerBlock->insertAtTail(
        new CallExpr(PRIM_MOVE, localOp,
          new CallExpr(PRIM_NEW,
            new CallExpr(op->copy(),
              new NamedExpr("eltType", new SymExpr(eltType))))));

      BlockStmt* followerBody =new BlockStmt(new CallExpr(new CallExpr(".", localOp, new_StringSymbol("accumulate")), followerIndex));
      
      followerBody->blockInfo = new CallExpr(PRIM_BLOCK_FOR_LOOP, followerIndex, followerIterator);
      followerBlock->insertAtTail(followerBody);
      BlockStmt* combineBlock = new BlockStmt();
      combineBlock->insertAtTail(new CallExpr(new CallExpr(".", globalOp, new_StringSymbol("lock"))));
      combineBlock->insertAtTail(new CallExpr(new CallExpr(".", globalOp, new_StringSymbol("combine")), localOp));
      combineBlock->insertAtTail(new CallExpr(new CallExpr(".", globalOp, new_StringSymbol("unlock"))));
      followerBlock->insertAtTail(buildOnStmt(new SymExpr(globalOp), combineBlock));
      followerBlock->insertAtTail(new CallExpr(PRIM_DELETE, localOp));
      followerBlock->insertAtTail(new CallExpr("_freeIterator", followerIterator));

      BlockStmt* beginBlock = new BlockStmt();
      beginBlock->insertAtTail(new DefExpr(leaderIndexCopy));
      beginBlock->insertAtTail(new CallExpr(PRIM_MOVE, leaderIndexCopy, leaderIndex));
      beginBlock->insertAtTail(followerBlock);

      BlockStmt* leaderBody = new BlockStmt(beginBlock);
      leaderBlock->insertAtTail(new BlockStmt(new CallExpr(PRIM_MOVE, leaderIndex, new CallExpr("iteratorIndex", leaderIterator)), BLOCK_TYPE));
      leaderBody->blockInfo = new CallExpr(PRIM_BLOCK_FOR_LOOP, leaderIndex, leaderIterator);
      leaderBlock->insertAtTail(leaderBody);
      leaderBlock->insertAtTail(new CallExpr("_freeIterator", leaderIterator));

      fn->insertAtTail(new CondStmt(new SymExpr(gTryToken), leaderBlock, serialBlock));
    }
    VarSymbol* result = new VarSymbol("result");
    fn->insertAtTail(new DefExpr(result, new CallExpr(new CallExpr(".", globalOp, new_StringSymbol("generate")))));
    fn->insertAtTail(new CallExpr(PRIM_DELETE, globalOp));
    fn->insertAtTail(new CallExpr(PRIM_RETURN, result));
  }
  return new CallExpr(new DefExpr(fn));
}


CallExpr* buildReduceExpr(Expr* op, Expr* data) {
  return buildReduceScanExpr(op, data, false);
}


CallExpr* buildScanExpr(Expr* op, Expr* data) {
  return buildReduceScanExpr(op, data, true);
}


static void
backPropagateInitsTypes(BlockStmt* stmts) {
  Expr* init = NULL;
  Expr* type = NULL;
  DefExpr* last = NULL;
  for_alist_backward(stmt, stmts->body) {
    if (DefExpr* def = toDefExpr(stmt)) {
      if (def->init || def->exprType) {
        init = def->init;
        type = def->exprType;
      } else {
        if (type)
          last->exprType =
            new CallExpr(PRIM_TYPEOF, new UnresolvedSymExpr(def->sym->name));
        if (init && type)
          last->init =
            new CallExpr("chpl__readXX", new UnresolvedSymExpr(def->sym->name));
        else if (init && !type)
          last->init = new UnresolvedSymExpr(def->sym->name);
        def->init = init;
        def->exprType = type;
      }
      last = def;
    } else
      INT_FATAL(stmt, "expected DefExpr in backPropagateInitsTypes");
  }
}


BlockStmt*
buildVarDecls(BlockStmt* stmts, bool isConfig, bool isParam, bool isConst) {
  for_alist(stmt, stmts->body) {
    if (DefExpr* defExpr = toDefExpr(stmt)) {
      if (VarSymbol* var = toVarSymbol(defExpr->sym)) {
        if (isConfig)
          var->addFlag(FLAG_CONFIG);
        if (isParam)
          var->addFlag(FLAG_PARAM);
        if (isConst)
          var->addFlag(FLAG_CONST);
        continue;
      }
    }
    INT_FATAL(stmt, "Major error in setVarSymbolAttributes");
  }
  backPropagateInitsTypes(stmts);
  return stmts;
}


DefExpr*
buildClassDefExpr(const char* name, Type* type, Expr* inherit, BlockStmt* decls, bool isExtern) {
  ClassType* ct = toClassType(type);
  INT_ASSERT(ct);
  TypeSymbol* ts = new TypeSymbol(name, ct);
  DefExpr* def = new DefExpr(ts);
  ct->addDeclarations(decls);
  if (isExtern)
    ts->addFlag(FLAG_EXTERN);
  if (inherit)
    ct->inherits.insertAtTail(inherit);
  return def;
}


DefExpr*
buildArgDefExpr(IntentTag tag, const char* ident, Expr* type, Expr* init, Expr* variable) {
  ArgSymbol* arg = new ArgSymbol(tag, ident, dtUnknown, type, init, variable);
  if (arg->intent == INTENT_TYPE) {
    type = NULL;
    arg->intent = INTENT_BLANK;
    arg->addFlag(FLAG_TYPE_VARIABLE);
    arg->type = dtAny;
  } else if (!type && !init)
    arg->type = dtAny;
  return new DefExpr(arg);
}


//
// create a single argument and store the tuple-grouped args in the
// variable argument list; these will be moved out of the variable
// argument list in the call to destructureTupleGroupedArgs when the
// formals are added to the formals list in the function (in
// buildFunctionFormal)
//
DefExpr*
buildTupleArgDefExpr(IntentTag tag, BlockStmt* tuple, Expr* type, Expr* init) {
  ArgSymbol* arg = new ArgSymbol(tag, "chpl__tuple_arg_temp", dtUnknown,
                                 type, init, tuple);
  arg->addFlag(FLAG_TEMP);
  if (arg->intent != INTENT_BLANK)
    USR_FATAL(tuple, "intents on tuple-grouped arguments are not yet supported");
  if (!type)
    arg->type = dtTuple;
  return new DefExpr(arg);
}


//
// Destructure tuple function arguments.  Add to the function's where
// clause to match the shape of the tuple being destructured.
//
static void
destructureTupleGroupedArgs(FnSymbol* fn, BlockStmt* tuple, Expr* base) {
  int i = 0;
  for_alist(expr, tuple->body) {
    i++;
    if (DefExpr* def = toDefExpr(expr)) {
      def->init = new CallExpr(base->copy(), new_IntSymbol(i));
      if (!strcmp(def->sym->name, "chpl__tuple_blank")) {
        def->remove();
      } else {
        fn->insertAtHead(def->remove());
      }
    } else if (BlockStmt* inner = toBlockStmt(expr)) {
      destructureTupleGroupedArgs(fn, inner, new CallExpr(base->copy(), new_IntSymbol(i)));
    }
  }

  Expr* where =
    buildLogicalAndExpr(
      new CallExpr(PRIM_IS_TUPLE, base->copy()),
      new CallExpr("==", new_IntSymbol(i),
        new CallExpr(".", base->copy(), new_StringSymbol("size"))));

  if (fn->where) {
    where = buildLogicalAndExpr(fn->where->body.head->remove(), where);
    fn->where->body.insertAtHead(where);
  } else {
    fn->where = new BlockStmt(where);
  }
}


FnSymbol*
buildFunctionFormal(FnSymbol* fn, DefExpr* def) {
  if (!fn)
    fn = new FnSymbol("_");
  if (!def)
    return fn;
  ArgSymbol* arg = toArgSymbol(def->sym);
  INT_ASSERT(arg);
  fn->insertFormalAtTail(def);
  if (!strcmp(arg->name, "chpl__tuple_arg_temp")) {
    destructureTupleGroupedArgs(fn, arg->variableExpr, new SymExpr(arg));
    arg->variableExpr = NULL;
  }
  return fn;
}


BlockStmt* buildLocalStmt(Expr* stmt) {
  BlockStmt* block = buildChapelStmt();

  if (fLocal) {
    block->insertAtTail(stmt);
    return block;
  }

  BlockStmt* localBlock = new BlockStmt(stmt);
  localBlock->blockInfo = new CallExpr(PRIM_BLOCK_LOCAL);
  block->insertAtTail(localBlock);
  return block;
}


static Expr* buildOnExpr(Expr* expr) {
  // If the on <x> expression is a primitive_on_locale_num, we just want
  // to strip off the primitive and have the naked integer value be the
  // locale ID.
  if (CallExpr* call = toCallExpr(expr)) {
    if (call->isPrimitive(PRIM_ON_LOCALE_NUM)) {
      return call->get(1);
    }
  }

  // Otherwise, we need to wrap the expression in a primitive to query
  // the locale ID of the expression
  return new CallExpr(PRIM_GET_LOCALEID, expr);
}


BlockStmt*
buildOnStmt(Expr* expr, Expr* stmt) {
  checkControlFlow(stmt, "on statement");

  /* GPU Case */
  if (CallExpr* call = toCallExpr(expr)) {
    if (call->isPrimitive(PRIM_ON_GPU)) {
      BlockStmt* block = buildChapelStmt();
      BlockStmt* onBlock = new BlockStmt(stmt);
      //Expr *arg1 = call->get(1)->remove(); 
      //Expr *arg2 = call->get(1)->remove(); 
      //onBlock->blockInfo = new CallExpr(PRIM_ON_GPU, arg1, arg2);
      onBlock->blockInfo = call;
      block->insertAtTail(onBlock);
      return block;
    }
  }

  CallExpr* onExpr = new CallExpr(PRIM_GET_REF, buildOnExpr(expr));

  BlockStmt* body = toBlockStmt(stmt);

  //
  // detect begin statement directly inside on-statement
  //
  BlockStmt* beginBlock = NULL;
  BlockStmt* tmp = body;
  while (tmp) {
    if (BlockStmt* b = toBlockStmt(tmp->body.tail)) {
      if (b->blockInfo && b->blockInfo->isPrimitive(PRIM_BLOCK_BEGIN)) {
        beginBlock = b;
        break;
      }
    }
    if (tmp->body.tail == tmp->body.head) {
      tmp = toBlockStmt(tmp->body.tail);
      if (tmp && tmp->blockInfo)
        tmp = NULL;
    } else
      tmp = NULL;
  }

  if (fLocal) {
    BlockStmt* block = new BlockStmt(stmt);
    block->insertAtHead(onExpr); // evaluate the expression for side effects
    return buildChapelStmt(block);
  }

  if (beginBlock) {
    Symbol* tmp = newTemp();
    body->insertAtHead(new CallExpr(PRIM_MOVE, tmp, onExpr));
    body->insertAtHead(new DefExpr(tmp));
    beginBlock->blockInfo = new CallExpr(PRIM_BLOCK_ON, tmp);
    beginBlock->blockInfo->primitive = primitives[PRIM_BLOCK_ON_NB];
    return body;
  } else {
    BlockStmt* block = buildChapelStmt();
    Symbol* tmp = newTemp();
    block->insertAtTail(new DefExpr(tmp));
    block->insertAtTail(new CallExpr(PRIM_MOVE, tmp, onExpr));
    BlockStmt* onBlock = new BlockStmt(stmt);
    onBlock->blockInfo = new CallExpr(PRIM_BLOCK_ON, tmp);
    block->insertAtTail(onBlock);
    return block;
  }
}


BlockStmt*
buildBeginStmt(Expr* stmt) {
  checkControlFlow(stmt, "begin statement");

  if (fSerial)
    return buildChapelStmt(new BlockStmt(stmt));

  BlockStmt* body = toBlockStmt(stmt);
  
  //
  // detect on-statement directly inside begin statement
  //
  BlockStmt* onBlock = NULL;
  BlockStmt* tmp = body;
  while (tmp) {
    if (BlockStmt* b = toBlockStmt(tmp->body.tail)) {
      if (b->blockInfo && b->blockInfo->isPrimitive(PRIM_BLOCK_ON)) {
        onBlock = b;
        break;
      }
    }
    if (tmp->body.tail == tmp->body.head) {
      tmp = toBlockStmt(tmp->body.tail);
      if (tmp && tmp->blockInfo)
        tmp = NULL;
    } else
      tmp = NULL;
  }

  if (onBlock) {
    body->insertAtHead(new CallExpr("_upEndCount"));
    onBlock->insertAtTail(new CallExpr("_downEndCount"));
    onBlock->blockInfo->primitive = primitives[PRIM_BLOCK_ON_NB];
    return body;
  } else {
    BlockStmt* block = buildChapelStmt();
    block->insertAtTail(new CallExpr("_upEndCount"));
    BlockStmt* beginBlock = new BlockStmt();
    beginBlock->blockInfo = new CallExpr(PRIM_BLOCK_BEGIN);
    beginBlock->insertAtHead(stmt);
    beginBlock->insertAtTail(new CallExpr("_downEndCount"));
    block->insertAtTail(beginBlock);
    return block;
  }
}


BlockStmt*
buildSyncStmt(Expr* stmt) {
  checkControlFlow(stmt, "sync statement");
  if (fSerial)
    return buildChapelStmt(new BlockStmt(stmt));
  BlockStmt* block = new BlockStmt();
  VarSymbol* endCountSave = newTemp("_endCountSave");
  block->insertAtTail(new DefExpr(endCountSave));
  block->insertAtTail(new CallExpr(PRIM_MOVE, endCountSave, new CallExpr(PRIM_GET_END_COUNT)));
  block->insertAtTail(new CallExpr(PRIM_SET_END_COUNT, new CallExpr("_endCountAlloc")));
  block->insertAtTail(stmt);
  block->insertAtTail(new CallExpr("_waitEndCount"));
  block->insertAtTail(new CallExpr("_endCountFree", new CallExpr(PRIM_GET_END_COUNT)));
  block->insertAtTail(new CallExpr(PRIM_SET_END_COUNT, endCountSave));
  return block;
}


BlockStmt*
buildCobeginStmt(BlockStmt* block) {
  BlockStmt* outer = block;

  checkControlFlow(block, "cobegin statement");

  if (block->blockTag == BLOCK_SCOPELESS) {
    block = toBlockStmt(block->body.only());
    INT_ASSERT(block);
    block->remove();
  }

  if (block->length() < 2) {
    USR_WARN(outer, "cobegin has no effect if it contains fewer than 2 statements");
    return buildChapelStmt(block);
  }

  if (fSerial)
    return buildChapelStmt(block);

  VarSymbol* cobeginCount = newTemp("_cobeginCount");
  cobeginCount->addFlag(FLAG_TEMP);

  for_alist(stmt, block->body) {
    BlockStmt* beginBlk = new BlockStmt();
    beginBlk->blockInfo = new CallExpr(PRIM_BLOCK_COBEGIN);
    stmt->insertBefore(beginBlk);
    beginBlk->insertAtHead(stmt->remove());
    beginBlk->insertAtTail(new CallExpr("_downEndCount", cobeginCount));
    block->insertAtHead(new CallExpr("_upEndCount", cobeginCount));
  }

  block->insertAtHead(new CallExpr(PRIM_MOVE, cobeginCount, new CallExpr("_endCountAlloc")));
  block->insertAtHead(new DefExpr(cobeginCount));
  block->insertAtTail(new CallExpr(PRIM_PROCESS_TASK_LIST, cobeginCount));
  block->insertAtTail(new CallExpr("_waitEndCount", cobeginCount));
  block->insertAtTail(new CallExpr("_endCountFree", cobeginCount));

  return block;
}


BlockStmt*
buildGotoStmt(GotoTag tag, const char* name) {
  return buildChapelStmt(new GotoStmt(tag, name));
}

BlockStmt* buildPrimitiveStmt(PrimitiveTag tag, Expr* e1, Expr* e2) {
  return buildChapelStmt(new CallExpr(tag, e1, e2));
}

BlockStmt*
buildAtomicStmt(Expr* stmt) {
  static bool atomic_warning = false;

  if (!atomic_warning) {
    atomic_warning = true;
    USR_WARN(stmt, "atomic keyword is ignored (not implemented)");
  }
  return buildChapelStmt(new BlockStmt(stmt));
}


CallExpr* buildPreDecIncWarning(Expr* expr, char sign) {
  if (sign == '+') {
    USR_WARN(expr, "++ is not a pre-increment");
    return new CallExpr("+", new CallExpr("+", expr));
  } else if (sign == '-') {
    USR_WARN(expr, "-- is not a pre-decrement");
    return new CallExpr("-", new CallExpr("-", expr));
  } else {
    INT_FATAL(expr, "Error in parser");
  }
  return NULL;
}

