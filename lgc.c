/*
** $Id: lgc.c,v 2.49 2009/03/10 17:14:37 roberto Exp roberto $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#include <string.h>

#define lgc_c
#define LUA_CORE

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100


#define maskcolors	cast_byte(~(bitmask(BLACKBIT)|WHITEBITS))

#define makewhite(g,x)	\
 (gch(x)->marked = cast_byte((gch(x)->marked & maskcolors) | luaC_white(g)))

#define white2gray(x)	resetbits(gch(x)->marked, WHITEBITS)
#define black2gray(x)	resetbit(gch(x)->marked, BLACKBIT)

#define stringmark(s)	resetbits((s)->tsv.marked, WHITEBITS)


#define isfinalized(u)		testbit((u)->marked, FINALIZEDBIT)


#define markvalue(g,o) { checkconsistency(o); \
  if (valiswhite(o)) reallymarkobject(g,gcvalue(o)); }

#define markobject(g,t) { if ((t) && iswhite(obj2gco(t))) \
		reallymarkobject(g, obj2gco(t)); }


#define setthreshold(g)  (g->GCthreshold = (g->estimate/100) * g->gcpause)


static void reallymarkobject (global_State *g, GCObject *o);


/*
** {======================================================
** Generic functions
** =======================================================
*/

static void linktable (Table *h, GCObject **p) {
  h->gclist = *p;
  *p = obj2gco(h);
}


static void removeentry (Node *n) {
  lua_assert(ttisnil(gval(n)));
  if (iscollectable(gkey(n)))
    setttype(gkey(n), LUA_TDEADKEY);  /* dead key; remove it */
}


/*
** The next function tells whether a key or value can be cleared from
** a weak table. Non-collectable objects are never removed from weak
** tables. Strings behave as `values', so are never removed too. for
** other objects: if really collected, cannot keep them; for objects
** being finalized, keep them in keys, but not in values
*/
static int iscleared (const TValue *o, int iskey) {
  if (!iscollectable(o)) return 0;
  if (ttisstring(o)) {
    stringmark(rawtsvalue(o));  /* strings are `values', so are never weak */
    return 0;
  }
  return iswhite(gcvalue(o)) ||
    (ttisuserdata(o) && (!iskey && isfinalized(uvalue(o))));
}

void luaC_barrierf (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  lua_assert(g->gcstate != GCSfinalize && g->gcstate != GCSpause);
  lua_assert(ttype(gch(o)) != LUA_TTABLE);
  /* must keep invariant? */
  if (g->gcstate == GCSpropagate)
    reallymarkobject(g, v);  /* restore invariant */
  else  /* don't mind */
    makewhite(g, o);  /* mark as white just to avoid other barriers */
}


void luaC_barrierback (lua_State *L, Table *t) {
  global_State *g = G(L);
  GCObject *o = obj2gco(t);
  lua_assert(isblack(o) && !isdead(g, o));
  lua_assert(g->gcstate != GCSfinalize && g->gcstate != GCSpause);
  black2gray(o);  /* make table gray (again) */
  t->gclist = g->grayagain;
  g->grayagain = o;
}


void luaC_link (lua_State *L, GCObject *o, lu_byte tt) {
  global_State *g = G(L);
  gch(o)->marked = luaC_white(g);
  gch(o)->tt = tt;
  gch(o)->next = g->rootgc;
  g->rootgc = o;
}


void luaC_linkupval (lua_State *L, UpVal *uv) {
  global_State *g = G(L);
  GCObject *o = obj2gco(uv);
  gch(o)->next = g->rootgc;  /* link upvalue into `rootgc' list */
  g->rootgc = o;
  if (isgray(o)) {
    if (g->gcstate == GCSpropagate) {
      gray2black(o);  /* closed upvalues need barrier */
      luaC_barrier(L, uv, uv->v);
    }
    else {  /* sweep phase: sweep it (turning it into white) */
      makewhite(g, o);
      lua_assert(g->gcstate != GCSfinalize && g->gcstate != GCSpause);
    }
  }
}

