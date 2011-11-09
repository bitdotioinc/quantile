/*
* quantile.c - Quantile aggregate function
* Copyright (C) Tomas Vondra, 2011
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*
* TODO Take care of the memory consumption (respect work_mem, flush
* the data to disk if more space is needed and use merge sort, i.e.
* something like the ORDER BY uses).
*/

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>

#include "postgres.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define GET_AGG_CONTEXT(fname, fcinfo, aggcontext)  \
    if (! AggCheckCallContext(fcinfo, &aggcontext)) {   \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }

#define CHECK_AGG_CONTEXT(fname, fcinfo)  \
    if (! AggCheckCallContext(fcinfo, NULL)) {   \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }

#define SLICE_SIZE 1024

/* Structures used to keep the data - the 'elements' array is extended
 * on the fly if needed. */

typedef struct struct_double {
    
    int nquantiles;
    int nelements;
    int next;
    
    double * quantiles;
    double * elements;
    
} struct_double;

typedef struct struct_int32 {
    
    int nquantiles;
    int nelements;
    int next;
    
    double * quantiles;
    int32  * elements;
    
} struct_int32;

typedef struct struct_int64 {
    
    int nquantiles;
    int nelements;
    int next;
    
    double * quantiles;
    int64  * elements;
    
} struct_int64;

typedef struct struct_numeric {
    
    int nquantiles;
    int nelements;
    int next;
    
    double * quantiles;
    Numeric * elements;
    
} struct_numeric;

/* comparators, used for qsort */

static int  double_comparator(const void *a, const void *b);
static int  int32_comparator(const void *a, const void *b);
static int  int64_comparator(const void *a, const void *b);
static int  numeric_comparator(const void *a, const void *b);

/* parse the quantiles array */
static double *
array_to_double(FunctionCallInfo fcinfo, ArrayType *v, int * len);

static Datum
double_to_array(FunctionCallInfo fcinfo, double * d, int len);

static Datum
int32_to_array(FunctionCallInfo fcinfo, int32 * d, int len);

static Datum
int64_to_array(FunctionCallInfo fcinfo, int64 * d, int len);

static Datum
numeric_to_array(FunctionCallInfo fcinfo, Numeric * d, int len);

/* prototypes */
PG_FUNCTION_INFO_V1(quantile_append_double_array);
PG_FUNCTION_INFO_V1(quantile_append_double);

PG_FUNCTION_INFO_V1(quantile_double_array);
PG_FUNCTION_INFO_V1(quantile_double);

PG_FUNCTION_INFO_V1(quantile_append_int32_array);
PG_FUNCTION_INFO_V1(quantile_append_int32);

PG_FUNCTION_INFO_V1(quantile_int32_array);
PG_FUNCTION_INFO_V1(quantile_int32);

PG_FUNCTION_INFO_V1(quantile_append_int64_array);
PG_FUNCTION_INFO_V1(quantile_append_int64);

PG_FUNCTION_INFO_V1(quantile_int64_array);
PG_FUNCTION_INFO_V1(quantile_int64);

PG_FUNCTION_INFO_V1(quantile_append_numeric_array);
PG_FUNCTION_INFO_V1(quantile_append_numeric);

PG_FUNCTION_INFO_V1(quantile_numeric_array);
PG_FUNCTION_INFO_V1(quantile_numeric);

Datum quantile_append_double_array(PG_FUNCTION_ARGS);
Datum quantile_append_double(PG_FUNCTION_ARGS);

Datum quantile_double_array(PG_FUNCTION_ARGS);
Datum quantile_double(PG_FUNCTION_ARGS);

Datum quantile_append_int32_array(PG_FUNCTION_ARGS);
Datum quantile_append_int32(PG_FUNCTION_ARGS);

Datum quantile_int32_array(PG_FUNCTION_ARGS);
Datum quantile_int32(PG_FUNCTION_ARGS);

Datum quantile_append_int64_array(PG_FUNCTION_ARGS);
Datum quantile_append_int64(PG_FUNCTION_ARGS);

Datum quantile_int64_array(PG_FUNCTION_ARGS);
Datum quantile_int64(PG_FUNCTION_ARGS);

Datum quantile_append_numeric_array(PG_FUNCTION_ARGS);
Datum quantile_append_numeric(PG_FUNCTION_ARGS);

