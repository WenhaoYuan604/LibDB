#include "slot_index.h"
#include "index.h"
#include "merge_flush.h"

void slot_allocator_initialize(struct slot_allocator *allocator) {
    atomic_init(&allocator->live_slots, 0);
}
size_t slot_allocator_live_slots(const struct slot_allocator *allocator) {
    return atomic_load_explicit(&allocator->live_slots, memory_order_relaxed);
}
struct index_slot *slot_index_allocate(struct slot_allocator *allocator) {
    if(!allocator) return NULL;
    size_t live = slot_allocator_live_slots(allocator);
    if(live == SIZE_MAX) return NULL;
    struct index_slot *slot = calloc(1, sizeof(*slot));
    if(slot) atomic_store_explicit(&allocator->live_slots, live + 1, memory_order_relaxed);
    return slot;
}
void slot_index_free(struct slot_allocator *allocator, struct index_slot *slot) {
    if(!slot) return;
    size_t live = slot_allocator_live_slots(allocator);
    if(!live) { fprintf(stderr, "fatal: DRAM slot accounting underflow\n"); abort(); }
    free(slot);
    atomic_store_explicit(&allocator->live_slots, live - 1, memory_order_relaxed);
}
int slot_index_apply_updates(const struct slot_update *updates, uint32_t count) {
    if((!updates && count) || count > 2U * SUBSPACE_MAX_SLOT) return -1;
    for(uint32_t i = 0; i < count; i++) {
        if(!updates[i].slot) return -1;
        for(uint32_t j = 0; j < i; j++) if(updates[i].slot == updates[j].slot) return -1;
    }
    for(uint32_t i = 0; i < count; i++) *updates[i].slot = updates[i].image;
    return 0;
}




int slot_index_insert_after_flush(struct subspace_info *group, key_type *keys,
        struct address_type *addresses, int count, struct slot_allocator *allocator) {
    enum { CAPACITY = SUBSPACE_MAX_SLOT * SLOT_MAX_ELEMENT };
    if(!group || !allocator || count < 0 || count > CAPACITY ||
       group->slot_num > SUBSPACE_MAX_SLOT || (count && (!keys || !addresses))) return -1;
    if(!count) return 0;
    for(int i = 0; i < count; i++) {
        if((i && keys[i] <= keys[i - 1]) || !addresses[i].size ||
           addresses[i].block >= (uint32_t)g_block_num || addresses[i].off < 0 ||
           addresses[i].size > BLOCK_SIZE || addresses[i].off + addresses[i].size > BLOCK_SIZE)
            return -1;
    }

    struct index_slot initial = {.boundary = group->boundary};
    bool initialize = group->slot_num == 0;
    if(initialize) {
        struct index_slot *slot = slot_index_allocate(allocator);
        if(!slot) return -1;
        group->slot[0] = slot;
        group->slot_key_num[0] = 1;
        group->slot_num = 1;
        group->min_key[0] = keys[0];
        group->key_num = 1;
        initial.entries[0].key = keys[0];
    }

    struct index_slot *slots[SUBSPACE_MAX_SLOT];
    uint8_t counts[SUBSPACE_MAX_SLOT];
    key_type minima[SUBSPACE_MAX_SLOT];
    uint32_t first = 0;
    while(first + 1 < group->slot_num && keys[0] >= group->min_key[first + 1]) first++;
    memcpy(slots, group->slot, first * sizeof(*slots));
    memcpy(counts, group->slot_key_num, first * sizeof(*counts));
    memcpy(minima, group->min_key, first * sizeof(*minima));
    uint32_t output = first;
    int input = 0;
    for(uint32_t s = first; s < group->slot_num; s++) {
        int end = input;
        while(end < count && (s + 1 == group->slot_num || keys[end] < group->min_key[s + 1])) end++;
        if(input == end) {
            if(output == SUBSPACE_MAX_SLOT) return -1;
            slots[output] = group->slot[s];
            counts[output] = group->slot_key_num[s];
            minima[output++] = group->min_key[s];
            continue;
        }


        struct slot_element merged[CAPACITY + SLOT_MAX_ELEMENT];
        unsigned n = 0, j = 0, previous = group->slot_key_num[s];
        if(!previous || previous > SLOT_MAX_ELEMENT || !group->slot[s]) return -1;
        const struct slot_element *entries = initialize ? initial.entries : group->slot[s]->entries;
        while(j < previous || input < end) {
            if(input == end || (j < previous && entries[j].key < keys[input])) {
                merged[n++] = entries[j++];
            } else {
                if(j < previous && entries[j].key == keys[input]) {
                    if(!initialize) {
                        group->redundant_size += entries[j].addr.size;
                        block_ref_remove(entries[j].addr.block);
                    }
                    j++;
                } else group->key_num++;
                block_ref_add(addresses[input].block);
                merged[n++] = (struct slot_element){keys[input], addresses[input]};
                input++;
            }
        }
        unsigned parts = (n + SLOT_MAX_ELEMENT - 1) / SLOT_MAX_ELEMENT;
        unsigned offset = 0;
        for(unsigned part = 0; part < parts; part++) {
            if(output == SUBSPACE_MAX_SLOT) return -1;
            unsigned length = (n - offset) / (parts - part);
            struct index_slot *slot = part == 0 ? group->slot[s] : slot_index_allocate(allocator);
            if(!slot) return -1;
            struct slot_update update = {.slot = slot, .image = {.boundary = group->boundary}};
            memcpy(update.image.entries, merged + offset, length * sizeof(merged[0]));
            if(slot_index_apply_updates(&update, 1)) return -1;
            slots[output] = slot;
            minima[output] = merged[offset].key;
            counts[output++] = length;
            offset += length;
        }
    }
    if(input != count) return -1;
    memcpy(group->slot, slots, output * sizeof(*slots));
    memcpy(group->min_key, minima, output * sizeof(*minima));
    memcpy(group->slot_key_num, counts, output * sizeof(*counts));
    group->slot_num = output;
    return 0;
}


