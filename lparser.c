/*
** $Id: lparser.c,v 1.141 2001/04/05 16:49:14 roberto Exp roberto $
** LL(1) Parser and code generator for Lua
** See Copyright Notice in lua.h
*/


#include <stdio.h>
#include <string.h>

#define LUA_PRIVATE
#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"


/*
** Constructors descriptor:
** `n' indicates number of elements, and `k' signals whether
** it is a list constructor (k = 0) or a record constructor (k = 1)
** or empty (k = `;' or `}')
*/
typedef struct Constdesc {
  int n;
  int k;
} Constdesc;


typedef struct Breaklabel {
  struct Breaklabel *previous;  /* chain */
  int breaklist;
  int stacklevel;
} Breaklabel;




/*
** prototypes for recursive non-terminal functions
*/
static void body (LexState *ls, int needself, int line);
static void chunk (LexState *ls);
static void constructor (LexState *ls);
static void expr (LexState *ls, expdesc *v);
static void exp1 (LexState *ls);



static void next (LexState *ls) {
  ls->lastline = ls->linenumber;
  if (ls->lookahead.token != TK_EOS) {  /* is there a look-ahead token? */
    ls->t = ls->lookahead;  /* use this one */
    ls->lookahead.token = TK_EOS;  /* and discharge it */
  }
  else
    ls->t.token = luaX_lex(ls, &ls->t.seminfo);  /* read next token */
}


static void lookahead (LexState *ls) {
  lua_assert(ls->lookahead.token == TK_EOS);
  ls->lookahead.token = luaX_lex(ls, &ls->lookahead.seminfo);
}


static void error_expected (LexState *ls, int token) {
  l_char buff[30], t[TOKEN_LEN];
  luaX_token2str(token, t);
  sprintf(buff, l_s("`%.10s' expected"), t);
  luaK_error(ls, buff);
}


static void check (LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
  next(ls);
}


static void check_condition (LexState *ls, int c, const l_char *msg) {
  if (!c) luaK_error(ls, msg);
}


static int optional (LexState *ls, int c) {
  if (ls->t.token == c) {
    next(ls);
    return 1;
  }
  else return 0;
}


static void check_match (LexState *ls, int what, int who, int where) {
  if (ls->t.token != what) {
    if (where == ls->linenumber)
      error_expected(ls, what);
    else {
      l_char buff[70];
      l_char t_what[TOKEN_LEN], t_who[TOKEN_LEN];
      luaX_token2str(what, t_what);
      luaX_token2str(who, t_who);
      sprintf(buff, l_s("`%.10s' expected (to close `%.10s' at line %d)"),
              t_what, t_who, where);
      luaK_error(ls, buff);
    }
  }
  next(ls);
}


static int string_constant (FuncState *fs, TString *s) {
  Proto *f = fs->f;
  int c = s->u.s.constindex;
  if (c >= fs->nkstr || f->kstr[c] != s) {
    luaM_growvector(fs->L, f->kstr, fs->nkstr, f->sizekstr, TString *,
                    MAXARG_U, l_s("constant table overflow"));
    c = fs->nkstr++;
    f->kstr[c] = s;
    s->u.s.constindex = c;  /* hint for next time */
  }
  return c;
}


static void code_string (LexState *ls, TString *s) {
  luaK_kstr(ls, string_constant(ls->fs, s));
}


static TString *str_checkname (LexState *ls) {
  TString *ts;
  check_condition(ls, (ls->t.token == TK_NAME), l_s("<name> expected"));
  ts = ls->t.seminfo.ts;
  next(ls);
  return ts;
}


static int checkname (LexState *ls) {
  return string_constant(ls->fs, str_checkname(ls));
}


static int luaI_registerlocalvar (LexState *ls, TString *varname) {
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  luaM_growvector(ls->L, f->locvars, fs->nlocvars, f->sizelocvars,
                  LocVar, MAX_INT, l_s(""));
  f->locvars[fs->nlocvars].varname = varname;
  return fs->nlocvars++;
}


static void new_localvar (LexState *ls, TString *name, int n) {
  FuncState *fs = ls->fs;
  luaX_checklimit(ls, fs->nactloc+n+1, MAXLOCALS, l_s("local variables"));
  fs->actloc[fs->nactloc+n] = luaI_registerlocalvar(ls, name);
}


static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  while (nvars--)
    fs->f->locvars[fs->actloc[fs->nactloc++]].startpc = fs->pc;
}


static void removelocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  while (nvars--)
    fs->f->locvars[fs->actloc[--fs->nactloc]].endpc = fs->pc;
}


