// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018-2019 The Linux Foundation. All rights reserved. */

#include <linux/ascii85.h>
#include "msm_gem.h"
#include "a6xx_gpu.h"
#include "a6xx_gmu.h"
#include "a6xx_gmu.xml.h"
#include "a8xx_gpu_state.h"

static struct gen8_reg_list gen8_2_0_gmu_gx_registers[] __always_unused;
static const u32 *gen8_2_0_external_core_regs[] __always_unused;
static struct gen8_sptp_cluster_registers gen8_2_0_sptp_clusters[] __always_unused;
static struct gen8_cluster_registers gen8_2_0_mvc_clusters[] __always_unused;
static struct gen8_cluster_registers gen8_2_0_cp_clusters[] __always_unused;
static const u32 gen8_2_0_debugbus_blocks[] __always_unused;
static const char *a8xx_cluster_names[] __always_unused;
static const struct a8xx_debugbus_block a8xx_cx_debugbus_blocks[] __always_unused;
static const u32 a8xx_gbif_debugbus_blocks[] __always_unused;
static const struct a8xx_debugbus_block a8xx_debugbus_blocks[] __always_unused;


#include "adreno_gen8_2_0_snapshot.h"

#define RANGE(reg, a) ((reg)[(a) + 1] - (reg)[(a)] + 1)

#define NUM_SLICES(gpu)  (hweight32((gpu)->slice_mask))

static inline int next_slice(int slice, u32 slice_mask)
{
	for (; slice < fls(slice_mask); slice++) {
		if (BIT(slice) & slice_mask)
			return slice;
	}

	return slice;
}

#define NEXT_SLICE(_slice, _region, _slice_mask) \
	((_region == UNSLICE) ? fls(_slice_mask) : next_slice(_slice, _slice_mask))

#define FIRST_SLICE(_region, _slice_mask) \
	((_region == UNSLICE) ? 0 : NEXT_SLICE(0, _region, _slice_mask))

#define FOR_EACH_SLICE(_slice, _region, _slice_mask) \
	for (_slice = FIRST_SLICE(_region, _slice_mask); \
		_slice < fls(_slice_mask); \
		_slice = NEXT_SLICE(++(_slice), _region, _slice_mask))

static void _aperture_slice_set(struct msm_gpu *gpu, enum adreno_pipe pipe,
		u32 slice, bool use_slice)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	u32 val;

	val = A8XX_CP_APERTURE_CNTL_HOST_PIPEID(pipe) | A8XX_CP_APERTURE_CNTL_HOST_SLICEID(slice);

	if (use_slice)
		val |= A8XX_CP_APERTURE_CNTL_HOST_USESLICEID;

	if (a6xx_gpu->cached_aperture == val)
		return;

	gpu_write(gpu, REG_A8XX_CP_APERTURE_CNTL_HOST, val);
	mb();

	a6xx_gpu->cached_aperture = val;
}

struct a8xx_gpu_state_obj {
	const void *handle;
	u32 *data;
	u32 count;	/* optional, used when count potentially read from hw */
	u32 slice;
};

struct a8xx_gpu_state {
	struct msm_gpu_state base;

	struct a8xx_gpu_state_obj *gmu_registers;
	int nr_gmu_registers;

	struct a8xx_gpu_state_obj *registers;
	int nr_registers;

	struct a8xx_gpu_state_obj *shaders;
	int nr_shaders;

	struct a8xx_gpu_state_obj *clusters;
	int nr_clusters;

	struct a8xx_gpu_state_obj *dbgahb_clusters;
	int nr_dbgahb_clusters;

	struct a8xx_gpu_state_obj *indexed_regs;
	int nr_indexed_regs;

	struct a8xx_gpu_state_obj *debugbus;
	int nr_debugbus;

	struct a8xx_gpu_state_obj *vbif_debugbus;

	struct a8xx_gpu_state_obj *cx_debugbus;
	int nr_cx_debugbus;

	struct msm_gpu_state_bo *gmu_log;
	struct msm_gpu_state_bo *gmu_hfi;
	struct msm_gpu_state_bo *gmu_debug;

