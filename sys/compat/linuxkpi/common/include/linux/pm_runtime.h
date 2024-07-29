/* Public domain. */

#ifndef _LINUXKPI_LINUX_PM_RUNTIME_H_
#define _LINUXKPI_LINUX_PM_RUNTIME_H_

#include <linux/device.h>
#include <linux/pm.h>

#define pm_runtime_mark_last_busy(x) (void)(x)
#define pm_runtime_use_autosuspend(x) (void)(x)
#define pm_runtime_dont_use_autosuspend(x) (void)(x)
#define pm_runtime_put_autosuspend(x) (void)(x)
#define pm_runtime_set_autosuspend_delay(x, y) (void)(x); (void)(y)
#define pm_runtime_set_active(x) (void)(x)
#define pm_runtime_allow(x) (void)(x)
#define pm_runtime_put_noidle(x) (void)(x)
#define pm_runtime_forbid(x) (void)(x)
#define pm_runtime_get_noresume(x) (void)(x)
#define pm_runtime_enable(x) (void)(x)
#define pm_runtime_disable(x) (void)(x)
#define pm_runtime_autosuspend(x) (void)(x)
#define pm_runtime_resume(x) (void)(x)

static inline int
pm_runtime_get_sync(struct device *dev)
{

	return (0);
}

static inline int
pm_runtime_get_if_in_use(struct device *dev)
{

	return (1);
}

static inline int
pm_runtime_get_if_active(struct device *dev, bool x)
{

	return (1);
}

static inline int
pm_runtime_suspended(struct device *dev)
{

	return (0);
}

static inline int
pm_runtime_resume_and_get(struct device *dev)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline int
devm_pm_runtime_enable(struct device *dev)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline int
pm_runtime_put(struct device *dev)
{

	pr_debug("%s: TODO\n", __func__);
	return (0);
}

static inline void
pm_runtime_put_sync(struct device *dev)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline void
pm_runtime_put_sync_suspend(struct device *dev)
{

	pr_debug("%s: TODO\n", __func__);
}

static inline bool
pm_runtime_status_suspended(struct device *dev)
{

	pr_debug("%s: TODO\n", __func__);
	return (false);
}

#endif	/* _LINUXKPI_LINUX_PM_RUNTIME_H_ */
