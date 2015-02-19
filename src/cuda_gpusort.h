/*
 * opencl_gpupreagg.h
 *
 * Preprocess of aggregate using GPU acceleration, to reduce number of
 * rows to be processed by CPU; including the Sort reduction.
 * --
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef OPENCL_GPUSORT_H
#define OPENCL_GPUSORT_H

/*
 * GPU Accelerated Sorting
 *
 * It packs kern_parambu, status field, and kern_row_map structure
 * within a continuous memory area, to translate this chunk with
 * a single DMA call.
 *
 * +----------------+
 * | kern_parambuf  |
 * | +--------------+
 * | | length   o---------+
 * | +--------------+     | kern_row_map is located just after
 * | | nparams      |     | the kern_parambuf (because of DMA
 * | +--------------+     | optimization), so head address of
 * | | poffset[0]   |     | kern_gpuscan + parambuf.length
 * | | poffset[1]   |     | points kern_row_map.
 * | |    :         |     |
 * | | poffset[M-1] |     |
 * | +--------------+     |
 * | | variable     |     |
 * | | length field |     |
 * | | for Param /  |     |
 * | | Const values |     |
 * | |     :        |     |
 * +-+--------------+ <---+
 * | kern_resultbuf |
 * | +--------------+
 * | | nrels (=2)   |
 * | +--------------+
 * | | nrooms       |
 * | +--------------+
 * | | nitems       |
 * | +--------------+
 * | | errcode      |
 * | +--------------+
 * | | has_rechecks |
 * | +--------------+
 * | | all_visible  |
 * | +--------------+
 * | | __padding__[]|
 * | +--------------+
 * | | results[0]   | A pair of results identify the records being sorted.
 * | | results[1]   | result[even number] indicates chunk_id.
 * | +--------------+   (It is always same in a single kernel execution)
 * | | results[2]   | result[odd number] indicated item_id; that is index
 * | | results[3]   |   of a row within a sorting chunk
 * | +--------------+
 * | |     :        |
 * +-+--------------+  -----
 */
typedef struct
{
	kern_parambuf	kparams;
	/* kern_resultbuf (nrels = 2) shall be located next to the kparams */
} kern_gpusort;

#define KERN_GPUSORT_PARAMBUF(kgpusort)				\
	((__global kern_parambuf *)(&(kgpusort)->kparams))
#define KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort)		\
	(KERN_GPUSORT_PARAMBUF(kgpusort)->length)
#define KERN_GPUSORT_RESULTBUF(kgpusort)			\
	((__global kern_resultbuf *)					\
	 ((__global char *)&(kgpusort)->kparams +		\
	  STROMALIGN((kgpusort)->kparams.length)))
#define KERN_GPUSORT_RESULTBUF_LENGTH(kgpusort)			\
	STROMALIGN(offsetof(kern_resultbuf,					\
		results[KERN_GPUSORT_RESULTBUF(kgpusort)->nrels *	\
				KERN_GPUSORT_RESULTBUF(kgpusort)->nrooms]))
#define KERN_GPUSORT_LENGTH(kgpusort)				\
	(offsetof(kern_gpusort, kparams) +				\
	 KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort) +		\
	 KERN_GPUSORT_RESULTBUF_LENGTH(kgpusort))
#define KERN_GPUSORT_DMASEND_OFFSET(kgpusort)		\
	offsetof(kern_gpusort, kparams)
#define KERN_GPUSORT_DMASEND_LENGTH(kgpusort)		\
	(KERN_GPUSORT_LENGTH(kgpusort) -				\
	 offsetof(kern_gpusort, kparams))
#define KERN_GPUSORT_DMARECV_OFFSET(kgpusort)		\
	((uintptr_t)KERN_GPUSORT_RESULTBUF(kgpusort) -	\
	 (uintptr_t)(kgpusort))
#define KERN_GPUSORT_DMARECV_LENGTH(kgpusort)		\
	KERN_GPUSORT_RESULTBUF_LENGTH(kgpusort)

