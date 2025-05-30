/*
 * Portions Copyright (c) 2016 Kungliga Tekniska Högskolan
 * (Royal Institute of Technology, Stockholm, Sweden).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>
#include <float.h>

#include "jv_alloc.h"
#include "jv.h"
#include "jv_unicode.h"
#include "util.h"

/*
 * Internal refcounting helpers
 */

typedef struct jv_refcnt {
  int count;
} jv_refcnt;

static const jv_refcnt JV_REFCNT_INIT = {1};

static void jvp_refcnt_inc(jv_refcnt* c) {
  c->count++;
}

static int jvp_refcnt_dec(jv_refcnt* c) {
  c->count--;
  return c->count == 0;
}

static int jvp_refcnt_unshared(jv_refcnt* c) {
  assert(c->count > 0);
  return c->count == 1;
}

#define KIND_MASK   0xF
#define PFLAGS_MASK 0xF0
#define PTYPE_MASK  0x70

typedef enum {
  JVP_PAYLOAD_NONE = 0,
  JVP_PAYLOAD_ALLOCATED = 0x80,
} payload_flags;

#define JVP_MAKE_PFLAGS(ptype, allocated) ((((ptype) << 4) & PTYPE_MASK) | ((allocated) ? JVP_PAYLOAD_ALLOCATED : 0))
#define JVP_MAKE_FLAGS(kind, pflags) ((kind & KIND_MASK) | (pflags & PFLAGS_MASK))

#define JVP_FLAGS(j)  ((j).kind_flags)
#define JVP_KIND(j)   (JVP_FLAGS(j) & KIND_MASK)

#define JVP_HAS_FLAGS(j, flags) (JVP_FLAGS(j) == flags)
#define JVP_HAS_KIND(j, kind)   (JVP_KIND(j) == kind)

#define JVP_IS_ALLOCATED(j) (j.kind_flags & JVP_PAYLOAD_ALLOCATED)

#define JVP_FLAGS_NULL      JVP_MAKE_FLAGS(JV_KIND_NULL, JVP_PAYLOAD_NONE)
#define JVP_FLAGS_INVALID   JVP_MAKE_FLAGS(JV_KIND_INVALID, JVP_PAYLOAD_NONE)
#define JVP_FLAGS_FALSE     JVP_MAKE_FLAGS(JV_KIND_FALSE, JVP_PAYLOAD_NONE)
#define JVP_FLAGS_TRUE      JVP_MAKE_FLAGS(JV_KIND_TRUE, JVP_PAYLOAD_NONE)

jv_kind jv_get_kind(jv x) {
  return JVP_KIND(x);
}

const char* jv_kind_name(jv_kind k) {
  switch (k) {
  case JV_KIND_INVALID: return "<invalid>";
  case JV_KIND_NULL:    return "null";
  case JV_KIND_FALSE:   return "boolean";
  case JV_KIND_TRUE:    return "boolean";
  case JV_KIND_NUMBER:  return "number";
  case JV_KIND_STRING:  return "string";
  case JV_KIND_ARRAY:   return "array";
  case JV_KIND_OBJECT:  return "object";
  }
  assert(0 && "invalid kind");
  return "<unknown>";
}

const jv JV_NULL = {JVP_FLAGS_NULL, 0, 0, 0, {0}};
const jv JV_INVALID = {JVP_FLAGS_INVALID, 0, 0, 0, {0}};
const jv JV_FALSE = {JVP_FLAGS_FALSE, 0, 0, 0, {0}};
const jv JV_TRUE = {JVP_FLAGS_TRUE, 0, 0, 0, {0}};

jv jv_true(void) {
  return JV_TRUE;
}

jv jv_false(void) {
  return JV_FALSE;
}

jv jv_null(void) {
  return JV_NULL;
}

jv jv_bool(int x) {
  return x ? JV_TRUE : JV_FALSE;
}

/*
 * Invalid objects, with optional error messages
 */

#define JVP_FLAGS_INVALID_MSG   JVP_MAKE_FLAGS(JV_KIND_INVALID, JVP_PAYLOAD_ALLOCATED)

typedef struct {
  jv_refcnt refcnt;
  jv errmsg;
} jvp_invalid;

jv jv_invalid_with_msg(jv err) {
  jvp_invalid* i = jv_mem_alloc(sizeof(jvp_invalid));
  i->refcnt = JV_REFCNT_INIT;
  i->errmsg = err;

  jv x = {JVP_FLAGS_INVALID_MSG, 0, 0, 0, {&i->refcnt}};
  return x;
}

jv jv_invalid(void) {
  return JV_INVALID;
}

jv jv_invalid_get_msg(jv inv) {
  assert(JVP_HAS_KIND(inv, JV_KIND_INVALID));

  jv x;
  if (JVP_HAS_FLAGS(inv, JVP_FLAGS_INVALID_MSG)) {
    x = jv_copy(((jvp_invalid*)inv.u.ptr)->errmsg);
  }
  else {
    x = jv_null();
  }

  jv_free(inv);
  return x;
}

int jv_invalid_has_msg(jv inv) {
  assert(JVP_HAS_KIND(inv, JV_KIND_INVALID));
  int r = JVP_HAS_FLAGS(inv, JVP_FLAGS_INVALID_MSG);
  jv_free(inv);
  return r;
}

static void jvp_invalid_free(jv x) {
  assert(JVP_HAS_KIND(x, JV_KIND_INVALID));
  if (JVP_HAS_FLAGS(x, JVP_FLAGS_INVALID_MSG) && jvp_refcnt_dec(x.u.ptr)) {
    jv_free(((jvp_invalid*)x.u.ptr)->errmsg);
    jv_mem_free(x.u.ptr);
  }
}

/*
 * Numbers
 */

#ifdef USE_DECNUM
#include "jv_dtoa.h"
#include "jv_dtoa_tsd.h"

// we will manage the space for the struct
#define DECNUMDIGITS 1
#include "decNumber/decNumber.h"

enum {
  JVP_NUMBER_NATIVE = 0,
  JVP_NUMBER_DECIMAL = 1
};

#define JVP_FLAGS_NUMBER_NATIVE       JVP_MAKE_FLAGS(JV_KIND_NUMBER, JVP_MAKE_PFLAGS(JVP_NUMBER_NATIVE, 0))
#define JVP_FLAGS_NUMBER_LITERAL      JVP_MAKE_FLAGS(JV_KIND_NUMBER, JVP_MAKE_PFLAGS(JVP_NUMBER_DECIMAL, 1))

// the decimal precision of binary double
#define DEC_NUMBER_DOUBLE_PRECISION   (17)
#define DEC_NUMBER_STRING_GUARD       (14)
#define DEC_NUMBER_DOUBLE_EXTRA_UNITS ((DEC_NUMBER_DOUBLE_PRECISION - DECNUMDIGITS + DECDPUN - 1)/DECDPUN)

#include "jv_thread.h"
#ifdef WIN32
#ifndef __MINGW32__
/* Copied from Heimdal: thread-specific keys; see lib/base/dll.c in Heimdal */

/*
 * This is an implementation of thread-specific storage with
 * destructors.  WIN32 doesn't quite have this.  Instead it has
 * DllMain(), an entry point in every DLL that gets called to notify the
 * DLL of thread/process "attach"/"detach" events.
 *
 * We use __thread (or __declspec(thread)) for the thread-local itself
 * and DllMain() DLL_THREAD_DETACH events to drive destruction of
 * thread-local values.
 *
 * When building in maintainer mode on non-Windows pthread systems this
 * uses a single pthread key instead to implement multiple keys.  This
 * keeps the code from rotting when modified by non-Windows developers.
 */

/* Logical array of keys that grows lock-lessly */
typedef struct tls_keys tls_keys;
struct tls_keys {
    void (**keys_dtors)(void *);    /* array of destructors         */
    size_t keys_start_idx;          /* index of first destructor    */
    size_t keys_num;
    tls_keys *keys_next;
};

/*
 * Well, not quite locklessly.  We need synchronization primitives to do
 * this locklessly.  An atomic CAS will do.
 */
static pthread_mutex_t tls_key_defs_lock = PTHREAD_MUTEX_INITIALIZER;
static tls_keys *tls_key_defs;

/* Logical array of values (per-thread; no locking needed here) */
struct tls_values {
    void **values; /* realloc()ed */
    size_t values_num;
};

#ifdef _MSC_VER
static __declspec(thread) struct nomem_handler nomem_handler;
#else
static __thread struct tls_values values;
#endif

#define DEAD_KEY ((void *)8)