static void new_localvarstr (LexState *ls, const l_char *name, int n) {
  new_localvar(ls, luaS_new(ls->L, name), n);
}


static int search_local (LexState *ls, TString *n, expdesc *var) {
  FuncState *fs;
  int level = 0;
  for (fs=ls->fs; fs; fs=fs->prev) {
    int i;
    for (i=fs->nactloc-1; i >= 0; i--) {
      if (n == fs->f->locvars[fs->actloc[i]].varname) {
        var->k = VLOCAL;
        var->u.index = i;
        return level;
      }
    }
    level++;  /* `var' not found; check outer level */
  }
  var->k = VGLOBAL;  /* not found in any level; must be global */
  return -1;
}


static void singlevar (LexState *ls, TString *n, expdesc *var) {
  int level = search_local(ls, n, var);
  if (level >= 1)  /* neither local (0) nor global (-1)? */
    luaX_syntaxerror(ls, l_s("cannot access a variable in outer function"),
                         getstr(n));
  else if (level == -1)  /* global? */
    var->u.index = string_constant(ls->fs, n);
}


static int indexupvalue (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  int i;
  for (i=0; i<fs->f->nupvalues; i++) {
    if (fs->upvalues[i].k == v->k && fs->upvalues[i].u.index == v->u.index)
      return i;
  }
  /* new one */
  luaX_checklimit(ls, fs->f->nupvalues+1, MAXUPVALUES, l_s("upvalues"));
  fs->upvalues[fs->f->nupvalues] = *v;
  return fs->f->nupvalues++;
}


static void pushupvalue (LexState *ls, TString *n) {
  FuncState *fs = ls->fs;
  expdesc v;
  int level = search_local(ls, n, &v);
  if (level == -1) {  /* global? */
    if (fs->prev == NULL)
      luaX_syntaxerror(ls, l_s("cannot access an upvalue at top level"), getstr(n));
    v.u.index = string_constant(fs->prev, n);
  }
  else if (level != 1) {
    luaX_syntaxerror(ls,
    l_s("upvalue must be global or local to immediately outer function"), getstr(n));
  }
  luaK_code1(fs, OP_PUSHUPVALUE, indexupvalue(ls, &v));
}


static void adjust_mult_assign (LexState *ls, int nvars, int nexps) {
  FuncState *fs = ls->fs;
  int diff = nexps - nvars;
  if (nexps > 0 && luaK_lastisopen(fs)) { /* list ends in a function call */
    diff--;  /* do not count function call itself */
    if (diff <= 0) {  /* more variables than values? */
      luaK_setcallreturns(fs, -diff);  /* function call provide extra values */
      diff = 0;  /* no more difference */
    }
    else  /* more values than variables */
      luaK_setcallreturns(fs, 0);  /* call should provide no value */
  }
  /* push or pop eventual difference between list lengths */
  luaK_adjuststack(fs, diff);
}


static void code_params (LexState *ls, int nparams, short dots) {
  FuncState *fs = ls->fs;
  adjustlocalvars(ls, nparams);
  luaX_checklimit(ls, fs->nactloc, MAXPARAMS, l_s("parameters"));
  fs->f->numparams = (short)fs->nactloc;  /* `self' could be there already */
  fs->f->is_vararg = dots;
  if (dots) {
    new_localvarstr(ls, l_s("arg"), 0);
    adjustlocalvars(ls, 1);
  }
  luaK_deltastack(fs, fs->nactloc);  /* count parameters in the stack */
}


static void enterbreak (FuncState *fs, Breaklabel *bl) {
  bl->stacklevel = fs->stacklevel;
  bl->breaklist = NO_JUMP;
  bl->previous = fs->bl;
  fs->bl = bl;
}


static void leavebreak (FuncState *fs, Breaklabel *bl) {
  fs->bl = bl->previous;
  lua_assert(bl->stacklevel == fs->stacklevel);
  luaK_patchlist(fs, bl->breaklist, luaK_getlabel(fs));
}


static void pushclosure (LexState *ls, FuncState *func) {
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int i;
  for (i=0; i<func->f->nupvalues; i++)
    luaK_tostack(ls, &func->upvalues[i], 1);
  luaM_growvector(ls->L, f->kproto, fs->nkproto, f->sizekproto, Proto *,
                  MAXARG_A, l_s("constant table overflow"));
  f->kproto[fs->nkproto++] = func->f;
  luaK_code2(fs, OP_CLOSURE, fs->nkproto-1, func->f->nupvalues);
}