	s32 hfi_queue_history[2][HFI_HISTORY_SZ];

	struct list_head objs;

	bool gpu_initialized;
};

static inline int CRASHDUMP_WRITE(u64 *in, u32 reg, u32 val)
{
	in[0] = val;
	in[1] = (((u64) reg) << 44 | (1 << 21) | 1);

	return 2;
}

static inline int CRASHDUMP_READ(u64 *in, u32 reg, u32 dwords, u64 target)
{
	in[0] = target;
	in[1] = (((u64) reg) << 44 | dwords);

	return 2;
}

static inline int CRASHDUMP_FINI(u64 *in)
{
	in[0] = 0;
	in[1] = 0;

	return 2;
}

struct a8xx_crashdumper {
	void *ptr;
	struct drm_gem_object *bo;
	u64 iova;
};

struct a8xx_state_memobj {
	struct list_head node;
	unsigned long long data[];
};

static void *state_kcalloc(struct a8xx_gpu_state *a8xx_state, int nr, size_t objsize)
{
	struct a8xx_state_memobj *obj =
		kvzalloc((nr * objsize) + sizeof(*obj), GFP_KERNEL);

	if (!obj)
		return NULL;

	list_add_tail(&obj->node, &a8xx_state->objs);
	return &obj->data;
}

static void *state_kmemdup(struct a8xx_gpu_state *a8xx_state, void *src,
		size_t size)
{
	void *dst = state_kcalloc(a8xx_state, 1, size);

	if (dst)
		memcpy(dst, src, size);
	return dst;
}

/*
 * Allocate 1MB for the crashdumper scratch region - 8k for the script and
 * the rest for the data
 */
#define A8XX_CD_DATA_OFFSET 8192
#define A8XX_CD_DATA_SIZE  (SZ_1M - 8192)

static int a8xx_crashdumper_init(struct msm_gpu *gpu,
		struct a8xx_crashdumper *dumper)
{
	dumper->ptr = msm_gem_kernel_new(gpu->dev,
		SZ_1M, MSM_BO_WC, gpu->vm,
		&dumper->bo, &dumper->iova);

	if (!IS_ERR(dumper->ptr))
		msm_gem_object_set_name(dumper->bo, "crashdump");

	return PTR_ERR_OR_ZERO(dumper->ptr);
}

static int a8xx_crashdumper_run(struct msm_gpu *gpu,
		struct a8xx_crashdumper *dumper)
{
	u32 val;
	int ret;

	if (IS_ERR_OR_NULL(dumper->ptr))
		return -EINVAL;

	/* Make sure all pending memory writes are posted */
	wmb();

	gpu_write64(gpu, REG_A8XX_CP_CRASH_DUMP_SCRIPT_BASE, dumper->iova);

	gpu_write(gpu, REG_A8XX_CP_CRASH_DUMP_CNTL, 1);

	ret = gpu_poll_timeout(gpu, REG_A8XX_CP_CRASH_DUMP_STATUS, val,
		val & 0x02, 100, 10000);

	gpu_write(gpu, REG_A8XX_CP_CRASH_DUMP_CNTL, 0);

	return ret;
}

static void a8xx_get_shader_block(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state,
		const struct gen8_shader_block *block,
		u32 slice_id,
		struct a8xx_gpu_state_obj *obj,
		struct a8xx_crashdumper *dumper)
{
	size_t datasize = block->size * block->num_sps * block->num_ctx * sizeof(u32);
	u64 out = dumper->iova + A8XX_CD_DATA_OFFSET;
	u64 *in = dumper->ptr;
	int i, j;

	if (WARN_ON(datasize > A8XX_CD_DATA_SIZE))
		return;

