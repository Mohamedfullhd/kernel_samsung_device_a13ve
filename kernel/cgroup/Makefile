# SPDX-License-Identifier: GPL-2.0
obj-y := cgroup.o rstat.o namespace.o cgroup-v1.o freezer.o

ifdef CONFIG_INLINE_OPTIMIZATION
ccflags-y += -mllvm -inline-threshold=15000
ccflags-y += -mllvm -inlinehint-threshold=10000
endif

obj-$(CONFIG_CGROUP_FREEZER) += legacy_freezer.o
obj-$(CONFIG_CGROUP_PIDS) += pids.o
obj-$(CONFIG_CGROUP_RDMA) += rdma.o
obj-$(CONFIG_CPUSETS) += cpuset.o
obj-$(CONFIG_CGROUP_DEBUG) += debug.o