Datum quantile_numeric_array(PG_FUNCTION_ARGS);
Datum quantile_numeric(PG_FUNCTION_ARGS);

/* These functions use a bit dirty trick to pass the data - the int
 * value is actually a pointer to the array allocated in the parent
 * memory context. A bit ugly but works fine.
 *
 * The memory consumption might be a problem, as all the values are
 * kept in the memory - for example 1.000.000 of 8-byte values (bigint)
 * requires about 8MB of memory.
 */

Datum
quantile_append_double(PG_FUNCTION_ARGS)
{
    
    struct_double * data;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;

    GET_AGG_CONTEXT("quantile_append_double", fcinfo, aggcontext);

    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        data = (struct_double*)palloc(sizeof(struct_double));
        data->elements  = (double*)palloc(SLICE_SIZE*sizeof(double));
        data->nelements = SLICE_SIZE;
        data->next = 0;
        
        data->quantiles = (double*)palloc(sizeof(double));
        data->quantiles[0] = PG_GETARG_FLOAT8(2);
        data->nquantiles = 1;
    } else {
        data = (struct_double*)PG_GETARG_POINTER(0);
    }
    
    /* ignore NULL values */
    if (! PG_ARGISNULL(1)) {
    
        double element = PG_GETARG_FLOAT8(1);
        
        if (data->next > data->nelements-1) {
            data->elements = (double*)repalloc(data->elements, sizeof(double)*(data->nelements + SLICE_SIZE));
            data->nelements = data->nelements + SLICE_SIZE;
        }
        
        data->elements[data->next++] = element;
        
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(data);

}

Datum
quantile_append_double_array(PG_FUNCTION_ARGS)
{
    
    struct_double * data;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;

    GET_AGG_CONTEXT("quantile_append_double_array", fcinfo, aggcontext);

    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        data = (struct_double*)palloc(sizeof(struct_double));
        data->elements  = (double*)palloc(SLICE_SIZE*sizeof(double));
        data->nelements = SLICE_SIZE;
        data->next = 0;
        
        /* read the array of quantiles */
        data->quantiles = array_to_double(fcinfo, PG_GETARG_ARRAYTYPE_P(2), &data->nquantiles);
        
    } else {
        data = (struct_double*)PG_GETARG_POINTER(0);
    }
    
    /* ignore NULL values */
    if (! PG_ARGISNULL(1)) {
    
        double element = PG_GETARG_FLOAT8(1);
        
        if (data->next > data->nelements-1) {
            data->elements = (double*)repalloc(data->elements, sizeof(double)*(data->nelements + SLICE_SIZE));
            data->nelements = data->nelements + SLICE_SIZE;
        }
        
        data->elements[data->next++] = element;
        
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(data);

}

Datum
quantile_append_numeric(PG_FUNCTION_ARGS)
{
    
    struct_numeric * data;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;

    GET_AGG_CONTEXT("quantile_append_numeric", fcinfo, aggcontext);
    
    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        data = (struct_numeric*)palloc(sizeof(struct_numeric));
        data->elements  = (Numeric*)palloc(SLICE_SIZE*sizeof(Numeric));
        data->nelements = SLICE_SIZE;
        data->next = 0;
        
        data->quantiles = (double*)palloc(sizeof(double));
        data->quantiles[0] = PG_GETARG_FLOAT8(2);
        data->nquantiles = 1;
    } else {
        data = (struct_numeric*)PG_GETARG_POINTER(0);
    }
    
    /* ignore NULL values */
    if (! PG_ARGISNULL(1)) {
    
        Numeric element = PG_GETARG_NUMERIC(1);
        
        if (data->next > data->nelements-1) {
            data->elements = (Numeric*)repalloc(data->elements, sizeof(Numeric)*(data->nelements + SLICE_SIZE));
            data->nelements = data->nelements + SLICE_SIZE;
        }
        
        data->elements[data->next++] = element;
        
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(data);

}

