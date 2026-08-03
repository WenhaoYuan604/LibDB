#ifndef PMDK_H_
#define PMDK_H_

#include<string.h>
#include <libpmemobj.h>
#include <libpmem.h>
#include <libpmemobj/pool_base.h>
#include <stdio.h>

int pmemoid_is_null(PMEMoid id);

int pmemoid_set_null(PMEMoid * id);
#endif