/* }====================================================== */



/*
** {======================================================
** Mark functions
** =======================================================
*/

static void reallymarkobject (global_State *g, GCObject *o) {
  lua_assert(iswhite(o) && !isdead(g, o));
  white2gray(o);
  switch (gch(o)->tt) {
    case LUA_TSTRING: {
      return;
    }
    case LUA_TUSERDATA: {
      Table *mt = gco2u(o)->metatable;
      gray2black(o);  /* udata are never gray */
      markobject(g, mt);
      markobject(g, gco2u(o)->env);
      return;
    }
    case LUA_TUPVAL: {
      UpVal *uv = gco2uv(o);
      markvalue(g, uv->v);
      if (uv->v == &uv->u.value)  /* closed? */
        gray2black(o);  /* open upvalues are never black */
      return;
    }
    case LUA_TFUNCTION: {
      gco2cl(o)->c.gclist = g->gray;
      g->gray = o;
      break;
    }
    case LUA_TTABLE: {
      linktable(gco2t(o), &g->gray);
      break;
    }
    case LUA_TTHREAD: {
      gco2th(o)->gclist = g->gray;
      g->gray = o;
      break;
    }
    case LUA_TPROTO: {
      gco2p(o)->gclist = g->gray;
      g->gray = o;
      break;
    }
    default: lua_assert(0);
  }
}


static void markmt (global_State *g) {
  int i;
  for (i=0; i<NUM_TAGS; i++)
    markobject(g, g->mt[i]);
}


static void markbeingfnz (global_State *g) {
  GCObject *o;
  for (o = g->tobefnz; o != NULL; o = gch(o)->next) {
    lua_assert(testbit(gch(o)->marked, SEPARATED));
    makewhite(g, o);
    reallymarkobject(g, o);
  }
}


/* mark root set */
static void markroot (lua_State *L) {
  global_State *g = G(L);
  g->gray = NULL;
  g->grayagain = NULL;
  g->weak = g->ephemeron = g->allweak = NULL;
  markobject(g, g->mainthread);
  /* make global table be traversed before main stack */
  markvalue(g, gt(g->mainthread));
  markvalue(g, registry(L));
  markmt(g);
  markbeingfnz(g);  /* mark any finalizing object left from previous cycle */
  g->gcstate = GCSpropagate;
}


static void remarkupvals (global_State *g) {
  UpVal *uv;
  for (uv = g->uvhead.u.l.next; uv != &g->uvhead; uv = uv->u.l.next) {
    lua_assert(uv->u.l.next->u.l.prev == uv && uv->u.l.prev->u.l.next == uv);
    if (isgray(obj2gco(uv)))
      markvalue(g, uv->v);
  }
}

/* }====================================================== */


/*
** {======================================================
** Traverse functions
** =======================================================
*/

static void traverseweakvalue (global_State *g, Table *h) {
  int i = sizenode(h);
  while (i--) {
    Node *n = gnode(h, i);
    lua_assert(ttype(gkey(n)) != LUA_TDEADKEY || ttisnil(gval(n)));
    if (ttisnil(gval(n)))
      removeentry(n);  /* remove empty entries */
    else {
      lua_assert(!ttisnil(gkey(n)));
      markvalue(g, gkey(n));
    }
  }
  linktable(h, &g->weak);
}


static int traverseephemeron (global_State *g, Table *h) {
  int marked = 0;
  int hasclears = 0;
  int i = h->sizearray;
  while (i--) {  /* mark array part (numeric keys are 'strong') */
    if (valiswhite(&h->array[i])) {
      marked = 1;
      reallymarkobject(g, gcvalue(&h->array[i]));
    }
  }
  i = sizenode(h);
  while (i--) {
    Node *n = gnode(h, i);
    lua_assert(ttype(gkey(n)) != LUA_TDEADKEY || ttisnil(gval(n)));
    if (ttisnil(gval(n)))  /* entry is empty? */
      removeentry(n);  /* remove it */
    else if (valiswhite(gval(n))) {
      /* value is not marked yet */
      if (iscleared(key2tval(n), 1))  /* key is not marked (yet)? */
        hasclears = 1;  /* may have to propagate mark from key to value */
      else {  /* mark value only if key is marked */
        marked = 1;  /* some mark changed status */
        reallymarkobject(g, gcvalue(gval(n)));
      }
    }
  }
  if (hasclears)
    linktable(h, &g->ephemeron);
  else  /* nothing to propagate */
    linktable(h, &g->weak);  /* avoid convergence phase  */
  return marked;
}


