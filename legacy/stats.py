import numpy as np
from scipy import stats

def bootstrap_ci(values, n_boot=2000, alpha=0.05):
    """Compute 95% bootstrap confidence interval."""
    arr = np.array(values)
    boot_means = np.array([np.mean(arr[np.random.randint(0, len(arr), len(arr))]) for _ in range(n_boot)])
    lo = np.percentile(boot_means, 100 * alpha / 2)
    hi = np.percentile(boot_means, 100 * (1 - alpha / 2))
    return np.mean(arr), lo, hi

def welch_t_test(group1, group2):
    """Perform Welch's t-test for unequal variances."""
    t_stat, p_val = stats.ttest_ind(group1, group2, equal_var=False)
    return t_stat, p_val

def cohen_d(group1, group2):
    """Calculate Cohen's d effect size."""
    n1, n2 = len(group1), len(group2)
    var1, var2 = np.var(group1, ddof=1), np.var(group2, ddof=1)
    pooled_std = np.sqrt(((n1 - 1) * var1 + (n2 - 1) * var2) / (n1 + n2 - 2))
    return (np.mean(group1) - np.mean(group2)) / pooled_std
