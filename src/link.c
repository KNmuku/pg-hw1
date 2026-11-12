#include "link.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct IndexEntry {
    uint64_t prefix;
    LinkData *node;
    size_t ordinal;
    int size;
} IndexEntry;

/* Only the head carries metadata; ordinary nodes retain the original layout. */
typedef struct LinkHead {
    LinkData node;                 /* must stay first */
    size_t count;
    size_t payload_bytes;
    IndexEntry *index;
    size_t indexed_count;
} LinkHead;

static int valid_data(int size, const void *mem)
{
    return mem != NULL && size >= LINK_MIN_DATA_SIZE && size <= LINK_MAX_DATA_SIZE;
}

/* First 8 original bytes in lexicographic order. This is NOT a hash:
 * no mixing, buckets, modulo or lossy equality test is used. */
static uint64_t ordered_prefix(const void *mem)
{
    const unsigned char *p = mem;
    uint64_t v;
    memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(v);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return v;
#else
    v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
#endif
}

static int compare_entries(const void *a, const void *b)
{
    const IndexEntry *x = a, *y = b;
    int c;
    if (x->size != y->size)
        return x->size < y->size ? -1 : 1;
    if (x->prefix != y->prefix)
        return x->prefix < y->prefix ? -1 : 1;
    c = memcmp(x->node->data + 8, y->node->data + 8, (size_t)x->size - 8);
    if (c != 0)
        return c;
    return (x->ordinal > y->ordinal) - (x->ordinal < y->ordinal);
}

LinkData *InitLink(int size, char *mem)
{
    LinkHead *head;
    if (!valid_data(size, mem)) {
        errno = EINVAL;
        return NULL;
    }
    head = malloc(sizeof(*head) + (size_t)size);
    if (!head)
        return NULL;
    head->node.prev = head->node.next = &head->node;
    head->node.type = 0;
    head->node.dataSize = size;
    head->node.data = (char *)(head + 1);
    memcpy(head->node.data, mem, (size_t)size);
    head->count = 1;
    head->payload_bytes = (size_t)size;
    head->index = NULL;
    head->indexed_count = 0;
    return &head->node;
}

LinkData *AppendNodeChecked(LinkData *firstNode, int size, const void *mem)
{
    LinkHead *head = (LinkHead *)firstNode;
    LinkData *node, *tail;
    if (!head || !valid_data(size, mem)) {
        errno = EINVAL;
        return NULL;
    }
    if (head->count == SIZE_MAX || head->payload_bytes > SIZE_MAX - (size_t)size) {
        errno = EOVERFLOW;
        return NULL;
    }
    node = malloc(sizeof(*node) + (size_t)size);
    if (!node)
        return NULL;
    node->data = (char *)(node + 1);
    node->type = 0;
    node->dataSize = size;
    memcpy(node->data, mem, (size_t)size);
    tail = firstNode->prev;
    node->prev = tail;
    node->next = firstNode;
    tail->next = node;
    firstNode->prev = node;
    ++head->count;
    head->payload_bytes += (size_t)size;
    InvalidateLinkIndex(firstNode);
    return node;
}

void AppendNode(LinkData *firstNode, int size, char *mem)
{
    (void)AppendNodeChecked(firstNode, size, mem);
}

void InvalidateLinkIndex(LinkData *firstNode)
{
    if (firstNode) {
        LinkHead *head = (LinkHead *)firstNode;
        free(head->index);
        head->index = NULL;
        head->indexed_count = 0;
    }
}

int PrepareLink(LinkData *firstNode)
{
    LinkHead *head = (LinkHead *)firstNode;
    LinkData *node;
    IndexEntry *entries;
    if (!head) {
        errno = EINVAL;
        return -1;
    }
    if (head->count < LINK_INDEX_THRESHOLD || head->indexed_count == head->count)
        return 0;
    if (head->count > SIZE_MAX / sizeof(*entries)) {
        errno = EOVERFLOW;
        return -1;
    }
    entries = malloc(head->count * sizeof(*entries));
    if (!entries)
        return -1;
    node = firstNode;
    for (size_t i = 0; i < head->count; ++i) {
        entries[i].prefix = ordered_prefix(node->data);
        entries[i].node = node;
        entries[i].ordinal = i;
        entries[i].size = node->dataSize;
        node = node->next;
    }
    qsort(entries, head->count, sizeof(*entries), compare_entries);
    free(head->index);
    head->index = entries;
    head->indexed_count = head->count;
    return 0;
}

LinkData *FindNodeLinear(LinkData *firstNode, int size, const void *mem)
{
    LinkData *node = firstNode;
    if (!node || !valid_data(size, mem)) {
        errno = EINVAL;
        return NULL;
    }
    do {
        if (node->dataSize == size && memcmp(node->data, mem, (size_t)size) == 0)
            return node;
        node = node->next;
    } while (node != firstNode);
    return NULL;
}

static int compare_key(const IndexEntry *entry, int size, uint64_t prefix,
                       const char *mem)
{
    if (entry->size != size)
        return entry->size < size ? -1 : 1;
    if (entry->prefix != prefix)
        return entry->prefix < prefix ? -1 : 1;
    return memcmp(entry->node->data + 8, mem + 8, (size_t)size - 8);
}

LinkData *FindNode(LinkData *firstNode, int size, char *mem)
{
    LinkHead *head = (LinkHead *)firstNode;
    size_t lo = 0, hi;
    uint64_t prefix;
    if (!head || !valid_data(size, mem)) {
        errno = EINVAL;
        return NULL;
    }
    if (head->count < LINK_INDEX_THRESHOLD)
        return FindNodeLinear(firstNode, size, mem);
    /* Allocation failure affects speed only, never correctness. */
    if (PrepareLink(firstNode) != 0)
        return FindNodeLinear(firstNode, size, mem);
    prefix = ordered_prefix(mem);
    hi = head->count;
    /* lower_bound + ordinal tie break preserves first-in-list duplicates. */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (compare_key(&head->index[mid], size, prefix, mem) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < head->count && compare_key(&head->index[lo], size, prefix, mem) == 0)
        return head->index[lo].node;
    return NULL;
}

size_t LinkCount(const LinkData *firstNode)
{
    return firstNode ? ((const LinkHead *)firstNode)->count : 0;
}

size_t LinkMemoryBytes(const LinkData *firstNode)
{
    const LinkHead *head = (const LinkHead *)firstNode;
    if (!head)
        return 0;
    /* Practical allocation sizes are bounded by successful mallocs. */
    return sizeof(*head) + (head->count - 1) * sizeof(LinkData) +
           head->payload_bytes + head->indexed_count * sizeof(IndexEntry);
}

void DestroyLink(LinkData *firstNode)
{
    LinkData *node;
    if (!firstNode)
        return;
    node = firstNode->next;
    while (node != firstNode) {
        LinkData *next = node->next;
        free(node);
        node = next;
    }
    free(((LinkHead *)firstNode)->index);
    free(firstNode);
}