static void traversestrongtable (global_State *g, Table *h) {
  int i;
  i = h->sizearray;
  while (i--)
    markvalue(g, &h->array[i]);
  i = sizenode(h);
  while (i--) {
    Node *n = gnode(h, i);
    lua_assert(ttype(gkey(n)) != LUA_TDEADKEY || ttisnil(gval(n)));
    if (ttisnil(gval(n)))
      removeentry(n);  /* remove empty entries */
    else {
      lua_assert(!ttisnil(gkey(n)));
      markvalue(g, gkey(n));
      markvalue(g, gval(n));
    }
  }
}


static void traversetable (global_State *g, Table *h) {
  const TValue *mode = gfasttm(g, h->metatable, TM_MODE);
  markobject(g, h->metatable);
  if (mode && ttisstring(mode)) {  /* is there a weak mode? */
    int weakkey = (strchr(svalue(mode), 'k') != NULL);
    int weakvalue = (strchr(svalue(mode), 'v') != NULL);
    if (weakkey || weakvalue) {  /* is really weak? */
      black2gray(obj2gco(h));  /* keep table gray */
      if (!weakkey)  /* strong keys? */
        traverseweakvalue(g, h);
      else if (!weakvalue)  /* strong values? */
        traverseephemeron(g, h);
      else
        linktable(h, &g->allweak);  /* nothing to traverse now */
      return;
    }  /* else go through */
  }
  traversestrongtable(g, h);
}


/*
** All marks are conditional because a GC may happen while the
** prototype is still being created
*/
static void traverseproto (global_State *g, Proto *f) {
  int i;
  if (f->source) stringmark(f->source);
  for (i=0; i<f->sizek; i++)  /* mark literals */
    markvalue(g, &f->k[i]);
  for (i=0; i<f->sizeupvalues; i++) {  /* mark upvalue names */
    if (f->upvalues[i])
      stringmark(f->upvalues[i]);
  }
  for (i=0; i<f->sizep; i++)  /* mark nested protos */
    markobject(g, f->p[i]);
  for (i=0; i<f->sizelocvars; i++) {  /* mark local-variable names */
    if (f->locvars[i].varname)
      stringmark(f->locvars[i].varname);
  }
}



static void traverseclosure (global_State *g, Closure *cl) {
  markobject(g, cl->c.env);
  if (cl->c.isC) {
    int i;
    for (i=0; i<cl->c.nupvalues; i++)  /* mark its upvalues */
      markvalue(g, &cl->c.upvalue[i]);
  }
  else {
    int i;
    lua_assert(cl->l.nupvalues == cl->l.p->nups);
    markobject(g, cl->l.p);
    for (i=0; i<cl->l.nupvalues; i++)  /* mark its upvalues */
      markobject(g, cl->l.upvals[i]);
  }
}


static void checkstacksize (lua_State *L, StkId max) {
  /* should not change the stack during an emergency gc cycle */
  if (G(L)->gckind == KGC_EMERGENCY)
    return;  /* do not touch the stack */
  else {
    int s_used = cast_int(max - L->stack) + 1;  /* part of stack in use */
    if (2*s_used < (L->stacksize - EXTRA_STACK))
      luaD_reallocstack(L, 2*s_used);
  }
}