	for (i = 0; i < block->num_sps; i++) {
		for (j = 0; j < block->num_ctx; j++) {
			in += CRASHDUMP_WRITE(in, REG_A7XX_SP_READ_SEL,
				A7XX_SP_READ_SEL_CONTEXT(j) |
				A7XX_SP_READ_SEL_SLICE(slice_id) |
				A7XX_SP_READ_SEL_LOCATION(block->location) |
				A7XX_SP_READ_SEL_PIPE(block->pipeid) |
				A7XX_SP_READ_SEL_STATETYPE(block->statetype) |
				A7XX_SP_READ_SEL_USPTP(block->usptp_id) |
				A7XX_SP_READ_SEL_SPTP(i));

			in += CRASHDUMP_READ(in, REG_A7XX_SP_AHB_READ_APERTURE,
				block->size, out);

			out += block->size * sizeof(u32);
		}
	}

	CRASHDUMP_FINI(in);

	if (a8xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = block;
	obj->slice = slice_id;
	obj->data = state_kmemdup(a8xx_state, dumper->ptr + A8XX_CD_DATA_OFFSET,
		datasize);
}

static void a8xx_get_shaders(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state,
		struct a8xx_crashdumper *dumper)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	const struct gen8_shader_block *shader_blocks;
	unsigned num_shader_blocks;
	int i, slice;

	shader_blocks = gen8_2_0_shader_blocks;
	num_shader_blocks = ARRAY_SIZE(gen8_2_0_shader_blocks);

	a8xx_state->shaders = state_kcalloc(a8xx_state,
		num_shader_blocks, sizeof(*a8xx_state->shaders));

	if (!a8xx_state->shaders)
		return;

	a8xx_state->nr_shaders = num_shader_blocks;

	for (i = 0; i < num_shader_blocks; i++) {
		FOR_EACH_SLICE(slice, shader_blocks[i].slice_region, a6xx_gpu->slice_mask)
			a8xx_get_shader_block(gpu, a8xx_state, &shader_blocks[i],
				slice, &a8xx_state->shaders[i], dumper);
	}
}

static void a8xx_get_crashdumper_registers(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state,
		const struct gen8_reg_list *regs,
		u32 slice_id,
		struct a8xx_gpu_state_obj *obj,
		struct a8xx_crashdumper *dumper)

{
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A8XX_CD_DATA_OFFSET;
	int i, regcount = 0;

	for (i = 0; regs->regs[i] != UINT_MAX; i += 2) {
		u32 count = RANGE(regs->regs, i);

		in += CRASHDUMP_READ(in, regs->regs[i], count, out);

		out += count * sizeof(u32);
		regcount += count;
	}

	CRASHDUMP_FINI(in);

	if (WARN_ON((regcount * sizeof(u32)) > A8XX_CD_DATA_SIZE))
		return;

	if (a8xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = regs;
	obj->slice = slice_id;
	obj->data = state_kmemdup(a8xx_state, dumper->ptr + A8XX_CD_DATA_OFFSET,
		regcount * sizeof(u32));
}


static void a8xx_get_ahb_gpu_registers(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state,
		const u32 *regs,
		u32 slice_id,
		struct a8xx_gpu_state_obj *obj)
{
	int i, regcount = 0, index = 0;

	for (i = 0; regs[i] != UINT_MAX; i += 2)
		regcount += RANGE(regs, i);

	obj->data = state_kcalloc(a8xx_state, regcount, sizeof(u32));
	if (!obj->data)
		return;

	for (i = 0; regs[i] != UINT_MAX; i += 2) {
		u32 count = RANGE(regs, i);
		int j;

		for (j = 0; j < count; j++)
			obj->data[index++] = gpu_read(gpu, regs[i] + j);
	}
}

static void a8xx_get_ahb_gpu_reglist(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state,
		const struct gen8_reg_list *regs,
		u32 slice_id,
		struct a8xx_gpu_state_obj *obj)
{
	if (regs->slice_region) {
		_aperture_slice_set(gpu, slice_id, 0, false);
		obj->handle = regs;
		obj->slice = slice_id;
	}

	a8xx_get_ahb_gpu_registers(gpu, a8xx_state, regs->regs, slice_id, obj);
}

/* Read a block of GMU registers */
static void _a8xx_get_gmu_registers(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state,
		const u32 *regs,
		struct a8xx_gpu_state_obj *obj)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct a6xx_gmu *gmu = &a6xx_gpu->gmu;
	int i, regcount, index = 0;

