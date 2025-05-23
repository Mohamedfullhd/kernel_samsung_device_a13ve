// SPDX-License-Identifier: GPL-2.0
/*
 *
 * (C) COPYRIGHT 2019-2020 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 */

/*
 * Base kernel context APIs
 */

#include <mali_kbase.h>
#include <gpu/mali_kbase_gpu_regmap.h>
#include <mali_kbase_mem_linux.h>
#include <mali_kbase_dma_fence.h>
#include <mali_kbase_ctx_sched.h>
#include <mali_kbase_mem_pool_group.h>
#include <tl/mali_kbase_timeline.h>
#include <mmu/mali_kbase_mmu.h>
#include <context/mali_kbase_context_internal.h>

/**
 * find_process_node - Used to traverse the process rb_tree to find if
 *                     process exists already in process rb_tree.
 *
 * @node: Pointer to root node to start search.
 * @tgid: Thread group PID to search for.
 *
 * Return: Pointer to kbase_process if exists otherwise NULL.
 */
static struct kbase_process *find_process_node(struct rb_node *node, pid_t tgid)
{
	struct kbase_process *kprcs = NULL;

	/* Check if the kctx creation request is from a existing process.*/
	while (node) {
		struct kbase_process *prcs_node =
			rb_entry(node, struct kbase_process, kprcs_node);
		if (prcs_node->tgid == tgid) {
			kprcs = prcs_node;
			break;
		}

		if (tgid < prcs_node->tgid)
			node = node->rb_left;
		else
			node = node->rb_right;
	}

	return kprcs;
}

/**
 * kbase_insert_kctx_to_process - Initialise kbase process context.
 *
 * @kctx: Pointer to kbase context.
 *
 * Here we initialise per process rb_tree managed by kbase_device.
 * We maintain a rb_tree of each unique process that gets created.
 * and Each process maintains a list of kbase context.
 * This setup is currently used by kernel trace functionality
 * to trace and visualise gpu memory consumption.
 *
 * Return: 0 on success and error number on failure.
 */
static int kbase_insert_kctx_to_process(struct kbase_context *kctx)
{
	struct rb_root *const prcs_root = &kctx->kbdev->process_root;
	const pid_t tgid = kctx->tgid;
	struct kbase_process *kprcs = NULL;

	lockdep_assert_held(&kctx->kbdev->kctx_list_lock);

	kprcs = find_process_node(prcs_root->rb_node, tgid);

	/* if the kctx is from new process then create a new kbase_process
	 * and add it to the &kbase_device->rb_tree
	 */
	if (!kprcs) {
		struct rb_node **new = &prcs_root->rb_node, *parent = NULL;

		kprcs = kzalloc(sizeof(*kprcs), GFP_KERNEL);
		if (kprcs == NULL)
			return -ENOMEM;
		kprcs->tgid = tgid;
		INIT_LIST_HEAD(&kprcs->kctx_list);
		kprcs->dma_buf_root = RB_ROOT;
		kprcs->total_gpu_pages = 0;
		kprcs->dma_buf_pages = 0;

		while (*new) {
			struct kbase_process *prcs_node;

			parent = *new;
			prcs_node = rb_entry(parent, struct kbase_process,
					     kprcs_node);
			if (tgid < prcs_node->tgid)
				new = &(*new)->rb_left;
			else
				new = &(*new)->rb_right;
		}
		rb_link_node(&kprcs->kprcs_node, parent, new);
		rb_insert_color(&kprcs->kprcs_node, prcs_root);
	}

	kctx->kprcs = kprcs;
	list_add(&kctx->kprcs_link, &kprcs->kctx_list);

	return 0;
}

int kbase_context_common_init(struct kbase_context *kctx)
{
	const unsigned long cookies_mask = KBASE_COOKIE_MASK;
	int err = 0;

	/* creating a context is considered a disjoint event */
	kbase_disjoint_event(kctx->kbdev);

	kctx->as_nr = KBASEP_AS_NR_INVALID;

	atomic_set(&kctx->refcount, 0);

	spin_lock_init(&kctx->mm_update_lock);
	kctx->process_mm = NULL;
	atomic_set(&kctx->nonmapped_pages, 0);
	atomic_set(&kctx->permanent_mapped_pages, 0);
	kctx->tgid = current->tgid;
	kctx->pid = current->pid;

	atomic_set(&kctx->used_pages, 0);

	mutex_init(&kctx->reg_lock);

	spin_lock_init(&kctx->mem_partials_lock);
	INIT_LIST_HEAD(&kctx->mem_partials);

	spin_lock_init(&kctx->waiting_soft_jobs_lock);
	INIT_LIST_HEAD(&kctx->waiting_soft_jobs);

	init_waitqueue_head(&kctx->event_queue);
	atomic_set(&kctx->event_count, 0);
	atomic_set(&kctx->event_closed, false);

	bitmap_copy(kctx->cookies, &cookies_mask, BITS_PER_LONG);

#ifdef CONFIG_GPU_TRACEPOINTS
	atomic_set(&kctx->jctx.work_id, 0);
#endif

	kctx->id = atomic_add_return(1, &(kctx->kbdev->ctx_num)) - 1;

	mutex_init(&kctx->legacy_hwcnt_lock);

	mutex_lock(&kctx->kbdev->kctx_list_lock);

	err = kbase_insert_kctx_to_process(kctx);
	if (err)
		dev_err(kctx->kbdev->dev,
		"(err:%d) failed to insert kctx to kbase_process\n", err);

	mutex_unlock(&kctx->kbdev->kctx_list_lock);

	return err;
}