static void
w32_service_thread_detach(void *unused)
{
    tls_keys *key_defs;
    void (*dtor)(void*);
    size_t i;

    pthread_mutex_lock(&tls_key_defs_lock);
    key_defs = tls_key_defs;
    pthread_mutex_unlock(&tls_key_defs_lock);

    if (key_defs == NULL)
        return;

    for (i = 0; i < values.values_num; i++) {
        assert(i >= key_defs->keys_start_idx);
        if (i >= key_defs->keys_start_idx + key_defs->keys_num) {
            pthread_mutex_lock(&tls_key_defs_lock);
            key_defs = key_defs->keys_next;
            pthread_mutex_unlock(&tls_key_defs_lock);

            assert(key_defs != NULL);
            assert(i >= key_defs->keys_start_idx);
            assert(i < key_defs->keys_start_idx + key_defs->keys_num);
        }
        dtor = key_defs->keys_dtors[i - key_defs->keys_start_idx];
        if (values.values[i] != NULL && dtor != NULL && dtor != DEAD_KEY)
            dtor(values.values[i]);
        values.values[i] = NULL;
    }
}

extern void jv_tsd_dtoa_ctx_init();
extern void jv_tsd_dtoa_ctx_fini();
void jv_tsd_dec_ctx_fini();
void jv_tsd_dec_ctx_init();

BOOL WINAPI DllMain(HINSTANCE hinstDLL,
                    DWORD fdwReason,
                    LPVOID lpvReserved)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
	/*create_pt_key();*/
	jv_tsd_dtoa_ctx_init();
	jv_tsd_dec_ctx_init();
	return TRUE;
    case DLL_PROCESS_DETACH:
	jv_tsd_dtoa_ctx_fini();
	jv_tsd_dec_ctx_fini();
	return TRUE;
    case DLL_THREAD_ATTACH: return 0;
    case DLL_THREAD_DETACH:
        w32_service_thread_detach(NULL);
        return TRUE;
    default: return TRUE;
    }
}

int
pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
    tls_keys *key_defs, *new_key_defs;
    size_t i, k;
    int ret = ENOMEM;

    pthread_mutex_lock(&tls_key_defs_lock);
    if (tls_key_defs == NULL) {
        /* First key */
        new_key_defs = calloc(1, sizeof(*new_key_defs));
        if (new_key_defs == NULL) {
            pthread_mutex_unlock(&tls_key_defs_lock);
            return ENOMEM;
        }
        new_key_defs->keys_num = 8;
        new_key_defs->keys_dtors = calloc(new_key_defs->keys_num,
                                          sizeof(*new_key_defs->keys_dtors));
        if (new_key_defs->keys_dtors == NULL) {
            pthread_mutex_unlock(&tls_key_defs_lock);
            free(new_key_defs);
            return ENOMEM;
        }
        tls_key_defs = new_key_defs;
        new_key_defs->keys_dtors[0] = dtor;
        for (i = 1; i < new_key_defs->keys_num; i++)
            new_key_defs->keys_dtors[i] = NULL;
        pthread_mutex_unlock(&tls_key_defs_lock);
        return 0;
    }

    for (key_defs = tls_key_defs;
         key_defs != NULL;
         key_defs = key_defs->keys_next) {
        k = key_defs->keys_start_idx;
        for (i = 0; i < key_defs->keys_num; i++, k++) {
            if (key_defs->keys_dtors[i] == NULL) {
                /* Found free slot; use it */
                key_defs->keys_dtors[i] = dtor;
                *key = k;
                pthread_mutex_unlock(&tls_key_defs_lock);
                return 0;
            }
        }
        if (key_defs->keys_next != NULL)
            continue;

        /* Grow the registration array */
        /* XXX DRY */
        new_key_defs = calloc(1, sizeof(*new_key_defs));
        if (new_key_defs == NULL)
            break;

        new_key_defs->keys_dtors =
            calloc(key_defs->keys_num + key_defs->keys_num / 2,
                   sizeof(*new_key_defs->keys_dtors));
        if (new_key_defs->keys_dtors == NULL) {
            free(new_key_defs);
            break;
        }
        new_key_defs->keys_start_idx = key_defs->keys_start_idx +
            key_defs->keys_num;
        new_key_defs->keys_num = key_defs->keys_num + key_defs->keys_num / 2;
        new_key_defs->keys_dtors[i] = dtor;
        for (i = 1; i < new_key_defs->keys_num; i++)
            new_key_defs->keys_dtors[i] = NULL;
        key_defs->keys_next = new_key_defs;
        ret = 0;
        break;
    }
    pthread_mutex_unlock(&tls_key_defs_lock);
    return ret;
}

static void
key_lookup(pthread_key_t key, tls_keys **kd,
           size_t *dtor_idx, void (**dtor)(void *))
{
    tls_keys *key_defs;

    if (kd != NULL)
        *kd = NULL;
    if (dtor_idx != NULL)
        *dtor_idx = 0;
    if (dtor != NULL)
        *dtor = NULL;

    pthread_mutex_lock(&tls_key_defs_lock);
    key_defs = tls_key_defs;
    pthread_mutex_unlock(&tls_key_defs_lock);

    while (key_defs != NULL) {
        if (key >= key_defs->keys_start_idx &&
            key < key_defs->keys_start_idx + key_defs->keys_num) {
            if (kd != NULL)
                *kd = key_defs;
            if (dtor_idx != NULL)
                *dtor_idx = key - key_defs->keys_start_idx;
            if (dtor != NULL)
                *dtor = key_defs->keys_dtors[key - key_defs->keys_start_idx];
            return;
        }

        pthread_mutex_lock(&tls_key_defs_lock);
        key_defs = key_defs->keys_next;
        pthread_mutex_unlock(&tls_key_defs_lock);
        assert(key_defs != NULL);
        assert(key >= key_defs->keys_start_idx);
    }
}

int
pthread_setspecific(pthread_key_t key, void *value)
{
    void **new_values;
    size_t new_num;
    void (*dtor)(void *);
    size_t i;

    key_lookup(key, NULL, NULL, &dtor);
    if (dtor == NULL)
        return EINVAL;

    if (key >= values.values_num) {
        if (values.values_num == 0) {
            values.values = NULL;
            new_num = 8;
        } else {
            new_num = (values.values_num + values.values_num / 2);
        }
        new_values = realloc(values.values, sizeof(void *) * new_num);
        if (new_values == NULL)
            return ENOMEM;
        for (i = values.values_num; i < new_num; i++)
            new_values[i] = NULL;
        values.values = new_values;
        values.values_num = new_num;
    }

    assert(key < values.values_num);

    if (values.values[key] != NULL && dtor != NULL && dtor != DEAD_KEY)
        dtor(values.values[key]);

    values.values[key] = value;
    return 0;
}

void *
pthread_getspecific(pthread_key_t key)
{
    if (key >= values.values_num)
        return NULL;
    return values.values[key];
}
#else
#include <pthread.h>
#endif
#else
#include <pthread.h>
#endif

static pthread_key_t dec_ctx_key;
static pthread_once_t dec_ctx_once = PTHREAD_ONCE_INIT;

#define DEC_CONTEXT() tsd_dec_ctx_get(&dec_ctx_key)

// atexit finalizer to clean up the tsd dec contexts if main() exits
// without having called pthread_exit()
void jv_tsd_dec_ctx_fini(void) {
  jv_mem_free(pthread_getspecific(dec_ctx_key));
  pthread_setspecific(dec_ctx_key, NULL);
}

void jv_tsd_dec_ctx_init(void) {
  if (pthread_key_create(&dec_ctx_key, jv_mem_free) != 0) {
    fprintf(stderr, "error: cannot create thread specific key");
    abort();
  }
  atexit(jv_tsd_dec_ctx_fini);
}

static decContext* tsd_dec_ctx_get(pthread_key_t *key) {
  pthread_once(&dec_ctx_once, jv_tsd_dec_ctx_init); // cannot fail
  decContext *ctx = (decContext*)pthread_getspecific(*key);
  if (ctx) {
    return ctx;
  }

  ctx = malloc(sizeof(decContext));
  if (ctx) {
    if (key == &dec_ctx_key)
    {
      decContextDefault(ctx, DEC_INIT_BASE);
      // make sure (Int)D2U(rhs->exponent-lhs->exponent) does not overflow
      ctx->digits = MIN(DEC_MAX_DIGITS,
          INT32_MAX - (DECDPUN - 1) - (ctx->emax - ctx->emin - 1));
      ctx->traps = 0; /*no errors*/
    }
    if (pthread_setspecific(*key, ctx) != 0) {
      fprintf(stderr, "error: cannot store thread specific data");
      abort();
    }
  }
  return ctx;
}