static void open_func (LexState *ls, FuncState *fs) {
  Proto *f = luaF_newproto(ls->L);
  fs->f = f;
  fs->prev = ls->fs;  /* linked list of funcstates */
  fs->ls = ls;
  fs->L = ls->L;
  ls->fs = fs;
  fs->pc = 0;
  fs->lasttarget = 0;
  fs->jlt = NO_JUMP;
  fs->stacklevel = 0;
  fs->nkstr = 0;
  fs->nkproto = 0;
  fs->nknum = 0;
  fs->nlineinfo = 0;
  fs->nlocvars = 0;
  fs->nactloc = 0;
  fs->lastline = 0;
  fs->bl = NULL;
  f->code = NULL;
  f->source = ls->source;
  f->maxstacksize = 0;
  f->numparams = 0;  /* default for main chunk */
  f->is_vararg = 0;  /* default for main chunk */
}


static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  luaK_code1(fs, OP_RETURN, ls->fs->nactloc);  /* final return */
  luaK_getlabel(fs);  /* close eventual list of pending jumps */
  removelocalvars(ls, fs->nactloc);
  luaM_reallocvector(L, f->code, f->sizecode, fs->pc, Instruction);
  f->sizecode = fs->pc;
  luaM_reallocvector(L, f->kstr, f->sizekstr, fs->nkstr, TString *);
  f->sizekstr = fs->nkstr;
  luaM_reallocvector(L, f->knum, f->sizeknum, fs->nknum, lua_Number);
  f->sizeknum = fs->nknum;
  luaM_reallocvector(L, f->kproto, f->sizekproto, fs->nkproto, Proto *);
  f->sizekproto = fs->nkproto;
  luaM_reallocvector(L, f->locvars, f->sizelocvars, fs->nlocvars, LocVar);
  f->sizelocvars = fs->nlocvars;
  luaM_reallocvector(L, f->lineinfo, f->sizelineinfo, fs->nlineinfo+1, int);
  f->lineinfo[fs->nlineinfo++] = MAX_INT;  /* end flag */
  f->sizelineinfo = fs->nlineinfo;
  lua_assert(luaG_checkcode(L, f));
  ls->fs = fs->prev;
  lua_assert(fs->bl == NULL);
}


Proto *luaY_parser (lua_State *L, ZIO *z) {
  struct LexState lexstate;
  struct FuncState funcstate;
  luaX_setinput(L, &lexstate, z, luaS_new(L, zname(z)));
  open_func(&lexstate, &funcstate);
  next(&lexstate);  /* read first token */
  chunk(&lexstate);
  check_condition(&lexstate, (lexstate.t.token == TK_EOS), l_s("<eof> expected"));
  close_func(&lexstate);
  lua_assert(funcstate.prev == NULL);
  lua_assert(funcstate.f->nupvalues == 0);
  return funcstate.f;
}



/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/


static int explist1 (LexState *ls) {
  /* explist1 -> expr { `,' expr } */
  int n = 1;  /* at least one expression */
  expdesc v;
  expr(ls, &v);
  while (ls->t.token == l_c(',')) {
    next(ls);  /* skip comma */
    luaK_tostack(ls, &v, 1);  /* gets only 1 value from previous expression */
    expr(ls, &v);
    n++;
  }
  luaK_tostack(ls, &v, 0);  /* keep open number of values of last expression */
  return n;
}