	for (i = 0; regs[i] != UINT_MAX; i += 2)
		regcount += RANGE(regs, i);

	obj->handle = (const void *) regs;
	obj->data = state_kcalloc(a8xx_state, regcount, sizeof(u32));
	if (!obj->data)
		return;

	for (i = 0; regs[i] != UINT_MAX; i += 2) {
		u32 count = RANGE(regs, i);
		int j;

		for (j = 0; j < count; j++) {
			u32 offset = regs[i] + j;
			u32 val;

			val = gmu_read(gmu, offset);

			obj->data[index++] = val;
		}
	}
}

static void a8xx_get_gmu_registers(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state)
{
	const u32 *cx_regs = gen8_2_0_gmucx_registers;

	a8xx_state->gmu_registers = state_kcalloc(a8xx_state,
		1, sizeof(*a8xx_state->gmu_registers));

	if (!a8xx_state->gmu_registers)
		return;

	a8xx_state->nr_gmu_registers = 1;

	/* Get the CX GMU registers from AHB */
	_a8xx_get_gmu_registers(gpu, a8xx_state, cx_regs,
		&a8xx_state->gmu_registers[0]);

	/* TODO: Dump other gmu regions */
}

static struct msm_gpu_state_bo *a8xx_snapshot_gmu_bo(
		struct a8xx_gpu_state *a8xx_state, struct a6xx_gmu_bo *bo)
{
	struct msm_gpu_state_bo *snapshot;

	if (!bo->size)
		return NULL;

	snapshot = state_kcalloc(a8xx_state, 1, sizeof(*snapshot));
	if (!snapshot)
		return NULL;

	snapshot->iova = bo->iova;
	snapshot->size = bo->size;
	snapshot->data = kvzalloc(snapshot->size, GFP_KERNEL);
	if (!snapshot->data)
		return NULL;

	memcpy(snapshot->data, bo->virt, bo->size);

	return snapshot;
}

static void a8xx_snapshot_gmu_hfi_history(struct msm_gpu *gpu,
					  struct a8xx_gpu_state *a8xx_state)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct a6xx_gmu *gmu = &a6xx_gpu->gmu;
	unsigned i, j;

	BUILD_BUG_ON(ARRAY_SIZE(gmu->queues) != ARRAY_SIZE(a8xx_state->hfi_queue_history));

	for (i = 0; i < ARRAY_SIZE(gmu->queues); i++) {
		struct a6xx_hfi_queue *queue = &gmu->queues[i];
		for (j = 0; j < HFI_HISTORY_SZ; j++) {
			unsigned idx = (j + queue->history_idx) % HFI_HISTORY_SZ;
			a8xx_state->hfi_queue_history[i][j] = queue->history[idx];
		}
	}
}

static void a8xx_get_registers(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state,
		struct a8xx_crashdumper *dumper)
{
	const struct gen8_reg_list *pre_crashdumper_reglist = gen8_2_0_ahb_registers;
	const struct gen8_reg_list *reglist = gen8_2_0_misc_registers;
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	int i, count, index = 0;
	u32 slice;

	count = ARRAY_SIZE(gen8_2_0_misc_registers) + ARRAY_SIZE(gen8_2_0_ahb_registers);

	/* The downstream reglist contains registers in other memory regions
	 * (cx_misc/cx_mem and cx_dbgc) and we need to plumb through their
	 * offsets and map them to read them on the CPU. For now only read the
	 * first region which is the main one.
	 */
	if (dumper) {
		for (i = 0; reglist[i].regs; i++)
			FOR_EACH_SLICE(slice, reglist[i].slice_region, a6xx_gpu->slice_mask)
				count++;
	} else {
		FOR_EACH_SLICE(slice, reglist[i].slice_region, a6xx_gpu->slice_mask)
			count++;
	}

