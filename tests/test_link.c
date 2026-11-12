#include "link.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
static int fail_alloc;
void *__wrap_malloc(size_t size)
{
    if (fail_alloc) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}

static uint64_t rng = UINT64_C(0x123456789abcdef);
static uint64_t next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void check_ring(LinkData *head, size_t count)
{
    LinkData *node = head;
    for (size_t i = 0; i < count; ++i) {
        assert(node->next->prev == node);
        assert(node->prev->next == node);
        node = node->next;
        assert(i + 1 == count || node != head);
    }
    assert(node == head);
    assert(LinkCount(head) == count);
}

static void basic_and_failures(void)
{
    char a[512] = {0}, b[512] = {0}, c[512] = {0};
    LinkData *head, *tail;
    a[31] = 7;
    b[31] = 8;
    c[31] = 9;
    assert(InitLink(31, a) == NULL && errno == EINVAL);
    assert(InitLink(513, a) == NULL && errno == EINVAL);
    assert(InitLink(32, NULL) == NULL && errno == EINVAL);
    assert(FindNode(NULL, 32, a) == NULL && errno == EINVAL);
    assert(PrepareLink(NULL) == -1 && errno == EINVAL);
    assert(AppendNodeChecked(NULL, 32, a) == NULL && errno == EINVAL);
    fail_alloc = 1;
    assert(InitLink(32, a) == NULL && errno == ENOMEM);
    fail_alloc = 0;
    head = InitLink(32, a);
    assert(head && head->prev == head && head->next == head);
    a[0] = 42;
    assert(head->data[0] == 0);   /* independent copy */
    a[0] = 0;
    assert(FindNode(head, 32, a) == head);
    assert(FindNode(head, 32, c) == NULL);
    assert(FindNode(head, 31, c) == NULL && errno == EINVAL);
    assert(FindNode(head, 32, NULL) == NULL && errno == EINVAL);
    assert(AppendNodeChecked(head, 513, b) == NULL && LinkCount(head) == 1);
    fail_alloc = 1;
    assert(AppendNodeChecked(head, 32, b) == NULL && errno == ENOMEM);
    fail_alloc = 0;
    check_ring(head, 1);
    AppendNode(head, 32, b);      /* required void API */
    tail = head->prev;
    assert(tail != head && FindNode(head, 32, b) == tail);
    assert(AppendNodeChecked(head, 32, a));
    assert(FindNode(head, 32, a) == head);
    check_ring(head, 3);
    assert(LinkMemoryBytes(head) >= 3 * (sizeof(LinkData) + 32));
    DestroyLink(head);
    DestroyLink(NULL);
    assert(LinkCount(NULL) == 0 && LinkMemoryBytes(NULL) == 0);
}

static void large_prefix_and_invalidation(void)
{
    char data[512];
    LinkData *head = NULL, *first_duplicate, *new_tail;
    memset(data, 0x5a, sizeof(data));
    for (uint64_t i = 0; i < 12000; ++i) {
        memcpy(data + 504, &i, 8); /* every first 504 bytes identical */
        if (!head)
            head = InitLink(512, data);
        else
            assert(AppendNodeChecked(head, 512, data));
        assert(head);
    }
    check_ring(head, 12000);
    fail_alloc = 1;
    assert(PrepareLink(head) == -1 && errno == ENOMEM);
    assert(FindNode(head, 512, data) == head->prev); /* linear fallback */
    fail_alloc = 0;
    assert(PrepareLink(head) == 0);
    for (uint64_t i = 0; i < 12000; i += 37) {
        memcpy(data + 504, &i, 8);
        assert(FindNode(head, 512, data) == FindNodeLinear(head, 512, data));
    }
    first_duplicate = head->next;
    memcpy(data, first_duplicate->data, 512);
    assert(AppendNodeChecked(head, 512, data));
    assert(FindNode(head, 512, data) == first_duplicate);
    memset(data, 0x7e, sizeof(data));
    new_tail = AppendNodeChecked(head, 512, data);
    assert(new_tail && FindNode(head, 512, data) == new_tail);
    head->next->data[0] = 0x11;
    InvalidateLinkIndex(head);
    assert(FindNode(head, 512, head->next->data) == head->next);
    check_ring(head, 12002);
    DestroyLink(head);
}

static void mixed_binary_oracle(void)
{
    enum { N = 900 };
    LinkData *head = NULL, *nodes[N];
    unsigned char data[512];
    for (size_t i = 0; i < N; ++i) {
        int size = LINK_MIN_DATA_SIZE + (int)(next_random() % 481);
        for (int j = 0; j < size; ++j)
            data[j] = (unsigned char)next_random();
        if (i % 11 == 0 && i > 0) {
            size = nodes[0]->dataSize;
            memcpy(data, nodes[0]->data, (size_t)size);
        }
        if (!head)
            nodes[i] = head = InitLink(size, (char *)data);
        else
            nodes[i] = AppendNodeChecked(head, size, data);
        assert(nodes[i]);
    }
    for (size_t i = 0; i < 7000; ++i) {
        LinkData *p = nodes[next_random() % N];
        memcpy(data, p->data, (size_t)p->dataSize);
        if (i & 1)
            data[next_random() % (unsigned)p->dataSize] ^= 0xa5;
        assert(FindNode(head, p->dataSize, (char *)data) ==
               FindNodeLinear(head, p->dataSize, data));
    }
    check_ring(head, N);
    DestroyLink(head);
}

static void many_independent_lists(void)
{
    enum { N = 20000 };
    LinkData **heads = malloc(N * sizeof(*heads));
    char data[32] = {0};
    assert(heads);
    for (uint64_t i = 0; i < N; ++i) {
        memcpy(data, &i, 8);
        heads[i] = InitLink(32, data);
        assert(heads[i]);
    }
    for (uint64_t i = 0; i < N; ++i) {
        memcpy(data, &i, 8);
        assert(FindNode(heads[i], 32, data) == heads[i]);
        assert(FindNode(heads[(i + 1) % N], 32, data) == NULL);
    }
    for (size_t i = 0; i < N; ++i)
        DestroyLink(heads[i]);
    free(heads);
}

int main(void)
{
    basic_and_failures();
    puts("PASS basic APIs, bounds, binary copy, ring and allocation failures");
    large_prefix_and_invalidation();
    puts("PASS 12000 nodes, 512-byte common prefixes, duplicates, index rebuild");
    mixed_binary_oracle();
    puts("PASS 7000 randomized mixed-length queries against linear oracle");
    many_independent_lists();
    puts("PASS 20000 independent lists and isolation");
    puts("ALL LINK TESTS PASSED");
    return 0;
}
