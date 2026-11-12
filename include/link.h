#ifndef HW1_LINK_H
#define HW1_LINK_H

#include <stddef.h>

#define LINK_MIN_DATA_SIZE 32
#define LINK_MAX_DATA_SIZE 512
#define LINK_INDEX_THRESHOLD 32u

/* Original assignment layout, 32 bytes on the tested x86-64 ABI. */
typedef struct LinkData {
    struct LinkData *prev;
    struct LinkData *next;
    int type;
    int dataSize;
    char *data;
} LinkData;

LinkData *InitLink(int size, char *mem);
void AppendNode(LinkData *firstNode, int size, char *mem);
LinkData *FindNode(LinkData *firstNode, int size, char *mem);

/* Checked append: NULL + errno on failure; leaves the old list unchanged. */
LinkData *AppendNodeChecked(LinkData *firstNode, int size, const void *mem);
LinkData *FindNodeLinear(LinkData *firstNode, int size, const void *mem);
/* Optional preparation, outside a measured search region. 0 or -1 + errno. */
int PrepareLink(LinkData *firstNode);
/* Call after directly changing a node's payload. Do not change dataSize/data. */
void InvalidateLinkIndex(LinkData *firstNode);
size_t LinkCount(const LinkData *firstNode);
/* Requested allocation bytes, excluding malloc metadata and temporary qsort data. */
size_t LinkMemoryBytes(const LinkData *firstNode);
void DestroyLink(LinkData *firstNode);

/* All firstNode parameters must be the pointer returned by InitLink.
 * Binary data are copied, not borrowed. Earliest duplicate in list order wins.
 * No deletion API. Externally synchronize concurrent access to the same list.
 */
#endif
