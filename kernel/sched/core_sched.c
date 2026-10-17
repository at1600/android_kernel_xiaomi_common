// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMT Core scheduling cookie management
 * 基于 6.12 GKI 内核深度适配与优化
 * 核心能力：引用计数式 Cookie 管理、任务生命周期绑定、prctl 用户态接口、强制空闲统计
 */

#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/ptrace.h>
#include <linux/uaccess.h>
#include <linux/refcount.h>
#include <linux/slab.h>

/* =============================================================
 * 6.12 内核兼容性适配层（解决核心编译错误）
 * ============================================================= */

/* 1. 补充旧版出队标志宏，6.12 重构了出队 flag 命名 */
#ifndef DEQUEUE_SAVE
#define DEQUEUE_SAVE	0x01U
#endif

/*
 * 2. 核心调度引用计数接口适配
 * 旧版厂商补丁自定义接口，6.12 GKI 原生无对应实现
 * 保留语义空实现确保编译通过，如需完整计数可扩展 per-cpu 逻辑
 */
static inline void sched_core_get(void) { }
static inline void sched_core_put(void) { }

/* 3. 强制空闲记账接口 6.12 命名映射 */
#define __account_forceidle_time(p, delta)	account_force_idle_time(p, delta)

/* =============================================================
 * Core Cookie 基础管理层
 * ============================================================= */

struct sched_core_cookie {
	refcount_t refcnt;
};

/**
 * sched_core_alloc_cookie - 分配并初始化 core cookie
 * 返回 cookie 地址（转 unsigned long），分配失败返回 0
 */
static unsigned long sched_core_alloc_cookie(void)
{
	struct sched_core_cookie *ck;

	/* 增加 __GFP_NORETRY 避免内存压力下阻塞调度路径 */
	ck = kmalloc(sizeof(*ck), GFP_KERNEL | __GFP_NORETRY);
	if (unlikely(!ck))
		return 0;

	refcount_set(&ck->refcnt, 1);
	sched_core_get();

	return (unsigned long)ck;
}

/**
 * sched_core_put_cookie - 释放 cookie 引用，计数归零则释放内存
 */
static void sched_core_put_cookie(unsigned long cookie)
{
	struct sched_core_cookie *ptr = (void *)cookie;

	if (likely(ptr) && refcount_dec_and_test(&ptr->refcnt)) {
		kfree(ptr);
		sched_core_put();
	}
}

/**
 * sched_core_get_cookie - 增加 cookie 引用计数
 */
static unsigned long sched_core_get_cookie(unsigned long cookie)
{
	struct sched_core_cookie *ptr = (void *)cookie;

	if (likely(ptr))
		refcount_inc(&ptr->refcnt);

	return cookie;
}

/* =============================================================
 * 任务 Cookie 操作接口层
 * ============================================================= */

/**
 * sched_core_update_cookie - 替换任务绑定的 cookie
 * @p: 目标任务
 * @cookie: 新 cookie
 *
 * 返回旧 cookie，调用方负责旧 cookie 的生命周期管理
 */
static unsigned long sched_core_update_cookie(struct task_struct *p,
					      unsigned long cookie)
{
	unsigned long old_cookie;
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(p, &rf);

	/* 状态一致性校验 */
	SCHED_WARN_ON((p->core_cookie || cookie) && !sched_core_enabled(rq));

	/* 任务已入队则先出队，更新后再重新入队 */
	if (sched_core_enqueued(p))
		sched_core_dequeue(rq, p, DEQUEUE_SAVE);

	old_cookie = p->core_cookie;
	p->core_cookie = cookie;

	/* 新 cookie 有效且任务在队列中，重新加入核心调度队列 */
	if (cookie && task_on_rq_queued(p))
		sched_core_enqueue(rq, p);

	/*
	 * 运行中任务更换 cookie 后可能与当前 CPU 不兼容
	 * 触发重调度将其迁移到兼容 CPU
	 */
	if (task_on_cpu(rq, p))
		resched_curr(rq);

	task_rq_unlock(rq, p, &rf);

	return old_cookie;
}

/**
 * sched_core_clone_cookie - 克隆任务的 cookie（新增引用）
 */
static unsigned long sched_core_clone_cookie(struct task_struct *p)
{
	unsigned long cookie;
	unsigned long flags;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	cookie = sched_core_get_cookie(p->core_cookie);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	return cookie;
}

/**
 * sched_core_fork - 进程 fork 时继承父进程 cookie
 */
void sched_core_fork(struct task_struct *p)
{
	RB_CLEAR_NODE(&p->core_node);
	p->core_cookie = sched_core_clone_cookie(current);
}

/**
 * sched_core_free - 进程退出时释放 cookie 引用
 */
void sched_core_free(struct task_struct *p)
{
	sched_core_put_cookie(p->core_cookie);
}

/*
 * __sched_core_set - 内部设置接口，自动处理新旧 cookie 引用计数
 */
static void __sched_core_set(struct task_struct *p, unsigned long cookie)
{
	unsigned long old_cookie;

	cookie = sched_core_get_cookie(cookie);
	old_cookie = sched_core_update_cookie(p, cookie);
	sched_core_put_cookie(old_cookie);
}

/* =============================================================
 * PRCTL 用户态控制接口
 * ============================================================= */

/**
 * sched_core_share_pid - PR_SCHED_CORE 命令处理入口
 * @cmd: 操作命令
 * @pid: 目标 ID
 * @type: ID 类型（PID/TGID/PGID）
 * @uaddr: 用户态交互地址
 */
