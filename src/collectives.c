/* -*- C -*-
 *
 * Copyright (c) 2011 Sandia National Laboratories. All rights reserved.
 */

#include "config.h"

#include <portals4.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "mpp/shmem.h"
#include "shmem_internal.h"

#define MAX(A, B) ((A > B) ? A : B)
#define MIN(A, B) ((A < B) ? A : B)

static long *barrier_work_array;

int
shmem_barrier_init(void)
{
    barrier_work_array = shmalloc(sizeof(long) * _SHMEM_BARRIER_SYNC_SIZE);
    if (NULL == barrier_work_array) return -1;
    bzero(barrier_work_array, sizeof(long) * _SHMEM_BARRIER_SYNC_SIZE);
    return 0;
}


void
shmem_barrier_all(void)
{
    shmem_quiet();
    shmem_barrier(0, 0, shmem_int_num_pes, barrier_work_array);
}


void
shmem_barrier(int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    /* BWB: make tree-based */
    int stride = (logPE_stride == 0) ? 1 : 1 << logPE_stride;
    if (PE_start == shmem_int_my_pe) {
        int pe, i;
        shmem_long_wait_until(pSync, SHMEM_CMP_EQ, PE_size - 1);
        pSync[0] = 0;
        for (pe = PE_start + stride, i = 1 ; 
             i < PE_size ;  
             i++, pe += stride) {
            shmem_long_p(pSync, 1, pe);
        }
    } else {
        shmem_long_inc(pSync, PE_start);
        shmem_long_wait(pSync, 0);
        pSync[0] = 0;
    }
}


static inline
void
shmem_internal_op_to_all(void *target, void *source, int count, int type_size,
                    int PE_start, int logPE_stride, int PE_size,
                    void *pWrk, long *pSync, 
                    ptl_op_t op, ptl_datatype_t datatype)
{
    int sent;
    int work_unit = MAX(_SHMEM_REDUCE_MIN_WRKDATA_SIZE, count / 2 + 1);
    int stride = (logPE_stride == 0) ? 1 : 1 << logPE_stride;
    int ret = 0;
    long tmp = 1;

    /* BWB: make tree-based */
    for (sent = 0 ; sent < count ; sent += work_unit) {
        int iter_count = MIN(work_unit, count - sent);
        int len = iter_count * type_size;
        int offset = sent * type_size;

        if (PE_start == shmem_int_my_pe) {
            int pe, i;
            ret = shmem_internal_atomic(pWrk, (char*) source + offset, len, PE_start, op, datatype);
            shmem_long_wait_until(pSync, SHMEM_CMP_EQ, PE_size - 1);
            shmem_internal_put_wait(ret);
            ret = 0;
            pSync[0] = 0;
            memcpy((char*) target + offset, pWrk, len);
            memset(pWrk, 0, len);
            for (pe = PE_start + stride, i = 1 ; 
                 i < PE_size ;  
                 i++, pe += stride) {
                ret += shmem_internal_put((char*) target + offset, (char*) target + offset, len, pe);
                ret += shmem_internal_put(pSync, &tmp, sizeof(long), pe);
            }
            shmem_internal_put_wait(ret);
        } else {
            ret += shmem_internal_atomic(pWrk, (char*) source + offset, len, PE_start, op, datatype);
            ret += shmem_internal_put(pSync, &tmp, sizeof(long), PE_start);
            shmem_internal_put_wait(ret);
            shmem_long_wait(pSync, 0);
            pSync[0] = 0;
        }
    }
}


static inline
void
shmem_internal_bcast(void *target, const void *source, size_t len,
                int PE_root, int PE_start, int logPE_stride, int PE_size,
                long *pSync)
{ 
    /* BWB: make tree-based */
    int stride = (logPE_stride == 0) ? 1 : 1 << logPE_stride;
    int i, ret = 0;
    long tmp = 1;

    if (PE_root == shmem_int_my_pe) {
        for (i = PE_start ; i < PE_size ; i += stride) {
            if (i == shmem_int_my_pe && source == target) continue;
            ret += shmem_internal_put(target, source, len, i);
            if (i != shmem_int_my_pe) ret += shmem_internal_put(pSync, &tmp, sizeof(long), i);
        }
        shmem_internal_put_wait(ret);
    } else {
        shmem_long_wait(pSync, 0);
        pSync[0] = 0;
    }
}


