#include "link.h"
#include <stdio.h>

int main(void)
{
    char a[32] = "PostgreSQL";
    char b[32] = "no hash sorted index";
    LinkData *head = InitLink(sizeof(a), a);
    if (!head) { perror("InitLink"); return 1; }
    if (!AppendNodeChecked(head, sizeof(b), b)) {
        perror("AppendNodeChecked");
        DestroyLink(head);
        return 1;
    }
    LinkData *found = FindNode(head, sizeof(b), b);
    int ok = found != NULL;
    printf("nodes=%zu found=%s\n", LinkCount(head), found ? found->data : "none");
    DestroyLink(head);
    return ok ? 0 : 1;
}