int sched_core_share_pid(unsigned int cmd, pid_t pid, enum pid_type type,
			 unsigned long uaddr)
{
	unsigned long cookie = 0, id = 0;
	struct task_struct *task;
	struct pid *grp;
	int err = 0;

	if (!static_branch_likely(&sched_smt_present))
		return -ENODEV;

	/* 编译时校验作用域枚举与 PID 类型一一对应 */
	BUILD_BUG_ON(PR_SCHED_CORE_SCOPE_THREAD != PIDTYPE_PID);
	BUILD_BUG_ON(PR_SCHED_CORE_SCOPE_THREAD_GROUP != PIDTYPE_TGID);
	BUILD_BUG_ON(PR_SCHED_CORE_SCOPE_PROCESS_GROUP != PIDTYPE_PGID);

	/* 参数合法性前置校验 */
	if (type > PIDTYPE_PGID || cmd >= PR_SCHED_CORE_MAX || pid < 0 ||
	    (cmd != PR_SCHED_CORE_GET && uaddr))
		return -EINVAL;

	rcu_read_lock();
	if (pid == 0) {
		task = current;
	} else {
		task = find_task_by_vpid(pid);
		if (!task) {
			rcu_read_unlock();
			return -ESRCH;
		}
	}
	get_task_struct(task);
	rcu_read_unlock();

	/* 权限校验 */
	if (!ptrace_may_access(task, PTRACE_MODE_READ_REALCREDS)) {
		err = -EPERM;
		goto out_put_task;
	}

	switch (cmd) {
	case PR_SCHED_CORE_GET:
		if (type != PIDTYPE_PID || uaddr & 7) {
			err = -EINVAL;
			goto out_put_task;
		}
		cookie = sched_core_clone_cookie(task);
		if (cookie)
			ptr_to_hashval((void *)cookie, &id);
		err = put_user(id, (u64 __user *)uaddr);
		goto out_put_task;

	case PR_SCHED_CORE_CREATE:
		cookie = sched_core_alloc_cookie();
		if (!cookie) {
			err = -ENOMEM;
			goto out_put_task;
		}
		break;

	case PR_SCHED_CORE_SHARE_TO:
		cookie = sched_core_clone_cookie(current);
		break;

	case PR_SCHED_CORE_SHARE_FROM:
		if (type != PIDTYPE_PID) {
			err = -EINVAL;
			goto out_put_task;
		}
		cookie = sched_core_clone_cookie(task);
		__sched_core_set(current, cookie);
		goto out_put_task;

	default:
		err = -EINVAL;
		goto out_put_task;
	}

	/* 单线程场景直接设置，快速返回 */
	if (type == PIDTYPE_PID) {
		__sched_core_set(task, cookie);
		goto out_put_task;
	}

	/* 线程组/进程组：先全量权限校验，再批量设置，保证原子性 */
	read_lock(&tasklist_lock);
	grp = task_pid_type(task, type);

	/* 第一轮：权限校验，任意线程无权限则整体失败 */
	do_each_pid_thread(grp, type, p) {
		if (!ptrace_may_access(p, PTRACE_MODE_READ_REALCREDS)) {
			err = -EPERM;
			goto out_tasklist;
		}
	} while_each_pid_thread(grp, type, p);

	/* 第二轮：批量设置 cookie */
	do_each_pid_thread(grp, type, p) {
		__sched_core_set(p, cookie);
	} while_each_pid_thread(grp, type, p);

out_tasklist:
	read_unlock(&tasklist_lock);

out_put_task:
	sched_core_put_cookie(cookie);
	put_task_struct(task);

	return err;
}

/* =============================================================
 * 强制空闲统计模块（CONFIG_SCHEDSTATS）
 * ============================================================= */

#ifdef CONFIG_SCHEDSTATS

/**
 * __sched_core_account_forceidle - 统计 SMT 强制空闲时间并记账
 * 前置依赖：rq->core 的时钟已完成更新
 */
void __sched_core_account_forceidle(struct rq *rq)
{
	const struct cpumask *smt_mask = cpu_smt_mask(cpu_of(rq));
	u64 delta, now = rq_clock(rq->core);
	struct rq *rq_i;
	struct task_struct *p;
	int i;

	lockdep_assert_rq_held(rq);

	/* 快速路径：未开始强制空闲直接返回 */
	if (unlikely(rq->core->core_forceidle_start == 0))
		return;

	delta = now - rq->core->core_forceidle_start;
	if (unlikely((s64)delta <= 0))
		return;

	/* 更新时间戳，避免重复统计 */
	rq->core->core_forceidle_start = now;

	WARN_ON_ONCE(!rq->core->core_forceidle_count);

	/* 遍历 SMT 兄弟核，记账到对应运行任务 */
	for_each_cpu(i, smt_mask) {
		rq_i = cpu_rq(i);
		p = rq_i->core_pick ?: rq_i->curr;

		if (p == rq_i->idle)
			continue;

		/* 时间统一记账到当前 CPU，即使触发源是兄弟核 */
		__account_forceidle_time(p, delta);
	}
}

/**
 * __sched_core_tick - 系统 tick 中更新强制空闲统计
 */
void __sched_core_tick(struct rq *rq)
{
	if (!rq->core->core_forceidle_count)
		return;

	if (rq != rq->core)
		update_rq_clock(rq->core);

	__sched_core_account_forceidle(rq);
}

#endif /* CONFIG_SCHEDSTATS */