static void traversestack (global_State *g, lua_State *L) {
  StkId o, lim;
  CallInfo *ci;
  if (L->stack == NULL)
    return;  /* stack not completely built yet */
  markvalue(g, gt(L));
  lim = L->top;
  for (ci = L->ci; ci != NULL; ci = ci->previous) {
    lua_assert(ci->top <= L->stack_last);
    if (lim < ci->top) lim = ci->top;
  }
  for (o = L->stack; o < L->top; o++)
    markvalue(g, o);
  for (; o <= lim; o++)
    setnilvalue(o);
  checkstacksize(L, lim);
}


/*
** traverse one gray object, turning it to black.
** Returns `quantity' traversed.
*/
static l_mem propagatemark (global_State *g) {
  GCObject *o = g->gray;
  lua_assert(isgray(o));
  gray2black(o);
  switch (gch(o)->tt) {
    case LUA_TTABLE: {
      Table *h = gco2t(o);
      g->gray = h->gclist;
      traversetable(g, h);
      return sizeof(Table) + sizeof(TValue) * h->sizearray +
                             sizeof(Node) * sizenode(h);
    }
    case LUA_TFUNCTION: {
      Closure *cl = gco2cl(o);
      g->gray = cl->c.gclist;
      traverseclosure(g, cl);
      return (cl->c.isC) ? sizeCclosure(cl->c.nupvalues) :
                           sizeLclosure(cl->l.nupvalues);
    }
    case LUA_TTHREAD: {
      lua_State *th = gco2th(o);
      g->gray = th->gclist;
      th->gclist = g->grayagain;
      g->grayagain = o;
      black2gray(o);
      traversestack(g, th);
      return sizeof(lua_State) + sizeof(TValue) * th->stacksize +
                                 sizeof(CallInfo) * th->nci;
    }
    case LUA_TPROTO: {
      Proto *p = gco2p(o);
      g->gray = p->gclist;
      traverseproto(g, p);
      return sizeof(Proto) + sizeof(Instruction) * p->sizecode +
                             sizeof(Proto *) * p->sizep +
                             sizeof(TValue) * p->sizek +
                             sizeof(int) * p->sizelineinfo +
                             sizeof(LocVar) * p->sizelocvars +
                             sizeof(TString *) * p->sizeupvalues;
    }
    default: lua_assert(0); return 0;
  }
}


static size_t propagateall (global_State *g) {
  size_t m = 0;
  while (g->gray) m += propagatemark(g);
  return m;
}


static void traverselistofgrays (global_State *g, GCObject **l) {
  lua_assert(g->gray == NULL);  /* no grays left */
  g->gray = *l;  /* now 'l' is new gray list */
  *l = NULL;
  propagateall(g);
}


static void convergeephemerons (global_State *g) {
  int changed;
  do {
    GCObject *w;
    GCObject *next = g->ephemeron;
    g->ephemeron = NULL;
    changed = 0;
    while ((w = next) != NULL) {
      next = gco2t(w)->gclist;
      if (traverseephemeron(g, gco2t(w))) {
        changed = 1;
        propagateall(g);
      }
    }
  } while (changed);
}

/* }====================================================== */


/*
** {======================================================
** Sweep Functions
** =======================================================
*/

/* clear collected entries from weaktables */
static void cleartable (GCObject *l) {
  while (l) {
    Table *h = gco2t(l);
    int i = h->sizearray;
    while (i--) {
      TValue *o = &h->array[i];
      if (iscleared(o, 0))  /* value was collected? */
        setnilvalue(o);  /* remove value */
    }
    i = sizenode(h);
    while (i--) {
      Node *n = gnode(h, i);
      if (!ttisnil(gval(n)) &&  /* non-empty entry? */
          (iscleared(key2tval(n), 1) || iscleared(gval(n), 0))) {
        setnilvalue(gval(n));  /* remove value ... */
        removeentry(n);  /* remove entry from table */
      }
    }
    l = h->gclist;
  }
}