struct write_buffer_return_type slot_index_insert(struct subspace_info * subspace_info , char *record , key_type key ,
int size ,  int thread){
    struct write_buffer_index * buffer_index = subspace_info->buffer_index;
    struct write_buffer_return_type return_value;
    int level = random_level(buffer_index->reserve_levels - 1 , &random_seed[thread]);
    return_value =  write_buffer_index_insert(buffer_index , key , size , record , level);

    subspace_info->buffered_size -= return_value.size;
    subspace_info->buffered_size += size;

    return return_value;
}

struct slot_element * binarySearch(struct slot_element arr[], int left, int right, key_type target) {
    while (left <= right) {
        int mid = left + (right - left) / 2;


        if (arr[mid].key == target) {
            return &arr[mid];
        }


        if (arr[mid].key < target) {
            left = mid + 1;
        }

        else {
            right = mid - 1;
        }
    }


    return NULL;
}



struct address_type slot_index_search(struct subspace_info * subspace_info , key_type key){
    if(subspace_info == NULL || subspace_info->slot_num == 0) return ADDR_NULL;
    int i;
    for(i = 0 ; i < subspace_info->slot_num - 1 ; i++){
        if(key >= subspace_info->min_key[i+1] ){
            continue;
        }
        else{
            break;
        }
    }
    struct address_type result = ADDR_NULL;

    struct slot_element *slot_elements =
        subspace_slot_elements(subspace_info, (uint32_t)i);
    struct slot_element *found = slot_elements == NULL ? NULL :
        binarySearch(slot_elements, 0, subspace_info->slot_key_num[i] - 1, key);
    if(found != NULL){
        result = found->addr;
    }

    return result;
}


bool slot_index_delete(struct subspace_info *group, key_type key, struct thread *thread){
    if(group->slot_num == 0) return false;
    assert(!subspace_busy(group));
    uint32_t i = 0;
    while(i + 1 < group->slot_num && key >= group->min_key[i + 1]) i++;
    struct index_slot image = *subspace_index_slot(group, i);
    uint32_t j = 0, count = group->slot_key_num[i];
    while(j < count && image.entries[j].key < key) j++;
    if(j == count || image.entries[j].key != key) return false;
    struct address_type old = image.entries[j].addr;
    memmove(image.entries + j, image.entries + j + 1,
            (count - j - 1) * sizeof(image.entries[0]));
    memset(image.entries + count - 1, 0, sizeof(image.entries[0]));
    if(count == 1) memset(&image, 0, sizeof(image));
    struct slot_update update = {group->slot[i], image};
    if(slot_index_apply_updates(&update, 1) != 0){
        fprintf(stderr, "delete: DRAM slot update failed\n");
        exit(EXIT_FAILURE);
    }
    group->key_num--;
    if(count == 1){
        slot_index_free(&thread->slots, group->slot[i]);
        group->slot_num--;
#define SHIFT_SLOT(field) memmove(group->field + i, group->field + i + 1, \
        (group->slot_num - i) * sizeof(group->field[0])); \
        memset(group->field + group->slot_num, 0, sizeof(group->field[0]))
        SHIFT_SLOT(slot); SHIFT_SLOT(min_key);
        SHIFT_SLOT(slot_key_num);
#undef SHIFT_SLOT
    }else{
        group->slot_key_num[i]--;
        group->min_key[i] = image.entries[0].key;
    }
    group->redundant_size += old.size;
    if(g_map[old.block]) block_ref_remove(old.block);
    if(!g_map[old.block]){
        reclaim_blocks(thread, &old.block, 1);
        if(group->total_block_num) group->total_block_num--;
        group->redundant_size = group->redundant_size > BLOCK_SIZE ? group->redundant_size - BLOCK_SIZE : 0;
    }
    return true;
}