	for (i = 0; pre_crashdumper_reglist[i].regs; i++)
		FOR_EACH_SLICE(slice, pre_crashdumper_reglist[i].slice_region, a6xx_gpu->slice_mask)
			count++;

	a8xx_state->registers = state_kcalloc(a8xx_state,
		count, sizeof(*a8xx_state->registers));

	if (!a8xx_state->registers)
		return;

	a8xx_state->nr_registers = count;

	for (i = 0; pre_crashdumper_reglist[i].regs; i++) {
		FOR_EACH_SLICE(slice, pre_crashdumper_reglist[i].slice_region, a6xx_gpu->slice_mask) {
			a8xx_get_ahb_gpu_reglist(gpu, a8xx_state,
					pre_crashdumper_reglist, slice,
					&a8xx_state->registers[index++]);
		}
	}

	if (!dumper) {
		FOR_EACH_SLICE(slice, reglist[i].slice_region, a6xx_gpu->slice_mask)
			a8xx_get_ahb_gpu_reglist(gpu,
				a8xx_state, &reglist[0], slice,
				&a8xx_state->registers[index++]);
		return;
	}

	for (i = 0; reglist[i].regs; i++)
		FOR_EACH_SLICE(slice, reglist[i].slice_region, a6xx_gpu->slice_mask)
			a8xx_get_crashdumper_registers(gpu,
				a8xx_state, &reglist[i], slice,
				&a8xx_state->registers[index++],
				dumper);
}

/* Read a block of data from an indexed register pair */
static void a8xx_get_indexed_regs(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state,
		const struct a8xx_indexed_registers *indexed,
		u32 slice,
		struct a8xx_gpu_state_obj *obj)
{
	u32 count = indexed->count;
	int i;

	obj->handle = (const void *) indexed;
	if (indexed->count_fn)
		count = indexed->count_fn(gpu);

	obj->data = state_kcalloc(a8xx_state, count, sizeof(u32));
	obj->slice = slice;
	obj->count = count;
	if (!obj->data)
		return;

	/* All the indexed banks start at address 0 */
	gpu_write(gpu, indexed->addr, 0);

	/* Read the data - each read increments the internal address by 1 */
	for (i = 0; i < count; i++)
		obj->data[i] = gpu_read(gpu, indexed->data);
}

static void a8xx_get_mempool(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state,
		const struct a8xx_indexed_registers *indexed_regs,
		u32 slice,
		struct a8xx_gpu_state_obj *obj)
{
	/* TODO: Remove hardcoded values */
	/* Set CP_CHICKEN_DBG[StabilizeMVC] to stabilize it while dumping */
	_aperture_slice_set(gpu, indexed_regs->pipe_id, 0, false);
	gpu_rmw(gpu, REG_A8XX_CP_CHICKEN_DBG_PIPE, BIT(2), BIT(2));

	_aperture_slice_set(gpu, indexed_regs->pipe_id, slice, true);
	gpu_rmw(gpu, REG_A8XX_CP_SLICE_CHICKEN_DBG_PIPE, BIT(2), BIT(2));

	_aperture_slice_set(gpu, indexed_regs->pipe_id, slice, false);
	/* Get the contents of the CP_BV mempool */
	a8xx_get_indexed_regs(gpu, a8xx_state, indexed_regs, slice, obj);

	/* Reset CP_CHICKEN_DBG[StabilizeMVC] once we are done */
	_aperture_slice_set(gpu, indexed_regs->pipe_id, 0, false);
	gpu_rmw(gpu, REG_A8XX_CP_CHICKEN_DBG_PIPE, BIT(2), 0);

	_aperture_slice_set(gpu, indexed_regs->pipe_id, slice, true);
	gpu_rmw(gpu, REG_A8XX_CP_SLICE_CHICKEN_DBG_PIPE, BIT(2), 0);
}

static void a8xx_get_indexed_registers(struct msm_gpu *gpu,
		struct a8xx_gpu_state *a8xx_state)
{
	const struct a8xx_indexed_registers *indexed_regs, *mempool_regs;
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	int i, indexed_count, mempool_count, idx = 0;
	u32 slice;

