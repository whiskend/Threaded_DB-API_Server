#ifndef BPTREE_H
#define BPTREE_H

#include <stddef.h>
#include <stdint.h>

/* B+Tree 한 노드가 가질 수 있는 최대 자식 수를 나타내는 차수 정의다. */
#define BPTREE_ORDER 64
/* 차수로부터 계산한 한 노드의 최대 key 개수다. */
#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)
/* 차수로부터 계산한 한 internal node의 최대 child 포인터 수다. */
#define BPTREE_MAX_CHILDREN (BPTREE_ORDER)

/* 
keys:      [30 | 60]
children:   ↓    ↓    ↓
         child0 child1 child2
👉 의미:
<30 → child0
30~59 → child1
≥60 → child2

keys:       [30, 40, 50]
offsets:    [100,200,300]
👉 매핑:
30 → offset 100
40 → offset 200
50 → offset 300
 */

/* B+Tree의 internal/leaf node를 공통 레이아웃으로 표현하는 구조체다. */
typedef struct BPTreeNode {
    int is_leaf;                                /* 1이면 leaf, 0이면 internal node다. */
    size_t key_count;                           /* 현재 노드에 실제로 저장된 key 개수다. */
    uint64_t keys[BPTREE_MAX_KEYS];             /* 정렬된 key 배열이다. */
    struct BPTreeNode *parent;                  /* 부모 노드 포인터다. */
    struct BPTreeNode *next;                    /* leaf chain에서 오른쪽 이웃 leaf를 가리킨다. */
    union {
        struct BPTreeNode *children[BPTREE_MAX_CHILDREN]; /* internal node일 때 자식 포인터 배열이다. */
        long row_offsets[BPTREE_MAX_KEYS];                /* leaf node일 때 key별 row offset 값 배열이다. */
    } ptrs;
} BPTreeNode;

/* 
Level 0 (Root)
                         [ K1 | K2 | ... | K63 ]
                    /     /     /           ...       \
                   ↓      ↓      ↓                     ↓

Level 1 (64개 노드)
          [ ... ]  [ ... ]  [ ... ]  ...  [ ... ]   (총 64개)
           /|\       /|\       /|\              /|\
          ↓ ↓ ↓     ↓ ↓ ↓     ↓ ↓ ↓           ↓ ↓ ↓

각각 최대 63개의 키를 가진 내부 노드를 64개 가지고 있다.
 */

/* 트리의 루트와 전체 key 개수를 추적하는 B+Tree 컨테이너다. */
typedef struct {
    BPTreeNode *root; /* 현재 트리의 루트 노드다. */
    size_t key_count; /* 트리에 저장된 전체 key 수다. */
} BPTree;

/* out_tree를 빈 B+Tree 상태로 초기화하고 STATUS_* 코드를 반환한다. */
int bptree_init(BPTree *out_tree, char *errbuf, size_t errbuf_size);
/*
 * tree에서 key를 탐색해 찾으면 out_offset에 row offset, out_found에 1을 저장하고,
 * 못 찾으면 out_found에 0을 저장한 뒤 상태 코드를 반환한다.
 */
int bptree_search(const BPTree *tree, uint64_t key,
                  long *out_offset, int *out_found,
                  char *errbuf, size_t errbuf_size);
/*
 * tree에 key와 row_offset을 정렬 상태로 삽입하고, split이 필요하면 전파까지 수행한 뒤
 * 성공/실패 상태 코드를 반환한다.
 */
int bptree_insert(BPTree *tree, uint64_t key, long row_offset,
                  char *errbuf, size_t errbuf_size);
/*
 * tree의 leaf depth, separator 범위, leaf chain 정렬, 전체 key 개수를 검증하고
 * 구조가 정상이면 STATUS_OK를 반환한다.
 */
int bptree_validate(const BPTree *tree, char *errbuf, size_t errbuf_size);
/* tree가 소유한 모든 노드를 재귀적으로 해제하고 빈 트리 상태로 되돌린다. */
void bptree_destroy(BPTree *tree);

#endif
