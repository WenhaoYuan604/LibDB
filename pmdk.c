#include "./pmdk.h"

int pmemoid_is_null(PMEMoid id){
    if(id.off == 0 ){
        return 1;
    }
    return 0;
}

int pmemoid_set_null(PMEMoid * id){
    id->off = 0;
    // id->pool_uuid_lo = 0;
    return 1;
}