	indexed_regs = gen8_2_0_cp_indexed_reg_list;
	mempool_regs = gen8_2_0_cp_mempool_reg_list;

	indexed_count = ARRAY_SIZE(gen8_2_0_cp_indexed_reg_list);
	/* TODO: Add back mempools */
	mempool_count = 0; //ARRAY_SIZE(gen8_2_0_cp_mempool_reg_list);

	a8xx_state->nr_indexed_regs = indexed_count + (mempool_count * NUM_SLICES(a6xx_gpu));
	a8xx_state->indexed_regs = state_kcalloc(a8xx_state,
			a8xx_state->nr_indexed_regs,
			sizeof(*a8xx_state->indexed_regs));
	if (!a8xx_state->indexed_regs)
		return;

	/* First read the common regs */
	for (i = 0; i < indexed_count; i++) {
		a8xx_aperture_set(gpu, indexed_regs[i].pipe_id);
		a8xx_get_indexed_regs(gpu, a8xx_state, &indexed_regs[i],
			0, &a8xx_state->indexed_regs[idx++]);
	}

	/* Get the contents of the CP_BV mempool */
	for (i = 0; i < mempool_count; i++)
		FOR_EACH_SLICE(slice, mempool_regs[i].slice_region, a6xx_gpu->slice_mask)
			a8xx_get_mempool(gpu, a8xx_state, &mempool_regs[i],
					slice,
					&a8xx_state->indexed_regs[idx++]);

	/* Reset aperture */
	a8xx_aperture_set(gpu, 0);
	return;
}

struct msm_gpu_state *a8xx_gpu_state_get(struct msm_gpu *gpu)
{
	struct a8xx_crashdumper _dumper = { 0 }, *dumper = NULL;
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct a8xx_gpu_state *a8xx_state = kzalloc(sizeof(*a8xx_state),
		GFP_KERNEL);
	bool stalled = !!(gpu_read(gpu, REG_A8XX_RBBM_MISC_STATUS) &
			A8XX_RBBM_MISC_STATUS_SMMU_STALLED_ON_FAULT);

	if (!a8xx_state)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&a8xx_state->objs);

	/* Get the generic state from the adreno core */
	adreno_gpu_state_get(gpu, &a8xx_state->base);

	a8xx_get_gmu_registers(gpu, a8xx_state);

	a8xx_state->gmu_log = a8xx_snapshot_gmu_bo(a8xx_state, &a6xx_gpu->gmu.log);
	a8xx_state->gmu_hfi = a8xx_snapshot_gmu_bo(a8xx_state, &a6xx_gpu->gmu.hfi);
	a8xx_state->gmu_debug = a8xx_snapshot_gmu_bo(a8xx_state, &a6xx_gpu->gmu.debug);

	a8xx_snapshot_gmu_hfi_history(gpu, a8xx_state);

	/* If GX isn't on the rest of the data isn't going to be accessible */
	if (!a6xx_gmu_gx_is_on(&a6xx_gpu->gmu))
		return &a8xx_state->base;

	/* Get the banks of indexed registers */
	a8xx_get_indexed_registers(gpu, a8xx_state);

	/*
	 * Try to initialize the crashdumper, if we are not dumping state
	 * with the SMMU stalled.  The crashdumper needs memory access to
	 * write out GPU state, so we need to skip this when the SMMU is
	 * stalled in response to an iova fault
	 */
	if (!stalled && !gpu->needs_hw_init &&
	    !a8xx_crashdumper_init(gpu, &_dumper)) {
		dumper = &_dumper;
	}

	a8xx_get_registers(gpu, a8xx_state, dumper);

	if (dumper) {
		a8xx_get_shaders(gpu, a8xx_state, dumper);
		/* TODO: Add clusters, mvc, debugbus... dumps */

		msm_gem_kernel_put(dumper->bo, gpu->vm);
	}

	a8xx_state->gpu_initialized = !gpu->needs_hw_init;

	return  &a8xx_state->base;
}