typedef struct {
  jv_refcnt refcnt;
  double num_double;
  char * literal_data;
  decNumber num_decimal; // must be the last field in the structure for memory management
} jvp_literal_number;

typedef struct {
  decNumber number;
  decNumberUnit units[DEC_NUMBER_DOUBLE_EXTRA_UNITS];
} decNumberDoublePrecision;


static inline int jvp_number_is_literal(jv n) {
  assert(JVP_HAS_KIND(n, JV_KIND_NUMBER));
  return JVP_HAS_FLAGS(n, JVP_FLAGS_NUMBER_LITERAL);
}

static jvp_literal_number* jvp_literal_number_ptr(jv j) {
  assert(JVP_HAS_FLAGS(j, JVP_FLAGS_NUMBER_LITERAL));
  return (jvp_literal_number*)j.u.ptr;
}

static decNumber* jvp_dec_number_ptr(jv j) {
  assert(JVP_HAS_FLAGS(j, JVP_FLAGS_NUMBER_LITERAL));
  return &(((jvp_literal_number*)j.u.ptr)->num_decimal);
}

static jvp_literal_number* jvp_literal_number_alloc(unsigned literal_length) {
  /* The number of units needed is ceil(DECNUMDIGITS/DECDPUN)         */
  int units = ((literal_length+DECDPUN-1)/DECDPUN);

  jvp_literal_number* n = jv_mem_alloc(
    sizeof(jvp_literal_number)
    + sizeof(decNumberUnit) * units
  );

  n->refcnt = JV_REFCNT_INIT;
  n->num_double = NAN;
  n->literal_data = NULL;
  return n;
}

static jv jvp_literal_number_new(const char * literal) {
  jvp_literal_number* n = jvp_literal_number_alloc(strlen(literal));

  decContext *ctx = DEC_CONTEXT();
  decContextClearStatus(ctx, DEC_Conversion_syntax);
  decNumberFromString(&n->num_decimal, literal, ctx);

  if (ctx->status & DEC_Conversion_syntax) {
    jv_mem_free(n);
    return JV_INVALID;
  }
  if (decNumberIsNaN(&n->num_decimal)) {
    // Reject NaN with payload.
    if (n->num_decimal.digits > 1 || *n->num_decimal.lsu != 0) {
      jv_mem_free(n);
      return JV_INVALID;
    }
    jv_mem_free(n);
    return jv_number(NAN);
  }

  jv r = {JVP_FLAGS_NUMBER_LITERAL, 0, 0, 0, {&n->refcnt}};
  return r;
}

static double jvp_literal_number_to_double(jv j) {
  assert(JVP_HAS_FLAGS(j, JVP_FLAGS_NUMBER_LITERAL));
  decContext dblCtx;

  // init as decimal64 but change digits to allow conversion to binary64 (double)
  decContextDefault(&dblCtx, DEC_INIT_DECIMAL64);
  dblCtx.digits = DEC_NUMBER_DOUBLE_PRECISION;

  decNumber *p_dec_number = jvp_dec_number_ptr(j);
  decNumberDoublePrecision dec_double;
  char literal[DEC_NUMBER_DOUBLE_PRECISION + DEC_NUMBER_STRING_GUARD + 1];

  // reduce the number to the shortest possible form
  // that fits into the 64 bit floating point representation
  decNumberReduce(&dec_double.number, p_dec_number, &dblCtx);

  decNumberToString(&dec_double.number, literal);

  char *end;
  return jvp_strtod(tsd_dtoa_context_get(), literal, &end);
}

static const char* jvp_literal_number_literal(jv n) {
  assert(JVP_HAS_FLAGS(n, JVP_FLAGS_NUMBER_LITERAL));
  decNumber *pdec = jvp_dec_number_ptr(n);
  jvp_literal_number* plit = jvp_literal_number_ptr(n);

  if (decNumberIsNaN(pdec)) {
    return "null";
  }

  if (decNumberIsInfinite(pdec)) {
    // We cannot preserve the literal data of numbers outside the limited
    // range of exponent. Since `decNumberToString` returns "Infinity"
    // (or "-Infinity"), and to reduce stack allocations as possible, we
    // normalize infinities in the callers instead of printing the maximum
    // (or minimum) double here.
    return NULL;
  }

  if (plit->literal_data == NULL) {
    int len = jvp_dec_number_ptr(n)->digits + 15 /* 14 + NUL */;
    plit->literal_data = jv_mem_alloc(len);

    // Preserve the actual precision as we have parsed it
    // don't do decNumberTrim(pdec);

    decNumberToString(pdec, plit->literal_data);
  }

  return plit->literal_data;
}

int jv_number_has_literal(jv n) {
  assert(JVP_HAS_KIND(n, JV_KIND_NUMBER));
  return JVP_HAS_FLAGS(n, JVP_FLAGS_NUMBER_LITERAL);
}

const char* jv_number_get_literal(jv n) {
  assert(JVP_HAS_KIND(n, JV_KIND_NUMBER));

  if (JVP_HAS_FLAGS(n, JVP_FLAGS_NUMBER_LITERAL)) {
    return jvp_literal_number_literal(n);
  } else {
    return NULL;
  }
}

jv jv_number_with_literal(const char * literal) {
  return jvp_literal_number_new(literal);
}
#endif /* USE_DECNUM */

jv jv_number(double x) {
  jv j = {
#ifdef USE_DECNUM
    JVP_FLAGS_NUMBER_NATIVE,
#else
    JV_KIND_NUMBER,
#endif
    0, 0, 0, {.number = x}
  };
  return j;
}

static void jvp_number_free(jv j) {
  assert(JVP_HAS_KIND(j, JV_KIND_NUMBER));
#ifdef USE_DECNUM
  if (JVP_HAS_FLAGS(j, JVP_FLAGS_NUMBER_LITERAL) && jvp_refcnt_dec(j.u.ptr)) {
    jvp_literal_number* n = jvp_literal_number_ptr(j);
    if (n->literal_data) {
      jv_mem_free(n->literal_data);
    }
    jv_mem_free(n);
  }
#endif
}

double jv_number_value(jv j) {
  assert(JVP_HAS_KIND(j, JV_KIND_NUMBER));
#ifdef USE_DECNUM
  if (JVP_HAS_FLAGS(j, JVP_FLAGS_NUMBER_LITERAL)) {
    jvp_literal_number* n = jvp_literal_number_ptr(j);

    if (isnan(n->num_double)) {
      n->num_double = jvp_literal_number_to_double(j);
    }

    return n->num_double;
  }
#endif
  return j.u.number;
}

int jv_is_integer(jv j){
  if (!JVP_HAS_KIND(j, JV_KIND_NUMBER)){
    return 0;
  }

  double x = jv_number_value(j);

  double ipart;
  double fpart = modf(x, &ipart);

  return fabs(fpart) < DBL_EPSILON;
}

int jvp_number_is_nan(jv n) {
  assert(JVP_HAS_KIND(n, JV_KIND_NUMBER));

#ifdef USE_DECNUM
  if (JVP_HAS_FLAGS(n, JVP_FLAGS_NUMBER_LITERAL)) {
    decNumber *pdec = jvp_dec_number_ptr(n);
    return decNumberIsNaN(pdec);
  }
#endif
  return isnan(n.u.number);
}

jv jv_number_abs(jv n) {
  assert(JVP_HAS_KIND(n, JV_KIND_NUMBER));

#ifdef USE_DECNUM
  if (JVP_HAS_FLAGS(n, JVP_FLAGS_NUMBER_LITERAL)) {
    jvp_literal_number* m = jvp_literal_number_alloc(jvp_dec_number_ptr(n)->digits);

    decNumberAbs(&m->num_decimal, jvp_dec_number_ptr(n), DEC_CONTEXT());
    jv r = {JVP_FLAGS_NUMBER_LITERAL, 0, 0, 0, {&m->refcnt}};
    return r;
  }
#endif
  return jv_number(fabs(jv_number_value(n)));
}

jv jv_number_negate(jv n) {
  assert(JVP_HAS_KIND(n, JV_KIND_NUMBER));

#ifdef USE_DECNUM
  if (JVP_HAS_FLAGS(n, JVP_FLAGS_NUMBER_LITERAL)) {
    jvp_literal_number* m = jvp_literal_number_alloc(jvp_dec_number_ptr(n)->digits);

    decNumberMinus(&m->num_decimal, jvp_dec_number_ptr(n), DEC_CONTEXT());
    jv r = {JVP_FLAGS_NUMBER_LITERAL, 0, 0, 0, {&m->refcnt}};
    return r;
  }
#endif
  return jv_number(-jv_number_value(n));
}

