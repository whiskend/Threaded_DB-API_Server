#include <stdio.h>
#include <stdlib.h>

#include "bptree.h"
#include "errors.h"

/* 실패 메시지를 stderr로 출력하고 프로세스를 종료해 테스트 중단을 명확히 만든다. */
static void fail(const char *message)
{
    fprintf(stderr, "test_bptree failed: %s\n", message);
    exit(1);
}

/* condition이 거짓이면 message와 함께 테스트를 즉시 실패시킨다. */
static void assert_true(int condition, const char *message)
{
    if (!condition) {
        fail(message);
    }
}

/* tree의 가장 왼쪽 leaf를 찾아 leaf chain 순회 시작점으로 반환한다. */
static BPTreeNode *leftmost_leaf(BPTree *tree)
{
    BPTreeNode *node = tree->root;

    while (node != NULL && !node->is_leaf) {
        node = node->ptrs.children[0];
    }

    return node;
}

/* 순차 삽입 후 모든 key가 검색되고 offset이 정확히 매핑되는지 검증한다. */
static void test_sequential_insert_and_search(void)
{
    BPTree tree = {0};
    char errbuf[256] = {0};
    size_t i;

    assert_true(bptree_init(&tree, errbuf, sizeof(errbuf)) == STATUS_OK, "init should succeed");
    for (i = 1U; i <= 1000U; ++i) {
        assert_true(bptree_insert(&tree, (uint64_t)i, (long)(i * 10U), errbuf, sizeof(errbuf)) == STATUS_OK,
                    "sequential insert should succeed");
    }
    assert_true(bptree_validate(&tree, errbuf, sizeof(errbuf)) == STATUS_OK, "tree should validate");

    for (i = 1U; i <= 1000U; ++i) {
        long offset = 0L;
        int found = 0;

        assert_true(bptree_search(&tree, (uint64_t)i, &offset, &found, errbuf, sizeof(errbuf)) == STATUS_OK,
                    "search should succeed");
        assert_true(found == 1, "inserted key should be found");
        assert_true(offset == (long)(i * 10U), "offset should match inserted value");
    }

    bptree_destroy(&tree);
}

/* 비정렬 순서로 삽입해도 search가 올바른 leaf와 offset을 찾는지 검증한다. */
static void test_random_insert_and_search(void)
{
    BPTree tree = {0};
    uint64_t keys[] = {42U, 7U, 99U, 13U, 5U, 88U, 1U, 67U, 55U, 23U};
    size_t i;
    char errbuf[256] = {0};

    assert_true(bptree_init(&tree, errbuf, sizeof(errbuf)) == STATUS_OK, "init should succeed");
    for (i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        assert_true(bptree_insert(&tree, keys[i], (long)(keys[i] * 3U), errbuf, sizeof(errbuf)) == STATUS_OK,
                    "random insert should succeed");
    }
    assert_true(bptree_validate(&tree, errbuf, sizeof(errbuf)) == STATUS_OK, "random tree should validate");

    for (i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        long offset = 0L;
        int found = 0;

        assert_true(bptree_search(&tree, keys[i], &offset, &found, errbuf, sizeof(errbuf)) == STATUS_OK,
                    "search should succeed");
        assert_true(found == 1, "random key should be found");
        assert_true(offset == (long)(keys[i] * 3U), "offset should match");
    }

    bptree_destroy(&tree);
}

/* 대량 삽입으로 root split과 internal split이 일어난 뒤에도 검색과 validate가 유지되는지 본다. */
static void test_root_and_internal_split(void)
{
    BPTree tree = {0};
    char errbuf[256] = {0};
    size_t i;

    assert_true(bptree_init(&tree, errbuf, sizeof(errbuf)) == STATUS_OK, "init should succeed");
    for (i = 1U; i <= 5000U; ++i) {
        assert_true(bptree_insert(&tree, (uint64_t)i, (long)i, errbuf, sizeof(errbuf)) == STATUS_OK,
                    "large insert should succeed");
    }
    assert_true(tree.root != NULL && tree.root->is_leaf == 0, "root should split into internal node");
    assert_true(bptree_validate(&tree, errbuf, sizeof(errbuf)) == STATUS_OK, "large tree should validate");

    {
        long offset = 0L;
        int found = 0;

        assert_true(bptree_search(&tree, 4096U, &offset, &found, errbuf, sizeof(errbuf)) == STATUS_OK,
                    "search after internal split should succeed");
        assert_true(found == 1 && offset == 4096L, "internal split search result should match");
    }

    bptree_destroy(&tree);
}

/* 같은 key를 두 번 넣으면 duplicate key 오류로 거절되는지 확인한다. */
static void test_duplicate_key_insert_fails(void)
{
    BPTree tree = {0};
    char errbuf[256] = {0};

    assert_true(bptree_init(&tree, errbuf, sizeof(errbuf)) == STATUS_OK, "init should succeed");
    assert_true(bptree_insert(&tree, 1U, 10L, errbuf, sizeof(errbuf)) == STATUS_OK, "first insert should succeed");
    assert_true(bptree_insert(&tree, 1U, 20L, errbuf, sizeof(errbuf)) == STATUS_INDEX_ERROR,
                "duplicate insert should fail");

    bptree_destroy(&tree);
}

/* leaf chain을 왼쪽부터 순회했을 때 key가 오름차순으로 모두 방문되는지 검증한다. */
static void test_leaf_chain_sorted(void)
{
    BPTree tree = {0};
    BPTreeNode *leaf;
    uint64_t expected = 1U;
    char errbuf[256] = {0};
    size_t i;

    assert_true(bptree_init(&tree, errbuf, sizeof(errbuf)) == STATUS_OK, "init should succeed");
    for (i = 1U; i <= 2048U; ++i) {
        assert_true(bptree_insert(&tree, (uint64_t)i, (long)i, errbuf, sizeof(errbuf)) == STATUS_OK,
                    "leaf chain insert should succeed");
    }

    leaf = leftmost_leaf(&tree);
    while (leaf != NULL) {
        size_t key_index;

        assert_true(leaf->is_leaf == 1, "leaf chain should contain only leaves");
        for (key_index = 0U; key_index < leaf->key_count; ++key_index) {
            assert_true(leaf->keys[key_index] == expected, "leaf chain keys should be sorted");
            expected += 1U;
        }

        leaf = leaf->next;
    }

    assert_true(expected == 2049U, "leaf chain should visit every inserted key");
    assert_true(bptree_validate(&tree, errbuf, sizeof(errbuf)) == STATUS_OK, "leaf chain tree should validate");

    bptree_destroy(&tree);
}

/* 모든 B+Tree 단위 테스트를 순서대로 실행하고 통과 시 OK를 출력한다. */
int main(void)
{
    test_sequential_insert_and_search();
    test_random_insert_and_search();
    test_root_and_internal_split();
    test_duplicate_key_insert_fails();
    test_leaf_chain_sorted();
    puts("test_bptree: OK");
    return 0;
}