static void a8xx_gpu_state_destroy(struct kref *kref)
{
	struct a8xx_state_memobj *obj, *tmp;
	struct msm_gpu_state *state = container_of(kref,
			struct msm_gpu_state, ref);
	struct a8xx_gpu_state *a8xx_state = container_of(state,
			struct a8xx_gpu_state, base);

	if (a8xx_state->gmu_log)
		kvfree(a8xx_state->gmu_log->data);

	if (a8xx_state->gmu_hfi)
		kvfree(a8xx_state->gmu_hfi->data);

	if (a8xx_state->gmu_debug)
		kvfree(a8xx_state->gmu_debug->data);

	list_for_each_entry_safe(obj, tmp, &a8xx_state->objs, node) {
		list_del(&obj->node);
		kvfree(obj);
	}

	adreno_gpu_state_destroy(state);
	kfree(a8xx_state);
}

int a8xx_gpu_state_put(struct msm_gpu_state *state)
{
	if (IS_ERR_OR_NULL(state))
		return 1;

	return kref_put(&state->ref, a8xx_gpu_state_destroy);
}

static void a8xx_show_registers_indented(const u32 *registers,
		u32 *data,
		u32 slice_region,
		u32 slice,
		struct drm_printer *p, unsigned indent)
{
	int i, index = 0;

	if (slice_region) {
		for (i = 0; i < indent; i++)
			drm_printf(p, "  ");

		drm_printf(p, "- slice: 0x%06x\n", slice);
		indent++;
	}

	for (i = 0; registers[i] != UINT_MAX; i += 2) {
		u32 count = RANGE(registers, i);
		u32 offset = registers[i];
		int j;

		for (j = 0; j < count; index++, offset++, j++) {
			int k;

			if (data[index] == 0xdeafbead)
				continue;

			for (k = 0; k < indent; k++)
				drm_printf(p, "  ");
			drm_printf(p, "- { offset: 0x%06x, value: 0x%08x }\n",
				offset << 2, data[index]);
		}
	}
}

static void a8xx_show_registers(const u32 *registers,
		u32 *data,
		u32 slice_region,
		u32 slice,
		struct drm_printer *p)
{
	a8xx_show_registers_indented(registers, data, slice_region, slice, p, 1);
}

static void print_ascii85(struct drm_printer *p, size_t len, u32 *data)
{
	char out[ASCII85_BUFSZ];
	long i, l, datalen = 0;

	for (i = 0; i < len >> 2; i++) {
		if (data[i])
			datalen = (i + 1) << 2;
	}

	if (datalen == 0)
		return;

	drm_puts(p, "    data: !!ascii85 |\n");
	drm_puts(p, "      ");


	l = ascii85_encode_len(datalen);

	for (i = 0; i < l; i++)
		drm_puts(p, ascii85_encode(data[i], out));

	drm_puts(p, "\n");
}

static void print_name(struct drm_printer *p, const char *fmt, const char *name)
{
	drm_puts(p, fmt);
	drm_puts(p, name);
	drm_puts(p, "\n");
}

static void a8xx_show_shader(struct a8xx_gpu_state_obj *obj,
		struct drm_printer *p)
{
	const struct gen8_shader_block *block = obj->handle;
	int i, j;
	u32 *data = obj->data;

	if (!obj->handle)
		return;

	print_name(p, "  - type: ", a8xx_statetype_names[block->statetype]);
	print_name(p, "    - pipe: ", a8xx_pipe_names[block->pipeid]);
	drm_printf(p, "    - location: %d\n", block->location);

	for (i = 0; i < block->num_sps; i++) {
		drm_printf(p, "      - sp: %d\n", i);

		for (j = 0; j < block->num_ctx; j++) {
			drm_printf(p, "        - ctx: %d\n", j);
			drm_printf(p, "          size: %d\n", block->size);

			if (!obj->data)
				continue;

			print_ascii85(p, block->size << 2, data);

			data += block->size;
		}
	}
}