int jvp_number_cmp(jv a, jv b) {
  assert(JVP_HAS_KIND(a, JV_KIND_NUMBER));
  assert(JVP_HAS_KIND(b, JV_KIND_NUMBER));

#ifdef USE_DECNUM
  if (JVP_HAS_FLAGS(a, JVP_FLAGS_NUMBER_LITERAL) && JVP_HAS_FLAGS(b, JVP_FLAGS_NUMBER_LITERAL)) {
    struct {
      decNumber number;
      decNumberUnit units[1];
    } res;

    decNumberCompare(&res.number,
                     jvp_dec_number_ptr(a),
                     jvp_dec_number_ptr(b),
                     DEC_CONTEXT()
                     );
    if (decNumberIsZero(&res.number)) {
      return 0;
    } else if (decNumberIsNegative(&res.number)) {
      return -1;
    } else {
      return 1;
    }
  }
#endif
  double da = jv_number_value(a), db = jv_number_value(b);
  if (da < db) {
    return -1;
  } else if (da == db) {
    return 0;
  } else {
    return 1;
  }
}

static int jvp_number_equal(jv a, jv b) {
  return jvp_number_cmp(a, b) == 0;
}

/*
 * Arrays (internal helpers)
 */

#define ARRAY_SIZE_ROUND_UP(n) (((n)*3)/2)
#define JVP_FLAGS_ARRAY   JVP_MAKE_FLAGS(JV_KIND_ARRAY, JVP_PAYLOAD_ALLOCATED)

static int imax(int a, int b) {
  if (a>b) return a;
  else return b;
}

//FIXME signed vs unsigned
typedef struct {
  jv_refcnt refcnt;
  int length, alloc_length;
  jv elements[];
} jvp_array;

static jvp_array* jvp_array_ptr(jv a) {
  assert(JVP_HAS_KIND(a, JV_KIND_ARRAY));
  return (jvp_array*)a.u.ptr;
}

static jvp_array* jvp_array_alloc(unsigned size) {
  jvp_array* a = jv_mem_alloc(sizeof(jvp_array) + sizeof(jv) * size);
  a->refcnt.count = 1;
  a->length = 0;
  a->alloc_length = size;
  return a;
}

static jv jvp_array_new(unsigned size) {
  jv r = {JVP_FLAGS_ARRAY, 0, 0, 0, {&jvp_array_alloc(size)->refcnt}};
  return r;
}

static void jvp_array_free(jv a) {
  assert(JVP_HAS_KIND(a, JV_KIND_ARRAY));
  if (jvp_refcnt_dec(a.u.ptr)) {
    jvp_array* array = jvp_array_ptr(a);
    for (int i=0; i<array->length; i++) {
      jv_free(array->elements[i]);
    }
    jv_mem_free(array);
  }
}

static int jvp_array_length(jv a) {
  assert(JVP_HAS_KIND(a, JV_KIND_ARRAY));
  return a.size;
}

static int jvp_array_offset(jv a) {
  assert(JVP_HAS_KIND(a, JV_KIND_ARRAY));
  return a.offset;
}

static jv* jvp_array_read(jv a, int i) {
  assert(JVP_HAS_KIND(a, JV_KIND_ARRAY));
  if (i >= 0 && i < jvp_array_length(a)) {
    jvp_array* array = jvp_array_ptr(a);
    assert(i + jvp_array_offset(a) < array->length);
    return &array->elements[i + jvp_array_offset(a)];
  } else {
    return 0;
  }
}

static jv* jvp_array_write(jv* a, int i) {
  assert(i >= 0);
  jvp_array* array = jvp_array_ptr(*a);

  int pos = i + jvp_array_offset(*a);
  if (pos < array->alloc_length && jvp_refcnt_unshared(a->u.ptr)) {
    // use existing array space
    for (int j = array->length; j <= pos; j++) {
      array->elements[j] = JV_NULL;
    }
    array->length = imax(pos + 1, array->length);
    a->size = imax(i + 1, a->size);
    return &array->elements[pos];
  } else {
    // allocate a new array
    int new_length = imax(i + 1, jvp_array_length(*a));
    jvp_array* new_array = jvp_array_alloc(ARRAY_SIZE_ROUND_UP(new_length));
    int j;
    for (j = 0; j < jvp_array_length(*a); j++) {
      new_array->elements[j] =
        jv_copy(array->elements[j + jvp_array_offset(*a)]);
    }
    for (; j < new_length; j++) {
      new_array->elements[j] = JV_NULL;
    }
    new_array->length = new_length;
    jvp_array_free(*a);
    jv new_jv = {JVP_FLAGS_ARRAY, 0, 0, new_length, {&new_array->refcnt}};
    *a = new_jv;
    return &new_array->elements[i];
  }
}

static int jvp_array_equal(jv a, jv b) {
  if (jvp_array_length(a) != jvp_array_length(b))
    return 0;
  if (jvp_array_ptr(a) == jvp_array_ptr(b) &&
      jvp_array_offset(a) == jvp_array_offset(b))
    return 1;
  for (int i=0; i<jvp_array_length(a); i++) {
    if (!jv_equal(jv_copy(*jvp_array_read(a, i)),
                  jv_copy(*jvp_array_read(b, i))))
      return 0;
  }
  return 1;
}

static void jvp_clamp_slice_params(int len, int *pstart, int *pend)
{
  if (*pstart < 0) *pstart = len + *pstart;
  if (*pend < 0) *pend = len + *pend;

  if (*pstart < 0) *pstart = 0;
  if (*pstart > len) *pstart = len;
  if (*pend > len) *pend = len;
  if (*pend < *pstart) *pend = *pstart;
}


static int jvp_array_contains(jv a, jv b) {
  int r = 1;
  jv_array_foreach(b, bi, belem) {
    int ri = 0;
    jv_array_foreach(a, ai, aelem) {
      if (jv_contains(aelem, jv_copy(belem))) {
        ri = 1;
        break;
      }
    }
    jv_free(belem);
    if (!ri) {
      r = 0;
      break;
    }
  }
  return r;
}


/*
 * Public
 */

static jv jvp_array_slice(jv a, int start, int end) {
  assert(JVP_HAS_KIND(a, JV_KIND_ARRAY));
  int len = jvp_array_length(a);
  jvp_clamp_slice_params(len, &start, &end);
  assert(0 <= start && start <= end && end <= len);

  // FIXME: maybe slice should reallocate if the slice is small enough
  if (start == end) {
    jv_free(a);
    return jv_array();
  }

  if (a.offset + start >= 1 << (sizeof(a.offset) * CHAR_BIT)) {
    jv r = jv_array_sized(end - start);
    for (int i = start; i < end; i++)
      r = jv_array_append(r, jv_array_get(jv_copy(a), i));
    jv_free(a);
    return r;
  } else {
    a.offset += start;
    a.size = end - start;
    return a;
  }
}

/*
 * Arrays (public interface)
 */

jv jv_array_sized(int n) {
  return jvp_array_new(n);
}

jv jv_array(void) {
  return jv_array_sized(16);
}

int jv_array_length(jv j) {
  assert(JVP_HAS_KIND(j, JV_KIND_ARRAY));
  int len = jvp_array_length(j);
  jv_free(j);
  return len;
}

jv jv_array_get(jv j, int idx) {
  assert(JVP_HAS_KIND(j, JV_KIND_ARRAY));
  jv* slot = jvp_array_read(j, idx);
  jv val;
  if (slot) {
    val = jv_copy(*slot);
  } else {
    val = jv_invalid();
  }
  jv_free(j);
  return val;
}

jv jv_array_set(jv j, int idx, jv val) {
  assert(JVP_HAS_KIND(j, JV_KIND_ARRAY));

  if (idx < 0)
    idx = jvp_array_length(j) + idx;
  if (idx < 0) {
    jv_free(j);
    jv_free(val);
    return jv_invalid_with_msg(jv_string("Out of bounds negative array index"));
  }
  if (idx > (INT_MAX >> 2) - jvp_array_offset(j)) {
    jv_free(j);
    jv_free(val);
    return jv_invalid_with_msg(jv_string("Array index too large"));
  }
  // copy/free of val,j coalesced
  jv* slot = jvp_array_write(&j, idx);
  jv_free(*slot);
  *slot = val;
  return j;
}