static void freeobj (lua_State *L, GCObject *o) {
  switch (gch(o)->tt) {
    case LUA_TPROTO: luaF_freeproto(L, gco2p(o)); break;
    case LUA_TFUNCTION: luaF_freeclosure(L, gco2cl(o)); break;
    case LUA_TUPVAL: luaF_freeupval(L, gco2uv(o)); break;
    case LUA_TTABLE: luaH_free(L, gco2t(o)); break;
    case LUA_TTHREAD: luaE_freethread(L, gco2th(o)); break;
    case LUA_TUSERDATA: luaM_freemem(L, o, sizeudata(gco2u(o))); break;
    case LUA_TSTRING: {
      G(L)->strt.nuse--;
      luaM_freemem(L, o, sizestring(gco2ts(o)));
      break;
    }
    default: lua_assert(0);
  }
}


#define sweepwholelist(L,p)	sweeplist(L,p,MAX_LUMEM)


static GCObject **sweeplist (lua_State *L, GCObject **p, lu_mem count) {
  GCObject *curr;
  global_State *g = G(L);
  int deadmask = otherwhite(g);
  while ((curr = *p) != NULL && count-- > 0) {
    if (ttisthread(gch(curr))) {
      lua_State *L1 = gco2th(curr);
      sweepwholelist(L, &L1->openupval);  /* sweep open upvalues */
      luaE_freeCI(L1);  /* free extra CallInfo slots */
    }
    if ((gch(curr)->marked ^ WHITEBITS) & deadmask) {  /* not dead? */
      lua_assert(!isdead(g, curr) || testbit(gch(curr)->marked, FIXEDBIT));
      makewhite(g, curr);  /* make it white (for next cycle) */
      p = &gch(curr)->next;
    }
    else {  /* must erase `curr' */
      lua_assert(isdead(g, curr) || deadmask == bitmask(SFIXEDBIT));
      *p = gch(curr)->next;  /* remove 'curr' from list */
      freeobj(L, curr);
    }
  }
  return p;
}

/* }====================================================== */


/*
** {======================================================
** Finalization
** =======================================================
*/

static void checkSizes (lua_State *L) {
  global_State *g = G(L);
  if (g->strt.nuse < cast(lu_int32, g->strt.size))
    luaS_resize(L, 1 << luaO_ceillog2(g->strt.nuse));
  luaZ_freebuffer(L, &g->buff);
}


static Udata *udata2finalize (global_State *g) {
  GCObject *o = g->tobefnz;  /* get first element */
  g->tobefnz = gch(o)->next;  /* remove it from 'tobefnz' list */
  gch(o)->next = g->rootgc;  /* return it to `root' list */
  g->rootgc = o;
  lua_assert(isfinalized(gch(o)));
  resetbit(gch(o)->marked, SEPARATED);  /* mark it as such */
  makewhite(g, o);
  return rawgco2u(o);
}


static void dothecall (lua_State *L, void *ud) {
  UNUSED(ud);
  luaD_call(L, L->top - 2, 0, 0);
}


static void GCTM (lua_State *L) {
  global_State *g = G(L);
  Udata *udata = udata2finalize(g);
  const TValue *tm = gfasttm(g, udata->uv.metatable, TM_GC);
  if (tm != NULL && ttisfunction(tm)) {
    lu_byte oldah = L->allowhook;
    lu_mem oldt = g->GCthreshold;
    L->allowhook = 0;  /* stop debug hooks during GC tag method */
    g->GCthreshold = 2*g->totalbytes;  /* avoid GC steps */
    setobj2s(L, L->top, tm);
    setuvalue(L, L->top+1, udata);
    L->top += 2;
    luaD_pcall(L, dothecall, NULL, savestack(L, L->top - 2), 0);
    L->allowhook = oldah;  /* restore hooks */
    g->GCthreshold = oldt;  /* restore threshold */
  }
}


/*
** Call all GC tag methods
*/
void luaC_callAllGCTM (lua_State *L) {
  while (G(L)->tobefnz) GCTM(L);
}