Datum
quantile_append_numeric_array(PG_FUNCTION_ARGS)
{
    
    struct_numeric * data;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;

    GET_AGG_CONTEXT("quantile_append_numeric_array", fcinfo, aggcontext);

    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        data = (struct_numeric*)palloc(sizeof(struct_numeric));
        data->elements  = (Numeric*)palloc(SLICE_SIZE*sizeof(Numeric));
        data->nelements = SLICE_SIZE;
        data->next = 0;
        
        /* read the array of quantiles */
        data->quantiles = array_to_double(fcinfo, PG_GETARG_ARRAYTYPE_P(2), &data->nquantiles);
        
    } else {
        data = (struct_numeric*)PG_GETARG_POINTER(0);
    }
    
    /* ignore NULL values */
    if (! PG_ARGISNULL(1)) {
    
        Numeric element = PG_GETARG_NUMERIC(1);
        
        if (data->next > data->nelements-1) {
            data->elements = (Numeric*)repalloc(data->elements, sizeof(Numeric)*(data->nelements + SLICE_SIZE));
            data->nelements = data->nelements + SLICE_SIZE;
        }
        
        data->elements[data->next++] = element;
        
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(data);

}

Datum
quantile_append_int32(PG_FUNCTION_ARGS)
{
    
    struct_int32 * data;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;

    GET_AGG_CONTEXT("quantile_append_int32", fcinfo, aggcontext);

    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        data = (struct_int32*)palloc(sizeof(struct_int32));
        data->quantiles = (double*)palloc(sizeof(double));
        data->elements  = (int32*)palloc(SLICE_SIZE*sizeof(int32));
        data->nelements = SLICE_SIZE;
        data->nquantiles = 1;
        data->quantiles[0] = PG_GETARG_FLOAT8(2);
        data->next = 0;
    } else {
        data = (struct_int32*)PG_GETARG_POINTER(0);
    }
    
    /* ignore NULL values */
    if (! PG_ARGISNULL(1)) {
    
        int32 element = PG_GETARG_INT32(1);
        
        if (data->next > data->nelements-1) {
            data->elements = (int32*)repalloc(data->elements, sizeof(int32)*(data->nelements + SLICE_SIZE));
            data->nelements = data->nelements + SLICE_SIZE;
        }
        
        data->elements[data->next++] = element;
        
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(data);

}

Datum
quantile_append_int32_array(PG_FUNCTION_ARGS)
{
    
    struct_int32 * data;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;

    GET_AGG_CONTEXT("quantile_append_int32_array", fcinfo, aggcontext);
    
    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        data = (struct_int32*)palloc(sizeof(struct_int32));
        data->elements  = (int32*)palloc(SLICE_SIZE*sizeof(int32));
        data->nelements = SLICE_SIZE;
        data->next = 0;
        
        /* read the array of quantiles */
        data->quantiles = array_to_double(fcinfo, PG_GETARG_ARRAYTYPE_P(2), &data->nquantiles);
        
    } else {
        data = (struct_int32*)PG_GETARG_POINTER(0);
    }
    
    /* ignore NULL values */
    if (! PG_ARGISNULL(1)) {

        int32 element = PG_GETARG_INT32(1);
        
        if (data->next > data->nelements-1) {
            data->elements = (int32*)repalloc(data->elements, sizeof(int32)*(data->nelements + SLICE_SIZE));
            data->nelements = data->nelements + SLICE_SIZE;
        }
        
        data->elements[data->next++] = element;
        
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(data);

}

Datum
quantile_append_int64(PG_FUNCTION_ARGS)
{
    
    struct_int64 * data;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;

    GET_AGG_CONTEXT("quantile_append_int64", fcinfo, aggcontext);

    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        data = (struct_int64*)palloc(sizeof(struct_int64));
        data->quantiles = (double*)palloc(sizeof(double));
        data->elements  = (int64*)palloc(SLICE_SIZE*sizeof(int64));
        data->nelements = SLICE_SIZE;
        data->nquantiles = 1;
        data->quantiles[0] = PG_GETARG_FLOAT8(2);
        data->next = 0;
    } else {
        data = (struct_int64*)PG_GETARG_POINTER(0);
    }
    
    /* ignore NULL values */
    if (! PG_ARGISNULL(1)) {
    
        int64 element = PG_GETARG_INT64(1);
        
        if (data->next > data->nelements-1) {
            data->elements = (int64*)repalloc(data->elements, sizeof(int64)*(data->nelements + SLICE_SIZE));
            data->nelements = data->nelements + SLICE_SIZE;
        }
        
        data->elements[data->next++] = element;
        
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(data);

}