jv jv_array_append(jv j, jv val) {
  // copy/free of val,j coalesced
  return jv_array_set(j, jv_array_length(jv_copy(j)), val);
}

jv jv_array_concat(jv a, jv b) {
  assert(JVP_HAS_KIND(a, JV_KIND_ARRAY));
  assert(JVP_HAS_KIND(b, JV_KIND_ARRAY));

  // FIXME: could be faster
  jv_array_foreach(b, i, elem) {
    a = jv_array_append(a, elem);
    if (!jv_is_valid(a)) break;
  }
  jv_free(b);
  return a;
}

jv jv_array_slice(jv a, int start, int end) {
  assert(JVP_HAS_KIND(a, JV_KIND_ARRAY));
  // copy/free of a coalesced
  return jvp_array_slice(a, start, end);
}

jv jv_array_indexes(jv a, jv b) {
  jv res = jv_array();
  int idx = -1;
  int alen = jv_array_length(jv_copy(a));
  for (int ai = 0; ai < alen; ++ai) {
    jv_array_foreach(b, bi, belem) {
      if (!jv_equal(jv_array_get(jv_copy(a), ai + bi), belem))
        idx = -1;
      else if (bi == 0 && idx == -1)
        idx = ai;
    }
    if (idx > -1)
      res = jv_array_append(res, jv_number(idx));
    idx = -1;
  }
  jv_free(a);
  jv_free(b);
  return res;
}

/*
 * Strings (internal helpers)
 */

#define JVP_FLAGS_STRING  JVP_MAKE_FLAGS(JV_KIND_STRING, JVP_PAYLOAD_ALLOCATED)

typedef struct {
  jv_refcnt refcnt;
  uint32_t hash;
  // high 31 bits are length, low bit is a flag
  // indicating whether hash has been computed.
  uint32_t length_hashed;
  uint32_t alloc_length;
  char data[];
} jvp_string;

static jvp_string* jvp_string_ptr(jv a) {
  assert(JVP_HAS_KIND(a, JV_KIND_STRING));
  return (jvp_string*)a.u.ptr;
}

static jvp_string* jvp_string_alloc(uint32_t size) {
  jvp_string* s = jv_mem_alloc(sizeof(jvp_string) + size + 1);
  s->refcnt.count = 1;
  s->alloc_length = size;
  return s;
}

/* Copy a UTF8 string, replacing all badly encoded points with U+FFFD */
static jv jvp_string_copy_replace_bad(const char* data, uint32_t length) {
  const char* end = data + length;
  const char* i = data;

  uint32_t maxlength = length * 3 + 1; // worst case: all bad bytes, each becomes a 3-byte U+FFFD
  jvp_string* s = jvp_string_alloc(maxlength);
  char* out = s->data;
  int c = 0;

  while ((i = jvp_utf8_next(i, end, &c))) {
    if (c == -1) {
      c = 0xFFFD; // U+FFFD REPLACEMENT CHARACTER
    }
    out += jvp_utf8_encode(c, out);
    assert(out < s->data + maxlength);
  }
  length = out - s->data;
  s->data[length] = 0;
  s->length_hashed = length << 1;
  jv r = {JVP_FLAGS_STRING, 0, 0, 0, {&s->refcnt}};
  return r;
}

/* Assumes valid UTF8 */
static jv jvp_string_new(const char* data, uint32_t length) {
  jvp_string* s = jvp_string_alloc(length);
  s->length_hashed = length << 1;
  if (data != NULL)
    memcpy(s->data, data, length);
  s->data[length] = 0;
  jv r = {JVP_FLAGS_STRING, 0, 0, 0, {&s->refcnt}};
  return r;
}

static jv jvp_string_empty_new(uint32_t length) {
  jvp_string* s = jvp_string_alloc(length);
  s->length_hashed = 0;
  memset(s->data, 0, length);
  s->data[length] = 0;
  jv r = {JVP_FLAGS_STRING, 0, 0, 0, {&s->refcnt}};
  return r;
}


static void jvp_string_free(jv js) {
  jvp_string* s = jvp_string_ptr(js);
  if (jvp_refcnt_dec(&s->refcnt)) {
    jv_mem_free(s);
  }
}

static uint32_t jvp_string_length(jvp_string* s) {
  return s->length_hashed >> 1;
}

static uint32_t jvp_string_remaining_space(jvp_string* s) {
  assert(s->alloc_length >= jvp_string_length(s));
  uint32_t r = s->alloc_length - jvp_string_length(s);
  return r;
}

static jv jvp_string_append(jv string, const char* data, uint32_t len) {
  jvp_string* s = jvp_string_ptr(string);
  uint32_t currlen = jvp_string_length(s);

  if (jvp_refcnt_unshared(string.u.ptr) &&
      jvp_string_remaining_space(s) >= len) {
    // the next string fits at the end of a
    memcpy(s->data + currlen, data, len);
    s->data[currlen + len] = 0;
    s->length_hashed = (currlen + len) << 1;
    return string;
  } else {
    // allocate a bigger buffer and copy
    uint32_t allocsz = (currlen + len) * 2;
    if (allocsz < 32) allocsz = 32;
    jvp_string* news = jvp_string_alloc(allocsz);
    news->length_hashed = (currlen + len) << 1;
    memcpy(news->data, s->data, currlen);
    memcpy(news->data + currlen, data, len);
    news->data[currlen + len] = 0;
    jvp_string_free(string);
    jv r = {JVP_FLAGS_STRING, 0, 0, 0, {&news->refcnt}};
    return r;
  }
}

static const uint32_t HASH_SEED = 0x432A9843;

static uint32_t rotl32 (uint32_t x, int8_t r){
  return (x << r) | (x >> (32 - r));
}

static uint32_t jvp_string_hash(jv jstr) {
  jvp_string* str = jvp_string_ptr(jstr);
  if (str->length_hashed & 1)
    return str->hash;

  /* The following is based on MurmurHash3.
     MurmurHash3 was written by Austin Appleby, and is placed
     in the public domain. */

  const uint8_t* data = (const uint8_t*)str->data;
  int len = (int)jvp_string_length(str);
  const int nblocks = len / 4;

  uint32_t h1 = HASH_SEED;

  const uint32_t c1 = 0xcc9e2d51;
  const uint32_t c2 = 0x1b873593;
  const uint32_t* blocks = (const uint32_t *)(data + nblocks*4);

  for(int i = -nblocks; i; i++) {
    uint32_t k1 = blocks[i]; //FIXME: endianness/alignment

    k1 *= c1;
    k1 = rotl32(k1,15);
    k1 *= c2;

    h1 ^= k1;
    h1 = rotl32(h1,13);
    h1 = h1*5+0xe6546b64;
  }

  const uint8_t* tail = (const uint8_t*)(data + nblocks*4);

  uint32_t k1 = 0;

  switch(len & 3) {
  case 3: k1 ^= tail[2] << 16;
          JQ_FALLTHROUGH;
  case 2: k1 ^= tail[1] << 8;
          JQ_FALLTHROUGH;
  case 1: k1 ^= tail[0];
          k1 *= c1; k1 = rotl32(k1,15); k1 *= c2; h1 ^= k1;
  }

  h1 ^= len;

  h1 ^= h1 >> 16;
  h1 *= 0x85ebca6b;
  h1 ^= h1 >> 13;
  h1 *= 0xc2b2ae35;
  h1 ^= h1 >> 16;

  str->length_hashed |= 1;
  str->hash = h1;

  return h1;
}


static int jvp_string_equal(jv a, jv b) {
  assert(JVP_HAS_KIND(a, JV_KIND_STRING));
  assert(JVP_HAS_KIND(b, JV_KIND_STRING));
  jvp_string* stra = jvp_string_ptr(a);
  jvp_string* strb = jvp_string_ptr(b);
  if (jvp_string_length(stra) != jvp_string_length(strb)) return 0;
  return memcmp(stra->data, strb->data, jvp_string_length(stra)) == 0;
}

/*
 * Strings (public API)
 */

jv jv_string_sized(const char* str, int len) {
  return
    jvp_utf8_is_valid(str, str+len) ?
    jvp_string_new(str, len) :
    jvp_string_copy_replace_bad(str, len);
}

jv jv_string_empty(int len) {
  return jvp_string_empty_new(len);
}

jv jv_string(const char* str) {
  return jv_string_sized(str, strlen(str));
}

int jv_string_length_bytes(jv j) {
  assert(JVP_HAS_KIND(j, JV_KIND_STRING));
  int r = jvp_string_length(jvp_string_ptr(j));
  jv_free(j);
  return r;
}