static void funcargs (LexState *ls, int slf) {
  FuncState *fs = ls->fs;
  int slevel = fs->stacklevel - slf - 1;  /* where is func in the stack */
  switch (ls->t.token) {
    case l_c('('): {  /* funcargs -> `(' [ explist1 ] `)' */
      int line = ls->linenumber;
      int nargs = 0;
      next(ls);
      if (ls->t.token != l_c(')'))  /* arg list not empty? */
        nargs = explist1(ls);
      check_match(ls, l_c(')'), l_c('('), line);
#ifdef LUA_COMPAT_ARGRET
      if (nargs > 0)  /* arg list is not empty? */
        luaK_setcallreturns(fs, 1);  /* last call returns only 1 value */
#else
      UNUSED(nargs);  /* to avoid warnings */
#endif
      break;
    }
    case l_c('{'): {  /* funcargs -> constructor */
      constructor(ls);
      break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
      code_string(ls, ls->t.seminfo.ts);  /* must use `seminfo' before `next' */
      next(ls);
      break;
    }
    default: {
      luaK_error(ls, l_s("function arguments expected"));
      break;
    }
  }
  fs->stacklevel = slevel;  /* call will remove function and arguments */
  luaK_code2(fs, OP_CALL, slevel, MULT_RET);
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/


static void recfield (LexState *ls) {
  /* recfield -> (NAME | `['exp1`]') = exp1 */
  switch (ls->t.token) {
    case TK_NAME: {
      luaK_kstr(ls, checkname(ls));
      break;
    }
    case l_c('['): {
      next(ls);
      exp1(ls);
      check(ls, l_c(']'));
      break;
    }
    default: luaK_error(ls, l_s("<name> or `[' expected"));
  }
  check(ls, l_c('='));
  exp1(ls);
}


static int recfields (LexState *ls) {
  /* recfields -> recfield { `,' recfield } [`,'] */
  FuncState *fs = ls->fs;
  int t = fs->stacklevel-1;  /* level of table on the stack */
  int n = 1;  /* at least one element */
  recfield(ls);
  while (ls->t.token == l_c(',') &&
         (next(ls), (ls->t.token != l_c(';') && ls->t.token != l_c('}')))) {
    if (n%RFIELDS_PER_FLUSH == 0)
      luaK_code1(fs, OP_SETMAP, t);
    recfield(ls);
    n++;
  }
  luaK_code1(fs, OP_SETMAP, t);
  return n;
}


static int listfields (LexState *ls) {
  /* listfields -> exp1 { `,' exp1 } [`,'] */
  expdesc v;
  FuncState *fs = ls->fs;
  int t = fs->stacklevel-1;  /* level of table on the stack */
  int n = 1;  /* at least one element */
  expr(ls, &v);
  while (ls->t.token == l_c(',') &&
         (next(ls), (ls->t.token != l_c(';') && ls->t.token != l_c('}')))) {
    luaK_tostack(ls, &v, 1);  /* only one value from intermediate expressions */
    luaX_checklimit(ls, n/LFIELDS_PER_FLUSH, MAXARG_A,
               l_s("`item groups' in a list initializer"));
    if (n%LFIELDS_PER_FLUSH == 0)
      luaK_code2(fs, OP_SETLIST, (n-1)/LFIELDS_PER_FLUSH, t);
    expr(ls, &v);
    n++;
  }
  luaK_tostack(ls, &v, 0);  /* allow multiple values for last expression */
  luaK_code2(fs, OP_SETLIST, (n-1)/LFIELDS_PER_FLUSH, t);
  return n;
}



static void constructor_part (LexState *ls, Constdesc *cd) {
  switch (ls->t.token) {
    case l_c(';'): case l_c('}'): {  /* constructor_part -> empty */
      cd->n = 0;
      cd->k = ls->t.token;
      break;
    }
    case TK_NAME: {  /* may be listfields or recfields */
      lookahead(ls);
      if (ls->lookahead.token != l_c('='))  /* expression? */
        goto case_default;
      /* else go through to recfields */
    }
    case l_c('['): {  /* constructor_part -> recfields */
      cd->n = recfields(ls);
      cd->k = 1;  /* record */
      break;
    }
    default: {  /* constructor_part -> listfields */
    case_default:
      cd->n = listfields(ls);
      cd->k = 0;  /* list */
      break;
    }
  }
}


static void constructor (LexState *ls) {
  /* constructor -> `{' constructor_part [`;' constructor_part] `}' */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int pc = luaK_code1(fs, OP_CREATETABLE, 0);
  int nelems;
  Constdesc cd;
  check(ls, l_c('{'));
  constructor_part(ls, &cd);
  nelems = cd.n;
  if (optional(ls, l_c(';'))) {
    Constdesc other_cd;
    constructor_part(ls, &other_cd);
    check_condition(ls, (cd.k != other_cd.k), l_s("invalid constructor syntax"));
    nelems += other_cd.n;
  }
  check_match(ls, l_c('}'), l_c('{'), line);
  luaX_checklimit(ls, nelems, MAXARG_U, l_s("elements in a table constructor"));
  SETARG_U(fs->f->code[pc], nelems);  /* set initial table size */
}

/* }====================================================================== */




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/

static void primaryexp (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs;
  switch (ls->t.token) {
    case TK_NUMBER: {
      lua_Number r = ls->t.seminfo.r;
      next(ls);
      luaK_number(fs, r);
      break;
    }
    case TK_STRING: {
      code_string(ls, ls->t.seminfo.ts);  /* must use `seminfo' before `next' */
      next(ls);
      break;
    }
    case TK_NIL: {
      luaK_adjuststack(fs, -1);
      next(ls);
      break;
    }
    case l_c('{'): {  /* constructor */
      constructor(ls);
      break;
    }
    case TK_FUNCTION: {
      next(ls);
      body(ls, 0, ls->linenumber);
      break;
    }
    case l_c('('): {
      next(ls);
      expr(ls, v);
      check(ls, l_c(')'));
      return;
    }
    case TK_NAME: {
      singlevar(ls, str_checkname(ls), v);
      return;
    }
    case l_c('%'): {
      next(ls);  /* skip `%' */
      pushupvalue(ls, str_checkname(ls));
      break;
    }
    default: {
      luaK_error(ls, l_s("unexpected symbol"));
      return;
    }
  }
  v->k = VEXP;
  v->u.l.t = v->u.l.f = NO_JUMP;
}


static void simpleexp (LexState *ls, expdesc *v) {
  /* simpleexp ->
        primaryexp { `.' NAME | `[' exp `]' | `:' NAME funcargs | funcargs } */
  primaryexp(ls, v);
  for (;;) {
    switch (ls->t.token) {
      case l_c('.'): {  /* `.' NAME */
        next(ls);
        luaK_tostack(ls, v, 1);  /* `v' must be on stack */
        luaK_kstr(ls, checkname(ls));
        v->k = VINDEXED;
        break;
      }
      case l_c('['): {  /* `[' exp1 `]' */
        next(ls);
        luaK_tostack(ls, v, 1);  /* `v' must be on stack */
        v->k = VINDEXED;
        exp1(ls);
        check(ls, l_c(']'));
        break;
      }
      case l_c(':'): {  /* `:' NAME funcargs */
        next(ls);
        luaK_tostack(ls, v, 1);  /* `v' must be on stack */
        luaK_code1(ls->fs, OP_PUSHSELF, checkname(ls));
        funcargs(ls, 1);
        v->k = VEXP;
        v->u.l.t = v->u.l.f = NO_JUMP;
        break;
      }
      case l_c('('): case TK_STRING: case l_c('{'): {  /* funcargs */
        luaK_tostack(ls, v, 1);  /* `v' must be on stack */
        funcargs(ls, 0);
        v->k = VEXP;
        v->u.l.t = v->u.l.f = NO_JUMP;
        break;
      }
      default: return;  /* should be follow... */
    }
  }
}


static UnOpr getunopr (int op) {
  switch (op) {
    case TK_NOT: return OPR_NOT;
    case l_c('-'): return OPR_MINUS;
    default: return OPR_NOUNOPR;
  }
}


static BinOpr getbinopr (int op) {
  switch (op) {
    case l_c('+'): return OPR_ADD;
    case l_c('-'): return OPR_SUB;
    case l_c('*'): return OPR_MULT;
    case l_c('/'): return OPR_DIV;
    case l_c('^'): return OPR_POW;
    case TK_CONCAT: return OPR_CONCAT;
    case TK_NE: return OPR_NE;
    case TK_EQ: return OPR_EQ;
    case l_c('<'): return OPR_LT;
    case TK_LE: return OPR_LE;
    case l_c('>'): return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_AND: return OPR_AND;
    case TK_OR: return OPR_OR;
    default: return OPR_NOBINOPR;
  }
}


static const struct {
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {  /* ORDER OPR */
   {5, 5}, {5, 5}, {6, 6}, {6, 6},  /* arithmetic */
   {9, 8}, {4, 3},                  /* power and concat (right associative) */
   {2, 2}, {2, 2},                  /* equality */
   {2, 2}, {2, 2}, {2, 2}, {2, 2},  /* order */
   {1, 1}, {1, 1}                   /* logical */
};

#define UNARY_PRIORITY	7  /* priority for unary operators */


/*
** subexpr -> (simplexep | unop subexpr) { binop subexpr }
** where `binop' is any binary operator with a priority higher than `limit'
*/
static BinOpr subexpr (LexState *ls, expdesc *v, int limit) {
  BinOpr op;
  UnOpr uop = getunopr(ls->t.token);
  if (uop != OPR_NOUNOPR) {
    next(ls);
    subexpr(ls, v, UNARY_PRIORITY);
    luaK_prefix(ls, uop, v);
  }
  else simpleexp(ls, v);
  /* expand while operators have priorities higher than `limit' */
  op = getbinopr(ls->t.token);
  while (op != OPR_NOBINOPR && (int)priority[op].left > limit) {
    expdesc v2;
    BinOpr nextop;
    next(ls);
    luaK_infix(ls, op, v);
    /* read sub-expression with higher priority */
    nextop = subexpr(ls, &v2, (int)priority[op].right);
    luaK_posfix(ls, op, v, &v2);
    op = nextop;
  }
  return op;  /* return first untreated operator */
}


static void expr (LexState *ls, expdesc *v) {
  subexpr(ls, v, -1);
}


static void exp1 (LexState *ls) {
  expdesc v;
  expr(ls, &v);
  luaK_tostack(ls, &v, 1);
}

/* }==================================================================== */


/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


static int block_follow (int token) {
  switch (token) {
    case TK_ELSE: case TK_ELSEIF: case TK_END:
    case TK_UNTIL: case TK_EOS:
      return 1;
    default: return 0;
  }
}


static void block (LexState *ls) {
  /* block -> chunk */
  FuncState *fs = ls->fs;
  int nactloc = fs->nactloc;
  chunk(ls);
  luaK_adjuststack(fs, fs->nactloc - nactloc);  /* remove local variables */
  removelocalvars(ls, fs->nactloc - nactloc);
}


static int assignment (LexState *ls, expdesc *v, int nvars) {
  int left = 0;  /* number of values left in the stack after assignment */
  luaX_checklimit(ls, nvars, MAXVARSLH, l_s("variables in a multiple assignment"));
  if (ls->t.token == l_c(',')) {  /* assignment -> `,' simpleexp assignment */
    expdesc nv;
    next(ls);
    simpleexp(ls, &nv);
    check_condition(ls, (nv.k != VEXP), l_s("syntax error"));
    left = assignment(ls, &nv, nvars+1);
  }
  else {  /* assignment -> `=' explist1 */
    int nexps;
    check(ls, l_c('='));
    nexps = explist1(ls);
    adjust_mult_assign(ls, nvars, nexps);
  }
  if (v->k != VINDEXED)
    luaK_storevar(ls, v);
  else {  /* there may be garbage between table-index and value */
    luaK_code2(ls->fs, OP_SETTABLE, left+nvars+2, 1);
    left += 2;
  }
  return left;
}


static void cond (LexState *ls, expdesc *v) {
  /* cond -> exp */
  expr(ls, v);  /* read condition */
  luaK_goiftrue(ls->fs, v, 0);
}


static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE cond DO block END */
  FuncState *fs = ls->fs;
  int while_init = luaK_getlabel(fs);
  expdesc v;
  Breaklabel bl;
  enterbreak(fs, &bl);
  next(ls);
  cond(ls, &v);
  check(ls, TK_DO);
  block(ls);
  luaK_patchlist(fs, luaK_jump(fs), while_init);
  luaK_patchlist(fs, v.u.l.f, luaK_getlabel(fs));
  check_match(ls, TK_END, TK_WHILE, line);
  leavebreak(fs, &bl);
}


static void repeatstat (LexState *ls, int line) {
  /* repeatstat -> REPEAT block UNTIL cond */
  FuncState *fs = ls->fs;
  int repeat_init = luaK_getlabel(fs);
  expdesc v;
  Breaklabel bl;
  enterbreak(fs, &bl);
  next(ls);
  block(ls);
  check_match(ls, TK_UNTIL, TK_REPEAT, line);
  cond(ls, &v);
  luaK_patchlist(fs, v.u.l.f, repeat_init);
  leavebreak(fs, &bl);
}


static void forbody (LexState *ls, int nvar, OpCode prepfor, OpCode loopfor) {
  /* forbody -> DO block END */
  FuncState *fs = ls->fs;
  int prep = luaK_code1(fs, prepfor, NO_JUMP);
  int blockinit = luaK_getlabel(fs);
  check(ls, TK_DO);
  adjustlocalvars(ls, nvar);  /* scope for control variables */
  block(ls);
  luaK_patchlist(fs, luaK_code1(fs, loopfor, NO_JUMP), blockinit);
  luaK_fixfor(fs, prep, luaK_getlabel(fs));
  removelocalvars(ls, nvar);
}


static void fornum (LexState *ls, TString *varname) {
  /* fornum -> NAME = exp1,exp1[,exp1] forbody */
  FuncState *fs = ls->fs;
  check(ls, l_c('='));
  exp1(ls);  /* initial value */
  check(ls, l_c(','));
  exp1(ls);  /* limit */
  if (optional(ls, l_c(',')))
    exp1(ls);  /* optional step */
  else
    luaK_code1(fs, OP_PUSHINT, 1);  /* default step */
  new_localvar(ls, varname, 0);
  new_localvarstr(ls, l_s("(limit)"), 1);
  new_localvarstr(ls, l_s("(step)"), 2);
  forbody(ls, 3, OP_FORPREP, OP_FORLOOP);
}


static void forlist (LexState *ls, TString *indexname) {
  /* forlist -> NAME,NAME IN exp1 forbody */
  TString *valname;
  check(ls, l_c(','));
  valname = str_checkname(ls);
  /* next test is dirty, but avoids `in' being a reserved word */
  check_condition(ls,
       (ls->t.token == TK_NAME &&
        ls->t.seminfo.ts == luaS_newliteral(ls->L, l_s("in"))),
       l_s("`in' expected"));
  next(ls);  /* skip `in' */
  exp1(ls);  /* table */
  new_localvarstr(ls, l_s("(table)"), 0);
  new_localvarstr(ls, l_s("(index)"), 1);
  new_localvar(ls, indexname, 2);
  new_localvar(ls, valname, 3);
  forbody(ls, 4, OP_LFORPREP, OP_LFORLOOP);
}


static void forstat (LexState *ls, int line) {
  /* forstat -> fornum | forlist */
  FuncState *fs = ls->fs;
  TString *varname;
  Breaklabel bl;
  enterbreak(fs, &bl);
  next(ls);  /* skip `for' */
  varname = str_checkname(ls);  /* first variable name */
  switch (ls->t.token) {
    case l_c('='): fornum(ls, varname); break;
    case l_c(','): forlist(ls, varname); break;
    default: luaK_error(ls, l_s("`=' or `,' expected"));
  }
  check_match(ls, TK_END, TK_FOR, line);
  leavebreak(fs, &bl);
}


static void test_then_block (LexState *ls, expdesc *v) {
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  next(ls);  /* skip IF or ELSEIF */
  cond(ls, v);
  check(ls, TK_THEN);
  block(ls);  /* `then' part */
}


static void ifstat (LexState *ls, int line) {
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState *fs = ls->fs;
  expdesc v;
  int escapelist = NO_JUMP;
  test_then_block(ls, &v);  /* IF cond THEN block */
  while (ls->t.token == TK_ELSEIF) {
    luaK_concat(fs, &escapelist, luaK_jump(fs));
    luaK_patchlist(fs, v.u.l.f, luaK_getlabel(fs));
    test_then_block(ls, &v);  /* ELSEIF cond THEN block */
  }
  if (ls->t.token == TK_ELSE) {
    luaK_concat(fs, &escapelist, luaK_jump(fs));
    luaK_patchlist(fs, v.u.l.f, luaK_getlabel(fs));
    next(ls);  /* skip ELSE */
    block(ls);  /* `else' part */
  }
  else
    luaK_concat(fs, &escapelist, v.u.l.f);
  luaK_patchlist(fs, escapelist, luaK_getlabel(fs));
  check_match(ls, TK_END, TK_IF, line);
}


static void localstat (LexState *ls) {
  /* stat -> LOCAL NAME {`,' NAME} [`=' explist1] */
  int nvars = 0;
  int nexps;
  do {
    next(ls);  /* skip LOCAL or `,' */
    new_localvar(ls, str_checkname(ls), nvars++);
  } while (ls->t.token == l_c(','));
  if (optional(ls, l_c('=')))
    nexps = explist1(ls);
  else
    nexps = 0;
  adjust_mult_assign(ls, nvars, nexps);
  adjustlocalvars(ls, nvars);
}


static int funcname (LexState *ls, expdesc *v) {
  /* funcname -> NAME {`.' NAME} [`:' NAME] */
  int needself = 0;
  singlevar(ls, str_checkname(ls), v);
  while (ls->t.token == l_c('.')) {
    next(ls);
    luaK_tostack(ls, v, 1);
    luaK_kstr(ls, checkname(ls));
    v->k = VINDEXED;
  }
  if (ls->t.token == l_c(':')) {
    needself = 1;
    next(ls);
    luaK_tostack(ls, v, 1);
    luaK_kstr(ls, checkname(ls));
    v->k = VINDEXED;
  }
  return needself;
}


static void funcstat (LexState *ls, int line) {
  /* funcstat -> FUNCTION funcname body */
  int needself;
  expdesc v;
  next(ls);  /* skip FUNCTION */
  needself = funcname(ls, &v);
  body(ls, needself, line);
  luaK_storevar(ls, &v);
}


static void exprstat (LexState *ls) {
  /* stat -> func | assignment */
  FuncState *fs = ls->fs;
  expdesc v;
  simpleexp(ls, &v);
  if (v.k == VEXP) {  /* stat -> func */
    check_condition(ls, luaK_lastisopen(fs), l_s("syntax error"));  /* an upvalue? */
    luaK_setcallreturns(fs, 0);  /* call statement uses no results */
  }
  else {  /* stat -> assignment */
    int left = assignment(ls, &v, 1);
    luaK_adjuststack(fs, left);  /* remove eventual garbage left on stack */
  }
}


static void retstat (LexState *ls) {
  /* stat -> RETURN explist */
  FuncState *fs = ls->fs;
  next(ls);  /* skip RETURN */
  if (!block_follow(ls->t.token) && ls->t.token != l_c(';'))
    explist1(ls);  /* optional return values */
  luaK_code1(fs, OP_RETURN, ls->fs->nactloc);
  fs->stacklevel = fs->nactloc;  /* removes all temp values */
}


static void breakstat (LexState *ls) {
  /* stat -> BREAK [NAME] */
  FuncState *fs = ls->fs;
  int currentlevel = fs->stacklevel;
  Breaklabel *bl = fs->bl;
  if (!bl)
    luaK_error(ls, l_s("no loop to break"));
  next(ls);  /* skip BREAK */
  luaK_adjuststack(fs, currentlevel - bl->stacklevel);
  luaK_concat(fs, &bl->breaklist, luaK_jump(fs));
  /* correct stack for compiler and symbolic execution */
  luaK_adjuststack(fs, bl->stacklevel - currentlevel);
}


static int statement (LexState *ls) {
  int line = ls->linenumber;  /* may be needed for error messages */
  switch (ls->t.token) {
    case TK_IF: {  /* stat -> ifstat */
      ifstat(ls, line);
      return 0;
    }
    case TK_WHILE: {  /* stat -> whilestat */
      whilestat(ls, line);
      return 0;
    }
    case TK_DO: {  /* stat -> DO block END */
      next(ls);  /* skip DO */
      block(ls);
      check_match(ls, TK_END, TK_DO, line);
      return 0;
    }
    case TK_FOR: {  /* stat -> forstat */
      forstat(ls, line);
      return 0;
    }
    case TK_REPEAT: {  /* stat -> repeatstat */
      repeatstat(ls, line);
      return 0;
    }
    case TK_FUNCTION: {
      lookahead(ls);
      if (ls->lookahead.token == '(')
        exprstat(ls);
      else
        funcstat(ls, line);  /* stat -> funcstat */
      return 0;
    }
    case TK_LOCAL: {  /* stat -> localstat */
      localstat(ls);
      return 0;
    }
    case TK_RETURN: {  /* stat -> retstat */
      retstat(ls);
      return 1;  /* must be last statement */
    }
    case TK_BREAK: {  /* stat -> breakstat */
      breakstat(ls);
      return 1;  /* must be last statement */
    }
    default: {
      exprstat(ls);
      return 0;  /* to avoid warnings */
    }
  }
}


static void parlist (LexState *ls) {
  /* parlist -> [ param { `,' param } ] */
  int nparams = 0;
  short dots = 0;
  if (ls->t.token != l_c(')')) {  /* is `parlist' not empty? */
    do {
      switch (ls->t.token) {
        case TK_DOTS: next(ls); dots = 1; break;
        case TK_NAME: new_localvar(ls, str_checkname(ls), nparams++); break;
        default: luaK_error(ls, l_s("<name> or `...' expected"));
      }
    } while (!dots && optional(ls, l_c(',')));
  }
  code_params(ls, nparams, dots);
}


static void body (LexState *ls, int needself, int line) {
  /* body ->  `(' parlist `)' chunk END */
  FuncState new_fs;
  open_func(ls, &new_fs);
  new_fs.f->lineDefined = line;
  check(ls, l_c('('));
  if (needself) {
    new_localvarstr(ls, l_s("self"), 0);
    adjustlocalvars(ls, 1);
  }
  parlist(ls);
  check(ls, l_c(')'));
  chunk(ls);
  check_match(ls, TK_END, TK_FUNCTION, line);
  close_func(ls);
  pushclosure(ls, &new_fs);
}


/* }====================================================================== */


static void chunk (LexState *ls) {
  /* chunk -> { stat [`;'] } */
  int islast = 0;
  while (!islast && !block_follow(ls->t.token)) {
    islast = statement(ls);
    optional(ls, l_c(';'));
    lua_assert(ls->fs->stacklevel == ls->fs->nactloc);
  }
}

