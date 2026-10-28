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
    size_t length;

    if (copy_config_value(g_libdb_config.pmem_dir,
                          sizeof(g_libdb_config.pmem_dir), value) != 0) {
        return -1;
    }
    length = strlen(g_libdb_config.pmem_dir);
    while (length > 1 && g_libdb_config.pmem_dir[length - 1] == '/') {
        g_libdb_config.pmem_dir[--length] = '\0';
    }
    return 0;
}

int libdb_config_set_warm_path(const char *value)
{
    return copy_config_value(g_libdb_config.warm_path,
                             sizeof(g_libdb_config.warm_path), value);
}

int libdb_config_set_precondition_path(const char *value)
{
    return copy_config_value(g_libdb_config.precondition_path,
                             sizeof(g_libdb_config.precondition_path), value);
}

int libdb_config_set_test_path(const char *value)
{
    return copy_config_value(g_libdb_config.test_path,
                             sizeof(g_libdb_config.test_path), value);
}

int libdb_config_init_from_env(void)
{
    const char *pci_addr = getenv("LIBDB_PCI_ADDR");
    const char *pmem_dir = getenv("LIBDB_PMEM_DIR");
    const char *warm_path = getenv("LIBDB_WARM_FILE");
    const char *test_path = getenv("LIBDB_TEST_FILE");

    memset(&g_libdb_config, 0, sizeof(g_libdb_config));
    if (libdb_config_set_pci_addr(pci_addr) != 0 ||
        libdb_config_set_pmem_dir(pmem_dir) != 0 ||
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
    struct stat status;

    if (g_libdb_config.pci_addr[0] == '\0') {
        snprintf(error, error_size, "missing --pci-addr (or LIBDB_PCI_ADDR)");
        return -1;
    }
    if (!validate_pci_addr(g_libdb_config.pci_addr)) {
        snprintf(error, error_size, "invalid PCI address '%s' (expected DDDD:BB:DD.F)",
                 g_libdb_config.pci_addr);
        return -1;
    }
    if (g_libdb_config.pmem_dir[0] == '\0') {
        snprintf(error, error_size, "missing --pmem-dir (or LIBDB_PMEM_DIR)");
        return -1;
    }
    if (stat(g_libdb_config.pmem_dir, &status) != 0) {
        snprintf(error, error_size, "PMEM directory '%s' is not an accessible directory: %s",
                 g_libdb_config.pmem_dir, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(status.st_mode)) {
        snprintf(error, error_size, "PMEM path '%s' is not a directory",
                 g_libdb_config.pmem_dir);
        return -1;
    }
    if (access(g_libdb_config.pmem_dir, W_OK | X_OK) != 0) {
        snprintf(error, error_size, "PMEM directory '%s' is not writable: %s",
                 g_libdb_config.pmem_dir, strerror(errno));
        return -1;
    }
    if (g_libdb_config.warm_path[0] == '\0' || access(g_libdb_config.warm_path, R_OK) != 0) {
        snprintf(error, error_size, "warm workload '%s' is not readable",
                 g_libdb_config.warm_path[0] ? g_libdb_config.warm_path : "(unset)");
        return -1;
    }
    if (g_libdb_config.precondition_path[0] != '\0' &&
        (stat(g_libdb_config.precondition_path, &status) != 0 ||
         !S_ISREG(status.st_mode) ||
         access(g_libdb_config.precondition_path, R_OK) != 0 ||
         strstr(g_libdb_config.precondition_path, "Production") != NULL ||
         strstr(g_libdb_config.test_path, "Production") != NULL)) {
        snprintf(error, error_size, "precondition requires readable YCSB A and YCSB test traces");
        return -1;
    }
    if (g_libdb_config.test_path[0] == '\0' || access(g_libdb_config.test_path, R_OK) != 0) {
        snprintf(error, error_size, "test workload '%s' is not readable",
                 g_libdb_config.test_path[0] ? g_libdb_config.test_path : "(unset)");
        return -1;
    }
    return 0;
}

void libdb_config_print(FILE *stream)
{
    fprintf(stream, "NVMe PCI address: %s\n", g_libdb_config.pci_addr);
    fprintf(stream, "PMEM directory: %s\n", g_libdb_config.pmem_dir);
    fprintf(stream, "Warm workload: %s\n", g_libdb_config.warm_path);
    if (g_libdb_config.precondition_path[0] != '\0')
        fprintf(stream, "Precondition workload: %s\n", g_libdb_config.precondition_path);
    fprintf(stream, "Test workload: %s\n", g_libdb_config.test_path);
}

char *libdb_config_make_pmem_path(const char *stem, unsigned int file_seq)
{
    char *path;
    int written;
    const char *separator;

    if (stem == NULL || stem[0] == '\0' || g_libdb_config.pmem_dir[0] == '\0') {
        return NULL;
    }
    separator = strcmp(g_libdb_config.pmem_dir, "/") == 0 ? "" : "/";
    path = malloc(LIBDB_PATH_CAPACITY);
    if (path == NULL) {
        return NULL;
    }
    written = snprintf(path, LIBDB_PATH_CAPACITY, "%s%s%s%u",
                       g_libdb_config.pmem_dir, separator, stem, file_seq);
    if (written < 0 || written >= LIBDB_PATH_CAPACITY) {
        free(path);
        return NULL;
    }
    return path;
}