int jv_string_length_codepoints(jv j) {
  assert(JVP_HAS_KIND(j, JV_KIND_STRING));
  const char* i = jv_string_value(j);
  const char* end = i + jv_string_length_bytes(jv_copy(j));
  int c = 0, len = 0;
  while ((i = jvp_utf8_next(i, end, &c))) len++;
  jv_free(j);
  return len;
}


jv jv_string_indexes(jv j, jv k) {
  assert(JVP_HAS_KIND(j, JV_KIND_STRING));
  assert(JVP_HAS_KIND(k, JV_KIND_STRING));
  const char *jstr = jv_string_value(j);
  const char *idxstr = jv_string_value(k);
  const char *p, *lp;
  int jlen = jv_string_length_bytes(jv_copy(j));
  int idxlen = jv_string_length_bytes(jv_copy(k));
  jv a = jv_array();

  if (idxlen != 0) {
    int n = 0;
    p = lp = jstr;
    while ((p = _jq_memmem(p, (jstr + jlen) - p, idxstr, idxlen)) != NULL) {
      while (lp < p) {
        lp += jvp_utf8_decode_length(*lp);
        n++;
      }

      a = jv_array_append(a, jv_number(n));
      if (!jv_is_valid(a)) break;
      p++;
    }
  }
  jv_free(j);
  jv_free(k);
  return a;
}

jv jv_string_repeat(jv j, int n) {
  assert(JVP_HAS_KIND(j, JV_KIND_STRING));
  if (n < 0) {
    jv_free(j);
    return jv_null();
  }
  int len = jv_string_length_bytes(jv_copy(j));
  int64_t res_len = (int64_t)len * n;
  if (res_len >= INT_MAX) {
    jv_free(j);
    return jv_invalid_with_msg(jv_string("Repeat string result too long"));
  }
  if (res_len == 0) {
    jv_free(j);
    return jv_string("");
  }
  jv res = jv_string_empty(res_len);
  res = jvp_string_append(res, jv_string_value(j), len);
  for (int curr = len, grow; curr < res_len; curr += grow) {
    grow = MIN(res_len - curr, curr);
    res = jvp_string_append(res, jv_string_value(res), grow);
  }
  jv_free(j);
  return res;
}

jv jv_string_split(jv j, jv sep) {
  assert(JVP_HAS_KIND(j, JV_KIND_STRING));
  assert(JVP_HAS_KIND(sep, JV_KIND_STRING));
  const char *jstr = jv_string_value(j);
  const char *jend = jstr + jv_string_length_bytes(jv_copy(j));
  const char *sepstr = jv_string_value(sep);
  const char *p, *s;
  int seplen = jv_string_length_bytes(jv_copy(sep));
  jv a = jv_array();

  assert(jv_get_refcnt(a) == 1);

  if (seplen == 0) {
    int c;
    while ((jstr = jvp_utf8_next(jstr, jend, &c))) {
      a = jv_array_append(a, jv_string_append_codepoint(jv_string(""), c));
      if (!jv_is_valid(a)) break;
    }
  } else {
    for (p = jstr; p < jend; p = s + seplen) {
      s = _jq_memmem(p, jend - p, sepstr, seplen);
      if (s == NULL)
        s = jend;
      a = jv_array_append(a, jv_string_sized(p, s - p));
      if (!jv_is_valid(a)) break;
      // Add an empty string to denote that j ends on a sep
      if (s + seplen == jend && seplen != 0)
        a = jv_array_append(a, jv_string(""));
    }
  }
  jv_free(j);
  jv_free(sep);
  return a;
}

jv jv_string_explode(jv j) {
  assert(JVP_HAS_KIND(j, JV_KIND_STRING));
  const char* i = jv_string_value(j);
  int len = jv_string_length_bytes(jv_copy(j));
  const char* end = i + len;
  jv a = jv_array_sized(len);
  int c;
  while ((i = jvp_utf8_next(i, end, &c))) {
    a = jv_array_append(a, jv_number(c));
    if (!jv_is_valid(a)) break;
  }
  jv_free(j);
  return a;
}

jv jv_string_implode(jv j) {
  assert(JVP_HAS_KIND(j, JV_KIND_ARRAY));
  int len = jv_array_length(jv_copy(j));
  jv s = jv_string_empty(len);
  int i;

  assert(len >= 0);

  for (i = 0; i < len; i++) {
    jv n = jv_array_get(jv_copy(j), i);
    assert(JVP_HAS_KIND(n, JV_KIND_NUMBER));
    int nv = jv_number_value(n);
    jv_free(n);
    // outside codepoint range or in utf16 surrogate pair range
    if (nv < 0 || nv > 0x10FFFF || (nv >= 0xD800 && nv <= 0xDFFF))
      nv = 0xFFFD; // U+FFFD REPLACEMENT CHARACTER
    s = jv_string_append_codepoint(s, nv);
  }

  jv_free(j);
  return s;
}

unsigned long jv_string_hash(jv j) {
  assert(JVP_HAS_KIND(j, JV_KIND_STRING));
  uint32_t hash = jvp_string_hash(j);
  jv_free(j);
  return hash;
}

const char* jv_string_value(jv j) {
  assert(JVP_HAS_KIND(j, JV_KIND_STRING));
  return jvp_string_ptr(j)->data;
}

jv jv_string_slice(jv j, int start, int end) {
  assert(JVP_HAS_KIND(j, JV_KIND_STRING));
  const char *s = jv_string_value(j);
  int len = jv_string_length_bytes(jv_copy(j));
  int i;
  const char *p, *e;
  int c;
  jv res;

  jvp_clamp_slice_params(len, &start, &end);
  assert(0 <= start && start <= end && end <= len);

  /* Look for byte offset corresponding to start codepoints */
  for (p = s, i = 0; i < start; i++) {
    p = jvp_utf8_next(p, s + len, &c);
    if (p == NULL) {
      jv_free(j);
      return jv_string_empty(16);
    }
    if (c == -1) {
      jv_free(j);
      return jv_invalid_with_msg(jv_string("Invalid UTF-8 string"));
    }
  }
  /* Look for byte offset corresponding to end codepoints */
  for (e = p; e != NULL && i < end; i++) {
    e = jvp_utf8_next(e, s + len, &c);
    if (e == NULL) {
      e = s + len;
      break;
    }
    if (c == -1) {
      jv_free(j);
      return jv_invalid_with_msg(jv_string("Invalid UTF-8 string"));
    }
  }

  /*
   * NOTE: Ideally we should do here what jvp_array_slice() does instead
   * of allocating a new string as we do!  However, we assume NUL-
   * terminated strings all over, and in the jv API, so for now we waste
   * memory like a drunken navy programmer.  There's probably nothing we
   * can do about it.
   */
  res = jv_string_sized(p, e - p);
  jv_free(j);
  return res;
}

jv jv_string_concat(jv a, jv b) {
  a = jvp_string_append(a, jv_string_value(b),
                        jvp_string_length(jvp_string_ptr(b)));
  jv_free(b);
  return a;
}

jv jv_string_append_buf(jv a, const char* buf, int len) {
  if (jvp_utf8_is_valid(buf, buf+len)) {
    a = jvp_string_append(a, buf, len);
  } else {
    jv b = jvp_string_copy_replace_bad(buf, len);
    a = jv_string_concat(a, b);
  }
  return a;
}

jv jv_string_append_codepoint(jv a, uint32_t c) {
  char buf[5];
  int len = jvp_utf8_encode(c, buf);
  a = jvp_string_append(a, buf, len);
  return a;
}

jv jv_string_append_str(jv a, const char* str) {
  return jv_string_append_buf(a, str, strlen(str));
}

jv jv_string_vfmt(const char* fmt, va_list ap) {
  int size = 1024;
  while (1) {
    char* buf = jv_mem_alloc(size);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, size, fmt, ap2);
    va_end(ap2);
    /*
     * NOTE: here we support old vsnprintf()s that return -1 because the
     * buffer is too small.
     */
    if (n >= 0 && n < size) {
      jv ret = jv_string_sized(buf, n);
      jv_mem_free(buf);
      return ret;
    } else {
      jv_mem_free(buf);
      size = (n > 0) ? /* standard */ (n * 2) : /* not standard */ (size * 2);
    }
  }
}

jv jv_string_fmt(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  jv res = jv_string_vfmt(fmt, args);
  va_end(args);
  return res;
}

/*
 * Objects (internal helpers)
 */