#ifdef OPENCL_DEVICE_CODE
/*
 * Sorting key comparison function - to be generated by PG-Strom
 * on the fly.
 */
static cl_int gpusort_keycomp(__private cl_int *errcode,
							  __global kern_data_store *kds,
							  __global kern_data_store *ktoast,
							  size_t x_index,
							  size_t y_index);
/*
 * Projection of sorting key - to be generated by PG-Strom on the fly.
 */
static void gpusort_projection(__private cl_int *errcode,
							   __global Datum *ts_values,
							   __global cl_char *ts_isnull,
							   __global kern_data_store *ktoast,
							   __global HeapTupleHeaderData *htup);
/*
 * Fixup special internal variables (numeric, at this moment)
 */
static void gpusort_fixup_variables(__private cl_int *errcode,
									__global Datum *ts_values,
									__global cl_char *ts_isnull,
									__global kern_data_store *ktoast,
									__global HeapTupleHeaderData *htup);

/*
 * gpusort_preparation - fill up krowmap->rindex array and setup
 * kds (tupslot format) according to the ktoast (row-flat format)
 */
__kernel void
gpusort_preparation(__global kern_gpusort *kgpusort,
					__global kern_data_store *kds,
					__global kern_data_store *ktoast,
					cl_int chunk_id,
					KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	size_t		nitems = ktoast->nitems;
	size_t		ncols;
	size_t		index;
	int			errcode = StromError_Success;

	/* sanity checks */
	if (kresults->nrels != 2 ||
		kresults->nitems != nitems ||
		ktoast->format != KDS_FORMAT_ROW_FMAP ||
		kds->format != KDS_FORMAT_TUPSLOT)
	{
		STROM_SET_ERROR(&errcode, StromError_DataStoreCorruption);
		goto out;
	}
	if (kds->nrooms < nitems)
	{
		STROM_SET_ERROR(&errcode, StromError_DataStoreNoSpace);
		goto out;
	}

	/* kds also has same nitems */
	if (get_global_id(0) == 0)
		kds->nitems = nitems;

	/* put initial value of row-index */
	for (index = get_global_id(0);
		 index < nitems;
		 index += get_global_size(0))
	{
		kresults->results[2 * index] = chunk_id;
		kresults->results[2 * index + 1] = index;
	}

	/* projection of kds */
	if (get_global_id(0) < nitems)
	{
		__global HeapTupleHeaderData *htup;
		__global Datum	   *ts_values;
		__global cl_char   *ts_isnull;

		htup = kern_get_tuple_rsflat(ktoast, get_global_id(0));
		if (!htup)
		{
			STROM_SET_ERROR(&errcode, StromError_DataStoreCorruption);
			goto out;
		}
		ts_values = KERN_DATA_STORE_VALUES(kds, get_global_id(0));
		ts_isnull = KERN_DATA_STORE_ISNULL(kds, get_global_id(0));
		gpusort_projection(&errcode,
						   ts_values,
						   ts_isnull,
						   ktoast, htup);
	}
out:
	kern_writeback_error_status(&kresults->errcode, errcode, LOCAL_WORKMEM);
}

/*
 * gpusort_bitonic_local
 *
 * It tries to apply each steps of bitonic-sorting until its unitsize
 * reaches the workgroup-size (that is expected to power of 2).
 */