/* move 'dead' udata that need finalization to list 'tobefnz' */
size_t luaC_separateudata (lua_State *L, int all) {
  global_State *g = G(L);
  size_t deadmem = 0;  /* total size of all objects to be finalized */
  GCObject **p = &g->mainthread->next;
  GCObject *curr;
  GCObject **lastnext = &g->tobefnz;
  /* find last 'next' field in 'tobefnz' list (to insert elements in its end) */
  while (*lastnext != NULL) lastnext = &gch(*lastnext)->next;
  while ((curr = *p) != NULL) {  /* traverse all finalizable objects */
    lua_assert(ttisuserdata(gch(curr)) && !isfinalized(gco2u(curr)));
    lua_assert(testbit(gch(curr)->marked, SEPARATED));
    if (!(all || iswhite(curr)))  /* not being collected? */
      p = &gch(curr)->next;  /* don't bother with it */
    else {
      l_setbit(gch(curr)->marked, FINALIZEDBIT); /* won't be finalized again */
      deadmem += sizeudata(gco2u(curr));
      *p = gch(curr)->next;  /* remove 'curr' from 'rootgc' list */
      /* link 'curr' at the end of 'tobefnz' list */
      gch(curr)->next = *lastnext;
      *lastnext = curr;
      lastnext = &gch(curr)->next;
    }
  }
  return deadmem;
}


void luaC_checkfinalizer (lua_State *L, Udata *u) {
  global_State *g = G(L);
  if (testbit(u->uv.marked, SEPARATED) || /* userdata is already separated... */
      isfinalized(&u->uv) ||                        /* ... or is finalized... */
      gfasttm(g, u->uv.metatable, TM_GC) == NULL)  /* or has no finalization? */
    return;  /* nothing to be done */
  else {  /* move 'u' to 2nd part of root list */
    GCObject **p;
    for (p = &g->rootgc; *p != obj2gco(u); p = &gch(*p)->next)
      lua_assert(*p != obj2gco(g->mainthread));  /* 'u' must be in this list */
    *p = u->uv.next;  /* remove 'u' from root list */
    u->uv.next = g->mainthread->next;  /* re-link it in list */
    g->mainthread->next = obj2gco(u);
    l_setbit(u->uv.marked, SEPARATED);  /* mark it as such */
  }
}

/* }====================================================== */


/*
** {======================================================
** GC control
** =======================================================
*/

void luaC_freeall (lua_State *L) {
  global_State *g = G(L);
  int i;
  lua_assert(g->tobefnz == NULL);
  /* mask to collect all elements */
  g->currentwhite = WHITEBITS | bitmask(SFIXEDBIT);
  sweepwholelist(L, &g->rootgc);
  lua_assert(g->rootgc == obj2gco(g->mainthread));
  lua_assert(g->mainthread->next == NULL);
  for (i = 0; i < g->strt.size; i++)  /* free all string lists */
    sweepwholelist(L, &g->strt.hash[i]);
  lua_assert(g->strt.nuse == 0);
}


static void atomic (lua_State *L) {
  global_State *g = G(L);
  size_t udsize;  /* total size of userdata to be finalized */
  /* remark occasional upvalues of (maybe) dead threads */
  remarkupvals(g);
  /* traverse objects cautch by write barrier and by 'remarkupvals' */
  propagateall(g);
  /* remark weak tables */
  g->gray = g->weak;
  g->weak = NULL;
  lua_assert(!iswhite(obj2gco(g->mainthread)));
  markobject(g, L);  /* mark running thread */
  markmt(g);  /* mark basic metatables (again) */
  propagateall(g);
  traverselistofgrays(g, &g->ephemeron);  /* remark ephemeron tables */
  traverselistofgrays(g, &g->grayagain);  /* remark gray again */
  convergeephemerons(g);
  udsize = luaC_separateudata(L, 0);  /* separate userdata to be finalized */
  markbeingfnz(g);  /* mark userdata that will be finalized */
  udsize += propagateall(g);  /* remark, to propagate `preserveness' */
  convergeephemerons(g);
  /* remove collected objects from weak tables */
  cleartable(g->weak);
  cleartable(g->ephemeron);
  cleartable(g->allweak);
  /* flip current white */
  g->currentwhite = cast_byte(otherwhite(g));
  g->sweepstrgc = 0;
  g->gcstate = GCSsweepstring;
  g->estimate = g->totalbytes - udsize;  /* first estimate */
}