#define JVP_FLAGS_OBJECT  JVP_MAKE_FLAGS(JV_KIND_OBJECT, JVP_PAYLOAD_ALLOCATED)

struct object_slot {
  int next; /* next slot with same hash, for collisions */
  uint32_t hash;
  jv string;
  jv value;
};

typedef struct {
  jv_refcnt refcnt;
  int next_free;
  struct object_slot elements[];
} jvp_object;


/* warning: nontrivial justification of alignment */
static jv jvp_object_new(int size) {
  // Allocates an object of (size) slots and (size*2) hash buckets.

  // size must be a power of two
  assert(size > 0 && (size & (size - 1)) == 0);

  jvp_object* obj = jv_mem_alloc(sizeof(jvp_object) +
                                 sizeof(struct object_slot) * size +
                                 sizeof(int) * (size * 2));
  obj->refcnt.count = 1;
  for (int i=0; i<size; i++) {
    obj->elements[i].next = i - 1;
    obj->elements[i].string = JV_NULL;
    obj->elements[i].hash = 0;
    obj->elements[i].value = JV_NULL;
  }
  obj->next_free = 0;
  int* hashbuckets = (int*)(&obj->elements[size]);
  for (int i=0; i<size*2; i++) {
    hashbuckets[i] = -1;
  }
  jv r = {JVP_FLAGS_OBJECT, 0, 0, size, {&obj->refcnt}};
  return r;
}

static jvp_object* jvp_object_ptr(jv o) {
  assert(JVP_HAS_KIND(o, JV_KIND_OBJECT));
  return (jvp_object*)o.u.ptr;
}

static uint32_t jvp_object_mask(jv o) {
  assert(JVP_HAS_KIND(o, JV_KIND_OBJECT));
  return (o.size * 2) - 1;
}

static int jvp_object_size(jv o) {
  assert(JVP_HAS_KIND(o, JV_KIND_OBJECT));
  return o.size;
}

static int* jvp_object_buckets(jv o) {
  return (int*)(&jvp_object_ptr(o)->elements[o.size]);
}

static int* jvp_object_find_bucket(jv object, jv key) {
  return jvp_object_buckets(object) + (jvp_object_mask(object) & jvp_string_hash(key));
}

static struct object_slot* jvp_object_get_slot(jv object, int slot) {
  assert(slot == -1 || (slot >= 0 && slot < jvp_object_size(object)));
  if (slot == -1) return 0;
  else return &jvp_object_ptr(object)->elements[slot];
}

static struct object_slot* jvp_object_next_slot(jv object, struct object_slot* slot) {
  return jvp_object_get_slot(object, slot->next);
}

static struct object_slot* jvp_object_find_slot(jv object, jv keystr, int* bucket) {
  uint32_t hash = jvp_string_hash(keystr);
  for (struct object_slot* curr = jvp_object_get_slot(object, *bucket);
       curr;
       curr = jvp_object_next_slot(object, curr)) {
    if (curr->hash == hash && jvp_string_equal(keystr, curr->string)) {
      return curr;
    }
  }
  return 0;
}

static struct object_slot* jvp_object_add_slot(jv object, jv key, int* bucket) {
  jvp_object* o = jvp_object_ptr(object);
  int newslot_idx = o->next_free;
  if (newslot_idx == jvp_object_size(object)) return 0;
  struct object_slot* newslot = jvp_object_get_slot(object, newslot_idx);
  o->next_free++;
  newslot->next = *bucket;
  *bucket = newslot_idx;
  newslot->hash = jvp_string_hash(key);
  newslot->string = key;
  return newslot;
}

static jv* jvp_object_read(jv object, jv key) {
  assert(JVP_HAS_KIND(key, JV_KIND_STRING));
  int* bucket = jvp_object_find_bucket(object, key);
  struct object_slot* slot = jvp_object_find_slot(object, key, bucket);
  if (slot == 0) return 0;
  else return &slot->value;
}

static void jvp_object_free(jv o) {
  assert(JVP_HAS_KIND(o, JV_KIND_OBJECT));
  if (jvp_refcnt_dec(o.u.ptr)) {
    for (int i=0; i<jvp_object_size(o); i++) {
      struct object_slot* slot = jvp_object_get_slot(o, i);
      if (jv_get_kind(slot->string) != JV_KIND_NULL) {
        jvp_string_free(slot->string);
        jv_free(slot->value);
      }
    }
    jv_mem_free(jvp_object_ptr(o));
  }
}

static int jvp_object_rehash(jv *objectp) {
  jv object = *objectp;
  assert(JVP_HAS_KIND(object, JV_KIND_OBJECT));
  assert(jvp_refcnt_unshared(object.u.ptr));
  int size = jvp_object_size(object);
  if (size > INT_MAX >> 2)
    return 0;
  jv new_object = jvp_object_new(size * 2);
  for (int i=0; i<size; i++) {
    struct object_slot* slot = jvp_object_get_slot(object, i);
    if (jv_get_kind(slot->string) == JV_KIND_NULL) continue;
    int* new_bucket = jvp_object_find_bucket(new_object, slot->string);
    assert(!jvp_object_find_slot(new_object, slot->string, new_bucket));
    struct object_slot* new_slot = jvp_object_add_slot(new_object, slot->string, new_bucket);
    assert(new_slot);
    new_slot->value = slot->value;
  }
  // references are transported, just drop the old table
  jv_mem_free(jvp_object_ptr(object));
  *objectp = new_object;
  return 1;
}

static jv jvp_object_unshare(jv object) {
  assert(JVP_HAS_KIND(object, JV_KIND_OBJECT));
  if (jvp_refcnt_unshared(object.u.ptr))
    return object;

  jv new_object = jvp_object_new(jvp_object_size(object));
  jvp_object_ptr(new_object)->next_free = jvp_object_ptr(object)->next_free;
  for (int i=0; i<jvp_object_size(new_object); i++) {
    struct object_slot* old_slot = jvp_object_get_slot(object, i);
    struct object_slot* new_slot = jvp_object_get_slot(new_object, i);
    *new_slot = *old_slot;
    if (jv_get_kind(old_slot->string) != JV_KIND_NULL) {
      new_slot->string = jv_copy(old_slot->string);
      new_slot->value = jv_copy(old_slot->value);
    }
  }

  int* old_buckets = jvp_object_buckets(object);
  int* new_buckets = jvp_object_buckets(new_object);
  memcpy(new_buckets, old_buckets, sizeof(int) * jvp_object_size(new_object)*2);

  jvp_object_free(object);
  assert(jvp_refcnt_unshared(new_object.u.ptr));
  return new_object;
}

static int jvp_object_write(jv* object, jv key, jv **valpp) {
  *object = jvp_object_unshare(*object);
  int* bucket = jvp_object_find_bucket(*object, key);
  struct object_slot* slot = jvp_object_find_slot(*object, key, bucket);
  if (slot) {
    // already has the key
    jvp_string_free(key);
    *valpp = &slot->value;
    return 1;
  }
  slot = jvp_object_add_slot(*object, key, bucket);
  if (slot) {
    slot->value = jv_invalid();
  } else {
    if (!jvp_object_rehash(object)) {
      *valpp = NULL;
      return 0;
    }
    bucket = jvp_object_find_bucket(*object, key);
    assert(!jvp_object_find_slot(*object, key, bucket));
    slot = jvp_object_add_slot(*object, key, bucket);
    assert(slot);
    slot->value = jv_invalid();
  }
  *valpp = &slot->value;
  return 1;
}

static int jvp_object_delete(jv* object, jv key) {
  assert(JVP_HAS_KIND(key, JV_KIND_STRING));
  *object = jvp_object_unshare(*object);
  int* bucket = jvp_object_find_bucket(*object, key);
  int* prev_ptr = bucket;
  uint32_t hash = jvp_string_hash(key);
  for (struct object_slot* curr = jvp_object_get_slot(*object, *bucket);
       curr;
       curr = jvp_object_next_slot(*object, curr)) {
    if (hash == curr->hash && jvp_string_equal(key, curr->string)) {
      *prev_ptr = curr->next;
      jvp_string_free(curr->string);
      curr->string = JV_NULL;
      jv_free(curr->value);
      return 1;
    }
    prev_ptr = &curr->next;
  }
  return 0;
}

static int jvp_object_length(jv object) {
  int n = 0;
  for (int i=0; i<jvp_object_size(object); i++) {
    struct object_slot* slot = jvp_object_get_slot(object, i);
    if (jv_get_kind(slot->string) != JV_KIND_NULL) n++;
  }
  return n;
}