int kbase_context_add_to_dev_list(struct kbase_context *kctx)
{
	if (WARN_ON(!kctx))
		return -EINVAL;

	if (WARN_ON(!kctx->kbdev))
		return -EINVAL;

	mutex_lock(&kctx->kbdev->kctx_list_lock);
	list_add(&kctx->kctx_list_link, &kctx->kbdev->kctx_list);
	mutex_unlock(&kctx->kbdev->kctx_list_lock);

	kbase_timeline_post_kbase_context_create(kctx);

	return 0;
}

void kbase_context_remove_from_dev_list(struct kbase_context *kctx)
{
	if (WARN_ON(!kctx))
		return;

	if (WARN_ON(!kctx->kbdev))
		return;

	kbase_timeline_pre_kbase_context_destroy(kctx);

	mutex_lock(&kctx->kbdev->kctx_list_lock);
	list_del_init(&kctx->kctx_list_link);
	mutex_unlock(&kctx->kbdev->kctx_list_lock);
}

/**
 * kbase_remove_kctx_from_process - remove a terminating context from
 *                                    the process list.
 *
 * @kctx: Pointer to kbase context.
 *
 * Remove the tracking of context from the list of contexts maintained under
 * kbase process and if the list if empty then there no outstanding contexts
 * we can remove the process node as well.
 */

static void kbase_remove_kctx_from_process(struct kbase_context *kctx)
{
	struct kbase_process *kprcs = kctx->kprcs;

	lockdep_assert_held(&kctx->kbdev->kctx_list_lock);
	list_del(&kctx->kprcs_link);

	/* if there are no outstanding contexts in current process node,
	 * we can remove it from the process rb_tree.
	 */
	if (list_empty(&kprcs->kctx_list)) {
		rb_erase(&kprcs->kprcs_node, &kctx->kbdev->process_root);
		/* Add checks, so that the terminating process Should not
		 * hold any gpu_memory.
		 */
		WARN_ON(kprcs->total_gpu_pages);
		WARN_ON(!RB_EMPTY_ROOT(&kprcs->dma_buf_root));
		kfree(kprcs);
	}
}

void kbase_context_common_term(struct kbase_context *kctx)
{
	unsigned long flags;
	int pages;

	mutex_lock(&kctx->kbdev->mmu_hw_mutex);
	spin_lock_irqsave(&kctx->kbdev->hwaccess_lock, flags);
	kbase_ctx_sched_remove_ctx(kctx);
	spin_unlock_irqrestore(&kctx->kbdev->hwaccess_lock, flags);
	mutex_unlock(&kctx->kbdev->mmu_hw_mutex);

	pages = atomic_read(&kctx->used_pages);
	if (pages != 0)
		dev_warn(kctx->kbdev->dev,
			"%s: %d pages in use!\n", __func__, pages);

	WARN_ON(atomic_read(&kctx->nonmapped_pages) != 0);

	mutex_lock(&kctx->kbdev->kctx_list_lock);
	kbase_remove_kctx_from_process(kctx);
	mutex_unlock(&kctx->kbdev->kctx_list_lock);

	KBASE_KTRACE_ADD(kctx->kbdev, CORE_CTX_DESTROY, kctx, 0u);
}

int kbase_context_mem_pool_group_init(struct kbase_context *kctx)
{
	return kbase_mem_pool_group_init(&kctx->mem_pools,
		kctx->kbdev,
		&kctx->kbdev->mem_pool_defaults,
		&kctx->kbdev->mem_pools);
}

void kbase_context_mem_pool_group_term(struct kbase_context *kctx)
{
	kbase_mem_pool_group_term(&kctx->mem_pools);
}

int kbase_context_mmu_init(struct kbase_context *kctx)
{
	return kbase_mmu_init(kctx->kbdev,
		&kctx->mmu, kctx,
		base_context_mmu_group_id_get(kctx->create_flags));
}

void kbase_context_mmu_term(struct kbase_context *kctx)
{
	kbase_mmu_term(kctx->kbdev, &kctx->mmu);
}

int kbase_context_mem_alloc_page(struct kbase_context *kctx)
{
	struct page *p;

	p = kbase_mem_alloc_page(&kctx->mem_pools.small[KBASE_MEM_GROUP_SINK]);
	if (!p)
		return -ENOMEM;

	kctx->aliasing_sink_page = as_tagged(page_to_phys(p));

	return 0;
}

void kbase_context_mem_pool_free(struct kbase_context *kctx)
{
	/* drop the aliasing sink page now that it can't be mapped anymore */
	kbase_mem_pool_free(
		&kctx->mem_pools.small[KBASE_MEM_GROUP_SINK],
		as_page(kctx->aliasing_sink_page),
		false);
}

void kbase_context_sticky_resource_term(struct kbase_context *kctx)
{
	unsigned long pending_regions_to_clean;

	kbase_gpu_vm_lock(kctx);
	kbase_sticky_resource_term(kctx);

	/* free pending region setups */
	pending_regions_to_clean = KBASE_COOKIE_MASK;
	bitmap_andnot(&pending_regions_to_clean, &pending_regions_to_clean,
		      kctx->cookies, BITS_PER_LONG);
	while (pending_regions_to_clean) {
		unsigned int cookie = find_first_bit(&pending_regions_to_clean,
				BITS_PER_LONG);

		if (!WARN_ON(!kctx->pending_regions[cookie])) {
			dev_dbg(kctx->kbdev->dev, "Freeing pending unmapped region\n");
			kbase_mem_phy_alloc_put(
				kctx->pending_regions[cookie]->cpu_alloc);
			kbase_mem_phy_alloc_put(
				kctx->pending_regions[cookie]->gpu_alloc);
			kfree(kctx->pending_regions[cookie]);

			kctx->pending_regions[cookie] = NULL;
		}

		bitmap_clear(&pending_regions_to_clean, cookie, 1);
	}
	kbase_gpu_vm_unlock(kctx);
}