#define correctestimate(g,s)  {lu_mem old = g->totalbytes; s; \
          lua_assert(old >= g->totalbytes); g->estimate -= old - g->totalbytes;}


static l_mem singlestep (lua_State *L) {
  global_State *g = G(L);
  /*lua_checkmemory(L);*/
  switch (g->gcstate) {
    case GCSpause: {
      markroot(L);  /* start a new collection */
      return 0;
    }
    case GCSpropagate: {
      if (g->gray)
        return propagatemark(g);
      else {  /* no more `gray' objects */
        atomic(L);  /* finish mark phase */
        return 0;
      }
    }
    case GCSsweepstring: {
      correctestimate(g, sweepwholelist(L, &g->strt.hash[g->sweepstrgc++]));
      if (g->sweepstrgc >= g->strt.size) {  /* nothing more to sweep? */
        g->sweepgc = &g->rootgc;
        g->gcstate = GCSsweep;  /* sweep all other objects */
      }
      return GCSWEEPCOST;
    }
    case GCSsweep: {
      correctestimate(g, g->sweepgc = sweeplist(L, g->sweepgc, GCSWEEPMAX));
      if (*g->sweepgc == NULL)  /* nothing more to sweep? */
        g->gcstate = GCSfinalize;  /* end sweep phase */
      return GCSWEEPMAX*GCSWEEPCOST;
    }
    case GCSfinalize: {
      if (g->tobefnz) {
        GCTM(L);
        if (g->estimate > GCFINALIZECOST)
          g->estimate -= GCFINALIZECOST;
        return GCFINALIZECOST;
      }
      else {
        correctestimate(g, checkSizes(L));
        g->gcstate = GCSpause;  /* end collection */
        g->gcdept = 0;
        return 0;
      }
    }
    default: lua_assert(0); return 0;
  }
}


void luaC_step (lua_State *L) {
  global_State *g = G(L);
  l_mem lim = (GCSTEPSIZE/100) * g->gcstepmul;
  lua_assert(g->gckind == KGC_NORMAL);
  if (lim == 0)
    lim = (MAX_LUMEM-1)/2;  /* no limit */
  g->gcdept += g->totalbytes - g->GCthreshold;
  do {
    lim -= singlestep(L);
    if (g->gcstate == GCSpause)
      break;
  } while (lim > 0);
  if (g->gcstate != GCSpause) {
    if (g->gcdept < GCSTEPSIZE)
      g->GCthreshold = g->totalbytes + GCSTEPSIZE;  /* - lim/g->gcstepmul;*/
    else {
      g->gcdept -= GCSTEPSIZE;
      g->GCthreshold = g->totalbytes;
    }
  }
  else {
    lua_assert(g->totalbytes >= g->estimate);
    setthreshold(g);
  }
}


void luaC_fullgc (lua_State *L, int isemergency) {
  global_State *g = G(L);
  lua_assert(g->gckind == KGC_NORMAL);
  g->gckind = isemergency ? KGC_EMERGENCY : KGC_FORCED;
  if (g->gcstate <= GCSpropagate) {
    /* reset other collector lists */
    g->gray = NULL;
    g->grayagain = NULL;
    g->weak = g->ephemeron = g->allweak = NULL;
    g->sweepstrgc = 0;
    g->gcstate = GCSsweepstring;
  }
  lua_assert(g->gcstate != GCSpause && g->gcstate != GCSpropagate);
  /* finish any pending sweep phase */
  while (g->gcstate != GCSfinalize) {
    lua_assert(issweep(g));
    singlestep(L);
  }
  markroot(L);
  /* run collector up to finalizers */
  while (g->gcstate != GCSfinalize)
    singlestep(L);
  g->gckind = KGC_NORMAL;
  if (!isemergency) {  /* do not run finalizers during emergency GC */
    while (g->gcstate != GCSpause)
      singlestep(L);
  }
  setthreshold(g);
}

/* }====================================================== */