static int jvp_object_equal(jv o1, jv o2) {
  int len2 = jvp_object_length(o2);
  int len1 = 0;
  for (int i=0; i<jvp_object_size(o1); i++) {
    struct object_slot* slot = jvp_object_get_slot(o1, i);
    if (jv_get_kind(slot->string) == JV_KIND_NULL) continue;
    jv* slot2 = jvp_object_read(o2, slot->string);
    if (!slot2) return 0;
    // FIXME: do less refcounting here
    if (!jv_equal(jv_copy(slot->value), jv_copy(*slot2))) return 0;
    len1++;
  }
  return len1 == len2;
}

static int jvp_object_contains(jv a, jv b) {
  assert(JVP_HAS_KIND(a, JV_KIND_OBJECT));
  assert(JVP_HAS_KIND(b, JV_KIND_OBJECT));
  int r = 1;

  jv_object_foreach(b, key, b_val) {
    jv a_val = jv_object_get(jv_copy(a), key);

    r = jv_contains(a_val, b_val);

    if (!r) break;
  }
  return r;
}

/*
 * Objects (public interface)
 */
#define DEFAULT_OBJECT_SIZE 8
jv jv_object(void) {
  return jvp_object_new(8);
}

jv jv_object_get(jv object, jv key) {
  assert(JVP_HAS_KIND(object, JV_KIND_OBJECT));
  assert(JVP_HAS_KIND(key, JV_KIND_STRING));
  jv* slot = jvp_object_read(object, key);
  jv val;
  if (slot) {
    val = jv_copy(*slot);
  } else {
    val = jv_invalid();
  }
  jv_free(object);
  jv_free(key);
  return val;
}

int jv_object_has(jv object, jv key) {
  assert(JVP_HAS_KIND(object, JV_KIND_OBJECT));
  assert(JVP_HAS_KIND(key, JV_KIND_STRING));
  jv* slot = jvp_object_read(object, key);
  int res = slot ? 1 : 0;
  jv_free(object);
  jv_free(key);
  return res;
}

jv jv_object_set(jv object, jv key, jv value) {
  assert(JVP_HAS_KIND(object, JV_KIND_OBJECT));
  assert(JVP_HAS_KIND(key, JV_KIND_STRING));
  // copy/free of object, key, value coalesced
  jv* slot;
  if (!jvp_object_write(&object, key, &slot)) {
    jv_free(object);
    return jv_invalid_with_msg(jv_string("Object too big"));
  }
  jv_free(*slot);
  *slot = value;
  return object;
}

jv jv_object_delete(jv object, jv key) {
  assert(JVP_HAS_KIND(object, JV_KIND_OBJECT));
  assert(JVP_HAS_KIND(key, JV_KIND_STRING));
  jvp_object_delete(&object, key);
  jv_free(key);
  return object;
}

int jv_object_length(jv object) {
  assert(JVP_HAS_KIND(object, JV_KIND_OBJECT));
  int n = jvp_object_length(object);
  jv_free(object);
  return n;
}

jv jv_object_merge(jv a, jv b) {
  assert(JVP_HAS_KIND(a, JV_KIND_OBJECT));
  jv_object_foreach(b, k, v) {
    a = jv_object_set(a, k, v);
    if (!jv_is_valid(a)) break;
  }
  jv_free(b);
  return a;
}

jv jv_object_merge_recursive(jv a, jv b) {
  assert(JVP_HAS_KIND(a, JV_KIND_OBJECT));
  assert(JVP_HAS_KIND(b, JV_KIND_OBJECT));

  jv_object_foreach(b, k, v) {
    jv elem = jv_object_get(jv_copy(a), jv_copy(k));
    if (jv_is_valid(elem) &&
        JVP_HAS_KIND(elem, JV_KIND_OBJECT) &&
        JVP_HAS_KIND(v, JV_KIND_OBJECT)) {
      a = jv_object_set(a, k, jv_object_merge_recursive(elem, v));
    } else {
      jv_free(elem);
      a = jv_object_set(a, k, v);
    }
    if (!jv_is_valid(a)) break;
  }
  jv_free(b);
  return a;
}

/*
 * Object iteration (internal helpers)
 */

enum { ITER_FINISHED = -2 };

int jv_object_iter_valid(jv object, int i) {
  return i != ITER_FINISHED;
}

int jv_object_iter(jv object) {
  assert(JVP_HAS_KIND(object, JV_KIND_OBJECT));
  return jv_object_iter_next(object, -1);
}

int jv_object_iter_next(jv object, int iter) {
  assert(JVP_HAS_KIND(object, JV_KIND_OBJECT));
  assert(iter != ITER_FINISHED);
  struct object_slot* slot;
  do {
    iter++;
    if (iter >= jvp_object_size(object))
      return ITER_FINISHED;
    slot = jvp_object_get_slot(object, iter);
  } while (jv_get_kind(slot->string) == JV_KIND_NULL);
  assert(jv_get_kind(jvp_object_get_slot(object,iter)->string)
         == JV_KIND_STRING);
  return iter;
}

jv jv_object_iter_key(jv object, int iter) {
  jv s = jvp_object_get_slot(object, iter)->string;
  assert(JVP_HAS_KIND(s, JV_KIND_STRING));
  return jv_copy(s);
}

jv jv_object_iter_value(jv object, int iter) {
  return jv_copy(jvp_object_get_slot(object, iter)->value);
}

/*
 * Memory management
 */
jv jv_copy(jv j) {
  if (JVP_IS_ALLOCATED(j)) {
    jvp_refcnt_inc(j.u.ptr);
  }
  return j;
}

void jv_free(jv j) {
  switch(JVP_KIND(j)) {
    case JV_KIND_ARRAY:
      jvp_array_free(j);
      break;
    case JV_KIND_STRING:
      jvp_string_free(j);
      break;
    case JV_KIND_OBJECT:
      jvp_object_free(j);
      break;
    case JV_KIND_INVALID:
      jvp_invalid_free(j);
      break;
    case JV_KIND_NUMBER:
      jvp_number_free(j);
      break;
  }
}

int jv_get_refcnt(jv j) {
  if (JVP_IS_ALLOCATED(j)) {
    return j.u.ptr->count;
  } else {
    return 1;
  }
}

/*
 * Higher-level operations
 */

int jv_equal(jv a, jv b) {
  int r;
  if (jv_get_kind(a) != jv_get_kind(b)) {
    r = 0;
  } else if (JVP_IS_ALLOCATED(a) &&
             JVP_IS_ALLOCATED(b) &&
             a.kind_flags == b.kind_flags &&
             a.size == b.size &&
             a.u.ptr == b.u.ptr) {
    r = 1;
  } else {
    switch (jv_get_kind(a)) {
    case JV_KIND_NUMBER:
      r = jvp_number_equal(a, b);
      break;
    case JV_KIND_ARRAY:
      r = jvp_array_equal(a, b);
      break;
    case JV_KIND_STRING:
      r = jvp_string_equal(a, b);
      break;
    case JV_KIND_OBJECT:
      r = jvp_object_equal(a, b);
      break;
    default:
      r = 1;
      break;
    }
  }
  jv_free(a);
  jv_free(b);
  return r;
}

int jv_identical(jv a, jv b) {
  int r;
  if (a.kind_flags != b.kind_flags
      || a.offset != b.offset
      || a.size != b.size) {
    r = 0;
  } else {
    if (JVP_IS_ALLOCATED(a) /* b has the same flags */) {
      r = a.u.ptr == b.u.ptr;
    } else {
      r = memcmp(&a.u.ptr, &b.u.ptr, sizeof(a.u)) == 0;
    }
  }
  jv_free(a);
  jv_free(b);
  return r;
}

int jv_contains(jv a, jv b) {
  int r = 1;
  if (jv_get_kind(a) != jv_get_kind(b)) {
    r = 0;
  } else if (JVP_HAS_KIND(a, JV_KIND_OBJECT)) {
    r = jvp_object_contains(a, b);
  } else if (JVP_HAS_KIND(a, JV_KIND_ARRAY)) {
    r = jvp_array_contains(a, b);
  } else if (JVP_HAS_KIND(a, JV_KIND_STRING)) {
    int b_len = jv_string_length_bytes(jv_copy(b));
    if (b_len != 0) {
      r = _jq_memmem(jv_string_value(a), jv_string_length_bytes(jv_copy(a)),
                     jv_string_value(b), b_len) != 0;
    } else {
      r = 1;
    }
  } else {
    r = jv_equal(jv_copy(a), jv_copy(b));
  }
  jv_free(a);
  jv_free(b);
  return r;
}