__kernel void
gpusort_bitonic_local(__global kern_gpusort *kgpusort,
					  __global kern_data_store *kds,
					  __global kern_data_store *ktoast,
					  KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	__local cl_int *localIdx = LOCAL_WORKMEM;
	cl_int			errcode = StromError_Success;
	cl_uint			nitems = kds->nitems;
	size_t			localID = get_local_id(0);
	size_t			globalID = get_global_id(0);
	size_t			localSize = get_local_size(0);
	size_t			prtID = globalID / localSize;	/* partition ID */
	size_t			prtSize = localSize * 2;		/* partition Size */
	size_t			prtPos = prtID * prtSize;		/* partition Position */
	size_t			localEntry;
	size_t			blockSize;
	size_t			unitSize;
	size_t			i;

	/* create row index and then store to localIdx */
	localEntry = ((prtPos + prtSize < nitems) ? prtSize : (nitems - prtPos));
	for (i = localID; i < localEntry; i += localSize)
		localIdx[i] = prtPos + i;
    barrier(CLK_LOCAL_MEM_FENCE);

	/* bitonic sorting */
	for (blockSize = 2; blockSize <= prtSize; blockSize *= 2)
	{
		for (unitSize = blockSize; unitSize >= 2; unitSize /= 2)
        {
			size_t	unitMask		= unitSize - 1;
			size_t	halfUnitSize	= unitSize / 2;
			bool	reversing  = (unitSize == blockSize ? true : false);
			size_t	idx0 = ((localID / halfUnitSize) * unitSize
							+ localID % halfUnitSize);
            size_t	idx1 = ((reversing == true)
							? ((idx0 & ~unitMask) | (~idx0 & unitMask))
							: (halfUnitSize + idx0));

            if(idx1 < localEntry)
			{
				cl_int	pos0 = localIdx[idx0];
				cl_int	pos1 = localIdx[idx1];

				if (gpusort_keycomp(&errcode, kds, ktoast, pos0, pos1) > 0)
				{
					/* swap them */
					localIdx[idx0] = pos1;
					localIdx[idx1] = pos0;
				}
			}
			barrier(CLK_LOCAL_MEM_FENCE);
		}
	}
	/* write back local sorted result */
	for (i=localID; i < localEntry; i+=localSize)
		kresults->results[2 * (prtPos + i) + 1] = localIdx[i];
	barrier(CLK_LOCAL_MEM_FENCE);

	/* any error during run-time? */
	kern_writeback_error_status(&kresults->errcode, errcode, LOCAL_WORKMEM);
}

/*
 * gpusort_bitonic_step
 *
 * It tries to apply individual steps of bitonic-sorting for each step,
 * but does not have restriction of workgroup size. The host code has to
 * control synchronization of each step not to overrun.
 */
__kernel void
gpusort_bitonic_step(__global kern_gpusort *kgpusort,
					 cl_int bitonic_unitsz,
					 __global kern_data_store *kds,
					 __global kern_data_store *ktoast,
					 KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	cl_int		errcode = StromError_Success;
	cl_bool		reversing = (bitonic_unitsz < 0 ? true : false);
	size_t		unitsz = (bitonic_unitsz < 0
						  ? -bitonic_unitsz
						  : bitonic_unitsz);
	cl_uint		nitems = kds->nitems;
	size_t		globalID = get_global_id(0);
	size_t		halfUnitSize = unitsz / 2;
	size_t		unitMask = unitsz - 1;
	cl_int		idx0, idx1;
	cl_int		pos0, pos1;

	idx0 = (globalID / halfUnitSize) * unitsz + globalID % halfUnitSize;
	idx1 = (reversing
			? ((idx0 & ~unitMask) | (~idx0 & unitMask))
			: (idx0 + halfUnitSize));
	if (idx1 >= nitems)
		goto out;

	pos0 = kresults->results[2 * idx0 + 1];
	pos1 = kresults->results[2 * idx1 + 1];
	if (gpusort_keycomp(&errcode, kds, ktoast, pos0, pos1) > 0)
	{
		/* swap them */
		kresults->results[2 * idx0 + 1] = pos1;
		kresults->results[2 * idx1 + 1] = pos0;
	}
out:
	kern_writeback_error_status(&kresults->errcode, errcode, LOCAL_WORKMEM);
}

/*
 * gpusort_bitonic_merge
 *
 * It handles the merging step of bitonic-sorting if unitsize becomes less
 * than or equal to the workgroup size.
 */
