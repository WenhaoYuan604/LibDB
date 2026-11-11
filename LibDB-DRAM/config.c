#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct libdb_runtime_config g_libdb_config;

static int copy_config_value(char *destination, size_t capacity, const char *value)
{
    int written;

    if (value == NULL || value[0] == '\0') {
        destination[0] = '\0';
        return 0;
    }
    written = snprintf(destination, capacity, "%s", value);
    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

int libdb_config_set_pci_addr(const char *value)
{
    return copy_config_value(g_libdb_config.pci_addr,
                             sizeof(g_libdb_config.pci_addr), value);
}

int libdb_config_set_pmem_dir(const char *value)
{
    (void)value;
    fprintf(stderr, "warning: --pmem-dir is ignored; indexes and write buffers use DRAM\n");
    return 0;
}

int libdb_config_set_warm_path(const char *value)
{
    return copy_config_value(g_libdb_config.warm_path,
                             sizeof(g_libdb_config.warm_path), value);
}

int libdb_config_set_test_path(const char *value)
{
    return copy_config_value(g_libdb_config.test_path,
                             sizeof(g_libdb_config.test_path), value);
}

int libdb_config_init_from_env(void)
{
    const char *pci_addr = getenv("LIBDB_PCI_ADDR");
    if(getenv("LIBDB_PMEM_DIR"))
        fprintf(stderr, "warning: LIBDB_PMEM_DIR is ignored; no PMEM path is accessed\n");
    const char *warm_path = getenv("LIBDB_WARM_FILE");
    const char *test_path = getenv("LIBDB_TEST_FILE");

    memset(&g_libdb_config, 0, sizeof(g_libdb_config));
    if (libdb_config_set_pci_addr(pci_addr) != 0 ||
        libdb_config_set_warm_path(warm_path) != 0 ||
        libdb_config_set_test_path(test_path) != 0) {
        return -1;
    }
    return 0;
}

static int validate_pci_addr(const char *value)
{
    unsigned int domain, bus, device, function;
    char trailing;

    if (sscanf(value, "%4x:%2x:%2x.%1x%c",
               &domain, &bus, &device, &function, &trailing) != 4) {
        return 0;
    }
    return domain <= 0xffff && bus <= 0xff && device <= 0x1f && function <= 7;
}

int libdb_config_validate(char *error, size_t error_size)
{
    return libdb_config_validate_inputs(error, error_size, 1);
}

int libdb_config_validate_inputs(char *error, size_t error_size, int require_test)
{

    if (g_libdb_config.pci_addr[0] == '\0') {
        snprintf(error, error_size, "missing --pci-addr (or LIBDB_PCI_ADDR)");
        return -1;
    }
    if (!validate_pci_addr(g_libdb_config.pci_addr)) {
        snprintf(error, error_size, "invalid PCI address '%s' (expected DDDD:BB:DD.F)",
                 g_libdb_config.pci_addr);
        return -1;
    }
    if (g_libdb_config.warm_path[0] == '\0' || access(g_libdb_config.warm_path, R_OK) != 0) {
        snprintf(error, error_size, "warm workload '%s' is not readable",
                 g_libdb_config.warm_path[0] ? g_libdb_config.warm_path : "(unset)");
        return -1;
    }
    if (require_test && (g_libdb_config.test_path[0] == '\0' ||
                         access(g_libdb_config.test_path, R_OK) != 0)) {
        snprintf(error, error_size, "test workload '%s' is not readable",
                 g_libdb_config.test_path[0] ? g_libdb_config.test_path : "(unset)");
        return -1;
    }
    return 0;
}