static inline
void
shmem_internal_collect(void *target, const void *source, size_t len,
                  int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    /* BWB: make tree-based */
    int ret = 0;
    long tmp[2] = {0, 1};
    int stride = (logPE_stride == 0) ? 1 : 1 << logPE_stride;
    int pe;
    int bcast_len = 0;

    if (PE_size == 1) {
        if (target != source) memcpy(target, source, len);
        return;
    }

    /* collect in PE_start */
    if (PE_start == shmem_int_my_pe) {
        if (target != source) {
            ret += shmem_internal_put(target, source, len, PE_start);
        }
        tmp[0] = len;
        ret += shmem_internal_put(pSync, tmp, 2 * sizeof(long), PE_start + stride);
        shmem_long_wait(&pSync[1], 0);
        shmem_internal_put_wait(ret);
        bcast_len = pSync[0];
        pSync[0] = pSync[1] = 0;
    } else {
        shmem_long_wait(&pSync[1], 0);
        ret += shmem_internal_put((char*) target + pSync[0], source, len, PE_start);
        if (shmem_int_my_pe == PE_start + stride * (PE_size - 1)) {
            pe = PE_start;
        } else {
            pe = shmem_int_my_pe + stride;
        }
        tmp[0] = pSync[0] + len;
        pSync[0] = pSync[1] = 0;
        ret += shmem_internal_put(pSync, tmp, 2 * sizeof(long), PE_start);
        shmem_internal_put_wait(ret);
    }

    /* broadcast out */
    shmem_internal_bcast(target, target, bcast_len, PE_start, PE_start, logPE_stride, PE_size, pSync);
}


static inline
void
shmem_internal_fcollect(void *target, const void *source, size_t len,
                   int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    /* BWB: make tree-based */
    int ret = 0;
    long tmp = 1;
    int stride = (logPE_stride == 0) ? 1 : 1 << logPE_stride;

    if (PE_start == shmem_int_my_pe) {
        if (source != target) {
            ret += shmem_internal_put(target, source, len, PE_start);
        }
        shmem_long_wait_until(pSync, SHMEM_CMP_EQ, PE_size - 1);
        shmem_internal_put_wait(ret);
        pSync[0] = 0;
    } else {
        size_t offset = (shmem_int_my_pe - PE_start) / stride;
        ret += shmem_internal_put((char*) target + offset, source, len, PE_start);
        ret += shmem_internal_atomic(pSync, &tmp, sizeof(long), PE_start, PTL_SUM, PTL_LONG);
        shmem_internal_put_wait(ret);
    }
    
    shmem_internal_bcast(target, target, len * PE_size, PE_start, PE_start, logPE_stride, PE_size, pSync);
}


void
shmem_short_and_to_all(short *target, short *source, int nreduce,
                       int PE_start, int logPE_stride, int PE_size,
                       short *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(short),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LAND, PTL_SHORT);
}


void
shmem_int_and_to_all(int *target, int *source, int nreduce,
                     int PE_start, int logPE_stride, int PE_size,
                     int *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(int),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LAND, PTL_INT);
}


void
shmem_long_and_to_all(long *target, long *source, int nreduce,
                      int PE_start, int logPE_stride, int PE_size,
                      long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LAND, PTL_LONG);
}


void
shmem_longlong_and_to_all(long long *target, long long *source, int nreduce,
                          int PE_start, int logPE_stride, int PE_size,
                          long long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LAND, PTL_LONG);
}


void
shmem_short_or_to_all(short *target, short *source, int nreduce, 
                      int PE_start, int logPE_stride, int PE_size, 
                      short *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(short),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LOR, PTL_SHORT);
}


void
shmem_int_or_to_all(int *target, int *source, int nreduce, 
                    int PE_start, int logPE_stride, int PE_size, 
                    int *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(int),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LOR, PTL_INT);
}


void
shmem_long_or_to_all(long *target, long *source, int nreduce,
                     int PE_start, int logPE_stride, int PE_size, 
                     long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LOR, PTL_LONG);
}


void
shmem_longlong_or_to_all(long long *target, long long *source, int nreduce,
                         int PE_start, int logPE_stride, int PE_size, 
                         long long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LOR, PTL_LONG);
}


void
shmem_short_xor_to_all(short *target, short *source, int nreduce,
                       int PE_start, int logPE_stride, int PE_size, 
                       short *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(short),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LXOR, PTL_SHORT);
}