__kernel void
gpusort_bitonic_merge(__global kern_gpusort *kgpusort,
					  __global kern_data_store *kds,
					  __global kern_data_store *ktoast,
					  KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	__local cl_int *localIdx = LOCAL_WORKMEM;
	cl_int			errcode = StromError_Success;
	cl_uint			nitems = kds->nitems;
    size_t			localID = get_local_id(0);
    size_t			globalID = get_global_id(0);
    size_t			localSize = get_local_size(0);
	size_t			prtID = globalID / localSize;	/* partition ID */
	size_t			prtSize = 2 * localSize;		/* partition Size */
	size_t			prtPos = prtID * prtSize;		/* partition Position */
	size_t			localEntry;
	size_t			blockSize = prtSize;
	size_t			unitSize = prtSize;
	size_t			i;

	/* Load index to localIdx[] */
	localEntry = (prtPos+prtSize < nitems) ? prtSize : (nitems-prtPos);
	for (i = localID; i < localEntry; i += localSize)
		localIdx[i] = kresults->results[2 * (prtPos + i) + 1];
	barrier(CLK_LOCAL_MEM_FENCE);

	/* merge two sorted blocks */
	for (unitSize = blockSize; unitSize >= 2; unitSize /= 2)
	{
		size_t	halfUnitSize = unitSize / 2;
		size_t	idx0, idx1;

		idx0 = localID / halfUnitSize * unitSize + localID % halfUnitSize;
		idx1 = halfUnitSize + idx0;

        if (idx1 < localEntry)
		{
			size_t	pos0 = localIdx[idx0];
			size_t	pos1 = localIdx[idx1];

			if (gpusort_keycomp(&errcode, kds, ktoast, pos0, pos1) > 0)
			{
				/* swap them */
				localIdx[idx0] = pos1;
                localIdx[idx1] = pos0;
			}
		}
		barrier(CLK_LOCAL_MEM_FENCE);
	}
	/* Save index to kresults[] */
	for (i = localID; i < localEntry; i += localSize)
		kresults->results[2 * (prtPos + i) + 1] = localIdx[i];
	barrier(CLK_LOCAL_MEM_FENCE);

	kern_writeback_error_status(&kresults->errcode, errcode, LOCAL_WORKMEM);
}

__kernel void
gpusort_fixup_datastore(__global kern_gpusort *kgpusort,
						__global kern_data_store *kds,
						__global kern_data_store *ktoast,
						KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	int			errcode = StromError_Success;

	if (get_global_id(0) < kds->nitems)
	{
		__global HeapTupleHeaderData *htup;
		__global Datum	   *ts_values;
		__global cl_char   *ts_isnull;

		htup = kern_get_tuple_rsflat(ktoast, get_global_id(0));
		if (!htup)
			STROM_SET_ERROR(&errcode, StromError_DataStoreCorruption);
		else
		{
			ts_values = KERN_DATA_STORE_VALUES(kds, get_global_id(0));
			ts_isnull = KERN_DATA_STORE_ISNULL(kds, get_global_id(0));
			gpusort_fixup_variables(&errcode,
									ts_values,
									ts_isnull,
									ktoast, htup);
		}
	}
	kern_writeback_error_status(&kresults->errcode, errcode, LOCAL_WORKMEM);
}

#else
/* Host side representation of kern_gpusort. It performs as a message
 * object of PG-Strom, has a key of OpenCL device program, a file mapped
 * data-store (because it tends to consume massive RAM) and kern_row_map
 * that is expected to store the index of records.
 */
typedef struct
{
	pgstrom_message		msg;
	Datum				dprog_key;
	cl_int				chunk_id;
	pgstrom_data_store *pds;	/* source data store (file mapped row-store) */
	kern_gpusort		kern;
} pgstrom_gpusort;

#endif	/* OPENCL_DEVICE_CODE */
#endif	/* OPENCL_GPUSORT_H */