Datum
quantile_append_int64_array(PG_FUNCTION_ARGS)
{
    
    struct_int64 * data;
    
    MemoryContext oldcontext;
    MemoryContext aggcontext;

    GET_AGG_CONTEXT("quantile_append_int64_array", fcinfo, aggcontext);

    oldcontext = MemoryContextSwitchTo(aggcontext);
        
    if (PG_ARGISNULL(0)) {
        data = (struct_int64*)palloc(sizeof(struct_int64));
        data->elements  = (int64*)palloc(SLICE_SIZE*sizeof(int64));
        data->nelements = SLICE_SIZE;
        data->next = 0;
        
        /* read the array of quantiles */
        data->quantiles = array_to_double(fcinfo, PG_GETARG_ARRAYTYPE_P(2), &data->nquantiles);
        
    } else {
        data = (struct_int64*)PG_GETARG_POINTER(0);
    }
    
    /* ignore NULL values */
    if (! PG_ARGISNULL(1)) {
    
        int64 element = PG_GETARG_INT64(1);
        
        if (data->next > data->nelements-1) {
            data->elements = (int64*)repalloc(data->elements, sizeof(int64)*(data->nelements + SLICE_SIZE));
            data->nelements = data->nelements + SLICE_SIZE;
        }
        
        data->elements[data->next++] = element;
        
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(data);

}

Datum
quantile_double(PG_FUNCTION_ARGS)
{
    
    int     idx = 0;
    double *result;
    
    struct_double * data;
    
    CHECK_AGG_CONTEXT("quantile_double", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    data = (struct_double*)PG_GETARG_POINTER(0);
    
    result = palloc(data->nquantiles * sizeof(double));
    
    qsort(data->elements, data->next, sizeof(double), &double_comparator);

    if ((data->quantiles[0] > 0) && (data->quantiles[0] < 1)) {
        idx = (int)ceil(data->next * data->quantiles[0]) - 1;
    } else {
        idx = 0;
    }
    
    PG_RETURN_FLOAT8(data->elements[idx]);

}

Datum
quantile_double_array(PG_FUNCTION_ARGS)
{
    
    int i, idx = 0;
    double * result;
    
    struct_double * data;
    
    CHECK_AGG_CONTEXT("quantile_double_array", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    data = (struct_double*)PG_GETARG_POINTER(0);
    
    result = palloc(data->nquantiles * sizeof(double));
    
    qsort(data->elements, data->next, sizeof(double), &double_comparator);

    for (i = 0; i < data->nquantiles; i++) {
        
        if ((data->quantiles[i] > 0) && (data->quantiles[i] < 1)) {
            idx = (int)ceil(data->next * data->quantiles[i]) - 1;
        } else if (data->quantiles[i] <= 0) {
            idx = 0;
        } else if (data->quantiles[i] >= 1) {
            idx = data->next - 1;
        }
        
        result[i] = data->elements[idx];
    }

    return double_to_array(fcinfo, result, data->nquantiles);

}

Datum
quantile_int32(PG_FUNCTION_ARGS)
{
    
    int idx;
    struct_int32 * data;
    
    CHECK_AGG_CONTEXT("quantile_int32", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    data = (struct_int32*)PG_GETARG_POINTER(0);
    
    qsort(data->elements, data->next, sizeof(int32), &int32_comparator);
    
    if (data->quantiles[0] > 0) {
        idx = (int)ceil(data->next * data->quantiles[0]) - 1;
    } else {
        idx = 0;
    }
    
    PG_RETURN_INT32(data->elements[idx]);

}

Datum
quantile_int32_array(PG_FUNCTION_ARGS)
{
    
    int i, idx = 0;
    struct_int32 * data;
    int32 * result;
    
    CHECK_AGG_CONTEXT("quantile_int32_array", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    data = (struct_int32*)PG_GETARG_POINTER(0);
    
    result = palloc(data->nquantiles * sizeof(int32));
    
    qsort(data->elements, data->next, sizeof(int32), &int32_comparator);

    for (i = 0; i < data->nquantiles; i++) {
        
        if ((data->quantiles[i] > 0) && (data->quantiles[i] < 1)) {
            idx = (int)ceil(data->next * data->quantiles[i]) - 1;
        } else if (data->quantiles[i] <= 0) {
            idx = 0;
        } else if (data->quantiles[i] >= 1) {
            idx = data->next - 1;
        }
        
        result[i] = data->elements[idx];
    }

    return int32_to_array(fcinfo, result, data->nquantiles);

}

Datum
quantile_int64(PG_FUNCTION_ARGS)
{
    
    int idx;
    struct_int64 * data;
    
    CHECK_AGG_CONTEXT("quantile_int64", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    data = (struct_int64*)PG_GETARG_POINTER(0);
    
    qsort(data->elements, data->next, sizeof(int64), &int64_comparator);
    
    if (data->quantiles[0] > 0) {
        idx = (int)ceil(data->next * data->quantiles[0]) - 1;
    } else {
        idx = 0;
    }
    
    PG_RETURN_INT64(data->elements[idx]);

}

Datum
quantile_int64_array(PG_FUNCTION_ARGS)
{
    
    int i, idx = 0;
    struct_int64 * data;
    int64 * result;
    
    CHECK_AGG_CONTEXT("quantile_int64_array", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    data = (struct_int64*)PG_GETARG_POINTER(0);
    
    result = palloc(data->nquantiles * sizeof(int64));
    
    qsort(data->elements, data->next, sizeof(int64), &int64_comparator);

    for (i = 0; i < data->nquantiles; i++) {
        
        if ((data->quantiles[i] > 0) && (data->quantiles[i] < 1)) {
            idx = (int)ceil(data->next * data->quantiles[i]) - 1;
        } else if (data->quantiles[i] <= 0) {
            idx = 0;
        } else if (data->quantiles[i] >= 1) {
            idx = data->next - 1;
        }
        
        result[i] = data->elements[idx];
        
    }

    return int64_to_array(fcinfo, result, data->nquantiles);

}

Datum
quantile_numeric(PG_FUNCTION_ARGS)
{
    
    int idx;
    struct_numeric * data;
    
    CHECK_AGG_CONTEXT("quantile_numeric", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    data = (struct_numeric*)PG_GETARG_POINTER(0);
    
    qsort(data->elements, data->next, sizeof(Numeric), &numeric_comparator);
    
    if (data->quantiles[0] > 0) {
        idx = (int)ceil(data->next * data->quantiles[0]) - 1;
    } else {
        idx = 0;
    }
    
    PG_RETURN_NUMERIC(data->elements[idx]);

}

Datum
quantile_numeric_array(PG_FUNCTION_ARGS)
{
    
    int i, idx = 0;
    struct_numeric * data;
    Numeric * result;
    
    CHECK_AGG_CONTEXT("quantile_numeric_array", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    data = (struct_numeric*)PG_GETARG_POINTER(0);
    
    result = palloc(data->nquantiles * sizeof(Numeric));
    
    qsort(data->elements, data->next, sizeof(Numeric), &numeric_comparator);

    for (i = 0; i < data->nquantiles; i++) {
        
        if ((data->quantiles[i] > 0) && (data->quantiles[i] < 1)) {
            idx = (int)ceil(data->next * data->quantiles[i]) - 1;
        } else if (data->quantiles[i] <= 0) {
            idx = 0;
        } else if (data->quantiles[i] >= 1) {
            idx = data->next - 1;
        }
        
        result[i] = data->elements[idx];
        
    }

    return numeric_to_array(fcinfo, result, data->nquantiles);

}

/* Comparators for the qsort() calls. */

static int  double_comparator(const void *a, const void *b) {
    double af = (*(double*)a);
    double bf = (*(double*)b);
    return (af > bf) - (af < bf);
}

static int  int32_comparator(const void *a, const void *b) {
    int32 af = (*(int32*)a);
    int32 bf = (*(int32*)b);
    return (af > bf) - (af < bf);
}

static int  int64_comparator(const void *a, const void *b) {
    int64 af = (*(int64*)a);
    int64 bf = (*(int64*)b);
    return (af > bf) - (af < bf);
}

static int  numeric_comparator(const void *a, const void *b) {

    FunctionCallInfoData fcinfo;
    
    /* set params */
    fcinfo.arg[0] = NumericGetDatum(*(Numeric*)a);
    fcinfo.arg[1] = NumericGetDatum(*(Numeric*)b);
    fcinfo.argnull[0] = false;
    fcinfo.argnull[1] = false;
    
    /* return the result */
    return DatumGetInt32(numeric_cmp(&fcinfo));
    
}

/*
 * Reading quantiles from an input array, based mostly on
 * array_to_text_internal (it's a modified copy). This expects
 * to receive a single-dimensional float8 array as input, fails
 * otherwise.
 */
static double *
array_to_double(FunctionCallInfo fcinfo, ArrayType *v, int * len)
{
    double     *result;
    int         nitems,
               *dims,
                ndims;
    Oid         element_type;
    int         typlen;
    bool        typbyval;
    char        typalign;
    char       *p;
    int         i, idx = 0;
    
    ArrayMetaState *my_extra;

    ndims = ARR_NDIM(v);
    dims = ARR_DIMS(v);
    nitems = ArrayGetNItems(ndims, dims);
    
    /* this is a special-purpose function for single-dimensional arrays */
    if (ndims != 1) {
        elog(ERROR, "error, array_to_double expects a single-dimensional array"
                    "(dims = %d)", ndims);
    }

    /* if there are no elements, set the length to 0 and return NULL */
    if (nitems == 0) {
        (*len) = 0;
        return NULL;
    }
    
    element_type = ARR_ELEMTYPE(v);
    
    result = (double*)palloc(nitems * sizeof(double));

    /*
     * We arrange to look up info about element type, including its output
     * conversion proc, only once per series of calls, assuming the element
     * type doesn't change underneath us.
     */
    my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
    if (my_extra == NULL)
    {
        fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
                                                      sizeof(ArrayMetaState));
        my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
        my_extra->element_type = ~element_type;
    }

    if (my_extra->element_type != element_type)
    {
        /*
         * Get info about element type, including its output conversion proc
         */
        get_type_io_data(element_type, IOFunc_output,
                         &my_extra->typlen, &my_extra->typbyval,
                         &my_extra->typalign, &my_extra->typdelim,
                         &my_extra->typioparam, &my_extra->typiofunc);
        fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
                      fcinfo->flinfo->fn_mcxt);
        my_extra->element_type = element_type;
    }
    
    typlen = my_extra->typlen;
    typbyval = my_extra->typbyval;
    typalign = my_extra->typalign;
    
    p = ARR_DATA_PTR(v);

    for (i = 0; i < nitems; i++) {

        Datum itemvalue = fetch_att(p, typbyval, typlen);
        
        double val = DatumGetFloat8(itemvalue);
        
        result[idx++] = val;
        
        p = att_addlength_pointer(p, typlen, p);
        p = (char *) att_align_nominal(p, typalign);

    }
    
    (*len) = idx;

    return result;
}

/*
 * Helper functions used to prepare the resulting array (when there's
 * an array of quantiles).
 */
static Datum
double_to_array(FunctionCallInfo fcinfo, double * d, int len) {
    
    ArrayBuildState *astate = NULL;
    int         i;

    for (i = 0; i < len; i++) {

        /* stash away this field */
        astate = accumArrayResult(astate,
                                  Float8GetDatum(d[i]),
                                  false,
                                  FLOAT8OID,
                                  CurrentMemoryContext);
    }

    PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate,
                                          CurrentMemoryContext));
}

static Datum
int32_to_array(FunctionCallInfo fcinfo, int32 * d, int len) {
    
    ArrayBuildState *astate = NULL;
    int         i;

    for (i = 0; i < len; i++) {

        /* stash away this field */
        astate = accumArrayResult(astate,
                                  Int32GetDatum(d[i]),
                                  false,
                                  INT4OID,
                                  CurrentMemoryContext);
    }

    PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate,
                                          CurrentMemoryContext));
}

static Datum
int64_to_array(FunctionCallInfo fcinfo, int64 * d, int len) {
    
    ArrayBuildState *astate = NULL;
    int         i;

    for (i = 0; i < len; i++) {

        /* stash away this field */
        astate = accumArrayResult(astate,
                                  Int64GetDatum(d[i]),
                                  false,
                                  INT8OID,
                                  CurrentMemoryContext);
        
    }

    PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate,
                                          CurrentMemoryContext));
}

static Datum
numeric_to_array(FunctionCallInfo fcinfo, Numeric * d, int len) {
    
    ArrayBuildState *astate = NULL;
    int         i;

    for (i = 0; i < len; i++) {

        /* stash away this field */
        astate = accumArrayResult(astate,
                                  NumericGetDatum(d[i]),
                                  false,
                                  NUMERICOID,
                                  CurrentMemoryContext);
        
    }

    PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate,
                                          CurrentMemoryContext));
}