void
shmem_int_xor_to_all(int *target, int *source, int nreduce, 
                     int PE_start, int logPE_stride, int PE_size, 
                     int *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(int),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LXOR, PTL_INT);
}


void
shmem_long_xor_to_all(long *target, long *source, int nreduce,
                      int PE_start, int logPE_stride, int PE_size, 
                      long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LXOR, PTL_LONG);
}


void
shmem_longlong_xor_to_all(long long *target, long long *source, int nreduce,
                          int PE_start, int logPE_stride, int PE_size, 
                          long long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_LXOR, PTL_LONG);
}


void
shmem_float_min_to_all(float *target, float *source, int nreduce, 
                       int PE_start, int logPE_stride, int PE_size, 
                       float *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(float),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MIN, PTL_FLOAT);
}


void
shmem_double_min_to_all(double *target, double *source, int nreduce,
                        int PE_start, int logPE_stride, int PE_size, 
                        double *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(double),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MIN, PTL_DOUBLE);
}


void
shmem_longdouble_min_to_all(long double *target, long double *source, int nreduce, 
                            int PE_start, int logPE_stride, int PE_size,
                            long double *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long double),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MIN, PTL_LONG_DOUBLE);
}


void
shmem_short_min_to_all(short *target, short *source, int nreduce, 
                       int PE_start, int logPE_stride, int PE_size, 
                       short *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(short),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MIN, PTL_SHORT);
}


void
shmem_int_min_to_all(int *target, int *source, int nreduce, 
                     int PE_start, int logPE_stride, int PE_size, 
                     int *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(int),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MIN, PTL_INT);
}


void
shmem_long_min_to_all(long *target, long *source, int nreduce,
                      int PE_start, int logPE_stride, int PE_size, 
                      long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MIN, PTL_LONG);
}


void
shmem_longlong_min_to_all(long long *target, long long *source, int nreduce,
                          int PE_start, int logPE_stride, int PE_size,
                          long long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MIN, PTL_LONG);
}


void
shmem_float_max_to_all(float *target, float *source, int nreduce,
                       int PE_start, int logPE_stride, int PE_size,
                       float *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(float),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MAX, PTL_FLOAT);
}


void
shmem_double_max_to_all(double *target, double *source, int nreduce,
                        int PE_start, int logPE_stride, int PE_size,
                        double *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(double),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MAX, PTL_DOUBLE);
}


void
shmem_longdouble_max_to_all(long double *target, long double *source, int nreduce,
                            int PE_start, int logPE_stride, int PE_size,
                            long double *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long double),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MAX, PTL_LONG_DOUBLE);
}


void
shmem_short_max_to_all(short *target, short *source, int nreduce,
                       int PE_start, int logPE_stride, int PE_size,
                       short *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(short),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MAX, PTL_SHORT);
}


void
shmem_int_max_to_all(int *target, int *source, int nreduce,
                     int PE_start, int logPE_stride, int PE_size,
                     int *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(int),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MAX, PTL_INT);
}


void
shmem_long_max_to_all(long *target, long *source, int nreduce,
                      int PE_start, int logPE_stride, int PE_size,
                      long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MAX, PTL_LONG);
}


void 
shmem_longlong_max_to_all(long long *target, long long *source, int nreduce,
                          int PE_start, int logPE_stride, int PE_size,
                          long long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_MAX, PTL_LONG);
}


void
shmem_float_sum_to_all(float *target, float *source, int nreduce,
                       int PE_start, int logPE_stride, int PE_size, 
                       float *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(float),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_SUM, PTL_FLOAT);
}


void
shmem_double_sum_to_all(double *target, double *source, int nreduce, 
                        int PE_start, int logPE_stride, int PE_size, 
                        double *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(double),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_SUM, PTL_DOUBLE);
}


void
shmem_longdouble_sum_to_all(long double *target, long double *source, int nreduce,
                            int PE_start, int logPE_stride, int PE_size,
                            long double *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long double),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_SUM, PTL_LONG_DOUBLE);
}


void
shmem_complexf_sum_to_all(float complex *target, float complex *source, int nreduce, 
                          int PE_start, int logPE_stride, int PE_size,
                          float complex *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(float complex),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_SUM, PTL_FLOAT_COMPLEX);
}


void 
shmem_complexd_sum_to_all(double complex *target, double complex *source, int nreduce, 
                          int PE_start, int logPE_stride, int PE_size, 
                          double complex *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(double complex),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_SUM, PTL_DOUBLE_COMPLEX);
}