static void a8xx_show_indexed_regs(struct a8xx_gpu_state_obj *obj,
		struct drm_printer *p)
{
	const struct a8xx_indexed_registers *indexed = obj->handle;

	if (!indexed)
		return;

	print_name(p, "  - regs-name: ", indexed->name);
	drm_printf(p, "    dwords: %d\n", obj->count);
	drm_printf(p, "    pipe: %d\n", indexed->pipe_id);
	if (indexed->slice_region == SLICE)
		drm_printf(p, "    slice: %d\n", obj->slice);

	print_ascii85(p, obj->count << 2, obj->data);
}

void a8xx_show(struct msm_gpu *gpu, struct msm_gpu_state *state,
		struct drm_printer *p)
{
	struct a8xx_gpu_state *a8xx_state = container_of(state,
			struct a8xx_gpu_state, base);
	int i;

	if (IS_ERR_OR_NULL(state))
		return;

	drm_printf(p, "gpu-initialized: %d\n", a8xx_state->gpu_initialized);

	adreno_show(gpu, state, p);

	drm_puts(p, "gmu-log:\n");
	if (a8xx_state->gmu_log) {
		struct msm_gpu_state_bo *gmu_log = a8xx_state->gmu_log;

		drm_printf(p, "    iova: 0x%016llx\n", gmu_log->iova);
		drm_printf(p, "    size: %zu\n", gmu_log->size);
		adreno_show_object(p, &gmu_log->data, gmu_log->size,
				&gmu_log->encoded);
	}

	drm_puts(p, "gmu-hfi:\n");
	if (a8xx_state->gmu_hfi) {
		struct msm_gpu_state_bo *gmu_hfi = a8xx_state->gmu_hfi;
		unsigned i, j;

		drm_printf(p, "    iova: 0x%016llx\n", gmu_hfi->iova);
		drm_printf(p, "    size: %zu\n", gmu_hfi->size);
		for (i = 0; i < ARRAY_SIZE(a8xx_state->hfi_queue_history); i++) {
			drm_printf(p, "    queue-history[%u]:", i);
			for (j = 0; j < HFI_HISTORY_SZ; j++) {
				drm_printf(p, " %d", a8xx_state->hfi_queue_history[i][j]);
			}
			drm_printf(p, "\n");
		}
		adreno_show_object(p, &gmu_hfi->data, gmu_hfi->size,
				&gmu_hfi->encoded);
	}

	drm_puts(p, "gmu-debug:\n");
	if (a8xx_state->gmu_debug) {
		struct msm_gpu_state_bo *gmu_debug = a8xx_state->gmu_debug;

		drm_printf(p, "    iova: 0x%016llx\n", gmu_debug->iova);
		drm_printf(p, "    size: %zu\n", gmu_debug->size);
		adreno_show_object(p, &gmu_debug->data, gmu_debug->size,
				&gmu_debug->encoded);
	}

	drm_puts(p, "registers:\n");
	for (i = 0; i < a8xx_state->nr_registers; i++) {
		struct a8xx_gpu_state_obj *obj = &a8xx_state->registers[i];
		const struct gen8_reg_list *regs = obj->handle;

		if (!obj->handle)
			continue;

		a8xx_show_registers(regs->regs, obj->data, regs->slice_region, obj->slice, p);
	}

	drm_puts(p, "registers-gmu:\n");
	for (i = 0; i < a8xx_state->nr_gmu_registers; i++) {
		struct a8xx_gpu_state_obj *obj = &a8xx_state->gmu_registers[i];

		if (!obj->handle)
			continue;

		a8xx_show_registers(obj->handle, obj->data, UNSLICE, 0, p);
	}

	drm_puts(p, "indexed-registers:\n");
	for (i = 0; i < a8xx_state->nr_indexed_regs; i++)
		a8xx_show_indexed_regs(&a8xx_state->indexed_regs[i], p);

	drm_puts(p, "shader-blocks:\n");
	for (i = 0; i < a8xx_state->nr_shaders; i++)
		a8xx_show_shader(&a8xx_state->shaders[i], p);
}