void
shmem_short_sum_to_all(short *target, short *source, int nreduce, 
                       int PE_start, int logPE_stride, int PE_size, 
                       short *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(short),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_SUM, PTL_SHORT);
}


void
shmem_int_sum_to_all(int *target, int *source, int nreduce, 
                     int PE_start, int logPE_stride, int PE_size, 
                     int *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(int),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_SUM, PTL_INT);
}


void
shmem_long_sum_to_all(long *target, long *source, int nreduce,
                      int PE_start, int logPE_stride, int PE_size, 
                      long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_SUM, PTL_LONG);
}


void
shmem_longlong_sum_to_all(long long *target, long long *source, int nreduce, 
                          int PE_start, int logPE_stride, int PE_size, 
                          long long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_SUM, PTL_LONG);
}


void
shmem_float_prod_to_all(float *target, float *source, int nreduce, 
                        int PE_start, int logPE_stride, int PE_size, 
                        float *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(float),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_PROD, PTL_FLOAT);
}


void
shmem_double_prod_to_all(double *target, double *source, int nreduce, 
                         int PE_start, int logPE_stride, int PE_size,
                         double *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(double),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_PROD, PTL_DOUBLE);
}


void
shmem_longdouble_prod_to_all(long double *target, long double *source, int nreduce, 
                             int PE_start, int logPE_stride, int PE_size,
                             long double *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long double),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_PROD, PTL_LONG_DOUBLE);
}


void
shmem_complexf_prod_to_all(float complex *target, float complex *source, int nreduce, 
                           int PE_start, int logPE_stride, int PE_size,
                           float complex *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(float complex),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_PROD, PTL_FLOAT_COMPLEX);
}


void
shmem_complexd_prod_to_all(double complex *target, double complex *source, int nreduce, 
                           int PE_start, int logPE_stride, int PE_size, 
                           double complex *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(double complex),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_PROD, PTL_DOUBLE_COMPLEX);
}


void
shmem_short_prod_to_all(short *target, short *source, int nreduce, 
                        int PE_start, int logPE_stride, int PE_size, 
                        short *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(short),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_PROD, PTL_SHORT);
}


void
shmem_int_prod_to_all(int *target, int *source, int nreduce,
                      int PE_start, int logPE_stride, int PE_size, 
                      int *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(int),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_PROD, PTL_INT);
}


void
shmem_long_prod_to_all(long *target, long *source, int nreduce,
                       int PE_start, int logPE_stride, int PE_size, 
                       long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_PROD, PTL_LONG);
}


void
shmem_longlong_prod_to_all(long long *target, long long *source, int nreduce, 
                           int PE_start, int logPE_stride, int PE_size,
                           long long *pWrk, long *pSync)
{
    shmem_internal_op_to_all(target, source, nreduce, sizeof(long long),
                    PE_start, logPE_stride, PE_size,
                    pWrk, pSync, PTL_PROD, PTL_LONG);
}


void
shmem_broadcast32(void *target, const void *source, size_t nlong, 
                  int PE_root, int PE_start, int logPE_stride, int PE_size,
                  long *pSync)
{
    shmem_internal_bcast(target, source, nlong * 4,
                    PE_root, PE_start, logPE_stride, PE_size,
                    pSync);
}


void
shmem_broadcast64(void *target, const void *source, size_t nlong,
                  int PE_root, int PE_start, int logPE_stride, int PE_size,
                  long *pSync)
{
    shmem_internal_bcast(target, source, nlong * 8,
                    PE_root, PE_start, logPE_stride, PE_size,
                    pSync);
}


void
shmem_collect32(void *target, const void *source, size_t nlong,
                int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    shmem_internal_collect(target, source, nlong * 4,
                      PE_start, logPE_stride, PE_size, pSync);
}


void
shmem_collect64(void *target, const void *source, size_t nlong,
                int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    shmem_internal_collect(target, source, nlong * 8,
                      PE_start, logPE_stride, PE_size, pSync);
}


void
shmem_fcollect32(void *target, const void *source, size_t nlong,
                 int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    shmem_internal_fcollect(target, source, nlong * 4,
                       PE_start, logPE_stride, PE_size, pSync);
}


void
shmem_fcollect64(void *target, const void *source, size_t nlong,
                 int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    shmem_internal_fcollect(target, source, nlong * 8,
                       PE_start, logPE_stride, PE_size, pSync);
}
